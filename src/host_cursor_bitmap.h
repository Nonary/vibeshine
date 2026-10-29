// SPDX-License-Identifier: GPL-3.0-or-later
// Converts Windows cursor bitmaps into the straight-alpha BGRA shapes the client
// draws. Portable so the conversion rules are unit tested on every platform.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace host_cursor_bitmap {
  struct image_t {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t hotspot_x = 0;
    std::uint16_t hotspot_y = 0;
    double pixel_scale = 1;  // display pixels per shape pixel (> 1 after downscaling a huge cursor)
    std::vector<std::uint8_t> bgra;  // width * height * 4, straight alpha, top-down

    bool operator==(const image_t &) const = default;
  };

  // A pixel that inverts the screen cannot be drawn by the client. It is
  // approximated as opaque black, and transparent pixels next to it become opaque
  // white, so an inverting I-beam stays visible on both light and dark content.

  // 1 bpp AND mask rows followed by the same number of XOR mask rows (DXGI
  // MONOCHROME and GDI cursors without a colour bitmap). `height` is the visible
  // height; `masks` holds 2 * height rows of `pitch` bytes, most significant bit
  // first. AND/XOR: 0/0 black, 0/1 white, 1/0 transparent, 1/1 invert.
  std::optional<image_t> from_monochrome(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t pitch,
    std::span<const std::uint8_t> masks,
    std::uint32_t hotspot_x,
    std::uint32_t hotspot_y
  );

  // 32 bpp BGRA with straight alpha (DXGI COLOR, GDI colour cursors with alpha).
  std::optional<image_t> from_color(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t pitch,
    std::span<const std::uint8_t> bgra,
    std::uint32_t hotspot_x,
    std::uint32_t hotspot_y
  );

  // DXGI MASKED_COLOR: the alpha byte is a mask. 0x00 replaces the screen with
  // the colour; 0xFF XORs it (black XOR is a no-op, so it is transparent).
  std::optional<image_t> from_masked_color(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t pitch,
    std::span<const std::uint8_t> bgra,
    std::uint32_t hotspot_x,
    std::uint32_t hotspot_y
  );

  // GDI colour cursor without alpha: a 1 bpp AND mask selects XOR (1) or replace (0).
  std::optional<image_t> from_color_and_mask(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t color_pitch,
    std::span<const std::uint8_t> bgra,
    std::uint32_t mask_pitch,
    std::span<const std::uint8_t> and_mask,
    std::uint32_t hotspot_x,
    std::uint32_t hotspot_y
  );

  // Display pixels per pixel of a cursor bitmap read with GetIconInfo(). Windows
  // draws a cursor of the standard size at the standard size for the DPI of the
  // display it is on, but GetIconInfo() returns it at the standard size for the
  // reading process's system DPI. The standard sizes are SM_CXCURSOR at each DPI.
  // A cursor of any other size is drawn as it is.
  double gdi_display_scale(std::uint32_t width, std::uint32_t height, std::uint32_t read_standard, std::uint32_t display_standard);

  // 64-bit FNV-1a over dimensions, hotspot, scale and pixels.
  std::uint64_t content_hash(const image_t &image);
}  // namespace host_cursor_bitmap
