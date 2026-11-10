"""Exercise the portable shell-refresh worker and check legacy APPLY wiring."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
compiler = sys.argv[1] if len(sys.argv) > 1 else "g++"
source = (root / "tools/display_settings_helper.cpp").read_text()
apply = source[source.index("  bool handle_apply("):source.index("  void handle_revert(")]
frame = source[source.index("  void handle_frame("):source.index("  void attempt_revert_after_disconnect(")]
adjuncts = source[source.index("    bool run_initial_apply_adjuncts("):source.index("  };", source.index("    bool run_initial_apply_adjuncts("))]
assert "refresh_shell_after_display_change(" not in adjuncts
assert "state.best_effort_apply_last_cfg(false)" in apply
assert apply.index("state.controller.apply(cfg, sunshine_topology)") < apply.index("refresh_shell = true") < apply.index("run_initial_apply_adjuncts(")
assert frame.index("handle_apply(") < frame.index("MsgType::ApplyResult, result_payload") < frame.index("state.shell_refresh.request()")

program = r"""
#include "tools/display_helper_shell_refresh_worker.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <thread>
using namespace std::chrono_literals;
int main() {
  // A slow refresh must not hold up repeated requests or cancellation. Pending
  // requests coalesce and a cancelled queued refresh never runs.
  {
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::atomic<int> calls {0};
    display_helper::shell_refresh_worker worker([&](std::stop_token) {
      if (++calls == 1) { entered.set_value(); gate.wait(); }
    });
    worker.request();
    assert(entered.get_future().wait_for(2s) == std::future_status::ready);
    worker.request(); worker.request(); worker.cancel_pending();
    release.set_value();
    std::this_thread::sleep_for(200ms);
    assert(calls == 1);
  }
  {
    std::promise<void> entered, release, second;
    auto gate = release.get_future().share();
    std::atomic<int> calls {0};
    display_helper::shell_refresh_worker worker([&](std::stop_token) {
      int n = ++calls;
      if (n == 1) { entered.set_value(); gate.wait(); }
      if (n == 2) second.set_value();
    });
    worker.request();
    assert(entered.get_future().wait_for(2s) == std::future_status::ready);
    worker.request(); worker.request();
    release.set_value();
    assert(second.get_future().wait_for(2s) == std::future_status::ready);
    std::this_thread::sleep_for(100ms);
    assert(calls == 2);
  }
  // Destruction requests stop, waits for the running callback, and never lets
  // the worker access destroyed state.
  {
    std::promise<void> entered;
    std::atomic<bool> observed_stop {false};
    {
      display_helper::shell_refresh_worker worker([&](std::stop_token stop) {
        entered.set_value();
        while (!stop.stop_requested()) std::this_thread::sleep_for(1ms);
        observed_stop = true;
      });
      worker.request();
      assert(entered.get_future().wait_for(2s) == std::future_status::ready);
    }
    assert(observed_stop);
  }
}
"""
with tempfile.TemporaryDirectory() as tmp:
    cpp = Path(tmp) / "worker.cpp"
    exe = Path(tmp) / "worker"
    cpp.write_text(program)
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(root), str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
print("shell refresh worker and APPLY wiring checks passed")
