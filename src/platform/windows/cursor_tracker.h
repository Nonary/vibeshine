// SPDX-License-Identifier: GPL-3.0-or-later
// Windows hooks into the client-side cursor tracker (src/host_cursor.h).
#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include <dxgi1_2.h>

namespace host_cursor::tracker {
  // Held by a Desktop Duplication capture. While one is alive, the pointer shape
  // DXGI reports (what Windows draws on that output) replaces GetIconInfo().
  std::shared_ptr<void> dxgi_shape_source();

  // Called on the capture thread after GetFramePointerShape(). Only copies the
  // buffer; conversion happens on the tracker thread.
  void note_dxgi_pointer_shape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO &info, std::span<const std::uint8_t> data);
}  // namespace host_cursor::tracker
