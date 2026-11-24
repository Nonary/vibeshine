# Steam Controller (2026) over the VHF virtual gamepad

On Windows, Vibeshine can expose a native Steam Controller (2026) through its
Virtual HID Framework (VHF) gamepad driver. The driver must offer the
Steam Controller profile, and the client must report the controller as
`LI_CTYPE_STEAM` with at least one of the following capabilities.

- `LI_CCAP_GRIP_SENSE` (grip sensing)
- `LI_CCAP_STEAM_HAPTIC` (pad haptics)
- `LI_CCAP_SC26_BUTTONS` (explicit pad clicks)

One client implementation is the [modified Moonlight Android client](https://github.com/jlobue10/moonlight-android).
Steam on the host sees a Steam Controller with two trackpads, capacitive
grips and stick touch, motion sensors, a battery, and pad haptics.

## Selection

| `gamepad` setting | When the client reports the Steam Controller type and a capability listed above |
|---|---|
| `auto` | Prefers the VHF Steam Controller profile. If unavailable, it can try the driver's automatic profile and fall back to ViGEmBus. |
| `vhf` | Tries the VHF Steam Controller profile, then the driver's automatic profile if needed. If VHF allocation fails, it can fall back to ViGEmBus. |
| `vhf_steam` | Requests the VHF Steam Controller profile. If the host cannot create it, it refuses to substitute another profile. |

With `vhf_steam`, all other controller reports use automatic profile selection
based on the type and capabilities the client sends. This includes a physical
Steam Controller that the client forwards as an Xbox or generic gamepad, or
a controller reported as `LI_CTYPE_STEAM` with only generic capabilities.
If the host reaches VHF allocation for one of these reports, its log records
the reported type and capabilities and says that the controller gets the
automatic choice.

## What the host maps

* **Buttons and clicks.** The grip-sense (`LI_CCAP_GRIP_SENSE`), stick-touch
  (`LI_CCAP_STICK_TOUCH`) and pad/trigger click (`LI_CCAP_SC26_BUTTONS`) flags
  ride in the upper half of the button word and pass straight to the driver.
  A client that advertised authoritative clicks is marked with the driver's
  `sc26_buttons_explicit` metadata bit, derived from the arrival capability
  (client input cannot set it), so an all-released first report is
  authoritative and the driver stops guessing clicks from trigger travel.
* **Trackpads.** A client with `LI_CCAP_DUAL_TOUCHPAD` addresses the pads by
  touchpad index (0 left, 1 right). A single-touchpad client sends both pads
  as the halves of one DualShock-style pad; the host maps the half a finger
  went down on to that pad for the pointer's moves and release and stretches
  the half back to the pad's full width.
* **Motion and battery.** Motion is requested at the IMU's native 250 Hz;
  battery state and level are forwarded as for the PlayStation profiles.

## Feedback

Steam drives the pads through the controller's haptic output reports (0x80
rumble, 0x81 pulse, 0x82 command, 0x83 LFO tone, 0x84 sweep, 0x85 script) and
writes its per-controller settings over the control channel. The driver hands
both back to the host verbatim.

* A client that advertised `LI_CCAP_STEAM_HAPTIC` receives every haptic
  report as control packet `0x5504` and replays it on the real pads. Nothing
  is deduplicated: Steam re-sends the same click to keep a buzz going.
* Any other client gets rumble synthesized from the report: 0x80 motor speeds
  with the real unit's safety timeout, 0x81 pulse trains as a duty cycle for
  the train's length, 0x82 ticks and clicks as a short timed rumble from their
  gain. The two report families number their sides the opposite way round
  (0x81: side 1 is the left pad; 0x82: side 0 is; side 2 is both), as
  measured on the real unit.
* A client that advertised `LI_CCAP_STEAM_SETTINGS` receives settings writes
  (`SET_SETTINGS_VALUES` triples, at most nine per packet, or
  `LOAD_DEFAULT_SETTINGS`) as control packet `0x5505`, so the real controller's
  firmware runs with Steam's settings (haptic gain and intensity among them).
  Settings are never coalesced; a chunk of a longer write cannot replace its
  siblings. The virtual device keeps answering Steam from its own table.

Haptic reports and synthesized rumble follow Vibeshine’s existing unconditional
rumble forwarding. Settings writes are forwarded independently.

When a slot is torn down the host logs the driver's feedback-drop counters for
it (`haptic_evicted`, `settings_evicted`, `transitions_dropped`,
`unknown_commands`), the only way to tell a quiet controller from one whose
output the driver had to discard, and discards any feedback still queued for
the slot's client-relative index before it can be reused.

## Firmware attributes and Steam's update notice

The virtual controller answers Steam's attribute reads (GET_ATTRIBUTES_VALUES
0x83, GET_DEVICE_INFO 0xF2) with defaults captured from one wired unit, so
Steam used to show "controller firmware update available" for the streamed
controller whenever those defaults fell behind. A client advertising
`LI_CCAP_STEAM_ATTRIBUTES` (0x4000) reads the real controller's attributes at
link setup (USB cable, puck and BLE alike) and sends them after the arrival
event as the `SS_STEAM_CONTROLLER_ATTRIBUTES` input packet (magic
`0x55000008`, 84 bytes, every u32 little-endian): firmware and bootloader
build times, board revision, capabilities, radio/dongle firmware build times,
build id and the unit/board serials, with a mask saying which were actually
read.

The host honours the packet only for a controller the arrival announced as
`LI_CTYPE_STEAM` with that capability, sanitises the strings (printable ASCII,
at most 15 characters) and hands the six fields the driver takes to the VHF
driver's `set_steam_controller_attributes` call. The radio and dongle build
times are logged only: the virtual device is a wired unit. An installed driver
that predates the call answers `ERROR_REVISION_MISMATCH`; the host logs one
info line and the virtual controller keeps its defaults. On success the host
log shows what Steam will see:

```
VHF gamepad 0 (Steam Controller): real controller firmware build 2026-07-07 23:04:03 UTC (0x6a4d85e3), bootloader 2025-09-23 19:46:54 UTC (0x68d2f92e), board rev 74, build id 26e1f7ef1955, serial FXA..., via usb
```

Steam re-polls the attributes periodically, so a packet that arrives late is
picked up. Attributes are reset whenever the slot is recreated and are re-sent
with the next arrival.

> **Warning:** Do not start a Steam Controller firmware update from Steam while streaming: the update would target this virtual controller, not the real one. Update with the controller connected directly to a PC.
> Vibeshine logs this warning every time a Steam Controller slot is created.

## Protocol notes

The new client type, capability bits, button flags and packet types are
defined once in `src/steam_controller_protocol.h`. Each is a fallback define
guarded by a `static_assert`, so a `moonlight-common-c` that carries the real
names with other values fails the build instead of silently masking a drift.
The bits and packet types are additive: a client that does not advertise a
capability never receives the corresponding packet.

## Driver dependency

The profile, the extra button bits, the `steam_haptic`/`steam_settings`
feedback types, the per-slot feedback counters and the
`set_steam_controller_attributes` call are part of the libvirtualgamepad
protocol (header and driver). The host must be built against
a libvirtualgamepad that defines them and ships a driver that offers the
profile. If the installed driver lacks support, the result depends on the
setting as described in [Selection](#selection).

## Tests

* `tests/unit/platform/windows/test_vhf_gamepad_policy.cpp`: profile
  selection, the button mask and the explicit-click metadata bit, split-pad
  touch ownership, haptic/settings decoding and rumble synthesis (including
  the side conventions), the feedback counters and the firmware attribute
  relay (packet layout, gating, field and flag mapping, string sanitising,
  UTC formatting and the log line).
* `tests/input/test_haptic_queue.py`, `test_steam_settings_forward.py`,
  `test_vhf_feedback_retirement.py`, `test_vhf_fallback.py`,
  `test_sc26_buttons.py`, `test_steam_attributes_forward.py`: see
  `tests/input/README.md`.
