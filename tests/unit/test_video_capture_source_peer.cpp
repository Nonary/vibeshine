#include <display_device/windows/win_api_recovery.h>

// A separate translation unit must observe the same thread-local guard.
void check_capture_display_from_peer() {
  display_device::check_display_query_stop();
}
