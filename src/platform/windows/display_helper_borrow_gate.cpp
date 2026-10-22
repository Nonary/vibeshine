#include "src/platform/windows/display_helper_borrow_gate.h"

namespace display_helper::borrow {
  thread_local std::map<const gate_t *, std::size_t> gate_t::depth_;
}
