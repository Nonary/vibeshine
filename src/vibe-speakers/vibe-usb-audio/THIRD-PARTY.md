# Source and license provenance

This new module is GPL-3.0-or-later; see `LICENSE`. It adapts USB/IP framing,
overlapped IOCTL lifetime management and exact-location ownership checks from
the existing Vibeshine tree at commit
`148fc0237bb1cf1b30acafa90b186934b24e4bf3`:

* `src/platform/windows/dualsense_usbip_transport.cpp`, original SHA256
  `B99DB1B96A868A818C40899F3B81C58A59D0444FA08AD050A2F15658BAC81522`.
* `src/platform/windows/dualsense_usbip_protocol.cpp`, original SHA256
  `9E01349699221A8C7476AAE0D8FD261C1EBB0250CDD78DE72D8169474F3F0DCB`.

The USB Audio 2.0 descriptor topology and controls here are new. No Sony
identity, HID report descriptor, haptics split, controller feature report,
game hook or copied hardware audio descriptor is included.

`include/usbip_win2_abi.h` retains the complete BSD-2-Clause notice for Vadym
Hrynchyshyn's public release ABI; only its C++ namespace changes. Original
Vibeshine copy SHA256:
`B781BCFEB898C925BB3A97D429236D4975BD06B5AD286CEAD966FD6CFEA013F2`.
Canonical upstream release is [usbip-win2 v0.9.8.1](https://github.com/vadimgrn/usbip-win2/tree/55e1fa7f0c2157017b02dc1f3236e98169a535e4),
commit `55e1fa7f0c2157017b02dc1f3236e98169a535e4`.
The Microsoft-signed release driver is used unchanged and is not built here.

Protocol references:

* [Microsoft USB Audio 2.0 driver documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers):
  PCM subslot sizes/valid bits, fixed clock CUR/RANGE, explicit feedback and
  the documented restriction for arbitrary channel counts above eight in
  shared mode. Actual Windows format support must be measured separately.
* Vibe Speakers uses USB identity `1209:7140` for all supported profiles,
  with unique instance serial numbers.

This source is a modified standalone implementation. The cited upstream
authors do not certify this emulator, its descriptor choices or its tests.
