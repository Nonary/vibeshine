#pragma once

namespace platf::global_fps {
  // App identity survives a paused stream so reconnects reacquire the lease.
  void set_managed_steam(bool managed);
  // The first stream owns the policy; later joins reuse it until the shared
  // runtime's final stop. A failed helper may be retried with that same policy.
  void start(int fps, bool virtual_display);
  void stop();
  bool is_active();
  bool is_available();
}  // namespace platf::global_fps
