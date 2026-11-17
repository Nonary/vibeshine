# Vibe Speakers on Windows

Vibe Speakers is an optional virtual USB Audio 2.0 playback endpoint for
Windows 11 x64. It supports 48 kHz stereo, 5.1 back/side, 7.1 and native 7.1.4.
PCM24 uses 24 valid bits in a 32-bit container; PCM16 is an explicit compatibility
choice. Float32 reaches the existing Opus encoder and encrypted RTP/FEC transport.
Opus is lossy. Native12 order is FL FR FC LFE BL BR SL SR TFL TFR TBL TBR.

## Using the installed product

1. Install or update Vibeshine normally.
2. In either Audio settings interface select **Vibe Speakers**. The regular
   installer has already prepared the account-owned controller in the background;
   no separate setup dialog or Bridge command is required.
3. Leave **24-bit** selected, or explicitly choose **16-bit** for compatibility.
4. Select **7.1.4 surround sound** in the compatible Moonlight client and stream.

No separate Bridge command, environment variable, device ID or runtime JSON is
required. The host launches and stops its bundled Bridge. Setup is not repeated
for every stream. Existing Default capture and other saved audio choices remain
available. This source uses Opus even if a previous Default capture configuration
selected another codec; that saved preference is not overwritten.

The negotiated profile is prepared before an application is launched, and the
audio capture reuses that same owned session. A game already running before a
profile change may need restarting because some games inspect their output
format only at startup. All requested channels/format and endpoint readiness
are checked; native12 never silently becomes stereo or 7.1.

The client automatically selects its static Windows Spatial renderer for 7.1.4.
A compatible configured Spatial provider and suitable active HDMI/DisplayPort
output are external prerequisites. The source must actually produce height
content; selecting twelve channels cannot create it. See
[the client contribution](https://github.com/Nonary/moonlight-qt/pull/40) and
[Common negotiation](https://github.com/Nonary/moonlight-common-c/pull/4).

## Ownership, service and removal

The installer captures the Windows user's SID before UAC, including when another
administrator supplies approval. Machine-protected state lives in
`%ProgramData%/VibeSpeakers/users/<SID>/setup.json`, with an installation root,
controller instance, nonce marker and protected ACL. Unknown or foreign devices
are never adopted. Historical private controllers/journals remain untouched;
the integrated product creates its own controller and reuses compatible signed
shared drivers. Removing the product removes only its own validated controller.

The existing Vibeshine service runs its host in the interactive session under
SYSTEM. That launch policy is unchanged. Audio setup resolves the logged-on
account from that exact session, rather than treating SYSTEM as the user.
The Bridge retains its host's token and exact process/session lifetime binding.
An ordinary user launch stays unelevated; streaming never requests UAC. A
machine-wide controller lease rejects competing sessions/processes. IPC is local,
access-restricted, bounded and pinned to the actual process/image lifetime.

Render roles are restored at session end. User changes made during the session
are preserved rather than overwritten. Upgrade/repair keeps the protected setup
and pairings. Uninstall stops the owning host before targeted controller cleanup;
shared USB/IP drivers and unrelated devices are retained.

Only unmodified, pinned Microsoft-signed usbip-win2 0.9.8.1 and Windows
`usbaudio2.sys` are used. Setup validates signatures, versions, Code Integrity,
boot policy, ownership and paths. It does not alter Windows security settings.
HVCI/VBS is supported and tested, but is not an artificial prerequisite. An
incompatible existing shared driver is refused instead of replaced. Source
provenance/licenses are in `src/vibe-speakers/vibe-usb-audio/THIRD-PARTY.md`.

The normal setup executable performs the direct firmware and boot-loader observations under its
approved administrator token. Its short-lived, administrator-protected receipt
is checked against the current boot and Windows Secure Boot state inside the MSI;
the restricted installer-service token cannot perform those privileged queries itself.
Driver signatures, kernel Code Integrity and validation of the observed boot policy still run in the MSI.
Use the setup executable for installation and maintenance.

After a failed installer transaction, a previously running service is restarted
only if its registered command and both restored service/host image hashes still
match the pre-installation checkpoint. Previously stopped services stay stopped.

## Packaging and signing

The normal Windows CMake build includes the optional component; CPack/MSI and
the existing bootstrapper install the Bridge and configure its owned controller
inside the MSI transaction, with targeted rollback and commit actions.
The optional component is enabled by default on Windows x64. Builds that
explicitly exclude `SUNSHINE_ENABLE_VIBE_SPEAKERS` show its unavailability in the
UI instead of offering a working-looking source. All supported profiles use
USB identity **1209:7140**, with unique instance serial numbers.

Deep-signing includes `tools/VibeUsbBridge.exe`. Executable hashes are computed
from the protected final installed files after signing and held as file/process
pins for the session. A pre-signing hash is not persisted as an update barrier.
The shared signed driver/catalog/INF files are never modified by signing.

## Diagnostics and tests

Normal sessions record no PCM or per-block sample files. Optional control logs
are capped at 64 KiB/session, with at most 32 completed session summaries retained.
Optional diagnostic failure cannot stop audio; a missing/invalid required binding
record must stop startup. Detailed sample/timing collection and finite durations
are available only in explicit `VIBE_AUDIO_TEST_INSTRUMENTATION` builds; the
normal session has no four-hour or other wall-clock expiry.

`tests/host714` uses production negotiation, codec, packetizer, queue and setup
policy functions. `src/vibe-speakers` tests conversion, local pipes, lifetime,
overload and recovery without attaching a device. These do not substitute for
a real Windows source/end-to-end session or physical HDMI/soundbar acceptance.
