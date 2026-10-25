// SPDX-License-Identifier: GPL-3.0-or-later
// Client-side cursor: per-session policy, shape registry and the platform tracker
// interface. See docs/client-cursor-design.md.
#pragma once

#include "host_cursor_bitmap.h"
#include "host_cursor_protocol.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace host_cursor {
  using clock = std::chrono::steady_clock;

  struct rect_t {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;

    bool operator==(const rect_t &) const = default;
  };

  // Desktop coordinates are relative to the virtual-desktop origin, the space
  // input::touch_port_t::offset_x/offset_y use on Windows.
  struct snapshot_t {
    std::uint64_t generation = 0;
    bool available = false;  // false: cursor information is unavailable (fallback)
    bool visible = false;
    std::uint32_t shape_id = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool clipped = false;
    rect_t clip;
    std::uint64_t warp_serial = 0;  // increments when the host moved the cursor itself
  };

  // The subset of input::touch_port_t that maps desktop coordinates to the stream.
  struct geometry_t {
    bool valid = false;
    std::int32_t offset_x = 0;
    std::int32_t offset_y = 0;
    std::int32_t stream_width = 0;
    std::int32_t stream_height = 0;
    float client_offset_x = 0;
    float client_offset_y = 0;
    float scalar_inv = 1;
  };

  struct point_t {
    std::int32_t x = 0;
    std::int32_t y = 0;

    bool operator==(const point_t &) const = default;
  };

  // Inverse of input::client_to_touchport() followed by platf::abs_mouse() on
  // Windows, so a reported position is where absolute input would put the cursor.
  point_t to_stream(const geometry_t &geometry, double x, double y);

  // Converted shapes, deduplicated by content. Thread-safe; ids are never reused
  // within a process.
  class shape_registry_t {
  public:
    static constexpr std::size_t capacity = 64;

    std::uint32_t insert(host_cursor_bitmap::image_t image);
    std::shared_ptr<const host_cursor_bitmap::image_t> find(std::uint32_t id) const;

  private:
    struct entry_t {
      std::uint32_t id;
      std::uint64_t hash;
      std::shared_ptr<const host_cursor_bitmap::image_t> image;
    };

    mutable std::mutex _lock;
    mutable std::vector<entry_t> _entries;  // most recently used last
    std::uint32_t _next_id = 1;
  };

  shape_registry_t &shapes();

  // Recent input this host injected, so the tracker can tell a cursor the host
  // (an application, a local mouse) moved from one the client moved.
  class injection_history_t {
  public:
    static constexpr std::size_t depth = 8;
    static constexpr std::int32_t tolerance_px = 3;
    static constexpr auto absolute_window = std::chrono::milliseconds(250);
    static constexpr auto relative_quiet = std::chrono::milliseconds(50);

    void note_absolute(std::int32_t x, std::int32_t y, clock::time_point now);
    void note_relative(clock::time_point now);

    // True when the injected input explains a cursor at (x, y).
    bool explains(std::int32_t x, std::int32_t y, clock::time_point now) const;

  private:
    struct target_t {
      std::int32_t x;
      std::int32_t y;
      clock::time_point at;
    };

    mutable std::mutex _lock;
    std::array<target_t, depth> _targets {};
    std::size_t _next = 0;
    std::size_t _count = 0;
    std::optional<clock::time_point> _last_relative;
  };

  // Implemented per platform (Windows only today; elsewhere supported() is false).
  namespace tracker {
    bool supported();

    // The tracker polls while any returned handle is alive.
    std::shared_ptr<void> acquire();

    snapshot_t latest();

    // Desktop coordinates (virtual-desktop relative) of an absolute move the input path injected.
    void note_absolute_injection(std::int32_t x, std::int32_t y);
    void note_relative_injection();
  }  // namespace tracker

  // Whether captured frames must still contain the cursor. True unless every
  // connected session draws the cursor itself and cursor information is available.
  bool compositing_required();
  void set_compositing_required(bool required);

  constexpr bool compute_compositing_required(std::size_t connected, std::size_t client_drawing, bool tracker_available) {
    return connected == 0 || client_drawing < connected || !tracker_available;
  }

  struct session_state_t {
    bool subscribed = false;
    bool client_draws = false;
    bool resync_pending = false;
    std::uint32_t resend_shape_id = 0;
    std::uint32_t seq = 0;
    std::optional<host_cursor_protocol::state_t> last_sent;
    std::optional<point_t> last_position;
    std::uint64_t last_warp_serial = 0;
    std::set<std::uint32_t> shapes_sent;
    std::shared_ptr<void> tracker_handle;  // held by the owner while subscribed

    clock::time_point budget_window_start {};
    std::size_t budget_bytes = 0;
  };

  // New shapes a session may receive per second; a cursor that changes faster
  // than this keeps its previous shape until the budget recovers.
  constexpr std::size_t shape_budget_bytes_per_second = 2 * 1024 * 1024;

  // Applies a CURSOR_CONTROL message. A new subscription schedules a resync.
  void apply_control(session_state_t &session, const host_cursor_protocol::control_t &control);

  struct update_t {
    std::vector<std::shared_ptr<const host_cursor_bitmap::image_t>> shapes;  // send before the state
    std::vector<std::uint32_t> shape_ids;
    std::optional<host_cursor_protocol::state_t> state;
  };

  // Decides what a session needs to receive for `snapshot` and records it as
  // sent. `composited` is whether frames currently contain the cursor.
  update_t plan_update(
    session_state_t &session,
    const snapshot_t &snapshot,
    const geometry_t &geometry,
    bool composited,
    clock::time_point now,
    const shape_registry_t &registry = shapes()
  );
}  // namespace host_cursor
