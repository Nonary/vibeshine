"""Compile the production legacy refresh handler with live game-mode fakes.

Usage: python3 tests/unit/test_display_helper_live_mode.py [repo] [compiler] [legacy-source]
The handler must update only refresh and must never enqueue a launch-mode replay.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"
legacy_source = pathlib.Path(sys.argv[3]) if len(sys.argv) > 3 else root / "tools/display_settings_helper.cpp"
source = legacy_source.read_text()
start = source.index("} else if (type == MsgType::RefreshRate) {")
start = source.index("{", start) + 1
end = source.index("} else if (type == MsgType::Ping)", start)
handler = source[start:end]

program = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <strings.h>
#include <vector>
using namespace std::chrono_literals;
#define BOOST_LOG(level) std::clog
#define _stricmp strcasecmp
struct Configuration { std::string m_device_id = "VIRTUAL"; std::optional<bool> m_hdr_state = true; };
struct Controller {
  int width = 1920;  // Game changed from the configured 3840-pixel launch mode.
  bool hdr = false;  // User toggled HDR after launch.
  unsigned numerator = 60, denominator = 1;
  bool succeed = true;
  int calls = 0;
  bool set_device_refresh_rate(const std::string &, unsigned n, unsigned d) {
    ++calls;
    if (succeed) { numerator = n; denominator = d; }
    return succeed;
  }
};
struct State {
  std::optional<Configuration> last_cfg = Configuration {};
  std::atomic<std::uint64_t> refresh_rate_override {0};
  Controller controller;
  bool recovery_ready = true;
  bool prepare_recovery_baseline(const char *) { return recovery_ready; }
  int cancellations = 0;
  std::vector<std::chrono::milliseconds> retries;
  void cancel_delayed_reapply() { ++cancellations; retries.clear(); }
  void schedule_delayed_reapply(std::vector<std::chrono::milliseconds> delays) { retries = delays; }
  void drain_delayed_repairs() {
    for (auto delay : retries) {
      (void)delay;
      // Production best_effort_apply_last_cfg repeats the original resolution/HDR.
      controller.width = 3840;
      controller.hdr = last_cfg->m_hdr_state.value_or(controller.hdr);
    }
    retries.clear();
  }
};
enum class MsgType { RefreshRateResult };
std::optional<unsigned> read_u32_le(std::span<const std::uint8_t> payload, std::size_t offset) {
  if (payload.size() < offset + 4) return std::nullopt;
  unsigned result = 0;
  for (unsigned i = 0; i < 4; ++i) result |= unsigned(payload[offset + i]) << (8 * i);
  return result;
}
void send_framed_content(bool &pipe, MsgType, const std::array<std::uint8_t, 1> &result) {
  pipe = result.front() != 0;
}
std::vector<std::uint8_t> payload_for(unsigned numerator, unsigned denominator, const std::string &device) {
  std::vector<std::uint8_t> payload;
  for (auto value : {numerator, denominator})
    for (unsigned i = 0; i < 4; ++i) payload.push_back((value >> (8 * i)) & 0xff);
  payload.insert(payload.end(), device.begin(), device.end());
  return payload;
}
void refresh(State &state, std::span<const std::uint8_t> payload, bool &async_pipe) {
'''
program += handler + r'''
}
int main() {
  // Test both HDR toggle directions during a running fullscreen game.
  for (bool hdr : {false, true}) {
    State state;
    state.last_cfg->m_hdr_state = !hdr;
    state.controller.hdr = hdr;
    state.retries = {750ms};
    bool result = false;
    refresh(state, payload_for(144, 1, "virtual"), result);
    assert(result && state.controller.calls == 1 && state.cancellations == 1);
    state.drain_delayed_repairs();
    assert(state.controller.width == 1920 && state.controller.hdr == hdr);
    assert(state.controller.numerator == 144);
    assert(state.refresh_rate_override == (std::uint64_t(144) << 32 | 1));
  }
  // A failed refresh never turns into a broad retry of launch configuration.
  State failed;
  failed.controller.succeed = false;
  bool result = true;
  refresh(failed, payload_for(144, 1, "VIRTUAL"), result);
  assert(!result && failed.refresh_rate_override == 0);
  failed.drain_delayed_repairs();
  assert(failed.controller.width == 1920 && !failed.controller.hdr);
  State unprotected;
  unprotected.recovery_ready = false;
  refresh(unprotected, payload_for(144, 1, "VIRTUAL"), result);
  assert(!result && unprotected.controller.calls == 0 && unprotected.refresh_rate_override == 0);
  assert(unprotected.retries.empty());
  // Invalid requests cannot mutate the output or enqueue a delayed retry.
  State invalid;
  refresh(invalid, payload_for(0, 1, "VIRTUAL"), result);
  assert(!result && invalid.controller.calls == 0 && invalid.retries.empty());
}
'''
with tempfile.TemporaryDirectory(prefix="vibeshine-live-mode-") as temp:
    path = pathlib.Path(temp)
    fixture, binary = path / "live-mode.cpp", path / "live-mode"
    fixture.write_text(program)
    subprocess.run([compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", str(fixture), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Legacy live fullscreen resolution and HDR preservation scenarios passed")
