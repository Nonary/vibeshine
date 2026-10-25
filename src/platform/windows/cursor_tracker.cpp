// SPDX-License-Identifier: GPL-3.0-or-later
// Polls the Windows cursor for the client-side cursor extension. Runs only while
// a client is subscribed. Sunshine runs inside the console session (as SYSTEM in
// service mode), so the thread binds to the current input desktop, which also
// covers the secure desktop.

// platform includes first: winsock2.h must precede windows.h
#include <winsock2.h>
#include <windows.h>

#include "cursor_tracker.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "misc.h"
#include "src/host_cursor.h"
#include "src/logging.h"
#include "src/utility.h"

using namespace std::literals;

namespace host_cursor::tracker {
  namespace {
    constexpr auto poll_interval = 5ms;  // 200 Hz
    constexpr auto shape_revalidate_interval = 1s;
    // Rebinding is cheap and catches input-desktop switches (UAC, lock screen)
    // that do not make GetCursorInfo() fail.
    constexpr auto desktop_rebind_interval = 1s;
    constexpr auto cost_log_interval = 30s;

    std::optional<host_cursor_bitmap::image_t> convert_dxgi(const DXGI_OUTDUPL_POINTER_SHAPE_INFO &info, std::span<const std::uint8_t> data) {
      const auto hx = std::uint32_t(std::max<LONG>(info.HotSpot.x, 0));
      const auto hy = std::uint32_t(std::max<LONG>(info.HotSpot.y, 0));
      switch (info.Type) {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
          return host_cursor_bitmap::from_monochrome(info.Width, info.Height / 2, info.Pitch, data, hx, hy);
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
          return host_cursor_bitmap::from_color(info.Width, info.Height, info.Pitch, data, hx, hy);
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
          return host_cursor_bitmap::from_masked_color(info.Width, info.Height, info.Pitch, data, hx, hy);
        default:
          return std::nullopt;
      }
    }

    std::optional<std::vector<std::uint8_t>> bitmap_bits(HBITMAP bitmap, const BITMAP &bm) {
      std::vector<std::uint8_t> bits(std::size_t(bm.bmWidthBytes) * bm.bmHeight);
      if (bits.empty() || GetBitmapBits(bitmap, LONG(bits.size()), bits.data()) != LONG(bits.size())) {
        return std::nullopt;
      }
      return bits;
    }

    // GetIconInfo() + raw bitmap bits, as drawn by GDI. Masks are WORD-aligned 1 bpp
    // rows; a monochrome cursor stacks its AND and XOR masks in one bitmap.
    std::optional<host_cursor_bitmap::image_t> convert_hcursor(HCURSOR cursor) {
      ICONINFO info {};
      if (!GetIconInfo(cursor, &info)) {
        return std::nullopt;
      }
      auto free_bitmaps = util::fail_guard([&]() {
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
      });

      BITMAP mask_bm {};
      if (!info.hbmMask || !GetObject(info.hbmMask, sizeof(mask_bm), &mask_bm) || mask_bm.bmBitsPixel != 1) {
        return std::nullopt;
      }
      auto mask = bitmap_bits(info.hbmMask, mask_bm);
      if (!mask) {
        return std::nullopt;
      }

      if (!info.hbmColor) {
        return host_cursor_bitmap::from_monochrome(mask_bm.bmWidth, mask_bm.bmHeight / 2, mask_bm.bmWidthBytes, *mask, info.xHotspot, info.yHotspot);
      }

      BITMAP color_bm {};
      if (!GetObject(info.hbmColor, sizeof(color_bm), &color_bm) || color_bm.bmWidth <= 0 || color_bm.bmHeight <= 0) {
        return std::nullopt;
      }
      std::vector<std::uint8_t> color;
      if (color_bm.bmBitsPixel == 32) {
        auto bits = bitmap_bits(info.hbmColor, color_bm);
        if (!bits) return std::nullopt;
        color = std::move(*bits);
      } else {
        BITMAPINFO bmi {};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = color_bm.bmWidth;
        bmi.bmiHeader.biHeight = -color_bm.bmHeight;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        color.resize(std::size_t(color_bm.bmWidth) * color_bm.bmHeight * 4);
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) return std::nullopt;
        const auto rows = GetDIBits(dc, info.hbmColor, 0, color_bm.bmHeight, color.data(), &bmi, DIB_RGB_COLORS);
        DeleteDC(dc);
        if (rows != color_bm.bmHeight) return std::nullopt;
        color_bm.bmWidthBytes = color_bm.bmWidth * 4;
      }

      bool has_alpha = false;
      for (std::size_t i = 3; i < color.size() && !has_alpha; i += 4) has_alpha = color[i] != 0;
      if (has_alpha) {
        return host_cursor_bitmap::from_color(color_bm.bmWidth, color_bm.bmHeight, color_bm.bmWidthBytes, color, info.xHotspot, info.yHotspot);
      }
      return host_cursor_bitmap::from_color_and_mask(color_bm.bmWidth, color_bm.bmHeight, color_bm.bmWidthBytes, color, mask_bm.bmWidthBytes, *mask, info.xHotspot, info.yHotspot);
    }

    class tracker_t {
    public:
      std::shared_ptr<void> acquire() {
        std::lock_guard lg {_users_lock};
        if (_users++ == 0) {
          BOOST_LOG(info) << "Client-side cursor: tracker started"sv;
          _running.store(true, std::memory_order_release);
          _thread = std::jthread([this](std::stop_token stop) {
            run(stop);
          });
        }
        return std::shared_ptr<void>(this, [](void *self) {
          static_cast<tracker_t *>(self)->release();
        });
      }

      snapshot_t latest() {
        std::lock_guard lg {_snapshot_lock};
        return _snapshot;
      }

      bool running() const {
        return _running.load(std::memory_order_acquire);
      }

      injection_history_t injections;

      std::shared_ptr<void> dxgi_source() {
        std::lock_guard lg {_dxgi_lock};
        ++_dxgi_sources;
        return std::shared_ptr<void>(this, [](void *self) {
          auto tracker = static_cast<tracker_t *>(self);
          std::lock_guard lg {tracker->_dxgi_lock};
          if (--tracker->_dxgi_sources == 0) {
            tracker->_dxgi_shape.reset();
          }
        });
      }

      void note_dxgi_shape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO &info, std::span<const std::uint8_t> data) {
        std::lock_guard lg {_dxgi_lock};
        _dxgi_shape = dxgi_shape_t {info, {data.begin(), data.end()}, ++_dxgi_serial};
      }

    private:
      struct dxgi_shape_t {
        DXGI_OUTDUPL_POINTER_SHAPE_INFO info;
        std::vector<std::uint8_t> data;
        std::uint64_t serial;
      };

      void release() {
        std::jthread stopping;
        {
          std::lock_guard lg {_users_lock};
          if (--_users != 0) return;
          _running.store(false, std::memory_order_release);
          stopping = std::move(_thread);
        }
        stopping.request_stop();
        stopping.join();
        BOOST_LOG(info) << "Client-side cursor: tracker stopped"sv;
      }

      // Returns the shape id from the newest DXGI shape while Desktop Duplication
      // is capturing, or nullopt to fall back to GetIconInfo().
      std::optional<std::uint32_t> dxgi_shape_id() {
        std::optional<dxgi_shape_t> pending;
        {
          std::lock_guard lg {_dxgi_lock};
          if (!_dxgi_sources) {
            _dxgi_id.reset();
            return std::nullopt;
          }
          if (_dxgi_shape && _dxgi_shape->serial != _dxgi_converted_serial) {
            _dxgi_converted_serial = _dxgi_shape->serial;
            pending = _dxgi_shape;
          }
        }
        if (pending) {
          if (auto image = convert_dxgi(pending->info, pending->data)) {
            _dxgi_id = shapes().insert(std::move(*image));
          } else {
            BOOST_LOG(debug) << "Client-side cursor: unsupported DXGI pointer shape type "sv << pending->info.Type;
          }
        }
        return _dxgi_id;
      }

      // A desktop this process cannot open (the secure desktop when Sunshine is
      // not running as SYSTEM) means cursor information is unavailable there.
      void bind_desktop(clock::time_point now, bool force) {
        if (!force && _last_bind_attempt && now - *_last_bind_attempt < desktop_rebind_interval) {
          return;
        }
        _last_bind_attempt = now;
        _desktop_bound = platf::syncThreadDesktop();
      }

      void publish(snapshot_t next) {
        std::lock_guard lg {_snapshot_lock};
        next.generation = _snapshot.generation;
        if (next.available != _snapshot.available || next.visible != _snapshot.visible ||
            next.shape_id != _snapshot.shape_id || next.x != _snapshot.x || next.y != _snapshot.y ||
            next.clipped != _snapshot.clipped || !(next.clip == _snapshot.clip) || next.warp_serial != _snapshot.warp_serial) {
          next.generation++;
          _snapshot = next;
        }
      }

      void run(std::stop_token stop) {
        _last_bind_attempt.reset();
        bind_desktop(clock::now(), true);

        HCURSOR last_cursor = nullptr;
        std::optional<std::uint32_t> gdi_shape_id;
        clock::time_point last_gdi_read {};
        std::optional<point_t> last_position;
        std::uint64_t warp_serial = 0;
        bool was_available = true;

        std::chrono::nanoseconds cost_total {};
        std::chrono::nanoseconds cost_max {};
        std::uint64_t polls = 0;
        auto cost_window_start = clock::now();

        auto next_poll = clock::now();
        while (!stop.stop_requested()) {
          const auto now = clock::now();

          bind_desktop(now, false);
          CURSORINFO cursor_info {sizeof(cursor_info)};
          bool available = _desktop_bound && GetCursorInfo(&cursor_info);

          snapshot_t next;
          if (available) {
            const auto vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const auto vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const auto vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const auto vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

            next.available = true;
            next.visible = (cursor_info.flags & (CURSOR_SHOWING | CURSOR_SUPPRESSED)) && cursor_info.hCursor;
            next.x = cursor_info.ptScreenPos.x - vx;
            next.y = cursor_info.ptScreenPos.y - vy;

            RECT clip {};
            if (GetClipCursor(&clip) && !(clip.left <= vx && clip.top <= vy && clip.right >= vx + vw && clip.bottom >= vy + vh)) {
              next.clipped = true;
              next.clip = {clip.left - vx, clip.top - vy, clip.right - vx, clip.bottom - vy};
            }

            if (auto dxgi_id = dxgi_shape_id()) {
              next.shape_id = *dxgi_id;
            } else {
              if (next.visible && (cursor_info.hCursor != last_cursor || now - last_gdi_read >= shape_revalidate_interval)) {
                last_cursor = cursor_info.hCursor;
                last_gdi_read = now;
                auto image = convert_hcursor(cursor_info.hCursor);
                gdi_shape_id = image ? std::optional {shapes().insert(std::move(*image))} : std::nullopt;
              }
              next.shape_id = gdi_shape_id.value_or(0);
            }

            const point_t position {next.x, next.y};
            if (last_position && *last_position != position && !injections.explains(position.x, position.y, now)) {
              ++warp_serial;
            }
            last_position = position;
            next.warp_serial = warp_serial;
          }
          if (available != was_available) {
            BOOST_LOG(info) << "Client-side cursor: cursor information "sv << (available ? "available"sv : "unavailable, falling back to host compositing"sv);
            was_available = available;
          }
          publish(next);

          const auto cost = clock::now() - now;
          cost_total += cost;
          cost_max = std::max<std::chrono::nanoseconds>(cost_max, cost);
          ++polls;
          if (now - cost_window_start >= cost_log_interval) {
            BOOST_LOG(debug) << "Client-side cursor: "sv << polls << " polls, average "sv
                             << std::chrono::duration_cast<std::chrono::microseconds>(cost_total / std::max<std::uint64_t>(polls, 1)).count()
                             << " us, max "sv << std::chrono::duration_cast<std::chrono::microseconds>(cost_max).count() << " us"sv;
            cost_total = cost_max = {};
            polls = 0;
            cost_window_start = now;
          }

          next_poll += poll_interval;
          if (next_poll < now) next_poll = now + poll_interval;
          std::this_thread::sleep_until(next_poll);
        }
      }

      std::mutex _users_lock;
      std::size_t _users = 0;
      std::atomic_bool _running {false};
      std::jthread _thread;

      std::mutex _snapshot_lock;
      snapshot_t _snapshot;

      std::mutex _dxgi_lock;
      std::size_t _dxgi_sources = 0;
      std::optional<dxgi_shape_t> _dxgi_shape;
      std::uint64_t _dxgi_serial = 0;
      std::uint64_t _dxgi_converted_serial = 0;
      std::optional<std::uint32_t> _dxgi_id;  // tracker thread only

      std::optional<clock::time_point> _last_bind_attempt;  // tracker thread only
      bool _desktop_bound = false;  // tracker thread only
    };

    tracker_t &instance() {
      static tracker_t tracker;
      return tracker;
    }
  }  // namespace

  bool supported() {
    return true;
  }

  std::shared_ptr<void> acquire() {
    return instance().acquire();
  }

  snapshot_t latest() {
    return instance().latest();
  }

  void note_absolute_injection(std::int32_t x, std::int32_t y) {
    auto &tracker = instance();
    if (tracker.running()) tracker.injections.note_absolute(x, y, clock::now());
  }

  std::shared_ptr<void> dxgi_shape_source() {
    return instance().dxgi_source();
  }

  void note_dxgi_pointer_shape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO &info, std::span<const std::uint8_t> data) {
    auto &tracker = instance();
    // Kept even with no subscriber: shapes change rarely, and a client that
    // subscribes later then starts with the shape DXGI already reported.
    tracker.note_dxgi_shape(info, data);
  }
}  // namespace host_cursor::tracker
