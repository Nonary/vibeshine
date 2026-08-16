# Vibeshine Steam Offline Filter (WFP callout)

This directory is a source-only WDM driver lane.  It is intentionally isolated
from the normal Sunshine CMake build because the checkout does not carry the
Windows Driver Kit.  `build.ps1` and the project file are the only build entry
points; they fail closed when a matching WDK/MSBuild installation is absent.

## Contract and threat model

The control device is `\\.\VibeshineSteamOfflineFilter` and is created with the
SDDL `D:P(A;;GA;;;SY)`.  The device accepts only the two version-1 buffered
IOCTLs already defined by `src/steam_offline_filter_ioctl.h`.  The registration
request carries PID, process-creation timestamp, caller generation, and a
bounded printable seat token.  The C ABI mirror in
`steam_offline_filter_ioctl_abi.h` is byte-for-byte checked by the static
contract script; it is not a second user-mode protocol.

Unregister carries the registration ID together with the same seat token and
generation; the driver additionally requires the opening file object and
owner-process identity that performed registration.

`register_root.process_creation_time` is the value returned by
`PsGetProcessCreateTimeQuadPart` for the root process; `generation` is retained
as the broker's non-reusable seat generation and must be nonzero. Registration
looks up the PID, compares that creation identity, references the `PEPROCESS`,
and stores the root's kernel-supplied image path. Descendant lineage is bound
to `PS_CREATE_NOTIFY_INFO.CreatingThreadId.UniqueProcess` and that creator's
stored process-creation timestamp; the caller-selected `ParentProcessId` is
never trusted. A stale PID therefore cannot register a new process. The service must register while the root is suspended
and before it creates descendants; the process notification callback then
records every future descendant in the bounded nonpaged table by referenced
process object and registration generation. Up to 16 independent seat
registrations may coexist; seat ID and generation are duplicate guards and are
required again, together with the same opening file object and owner process
identity, for unregister.

The process table is bounded at 1024 entries and reserves a hard quota of 64
entries per registration (including its root). Registration fails explicitly
for an invalid root, duplicate registration, stale generation, malformed image
path, a per-registration quota, or a full table. If a later descendant of a
registered lineage arrives after its quota or the global table is full, the
process-create callback sets
`PS_CREATE_NOTIFY_INFO.CreationStatus` to the allocation failure and rejects
that exact child. It never globally blocks console Steam or unrelated
processes. Failure counters are maintained in nonpaged storage and emitted
through the driver diagnostic channel; a future status IOCTL can expose them
without weakening this ABI.

Only these exact, case-insensitive basenames are eligible for blocking:

* `steam.exe`
* `steamwebhelper.exe`
* `GameOverlayUI.exe`
* `steamerrorreporter.exe`
* `steamerrorreporter64.exe`

The path is taken from `PS_CREATE_NOTIFY_INFO.ImageFileName` or
`SeLocateProcessImageName`, must be an absolute kernel path with no control
characters or `..` segments, and is compared byte-for-byte (case-insensitive)
with WFP's `FWPS_METADATA_FIELD_PROCESS_PATH`.  This is the safe
basename-plus-image contract.  `steamservice.exe`, arbitrary game children,
addresses, DNS names, ports, payloads, and endpoint data are never inspected.
Process-notify and root-registration paths validate and copy that
`UNICODE_STRING` into a fixed driver-owned canonical image record at
`PASSIVE_LEVEL`, then release the source allocation before taking
`gProcessLock`.  Locked insertion copies only that resident record; it never
dereferences caller or image-provider backing bytes while running at elevated
IRQL.

Runtime isolation remains default-off because the WFP filter does nothing until
a SYSTEM worker registers an exact root. The Windows packaging option is
currently disabled at configure time: the driver is intentionally
non-unloadable after startup, and MSI has no coherent two-phase reboot
transaction for upgrade/uninstall. The source package artifacts and contract
checks are retained for a future enabled lane. The current service installation runs both the broker and its control path as
LocalSystem and does not configure a `SunshineService` service SID. Therefore
the device ACL intentionally grants only LocalSystem, and the driver also
checks the effective primary token. This prevents ordinary users but cannot
distinguish a malicious SYSTEM peer or administrator who can obtain SYSTEM;
that residual threat is explicit. The concrete migration recommendation is to
configure the existing `SunshineService` service SID (`NT SERVICE\SunshineService`)
and grant that SID device access only after the service and broker ACLs are
staged and verified; keep the SYSTEM-only SDDL until that migration is complete.
The stronger long-term boundary is a broker-issued per-boot capability
handshake as a versioned ABI change, but no undocumented kernel secret is
invented here. The seat/generation and opening-file binding are ownership and
lifetime checks, not a claim that LocalSystem peers are cryptographically
separated.

## State machine and teardown

`DriverEntry` creates the SYSTEM-only buffered control device, registers the
supported process notification callback, registers two UNKNOWN-action WFP callouts
and two dynamic filters, then waits for a SYSTEM broker registration:

```text
EMPTY --register(root PID + creation time + seat/generation)--> ACTIVE[n]
ACTIVE[n] --child create--> ACTIVE[n] (same registration lineage)
ACTIVE[n] --root/descendant exit--> ACTIVE[n] (entry removed and dereferenced)
ACTIVE[n] --unregister(exact ID + seat/generation + owner file)--> ACTIVE[n-1]
any state --driver unload--> STOPPING --> EMPTY
```

Both filters are restricted to `FWPM_LAYER_ALE_AUTH_CONNECT_V4` and
`FWPM_LAYER_ALE_AUTH_CONNECT_V6`, use the private stable
`GUID_STEAM_OFFLINE_SUBLAYER` (dynamic weight `0x8000`), and carry
`FWP_ACTION_CALLOUT_UNKNOWN` plus
`FWPM_FILTER_FLAG_PERMIT_IF_CALLOUT_UNREGISTERED`. Classification uses
`FWP_ACTION_CONTINUE` for eligible nonmatches and changes to block only when an exact process-table entry,
registered lineage, Steam-client basename, and matching process path are all
present. The classify and locked-table paths use only bounded nonpaged table/blob
data and manual ASCII/UTF-16 code-unit folding; they do not call
`RtlEqualUnicodeString` or resolve image paths. No registration means permit
for every process.

The opening `FILE_OBJECT` owns a registration. `IRP_MJ_CLEANUP` and
`IRP_MJ_CLOSE` automatically detach every registration owned by that file
object, so a broker crash cannot strand a filter. The unload sequence removes process notifications, deletes both dynamic
filters, callout objects, and the private sublayer in that order, unregisters
kernel callouts, and removes the registration table,
derefences every retained process, deletes the symbolic link and device, and
never leaves a worker or callback pointing at freed storage. A live successful
WFP registration clears `DriverUnload`, so service stop cannot unload callback
code; checked rollback/unregister paths are used before that point and a
failed package removal remains staged for reboot. Cleanup preserves the first
diagnostic deletion error for logging, but device teardown is governed by
authoritative callback/engine/subscription ownership (including a pending BFE
callback), not by that diagnostic alone. If startup rollback itself cannot
drain a callback, `DriverEntry` removes the public name and retains
`DO_DEVICE_INITIALIZING` on a dormant non-unloadable device with readiness
false instead of returning failure to a loader that could unload live callback
code; SYSTEM admission remains rejected until reboot. All table state
is fixed nonpaged storage protected by a spin lock; no classify path allocates,
hooks, or examines executable memory.

`uninstall.ps1` treats a still-running non-unloadable service as a staged
removal, marks the service/package for deletion, and reports the required
reboot; it never claims that callbacks are gone before that boundary.

The checked-in WDK 10.0.26100 headers expose no supported
`PsGetProcessSessionId`/`PsGetProcessSessionIdEx` or process-to-session query;
the local `km/ntddk.h` contains `ProcessSessionInformation` only as an
`NtQueryInformationProcess` information-class enum, not a callable kernel
driver API. They also expose no supported `PsGetProcessJob`, `PEJOB`, or
equivalent process-to-job membership query. The driver therefore cannot safely
replace the callback lineage table with exact session or job membership
without using undocumented interfaces. This is an explicit residual: a
non-standard process-clone path that does not produce a supported process
notification could evade lineage registration and will be permitted by WFP.
The Windows worker assigns the root to a non-breakaway job before resume and
does not request breakaway, but the source contract does not claim that this
closes undocumented clone behavior.

The driver subscribes to supported BFE state notifications and exposes a
SYSTEM-only readiness/status IOCTL. BFE callbacks and FWPM object
creation/publication serialize on a PASSIVE_LEVEL state lock; every state
transition advances a readiness generation and clears readiness before the
callback returns. A root registration response carries that exact generation,
and the worker performs a synchronous status proof against it immediately
before `ResumeThread`, then polls every 250 ms and terminates only its own job
on loss. The dynamic FWPM session is not recreated from the callback: RUNNING
also remains WFP-not-ready until a future rebuild/restart proves all
callout/filter objects ready. New registration is refused across a generation
change. This is a bounded detection window (up to one poll interval plus
callback scheduling), not a zero-window fail-closed guarantee.

## Build, package, and signing gates

`SteamOfflineFilter.vcxproj` uses the WDM WindowsKernelModeDriver10.0 toolset,
`fwpkclnt.lib`, `ntoskrnl.lib`, and `wdmsec.lib`.  `build.ps1` requires an
explicit WDK/MSBuild installation and emits a driver binary under the selected
build output directory.
`SteamOfflineFilter.inf` is demand-start (`StartType=3`), depends on BFE, sets
`PnpLockdown=1`, and references a generated catalog.  `package.ps1` runs
embedded-signs the SYS first, runs `Inf2Cat` for the chosen OS/architecture,
then signs and verifies the catalog. It requires configured certificate
thumbprint, issuer, and publisher values; it never offers a skip-signing
switch and makes no retail/HVCI trust claim.

The normal Windows CMake/CPack path rejects
`SUNSHINE_BUILD_STEAM_OFFLINE_FILTER_DRIVER=ON` at configure time with a
precise unsupported two-phase-reboot-transaction error. The option defaults
OFF, so ordinary packages contain no driver payload or actions. The source
fragment, artifact gate, signing checks, and lifecycle scripts remain staged
for a future safe package; runtime isolation remains dormant until a SYSTEM
worker registers a root. Missing or empty `.sys`, `.inf`, `.cat`, or lifecycle
scripts fail the future package lane.

## Required live validation (not run here)

Before shipping, validate on a HVCI/Core Isolation-enabled x64 machine with a
properly signed catalog: service start after BFE, SYSTEM-only device ACL,
registration rejection for stale PID/generation and non-SYSTEM callers,
descendant tracking across PID reuse, blocking of each five exact client
images, permit of `steamservice.exe` and arbitrary game children, IPv4 and IPv6
connect authorization, unregister race, process-exit race, driver unload, and
clean rollback after failed filter/callout installation.  Also capture Code
Integrity, BFE, WFP, and Driver Verifier evidence.  No live or build validation
is claimed by this source-only change.
