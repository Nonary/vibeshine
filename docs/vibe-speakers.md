# Native Vibe Speakers (Windows)

The optional user-mode USB Audio 2.0 endpoint supplies stereo, 5.1 (back or side),
7.1 or native 7.1.4 at 48 kHz. PCM24 has 24 valid bits in a 32-bit container;
PCM16 is an explicit compatibility choice. Internally the path uses Float32
and the host's existing Opus encoder and encrypted RTP/FEC transport. The twelve
channels are FL FR FC LFE BL BR SL SR TFL TFR TBL TBR. No upmixer, estimated
object, proprietary Dolby codec, custom kernel driver or GPU audio device is
part of this contribution. A conventional game cannot acquire height content
merely by selecting twelve channels.

## Pending identity: source review, not a public endpoint release

All profiles currently use requested **1209:7140**, with distinct instance
serials. [pidcodes/pidcodes#1384](https://github.com/pidcodes/pidcodes/pull/1384)
has not been treated as an allocation. Source review is allowed; functional
public endpoint installers/releases are blocked. `SUNSHINE_ENABLE_VIBE_SPEAKERS`
defaults OFF. Enabling it also requires `SUNSHINE_VIBE_PRIVATE_IDENTITY=ON` on
Windows. CPack refuses these private builds and rejects a stray Bridge executable
even in an otherwise ordinary package. Do not turn off the guard to publish one.

## Dependencies and setup

Uses unmodified Microsoft-signed usbip-win2 0.9.8.1 (upstream pinned transport
package) and Windows `usbaudio2.sys`. No local SYS/INF/CAT alteration, signing,
certificate trust, TESTSIGNING or security-policy change. Windows 11 x64 and
active HVCI/VBS/signature enforcement are required. Shared USB/IP packages may
belong to other features: they are reused only when compatible and are never
removed by Vibe Speakers teardown. Source attribution and the BSD ABI notice
are retained in `src/vibe-speakers/vibe-usb-audio/THIRD-PARTY.md`.

In a privately staged build, run `VibeSpeakers/Preflight.cmd` first. It reads
compatibility, signatures, hashes, identity collisions and ownership state.
`VibeSpeakers/Setup.cmd` asks for explicit administrator confirmation and creates
only a controller owned by the current user. Normal streaming is unelevated.
`VibeSpeakers/Remove.cmd` removes only that marked controller, keeping shared
drivers. Interrupted/foreign journals are rejected rather than adopted.

The host reads the administrator-protected setup journal for its current SID;
portable configuration cannot redirect it to another USB/IP controller. It
checks the protected ACL, owner, schema, nonce marker and driver service.
The bridge/host exchange pins process lifetimes, sessions, file identities and
SHA256, use local restricted pipes and cancelable bounded queues. No network
USB/IP server is exposed. No developer machine name/UUID is a runtime prerequisite.

## Session behavior

In either existing audio UI select **Audio capture: Vibe Speakers** and
**Bit depth: 24-bit** (or 16-bit). Defaults retain the existing capture paths.
The negotiated client layout selects the corresponding USB profile. Format and
render-role snapshots are restored at session end, with actual detach/role
notifications observed before restoring. Configuration edits apply to the next
session. Conflicting owners, stale epochs, gaps, slow consumers and unsupported
layouts fail visibly; they cannot silently remove height channels or build an
unbounded backlog. Complete 240-frame blocks go straight to the production encoder.

Native12 requires [Common PR #4](https://github.com/Nonary/moonlight-common-c/pull/4)
and the native update to [client PR #40](https://github.com/Nonary/moonlight-qt/pull/40).
The Windows client automatically selects its static Spatial renderer for native12;
an unavailable compatible provider/output is an error. Legacy clients/modes keep
their previous negotiation. New builds still require fresh physical HDMI/soundbar,
4K120 and network acceptance. Historical private RC1 hardware success is not a
hardware qualification of this canary port.

## Regression commands

`tests/host714` builds the production parser, codec, packetizer, lifecycle/queue
policies and real Common replay receiver. `VibeUsbBridgeTests` exercises the
actual bridge framing, conversion, bounded queues and local pipe/process recovery.
These tests are distinct from a full Windows source/USB/host/RTSP run. Reports
must state whether the receiving Windows renderer is physical or simulated.
