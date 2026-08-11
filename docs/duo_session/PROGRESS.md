# Duo Session Architecture Progress

This is the durable engineering ledger for implementing Duo-style multiseat in
Vibeshine with concurrent Windows sessions. It intentionally tracks experiments,
proof boundaries, known failures, and the next decision gates. It is not a claim
that the experimental components are production-ready.

## Project lane

- Branch: `duo_session`
- Worktree: `D:/sources/worktrees/duo_session`
- Base snapshot: `302ca497bb7c955f575e99830820c1e542107393`
- Base branch: local `unverified`
- Created: 2026-08-10
- Landing policy: keep this work isolated on `duo_session`; do not squash, merge,
  or cherry-pick it into `unverified` until the user explicitly approves that step.
- Product constraint: use concurrent Windows sessions, not VMs.
- Remote-driver lane: branch `duo_session`, worktree
  `D:/sources/worktrees/libvirtualdisplay-duo_session`, base
  `3e85c1fb0e155eebdd640ee4abfd4fdca25bf3ce` from local
  `libvirtualdisplay/master`.

## Target architecture

Each seat must own an independent Windows interactive session with its own DWM,
desktop, shell, focus/input context, display path, Sunshine process, ports,
credentials, and lifecycle. The privileged seat broker should create and retain
the session, activate its remote display, launch Sunshine with the correct user
token, and clean up only resources owned by that seat.

The normal console virtual-display path and the remote-session display path are
different products of the driver. A root-enumerated console IDD is not sufficient
for a non-console Windows session; the proof rig required a remote-session IDD
created through `SWD\\RemoteDisplayEnum` with
`IDDCX_ADAPTER_FLAGS_REMOTE_SESSION_DRIVER`.

### Windows account semantics

The production contract is now stricter than Duo's documented credential flow:

- use the already logged-in user's Windows identity for a seat;
- do not create or require a managed/fake local account;
- do not ask for, store, or replay the user's Windows password; and
- preserve the console logon while creating a distinct WTS session, desktop,
  DWM, shell, and Sunshine process.

The current `VibeSeatTest` session remains diagnostic evidence only. It is not
the intended product account model.

`DuplicateTokenEx` plus `SetTokenInformation(TokenSessionId)` is sufficient to
launch a process in an **existing** WTS session, but it does not allocate the
session or make Winlogon/DWM/shell own it. The provider's legacy
`GetUserCredentials` contract supplies username/domain/password, not a token.
The correct bootstrap must therefore join a token-transfer mechanism to session
allocation instead of merely changing Sunshine's process token.

Windows child sessions are the supported behavioral oracle. A child session is
a loopback RDP session tied to the current user's existing session and is logged
on without prompting for credentials. Windows documents a system-wide limit of
one active connected child session, so this path can prove the same-user/no-
password contract but cannot by itself provide arbitrary Duo-style seat count.

## Proven results

### Architecture and display ownership

- Duo v1.5.9 was inspected as a reference. It combines a TermService compatibility
  layer, a headless localhost RDP client, a custom protocol/provider and IDD path,
  and a modified Sunshine capture path. The terminal session supplies desktop
  isolation; Sunshine is the transport and encoder, not the isolation boundary.
- `libvirtualdisplay` already acquires each IddCx swapchain surface in
  `SwapChainProcessor`, but currently releases it without consuming the pixels.
  That is a viable future zero-copy export seam for the console VDD path.
- The existing root-enumerated Sunshine/libvirtualdisplay monitor did not appear
  in the non-console test session. A distinct remote-session driver/INF and
  creation path is required.
- A diagnostic remote IDD created `\\.\\DISPLAY1` inside Windows session 2 at
  1920x1080 and 120 Hz on the RTX 4090. DXGI duplication returned a real nonzero
  frame, proving that the independent session's DWM rendered to the remote IDD.
- A product-owned remote driver lane now has a distinct
  `SUNSHINE_REMOTE_IDDCX` INF, remote-session adapter flags, a separate protected
  control interface, session-specific device selection, console-HKCU isolation,
  and a controller-tracked bootstrap monitor. These changes are source-only and
  are not yet built, signed, installed, or runtime-proven.

### Independent Windows session

- Windows session 1 remained the active `Chase` console session.
- The custom listener/provider created a second active interactive session,
  `sunshine-idd#0`, for `VibeSeatTest` as session 2.
- Session 2 has its own `winlogon.exe`, `dwm.exe`, `explorer.exe`, shell processes,
  user token, and display topology. This is an independent Windows desktop, not
  another monitor attached to the console desktop.
- Windows 11 Pro client concurrency was enabled by TermWrap patching/wrapping
  TermService in memory. No Microsoft DLL was modified on disk, but production
  support and Windows Update resilience remain unresolved.
- `WTSIsChildSessionsEnabled` succeeds on this Windows 11 26100 host. Child
  sessions are currently disabled, so no child-session state was changed during
  the source investigation.
- The current `RegisterUsertokenForNoWinlogon` exports in both `wtsapi32.dll` and
  `usermgrcli.dll` are compatibility stubs that set `ERROR_NOT_SUPPORTED` and
  return failure. They are not a usable token-transfer API on this build.
- The internal `UMgrChangeSessionUserToken(HANDLE)` API is real on this build and
  routes to User Manager rather than returning the compatibility stub. It is a
  candidate mechanism behind same-user bootstrap, but calling it in a temporary
  RDS session has not yet been proven and must remain version-gated and fail
  closed if used.

### Sunshine and video transport

- A second Sunshine process runs in session 2 with isolated configuration,
  certificates, state, logs, and ports while the installed console Sunshine
  continues to run in session 1.
- Session 2 successfully launches its own `sunshine_wgc_capture.exe`; WGC captures
  the session-2 desktop rather than the console desktop.
- A paired Moonlight client completed RTSP launch, WGC capture, NVENC encode,
  network transport, hardware decode, and presentation at 1920x1080 60 FPS.
- The first video packet arrived in 200-300 ms in the recorded live and reconnect
  tests. Recorded client statistics reported zero network and jitter frame loss.
- Simultaneous console and session-2 streams were proven. During one six-second
  sample, the session-2 stream advanced 261 frames, 1,244 packets, and 1,751,552
  bytes while the console stream's frame counter advanced independently from
  21,222 to 22,604.
- Disconnect/reconnect was proven against the same paired seat. The resumed stream
  received a new session UUID, delivered its first packet in 300 ms, and reported
  zero loss.

### Input isolation

- Console and session-2 key probes ran simultaneously.
- A native `SendInput` event delivered through the session-2 Moonlight stream
  produced `session=2 key=X` only in the seat probe.
- The console probe did not receive `X`; its separate `D` events remained confined
  to session 1. This proves keyboard isolation for the tested path.
- Mouse, touch, pen, gamepad/ViGEm, secure desktop, and UAC isolation are not yet
  proven.

### HDR and 10-bit transport boundary

- The remote IDD advertises HDR support and accepts the Windows HDR preference.
- The session-2 status after enabling HDR reports `hdr_enabled=yes` and 10 bits per
  color channel.
- WGC changes to `DXGI_FORMAT_R16G16B16A16_FLOAT`.
- Sunshine selects a 10-bit HEVC NVENC session and Moonlight decodes an HEVC Main10
  bitstream with D3D11VA. The seat API reports `hdr=true` because the client asked
  for HDR.
- This does **not** prove correct HDR output. Windows simultaneously reports
  `advanced_color_active=no` and `active_color_mode=0`; DXGI reports
  `DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709`; Sunshine logs SDR color coding rather
  than PQ/Rec.2020. The current transport is Main10 carrying SDR-signaled color.

### Steam and application launch

- Launching Civilization VI's DX12 executable through an interactive session-2
  task stayed inside session 2 and started a second `steam.exe` there with
  `steam://run/289070//`.
- The console Steam instance remained in session 1. This proves that a second Steam
  process can be established in the other Windows session in the current rig.
- No Civilization VI process has appeared yet. The session-2 Steam UI/login/DRM
  state must be completed or inspected before treating game launch as proven.

### Source defect found during the spike

- The then-installed current Sunshine build crashed during RTSP launch because an
  expression moved `launch_session` and dereferenced it in arguments whose
  evaluation order was not safe.
- Commit `cfcd2d3aced3a21e1830da47a7b510c4f2136289` preserves the pending launch ID
  before transferring ownership. It is an ancestor of this branch's base.
- The live seat currently uses an extracted Vibeshine 1.18.1 host that predates
  that crash. The corrected current source has not yet been built and substituted
  into this diagnostic seat.

## Known failures and production gaps

1. **HDR semantics:** 10-bit capture and Main10 transport work, but the remote
   display never becomes active PQ/Rec.2020 advanced color. The `hdr=true` API field
   is only the requested client mode and is not proof of HDR signaling.
2. **Audio:** session 2 has no default audio endpoint. Sunshine reports
   `0x80070490` and sends no seat audio. A real per-session endpoint/transport is
   required before the architecture is equivalent to Duo.
3. **Display-helper globals:** the session-2 host encountered a cross-session
   display-helper instance it could not open (`winerr=5`). The stream works because
   the remote IDD was preactivated, but global helper ownership and recovery files
   are not safe for multiple seats.
4. **Machine-global side effects:** NVIDIA preference recovery, RTSS recovery,
   mDNS identity, update checks, driver/display helper coordination, and other
   singleton state need explicit per-seat ownership or a broker.
5. **Clean implementation boundary:** the diagnostic TermWrap/provider/remote IDD
   components prove Windows behavior but are references only. Their licensing and
   implementation cannot simply be copied into the product.
6. **Client SKU compatibility:** the concurrent-session path depends on in-memory
   TermService compatibility patching. Servicing fragility, security posture, and
   fail-closed behavior remain potential deal breakers.
7. **Application compatibility:** Steam login, Steam isolation, game DRM,
   anti-cheat, launchers, and applications that detect remote sessions need broad
   testing.
8. **Resource isolation:** controller visibility, GPU admission, NVENC session
   limits, audio, clipboard, device redirection, and per-seat network/QoS behavior
   remain incomplete.
9. **Lifecycle:** logoff, reconnect, host/provider/driver crashes, reboot,
   sleep/resume, GPU reset, TermService restart, and Windows Update are unproven.
10. **Same-user bootstrap:** ordinary custom protocol-provider authentication
    still requires credentials. Windows child sessions prove passwordless
    same-user login but allow only one active child session system-wide. The
    multi-seat token-transfer extension remains a runtime proof gate.

## Current live rig snapshot

Snapshot time: 2026-08-10 22:30 America/Chicago.

- Evidence root:
  `C:/ProgramData/VibeshineDiagnostics/duo-session-spike-20260810`
- Session-2 working directory:
  `C:/Users/VibeSeatTest/AppData/Local/Temp/duo-spike-session2-remote-idd`
- Provider log: `C:/ProgramData/Sunshine/RdpProtocolProvider/provider.log`
- Driver log: `C:/ProgramData/Sunshine/IddDriver/IddDriver.log`
- Original ignored findings:
  `D:/sources/sunshine/findings-duo-vdd-session-architecture.md`
- Session 2 is active as `VibeSeatTest`; stale disconnected session 3 also exists.
- Console Sunshine: PID 15504, session 1, normal ports 47984/47989/47990/48010.
- Seat Sunshine: PID 60516, session 2, loopback ports
  56995/57000/57001/57021.
- Seat WGC helper: PID 61060, session 2.
- Moonlight: PID 59588, session 1, connected to the seat in an HDR-requested
  windowed HEVC/Main10 stream.
- Seat Steam: PID 32292, session 2, launched for Civilization VI app ID 289070.
- The seat API snapshot showed state `running`, 6,606 frames, 22,181 packets,
  31,230,848 bytes, and zero client-reported losses.
- `SunshineService`, `SunshineVirtualDisplayBroker`, and `TermService` are running.
- Scheduled tasks retained for the experiment:
  `DuoSeat2Sunshine`, `DuoSeat2HdrEnable`, and `DuoSeat2Civ6` under
  `\\VibeshineDiagnostics\\`.
- Do not tear down this rig until the user finishes the visual inspection or asks
  for cleanup. The session-2 HDR preference can be restored with
  `disable-hdr-live.cmd` through the session-2 interactive task path.

## Next proof sequence

Work these in order unless new evidence changes the dependency chain.

1. **Prove passwordless same-user session creation.** Use the supported child-
   session transport as the oracle: create one disposable child session from the
   console user's context, confirm the account SID/logon identity and independent
   Winlogon/DWM/shell, and confirm no password or managed account is used. Then
   test the custom provider's temporary-session handoff with a duplicated primary
   token and `UMgrChangeSessionUserToken`; require the session to remain valid
   after the connection bootstrap and fail closed on any unsupported return.
   Do not alter the retained `VibeSeatTest` proof session for this experiment.
2. **Finish the visual application test.** Bring the Moonlight seat window forward,
   complete or inspect session-2 Steam startup, and launch a game. Use a known HDR
   test pattern/application as the HDR oracle if the selected game is ambiguous.
3. **Resolve the HDR state transition.** Trace why the IDD accepts `hdr_enabled`
   while `advanced_color_active` remains false. Inspect the remote IDD's
   `IDDCX_MONITOR_DESCRIPTION`, mode/color capabilities, DisplayConfig Update2
   payload, DWM state, and DXGI color-space publication. Success requires PQ/2020
   signaling and an HDR-preserving visual/capture test, not merely Main10.
4. **Test the corrected current Sunshine source.** After an explicit build request,
   build the canonical tree with tests enabled, refresh the installer as required,
   and replace the diagnostic 1.18.1 seat host. Reprove launch, reconnect, WGC,
   simultaneous streams, and teardown with the RTSP fix.
5. **Inventory same-profile concurrency.** Once passwordless same-user bootstrap
   works, inventory profile locks and singleton behavior in Steam, browsers,
   launchers, HKCU, AppData, and per-user services. Do not fall back to a fake seat
   account to avoid those product issues.
6. **Provide per-session audio.** Prototype the endpoint/transport, then prove the
   session-2 stream contains only session-2 audio and survives reconnect.
7. **Prove remaining input classes.** Mouse, relative mouse, gamepad/ViGEm,
   Bluetooth controllers, touch/pen, focus changes, UAC, and secure desktop must
   not leak across sessions.
8. **Complete and runtime-prove the libvirtualdisplay remote-session backend.**
   Build/sign/install the new remote package, make the clean provider return
   `SUNSHINE_REMOTE_IDDCX`, and prove its bootstrap monitor, session-scoped control,
   dynamic mode, swapchain, HDR Update2 behavior, and bounded teardown. Preserve
   the existing root/console backend separately.
9. **Implement a clean seat broker.** Own same-user session creation, listener/RDP
   lifetime, display activation, correct-token process launch, port/certificate
   allocation, resource admission, reconnect, and transactional cleanup.
10. **Remove global Sunshine assumptions.** Scope or broker display-helper state,
   recovery files, update checks, mDNS, integrations, input devices, and other
   singleton resources.
11. **Stress and compatibility test.** Run two and then more live seats at
    1080p120, 1440p120, and 4K HDR; measure GPU, VRAM, encoder, copy engine, CPU,
    frame pacing, audio, and network behavior. Include Steam, third-party launchers,
    DRM, and anti-cheat titles.
12. **Lifecycle and servicing matrix.** Exercise disconnect/logoff, crashes,
    provider/driver restart, TermService restart, sleep/resume, reboot, GPU driver
    update, and Windows cumulative update. Every unsupported state must fail closed
    without changing the console topology or another seat.
13. **Compatibility and licensing decision.** Select a supportable client-SKU
    strategy and document the clean-room boundary before productizing the session
    provider/display bridge.

## Release gates

Do not call this production-capable until all of the following are proven:

- two independent users can stream different desktops concurrently;
- each seat receives only its assigned video, audio, and input;
- HDR is active PQ/Rec.2020 end to end on a supported seat;
- reconnect and teardown cannot affect another seat or the console;
- current Vibeshine and the product-owned remote driver/provider pass the proof;
- unsupported Windows builds fail closed and preserve normal console access;
- the compatibility and licensing model is acceptable for distribution.

## Resume checklist

At the start of a new session:

1. Work only from `D:/sources/worktrees/duo_session` on branch `duo_session`.
2. Read this file and the original ignored findings report.
3. Check both the canonical `unverified` checkout and this worktree before editing.
4. Inventory `qwinsta`, seat/console Sunshine processes, scheduled tasks, listeners,
   driver/provider logs, HDR state, and the seat API before trusting old PIDs.
5. Treat all PIDs, session IDs, ports, and live states above as a timestamped
   snapshot; rediscover them rather than assuming they are still current.
6. Update this ledger after every proof, disproof, architectural decision, or
   destructive cleanup.
