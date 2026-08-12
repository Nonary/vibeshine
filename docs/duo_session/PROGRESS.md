# Duo Session Architecture Progress

This is the durable engineering ledger for implementing Duo-style multiseat in
Vibeshine with concurrent Windows sessions. It intentionally tracks experiments,
proof boundaries, known failures, and the next decision gates. It is not a claim
that the experimental components are production-ready.

## Project lane

- Branch: `duo_session_large`
- Worktree: `D:/sources/worktrees/duo_session_large`
- Mode: large/build-enabled feature integration lane
- Isolated build directory: `D:/sources/builds/duo_session_large`
- Base snapshot: `302ca497bb7c955f575e99830820c1e542107393`
- Base branch: local `unverified`
- Created: 2026-08-10
- Converted from the lightweight `duo_session` lane at commit
  `c7ff8322b703113315e8b3e0eed640844a0ce184` on 2026-08-11. The branch and
  worktree were renamed in place so the existing history remains the lane's
  history rather than being copied to a divergent branch.
- Landing policy: keep this work isolated on `duo_session_large`; do not squash,
  merge, or cherry-pick it into `unverified` until the user explicitly approves
  that step. New implementation tasks for this feature should branch from the
  current `duo_session_large` tip and land back into this lane first.
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

### Windows account and application-token semantics

The seat's Windows logon identity and the application's Windows identity do not
have to be the same. The revised product contract is:

- a disposable managed seat account may own the independent WTS session,
  Winlogon, DWM, shell, and remote display;
- games, launchers, and Sunshine may be launched with a duplicate of the already
  logged-in console user's token retargeted to the seat session;
- do not ask for, store, or replay the console user's Windows password; and
- preserve the console logon and its profile while creating a distinct WTS
  desktop and display path.

This means `VibeSeatTest` is a valid diagnostic model for session ownership. Its
credentials are still not a substitute for the console token used by
applications that need the console user's HKCU, profile, Steam state, or other
per-user resources.

`DuplicateTokenEx` plus `SetTokenInformation(TokenSessionId)` is sufficient to
launch a console-user process in an **existing** WTS session. It does not allocate
that session or make Winlogon/DWM/shell own it, but the managed seat account can
provide that bootstrap. The broker must prepare the seat's `WinSta0\Default` and
`BaseNamedObjects` ACLs for the duplicated console user and logon SID before
launching applications there.

Windows child sessions remain a useful supported behavioral oracle. A child session is
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
  and a controller-tracked bootstrap monitor.
- The product remote-driver lane was built with MSVC in Release configuration and
  `BUILD_TESTS=ON`; all 201 CTest tests passed. Inf2Cat passed, the package was
  locally signed, and `sunshineremotedisplaydriver.inf` version 1.3.0.1 was
  staged as `oem31.inf`.
- The custom provider selected `SUNSHINE_REMOTE_IDDCX` and loaded it for sessions
  4 and 6. Inside active session 6, the display probe enumerated twelve remote
  `Sunshine Remote Session Display Driver` paths with that product hardware ID.
  This closes the earlier product-driver build/install/runtime proof gate.

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
  routes to User Manager rather than returning the compatibility stub. Runtime
  calls against the console session, an established remote session, and the
  provider allocation window all returned `E_UNEXPECTED`; queries returned
  `ERROR_NOT_SUPPORTED`. It cannot replace TermService authentication on this
  build. The revised split-identity architecture does not require it.
- The official RDP ActiveX child-session connection proof timed out from both the
  console user and a newly logged-on disposable parent session without reaching
  connected state. This is a negative result for the tested host/build, not proof
  that Windows child sessions are universally unavailable. Child sessions were
  restored to disabled after the test.

### Console-token launch into the managed seat

- A primary token duplicated from the console `Chase` Explorer process was
  retargeted from session 1 to active `VibeSeatTest` session 6 and launched with
  `CreateProcessAsUser` on `WinSta0\Default`.
- The resulting process ran in session 6 as `AMBIDEX\Chase`, with SID ending
  `-1001`, `USERPROFILE=C:\Users\Chase`, and the console user's roaming profile.
- Before ACL preparation, GUI initialization failed with `STATUS_DLL_INIT_FAILED`.
  Granting the console user SID and console logon SID full access to session 6's
  window station and desktop made GUI probes run successfully.
- Both source and retargeted tokens report `TokenBnoIsolation=False`; the mutex
  failure was not a hidden token sandbox.
- The retargeted process initially received access denied for both `Local\` and
  `Global\` mutex creation. Session 6's `BaseNamedObjects` DACL granted access to
  the seat account/logon SID but not the console user. Adding the console user and
  console logon SID made both mutex probes pass. These changes are session-local
  and disappear when session 6 logs off.

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

- The earlier session-2 stream proved that the remote IDD advertises HDR support,
  Windows accepts the HDR preference, WGC can switch to
  `DXGI_FORMAT_R16G16B16A16_FLOAT`, and Sunshine/Moonlight can carry and decode a
  Main10 bitstream. It did **not** prove active HDR: Windows simultaneously reported
  `advanced_color_active=no`, `active_color_mode=0`, and an SDR DXGI color space.
- The product remote-driver path is now live in session 5. A permanent
  1920x1080@60 monitor arrives, `IddCxMonitorUpdateModes2` and
  `IddCxAdapterDisplayConfigUpdate2` return success, IddCx assigns a swapchain, and
  the driver acquires real 1920x1080 frames. This closes the earlier missing-
  swapchain ambiguity: the HDR failure is not caused by an absent terminal-session
  display or a driver that never sees frames.
- Both public Windows activation paths were exercised inside session 5:
  `DISPLAYCONFIG_SET_HDR_STATE` and legacy
  `DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE`. Both return without a native error.
  The resulting state remains `supported=1`, `hdr_supported=1`, `hdr_enabled=1`,
  `bits_per_color_channel=10`, but `active=0` and `active_color_mode=0`.
- The driver-side HDR request also succeeds. Its Update2 payload contains one HDR
  path, 1920x1080, an HDR monitor mode, 10-bit RGB support, and 80-nit SDR white.
  IddCx logs the accepted path as `flags=19`, `color space=3`, `color mode=6`, then
  returns `STATUS_SUCCESS`.
- The kernel response immediately converts that accepted path to SDR:
  `SET_TIMINGS ... color space=0, wire format=00000010`. The driver's subsequent
  `CommitModes2` callback reports the public SDR G22/P709 color space, and its first
  acquired frame is `DXGI_FORMAT_B8G8R8A8_UNORM`, surface color space 0
  (G22/P709), SDR white 80 nits, and no HDR10 metadata. A historical console-IDX
  trace on this same machine contains the contrasting working value
  `SET_TIMINGS ... color space=12`, so the SDR result is specific to the tested
  remote-session path rather than a universal limitation of the driver or GPU.
- A deterministic full-screen HDR10 producer was built and run in session 5. It
  successfully created a `DXGI_FORMAT_R10G10B10A2_UNORM` flip swapchain, received
  HDR10-present support, set `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`, attached
  1000-nit HDR10 metadata, and saved the exact pre-Present 10-bit test frame. Its
  five band samples are valid packed 10-bit values: black `(0,0,0)`, gray
  `(520,520,520)`, red `(668,0,0)`, green `(0,769,0)`, and white `(769,769,769)`.
- The same run saved the post-composition desktop as an 8-bit BGRA BMP. The bands
  are present, proving that the HDR application frame reached the terminal-session
  desktop, while the simultaneous IddCx trace proves the driver received only an
  SDR BGRA8 surface. The renderer used a SYSTEM token placed in session 5; token
  identity was not part of this display-transport proof.
- The current result is therefore decisive but negative: application HDR10
  submission works, the public Windows and driver configuration calls are
  accepted, and actual frames flow, but the RDS/kernel display-policy boundary
  keeps advanced color inactive and delivers SDR composition to the IDD. Main10
  transport by itself remains SDR-signaled and must not be labeled HDR.
- Evidence is under
  `C:/ProgramData/VibeshineDiagnostics/duo-session-spike-20260810`, principally
  `session5-current-session-hdr-legacy-20260812.txt`,
  `session5-hdr-app-20260812.log`, the paired source RAW/composed BMP captures,
  `session5-hdr-app-driver-20260812.etl/.csv`, and
  `session5-hdr-app-iddcx-20260812.etl/.txt`.

### Steam and application launch

- Launching Civilization VI's DX12 executable through an interactive session-2
  task stayed inside session 2 and started a second `steam.exe` there with
  `steam://run/289070//`.
- The console Steam instance remained in session 1. This proves that a second Steam
  process can be established in the other Windows session in the current rig.
- No Civilization VI process has appeared yet. The session-2 Steam UI/login/DRM
  state must be completed or inspected before treating game launch as proven.
- In session 6, Steam launched under the retargeted console token after the
  window-station, desktop, and named-object ACLs were prepared. Its complete CEF
  `steamwebhelper.exe` process tree started, proving that a single seat can reuse
  the console user's Steam identity and profile without signing the managed seat
  account into Steam.
- Starting a normal console Steam instance while the seat instance was running
  shut the seat Steam down and took ownership. This proves Steam's cross-session
  master IPC is still a singleton even though Windows `Local\` objects are
  session-scoped.
- Launching seat Steam with `-master_ipc_name_override vibeshine-seat-6` kept both
  `steam.exe` processes alive. The seat's webhelper still retried and exited:
  both clients used `C:\Users\Chase\AppData\Local\Steam\htmlcache`, and CEF
  refused the shared profile with lock error 32 to avoid corruption.
- Overriding `LOCALAPPDATA`, setting `VPROJECT=steammulti`, and passing a
  `-cachedir` argument to `steam.exe` did not change the webhelper cache path on
  the current Steam public-beta client. IPC naming alone is therefore
  insufficient for simultaneous same-profile Steam clients on this build.

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

1. **HDR kernel/session policy:** a terminal-session HDR10 app, both public HDR
   setters, and the driver's HDR Update2 request all succeed, but RDS/KMD returns
   SDR timings and IddCx supplies a BGRA8/G22-P709 surface without HDR metadata.
   The missing requirement is now below the application and public IDD control
   layers. The `hdr=true` API field and Main10 encode remain request/codec state,
   not proof of PQ/Rec.2020 transport.
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
7. **Application compatibility and file isolation:** console-token Steam works in
   one seat, but simultaneous clients require both a unique master IPC name and a
   per-seat CEF/profile path. A narrow Steam/webhelper child-launch rewrite or a
   sandbox/file-namespace layer must be proven without leaking hooks into games
   or tripping anti-cheat. Game DRM, launchers, and remote-session detection still
   need broad testing.
8. **Resource isolation:** controller visibility, GPU admission, NVENC session
   limits, audio, clipboard, device redirection, and per-seat network/QoS behavior
   remain incomplete.
9. **Lifecycle:** logoff, reconnect, host/provider/driver crashes, reboot,
   sleep/resume, GPU reset, TermService restart, and Windows Update are unproven.
10. **Token-launch broker:** ordinary custom protocol-provider authentication
    still requires managed seat credentials. `UMgrChangeSessionUserToken` is not
    usable on this build, but it is no longer a gate. Production now needs a
    privileged broker that securely obtains the console token, retargets child
    processes, prepares only the necessary per-session ACLs, and never exposes
    the token handle to the managed seat account.

## Current live rig snapshot

Snapshot time: 2026-08-12 11:08 America/Chicago.

- Evidence root:
  `C:/ProgramData/VibeshineDiagnostics/duo-session-spike-20260810`
- Provider log: `C:/ProgramData/Sunshine/RdpProtocolProvider/provider.log`
- Driver log: `C:/ProgramData/Sunshine/IddDriver/IddDriver.log`
- Original ignored findings:
  `D:/sources/sunshine/findings-duo-vdd-session-architecture.md`
- Session 1 remains the active `Chase` console. Session 5 is active as
  `VibeSeatTest` on listener `sunshine-idd#0` and owns the live product
  `SUNSHINE_REMOTE_IDDCX` remote display. Session 6 is disconnected.
- Active staged product driver: `oem33.inf`, version 1.3.0.2, local test
  signature. Older `oem31.inf` version 1.3.0.1 remains staged side by side.
- The final simultaneous Steam proof used console PID 73704 and seat PID 64936.
  Both proof-owned process trees were stopped after evidence capture; no Steam
  process remains in either session.
- The deterministic HDR proof application and its process tree exited normally.
  Its source/composed frame captures and ETW traces remain in the evidence root.
- Session 5 is intentionally retained for follow-up inspection. Session 6 remains
  disconnected. The proof-owned transient tasks and trace sessions were removed
  without tearing down the active seat.

## Next proof sequence

Work these in order unless new evidence changes the dependency chain.

1. **Resolve the RDS/kernel HDR policy gate.** Preserve the current renderer and
   trace as the baseline. Identify the internal remote-graphics capability,
   protocol-provider property, RdpIdd/KMD condition, or supported Windows path that
   changes `SET_TIMINGS color space=0` to the working HDR value. Compare against a
   known working terminal-session HDR implementation on the same Windows build if
   one can be established. Do not keep changing public IDD capability fields that
   the trace already proves IddCx accepts.
2. **Reprove HDR end to end after the gate moves.** The deterministic test must
   still submit R10/PQ/2020 with HDR10 metadata, while Windows reports advanced
   color active, KMD commits an HDR color space, the IddCx surface remains
   HDR-capable with metadata, and the encoded/decoded stream is PQ/Rec.2020. A
   bright-looking BMP or Main10 bitstream is insufficient.
3. **Choose and prove the Steam isolation boundary.** Keep the per-seat
   `-master_ipc_name_override`, then prototype the narrowest per-seat CEF/profile
   isolation. Prefer rewriting only Steam's `steamwebhelper.exe` child command to
   a seat-specific cache over a general-purpose sandbox or hooks inherited by
   games. Reprove two full Steam/webhelper trees, controller input, Steamworks IPC,
   updates, shutdown, and anti-cheat non-interference.
4. **Finish the visual game test.** With console Steam stopped, or after Steam
   isolation is green, launch Civilization VI app ID 289070 from `E:/SteamLibrary`
   inside the active seat under the retargeted console token. The deterministic
   renderer, not the game's appearance, remains the HDR oracle.
5. **Test the corrected current Sunshine source.** After an explicit build request,
   build the canonical tree with tests enabled, refresh the installer as required,
   and replace the diagnostic 1.18.1 seat host. Reprove launch, reconnect, WGC,
   simultaneous streams, and teardown with the RTSP fix.
6. **Inventory same-profile concurrency.** Extend the now-proven Steam collision
   audit to browsers, launchers, HKCU, AppData, and per-user services. Distinguish
   objects that merely need seat ACLs from names and files that require isolation.
7. **Provide per-session audio.** Prototype the endpoint/transport, then prove the
   session-2 stream contains only session-2 audio and survives reconnect.
8. **Prove remaining input classes.** Mouse, relative mouse, gamepad/ViGEm,
   Bluetooth controllers, touch/pen, focus changes, UAC, and secure desktop must
   not leak across sessions.
9. **Finish the libvirtualdisplay remote-session backend.** Build/sign/install and
   provider selection are now proven. Next prove dynamic mode, session-scoped
   control, swapchain/HDR Update2 behavior, and bounded teardown while preserving
   the existing root/console backend separately.
10. **Implement a clean seat broker.** Own managed-seat session creation,
   listener/RDP lifetime, display activation, console-token process launch,
   WinSta/Desktop/BaseNamedObjects ACL preparation, Steam isolation identity,
   port/certificate allocation, resource admission, reconnect, and transactional
   cleanup.
11. **Remove global Sunshine assumptions.** Scope or broker display-helper state,
   recovery files, update checks, mDNS, integrations, input devices, and other
   singleton resources.
12. **Stress and compatibility test.** Run two and then more live seats at
    1080p120, 1440p120, and 4K HDR; measure GPU, VRAM, encoder, copy engine, CPU,
    frame pacing, audio, and network behavior. Include Steam, third-party launchers,
    DRM, and anti-cheat titles.
13. **Lifecycle and servicing matrix.** Exercise disconnect/logoff, crashes,
    provider/driver restart, TermService restart, sleep/resume, reboot, GPU driver
    update, and Windows cumulative update. Every unsupported state must fail closed
    without changing the console topology or another seat.
14. **Compatibility and licensing decision.** Select a supportable client-SKU
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

1. Treat `D:/sources/worktrees/duo_session_large` on branch
   `duo_session_large` as the feature integration lane. New implementation tasks
   branch from its current tip and land back into it before any explicit movement
   to `unverified`.
2. Read this file and the original ignored findings report.
3. Check both the canonical `unverified` checkout and this worktree before editing.
4. Inventory `qwinsta`, seat/console Sunshine processes, scheduled tasks, listeners,
   driver/provider logs, HDR state, and the seat API before trusting old PIDs.
5. Treat all PIDs, session IDs, ports, and live states above as a timestamped
   snapshot; rediscover them rather than assuming they are still current.
6. Update this ledger after every proof, disproof, architectural decision, or
   destructive cleanup.
7. Build only in `D:/sources/builds/duo_session_large`, with `BUILD_TESTS=ON` and
   the provisioned shared WebRTC SDK. Reconfigure from the committed lane tip
   before compiling or packaging; never reuse the canonical Sunshine build tree.
