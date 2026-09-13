# Gaming Mode virtual-display investigation — 2026-09-06

## Saved direction

On 2026-09-06, the user asked to retain this idea for future work and defer
implementation: **AMD rendering → managed virtual screen → completed-frame
capture**, keeping the existing Gaming Mode Steam session and its games together.
The aim is client-specific resolution, refresh rate, and HDR through the existing
virtual-display infrastructure. Treat this as the preferred direction to
investigate when the work resumes, not a validated solution or a request to
deploy it now. Buffer ownership and encoder throughput remain separate concerns.

The desired path is the existing Gaming Mode Steam session rendered on AMD,
presented to a managed `vibeshine_drm` connector, and captured through completed
DRM frames. This is a proposed compositor integration, not an available setting.
It would give the stream its own mode and HDR display contract. It does not by
itself establish an encoding frame-rate guarantee or eliminate synchronization
bugs.

## Evidence from this Deck

The user reports Big Picture and Hades menus freezing until gameplay resumes.
The September 6 user-service journal shows:

- 15:54:51: Gaming Mode launched Steam app 1145350 through the running client.
- 15:54:51–52: requested 3024×1890 at 120 fps; Gamescope negotiated 2560×1440
  HDR10 capture. The encoder still produced 3024×1890 frames.
- 16:01:15: average capture interval 15.47 ms, approximately 65 frames/s.
- 16:02:15: maximum capture interval 8849.92 ms. This proves a capture gap,
  not which menu was visible or whether the source should have been animating.
- Around 16:01–02: HEVC encode calls averaged approximately 10 ms, exceeding
  the 8.33 ms budget for sustained 120 fps in the serial encode loop.
- Later logs from process 749758 use KWin in Desktop Mode and should not be
  confused with the Gaming Mode session (process 741786).

## Menu-capture defect

In pinned Valve commit `1290cbc1a7ca625688bde8728d8e3b1e703d6a40`,
`src/steamcompmgr.cpp::paint_pipewire` suppresses capture when the focus and
override commit IDs are unchanged. It later paints `overlayWindow`, but the
overlay does not participate in that suppression decision. An overlay-only
update can therefore remain invisible in capture until a game commit arrives.
This matches the reported symptom without requiring multiple compositors.
It does not prove every observed Hades menu freeze has that cause.

The bundled patch now includes layer identity, commit, opacity, capture size,
and pixel format in the decision, and records state only after successful GPU
rendering. App-specific capture continues excluding Steam overlays. The
regression harness executes the patched decision block with synthetic window
states. It does not validate full compositor compilation or actual menu pixels.

## Why selecting Virtual-1 is insufficient

`src/platform/linux/gamescope_display_backend.cpp` explicitly routes sessions
to the existing Gamescope scene and clears their virtual-display request.
The managed-display integration depends on KScreen/KWin for mode and topology
changes; those APIs do not control Gaming Mode Gamescope.

The pinned Gamescope `src/Backends/DRMBackend.cpp::init_drm` obtains the KMS
device from `vulkan_primary_dev_id`, then opens that device's primary node.
The AMD render device and `vibeshine_drm` connector are different DRM devices.
Connector preference cannot cross that device boundary. Gamescope already has
DMA-BUF framebuffer import and backend modifier negotiation, which make a
separate scanout device a plausible extension, but compatibility is untested.

## Concrete implementation path

1. Add explicit scanout-device selection to Gamescope while retaining AMD as
   its Vulkan renderer. Negotiate the intersection of AMD export and virtual
   plane import formats/modifiers. Use the virtual connector's single primary
   plane so Gamescope composes menus and game content into the captured image.
2. Add a Gaming Mode display controller that acquires the managed connector
   lease, requests the client mode, and asks Gamescope to switch output. It
   must report the accepted mode and HDR state without invoking KScreen.
3. Route this session to the existing managed KMS capture helper, including
   presentation sequencing and GPU synchronization. Audit producer reuse as
   well as framebuffer lifetime; retaining a DMA-BUF descriptor alone does
   not prevent its pixels from being overwritten.
4. Preserve the existing Steam instance and launch handoff. A second headless
   Gamescope can render its own clients, but does not automatically move the
   running Steam UI or games launched by that Steam instance into it.
5. Restore the prior Gaming Mode output on disconnect or failure. Supporting
   simultaneous local and virtual output needs additional multi-output work;
   an initial single-output implementation would switch the session's output.

First validate a separate test compositor presenting an animated pattern to
an unused leased virtual connector, with AMD rendering and the completed-frame
probe. Do not switch the real Steam session until that produces correct frames,
HDR values, requested modes, and clean teardown. Then validate Big Picture,
Hades menus/gameplay, overlay visibility changes, reconnect, and restoration.
Measure 1080p60 and the requested 3024×1890/120 separately; reducing duplicate
composition cannot remove the measured encoder cost.

## Separate shared-capture risk

`src/platform/linux/pipewire.cpp::fill_img_dmabuf` duplicates buffer FDs.
`on_process` returns the previous buffer to PipeWire on the next frame, while
the encoder may still consume the duplicate. PipeWire permits reuse after
queueing. This is a buffer-ownership race consistent with tearing, not proof
that it caused the observed artifact. Fixing it requires retaining the producer
buffer through GPU read completion or copying into consumer-owned storage with
proper synchronization. It is not fixed by the menu patch.

## Implementation status — 2026-09-12

The virtual-monitor work has resumed. The first prerequisite patch,
`0002-separate-drm-scanout-device.patch`, adds explicit startup KMS device
selection while retaining Vulkan rendering on the chosen hardware GPU.
It forces full composition on a separate scanout device, including modeset
retry, and rejects invalid device selection or an empty export/import modifier
intersection. Build it with `--experimental-drm-scanout`; it is not enabled in
the default patched Gamescope build or in the host's capability report.

The regression harness executes the actual patched device-opening function
against simulated DRM/session APIs. It covers default and explicit selection,
same-device and separate-device rendering, missing renderer identification,
invalid paths, inaccessible devices, non-primary/non-KMS descriptors, stat
failure, and resource cleanup. Passing this harness does not prove compositor
compilation, GPU import, captured pixels, or stream playback.

Validation on September 12:

- Both checksum-pinned patches apply in order to a fresh archive of the locked
  Valve commit and reproduce the tested source files.
- The scanout-selection and existing overlay-repaint regression harnesses pass
  on the development Mac.
- The complete patched Gamescope compositor and WSI layer compile and link in
  an isolated Ubuntu 24.04 **ARM64** container with GCC 13.3 and Wayland 1.23.1.
  DRM, PipeWire, and SDL backends are enabled; OpenVR, input emulation, and AVIF
  screenshots are disabled for this compilation check. This is not a CachyOS
  or SteamOS x86_64 artifact, and the container has no GPU/KMS access.
- No live compositor, Steam session, driver, or host service was changed.

### CachyOS integration boundary

CachyOS `gamescope-session` revision
`f151b891b3936f1a90ae3c7a659adf2298f484ea` uses
`gamescope-session.service`, `gamescope-session.target`, and
`/usr/lib/steamos/gamescope-session`. The launcher publishes `DISPLAY` and
`GAMESCOPE_WAYLAND_DISPLAY` in `$XDG_RUNTIME_DIR/gamescope-environment`
before sending readiness. Its service deliberately unsets `XAUTHORITY` and
uses `XDG_SESSION_TYPE=x11` for applications, while SDDM owns a Wayland login
session. Detection must use the authoritative login and service state, not
the application's `XDG_SESSION_TYPE` or the distribution name.

The current Vibeshine machine controller and broker remain Plasma-specific.
They require Plasma service readiness, KScreen, and a desktop Xauthority
file. Gaming Mode needs a distinct verified session binding, bounded
environment discovery under the selected user's identity, and compositor
operations through the existing generation-bound broker. Merely accepting a
Gamescope socket in the controller would leave display/capture/application
operations with the wrong session contract.

### Next hardware gate and remaining implementation

1. Build the experimental compositor for the target Linux ABI. In an isolated
   session, present an animated pattern to an unused managed connector with
   the hardware GPU rendering and `vibeshine_drm` scanning out. Verify the
   accepted resolution/refresh, complete frames, HDR pixel values, and teardown.
2. Audit buffer reuse through the managed completed-frame capture path. Vulkan
   completion before DRM submission does not establish encoder ownership.
3. Implement a compositor control/lease protocol for switching the **existing**
   Gaming Mode session between physical and virtual output, acknowledged mode
   changes, and restoration on normal release, failed activation, and owner loss.
   Startup `--drm-device` selection alone cannot perform this transition.
4. Add the matching Gamescope controller/broker binding for both CachyOS and
   SteamOS, then connect the host display backend and KMS capture routing.
5. Validate Big Picture, games, overlay changes, reconnects, logout, suspend,
   and restore. Simultaneous local/virtual output remains a separate compositor
   requirement unless explicitly selected for this implementation.

CachyOS source: [session launcher](https://github.com/CachyOS/gamescope-session/blob/f151b891b3936f1a90ae3c7a659adf2298f484ea/usr/lib/steamos/gamescope-session),
[service](https://github.com/CachyOS/gamescope-session/blob/f151b891b3936f1a90ae3c7a659adf2298f484ea/usr/lib/systemd/user/gamescope-session.service).

References: [pinned compositor source](https://github.com/ValveSoftware/gamescope/blob/1290cbc1a7ca625688bde8728d8e3b1e703d6a40/src/steamcompmgr.cpp),
[pinned DRM backend](https://github.com/ValveSoftware/gamescope/blob/1290cbc1a7ca625688bde8728d8e3b1e703d6a40/src/Backends/DRMBackend.cpp),
[PipeWire buffer lifecycle](https://docs.pipewire.org/page_streams.html).
