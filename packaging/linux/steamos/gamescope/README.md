# Patched Gamescope HDR10 capture

The patch and Vibeshine consumer implement HDR10 capture of Gamescope's existing
scene. The patch is pinned to Valve Gamescope **3.16.23.5**, commit
`1290cbc1a7ca625688bde8728d8e3b1e703d6a40`. `source-lock.json` records the patch
checksum. It does not create an independent virtual output or change panel modes.

An opt-in second patch adds **startup selection of a separate DRM scanout
device** for virtual-monitor hardware validation on Linux, including CachyOS.
It is a prerequisite, not live Gaming Mode virtual-monitor support. See
[the implementation status and validation sequence](VIRTUAL-DISPLAY.md#implementation-status--2026-09-12).

The patch also tracks Steam overlay changes when deciding whether to capture a
new frame, preventing overlay-only menu updates from waiting for a game commit.
See [the virtual-display investigation](VIRTUAL-DISPLAY.md) for the Gaming Mode
integration blockers and remaining capture issues. This source update requires
a rebuilt compositor and has not yet been validated in a live Gaming Mode session.

Test the repaint decision after applying the patch to the pinned source:

```bash
python3 packaging/linux/steamos/tests/test-gamescope-repaint.py /path/to/patched/gamescope
```

## Capture contract

The separate `vibeshine_capture_v1` Wayland global advertises profile 1 for the
existing `gamescope_pipewire` node. Valve's interface and SDR formats remain
unchanged. Vibeshine requires both this capability and actual negotiated
`xBGR_210LE`, full-range RGB, BT.2020 primaries and ST 2084 transfer. An unknown
profile, mismatched node, missing extension, memory-only buffer, or incompatible
format cannot silently enable HDR. A format change during HDR capture restarts
the stream instead of sending SDR pixels with HDR signaling.

The compositor renders PQ before physical-display tone mapping. Capture uses
its own LUTs, without panel looks or LUT overrides. SDR uses BT.709 primaries
converted into BT.2020, with 203 cd/m² white. The SDR shaper fills its LUT domain
to preserve that white point; the upstream screenshot shaper otherwise loses
brightness at its clamped endpoint. PQ sources retain their absolute luminance.

The advertised metadata describes the **capture reference volume**:
BT.2020/D65, 0–10,000 cd/m². It is not source mastering metadata or a measurement
of the Deck panel. MaxCLL and MaxFALL remain unknown (zero). Existing producer
GPU completion precedes frame delivery; this patch adds neither explicit
fences nor authoritative presentation timestamps.

HDR uses Vibeshine's VAAPI or Vulkan DMA-BUF path. Software RGB conversion and
CUDA are not enabled for this profile. The producer still has one negotiated
format: simultaneous SDR and HDR consumers require separate compositor
instances. Capture resolution remains bounded by the compositor's output.

## Build

Use a SteamOS-compatible SDK with Gamescope's normal development dependencies,
including Meson/Ninja, Vulkan, glslang, Wayland, PipeWire, DRM, SDL2, X11/Xwayland,
libinput, libei, libdecor, libseat, libdisplay-info, libavif, libcap and LuaJIT.
No package installation is performed by this script.

```bash
python3 packaging/linux/steamos/gamescope/build-gamescope.py \
  /tmp/gamescope-hdr-work /tmp/gamescope-hdr-stage
```

Both directories must be new. The script checks the upstream commit and patch
checksum, initializes pinned submodules, applies the patch, builds and stages
under `opt/vibeshine-gamescope/`. Additional SDK link flags can be passed using
`--meson-option=-Dcpp_link_args=...`. OpenVR is disabled for this Deck build;
the DRM and nested backends are retained. Wlroots warnings are not fatal because
newer libinput headers add an enum absent from Valve's pinned wlroots revision.

Build against the target OS ABI. A binary built on a newer Arch system is not
automatically SteamOS-compatible. See the local SDK notes in [AUDIT.md](../AUDIT.md).
This staging command does not activate a system extension, set file capabilities
or replace the live compositor. The [system-extension release requirements](../sysext/README.md)
still apply before privileged Gaming Mode deployment.

### Experimental separate scanout device

Add `--experimental-drm-scanout` to the build command to apply the second,
checksum-pinned patch and run its device-selection regression checks. The
default build retains only the HDR capture patch. `build-features.json` in the
staging root records which variant was built.

The experimental compositor accepts `--drm-device /dev/dri/cardN` separately
from `--prefer-vk-device vendor:device` and `--prefer-output Virtual-N`.
The first selects a KMS primary node; the second selects the Vulkan renderer;
the third selects a connector **on that KMS device**. Resolve the actual card
and connector on the test host; card numbers are not stable identifiers.
An invalid or inaccessible explicit node fails without choosing another card.

On a separate card, Gamescope fully composes the scene and waits for Vulkan
completion before submitting it. Existing Vulkan export/plane import modifier
intersection is retained; an empty intersection fails with a diagnostic.
The patch also handles missing optional plane fences at teardown and the
XBGR2101010 format on a primary-only device. Producer-buffer retention through
capture and encoding still needs a hardware audit.

This option is startup-only. It does not migrate an existing Steam session,
lease a connector, apply client modes, restore the physical display, or provide
simultaneous physical/virtual outputs. Use it only in a separately controlled
test session on an unused managed connector until the hardware gates pass.

The device-selection test can also run without Linux graphics libraries:

```bash
python3 packaging/linux/steamos/tests/test-gamescope-scanout.py /path/to/patched/gamescope
```

## Hardware validation

Build Vibeshine first, then compile the integration probe from its actual
objects, in the same SDK:

```bash
python3 packaging/linux/steamos/tests/build-hdr-probe.py \
  /path/to/vibeshine-build /tmp/hdr-probe
cc -O2 -I/tmp/gamescope-hdr-work/build/protocol \
  packaging/linux/steamos/tests/hdr-pattern.c \
  /tmp/gamescope-hdr-work/build/protocol/gamescope-swapchain-protocol.c \
  -lwayland-client -lX11 -lm -o /tmp/hdr-pattern
```

Run on the Deck with its user PipeWire session:

```bash
python3 packaging/linux/steamos/tests/run-hdr-smoke.py \
  --gamescope /tmp/gamescope-hdr-work/build/src/gamescope \
  --scripts /tmp/gamescope-hdr-work/source/scripts \
  --pattern /tmp/hdr-pattern --probe /tmp/hdr-probe/hdr-capture-probe \
  --payload /path/to/vibeshine-payload --output /tmp/hdr-results
```

The test owns a separate headless compositor and cleans it up on success or
failure. It checks captured PQ values for black, 100, 203, 1,000 and 4,000 nits
and RGB primaries, then encodes the captured image through the product's GPU
converter and HEVC encoder. It checks Main10/BT.2020/PQ signaling, decoded frame
count and decoded RGB values. Decoder errors fail even when FFmpeg exits zero.
Use `--mode sdr-source` for SDR white and `--mode stock --gamescope /usr/bin/gamescope`
to verify stock HDR rejection and SDR availability. `--capture-only` explicitly
skips encoding; it is not a stream-validation pass. `--encoder vulkan` selects
the alternative hardware path. Encoding defaults to limited-range HEVC;
`--full-range` verifies the full-range client option. Both consume the same
full-range RGB capture buffers.

### SteamOS Mesa prerequisite

The tested host's Mesa 26.1.2 produces malformed HEVC slice headers on this Deck.
The failure also reproduces with system FFmpeg, independently of Vibeshine.
Upstream fixed this exact firmware interaction in commit
[`5af4976e`](https://gitlab.freedesktop.org/mesa/mesa/-/commit/5af4976e23a6dd361f5a1e0d26595b47d228c191),
included in Mesa 26.1.7 and 26.2.1. Require a SteamOS driver build carrying that
fix and rerun the full decode test; a version string alone is insufficient.

For local validation, Mesa 26.1.7 and libva 2.24.1 were installed **only in the
development container**. `--probe-container moonlight-dev` tests those libraries
while sharing the host GPU and PipeWire sockets. That proves the patch and
encoder path with the fix, not that the installed host driver is fixed.
Do not install Arch graphics packages into SteamOS to satisfy this requirement.

Moonlight playback on an HDR client, native/Proton HDR games, Gaming Mode
activation and update/rollback testing remain release gates. Local artifacts
are development candidates, not signed system-extension releases.
