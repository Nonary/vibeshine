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
- HDR proof sublane: branch/worktree `duo_hdr_proof_large` at
  `D:/sources/worktrees/duo_hdr_proof_large`, with isolated build directory
  `D:/sources/builds/duo_hdr_proof_large`. Keep its commits out of `unverified`
  until this long-running lane is explicitly promoted.

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

### Steam offline isolation (opt-in, source-corrected; runtime gate incomplete)

The paired-client setting is default-off and is effective only when terminal
emulation is also enabled. Its value is carried through authenticated launch
material into the SYSTEM-owned private worker. Before resuming it, the SYSTEM
service resolves ProgramData through the OS, creates SYSTEM/Admin-only protected
roots, stages a bounded real-file mirror with no-follow/reparse rejection, and
publishes it atomically. Each service lifetime uses a fresh non-reusable epoch
in the protected path and filter ownership; stale generations are never reused
as an unfiltered clone. Source files are opened while impersonating the exact
console-user token, copied from held handles, and charged against the aggregate
byte budget from the handle's observed size. It then validates a persistent provider+sublayer schema
and installs exact V4/V6 ALE AppId block filters through the documented BFE
user-mode API. Filter and mirror admission failure aborts the launch; the
console-user worker cannot add or remove WFP objects.

When enabled, the app launcher adds one bounded, idempotent
`-master_ipc_name_override vibeshine-seat-<safe-seat-token>` only when the
configured command directly names `steam.exe`. It does not modify game commands,
does not use undocumented `-offline`, and does not change live filtering rules.
The user must prepare the Steam client and eligible games in Steam Offline Mode.
Windows profile/cache isolation is not provided by this feature: same-account
sessions can still share HKCU/AppData and file locks, and online-required games or
third-party launchers may fail. The mirror filters every discovered client EXE
while leaving `steamservice.exe` outside the mirror and unfiltered. `steamapps`,
game libraries, profile collision directories, and volatile global state remain
outside the client mirror. The packaged ordinary user-mode webhelper proxy
rewrites both CEF cache and user-data arguments to a seat-private root, launches
each copied real helper by its exact nested sibling path (including Steam's
`bin/cef/cef.win7x64` layout), waits for it, and propagates its exit. No
SYS/INF/CAT/callout, special certificate, TESTSIGNING, DLL patch, injection,
IFEO, or AppContainer is part of this implementation. This is product
offline-like behavior, not a cryptographic sandbox. Because the
console and seat intentionally share one Windows SID/profile, an intentionally
hostile same-console-user process can copy/rename Steam or launch the original
console path outside the mirror; standard path-based AppId WFP has no distinct
principal and cannot close that race. The worker monitor detects Steam client
images only when their canonical path exactly matches a recorded original
Steam-client executable from the trusted source manifest; an unrelated game
named `steam.exe` is not sufficient. It poisons/terminates the seat job, and its PID
manifest is normalized, deduplicated, and bounded once during admission; each
queried image is normalized once and checked through a sorted exact-path lookup.
Its PID enumeration retries successful partial JobObjectBasicProcessIdList results,
treating an incomplete list as live/unknown, but there is
no pre-network zero-window guarantee and administrator/higher-priority policy can
override a standard user-mode filter. Filters remain persistent and quarantined
when termination cannot be proven. Stale filter cleanup is allowed only after
all protected epoch, seat, and generation ancestors are pinned and the
generation root is proven absent; present and unknown roots retain filters.
Repeated stop attempts remain cleanup-pending while the service-lifetime
quarantine completion token is unresolved. Source correction was rejected by Daybreak's first pass for
filesystem, WFP lifetime, recursion, cache ACL, proxy argv, monitoring, and
ownership gaps; this final correction also closes successful partial PID-list
handling, source-root impersonation, held-handle accounting, teardown
monitoring, exact count/page bounds, transaction-local keys, and epoch
ownership. These corrections are now represented here.
No build, test execution, install, Steam launch, or live WFP mutation has been
performed. Remaining runtime gates are copied Steam launch, proxy/cache
separation, console-vs-clone path filtering, game-online behavior, BFE restart,
Steam updater/self-relaunch escape handling, and validation of the proxy against
Steam's actual CEF argument behavior.

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
- The private-RDS capability hypothesis was narrowed rather than guessed. The
  installed client/server pair accepts one private, zero-payload capability set
  with version `0x000C0000`, but disassembly proves its selected pointer is stored
  separately from the graphics-channel HDR boolean. The latter is initialized
  false and has no writer on this build, so sending that capability as an HDR
  fix would be protocol-valid but causally unsupported.
- The alternative atomic-first-arrival experiment also produced a clean negative.
  A proof-only remote-driver build published HDR intent before
  `IddCxMonitorArrival`; `IddCxAdapterDisplayConfigUpdate2` rejected the
  created-but-not-arrived monitor with `0xC000000D` (`STATUS_INVALID_PARAMETER`),
  and no monitor arrived. This locates the failure at the public API ordering
  boundary before any later RDS color-policy decision.
- A clean-room inspection of Duo v1.5.9 found no on-disk Windows binary patch in
  its HDR setup. On every successful service start Duo instead removes any
  existing priority 16..0 overrides for feature IDs `54238000` and `54538524`,
  then creates priority-8 overrides with `EnabledState=1` and
  `EnabledStateOptions=0`. The Windows SDK defines state 1 as disabled; Duo's
  own log describes this registry feature configuration as reconfiguring
  `dxgkrnl.sys`, but it does not write or replace that file.
- The exact two priority-8 feature overrides were reproduced locally with a
  targeted rollback script and followed by a full reboot. No competing priority
  contained either feature ID. The keys survived reboot, but a fresh
  `GetFeatureEnabledState` process still returned GatePerf enabled (`2`) and the
  second feature default (`0`) for all change times on Windows 26200.8875.
- The post-reboot session-2 display result is also negative and decisive. The
  normal DisplayConfig HDR setter reports `supported=1`, `hdr_supported=1`,
  `hdr_enabled=1`, `bits_per_color_channel=10`, and no policy limit, yet Windows
  keeps `active=0` and `active_color_mode=0` after both the v2 setter and legacy
  fallback. The two Duo feature overrides alone therefore do not release the
  native HDR gate on this build/provider path.
- Duo's public issue history supplies the name that was missing from the static
  v1.5.9 analysis: feature `54538524` was Duo's workaround for
  `HdrRequireSourcePixelFormat`. Duo's author reports that this feature changes
  `BmlPickColorSpaceAndWireFormat` so a remote IDD needs a qualifying source
  pixel format before Windows will select the HDR color space. The same issue
  reports that GatePerf and then this second requirement caused successful mode
  updates to remain SDR on newer Windows builds. Public issue 541 subsequently
  reproduces our exact state on Duo 1.5.9 and RdpIdd 10.0.26100.8737: HDR is
  supported, the driver logs an HDR change, but active color mode remains SDR.
- Matching public symbols for the currently installed dxgkrnl 10.0.26100.9168
  expose a newer feature named `Feature_Fp16ForRemoteSrcModes`, whose descriptor
  embeds feature ID `62841958` (`0x03BEE466`). Static xref analysis finds exactly
  one call to its state wrapper. At that call, enabled state selects source-mode
  pixel format `0x71`; disabled state selects `0x15`. The matching Windows SDK
  defines those values as `D3DDDIFMT_A16B16G16R16F` and
  `D3DDDIFMT_A8R8G8B8`, respectively. This is a direct current-build mechanism
  for satisfying the source-pixel-format requirement without patching a Windows
  binary.
- A controlled proof for feature `62841958` was staged at priority 8 with
  `EnabledState=2` and `EnabledStateOptions=0` on 2026-08-13. Before reboot, a
  fresh Feature Staging API process still reports default state at all four
  change times, confirming the running kernel has not changed. Scheduled task
  `\\VibeshineDiagnostics\\DuoFp16RemoteRollback` runs as SYSTEM five minutes
  after the next startup and deletes only the `62841958` override; the current
  boot keeps its cached state for the bounded HDR test, while the following boot
  returns to the pre-test feature configuration. The listener remained at safe
  baseline `0/1/1`, only the console user session was active, and all RDS
  services remained running when staging completed. No reboot has yet been
  issued for this proof.
- Historical correction: that reboot was subsequently completed. On the clean
  2026-08-14 boot, a fresh Feature Staging API process reports GatePerf
  `54238000=DISABLED`, HdrRequireSourcePixelFormat `54538524=DISABLED`, and
  Fp16ForRemoteSrcModes `62841958=ENABLED` at READ, MODULE_RELOAD, SESSION, and
  REBOOT change times. Only the two Duo-equivalent priority-8 disable overrides
  remain; the temporary `62841958` override and its one-shot rollback tasks are
  absent. The Fp16 feature is enabled by the current Windows configuration
  without a remaining override. The normal console login succeeded, so no
  additional reboot is needed for the next HDR experiment.
- The reboot did not disable or bypass platform security. Secure Boot remained
  enabled, `VirtualizationBasedSecurityStatus=2`, HVCI remained enabled and
  running, Code Integrity policy enforcement remained active, and the Code
  Integrity operational log contained no warning/error/critical events from the
  new boot. This is measured compatibility for the current boot, not a support
  guarantee for private feature overrides on future Windows builds.
- A historical private-inbox-driver proof found the real RdpIdd-handle seam but
  initially interpreted its control contract incorrectly. The custom provider
  received the RdpIdd handle in `ProtocolConnection::OnDriverLoad`; an early
  helper then issued unnecessary `BindDriver`/`SetPropertyBag` transactions on
  `0x80000040` before a malformed variable-length monitor request on
  `0x80000044`. The last call did not return. That established only that the wait
  was inside synchronous `DeviceIoControl`, not that the preliminary calls or
  packet layout were correct. The corrected contract is recorded below.
- That request was incorrectly issued on TermService's `OnDriverLoad` callback
  thread. One minute later Winlogon event 6005 reported that subscriber
  `<TermSrv>` was taking too long to handle the Logon notification. Repeated
  forced boots entered Startup Repair, and Safe Mode was required to recover the
  console. This was a service-thread liveness failure, not a Windows code-
  integrity failure: no Microsoft binary was changed, and after recovery Secure
  Boot is still enabled, VBS reports status 2 with security services 2 and 7
  running, and Code Integrity policy enforcement reports 2.
- Recovery left `Sunshine-Idd` fail-closed with `fEnableWinStation=0`,
  `WinStationDisabled=1`, and `fLogonDisabled=1`. The provider COM registration
  was restored to the known-safe
  `C:/ProgramData/Sunshine/RdpProtocolProvider/SunshineRdpProvider.dll`, SHA-256
  `C39B80B06E817E585E1ED29D0594402D5341CB87604967C7349BA7C97875511C`.
  Normal `RDP-Tcp` remains listening and the console recovered as session 1.
- The native route remains active, but the inline IOCTL implementation has been
  removed entirely; the TermService-loaded provider contains no
  `DeviceIoControl` call. It instead duplicates only the
  supplied RdpIdd handle into a disposable helper process, places that helper in
  a kill-on-close job, returns from `OnDriverLoad` immediately, and gives its
  watchdog ten seconds to terminate only the helper. The listener is also
  one-shot after the first credential-backed connection and cannot automatically
  retry after failure or closure.
- Historical offline validation was green only for the then-current 216-byte
  packet's internal size/TLV consistency; later Microsoft-code analysis proved
  that packet was not the correct existing-monitor update contract. A benign
  helper then waited forever without opening RdpIdd; the production
  watchdog terminated it at 10 seconds with exit code 1460, its watchdog thread
  completed, and no helper process remained. Proof-only artifact hashes are:
  provider `0826160356817F812D076DBED614881E1B048CBCC8FC4ECEFFBF31D063ABF3DF`,
  helper `2A1713294D16A60D1377A77E1D48CCD2F9DF8FA9C6326C0A4304FEDC8A78FB0C`,
  and checker `5B996AD0DC721EB122B5EFA7349726F5A5688A501B8513ECF100B275F3881A5A`.
  At that checkpoint the binaries were built only; the later bounded live runs
  described below staged and activated them.
- The isolated live safety boundary is now proven on the real machine. The
  TermService-loaded provider contains no `DeviceIoControl`; only a disposable
  child receives the duplicated RdpIdd handle. A kill-on-close job and ten-second
  watchdog repeatedly terminated a deliberately stuck child with exit code 1460
  while TermService, Winlogon, and the console remained responsive. A separate
  SYSTEM deadline also completed rollback successfully and removed only the
  proof-owned listener, CLSID, credential, account, runtime arm, and helper.
- Listener discovery requires a persistent WTS listener present while TermService
  builds its table at boot. The listener itself therefore cannot be volatile,
  but it stays disabled and inert until a separate `REG_OPTION_VOLATILE` runtime
  key arms the proof for the current boot. The proof directory and runtime key
  must grant NetworkService read/execute and read access respectively; missing
  those ACLs explained the first inert boot without compromising the normal
  login path.
- The disposable `VibeHdrProof` account must be a member of the built-in Remote
  Desktop Users group. Before that membership Winlogon rejected remote logon with
  error 1385. After it, Security event 4624 recorded a successful type-10 logon,
  Winlogon reported authentication result 0, and the terminal session reached
  csrss, DWM, LogonUI, ctfmon, and GPU-process startup.
- Three authenticated historical runs reached `OnDriverLoad`. Each malformed
  216-byte monitor call exceeded the ten-second deadline and the disposable
  helper was killed without blocking TermService. Those runs never proved a
  monitor update or active HDR state.
- Later read-only analysis with matching Microsoft public PDBs corrected the
  control contract completely. `RdpIdd!CRdpIdAdapter::ProcessIoctl` opcode `10`
  is `SET_MONITOR_CONFIGURATION` on IOCTL `0x80000044`. Microsoft's own
  `rdpcorets!CDriverV4::SendMonitorLayoutChange` sends that IOCTL synchronously
  with a null `OVERLAPPED`, so the synchronous API itself is not a guessed bug.
  No proof-only bind or property-bag call is required. The current helper sends
  one 184-byte action-`2` update for the existing all-zero one-monitor ID. Its
  type-2 display mode is the observed Microsoft 1280x720, 32/1, divider-1 mode;
  type 5 is HDR10 with BT.2020 primaries, ST.2084, 8/10-bpc masks and full-range
  flags; type 6 carries 80-nit SDR white. The exact TLV parser and Update2 path
  are retained in static artifacts 71 through 80 under
  `.codex/delegation-artifacts/20260814-displaybroker-static`.
- The first corrected live update reached `DeviceIoControl(0x80000044, opcode
  10, input 184, output 20)` at 21:33:37 and did not return before the watchdog
  killed only the helper at 21:33:44 with exit code 1460. The proof connection
  was still in session arbitration, so no HDR result can be inferred. Rollback
  then removed the proof listener, CLSID, credential, account, volatile arm,
  helper, and deadline task while preserving the console and known-safe driver
  and provider. A later activation did not exercise the update because the old
  provider's process-global `g_rdpidd_helper_launched` flag never reset after
  watchdog completion.
- The helper gate is now an active-helper lock. All pre-watchdog launch failures
  release it, and the watchdog releases it only after the child is confirmed
  stopped. One loaded provider DLL passed two consecutive ten-second containment
  tests; both helpers exited 1460 and the gate released between attempts.
- The failed same-boot TermService restart was traced rather than forced. System
  event 7046 and repeated event 7011 timeouts showed that `UmRdpService` was not
  responding to service controls. Matching `umrdp.pdb` live stacks proved its
  serialized control handler was blocked in
  `RDCameraServiceExtension::OnRemoteDisconnect -> RDCameraDeviceManager::Terminate
  -> CServerVCChannel::Terminate -> RtlEnterCriticalSection`, while its camera
  APC thread was blocked in `CServerVCChannel::IssueARead -> ReadFileEx ->
  NtReadFile`. Signed Sysinternals Handle v5.0 tied both waits to the fourth
  proof-owned pipe, `SunshineRdsVirtualChannel-30152-4`: TermService handle
  `0x920` and UmRdpService handle `0x27D4`.
- The causal provider bug is exact. `CreateVirtualChannelPipe` opened the client
  handle returned to RDS with creation flags `0`. UmRdp's camera implementation
  calls `ReadFileEx`, which requires `FILE_FLAG_OVERLAPPED`; because the supplied
  handle was synchronous, `NtReadFile` blocked while the camera channel lock was
  held. Closing the proof-owned server handle and cancelling its TermService
  worker produced provider error 995, but the malformed client handle cannot be
  converted or safely cancelled after creation. The shared PID 2328 hosts 17
  services, so it was deliberately not terminated.
- The provider correction opens every RDS-facing client endpoint with
  `FILE_FLAG_OVERLAPPED` and immediately queries native `FileModeInformation`;
  it rejects the channel if either synchronous-I/O bit is present. Its reader is
  now a joinable thread for the offline lifetime test. The exact final DLL passed
  the virtual-channel mode/peer-close self-test, helper packet self-test,
  isolated COM identity test, and two-cycle watchdog test. Final SHA-256 values
  are provider
  `66784352D8DB879A52195B1A4155D9263E30E3C6A678A83D9D24F2F306458297`,
  checker `2EF691E30BF8873BCCE3B01A6888F8D9979EEB0F22EA173532D71722758751C0`,
  helper `D137235875869DC5B5F96952DB4388218F23A47B6E209EF44F341D01DE2AB4E4`,
  controller `0B41B8108BD130F79206626A6CA1C873EF45B2FE20DC63FA0A6ED3D2D7578CDD`,
  and constrained recovery tool
  `25274C274BA30B4EEE7C4B733F38E73F3646B6660DE45F5B9151E050A93849AF`.
- Current recovery boundary: proof-owned registry/account/credential/listener/task
  state is fully rolled back and the console remains active, but the already-
  malformed UmRdp camera handle keeps service-control teardown wedged for this
  boot. One normal reboot is required to clear that transient handle and load
  the corrected provider. Do not force-kill PID 2328 and do not reboot
  automatically; after the user reboots, reassert the complete safe baseline
  before staging another bounded live capture.
- `proof_guard.ps1 -Mode ActivateLive` no longer enables the proof before a
  TermService restart. It now stops only responsive UmRdpService, restarts
  TermService while the proof listener is disabled/unarmed, restores
  UmRdpService in a `finally` block, and enables the proof only after both are
  running. This removes the observed connect-during-restart race and leaves the
  proof inert if service preparation fails.
- The authenticated synthetic connection still closed after about 31 seconds
  with reason 12 and no Explorer/user shell. Returning `1` instead of `0` for
  `DISABLE_LOGON_DIALOG_TIMEOUT` was observed exactly but did not change that
  timing, disproving it as the governing teardown control.
- The provider's `GetLastInputTime` implementation was wrong. Microsoft defines
  its output as elapsed milliseconds since the protocol last received input,
  but the probe returned absolute `GetTickCount64()` uptime. Static Duo v1.5.9
  disassembly at RVA `0x9100` returns a small value from 1 through 16 instead,
  which continually reports recent activity. The probe now returns elapsed time
  since connection creation until a real input transport can update the boundary.
  This causally explains why Windows could apply idle policy on its approximately
  30-second poll, but it still requires a future bounded live confirmation.
- A final same-boot activation exposed a separate TermService-restart race:
  Windows instantiated two proof-listener objects, stopped the first, consumed
  the one-use credential through the second, assigned session 2, then closed the
  connection about 57 ms later before `OnDriverLoad`. LocalSessionManager logged
  failure `0x80004005` while transitioning from `CsrConnected` on
  `EvCsrInitialized`, while the Application log recorded Winlogon event 4005,
  `The Windows logon process has unexpectedly terminated.` The listener
  duplication is restart noise; it is not proven to have killed Winlogon. This
  is a session-lifecycle failure, not an HDR result, and it makes further blind
  reboot-and-activate trials unjustified.
- At 2026-08-12 22:02 America/Chicago the proof was rolled back again. Session 1
  is the active `Chase` console; TermService, SessionEnv, and UmRdpService are
  running; the proof listener, isolated CLSID, volatile runtime arm, one-use
  credential, disposable account, rollback task, and helper process are absent.
  No Microsoft file was patched or replaced, and the experiments did not disable
  Core Isolation, HVCI, VBS, Secure Boot, or Code Integrity enforcement.
- Current-build disassembly closes the hidden error-reporting boundary. In
  `dxgkrnl.sys` 10.0.26100.9168,
  `DxgkIddHandleSetDisplayConfig` calls
  `MonitorIsMonitorAndLinkHDRCapable` for an HDR10 path. A rejection returns
  `0xC00000BB` as the D3DKMT escape status and writes an eight-bit mask whose
  exact IddCx labels are: bit 0 high-bpp scanout, bit 1 target high-color-space,
  bit 2 target wide-color-space, bit 3 ST.2084 in the descriptor, bit 4 BT.2020
  in the descriptor, bit 5 descriptor high-bpp, bit 6 OS WCG support, and bit 7
  3x4-matrix support. The HDR-capability function's internal reasons map onto
  the corresponding HDR subset of that public-facing text; their private enum
  names remain unavailable.
- Exact disassembly corrects the initial interpretation of the final envelope
  field. IddCx 10.0.26100.4202
  `IddAdapter::SendUserModeMessage` sends a public
  `D3DKMT_ESCAPE_IDD_REQUEST` (`Type=30`) containing a private 0x30-byte
  envelope: adapter handle, escape code, input pointer/size, output pointer/size,
  and an output-byte-count field at offset `0x28`. Its optional final `UINT *`
  receives that field after `D3DKMTEscape`; it is not a second command status.
  `ProcessDisplayConfigUpdate` passes no output-byte-count pointer and examines
  the eight-byte support result only when the D3DKMT escape status is negative.
  The escape status itself is the kernel acceptance or rejection result.
- The saved IddCx ETL cannot recover the result payload. Its two send-complete
  events contain only the IddAdapter pointer and zero escape status; the
  eight-byte support result is not logged. The direct probe below was therefore
  required to distinguish an accepted capability check from a hidden rejection.
- A non-patching diagnostic route is now fully specified. A seat-local tool can
  open the active adapter with `D3DKMTOpenAdapterFromLuid`, reproduce IddCx's
  one-path `4 + 0x84` display-config request, call the documented
  `D3DKMT_ESCAPE_IDD_REQUEST` transport, and retain the escape status, returned
  output byte count, and support mask that IddCx does not expose. It changes no Windows file or
  executable memory and requires no reboot. Because command 2 reapplies a
  display configuration, it must run only inside a bounded proof seat after the
  monitor exists and under the existing automatic listener rollback.
- The offline probe source is now prepared in the isolated
  `libvirtualdisplay-duo_session` worktree. The proof-only
  `--probe-idd-hdr-gate <target_luid> <target_id>` command opens the exact active
  adapter, serializes the one-path request, supplies both the outer and private
  adapter handles, preserves the escape status and eight-byte result,
  and decodes every known support bit. Its command contract is registered as a
  seat-local, no-control-device operation and `virtualdisplay_probe` now links
  the documented D3DKMT entry points from `gdi32`. Static source review and
  `git diff --check` pass.
- The focused MSVC build and probe contract tests now pass. The executed probe
  SHA-256 was
  `D1C2C8C7CF5285FB6D7A2D1D986AE73110E125A99255804557659DB30A42CFCD`.
  The bounded session-2 capture at
  `idd-hdr-gate-20260814-142600` returned escape status `0x00000000`, eight
  output bytes, missing-support mask `0x00000000`, path index 0, and
  `idd_hdr_gate_missing=none`. Therefore the kernel HDR capability check passed:
  no high-bpp scanout, target color-space, descriptor PQ/BT.2020/high-bpp,
  OS-WCG, or matrix prerequisite is missing on this path.
- Passing the capability check did not make HDR active. Before and two seconds
  after the transaction, advanced-color state was identical:
  `supported=1`, `active=0`, `limited_by_policy=0`, `hdr_supported=1`,
  `hdr_enabled=1`, `bits_per_color_channel=8`, and `active_color_mode=0`.
  The one active display path also remained present. This isolates the failure
  after capability validation and before or during Windows' source-mode/color-
  mode functionalization; further EDID or target-capability changes are not
  justified by the evidence.
- The seat-local script's obsolete supplemental PowerShell D3DKMT mode dumper
  returned nonzero after all decisive files had been written, so the scheduled
  task reported exit 1. The wrapper still immediately restored the fail-closed
  listener, safe provider hash, and zero proof seats. That supplemental step has
  been removed; it does not affect the gate result.
- The live functionalization trace also proves the downstream ordering. The
  display-policy sample already contains source pixel format `0x15`
  (`A8R8G8B8`) before serialization; DisplayBroker and USER32 copy that value
  unchanged, and both BML input and output retain `0x15`. Although the terminal
  mode list includes `0x71` (`A16B16G16R16F`) and the current Fp16 feature is
  enabled, its only current-build selector chooses `0x71` after the sampled
  monitor color mode is non-SDR. Because the hidden capability mask is now
  proven clear, the circular color-mode/source-format ordering is the next
  boundary to trace.
- Duo's current public issue 541 reports the same failure on Duo 1.5.9 and
  RdpIdd 10.0.26100.8737: the driver requests HDR, dxdiag reports HDR-supported
  BT.2020/ST.2084 monitor capabilities, but the display color space and active
  color mode remain SDR. This independently confirms that Duo's two historical
  overrides are no longer sufficient on every current Windows installation; it
  does not identify the new private reason.
- The corrected private RdpIdd monitor transaction now returns successfully
  after the adapter's normal first layout. Complete one-monitor HDR replacement
  packets, including a replacement with a fresh monitor identity, produce
  `hdr_supported=1`, `hdr_enabled=1`, and 10 bits per color channel, but Windows
  still reports `advanced_color_active=0`, `active_color_mode=0`, and a NONGDI
  source mode. Static inspection of
  `CRdpIdAdapter::ProcessSetMonitorLayout` proves every accepted complete-layout
  request already performs `DeleteAllMonitors`, `AddMonitors`, and
  `UpdateAllMonitors`; target removal/rearrival is therefore not the missing
  transition.
- Public source-mode activation did not close the gap. DisplayCore acquisition
  was denied for the remote path, and a full `SetDisplayConfig` supplied-source
  NONGDI transaction returned Win32 error 31 under both SYSTEM and an authentic
  seat token. A successful FP16/scRGB flip-model full-screen presentation and a
  successful legacy exclusive FP16 presentation each ran for 300 frames without
  changing the same native active-SDR state. This rules out an ordinary game
  swapchain as the operation that pins the qualifying remote source mode.
- A guarded first-layout provider experiment reached the actual Windows callback
  sequence without changing a Windows file or disabling HVCI. TermService only
  consumes a newly written `LoadableProtocol_Object` mapping when it rebuilds
  its listener table; after a guarded service restart it instantiated the exact
  isolated `Sunshine-HdrProof` provider and created disposable sessions 13 and
  14. Windows used the newer device-path `OnDriverLoad` overload, not the raw
  handle overload originally targeted.
- Routing the complete HDR layout through that device-path callback opened the
  exact RdpIdd device and launched the job-contained helper before logon. The
  first opcode-10 monitor transaction then blocked until its 10-second watchdog
  killed only the helper. Giving the child a 100-ms head start kept the request
  pending before `OnDriverLoad` returned and produced the same result. RdpIdd
  therefore does not service monitor configuration before its own initial
  monitor state exists; queue racing cannot make a second IOCTL become the first
  layout.
- The matching stock binaries expose the remaining pre-layout gate precisely.
  `rdplite` initializes `CGraphicsChannel+0x178` to false and contains no writer;
  `CGrfxPipeline::Start` publishes it as
  `PKEY_Caps_Support_Hdr_Monitor_Config`, and `rdpcorets!StartPipeMgrLite` skips
  its immediate legacy initial-layout path only when that Boolean is true. The
  recognized zero-payload capability version `0x000C0000` writes the adjacent
  selected-cap pointer at `+0x170`, not this Boolean. No protocol packet tested
  or justified on the installed build can set the gate.
- WGC provides a separate, working HDR source seam before the remote IDD's SDR
  sink. A session-5 window capture and monitor capture both returned
  `DXGI_FORMAT_R16G16B16A16_FLOAT` and real extended-range scRGB values, including
  white `12.4844`, gray `1.25195`, and red `8.32812/-0.625/-0.0910645`. This is
  content evidence, not merely an FP16 allocation.
- A default-off, per-app `remote_session_hdr_bypass` prototype now authorizes HDR
  only when all seven conditions hold: explicit opt-in, client HDR request,
  neither SDR override, a non-console interactive session, WGC provenance, and
  FP16 capture. It synthesizes a fixed Rec.2020/PQ mastering profile with a
  1000-nit peak, 1000-nit MaxCLL, and 250-nit MaxFALL while preserving the normal
  display-HDR and TrueHDR paths.
- The first live attempt exposed an independent admission defect: the current
  host treated the session-scoped remote IDD as an unowned machine virtual
  display, invoked console VDD recovery, and returned HTTP 503 before capture.
  The proof branch now treats a non-console interactive session as the owner of
  its Windows-provided display lifecycle, skips machine-global VDD startup and
  temporary-probe creation there, and probes the existing session display.
- End-to-end session-9 proof is green. The final host logged all seven bypass
  gates true, `Color coding: HDR (Rec. 2020 + SMPTE 2084 PQ)`, 10-bit depth, and
  successful NVENC HDR metadata programming. Its session-9 WGC helper accepted
  `hdr: 1`, published the first frame, and continued through frame 100 with no
  format-mismatch reinitialization. Moonlight requested `hdrMode=1`, received a
  200 launch response, negotiated video format `0x200` (HEVC Main10), received
  its first packet after 200 ms, and decoded Main10 frames. The SDR control run
  kept the app opt-in but requested no HDR; the gate denied authorization,
  Sunshine encoded 8-bit SDR, and Moonlight negotiated `0x100` (HEVC Main).
- This proves the authorized WGC/scRGB-to-PQ encode/decode path without modifying
  a Windows DLL. It does not yet independently parse the emitted mastering SEI,
  verify the Moonlight output swapchain color space, or measure decoded pixel
  luminance/color against the source pattern; those remain the final quality and
  client-output proof boundaries.
- Evidence is under
  `C:/ProgramData/VibeshineDiagnostics/duo-session-spike-20260810`, principally
  `session5-current-session-hdr-legacy-20260812.txt`,
  `session5-hdr-app-20260812.log`, the paired source RAW/composed BMP captures,
  `session5-hdr-app-driver-20260812.etl/.csv`, and
  `session5-hdr-app-iddcx-20260812.etl/.txt`.

### Authentic-token and real-RDP source-pin proof (2026-08-15)

- Retargeting an authentic `VibeSeatTest` token into a synthetic session was not
  enough to make Windows accept the HDR transition. A SYSTEM child inherited the
  exact token handle, applied it as thread impersonation, and still received
  access denied from the public HDR setter and topology path while the active
  source mode stayed `0x15` (`A8R8G8B8`).
- A direct native user process initially exited with `0xC0000142`, before reaching
  the probe. Microsoft documents that `CreateProcessAsUser` does not grant the
  token access to the requested window station and desktop automatically. The
  proof broker therefore granted only the token's logon SID temporary access to
  that session's `winsta0/default`, launched the native child, and restored the
  exact original window-station and desktop DACLs after it exited. Both grants
  and both restorations returned success. The native process then reached the
  probe normally but still received access denied and retained source format
  `0x15`. This proves process identity and desktop ACLs are no longer the
  governing HDR failure in a synthetic seat.
- Event-log comparison identified the missing lifecycle distinction. Every
  synthetic proof seat emitted the initial session event and then teardown, but
  never `WTS_SESSION_DESKTOP_READY`. A locally connected FreeRDP client produced
  the full RDP lifecycle: arbitration, logon success, shell start, and desktop
  ready. A fresh guarded run reproduced that complete state in session 14, so
  the native HDR test no longer depended on the incomplete synthetic session.
- The real desktop-ready RDP session still reported
  `supported=1`, `hdr_supported=1`, `limited_by_policy=0`, but
  `active=0`, `active_color_mode=0`, 8 bits per color channel, and current source
  format `0x15`. `DisplayConfigSetDeviceInfo` v2 fell back with Win32 error 31;
  the legacy setter also returned 31. The full functionalization probe enumerated
  28 source modes, including four FP16 `0x71` modes, and applied the public
  topology successfully, but Windows retained `0x15` and inactive SDR. The
  seat-local private IDD escape was denied with `0xC0000022` in this identity;
  that denial does not supersede the earlier SYSTEM-context capability-check
  success.
- This disproves the hypothesis that a complete interactive RDP desktop or the
  correct user token is sufficient to make Windows pin the advertised FP16
  source mode. The live evidence is retained at
  `real-rdp-native-hdr-20260815-0358`. The attempted detached direct-primary
  launcher produced no new child trace and is not counted as evidence.
- Cleanup was explicit rather than deadline-driven. Session 14 was logged off,
  its exact FreeRDP PID was stopped, `VibeSeatTest` was disabled, and the
  TermWrap and listener-ACL guards were rolled back in that order. Independent
  verification found only inbox `termsrv.dll` loaded, listener permissions back
  to 512 with remote-user logon disabled, no proof/rollback task, normal IPv4
  and IPv6 RDP listeners, running TermService/UmRdpService/SunshineService, VBS
  status 2, and HVCI running. No reboot or Windows-file replacement occurred.

### Remote-source pinning and historical Duo boundary (2026-08-15)

- Duo v1.6.0 does not supply a Core-Isolation-compatible native-HDR path on
  this Windows generation. Static inspection proves that Duo first checks
  whether Memory Integrity is enabled. When it is enabled, Duo skips its HDR
  kernel patch. When it is disabled, Duo stages `RTCore64.sys`, bypasses driver
  signature enforcement, and changes `dxgkrnl!IsHdrSourceModePinned` so it
  returns true. This was not installed or executed here; it is outside the
  project's Secure Boot/HVCI boundary.
- A proof-only launcher that attempted to retarget a LocalSystem token into the
  seat was quarantined by Microsoft Defender as token-injection behavior. No
  exclusion or Defender bypass was attempted, the quarantined executable was
  removed, and the live rig was restored. Kernel disassembly also proves this
  experiment would not have changed the display decision: a LocalSystem user
  process still lacks the DWM process flag at `DXGPROCESS+0x198 bit 4`; that
  flag is assigned by the DWM/VMBus lifecycle, not by the token's account.
- A controlled real-session comparison cloned both an FP16
  `DXGI_FORMAT_R16G16B16A16_FLOAT` allocation and an SDR
  `DXGI_FORMAT_B8G8R8A8_UNORM` allocation as a D3DKMT primary. Allocation
  creation and exclusive source ownership succeeded in both cases, while
  `D3DKMTSetDisplayMode(PreserveVidPn=TRUE)` returned the identical
  `STATUS_GRAPHICS_PRESENT_MODE_CHANGED` (`0xC01E0005`) five times for each
  format. Native state remained source format `0x15` and active SDR. The
  rejection is therefore the cloned allocation's display-mode contract, not
  the requested pixel format. Evidence is under
  `primary-format-20260815-044307`.
- `D3DKMTGetSharedPrimaryHandle` returned success with a zero global handle in
  a fresh desktop-ready RDP session. Exact current-build disassembly explains
  why: `DxgkGetSharedPrimaryHandle` reads the CDD primary handle, then skips the
  normal fallback primary creation when `DXGPROCESS::IsRemoteConnection`
  returns true. The RDP source deliberately has no shared scan-out primary for
  a user-mode proof to clone. Evidence is under
  `shared-primary-20260815-044544`.
- An authentic user process inside the real RDP session can submit a public
  `SetDisplayConfig` topology with source pixel format `NONGDI` while HDR
  intent is off; Windows returns success and immediately normalizes the source
  back to `DISPLAYCONFIG_PIXELFORMAT_32BPP`. After the HDR setter records
  enabled intent, the same NONGDI topology returns error 31. A zero-delay
  NONGDI-then-HDR sequence also fails: the NONGDI call succeeds, the immediately
  following HDR setter returns 31, and all 20 quarter-second polls report HDR
  enabled intent but inactive SDR/source format 4. There is no usable public
  API ordering window. Evidence is under
  `interactive-nongdi-20260815-044914` and
  `nongdi-then-hdr-20260815-045151`.
- Duo's own historical packages close the apparent alternative from its early
  HDR announcement. Version 1.1.1's `RdpIddCapture.dll` did not contain HDR
  configuration hooks. Version 1.2.0 added in-process hooks for the Windows
  `IddCxImplAdapterDisplayConfigUpdate` and
  `IddCxImplAdapterDisplayConfigUpdate2` implementations, rewrote the monitor
  `ColorMode`, and logged that it had set HDR. Its disassembly builds the same
  documented Update2 transaction used by the product-owned Remote IDD: path
  flags `0x19` (mode, colorimetry, and SDR-white valid), HDR color mode, PQ /
  BT.2020 colorimetry, 10-bpc support, and SDR-white data. Thus Duo v1.2 did not
  reveal another driver contract; it patched the Windows IddCx library in
  memory. Newer Windows source-format checks broke that coercion, leading to
  Duo's two feature overrides and ultimately its v1.6.0 kernel predicate patch.
  The packages were downloaded and extracted only, never executed; evidence is
  under `duo-v111-static-20260815` and `duo-v120-static-20260815`.
- Full-binary current `dxgkrnl.sys` analysis finds six direct callers of
  `VIDPN_MGR::PinVidPnSourceMode`: path removal, modality pinning,
  `BmlPreparePathOrderAndVidPn`, `BmlFunctionalizePath`, BML path-mode-list
  construction, and the kernel
  `DXGK_VIDPNSOURCEMODESET_INTERFACE_V1_IMPL::PinMode` entry offered to a real
  display miniport. No user-mode DisplayConfig or IddCx DDI exposes this pin.
  The Remote IDD can request HDR with the documented Update2 API, but only the
  kernel BML pipeline or a full WDDM display miniport can pin the source mode
  that `IsHdrSourceModePinned` later validates. The caller evidence is retained
  in `dxgkrnl-pin-source-callers-current.txt`.
- MultiSeat issue 15 independently measured the same topology boundary on
  Windows 26200.9168: a SudoVDA IddCx monitor created by a seat appears only in
  the console topology, while the seat enumerates only its RdpIdd surface. The
  project owner confirmed this has never worked on the reference host, that its
  24H2 per-session-display statement was aspirational, and that no Remote IDD
  implementation has started. It provides corroboration, not a missing HDR
  technique.
- Every live experiment above was explicitly cleaned up. The machine remains on
  inbox `termsrv.dll`, with only the Chase console session active, the proof
  account disabled, proof credential and rollback tasks absent, and VBS/HVCI
  running. No reboot was used or requested.

### Native terminal-session HDR activation breakthrough (2026-08-15)

- The earlier conclusion that only BML or a full display miniport could create
  the required source-mode pin was incomplete. A documented user-mode
  `D3DKMTSetDisplayMode` transaction can ask the kernel to rebuild the VidPN and
  select a new primary, but only when the caller supplies a valid primary
  allocation, owns the source exclusively, and does **not** preserve the
  existing VidPN.
- The working transaction is:
  1. publish the Remote IDD target as HDR-capable and commit its link at 10 bpc;
  2. create an FP16 `DXGI_FORMAT_R16G16B16A16_FLOAT` shared-displayable
     allocation on the session render adapter;
  3. reopen the resource through D3DKMT, copy its runtime and driver-private
     allocation data into a new allocation marked `Primary=TRUE` for source 0;
  4. acquire `D3DKMT_VIDPNSOURCEOWNER_EXCLUSIVE` for source 0;
  5. call `D3DKMTSetDisplayMode` with the cloned primary and
     `PreserveVidPn=FALSE`;
  6. release source ownership and destroy both temporary allocations after the
     transition.
- The distinction between `PreserveVidPn=TRUE` and `FALSE` is causal, not
  cosmetic. With the same valid cloned FP16 allocation, exclusive ownership,
  resolution, and target state, the preserving call returned
  `STATUS_GRAPHICS_PRESENT_MODE_CHANGED` (`0xC01E0005`) five times and left HDR
  inactive. The non-preserving call returned `STATUS_SUCCESS` on its first
  attempt and immediately changed Windows from `advancedColorActive=0`,
  `activeColorMode=0` to `advancedColorActive=1`,
  `activeColorMode=DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR` (2).
- The driver independently observed the same transition in
  `EVT_IDD_CX_ADAPTER_COMMIT_MODES2`: its wire contract changed from
  `IDDCX_COLOR_SPACE_G22_P709` (1) to
  `IDDCX_COLOR_SPACE_G2084_P2020` (2, HDR10) while retaining four encoded RGB
  bits-per-component flags including 10 bpc. The public advanced-color query
  reported HDR supported, user-enabled, active, not policy-limited, and 10 bpc.
- Session 44 was the first native success. Evidence is under
  `rdpcore-proxy-cloned-fp16-rebuild-10bpc-20260815-101236`. The source-pin
  helper recorded a successful primary clone, exclusive ownership, and
  non-preserving `SetDisplayMode`; Windows reported HDR active both during the
  pin and after ownership and allocations were released.
- Session 45 repeated the result and proved it persisted for a bounded ten
  seconds after the temporary primary and ownership were gone. Evidence is
  under `rdpcore-proxy-cloned-fp16-hdr-persist-20260815-101526`.
- Session 46 then behaved like an HDR application after activation. It released
  the temporary primary, created an ordinary FP16/scRGB application swapchain,
  received scRGB color-space support, successfully called `SetColorSpace1`, and
  presented all 300 frames while Windows remained HDR-active. The state was
  still active ten seconds after the application and source-pin allocations had
  exited. Evidence is under
  `rdpcore-proxy-fp16-app-after-hdr-20260815-101809`.
- Session 47 isolated the breakthrough from the prior BGRA8-rejection
  diagnostic. A clean proof driver 1.3.0.26 was compiled with
  `SUNSHINE_REMOTE_DISPLAY_PROOF_REJECT_SDR_SWAPCHAIN=OFF` and
  `SUNSHINE_REMOTE_DISPLAY_PROOF_LOCAL_ADAPTER_FLAGS=OFF`. The same native HDR
  activation, HDR10/10-bpc CommitModes2, FP16/scRGB application presentation,
  and post-release persistence all succeeded. No `ProofSdrSwapChain*` event was
  present. Evidence is under
  `rdpcore-proxy-clean-driver-hdr-20260815-102110`.
- The negative controls explain why the successful sequence works:
  - DisplayCore saw the exact 10-bpc HDR-capable target but could not acquire it
    because the remote compositor owned it (`STATUS_ACCESS_DENIED`), under
    `rdpcore-proxy-displaycore-live-20260815-095117`.
  - Closing three BGRA8 IDD swapchains made Windows deliberately recreate BGRA8
    four times; an FP16 acquisition surface was never offered, under
    `rdpcore-proxy-reject-bgra8-20260815-095828`.
  - Removing `IDDCX_ADAPTER_FLAGS_REMOTE_SESSION_DRIVER` caused Windows to reject
    the adapter before initialization, under
    `rdpcore-proxy-local-adapter-20260815-100151`.
  - Claiming D3DKMT ownership before an FP16 DXGI exclusive swapchain did not
    work: creation returned `DXGI_STATUS_OCCLUDED` and fullscreen returned
    `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`, under
    `rdpcore-proxy-owned-dxgi-10bpc-20260815-101010`.
- This HDR activation path does not patch, replace, or hook a Windows DLL and
  does not disable code integrity. It uses normal user-mode D3D11/DXGI and
  D3DKMT calls. The concurrent client-SKU session wrapper used by the proof rig
  is a separate compatibility boundary and is not part of the HDR transaction.
- The remaining pixel-path caveat is now explicit. Windows reports active HDR
  and commits an HDR10/10-bpc wire contract, and an application can submit
  FP16/scRGB frames, but the Remote IDD still receives a BGRA8 format-87
  acquisition surface with surface color space 0. The next engineering task is
  to measure those acquired pixels and decide whether the driver/capture path
  must reinterpret or convert them before Main10/PQ encoding. This no longer
  blocks games from detecting and enabling Windows HDR.
- The cleaned regression package 1.3.0.27 retained only the three mechanisms
  needed by the success: initial HDR publication, the HDR10/10-bpc wire
  contract, and pre-arrival WCG/source priming. The failed head-mounted,
  local-adapter, desktop-topology, and BGRA8-swapchain-rejection experiments
  were removed. All 26 focused checks and the complete 206-test driver suite
  passed before the package was signed and installed as `oem31.inf`.
- Session 54 then confirmed the cleaned path in a real Moonlight stream. The
  exact 1.3.0.27 driver binding reported `advanced_color_active=1`, 10 bpc,
  active color mode 2, and HDR remaining active after the temporary source owner
  and cloned primary were released. The user independently confirmed the stream
  was working. Evidence is retained under
  `vibeseattest-probe-live-20260815-run7`.
- Cleanup after that confirmation returned the machine to its fail-closed
  baseline: only console session 1 remained; listeners 3397, 56000, and 56001
  were absent; the proof provider, listener, volatile arms, disposable account,
  credential, and helpers were absent; Secure Boot remained enabled; and VBS
  status 2 with security services 2 and 7 remained active. No reboot was needed.
- The exact source is now committed as provider commit
  `00d5d5c43356c09b744f686a877a565e30af1977`, libvirtualdisplay commit
  `c2a589ae40dc088d07477f5a5d605ba17cee6589`, and the Sunshine changes on this
  `duo_session_large` branch. Replayable external-repository patch series are
  retained under `docs/duo_session/source-patches` so the proof does not depend
  on the continued existence of side worktrees.

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

### Single-pair terminal-seat control plane (source implementation started 2026-08-15)

- Task branch `duo-seat-broker`, based on the current `duo_session_large` tip,
  introduces the product boundary between the public paired host and private
  seat workers. The integration lane remains `duo_session_large`; this work must
  land there and must not be moved to `unverified` yet.
- Each paired client now has a Windows-only **Terminal emulation** setting in the
  Web UI. The setting is stored beside that client's certificate and UUID in
  `sunshine_state.json`; a Moonlight-supplied `uniqueid` still cannot select or
  authorize a seat.
- Pairing, `/serverinfo`, `/applist`, `/launch`, `/resume`, and `/cancel` remain
  on the main Vibeshine HTTPS endpoint. A terminal-enabled configured-app launch
  is diverted before the main process applies runtime overrides, changes the
  console display, or starts the console app.
- The main process hands a broker request the TLS-derived client UUID, paired
  certificate, one-use launch ID, negotiated RTSP encryption material, app and
  stream metadata, and normalized app/client overrides. The eventual IPC must
  remain local, access-controlled, bounded, and non-persistent; stream secrets
  must never be written to the Web UI, command line, or disk.
- A successful broker response contains an already allocated private RTSP
  listener port and the owned seat/session identity. Moonlight receives that
  endpoint in the ordinary launch response from the already-paired host. The
  worker must not expose pairing, configuration HTTPS, or mDNS, so there is no
  second host identity and no second pairing step.
- Launch, resume, disconnect, unpair, shutdown, state projection, and Web UI
  connected-state hooks are explicit. The main host projects only the requesting
  client's seat state and does not expose its console-only Remote Input/Monitor
  controls to a terminal-enabled client.
- The route remains deliberately fail-closed. The original boundary-only slice
  returned a retryable 503 and performed no console launch until a privileged
  Windows runtime could publish a complete route.

### Managed-seat runtime implementation (source-only, 2026-08-15)

- Task branch/worktree `duo_seat_pool_large`, based on
  `ba4ef79d89a9594323558f314210f87fd01889da`, implements the next runtime layer.
  It has not yet been compiled, packaged, installed, or exercised live.
- The LocalSystem broker now discovers exact broker-owned `VibeshineSeatNN`
  accounts, assigns reusable disconnected seats round-robin, creates another
  account only when no reusable seat exists, hides it from the normal logon
  picker, and keeps it disabled at rest. Every RDP bootstrap rotates a fresh
  one-connect password, enables the account only for that transaction, then
  disables it immediately; no reusable managed-seat password is persisted.
  Ordinary stream lifecycle never logs off or deletes a healthy seat. Service
  startup disables any exact managed account left enabled by a prior crash, and
  uninstall invokes the same sweep before deleting those accounts.
- The provider requires the already-live TermWrap plus inbox `termsrv.dll`
  combination and the exact enabled `Sunshine-Idd` listener. It does not write a
  Windows DLL, enable a listener, restart TermService, or modify either
  prerequisite automatically. Missing prerequisites fail before account/session
  mutation.
- A hidden inbox RDP ActiveX controller runs with a privilege-disabled
  LocalSystem token on a random SYSTEM-only private window station and desktop.
  It receives the ephemeral managed credential only over a
  random first-instance, local-only protected pipe. The broker verifies the exact
  controller PID; the controller requires CredSSP, negotiated transport security,
  and successful server authentication for the machine FQDN. The broker waits for
  the exact managed account's Winlogon, DWM, and shell and keeps Remote Audio
  playback enabled while disabling device, clipboard, printer, smart-card, and
  microphone redirection.
- Applications do not run as the managed account. The broker duplicates the
  active console user's primary token, retargets it to the managed WTS session,
  grants that exact user and logon SID access to the target session's
  `WinSta0\\Default` and `BaseNamedObjects`, and launches a private Sunshine
  worker under that token. The managed account owns Winlogon/DWM/Remote IDD;
  Steam, launchers, and games retain the console user's profile and identity.
- The private worker has isolated state/log/credential paths below a
  process-boot-random `ProgramData\VibeshineTerminalSeats-&lt;128-bit random&gt;`
  root and one deterministic
  non-ephemeral port allocation per seat. Storage creation rejects reparse points,
  mismatched canonical paths, unsafe owners/DACLs, and creates protected
  SYSTEM/Administrators directories atomically before applying leaf-only user
  access through validated handles. It starts no Web UI, pairing, mDNS,
  updater, UPnP, or global display/VDD recovery path. The
  already-paired main host passes a bounded, one-use encrypted-RTSP launch over
  protected service IPC; the worker probes the session display/encoder, launches
  the configured app, and publishes only the exact ready media route.
  The broker creates both the worker process and primary thread atomically with
  a validated explicit LocalSystem-only DACL; the seat user has no external real
  worker handle to open or inject, and the worker uses its pseudo-handle for
  self-management.
- Ordinary Moonlight cancel and Web UI disconnect now park the worker and its app,
  stop the hidden RDP controller, and call `WTSDisconnectSession`; they preserve
  client/generation affinity. Resume reconnects the same account/session, proves
  the same worker route, reapplies fresh stream keys/settings, and does not launch
  the app twice. Disabling Terminal emulation, unpairing, or service shutdown is
  explicit destructive teardown.
- The main process can reconstruct a retained route after its own restart through
  an authenticated, peer-bound, one-use per-client state query. There is no
  unauthenticated seat enumeration or second Moonlight pairing path. An ordinary
  main-process restart forgets only its local projection; the service retains the
  worker and reconstructs the route on the next authenticated query. True global
  teardown remains service-owned.
- HDR-requested terminal launches now call a packaged, disposable seat-local
  activator before encoder probing and before the game. The helper waits for one
  exact HDR-supported, user-enabled, 10-bpc Remote IDD path, creates the proven
  FP16 shared-displayable allocation, clones its private contract as the source-0
  primary, acquires exclusive ownership, and invokes
  `D3DKMTSetDisplayMode(PreserveVidPn=FALSE)`. It then releases ownership and both
  temporary allocations and succeeds only if Windows still reports active HDR.
  The SYSTEM broker launches the helper itself inside the existing worker job and
  owns its 15-second watchdog, failing the launch closed on timeout or any
  incomplete state. The worker waits only for the broker response and then
  independently rechecks the exact target. The helper rejects the console
  session, proves the exact `VibeshineSeatNN` WTS owner, and accepts only the
  installed session-0 LocalSystem `sunshinesvc.exe` parent whose canonical
  binaries and containing directories are protected from the caller's token.
  The adapter LUID/source/target contract is
  transferred by the SYSTEM broker over a random first-instance local named
  pipe with remote clients rejected, rather than through mutable command-line
  values; no inherited handle is used. The broker verifies the exact helper
  PID before writing the one-shot capability. The worker's first exact sole-target
  attestation binds both source and target adapter LUIDs plus source/target IDs to
  the broker-owned worker lifetime; provider_resource_t intentionally does not
  pretend to know this display identity. Every later HDR request/reconnect must
  match that binding. The helper revalidates both adapter identities and exact
  target immediately before source ownership and every display-mode attempt, caps all
  driver-returned private-data allocations, validates the returned private-data
  slice, and the worker independently rechecks the same target after helper exit.
  Reconnect reapplies the transaction only when HDR is
  requested and no longer active. No Windows DLL is patched or replaced.
- Private-worker application launches remain inside the worker's kill-on-close
  job instead of requesting job breakaway, so broker teardown retains containment.
- Remaining production gates are explicit: package/provision the proven provider
  and an authenticated machine-name listener without repeating the unsafe login
  experiment; compile and live-validate the new D3DKMT activator against the
  signed 1.3.0.27 Remote IDD contract; prove the acquired BGRA8 pixel semantics
  and Remote Audio capture path; notify the broker on abrupt child-stream loss;
  remove stale randomized storage roots safely; prove complete ACL revocation (or
  adopt a unique restricted SID); measure first-seat launch latency; test
  remote/NAT port routing; and isolate any Steam/global IPC that remains
  cross-session. None of this source-only task has yet been compiled, packaged,
  installed, or exercised live.

### Source defect found during the spike

- The then-installed current Sunshine build crashed during RTSP launch because an
  expression moved `launch_session` and dereferenced it in arguments whose
  evaluation order was not safe.
- Commit `cfcd2d3aced3a21e1830da47a7b510c4f2136289` preserves the pending launch ID
  before transferring ownership. It is an ancestor of this branch's base.
- The HDR proof replaced that old diagnostic host with a current-source build in
  session 9. Launch, WGC capture, NVENC, Moonlight transport, and clean teardown
  all completed without the moved-session crash.

## Known failures and production gaps

1. **Native HDR state is proven; acquired-pixel semantics remain:** the
   non-preserving cloned-primary D3DKMT transaction now makes Windows keep the
   terminal-session target in active HDR and commit HDR10/10-bpc to the Remote
   IDD. An FP16/scRGB application also presents successfully after the temporary
   primary is removed. The IDD acquisition surface nevertheless remains BGRA8
   format 87 with surface color space 0. Capture exact acquired pixels from a
   deterministic HDR pattern, compare them against the app backbuffer and WGC
   oracle, then implement any required BGRA8-to-linear/PQ interpretation or
   conversion before calling the path end-to-end HDR.
2. **Audio:** session 2 has no default audio endpoint. Sunshine reports
   `0x80070490` and sends no seat audio. A real per-session endpoint/transport is
   required before the architecture is equivalent to Duo.
3. **Display-helper globals:** the session host still encounters a cross-session
   display-helper instance it cannot open (`winerr=5`). Non-console hosts now skip
   machine-global VDD startup/recovery and can stream the preactivated remote IDD,
   but helper ownership, display-apply retries, and recovery files are not yet
   safely scoped for multiple seats.
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

Snapshot time: 2026-08-15 10:24 America/Chicago, after the native HDR activation,
FP16 application, clean-driver isolation, and verified rollback runs.

- Evidence root:
  `C:/ProgramData/VibeshineDiagnostics/duo-session-spike-20260810`
- Provider log: `C:/ProgramData/Sunshine/RdpProtocolProvider/provider.log`
- Driver log: `C:/ProgramData/Sunshine/IddDriver/IddDriver.log`
- Original ignored findings:
  `D:/sources/sunshine/findings-duo-vdd-session-architecture.md`
- Session 1 is the only active interactive session and remains the `Chase`
  console. No experimental seat session or RdpIdd helper process is running.
  `RDP-Tcp` is listening. The persistent `Sunshine-Idd` listener is fail-closed
  at `fEnableWinStation=0`, `WinStationDisabled=1`, and `fLogonDisabled=1` and
  must remain there except during a bounded, timed proof.
- TermService, SessionEnv, and UmRdpService report running. UmRdpService's
  service-control handler is responsive; multiple guarded TermService restarts
  completed without a reboot. Secure Boot is enabled, VBS status is 2, security
  services 2 and 7 are running, and Code Integrity policy enforcement status is
  2.
- The staged and currently copied Remote IDD proof package is `oem51.inf`,
  version 1.3.0.26, signed and timestamped with the existing local test
  certificate. Its package hashes are DLL
  `619487D8543AF038924391504A2692B3BCC99D0022A4C8ED13212EAA55AE01B9`, INF
  `958D3AE47643E2EF338817212B137025799DC8E0CA66141F7F955CD8CB55AD69`, and CAT
  `9A8BC77D50DAA8F5E2CD8A2ACB844DFC5CE07DBDDBA66E7E73EAB2A09AD531E8`.
  Its clean isolation configuration keeps initial HDR, Duo HDR wire, and WCG
  prime enabled while both local-adapter flags and BGRA8 swapchain rejection are
  disabled. `oem50.inf` 1.3.0.24 and `oem49.inf` 1.3.0.23 remain staged as exact
  rollback packages.
- The registered provider is the known-safe ProgramData DLL with SHA-256
  `C39B80B06E817E585E1ED29D0594402D5341CB87604967C7349BA7C97875511C`.
  Its CLSID registration remains in place because the disabled listener refers
  to it. Proof artifacts remain staged in their Administrator/SYSTEM-owned
  ProgramData directory for offline inspection, but the volatile runtime arm,
  disposable account, credential, rollback task, and helper process are absent.
  The staged proof DLL is the older loaded artifact; use the corrected worktree
  build hash recorded above after reboot rather than trusting the staged file.
- The active product virtual display is `ROOT\\DISPLAY\\0001`, started with
  `oem11.inf`, version 1.6.3.0, and signer `Sunshine Virtual Display Release
  Signing`. Its DriverStore payload hashes are DLL
  `A01230696450B2E4CAAE0FD0EB4F6BBB641A07167111B2BF69CEDD6DCA17C32B`, INF
  `AFB06CB8D5C152A96E4D939D664BC21AA8A289C6055DFA338DEC7A42623CF7C9`, and CAT
  `C0C845A9C33102D3E02CDAE1A2A16FF1905474EA151B2EF5E7D6FEE72C19788C`.
  The older 1.3.0.x remote proof packages remain staged but are not the active
  device package.
- Fresh feature-state results are `54238000=1`, `54538524=1`, and
  `62841958=2` for all four change times. Priority 8 contains only the first two
  disable overrides. No additional reboot is required to preserve this state.
- Three historical seat tasks remain dormant and `Ready` under
  `\\VibeshineDiagnostics\\`: `DuoSeat2Civ6`, `DuoSeat2HdrEnable`, and
  `DuoSeat2Sunshine`. They point at retained August 10 proof scripts and must not
  be started as part of the next HDR gate run. No Fp16 one-shot or rollback task
  remains.
- The final simultaneous Steam proof used console PID 73704 and seat PID 64936.
  Both proof-owned process trees were stopped after evidence capture; no Steam
  process remains in either session.
- The deterministic HDR proof application and its process tree exited normally.
  Its source/composed frame captures and ETW traces remain in the evidence root.
- Hidden-gate evidence is retained under `idd-hdr-gate-20260814-142600`. The
  gate returned success with an empty missing-support mask, while advanced color
  remained inactive SDR/8-bpc. Its rollback record proves listener `0/1/1`, the
  safe provider hash, and zero proof seats. No reboot was used.
- WGC HDR bypass evidence is retained under
  `wgc-hdr-bypass-live-20260812`, together with the final HDR and SDR-control
  Moonlight logs. The proof host, WGC helper, Moonlight clients, port-58000
  listeners, and proof-owned scheduled task were removed without tearing down
  session 9. No trace session remains active.

## Next proof sequence

Work these in order unless new evidence changes the dependency chain.

1. **Close the now-proven native HDR path.** The non-preserving cloned-primary
   D3DKMT transaction is the working source-mode pin and should replace further
   capability-bit or late-monitor experiments.
   - Capture a deterministic FP16/scRGB pattern from the actual IddCx-acquired
     BGRA8 surface and compare it to the application backbuffer, WGC oracle, and
     encoded Main10 output. Record transfer function, primaries, range, clipping,
     and SDR-white behavior.
   - The source-only runtime now contains the minimum disposable seat-scoped
     D3D11/DXGI/D3DKMT helper sequence, launched after the Remote IDD reports 10
     bpc and before the game. Compile/package it, then reprove exact adapter/source
     matching, bounded timeout, post-release verification, and fail-closed cleanup
     on the signed driver. Do not carry forward BGRA8 swapchain rejection.
   - Prove persistence across long sessions, multiple HDR applications,
     reconnect, mode changes, and helper/host failure. Reapply only when advanced
     color is no longer active; never retain source ownership or the temporary
     primary allocation.
2. **Keep the stock RDP graphics Boolean as a measured negative boundary.** The
   installed `rdplite` has no writer for its HDR-monitor Boolean. Do not patch a
   Windows DLL, proxy the PKEY, or claim that cap version `0x000C0000` sets it.
   A future serviced Windows build may be rechecked by exact symbol/xref analysis.
3. **Use the corrected proof harness for any live source-pin test.** Re-run the
   helper packet, virtual-channel peer-close, and twice-in-one-process watchdog
   tests; arm independent SYSTEM rollbacks for the isolated listener, exact RDP
   ACL, and exact signed user-mode TermWrap. Rebuild the disabled listener table
   before activation and explicitly roll back rather than waiting for deadlines.
4. **Treat only native state as success.** Require
   `advanced_color_active=1`, HDR active color mode, an HDR source/KMD color
   space, 10-bit or FP16 source processing, and an HDR swapchain/frame. Supported,
   user-enabled, 10-bpc, a successful IOCTL, or Main10 encoding alone are not
   success.
5. **Keep WGC HDR as a diagnostic and quality oracle.** Independently parse its
   Rec.2020/PQ VUI and mastering/content-light SEI, compare decoded pixels from
   the deterministic scRGB pattern, and verify Moonlight's HDR output swapchain.
   Do not substitute this bypass for the requirement that Windows report active
   HDR on the terminal-session display.
6. **Choose and prove the Steam isolation boundary.** Keep the per-seat
   `-master_ipc_name_override`, then prototype the narrowest per-seat CEF/profile
   isolation. Prefer rewriting only Steam's `steamwebhelper.exe` child command to
   a seat-specific cache over a general-purpose sandbox or hooks inherited by
   games. Reprove two full Steam/webhelper trees, controller input, Steamworks IPC,
   updates, shutdown, and anti-cheat non-interference.
7. **Finish the visual game test.** With console Steam stopped, or after Steam
   isolation is green, launch Civilization VI app ID 289070 from `E:/SteamLibrary`
   inside the active seat under the retargeted console token. The deterministic
   renderer, not the game's appearance, remains the HDR oracle.
8. **Productize and regression-test the corrected session host.** The proven
   Sunshine capture path and exact Remote IDD revision now live in the
   `duo_session_large` lane, and the guarded provider source is preserved as a
   replayable patch series. Move the proof-only provider/controller into a
   product-owned seat broker, then reprove launch, reconnect, WGC, simultaneous
   streams, helper isolation, and teardown before any promotion.
9. **Inventory same-profile concurrency.** Extend the now-proven Steam collision
   audit to browsers, launchers, HKCU, AppData, and per-user services. Distinguish
   objects that merely need seat ACLs from names and files that require isolation.
10. **Provide per-session audio.** Prototype the endpoint/transport, then prove the
   session-2 stream contains only session-2 audio and survives reconnect.
11. **Prove remaining input classes.** Mouse, relative mouse, gamepad/ViGEm,
   Bluetooth controllers, touch/pen, focus changes, UAC, and secure desktop must
   not leak across sessions.
12. **Finish the libvirtualdisplay remote-session backend.** Build/sign/install and
   provider selection are now proven. Next prove dynamic mode, session-scoped
   control, swapchain/HDR Update2 behavior, and bounded teardown while preserving
   the existing root/console backend separately.
13. **Finish the clean seat broker.** The single-pair control-plane contract and
   per-client opt-in now exist on task branch `duo-seat-broker`. Implement the
   privileged Windows runtime and protected local IPC behind those hooks. It must
   own managed-seat session creation, listener/RDP lifetime, display activation,
   console-token process launch, WinSta/Desktop/BaseNamedObjects ACL preparation,
   Steam isolation identity, port allocation, resource admission, reconnect, and
   transactional cleanup. The seat worker consumes a one-use launch reservation;
   it does not run a second pairing or public Web UI endpoint.
14. **Remove global Sunshine assumptions.** Scope or broker display-helper state,
   recovery files, update checks, mDNS, integrations, input devices, and other
   singleton resources.
15. **Stress and compatibility test.** Run two and then more live seats at
    1080p120, 1440p120, and 4K HDR; measure GPU, VRAM, encoder, copy engine, CPU,
    frame pacing, audio, and network behavior. Include Steam, third-party launchers,
    DRM, and anti-cheat titles.
16. **Lifecycle and servicing matrix.** Exercise disconnect/logoff, crashes,
    provider/driver restart, TermService restart, sleep/resume, reboot, GPU driver
    update, and Windows cumulative update. Every unsupported state must fail closed
    without changing the console topology or another seat.
17. **Compatibility and licensing decision.** Select a supportable client-SKU
    strategy and document the clean-room boundary before productizing the session
    provider/display bridge.

## Clean broker implementation checkpoint (2026-08-15)

The source-owned control plane is implemented on the correction branch and is
being landed back into `duo_session_large` after validation. This is deliberately not
a claim that concurrent Windows seats are available on this host.

- `terminal_session_protocol` defines version 1 of a bounded binary request /
  response protocol (4 KiB maximum), complete RTSP/control/video/audio route
  bundles, explicit reject reasons, and one-use ten-second admission tickets.
  Requests are bound to the paired TLS client UUID, launch ID, and role
  generation. The authority rejects malformed or oversized frames, wrong UUID,
  stale generation, unauthenticated peer identity, expired tickets, and replay.
- `terminal_session_service::endpoint_t` is hosted by `sunshinesvc` through a
  protected first-instance `NamedPipeFactory`/`FramedPipe` endpoint. The main
  process validates the server PID, SYSTEM token, and service image; the service
  validates the client PID, token SID, process creation identity, and Sunshine
  image. A service-issued one-use challenge is bound to the exact UUID,
  generation, launch, peer, and opcode. Remote clients and pipe precreation are
  rejected; ticket and peer secrets are never logged or persisted.
- `terminal_session::runtime_t` is a transactional provider-to-worker state
  machine. Provider preflight must prove concurrent-session, remote-display,
  token-launch, and seat-audio capability before allocation. Provider failure,
  incomplete worker readiness, or incomplete port bundles release owned
  resources in reverse order. Disconnect, unpair, shutdown, and duplicate
  cleanup are idempotent and scoped to the exact client.
- `terminal_session_worker` defines isolated config/state/log roots and an
  explicit worker command contract disabling public Web UI, pairing, mDNS,
  updater, and global display mutations. The worker consumes its one-use ticket
  from the protected local channel; it does not pair again.
- The service-owned worker process path is real and testable: it uses a
  CSPRNG rendezvous name, first-instance/reject-remote pipe, provider-owned
  primary token, explicit session/desktop contract, kill-on-close job, exact
  child PID handshake, one-use ticket validator, and reserved four-port route
  equality. It is unreachable in the production build until a provider supplies a valid token/session/display/audio
  resource, so no SYSTEM worker is ever launched as a fallback.
- HTTP startup registers the runtime before exposing launch/resume. The default
  in-tree provider is an explicit fail-closed capability gate because no
  supportable Windows concurrent-session provider with owned Remote IDD and
  seat-scoped audio is currently available. Terminal opt-in therefore returns
  service-unavailable/admission failure and never falls back to console
  streaming. Non-terminal clients retain the existing path.
- Focused tests cover codec bounds, complete route bundles, challenge-bound
  authentication, replay, identity binding, provider rollback, idempotent
  cleanup, and worker isolation flags. Tests use fake provider/worker objects;
  no driver, account,
  service, registry, TermService, or live session mutation is part of this
  implementation.

Still unsupported: a legally and operationally supportable Windows session
provider, Remote IDD ownership/activation in that provider, seat-scoped audio,
firewall/WAN port mapping, Steam/CEF singleton isolation, and end-to-end
multi-seat HDR/input proof. Those are provider admission requirements, not
hidden fallbacks or proof-only patch mechanisms. A future provider plugin must
implement capability preflight and owned session/display/audio/token handles,
complete reserved port allocation, and reverse-order release without modifying
Microsoft DLLs or weakening Core Isolation/Code Integrity.

## Remote display-helper mode integration (2026-08-16)

The live mode-control proof closed the driver-side part of the dynamic-resolution
gap without ending the terminal session or restarting either display driver.
Session 72 began at 1920x1080, then accepted 2560x1440 at 60 Hz on the same
Remote IDD display id (`8070450532247928833`). Windows DisplayConfig and the
driver state both reported the new mode, WGC rejected its stale continuation
because the output geometry changed and recaptured at 2560x1440, and HDR/HEVC
remained active. The installed console `SunshineService` stayed on PID 52564
throughout the proof.

The Sunshine integration now routes that proven operation through
`sunshine_display_helper.exe` when the helper is running in a non-console WTS
session:

- helper mutexes, IPC pipe names, stale-process cleanup, logs, snapshots, and
  recovery files are scoped to the exact Windows session;
- a private terminal worker launches its helper inside the containing job rather
  than requesting `CREATE_BREAKAWAY_FROM_JOB`, which was the source of Win32
  error 5 in the managed seat;
- the v2 helper opens only the Remote IDD control interface whose device session
  id matches its own session, requires exactly one driver display entry, and
  applies resolution, refresh, HDR, refresh-only changes, and snapshot restores
  through that display id;
- a remote helper never creates or deletes the console user's machine-wide boot
  restore task; its restore ledger remains inside the disposable WTS seat;
- `third-party/libvirtualdisplay` now points to
  `c7c8c97b3793a4f75c0f3f263b53376f6af98397`, and patch 0006 preserves the
  live mode-control source after the earlier HDR proof series.

This checkpoint has source/static validation only. A fresh Duo-lane build and a
helper-driven 1080p-to-1440p runtime repetition remain required before calling
the new integration proven end to end.

## Remote-session security correction (2026-08-16)

The remote driver control boundary is now retained during proof builds: the
generator no longer accepts proof-only SDDL overrides, both remote device and
interface descriptors remain SYSTEM plus the broker service SID, and remote
interface enumeration rejects zero or multiple matching seats before opening
one. The landed libvirtualdisplay source is `ac2a37aa2ae698f76c593540ef52a586b5886e38`.

Managed workers now carry immutable session/generation facts and a per-launch
capability into their helper. The helper endpoint, mutex, and recovery storage
are capability-scoped; invalid managed identity fails closed instead of
becoming console/global behavior. Snapshot reads are bounded and reject
reparse-point files, helper IPC binds both peer directions to PID/creation
time/image/session, and stale helper cleanup revalidates the enumerated
identity immediately before termination. Managed helper query, resolution,
refresh, and HDR requests now use a SYSTEM-owned per-worker capability pipe;
the broker authenticates the helper/job identity and chooses the sole remote
device server-side before opening the protected driver. These changes are
source/static only; adversarial runtime validation remains required.

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

## User-mode Steam isolation source checkpoint (2026-08-16, Daybreak correction)

The custom Steam kernel WFP driver, IOCTL ABI, WDK project, INF/CAT packaging,
installer actions, and special-signing option have been removed. The replacement
is a LocalSystem-only `fwpuclnt` manager using persistent provider/sublayer
objects and exact V4/V6 ALE AppId block filters. Filters are installed in a
transaction before a seat worker resumes and are removed only after its process
tree is confirmed stopped; missing filters or BFE admission failure poison the
seat and leave cleanup fail-closed.

The manager derives a protected root from OS ProgramData, uses randomized
SYSTEM-owned staging and no-follow/reparse rejection, bounds recursive files,
bytes, depth and path length, excludes game libraries/volatile state, retains
`steamservice.exe` outside the mirror, and publishes an ordinary user-mode
webhelper proxy plus renamed real helper. Cache/htmlcache/userdata leaves are
created by SYSTEM with seat-user Modify ACLs before launch. Steam's direct
command is rewritten to the mirror and private cache/user-data root while games
retain their configured library paths. WFP cleanup is scoped, paginated,
structured by exact seat/generation ownership, and health uses one scoped
enumeration rather than per-filter polling.

Daybreak's first-pass rejection identified a critical path-only filesystem
boundary, weak WFP lifetime/schema checks, unbounded recursion, cache ACL and
proxy argument failures, and teardown/performance gaps. The follow-up correction
also requires the broker to enumerate/open the source tree under a duplicated,
SID-validated console-user impersonation token, copy from held source handles,
apply and verify descendant ACLs before publication, discover nested helpers,
and retain a service-lifetime quarantine monitor for unresolved teardown. These
source corrections are represented above. The remaining deliberate limitation is same-console-SID
hostile bypass: standard AppId filters cannot distinguish a malicious process
that launches the original Steam path. The job monitor poisons/terminates
out-of-mirror Steam images, but does not provide a pre-network zero-window
security boundary; administrator/higher-priority WFP policy may also override
standard user-mode filters. Filters remain persistent when worker termination is
not proven; the quarantine monitor continues health/termination attempts and
secure cleanup. Health polling is bounded and scoped with foreign-seat owners
ignored, and no runtime proof is claimed.

This checkpoint is source/static only: no build, unit-test execution, install,
Steam launch, live WFP mutation, or runtime proof was performed. Required proof
gates are copied Steam launch, two independent webhelper/cache trees, console
versus clone AppId filtering, online game behavior, BFE restart and persistent
filter reconciliation, Steam update/relaunch escape detection, reconnect reuse,
and fail-closed teardown when filter deletion fails.
