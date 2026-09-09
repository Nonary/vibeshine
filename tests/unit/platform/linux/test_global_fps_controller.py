#!/usr/bin/env python3
"""Run the real Linux controller and desktop helper against isolated lease state.

Only dependency includes, logging, and installed paths are substituted. The
controller's spawn/monitor/lifecycle code, config type, policy, and helper are
compiled from production sources. Run as an unprivileged Linux user with C++20.
"""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
if not sys.platform.startswith("linux") or os.getuid() == 0:
    print("SKIP: controller runtime regression requires an unprivileged Linux user")
    sys.exit(77)


def declaration(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + ";"


with tempfile.TemporaryDirectory(prefix="global-fps-controller-") as temporary:
    temporary = Path(temporary)
    helper = temporary / "vibeshine-global-fps"
    manifest = temporary / "layer.json"
    manifest.write_text("{}")
    config_type = declaration((ROOT / "src/config.h").read_text(), "struct frame_limiter_t")
    (temporary / "test_config.h").write_text(
        "#include <string>\n#include <cstdint>\nnamespace config {\n" + config_type +
        "\ninline frame_limiter_t frame_limiter;\n}\n"
    )
    source_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "src/platform/linux/global_fps.cpp"
    controller = source_path.read_text().replace('#include "src/config.h"', '#include "test_config.h"')
    controller = controller.replace('#include "src/logging.h"',
                                    '#include <sstream>\n#define BOOST_LOG(level) std::ostringstream{}')
    installed_helper = '"/usr/libexec/vibeshine/vibeshine-global-fps"'
    assert controller.count(installed_helper) == 1
    controller = controller.replace(installed_helper, json.dumps(str(helper)))
    harness = temporary / "controller.cpp"
    harness.write_text(controller + r'''
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
  using namespace platf::global_fps;
  using namespace std::chrono_literals;
  void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
  }
  lease_t read_state() {
    std::ifstream input(lease_path(std::getenv("HOME")), std::ios::binary);
    lease_t state;
    input.read(reinterpret_cast<char *>(&state), sizeof(state));
    return input ? state : lease_t {};
  }
  void require_running(std::uint32_t expected) {
    require(is_active(), "controller is inactive while a lease should be running");
    const auto state = read_state();
    require(valid(state, monotonic_ns()) && state.limit_millihz == expected,
            "published lease does not match requested FPS");
  }
  void require_stopped() {
    stop();
    require(!is_active(), "stop did not clear active state");
    const auto state = read_state();
    require(state.limit_millihz == 0 && state.expires_ns == 0,
            "graceful stop left a limiter lease enabled");
  }
  void configure(std::string provider, bool enabled = true) {
    stop();
    config::frame_limiter = {};
    config::frame_limiter.provider = std::move(provider);
    config::frame_limiter.enable = enabled;
    set_managed_steam(false);
  }
  pid_t helper_child() {
    // Linux records children against the actual spawning thread, which must
    // remain alive while its PR_SET_PDEATHSIG child owns the limiter lease.
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/task")) {
      std::ifstream children(entry.path() / "children");
      pid_t child;
      while (children >> child) {
        std::error_code error;
        if (std::filesystem::read_symlink("/proc/" + std::to_string(child) + "/exe", error) ==
            TEST_HELPER_PATH) return child;
      }
    }
    return -1;
  }
}

int main() {
  try {
    require(is_available(), "test manifest was not recognized");
    configure("global", false);
    start(60, false);
    require(!is_active(), "disabled limiter started for a physical display");
    require_stopped();

    configure("global");
    config::frame_limiter.fps_limit_millihz = 59940;
    // A stream callback can return after start(). The limiter must still own
    // a helper and refresh its lease after that calling thread has exited.
    std::thread caller([] { start(120, false); });
    caller.join();
    const auto first_expiry = read_state().expires_ns;
    std::this_thread::sleep_for(800ms);
    require_running(59940);
    require(read_state().expires_ns > first_expiry,
            "helper stopped refreshing after the caller thread returned");
    require_stopped();
    require_stopped();
    std::cout << "PASS: caller-thread exit, fractional override, lease refresh and stop\n";

    // Both protocols use this shared controller. Exercise RTSP-first and
    // WebRTC-first joins with different FPS, preserving the original helper
    // until the final shared-runtime stop (individual departures do not stop).
    for (const auto &[first_fps, joining_fps] : {std::pair {120, 60}, std::pair {60, 120}}) {
      configure("global");
      start(first_fps, false);
      const auto original_child = helper_child();
      require(original_child > 0, "initial stream has no helper child");
      std::thread joining([joining_fps] { start(joining_fps, false); });
      joining.join();
      require_running(static_cast<std::uint32_t>(first_fps) * 1000);
      require(helper_child() == original_child, "joining stream replaced the active helper");
      // A departure leaves the first policy in place; subsequent joins do too.
      start(joining_fps, false);
      require_running(static_cast<std::uint32_t>(first_fps) * 1000);
      require_stopped();
      start(joining_fps, false);
      require_running(static_cast<std::uint32_t>(joining_fps) * 1000);
      require_stopped();
    }
    for (int fps : {30, 60, 144}) {
      std::thread next([fps] { start(fps, false); });
      next.join();
      require_running(static_cast<std::uint32_t>(fps) * 1000);
      require_stopped();
    }
    std::cout << "PASS: both protocol join orders preserve first policy, final stop and reconnect\n";

    configure("auto");
    set_managed_steam(true);
    start(60, false);
    require(!is_active(), "automatic global limiter duplicated managed Steam limiting");
    set_managed_steam(false);
    start(120, false);
    require(!is_active(), "joining stream replaced managed Steam's no-global-limiter policy");
    require_stopped();
    start(120, false);
    require_running(120000);
    require_stopped();
    set_managed_steam(true);
    config::frame_limiter.provider = "global";
    start(60, false);
    require_running(60000);
    require_stopped();
    for (const char *provider : {"", "auto", "proton", "mangohud-proton"}) {
      configure(provider);
      start(60, false);
      require_running(60000);
      require_stopped();
    }
    configure("none");
    start(60, false);
    require(!is_active(), "none provider started a limiter");
    configure("global", false);
    start(60, true);
    require_running(60000);
    start(120, false);
    require_running(60000);
    require_stopped();
    start(120, false);
    require(!is_active(), "disabled physical-display limiter unexpectedly started");
    start(60, true);
    require(!is_active(), "joining virtual stream replaced the original disabled policy");
    require_stopped();
    config::frame_limiter.virtual_display_capture_mode =
      config::frame_limiter_t::virtual_display_capture_mode_e::disabled;
    start(60, true);
    require(!is_active(), "disabled virtual display policy started a limiter");
    std::cout << "PASS: provider selection and virtual display opt-in\n";

    configure("global");
    for (const int fps : {0, -1, 1001}) {
      start(fps, false);
      require(!is_active(), "invalid stream FPS started a limiter");
      require_stopped();
    }
    config::frame_limiter.fps_limit_millihz = maximum_limit_millihz + 1;
    start(60, false);
    require(!is_active(), "invalid override started a limiter");
    require_stopped();
    config::frame_limiter.fps_limit_millihz = 59940;
    start(0, false);
    require_running(59940);
    require_stopped();
    config::frame_limiter.fps_limit_millihz = 0;
    const std::filesystem::path manifest = VIBESHINE_FRAME_LIMITER_MANIFEST;
    std::filesystem::rename(manifest, manifest.string() + ".saved");
    require(!is_available(), "missing manifest reported as available");
    start(60, false);
    require(!is_active(), "missing layer started a helper");
    std::filesystem::rename(manifest.string() + ".saved", manifest);
    const std::filesystem::path helper = TEST_HELPER_PATH;
    std::filesystem::rename(helper, helper.string() + ".saved");
    start(60, false);
    require(!is_active(), "failed helper spawn reported active");
    std::filesystem::rename(helper.string() + ".saved", helper);
    std::cout << "PASS: invalid limits and unavailable layer/helper\n";

    start(60, false);
    require_running(60000);
    const pid_t child = helper_child();
    require(child > 0 && kill(child, SIGKILL) == 0, "could not kill test helper");
    const auto monitor_deadline = monotonic_ns() + 2000000000;
    while (is_active() && monotonic_ns() < monitor_deadline) std::this_thread::sleep_for(10ms);
    require(!is_active(), "helper crash was not observed by the controller");
    const auto crashed = read_state();
    require(crashed.expires_ns <= monotonic_ns() + lease_duration_ns,
            "crashed helper left an unbounded lease");
    while (valid(read_state(), monotonic_ns())) std::this_thread::sleep_for(10ms);
    start(120, false);
    require_running(60000);
    require_stopped();
    start(120, false);
    require_running(120000);
    require_stopped();
    std::cout << "PASS: helper crash expiry, retry of original policy and reconnect\n";
  } catch (const std::exception &error) {
    platf::global_fps::stop();
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
''')
    compiler = os.environ.get("CXX", "c++")
    flags = ["-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=undefined", "-fno-sanitize-recover=undefined"]
    subprocess.run([compiler, *flags, "-I", str(ROOT),
                    str(ROOT / "packaging/linux/vibeshine-global-fps.cpp"),
                    "-o", str(helper)], check=True)
    binary = temporary / "controller"
    subprocess.run([compiler, *flags, "-I", str(temporary),
                    "-I", str(ROOT / "src/platform/linux"),
                    "-DVIBESHINE_FRAME_LIMITER_MANIFEST=" + json.dumps(str(manifest)),
                    "-DTEST_HELPER_PATH=" + json.dumps(str(helper)),
                    str(harness), "-o", str(binary)], check=True)
    environment = dict(os.environ, HOME=str(temporary))
    environment.pop("VIBESHINE_MACHINE_HOST", None)
    subprocess.run([str(binary)], env=environment, check=True, timeout=40)
