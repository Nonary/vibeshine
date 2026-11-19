"""Run the production display-config retry loop against scripted Win32 calls."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile


def replay(root, lib):
    source = (lib / "src/windows/win_api_layer.cpp").read_text()
    cooldown = next(line for line in source.splitlines() if "constexpr DWORD QUERY_ERROR_COOLDOWN_MS" in line)
    wait = cooldown + "\n" + source[source.index("    void wait_for_query_retry("):
                                    source.index("    std::string queryTypeToString")]
    query = source[source.index("  std::optional<PathAndModeData> WinApiLayer::queryDisplayConfig"):
                   source.index("  std::string WinApiLayer::getDeviceId")]

    # These guards opt the real worker into the library behavior in both phases.
    host = (root / "src/video.cpp").read_text()
    worker = host[host.index("  void captureThread("):host.index("  int encode_avcodec(")]
    phases = worker.split("DisplayQueryStopGuard stop_guard {display_stop};")
    assert len(phases) == 3
    assert all(phase.index("refresh_displays(") < phase.index("ensure_virtual_display_ready(")
               for phase in phases[1:])
    start = host[host.index("  int start_capture_async(capture_thread_async_ctx_t &capture_thread_ctx) {"):]
    end = start[start.index("  void end_capture_async("):start.index("  int start_capture_sync(")]
    assert start.index("display_query_stop = std::stop_source {};") < start.index("std::thread")
    assert "display_query_stop.get_token()" in start
    assert "catch (const display_device::DisplayQueryStopped &)" in start
    assert end.index("display_query_stop.request_stop()") < end.index("capture_thread.join()")

    return r"""
#include <display_device/windows/win_display_query_stop.h>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
// Some Windows C++ runtimes include winerror.h through their thread headers.
#undef ERROR_SUCCESS
#undef ERROR_NOT_SUPPORTED
#undef ERROR_INVALID_PARAMETER
#undef ERROR_INSUFFICIENT_BUFFER
#undef ERROR_GEN_FAILURE
#undef QDC_ONLY_ACTIVE_PATHS
#undef QDC_ALL_PATHS
#undef QDC_VIRTUAL_MODE_AWARE
#undef Sleep
#define DD_LOG(level) std::ostringstream {}
namespace display_device {
using LONG = long;
using DWORD = unsigned long;
using UINT32 = unsigned int;
constexpr LONG ERROR_SUCCESS = 0, ERROR_NOT_SUPPORTED = 1, ERROR_INVALID_PARAMETER = 2,
               ERROR_INSUFFICIENT_BUFFER = 3, ERROR_GEN_FAILURE = 4;
constexpr UINT32 QDC_ONLY_ACTIVE_PATHS = 1, QDC_ALL_PATHS = 2, QDC_VIRTUAL_MODE_AWARE = 4;
constexpr int DISPLAYCONFIG_MODE_INFO_TYPE_TARGET = 1;
struct rate_t { unsigned Numerator = 0, Denominator = 1; };
struct path_end_t {
  struct { int HighPart = 0, LowPart = 0; } adapterId;
  int id = 0;
  rate_t refreshRate;
};
struct DISPLAYCONFIG_PATH_INFO { path_end_t sourceInfo, targetInfo; int flags = 0; };
struct DISPLAYCONFIG_MODE_INFO {
  int id = 0, infoType = 0;
  struct { struct { rate_t vSyncFreq; int pixelRate = 0; } targetVideoSignalInfo; } targetMode;
};
struct PathAndModeData {
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};
enum class DisplayRecoveryBehavior { Automatic, Skip };
namespace detail {
  DisplayRecoveryBehavior behavior = DisplayRecoveryBehavior::Automatic;
  DisplayRecoveryBehavior current_display_recovery_behavior() { return behavior; }
}
enum class QueryType { Active, All };
struct WinApiLayer {
  std::optional<PathAndModeData> queryDisplayConfig(QueryType type) const;
  std::string getErrorString(LONG) const { return {}; }
};
std::string dumpPathsAndModes(const auto &, const auto &) { return {}; }
std::vector<DWORD> waits;
std::vector<UINT32> sizing, queries;
std::stop_source *stop_during_size = nullptr, *stop_during_query = nullptr;
LONG outcome = ERROR_GEN_FAILURE;
std::size_t recover_after_queries = 0;
LONG GetDisplayConfigBufferSizes(UINT32 flags, UINT32 *paths, UINT32 *modes) {
  sizing.push_back(flags);
  *paths = *modes = 1;
  if (stop_during_size) stop_during_size->request_stop();
  return ERROR_SUCCESS;
}
LONG QueryDisplayConfig(UINT32 flags, UINT32 *paths, DISPLAYCONFIG_PATH_INFO *,
                        UINT32 *, DISPLAYCONFIG_MODE_INFO *, void *) {
  queries.push_back(flags);
  if (stop_during_query) stop_during_query->request_stop();
  const auto result = recover_after_queries && queries.size() > recover_after_queries ? ERROR_SUCCESS : outcome;
  if (result == ERROR_INSUFFICIENT_BUFFER) ++*paths;
  return result;
}
}  // namespace display_device
void fake_sleep(unsigned long delay) { display_device::waits.push_back(delay); }
#define Sleep fake_sleep
namespace display_device {
""" + wait + query + r"""
}  // namespace display_device

int failures = 0;
void check(bool ok, const char *label) {
  if (!ok) {
    ++failures;
    std::cerr << "FAIL: " << label << std::endl;
  }
}
int main() {
  using namespace display_device;
  WinApiLayer api;
  check(!api.queryDisplayConfig(QueryType::All), "ordinary exhaustion yields no result");
  check(queries.size() == 18 && waits == std::vector<DWORD>(18, 250), "automatic query count and cooldown");
  check(!queries.empty() && queries.front() == (QDC_ALL_PATHS | QDC_VIRTUAL_MODE_AWARE) && queries.back() == QDC_ALL_PATHS,
        "virtual-aware pass precedes compatibility pass");
  waits.clear(); sizing.clear(); queries.clear();
  outcome = ERROR_INSUFFICIENT_BUFFER;
  check(!api.queryDisplayConfig(QueryType::Active), "topology churn still exhausts its budget");
  const std::vector<DWORD> ladder {25, 50, 100, 200, 400, 500, 500, 500, 500,
                                   25, 50, 100, 200, 400, 500, 500, 500, 500};
  check(waits == ladder, "the 25 to 500 ms backoff is retained in each mode");
  check(sizing.size() == 2 && queries.size() == 18, "growing buffers avoid another size call");
  waits.clear(); sizing.clear(); queries.clear();
  detail::behavior = DisplayRecoveryBehavior::Skip;
  check(!api.queryDisplayConfig(QueryType::All), "fast probe still reports exhaustion");
  check(queries.size() == 6 && waits.empty(), "fast probes retain three attempts per mode and no waits");
  detail::behavior = DisplayRecoveryBehavior::Automatic;

  // A live capture worker must survive transient topology growth.
  waits.clear(); sizing.clear(); queries.clear();
  recover_after_queries = 2;
  {
    std::stop_source live;
    DisplayQueryStopGuard guard {live.get_token()};
    const auto recovered = api.queryDisplayConfig(QueryType::Active);
    check(recovered && recovered->paths.size() == 3 && recovered->modes.size() == 1,
          "live worker opens the recovered topology");
    check(sizing.size() == 1 && queries == std::vector<UINT32>(3, QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE),
          "recovery continues on the original query mode");
    check(waits.empty() && !live.stop_requested(), "live retry uses a stop-aware wait without cancelling");
  }
  recover_after_queries = 0;

  for (bool stop_at_sizing : {true, false}) {
    std::stop_source stop;
    sizing.clear(); queries.clear();
    outcome = ERROR_SUCCESS;
    stop_during_size = stop_at_sizing ? &stop : nullptr;
    stop_during_query = stop_at_sizing ? nullptr : &stop;
    bool cancelled = false;
    try {
      DisplayQueryStopGuard guard {stop.get_token()};
      (void) api.queryDisplayConfig(QueryType::All);
    } catch (const DisplayQueryStopped &) {
      cancelled = true;
    }
    check(cancelled, "cancellation discards a successful native result");
    check(sizing.size() == 1 && queries.size() == (stop_at_sizing ? 0 : 1) && waits.empty(),
          "no next query, retry wait or compatibility pass");
  }
  return failures != 0;
}
"""


if __name__ == "__main__":
    root = Path(sys.argv[1]).resolve()
    lib = Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else root / "third-party/libdisplaydevice"
    with tempfile.TemporaryDirectory(prefix="display-query-stop-") as directory:
        fixture = Path(directory) / "fixture.cpp"
        binary = Path(directory) / ("fixture.exe" if os.name == "nt" else "fixture")
        fixture.write_text(replay(root, lib))
        subprocess.run([sys.argv[2], "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
                        "-I", str(lib / "src/windows/include"), str(fixture), "-o", str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True)
    print("Windows display query retry ladder, live churn, native-result cancellation and worker wiring passed")
