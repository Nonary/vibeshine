#include "rtx_hdr_runtime.h"

#include "display_helper_process.h"
#include "src/config.h"
#include "src/process.h"
#include "src/platform/common.h"

#include <algorithm>
#include <atomic>
#include <boost/program_options/parsers.hpp>
#include <cstdlib>
#include <fstream>
#include <gio/gio.h>

namespace platf::rtx_hdr {
  namespace {
    std::atomic<std::shared_ptr<const frame_settings_t>> current {std::make_shared<const frame_settings_t>()};
    std::atomic<std::uint32_t> generation {0};
    std::atomic<bool> global_enabled {false};
    constexpr auto runtime_digest = "0252866a7b798a6f92c8543443d88f716e8c5bbc6a7ddd77a2754728dada9fa8";
    struct runtime_manager_t {
      std::mutex mutex;
      bool downloading = false;
      std::string error;
    };
    // The detached installer can finish while the server is shutting down.
    [[maybe_unused]] runtime_manager_t &runtime_manager() {
      static auto *manager = new runtime_manager_t;
      return *manager;
    }

    [[maybe_unused]] bool runtime_ready(const std::filesystem::path &root) {
      std::error_code error;
      std::ifstream marker(root / ".ready");
      std::string digest;
      if (!(marker >> digest) || digest != runtime_digest) return false;
      const std::pair<const char *, std::uintmax_t> files[] = {
        {"nvvfx/libs/libnppc.so.12", 1656080}, {"nvvfx/libs/libnppial.so.12", 22046288},
        {"nvvfx/libs/libnppicc.so.12", 9373288}, {"nvvfx/libs/libnppidei.so.12", 13633464},
        {"nvvfx/libs/libnppig.so.12", 55871152}, {"nvvfx/libs/libcudnn.so.9", 125136},
        {"nvvfx/libs/libNVCVImage.so", 4943832}, {"nvvfx/libs/libnvngxruntime.so", 79776},
        {"nvvfx/libs/libVideoFXLocal.so", 4206473}, {"nvvfx/libs/libVideoFX.so", 4198857},
        {"nvvfx/libs/libnvVFXTrueHDR.so", 4203601}, {"nvvfx/libs/libnvidia-ngx-truehdr.so.1.2.3", 3845656},
        {"nvidia_vfx.libs/libcudart-256e6409.so.12.9.79", 2115321},
      };
      for (const auto &[name, size] : files) {
        const auto path = root / name;
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error) return false;
        if (std::filesystem::file_size(path, error) != size || error) return false;
      }
      return true;
    }
  }  // namespace

  std::filesystem::path runtime_path() {
    return platf::appdata() / "truehdr-runtime" / runtime_digest;
  }

  runtime_status_t runtime_status() {
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
    auto &manager = runtime_manager();
    std::lock_guard lock {manager.mutex};
    if (manager.downloading) {
      std::uint64_t downloaded = 0;
      std::ifstream progress(runtime_path().parent_path() / ".download-progress");
      progress >> downloaded;
      return {"downloading", {}, downloaded};
    }
    if (runtime_ready(runtime_path())) return {"ready", {}};
    if (!manager.error.empty()) return {"error", manager.error};
    return {"missing", {}};
#else
    return {"unsupported", {}};
#endif
  }

  bool globally_enabled() {
    return global_enabled.load();
  }

  void prepare_runtime_async(bool retry) {
#ifdef SUNSHINE_ENABLE_NV_TRUEHDR
    auto &manager = runtime_manager();
    {
      std::lock_guard lock {manager.mutex};
      if (manager.downloading || runtime_ready(runtime_path()) || (!retry && !manager.error.empty())) return;
      manager.downloading = true;
      manager.error.clear();
    }
    try {
      std::thread([] {
      std::string failure;
      try {
        const auto destination = runtime_path().string();
        GError *error = nullptr;
        GSubprocess *child = g_subprocess_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &error,
                                              "/usr/bin/python3", SUNSHINE_TRUEHDR_HELPER, destination.c_str(), "--runtime-only", nullptr);
        if (!child) {
          failure = error ? error->message : "Could not start the NVIDIA runtime installer";
          g_clear_error(&error);
        } else {
          gchar *out = nullptr, *err = nullptr;
          if (!g_subprocess_communicate_utf8(child, nullptr, nullptr, &out, &err, &error) || !g_subprocess_get_successful(child)) {
            failure = error ? error->message : (err && *err ? err : "NVIDIA runtime download failed");
          } else if (!runtime_ready(runtime_path())) {
            failure = "The downloaded NVIDIA runtime is incomplete";
          }
          g_free(out);
          g_free(err);
          g_clear_error(&error);
          g_object_unref(child);
        }
      } catch (const std::exception &exception) {
        failure = exception.what();
      }
      if (failure.size() > 1000) failure = failure.substr(failure.size() - 1000);
      auto &manager = runtime_manager();
      std::lock_guard lock {manager.mutex};
      manager.error = std::move(failure);
      manager.downloading = false;
      }).detach();
    } catch (const std::exception &exception) {
      std::lock_guard lock {manager.mutex};
      manager.downloading = false;
      manager.error = exception.what();
    }
#else
    (void) retry;
#endif
  }

  void notify_live_settings_changed() {
    auto next = std::make_shared<frame_settings_t>();
    const auto &values = config::video.rtx_hdr;
    global_enabled.store(values.enabled);
    if (values.enabled) prepare_runtime_async();
    next->enabled = values.enabled && config::runtime_config_override_enabled("rtx_hdr");
    next->sdr_brightness = std::clamp(values.sdr_brightness, 0, 100);
    next->values = {std::clamp(values.contrast + 100, 0, 200), std::clamp(values.saturation + 100, 0, 200), std::clamp(values.middle_gray, 10, 100), std::clamp(values.peak_brightness, 400, 2000)};
    const auto previous = current.load();
    if (previous->generation && previous->enabled == next->enabled && previous->values == next->values && previous->sdr_brightness == next->sdr_brightness) {
      return;
    }
    next->generation = generation.fetch_add(1) + 1;
    current.store(std::move(next));
  }

  std::shared_ptr<const frame_settings_t> frame_settings() {
    return current.load();
  }

  live_output_metadata_state_t live_output_metadata_state() {
    const auto settings = current.load();
    return {settings->generation, settings->values.peak};
  }

  foreground_t::foreground_t():
      worker([this](std::stop_token stop) {
        poll(stop);
      }) {}

  foreground_t::~foreground_t() {
    worker.request_stop();
    wake.notify_all();
  }

  bool foreground_t::matches() const {
    const auto age = std::chrono::steady_clock::now().time_since_epoch() - std::chrono::nanoseconds(observed_at.load());
    return matched.load() && app_id.load() == proc::proc.current_app_id() && age < std::chrono::seconds(2);
  }

  void foreground_t::poll(std::stop_token stop) {
    while (!stop.stop_requested()) {
      const int id = proc::proc.current_app_id();
      bool match = false;
      try {
        const auto app = proc::proc.active_app_snapshot();
        if (!app) {
          // A brief lifecycle/catalog lock is not a foreground change. Keep
          // the last observation until its normal two-second TTL, without
          // refreshing it or letting it survive an application transition.
          if (id <= 0 || id != app_id.load()) {
            matched.store(false);
          }
          std::unique_lock lock(mutex);
          wake.wait_for(lock, stop, std::chrono::milliseconds(500), [] {
            return false;
          });
          continue;
        }
        std::string provider, identity;
        if (app && !app->steam_id.empty()) {
          provider = "steam";
          identity = app->steam_id;
        } else if (app && !app->lutris_id.empty()) {
          provider = "lutris";
          identity = app->lutris_id;
        } else if (app && !app->cmd.empty()) {
          const auto parts = boost::program_options::split_unix(app->cmd);
          if (!parts.empty()) {
            provider = "executable";
            identity = parts.front();
          }
        }
        if (id > 0 && !identity.empty()) {
          const bool machine = std::getenv("VIBESHINE_MACHINE_HOST") != nullptr;
          GError *error = nullptr;
          auto *process = g_subprocess_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &error, machine ? "/usr/libexec/vibeshine/vibeshine-session-exec" : "/usr/libexec/vibeshine/vibeshine-app-focus", machine ? "rtx-hdr-foreground" : "--foreground", provider.c_str(), identity.c_str(), nullptr);
          if (process) {
            const auto reply = linux_private_display::helper_process::communicate_until(process, std::chrono::steady_clock::now() + std::chrono::milliseconds(1500), machine);
            match = reply.success && reply.stdout_text == "1\n";
            g_object_unref(process);
          }
          g_clear_error(&error);
        }
      } catch (...) {
        match = false;
      }
      // Never publish a result for the app which was replaced during IPC.
      matched.store(false);
      app_id.store(id);
      observed_at.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
      matched.store(match && id == proc::proc.current_app_id());
      std::unique_lock lock(mutex);
      wake.wait_for(lock, stop, std::chrono::milliseconds(500), [] {
        return false;
      });
    }
    matched.store(false);
  }
}  // namespace platf::rtx_hdr
