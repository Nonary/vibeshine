# Client-side cursor and automatic mouse mode

Status: implemented on `feature/client-side-cursor` in Vibeshine, moonlight-common-c and moonlight-qt. Unit tested; manual qualification follows [the checklist](client-cursor-test-checklist.md).

## Problem

Moonlight offers two mouse models today. Game mode sends relative motion and the
user sees the host cursor in the video, so pointer movement carries the full
capture, encode, network and decode latency. "Optimize mouse for remote desktop"
sends absolute positions, but the cursor the user sees is still the host cursor
in the video, and most games that capture the mouse do not work in it. The user
has to pick a mode and toggle it with Ctrl+Alt+Shift+M as they move between the
desktop and a game.

Parsec-style streaming solves both: the client draws the cursor locally as a real
OS cursor using the host's shape, so movement feels local, and the client
switches to relative mode on its own when a game hides or locks the cursor.

## Goals and non-goals

- The feature activates only when both sides support it. Stock Moonlight against
  this host, and this client against stock Sunshine/Apollo/Vibepollo or an older
  Vibeshine, behave exactly as today.
- Windows hosts first. Linux hosts do not advertise the feature yet.
- With automatic mode turned off on the client, behaviour is identical to today.
- Non-goals for v1: animated cursors (the current frame is sent), per-client
  cursor ownership when several clients drive one host cursor at once.

## Where things live

| Concern | Host (Vibeshine) | Client |
|---|---|---|
| Wire format | `src/host_cursor_protocol.h` (header-only, LE) | `moonlight-common-c/src/HostCursor.h` |
| Shape conversion | `src/host_cursor_bitmap.{h,cpp}` (portable) | — (client receives straight BGRA only) |
| Session policy | `src/host_cursor.{h,cpp}` (portable) | `app/streaming/input/cursormodepolicy.h` (pure) |
| OS polling | `src/platform/windows/cursor_tracker.cpp` | — |
| Rendering | blend/skip in DDA and WGC capture | `app/streaming/input/hostcursor.cpp` (SDL colour cursors) |

### Findings that shaped the design

- **No helper process is needed for cursor APIs.** In service mode,
  `tools/sunshinesvc.cpp` starts `Sunshine.exe` as SYSTEM *inside the active
  console session* (not session 0). Threads attach to the current input desktop
  with `platf::syncThreadDesktop()`, which also reaches the Winlogon/secure
  desktop. The tracker therefore runs as a thread in the main process. The WGC
  helper runs with the user's token and cannot see the secure desktop, so it is
  the wrong place.
- **Shapes.** Desktop Duplication already reads `GetFramePointerShape` on the
  capture thread (`display_vram.cpp`, `display_ram.cpp`). That shape is what
  Windows actually draws on the captured output (per-monitor DPI and the user's
  cursor-size setting included), so it is preferred. The capture thread only
  copies the raw buffer; conversion happens on the tracker thread. WGC never sees
  pointer shapes, so on that path (and before the first DXGI shape arrives) the
  tracker reads the `HCURSOR` from `GetCursorInfo` with `GetIconInfo` +
  `GetDIBits`.
- **Compositing is shared.** One captured image feeds every session, so the host
  stops blending only when *every* connected session has asked to draw the
  cursor itself. The `COMPOSITED` state flag tells a client when the host is
  blending anyway, so it never shows a second cursor.
- **Control stream limits.** A control message carries at most ~65 KB
  (`u16` lengths, `encode_control`), and client→host messages must stay under
  252 bytes (`sendMessageEnet`'s 256-byte buffer). Both sides already ignore
  unknown message types (host logs at debug, client frees silently), so new types
  are backward compatible on their own.
- **Callbacks.** `CONNECTION_LISTENER_CALLBACKS` has a frozen V1 layout (an ABI
  test pins it), so the new callbacks are registered with a setter, following
  `LiSetVideoReassemblyDeadlineCallback`.

## Negotiation

1. **Host advertises** `LI_FF_HOST_CURSOR = 0x00010000` in the existing
   `a=x-ss-general.featureFlags` DESCRIBE attribute, only on Windows. Upstream
   Sunshine, Apollo and Vibepollo allocate this space sequentially from 0x01 (only
   0x01 and 0x02 are used today), so a fork-owned high bit cannot be confused with
   a future upstream flag.
2. **Client opts in at runtime** by sending `CURSOR_CONTROL` with `SUBSCRIBE`
   once the control stream is up, and only if the host advertised the flag and
   the user's "Automatic mouse mode" setting is on. The host sends no cursor
   messages and keeps compositing as today until a session subscribes. A runtime
   message (rather than an `x-ml-general.featureFlags` bit) means stock Moonlight
   can never opt in by accident and the client can change `CLIENT_DRAWS` at any
   time.
3. **Client stays in fallback until the first `RESYNC` state arrives.** A host
   that sets the bit for some other reason, or never answers, leaves the client
   on the user's existing mouse setting.

## Wire format (version 1)

All messages travel inside the encrypted control stream (`0x0001` envelope).
Multibyte fields are little-endian; structures are packed. Type IDs are scoped
by direction; the block 0x5610–0x5612 is unused by every known host and client.

Host→client messages use ENet **reliable channel 0x09** (`CTRL_CHANNEL_CURSOR`),
so a 256 KB shape never blocks rumble, HDR or termination on channel 0, and
shapes and the states that reference them stay ordered relative to each other.

### `CURSOR_SHAPE` (host→client, type 0x5610)

| Offset | Size | Field | Rule |
|---|---|---|---|
| 0 | 1 | version | 1 |
| 1 | 1 | format | 1 = B8G8R8A8, straight alpha, top-down, tightly packed |
| 2 | 2 | flags | 0 |
| 4 | 4 | shape_id | non-zero |
| 8 | 2 | width | 1..256 |
| 10 | 2 | height | 1..256 |
| 12 | 2 | hotspot_x | < width |
| 14 | 2 | hotspot_y | < height |
| 16 | 4 | total_size | width × height × 4 |
| 20 | 4 | chunk_offset | equals bytes already received for this id |
| 24 | 2 | chunk_length | 1..16384, offset + length ≤ total_size |
| 26 | 2 | reserved | 0 |
| 28 | n | pixels | `chunk_length` bytes |

A shape larger than 16 KiB arrives as consecutive chunks of the same id. The
client keeps one assembly slot. A chunk that does not continue it (different id,
wrong offset, inconsistent header) discards the partial shape. Only a complete,
fully validated shape reaches the application.

### `CURSOR_STATE` (host→client, type 0x5611)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | version (1) |
| 1 | 1 | flags: `0x01 VISIBLE`, `0x02 FALLBACK`, `0x04 HAS_POSITION`, `0x08 HAS_CLIP`, `0x10 COMPOSITED`, `0x20 RESYNC` |
| 2 | 2 | reserved (0) |
| 4 | 4 | seq (per session, increments per state) |
| 8 | 4 | shape_id (0 = no shape) |
| 12 | 2 | ref_width (stream coordinate space, > 0) |
| 14 | 2 | ref_height (> 0) |
| 16 | 4 | scale_q16: stream pixels per host cursor pixel (16.16, > 0) |
| 20 | 4 | x (i32, stream coordinates, valid with `HAS_POSITION`) |
| 24 | 4 | y |
| 28 | 4 | clip_left (i32, valid with `HAS_CLIP`) |
| 32 | 4 | clip_top |
| 36 | 4 | clip_right (exclusive) |
| 40 | 4 | clip_bottom (exclusive) |

Coordinates are in the stream's coordinate space: `(0,0)`–`(ref_width,
ref_height)` is the whole encoded frame, including any letterbox the host adds
when the output's aspect differs from the stream's. That is the same space the
client already uses for absolute input, so the client maps it with its existing
letterbox maths.

- **Sent only on change**, plus a full state with `RESYNC` (and every shape it
  needs) whenever a session subscribes, including after a reconnect. Nothing is
  sent until the session's encoder has published its input geometry, so a client
  that subscribes early still gets its resync.
- **`HAS_POSITION`** is set on `RESYNC`, when the cursor becomes visible, when the
  clip changes, when the host moved the cursor itself (application warp, a local
  mouse), and, while the client is *not* drawing the cursor, on every position
  change. The last case keeps the client ready to warp when it returns to
  absolute mode; it is not echo, because a non-drawing client is not sending
  absolute positions. Absolute positions injected for a client are never
  reported back as warps.
- **`FALLBACK`**: cursor information is unavailable (tracker failure, the input
  desktop cannot be opened). The host also resumes compositing.

### `CURSOR_CONTROL` (client→host, type 0x5612)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | version (1) |
| 1 | 1 | flags: `0x01 SUBSCRIBE`, `0x02 CLIENT_DRAWS` |
| 2 | 2 | reserved (0) |
| 4 | 4 | resend_shape_id (0 = none) |

Each message replaces the session's subscription state. `SUBSCRIBE` going from
0 to 1 triggers a resync. A non-zero `resend_shape_id` asks for that shape again
(the client got a state that names an id it does not have).

### Malformed input

Neither side trusts the other. A message with the wrong version, a short or
oversized payload, out-of-range dimensions, non-zero reserved fields, or
inconsistent chunk bookkeeping is dropped (logged once per session). The client
never allocates more than 256 KiB for an assembly. Fuzz-style truncation tests
cover every field boundary on both sides.

## Host design

### Tracker (Windows)

`cursor_tracker` is a reference-counted singleton that runs a thread only while
at least one session is subscribed. Every 5 ms (200 Hz) it:

1. Calls `GetCursorInfo`. Visible means `CURSOR_SHOWING` (or `CURSOR_SUPPRESSED`,
   which only means touch/pen input is active) and a non-null `hCursor`.
   `SetCursor(NULL)`, the common way games hide the cursor, therefore reads as
   hidden.
2. Reads the position from `CURSORINFO.ptScreenPos`, the clip from
   `GetClipCursor`, and the virtual-screen origin and size. A clip equal to the
   virtual screen means "not clipped".
3. Resolves the shape: the latest DXGI shape while Desktop Duplication is
   capturing, otherwise `GetIconInfo`/`GetDIBits` when the `HCURSOR` changes or
   once a second (handles can be reused and the cursor-size setting can change).
   Converted shapes are deduplicated by a 64-bit content hash in a bounded
   registry (64 shapes); ids are small integers.
4. Detects host-originated moves: the input path records the last eight absolute
   positions it injected (`note_absolute_injection`). A new position that matches
   none of them (3 px tolerance) increments a warp serial. Comparing against a
   short history rather than only the last target avoids reporting the gap
   between recording a target and `SendInput` taking effect. Relative input
   (gamepad mouse emulation, a trackpad-mode touchscreen) counts as a host move,
   so a client drawing the cursor follows it.
5. Publishes a snapshot when anything changed.

Once a second it re-binds to the input desktop, which catches a switch to the
secure desktop that does not make `GetCursorInfo` fail. A desktop it cannot open
(the secure desktop when Sunshine is not SYSTEM), or a failing poll, reports
`available = false` until a poll succeeds.

The poll is a handful of user32 calls: about 0.9 µs on average on a test machine
(about 0.02% of a core at 200 Hz). The tracker logs its average and maximum cost
at debug level every 30 s.

### Per-session state (portable)

The control thread already owns each session's encryption state and sends, so
all cursor sends happen there. On every iteration, for each subscribed session
with a peer, `host_cursor::session_state_t` maps the newest snapshot to stream
coordinates with the session's current input geometry (`touch_port`):

```
stream_x = (screen_x - virtual_left - offset_x) / scalar_inv + client_offsetX
```

This is the exact inverse of `client_to_touchport()` plus `platf::abs_mouse()`,
so warps land where absolute input would have put the cursor. It builds the wire
state, compares it with the last one sent, sends any shape the client has not
had yet (each shape once per session, ≤ 2 MB/s per session), then the state.
While any session is subscribed, the loop's ENet wait drops from 150 ms to 5 ms,
the same rule haptics clients already use.

### Compositing

`host_cursor::compositing_required()` is false only when at least one session is
connected, every connected session is subscribed with `CLIENT_DRAWS`, and the
tracker is available. `display_base_t::capture` ANDs it with the existing
`display_cursor` toggle (Ctrl+Alt+Shift+N) before every snapshot, which covers
the DDA VRAM/RAM paths and WGC (whose helper already toggles
`IsCursorCaptureEnabled` at runtime, no session restart). Linux capture paths are
untouched.

### Linux

`platf::cursor_tracker` has no Linux implementation, so the flag is not
advertised and nothing else changes. A future PipeWire implementation would
request `SPA_META_Cursor` (cursor mode `METADATA`) and feed the same snapshot and
shape registry.

## Client design

### moonlight-common-c

- `LiSetHostCursorCallbacks(stateCb, shapeCb)` registers the callbacks before
  `LiStartConnection` (NULL disables them; unregistered means host cursor
  messages are dropped).
- Shapes and states are validated and delivered synchronously on the control
  receive thread, like haptics PCM. Callbacks must copy and return.
- `LiSendHostCursorControl(flags, resendShapeId)` sends `CURSOR_CONTROL`; it
  fails when the host did not advertise `LI_FF_HOST_CURSOR`.

### moonlight-qt

The callbacks copy the payload and push `SDL_CODE_HOST_CURSOR_STATE` /
`SDL_CODE_HOST_CURSOR_SHAPE` user events. `SdlInputHandler` handles them on the
main thread.

#### Setting

`automaticMouseMode` (QSettings key `automaticmousemode`), default **on** for
new and existing installs: the key is new, so every existing profile reads the
default and nobody's `absoluteMouseMode` changes. The existing "Optimize mouse
for remote desktop" option and Ctrl+Alt+Shift+M define the fallback behaviour.

The settings page lists "Automatic mouse mode on supported hosts" above the
remote-desktop option, which reads "Optimize mouse for remote desktop on hosts
without automatic mode" while automatic mode is on. `--automatic-mouse` /
`--no-automatic-mouse` override it from the command line. With the setting off,
no callback is registered, nothing is subscribed, and the stats overlay has no
mouse line.

#### State machine

```
             host unsupported / setting off / FALLBACK flag / no RESYNC yet
                    ┌──────────────────────────────────────┐
                    ▼                                      │
              ┌──────────┐   RESYNC, wants absolute   ┌────┴───────────┐
              │ FALLBACK │ ─────────────────────────▶ │ ABSOLUTE_LOCAL │
              └──────────┘                            └────────────────┘
                    │  RESYNC, wants relative          ▲            │
                    ▼                                  │ visible &  │ hidden or locked
              ┌──────────┐ ───────────────────────────┘ free for   │ for 60 ms
              │ RELATIVE │   ≥ 100 ms                    100 ms     │
              └──────────┘ ◀──────────────────────────────────────┘
```

- **Wants relative** when the host cursor is hidden, clipped to a tiny rect
  (both sides < 10 % of the stream or < 32 stream pixels), or being recentred
  (three or more host warps within 300 ms that land within 3 % of the stream of
  each other).
- **ABSOLUTE_LOCAL**: absolute input; the local OS cursor shows the host shape
  (unless `COMPOSITED`); a large clip confines the local cursor to the mapped
  rect with `SDL_SetWindowMouseRect`; host warps move the local cursor if it is
  inside the video region.
- **RELATIVE**: SDL relative mode; local cursor hidden; video cursor shown if the
  host draws one.
- **FALLBACK**: the user's existing setting, exactly as today.
- **Hysteresis** (constants in `cursormodepolicy.h`): enter RELATIVE after 60 ms,
  return after 100 ms, and at least 150 ms between automatic switches. A brief
  hide (an MMO click, a loading flicker) never switches; a camera drag that hides
  the cursor for seconds switches once each way.
- **Typing**: Windows can hide the pointer while the user types. A hide that
  starts within 1 s of a (non-modifier) key press waits for mouse motion and then
  120 ms of the cursor staying hidden. The host shows the pointer again on the
  first move, so typing never switches, while a game resumed with a key press
  still does once the mouse moves.
- **RELATIVE → ABSOLUTE** releases relative mode, warps the local cursor to the
  host's last reported position (inverse letterbox mapping), then re-enables
  absolute capture.
- **Overrides**: Ctrl+Alt+Shift+M in automatic mode switches to the other mode and
  locks it; the new **Ctrl+Alt+Shift+A** locks the current mode, or returns to
  automatic if a mode is locked. A lock lasts until it is released or the session
  ends. In fallback M behaves as today; A is only intercepted once the host
  supports automatic mode, so on other hosts it reaches the host as before.
- **Focus and capture**: switches while the window is unfocused or capture is
  released do not warp or grab; a relative switch while unfocused releases
  capture, as game mode does today, and a click recaptures.

#### Rendering

Shapes are cached by id (≤ 64 shapes, freed at session end). The SDL colour
cursor is built at `scale_q16 × (video width in window units / ref_width)` with
premultiplied bilinear filtering, so it matches its in-stream size. On Windows
window units are physical pixels (per-monitor DPI manifest); on macOS and
Wayland they are points, so the OS scales the cursor. A scale change (resize,
fullscreen toggle) rebuilds the current cursor.

#### Stats overlay

One line: `Mouse mode: absolute, local cursor (automatic)`, `relative (locked)`,
`absolute (fallback: host does not support automatic mode)`, and so on.

## Edge cases

| Case | Behaviour |
|---|---|
| UAC / secure desktop | The tracker binds to Winlogon as SYSTEM; DDA captures it and supplies shapes. If binding fails (Sunshine not running as SYSTEM), `FALLBACK` and host compositing. |
| Lock screen | Same as secure desktop. |
| Text I-beam, resize cursors | Monochrome/masked inverting pixels become black with a 1 px white outline. |
| Software cursor (game draws its own, hides the OS cursor) | Hidden → RELATIVE, which is what a game with a software cursor usually expects; the game's cursor stays in the video. RTS games with software cursors and absolute expectations can be switched to absolute and locked there with Ctrl+Alt+Shift+M. |
| RTS with confined visible cursor | Large clip → ABSOLUTE_LOCAL confined to the mapped rect. |
| Multi-monitor host | Positions off the streamed output are reported as-is; absolute input still only reaches the streamed output. |
| Several clients | The host blends if any client needs it (`COMPOSITED` tells the others). One client's absolute moves are not reported as warps to another (v1 limitation). |
| Reconnect | A new session subscribes again and gets a full resync; caches are per session. |
| Pathological cursor churn | Shape sends are budgeted per session; the client keeps its last shape. |
| Wayland client | Warping is a no-op; the cursor appears where the user left it. |
| Gamepad mouse emulation, trackpad-mode touch | Relative input moves the host cursor away from the client's last absolute position, which the host reports as a host move, so the local cursor follows. |
| Capture released (Ctrl+Alt+Shift+Z) or window unfocused in relative mode | The client stops drawing (`CLIENT_DRAWS` off) and the host composites again, so the video shows the cursor. |

## Testing

- Host gtest: wire encode/parse with byte-exact vectors and truncation, bitmap
  conversion (monochrome, colour, masked, XOR outline), geometry inverse mapping,
  session diff/resync/warp rules, compositing aggregate.
- common-c: shape reassembly (chunking, reordering, oversize, truncation at every
  byte), state parsing, dispatch through `ControlStream.c`.
- moonlight-qt: state machine and hysteresis timing, cursor scaling, settings
  defaults and migration.
- Manual: `docs/client-cursor-test-checklist.md`.

## Known limitations

- Animated cursors show their current frame.
- With several clients moving one host cursor, one client's absolute moves are
  not reported to the others as host moves.
- Shapes are capped at 256x256 on the wire; larger Windows cursors are box-filtered
  by an integer factor (the scale in `CURSOR_STATE` accounts for it).
- macOS and Wayland clients draw the cursor at point resolution; Wayland cannot warp.
- The tight-clip and recentring thresholds are heuristics. A title they misjudge
  can be locked with Ctrl+Alt+Shift+M or A.

## Follow-ups

- Linux hosts: PipeWire `SPA_META_Cursor` (cursor mode `METADATA`) feeding the same
  snapshot and shape registry, then advertising the flag.
- Vibepollo: the host side is self-contained (`host_cursor*`, `cursor_tracker`,
  and small hooks in `stream.cpp`, `rtsp.cpp` and the Windows capture/input files),
  so it ports directly.
- Animated cursors: frame list plus delays in `CURSOR_SHAPE`, using the reserved
  flags field.
- High-DPI cursors on macOS/Wayland clients via SDL3 alternate cursor images.
- Skip re-encoding the unchanged last frame on a Desktop Duplication pointer-only
  update while the host is not drawing the cursor (today, as with
  Ctrl+Alt+Shift+N, a moving cursor still produces duplicate frames). Returning a
  timeout there is not enough: the capture loop sleeps 10 ms after a timeout and
  busts its pacing group, so this needs its own path.
