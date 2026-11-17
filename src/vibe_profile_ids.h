// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

// Private requested identity 1209:7140; allocation pending (PR #1384).
// Private endpoint builds only until allocation is confirmed. Zero rejects every unlisted layout, container or valid-bit pair.
constexpr std::uint16_t VibeLabUsbProduct(unsigned channels, unsigned mask,
                                         unsigned containerBits, unsigned validBits) noexcept {
    const bool pcm24 = containerBits == 32 && validBits == 24;
    const bool pcm16 = containerBits == 16 && validBits == 16;
    if (!pcm24 && !pcm16) return 0;
    if (channels == 2 && mask == 0x3) return 0x7140;
    if (channels == 6 && mask == 0x3f) return 0x7140;
    if (channels == 6 && mask == 0x60f) return 0x7140;
    if (channels == 8 && mask == 0x63f) return 0x7140;
    if (channels == 12 && mask == 0x2d63f) return 0x7140;
    return 0;
}
