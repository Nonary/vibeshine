# Delegation plan: Display engine v2 regenerative clean-room audit

- Created: 2026-08-03T23:12:09.4321104-05:00
- Canonical plan: D:\sources\worktrees\display-engine-v2-regenerative-audit\.codex\delegation-plans\20260803-231209-display-engine-v2-regenerative-audit.md
- Requested outcome: Establish that the Gherkin retains every source-visible display engine v2 business rule needed for a future clean-room regeneration, and close every evidenced gap without prescribing implementation structure.
- Main-model status: source audit and integration complete; final static validation and handoff in progress
- Default worker model: Sol Ultra, explicitly requested by the user for this final audit
- Explorer model: Sol Ultra, overriding the skill default because the user explicitly requested Sol Ultra
- Maximum active subagents: 3

## 1. Outcome and completion criteria

### Outcome

The feature suite under `behaviors/display_engine_2/scenarios` will serve as a regenerative functional contract: a future implementation may use different classes, processes, concurrency, persistence mechanics, or Windows APIs, while reproducing every observable decision, default, state transition, compatibility rule, fallback, failure outcome, ordering constraint, retry cap, cancellation boundary, and known non-guarantee defined by the current display engine v2 production code.

### Complete when

- Every relevant production source family is mapped to one or more Gherkin scenarios, including shared configuration-to-request policy and every direct lifecycle consumer, not only the v2 helper internals.
- Every branch that changes a caller result, display mutation, recovery obligation, persisted state, target choice, stream decision, retry/reset path, workaround, or externally meaningful timing envelope has a scenario.
- Durations are named as current source defaults and may be tuned; source-defined attempt caps, ordering, attribution, cancellation, and terminal outcomes remain explicit.
- Protocol compatibility, malformed input, unavailable evidence, stale work, partial persistence, platform failure, and best-effort behavior remain distinguishable rather than being idealized into guarantees.
- No scenario requires a particular class, thread, queue, callback, IPC transport, Windows API, storage primitive, or scheduling implementation unless the mechanism itself is externally visible behavior.
- All feature files pass structural Given/When/Then, Scenario Outline/Examples, unique-title, causal-order, whitespace, and implementation-prescription checks.
- The main model performs a source-to-scenario closure audit after all workers finish and records any deliberately excluded non-behavioral mechanics.
- Documentation changes are committed on the task branch and offered to the normal `unverified` landing lane only when repository ownership policy permits it safely.

## 2. Main-model inspection findings

- Workspace state: task worktree `D:\sources\worktrees\display-engine-v2-regenerative-audit` is clean on `codex/display-engine-v2-regenerative-audit`, based at current local `unverified` commit `b76469374`. The primary checkout is clean but currently owns `unverified`; it must not be changed or used for task edits.
- Applicable instructions: `D:\sources\sunshine\AGENTS.md` requires task-worktree isolation, no unit-test creation/execution, no build for documentation validation, narrow commits, and serialized landing through `unverified` only when that branch is not owned elsewhere.
- Existing contract: seven feature files contain 434 scenarios: helper protocol/session lifecycle (88), state/orchestration (55), snapshots/golden (62), Apply/topology (38), recovery/platform (48), client/stream integration (137), and clean-room/defaults (6).
- Source drift check: no changes exist from the final prior contract commit `a20b170a1` to current `HEAD` in the already-audited v2 helper, v2 core, integration, builder, request-helper, client, watchdog, or scenario paths.
- Newly exposed coverage risk: `src/display_device.cpp` and `src/display_device.h` contain shared business policy used to form v2 requests, but the current feature text does not explicitly cover all resolution/refresh parsing, client override precedence, manual/automatic/disabled/prefer-highest modes, remapping selection/matching/failure, output-name resolution, existence/active uncertainty, or refresh-override detection.
- Broader consumer surface confirmed by direct search: `src/config.cpp`, `src/confighttp.cpp`, `src/main.cpp`, `src/nvhttp.cpp`, `src/process.cpp`, `src/stream.cpp`, `src/video.cpp`, `src/webrtc_stream.cpp`, `src/platform/windows/hotkey_manager.cpp`, `src/platform/windows/misc.cpp`, and `src/platform/windows/virtual_display_cleanup.cpp` call or influence display-helper lifecycle behavior.
- Relevant core paths and roles:
  - `tools/display_settings_helper_v2.cpp` — startup modes, command parsing, wire compatibility, connection lifecycle, and helper event dispatch.
  - `src/platform/windows/display_helper_v2/types.h`, `runtime_support.h`, `async_dispatcher.*`, `state_machine.*` — command vocabulary, ownership, ordering, cancellation, verification, recovery, and timing policy.
  - `src/platform/windows/display_helper_v2/snapshot*`, `golden_health.*`, `staged_settings.h` — compatible persistence, filtering, rotation, candidate eligibility, health, and known persistence limitations.
  - `src/platform/windows/display_helper_v2/operations.*`, `win_display_settings.*`, `win_event_pump.*`, `win_scheduled_task_manager.*`, `win_platform_workarounds.*`, `win_virtual_display_driver.h` — Apply, verification, recovery, Windows workarounds, event behavior, durable restore, and virtual reset.
  - `src/display_device.*`, `src/display_helper_builder.*`, `src/platform/windows/display_helper_request_helpers.*` — configuration and session policy translated into request intent.
  - `src/platform/windows/display_helper_integration.*`, coordinator/session-deferral/watchdog files, and `src/platform/windows/ipc/display_settings_client.*` — helper selection, deadlines, compatibility, deferred work, direct fallbacks, supervision, and reply attribution.
  - direct callers listed above — stream start/resume/stop, capture settling, process exit, configuration changes, HTTP APIs, hotkeys, cleanup, and shutdown.
- Existing work to preserve: all 434 scenarios and three prior audit plans are user-requested work. Concurrent agent edits must remain restricted to disjoint feature ownership.
- Validation restriction: read production source and existing tests only as evidence; do not create, modify, compile, or run tests, builds, packages, or runtime artifacts.
- Uncertainty retained for execution: the explorer must define the final production source universe and identify externally meaningful behavior outside the established paths; workers must not treat logging cadence, polling granularity, class layout, or transport mechanics as business rules without an observable effect.

## 3. High-level strategy

1. Build an independent source-to-behavior inventory that starts at configuration and public callers, follows request derivation into helper protocol/core/platform work, and follows every result back to stream, API, process, and cleanup consumers.
2. Audit the contract in three disjoint behavioral domains: public policy/protocol/callers; state/persistence; Windows Apply/recovery/workarounds. Each owner patches only its Gherkin files and returns an explicit source-to-scenario closure map.
3. Add a separate configuration/mode-policy feature if needed so shared business policy is reconstructible without overloading the already-large client lifecycle feature.
4. Main-model integration will challenge every claimed omission and addition against source, reconcile cross-domain ordering and vocabulary, remove structural prescription, and run full-suite static validation.
5. Commit only documentation artifacts. Attempt serialized local `unverified` integration only if its checkout ownership becomes safe; otherwise preserve the clean task branch and report the exact blocker.

### Preserve

- All current externally visible outcome distinctions: verified, failed, unknown, unavailable, cancelled, gave-up, retryable, fatal, invalid, and reset-required.
- Request/session attribution, stale-result rejection, durable recovery after possible mutation, and cancellation at the next safe behavioral boundary.
- Exact source-defined branch order and retry/attempt ceilings; timings remain recommended operational defaults unless the value itself is a compatibility boundary.
- Actual compatibility and non-guarantees, including legacy replies, partial or unavailable platform evidence, best-effort workarounds, and persistence replacement limitations.
- Existing user wording preference: define logic and structure of behavior, not implementation architecture.

### Exclude

- Reimplementation design, classes, data structures, process/thread topology, synchronization, transport choice, persistence primitives, Windows API call lists, or build/package work.
- Legacy display helper behavior except where v2 selection, fallback, compatibility, or caller result explicitly depends on it.
- Pure diagnostics and logging frequency that do not change an observable operation, safety outcome, or supported interface.
- Unit-test changes or execution.

## 4. Delegation topology

| ID | Role | Model | Wave | Depends on | Shared-state notes | Status |
|---|---|---|---:|---|---|---|
| E1 | Complete source-universe and branch explorer | Sol Ultra | 1 | Plan publication | Read-only; routes findings through main model | Completed through incremental evidence routing and main-model closure inventory |
| W1 | Public policy, protocol, caller, and lifecycle contract owner | Sol Ultra | 1 | Plan publication | Owns 01, 06, 07, and may create 08; no other feature edits | Completed |
| W2 | State, orchestration, persistence, and golden contract owner | Sol Ultra | 1 | Plan publication | Owns 02-03 only; reports platform gaps | Completed |
| W3 | Windows Apply, recovery, event, and workaround contract owner | Sol Ultra | 2 | E1 source map | Owns 04-05 only; consumes routed cross-file findings | Completed |

### Wave rationale

- Wave 1 starts the mandatory read-only explorer and the two domains whose source boundaries are already directly established.
- Wave 2 starts W3 after E1 has enumerated every Windows/platform source and caller, avoiding a second platform blind spot.
- The main model remains active throughout, inspects shared-policy source directly, routes explorer findings, and owns the final adversarial review.

### Shared-state protocol

- All agents work in the shared task worktree but edit disjoint feature files.
- E1 is read-only. W1 owns 01/06/07 and any new configuration-policy feature; W2 owns 02/03; W3 owns 04/05.
- No agent stages, commits, resets, cleans, deletes, builds, runs tests, or changes production source.
- An agent that finds a gap owned by another domain reports exact source evidence to the main model rather than editing the other owner's file.
- Agents must inspect current file content immediately before each patch and preserve concurrent changes.

## 5. Detailed agent instructions

### E1 — Complete source-universe and branch explorer

**Model:** Sol Ultra, honoring the user's explicit model request
**Wave:** 1
**Depends on:** Published delegation plan
**Status:** Pending

#### Objective

Produce a read-only, branch-level inventory of every current production behavior that participates in display engine v2, from user/config/API/session inputs through request construction, helper/core/platform execution, persistence/recovery, and every caller-visible result or cleanup decision. Map each behavior to an existing scenario or flag it as unowned with exact evidence.

#### Why this assignment exists

Prior audits were deep inside the helper and integration layers, but regenerative completeness requires proving the source boundary itself. This explorer prevents shared configuration policy or distant lifecycle consumers from falling outside the contract.

#### Search targets

1. Trace `display_device::parse_configuration`, `refresh_rate_override_active`, `map_output_name`, `output_exists`, and `output_is_active` through request helpers and all callers.
2. Trace every public function in `src/platform/windows/display_helper_integration.h`, `src/platform/windows/ipc/display_settings_client.h`, and `src/display_helper_integration.h` to direct production consumers.
3. Enumerate all commands, statuses, events, state transitions, timing constants, retry/attempt caps, compatibility branches, persistence behaviors, Windows workarounds, and failure/cancellation outcomes in the v2 helper/core/platform source.
4. Inspect config parsing/defaults and HTTP/public maintenance routes that change or expose display engine behavior.
5. Search all production `src` and `tools` call sites, including `config`, `confighttp`, `nvhttp`, `process`, `stream`, `video`, `webrtc_stream`, hotkey/misc/cleanup, main/shutdown, builder, coordinator, session deferral, watchdog, and client IPC.
6. Compare each branch family against scenario titles/steps in 01-07; identify exact gaps, inaccurate claims, over-prescribed mechanisms, duplicate/contradictory rules, and known source limitations missing from the contract.
7. Treat tests only as secondary evidence of edge cases; production source decides the contract.

#### Boundaries

- Read and inspect only. Do not edit files, change Git state, implement scenarios, run tests/builds, or spawn subagents.
- Do not redesign the plan. Report exact paths, symbols, conditions, and externally observable consequences.
- Distinguish business logic from implementation mechanics. A branch is contract-worthy when it changes an accepted input, output, target, mutation, ordering, safety obligation, persisted state, caller action, retry cap, default envelope, workaround, or supported compatibility behavior.

#### Deliverables

Return a structured map containing:

1. Confirmed production source universe and public/caller integration points.
2. A source branch family -> scenario file/rule/scenario mapping.
3. Unowned or inaccurately documented behaviors with exact source evidence and recommended owner ID.
4. Source-defined defaults/caps and which are duration-tunable versus logically fixed.
5. Deliberately excluded non-behavioral mechanics and why.
6. Conflicts with this plan or concurrent work.

#### Escalate to the main model when

- The production boundary extends beyond the listed source families.
- Current source contradicts an existing scenario or makes the intended behavior ambiguous.
- A behavior crosses ownership in a way that requires coordinated edits.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

### W1 — Public policy, protocol, caller, and lifecycle contract owner

**Model:** Sol Ultra, honoring the user's explicit model request
**Wave:** 1
**Depends on:** Published delegation plan
**Status:** Pending

#### Objective

Make all configuration-to-request policy, helper protocol, public caller, and stream/process/API lifecycle business logic reconstructible in Gherkin, patching only 01, 06, 07, and if warranted a new `08_configuration_and_mode_policy.feature`.

#### Why this assignment exists

The largest identified regenerative risk is shared policy outside the helper core—especially `src/display_device.cpp`—plus distant callers whose failure and cleanup decisions define user-visible behavior.

#### Inputs and established facts

- Current 01 covers helper startup/protocol/compatibility; 06 covers request construction/integration/stream lifecycle; 07 defines public outcomes and tunable defaults.
- `src/display_device.cpp` contains explicit parsing, precedence, remapping, output mapping, existence/active uncertainty, and refresh-override rules not obviously represented today.
- Direct callers include config, HTTP APIs, RTSP/HTTP stream start and resume, WebRTC, process and stream teardown, capture initialization, hotkeys, cleanup, and shutdown.
- Internal transport or concurrency is not the contract; exact accepted forms, precedence, results, deadlines, cancellation, fallbacks, and caller actions are.

#### Scope and ownership

- Primary files: `behaviors/display_engine_2/scenarios/01_helper_protocol_and_session_lifecycle.feature`, `06_client_integration_and_stream_lifecycle.feature`, `07_clean_room_contract_and_defaults.feature`, and optionally new `08_configuration_and_mode_policy.feature`.
- Primary source: `tools/display_settings_helper_v2.cpp`, `src/display_device.*`, `src/display_helper_builder.*`, request helpers, integration/coordinator/session-deferral/watchdog, IPC client, and all direct callers listed in section 2.
- May inspect v2 core/platform and tests for context.
- Must not modify 02-05, production source, prior plans, Git state, tests, or build artifacts.
- Concurrent overlap: E1 is read-only; W2 edits 02-03. Report state/persistence/platform gaps to main model.

#### Required implementation or production steps

1. Build a function/branch checklist for every primary source and direct caller; mark each as already covered, missing, inaccurate, purely diagnostic, or structural-only.
2. Add detailed scenarios for configuration parsing and precedence: resolution syntax and empty/invalid behavior; refresh rational/decimal syntax and range behavior; client display-mode override precedence; automatic/manual/disabled/prefer-highest choices; HDR/RTX/dummy-plug choices; device-preparation modes; invalid parsing terminal behavior.
3. Add mode-remapping behavior: when mixed/resolution-only/refresh-only lists apply, selected list, entry parsing, required final value, ordered first match, wildcard fields, comparison basis, client override bypass, no-match behavior, and invalid-entry failure.
4. Add output identity behavior: case-insensitive matching by device/display/friendly identity, logical display passthrough, mapping fallback, empty output, unavailable enumeration uncertainty, existence versus active distinction, and capture-target consequences.
5. Add refresh-override detection and any request-helper/builder branch not already explicit, including frame-generation effective rate and configuration-disabled exceptions.
6. Audit every direct caller for apply/revert/disarm/snapshot/export/reset/query/watchdog/capture result decisions, including config-change reverts, process/stream deferred reverts, hotkey/misc cleanup, HTTP route semantics, and shutdown.
7. Correct any protocol/default/deadline/cancellation wording contradicted by source. Preserve legacy compatibility and distinct failed/unknown/unavailable/cancelled/gave-up outcomes.
8. Prefer a new 08 feature for configuration/mode policy if that keeps source-independent business vocabulary coherent; do not organize by class or file.
9. Validate owned features structurally and run `git diff --check`. Return a source-to-scenario closure map and deliberate exclusions.

#### Required behavior and edge cases

- Invalid or missing manual values must produce the source's parse failure, not a fabricated automatic value.
- Client display-mode override must retain its actual priority and bypass semantics.
- A remapping entry must not partially apply after an invalid required field; first matching valid entry and wildcard semantics must remain exact.
- Enumeration failure must preserve the source's uncertainty behavior rather than claim the output is absent/inactive.
- Direct callers must retain their actual soft-gate, fallback, cleanup, and ownership behavior even when helper results are unavailable.
- Durations may be presented as suggested defaults, but exact caller deadlines, finite caps, and ownership consequences must be visible.

#### Decision branches

- If a source branch is shared with legacy but required to form or consume a v2 request, include it as v2 business policy.
- If a code path is pure logging or internal polling with no observable effect, list it as deliberately excluded rather than creating a scenario.
- If a branch belongs to 02-05, report it to main model with exact evidence.

#### Tools and commands

- Use `rg`, `Get-Content`, read-only scripts, and `apply_patch`.
- Validate with a local Given/When/Then parser, architecture-term scan, and `git diff --check`.
- Do not run tests/builds, edit production source, stage, commit, reset, clean, or spawn agents.

#### Deliverables

- Revised owned features and optional new 08.
- Branch-level source-to-scenario mapping for all inspected public policy/protocol/caller surfaces.
- Validation results and explicit deliberate exclusions.

#### Completion report

Return outcome, exact files changed, missing/inaccurate behaviors corrected, mapping, validation, risks, and concurrent changes preserved.

#### Escalate to the main model when

- Source implies an observable behavior that cannot be expressed without another owner's file.
- Caller behavior contradicts a core/platform scenario.
- The source boundary expands materially beyond the listed production consumers.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

### W2 — State, orchestration, persistence, and golden contract owner

**Model:** Sol Ultra, honoring the user's explicit model request
**Wave:** 1
**Depends on:** Published delegation plan
**Status:** Pending

#### Objective

Prove and, where needed, complete the regenerative contract for command/state ordering, ownership, cancellation, snapshots, persistence compatibility, candidate eligibility, filtering, recovery ordering, and golden health in features 02-03.

#### Why this assignment exists

These rules preserve the user's desktop and recovery data across implementation changes. Small omissions in mutation boundaries, stale completions, filtering, rotation, or known persistence limitations can create severe clean-room regressions.

#### Inputs and established facts

- Current 02 has 55 scenarios and current 03 has 62.
- Primary source: `types.h`, `runtime_support.h`, `async_dispatcher.*`, `state_machine.*`, `snapshot.*`, `snapshot_codec.*`, `golden_health.*`, and `staged_settings.h`.
- `operations.*` may be inspected for state/persistence consequences, but W3 owns platform Apply/recovery execution in 04-05.
- The contract must preserve actual non-guarantees and failure mapping rather than infer ideal transactional or atomic behavior.

#### Scope and ownership

- Primary files: `behaviors/display_engine_2/scenarios/02_state_and_command_orchestration.feature` and `03_snapshots_and_golden_baselines.feature`.
- May inspect all v2 core/platform source, helper entrypoint, client integration, persistence files, and existing tests as secondary evidence.
- Must not modify 01, 04-08, production source, plans, Git state, tests, or build artifacts.
- Concurrent overlap: W1 owns public callers; E1 read-only. Report platform/caller gaps to main model.

#### Required implementation or production steps

1. Enumerate every command/message/event/status transition and every conditional handler branch in the state machine; map it to a scenario or patch the omission.
2. Verify precedence among Apply, Revert, Disarm, Reset, Refresh, Snapshot, Export, Stop, heartbeat, display events, active mutations, deferred intents, and stale completions.
3. Verify cancellation before/after possible mutation, durable recovery retention, staged-state cleanup, verification ownership, transient disconnect settlement, stabilization, reset/reapply, HDR fallback, and terminal exit behavior.
4. Audit persistence format versions, parsing/defaulting, semantic equality, topology canonicalization, layout/rotation compatibility, exclusion updates, filtering, virtual/physical identity rules, invalid/empty/corrupt data, and partial-write/replacement limitations.
5. Audit Current/Previous/Golden capture, rotation, replacement, deletion, retry, candidate order, cooldown, pending thresholds, health recording, stale reporting, confirmation, and cleanup behavior.
6. State source defaults and exact caps where behaviorally meaningful; allow duration tuning only while preserving fixed ordering, attempts, cancellation, and outcomes.
7. Remove any class/generation/callback/queue/storage-primitive wording that is not itself observable; retain logical request/session ownership.
8. Validate 02-03 structurally, causally, for unique titles/outlines, architecture wording, and whitespace.

#### Required behavior and edge cases

- A stale completion may never answer or transition a replacement request, but its possible desktop mutation must still preserve recovery responsibility.
- Reset must remain an ordered barrier and must not erase an in-flight mutation's safety outcome.
- Snapshot compatibility and recovery eligibility remain separate; parseable data is not automatically safe to restore.
- Exclusion/filtering must not silently manufacture a partial topology or misclassify active virtual displays.
- Persistence failures and concurrent-reader limitations must retain their actual caller-visible consequences.
- Golden health metadata never manufactures eligibility or restoration success.

#### Decision branches

- If a behavior changes Windows mutation/verification rather than state/persistence ownership, report it for W3.
- If a helper/client reply behavior is missing, report it for W1.
- If code only implements internal scheduling granularity under an already documented deadline/cancellation envelope, document the envelope rather than the mechanism.

#### Tools and commands

- Use `rg`, `Get-Content`, read-only scripts, and `apply_patch`.
- Validate with structural/causal Gherkin checks, terminology scan, and `git diff --check`.
- Do not run tests/builds, edit source, stage, commit, reset, clean, or spawn agents.

#### Deliverables

- Revised 02-03 if evidence requires changes.
- Complete source-branch-to-scenario map for state and persistence.
- Validation and deliberate-exclusion report.

#### Completion report

Return outcome, exact changes, mapping, validation, limitations preserved, cross-owner findings, and concurrent changes.

#### Escalate to the main model when

- Source contradicts an existing scenario or exposes an ownership boundary ambiguity.
- A required correction belongs to another feature owner.
- A persistence behavior cannot be stated without prescribing a mechanism.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

### W3 — Windows Apply, recovery, event, and workaround contract owner

**Model:** Sol Ultra, honoring the user's explicit model request
**Wave:** 2
**Depends on:** E1 source map
**Status:** Pending

#### Objective

Prove and complete the regenerative behavior contract for Windows topology/configuration, verification, rollback, recovery, display events, virtual-display repair/reset, durable restore, shell/HDR workarounds, and all platform-visible timing/cancellation behavior in features 04-05.

#### Why this assignment exists

Windows compatibility workarounds and recovery edges are the most regression-prone part of a clean-room rewrite. The contract must state the trigger, ordering, visible result, failure behavior, skip/no-op condition, and cancellation boundary without naming the implementation mechanism.

#### Inputs and established facts

- Current 04 has 38 scenarios and current 05 has 48.
- Primary source: `operations.*`, `win_display_settings.*`, `win_event_pump.*`, `win_scheduled_task_manager.*`, `win_platform_workarounds.*`, `win_virtual_display_driver.h`, plus relevant state-machine call sites and client/platform callers routed by E1.
- Existing scenarios already cover major attempts, stable/quiet confirmation, event windows, virtual reset/identity, durable restore, shell refresh, HDR blanking, positions, duplicate groups, refresh tolerance, and physical refresh restoration.
- The job is to challenge completeness and accuracy branch by branch, not to restate those summaries.

#### Scope and ownership

- Primary files: `behaviors/display_engine_2/scenarios/04_apply_topology_and_configuration.feature` and `05_recovery_events_watchdog_and_platform.feature`.
- May inspect all v2 source, integration/callers, and tests as secondary evidence.
- Must not modify 01-03 or 06-08, production source, plans, Git state, tests, or build artifacts.
- Concurrent overlap: E1 supplies map; W1/W2 own other features. Report cross-domain gaps to main model.

#### Required implementation or production steps

1. Enumerate every Apply/Verification/Recovery/RecoveryValidation branch and status mapping, including pre-mutation protection, topology planning/activation/readiness, settings order, best-effort work, rollback, retry/reset/HDR fallback, and final confirmation.
2. Audit target resolution and duplicate-group scope under Windows-adjusted topology, missing targets, omitted target/original-primary semantics, primary selection, HDR absence, equivalent rational rates, legacy decimal tolerance, and layout/rotation recovery-only behavior.
3. Audit monitor position clamping/retries, unknown dimensions, identity preservation, cancellation, physical refresh restoration, and failure effect on core Apply.
4. Audit every display event mapping, debounce/coalescing, ownership, retry-window reopening, heartbeat/disconnect interaction, same/changed virtual identity, virtual driver reset/re-enable/cooldown, and cancellation during waits.
5. Audit durable restore creation targeting, safe no-user fallback, logon/battery behavior, creation/deletion/presence failures, idempotence, cleanup timing, and helper restore-startup outcomes.
6. Audit shell refresh and temporary HDR blanking triggers, serialization/non-overlap, best-effort failure, cleanup, and stale/cancelled/no-op behavior.
7. Add or correct scenarios for every evidenced gap; express current durations as tunable defaults and preserve fixed attempt caps/order/outcomes.
8. Validate 04-05 structurally, causally, for unique titles/outlines, architecture wording, and whitespace.

#### Required behavior and edge cases

- A Windows API success is insufficient unless the intended target becomes usable and required settings verify.
- An OS-adjusted topology may be accepted only under the exact intended-target rules; no unrelated display may substitute.
- A possible mutation keeps recovery armed until confirmed recovery, safe disarm, verified replacement, or explicit reset owns the desktop.
- Best-effort position, physical-refresh, shell, and HDR workaround failures must have their exact source-defined effect on the core result.
- A stale event, delayed retry, cancelled wait, or previous virtual identity cannot mutate a newer session.
- Durable task failure can never be interpreted as safe restoration.

#### Decision branches

- If E1 finds a Windows behavior outside the primary files, inspect it and either document the observable result in 04/05 or report why another owner must handle it.
- If source behavior is hardware/API uncertainty rather than a guarantee, write the uncertainty and safe result explicitly.
- If a detail is only an API choice or polling cadence, exclude it after confirming the enclosing observable boundary is covered.

#### Tools and commands

- Use `rg`, `Get-Content`, read-only scripts, and `apply_patch`.
- Validate with structural/causal Gherkin checks, terminology scan, and `git diff --check`.
- Do not run tests/builds, edit source, stage, commit, reset, clean, or spawn agents.

#### Deliverables

- Revised 04-05 if evidence requires changes.
- Complete Windows branch/workaround-to-scenario map.
- Validation and deliberate-exclusion report.

#### Completion report

Return outcome, exact changes, mapping, validation, defaults/caps, limitations, cross-owner findings, and concurrent changes.

#### Escalate to the main model when

- Current source contradicts the contract or E1 map.
- A required correction belongs to another owner.
- A workaround has no stable observable consequence and should be deliberately excluded.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

## 6. Main-model integration and validation

### Integration procedure

1. Receive E1's complete source-universe map and route each unowned branch to W1, W2, or W3 with exact evidence.
2. Inspect every worker diff against production source; reject invented guarantees, omitted failure branches, contradictory defaults, or structural prescriptions.
3. Build a final source-family matrix covering config/public inputs, request policy, helper protocol, client integration, core state/persistence, Windows platform, and every direct lifecycle consumer.
4. Reconcile cross-feature vocabulary, request/session attribution, timing tunability, retry caps, result categories, mutation/recovery ownership, and maintenance/public API behavior.
5. Personally inspect `src/display_device.cpp`, all public integration functions, every direct caller list returned by E1, and every Windows workaround before declaring closure.

### Validation procedure

1. Read-only Python structural scan of all `.feature` files — exactly one Feature, unique scenario titles, Given/When/Then, Outline/Examples, and no causal phase regression.
2. Branch/source inventory comparison using `rg` and a recorded source-family matrix — no behavior-changing production branch family left unowned.
3. Timing/default inventory scan across source and Gherkin — all observable defaults/caps represented, durations marked tunable, fixed logical limits preserved.
4. Architecture-prescription scan for class/thread/queue/callback/mutex/atomic/pipe/dispatcher/storage-primitive wording, followed by manual classification of intentional public terms.
5. Contradiction/duplication scan and manual scenario review — mutually exclusive outcomes are separate scenarios/outlines and no scenario idealizes a source limitation.
6. `git diff --check` and task-worktree status — documentation-only, whitespace-clean, no source/test/build changes.

### Rework threshold

Return work to its owner for any unowned business branch, inaccurate source claim, omitted caller consequence, timing without cancellation/terminal behavior, retry without exact cap/order, Windows workaround without trigger/failure/skip behavior, persistence guarantee stronger than source, cross-feature contradiction, invalid Gherkin, or implementation prescription. The main model may make only small wording, causal-structure, and integration-record edits directly.

## 7. Execution amendments

### A1 — 2026-08-03T23:18:00-05:00 — Direct-caller timing and cleanup evidence routed

- Evidence: Main-model caller inspection found deferred virtual-output lease/unlock behavior and an 8-second pending-Apply-before-capture-retarget allowance in `config.cpp`; recent-Apply capture-device retries and stale-name avoidance in `video.cpp`; app-termination revert skip/defer rules in `process.cpp`; and pause/hotkey/virtual-cleanup restore ordering across `stream.cpp`, `misc.cpp`, and `virtual_display_cleanup.cpp`.
- Original assumption: W1's broad direct-caller inventory was sufficient without these exact branches named up front.
- Impact: No ownership or wave change. These branches remain W1-owned public/lifecycle behavior and are candidate gaps in feature 06 or the new policy feature.
- Action: Routed exact evidence to W1 for source verification and scenario coverage; E1 remains responsible for confirming whether any additional caller family exists.
- User authority: This is within the requested exhaustive clean-room business-logic audit.

### A2 — 2026-08-03T23:23:00-05:00 — Configuration-policy boundary expanded

- Evidence: Direct inspection of `config.cpp`/`config.h` found display defaults and invalid-value fallbacks, Windows 10 virtual-display default suppression, an explicit scale allowlist, permanent-display count compatibility, snapshot-exclusion input forms, restore-hotkey parsing/modifiers, ignored legacy HDR-toggle keys, and session-override allowlisting that are not represented by the current seven features.
- Original assumption: The uncovered shared policy was primarily `display_device.cpp` parsing and mode remapping.
- Impact: W1's optional feature 08 is now expected to own both persisted/runtime configuration policy and configuration-to-request derivation; caller lifecycle remains in feature 06.
- Action: Routed exact defaults, accepted forms, fallback semantics, and compatibility behavior to W1. Main-model final validation will treat this configuration surface as required regenerative business logic.
- User authority: The user explicitly requires all business logic necessary to regenerate the software, not only helper internals.

### A3 - 2026-08-03T23:45:00-05:00 - Reachable behavior separated from dormant code

- Evidence: The explorer and main-model source audit found that production watchdog supervision uses a default 5-second active interval and 20-second suspended interval, while the standalone 10-second watchdog path is not a production caller. The apparent 3-second virtual re-enable cooldown and its explicit reset helper are also dormant; the live helper v2 reset policy instead uses a distinct default 30-second retry cooldown.
- Original assumption: Existing scenarios treated both compiled helper utilities as active production behavior.
- Impact: Features 05-07 must describe only reachable behavior. The dormant 3-second cooldown is removed, the live 30-second reset policy is retained, and watchdog defaults are corrected to 5/20 seconds.
- Action: Routed the liveness corrections to W1 and W3 and made reachable call sites a required final-closure check.
- User authority: Regenerative documentation must reproduce current business behavior, not preserve unused implementation artifacts as false requirements.

### A4 - 2026-08-03T23:48:00-05:00 - Bounded compatibility and platform non-guarantees retained

- Evidence: IPC inspection found a 64-request issued-ID memory and a 32-response unmatched inbox, so stale-response rejection is bounded on an as-yet-unknown protocol. Ping probes establish accepted connection/send, not an acknowledged response. Windows recovery inspection also found that final recovery validation omits layout/rotation comparison and that a final transient-disconnect repair may be accepted without a following observation.
- Original assumption: Existing wording implied absolute stale-response isolation, round-trip liveness, and stronger post-recovery verification than production provides.
- Impact: The suite must expose these limits so a clean-room rewrite does not silently strengthen or depend on guarantees absent from the current product.
- Action: Routed protocol limits to W1 and Windows recovery limits to W3; final validation will reject absolute wording contradicted by these bounded behaviors.
- User authority: The user explicitly requested all business logic and procedures, including failure, retry, timing, and workaround behavior.

### A5 - 2026-08-04T00:13:00-05:00 - Shared state and reachable configuration quirks added

- Evidence: The finished persistence audit found ordered independent startup adoption, a newest-16 managed-virtual identity cap enforced only on genuinely new additions, case-insensitive no-rewrite matching, cross-root scope limits, and several replacement/non-rollback limitations. Main-model config review then found reset-before-parse behavior, a reachable zero-second paused-cleanup fallback despite a 7200-second object initializer, lowercase-only Boolean truth words plus literal-1 acceptance, permissive integer conversion, and nontransactional remapping failures.
- Original assumption: Product object initializers and intended parser comments were treated as reachable defaults, and generic configuration parsing was assumed to reject malformed numeric or differently cased Boolean text cleanly.
- Impact: Features 01, 03, and 08 now preserve the observable runtime values and failure boundaries rather than intended-but-unreached defaults or idealized parsing.
- Action: Added shared-state retention and failure scenarios, corrected paused cleanup/scale/remapping behavior, and documented Boolean, integer, hot-reload, and startup exception compatibility quirks.
- User authority: A clean-room regeneration must reproduce source-visible behavior without relying on folklore or inferred intent.

### A6 - 2026-08-04T01:05:00-05:00 - Protocol and capture guarantees corrected to their real boundary

- Evidence: The adversarial public-boundary review found that the helper protocol retains partial framed data across timeout returns but drains only immediately available bytes after its first partial read; Apply extension members are extracted in a fixed semantic order independent of JSON text order; integer extensions narrow rather than range-fail; and the core Apply JSON dependency accepts every JSON number category before conversion. It also found that initial capture, reinitialization, and synchronous capture-open outer cycles have no fixed attempt cap even though each individual reopen invocation uses a two- or five-attempt profile.
- Original assumption: Several scenarios treated an inner attempt group as the terminal caller loop and described range/timeout behavior more strictly than the current implementations provide.
- Impact: Features 01, 06, 07, and 08 now preserve exact wire names and schemas, buffering behavior, compatibility conversions, fixed extraction order, per-invocation caps, outer-loop shutdown ownership, and the narrow cancellation publication boundary.
- Action: Added exact core and extension JSON member contracts, Snapshot/Revert metadata names, SFR2 marker bytes, unsigned correlation rules, bare-hex hotkey compatibility, and the true bounded-versus-unbounded capture hierarchy.
- User authority: Regeneration requires a future peer and capture lifecycle to interoperate with current behavior, including awkward compatibility and non-guarantees.

### A7 - 2026-08-04T01:20:00-05:00 - Dormant Windows policy removed from the product contract

- Evidence: Repo-wide production search proved that no production path produces `NeedsVirtualDisplayReset`; its control-handle reset path, 500/1000-millisecond waits, and 30-second reset cooldown were consumers of an unreachable status. The explicit 3-second virtual re-enable helper, a standalone 10-second watchdog path, task-presence query, and virtual-availability probe likewise have no production caller.
- Original assumption: Compiled policy utilities were treated as active product behavior merely because their consumers exist in source.
- Impact: Features 01, 02, 04, and 05 no longer tell a clean-room implementation to recreate dead behavior or expose producerless status outcomes.
- Action: Removed the dormant status/result/reset scenarios and retained independently reachable virtual-identity event verification, retargeting, and owned virtual-display recreation behavior.
- User authority: Regenerative documentation describes the running product boundary, not unreachable implementation inventory.

### A8 - 2026-08-04T01:28:00-05:00 - Windows limitations and workaround ordering made source-faithful

- Evidence: Windows review found that failed durable-safeguard creation does not stop mutation, candidate recovery does not refresh that safeguard, a no-user logon trigger remains unscoped while group-logon registration uses SID S-1-5-32-545, the executable path query may report a fixed-capacity truncated path, HDR workaround launch failure is outside its internal best-effort handling, and an omitted display target selects the first accepted group containing any planned original-primary candidate.
- Original assumption: The suite occasionally upgraded recovery intent into guaranteed durable registration, treated every HDR failure as harmless, and preserved a stronger duplicate-group identity than accepted topology ordering provides.
- Impact: Features 04 and 05 now retain recovery even without a task, qualify durable persistence on actual creation/existence, preserve the exact no-user and executable-path limitations, distinguish HDR launch from in-operation failure, and state accepted-group ordering explicitly.
- Action: Harmonized all recovery, cancellation, task, HDR, and omitted-target scenarios with reachable Windows behavior.
- User authority: Windows workarounds, retries, cancellation, and failure limitations were explicitly required for clean-room parity.

### A9 - 2026-08-04T01:35:00-05:00 - Final production-boundary matrix closed

| Production behavior family | Principal current evidence | Regenerative contract |
|---|---|---|
| Product defaults, configuration parsing, runtime overlays, mode policy, output identity | `src/config.*`, `src/display_device.*`, request helpers, builder | 06, 07, 08 |
| Helper entry, engine selection, singleton/control lifecycle, frame protocol, request/result compatibility | helper entry tool, helper paths, framed transport, display-settings client | 01, 06, 07 |
| Request/session ownership, command precedence, cancellation, verification, heartbeat and event transitions | v2 types, runtime support, state machine, asynchronous operation dispatch | 02, 05 |
| Snapshot/state persistence, filtering, rotation, compatibility, golden health and candidate policy | snapshot/codec/health/staged state plus shared state-file consumers | 01, 02, 03, 06 |
| Topology planning, activation, target resolution, settings, rollback and verification | v2 operations and Windows display-settings adapter, including consumed display-device semantics | 04 |
| Recovery, Windows events, durable safeguard, shell refresh, HDR blanking and virtual identity repair | recovery operations, event source, scheduled safeguard, platform workarounds and virtual identity facade | 02, 04, 05 |
| Helper readiness, deadlines, deferral, supervision, fallback and result attribution | integration, coordinator, session deferral, watchdog and display-settings client | 01, 06, 07 |
| RTSP, WebRTC, capture, process, configuration, hotkey, cleanup and shutdown consequences | direct production callers in config, HTTP/RTSP, process, stream, video, WebRTC, hotkey, cleanup and main shutdown | 06, 08 |
| Authenticated maintenance APIs, enumeration, EDID, golden status/comparison/deletion | configuration HTTP routes and integration query functions | 06 |

- Deliberate non-behavioral exclusions: internal class/process/thread/synchronization shape; storage and Windows API choices; cancellation polling slices; pure logging/diagnostic cadence; and build/test/package mechanics.
- Deliberate scope exclusions: virtual-display driver internals beyond the host-visible identity/control results consumed here; legacy helper internals beyond v2 selection and compatibility outcomes; generic streaming/encoding/input/application behavior without a display decision; and generic HTTP/authentication infrastructure beyond display routes.
- Deliberate dead-code exclusions: producerless virtual-reset status/control-handle policy, dormant 3-second re-enable and standalone 10-second watchdog utilities, unused task-presence/driver-availability queries, and any exact post-Apply topology guarantee not made by production.
- Closure statement: every reachable branch in the defined production boundary that changes accepted input, public result, target choice, display mutation, ownership, persistence, recovery obligation, retry/cancellation outcome, workaround, or caller action is owned by at least one scenario. Future source drift remains the only unbounded completeness risk.

### A10 - 2026-08-04T01:45:00-05:00 - Cross-feature contract contradictions closed

- Evidence: The final independent integration read found four wording conflicts across otherwise source-correct domains: wire-invalid Apply rejection was conflated with an already-dispatched configuration-less Apply; a core verification invariant was stronger than the caller's documented failed/unknown soft gate; Golden-health fallback named source order instead of observable asymmetry; and Golden status called an unreadable first-existing candidate eligible.
- Impact: Features 01, 02, 03, and 06 could have directed a future generator toward mutually inconsistent behavior even though each underlying source branch was represented.
- Action: Separated wire rejection from state-engine validation, narrowed verification publication without removing nonverified caller fallback, and corrected both observable titles. The independent integration reviewer then reported `integrated-contract-closed`.
- User authority: A regenerative contract must be mutually consistent across boundaries, not merely complete within each source domain.

## 8. Outcomes

| ID | Result | Deliverables | Validation | Follow-up |
|---|---|---|---|---|
| E1 | Completed | Complete production source universe and direct-caller map, delivered incrementally and consolidated in A9 | Read-only source and caller inventory reconciled against every scenario family | No unowned reachable source family remains |
| W1 | Completed | Revised 01, 06, and 07 and added 08; 420 scenarios across protocol, configuration, caller lifecycle, public APIs, capture, and compatibility | Independent Sol Ultra adversarial re-review is source-closed | Future source drift only |
| W2 | Completed | Revised 02-03; 184 scenarios covering state ordering, persistence compatibility, recovery candidate consequences, shared display state, and golden health | Independent Sol Ultra adversarial re-review is source-closed | Future source drift only |
| W3 | Completed | Revised 04-05; 99 scenarios covering reachable Windows Apply, verification, recovery, events, durable restore, virtual identity, and workarounds | Independent Sol Ultra adversarial re-review is source-closed, including cross-file dormant-status audit | Future source drift only |

### Final integration status

- Status: source-closed against the defined current production boundary; final repository handoff follows the static checks below.
- Validation evidence: 8 feature files; 703 globally unique scenarios including 112 outlines with matching Examples; explicit Given/When/Then and monotonic causal phases; UTF-8 and final-newline checks; no trailing whitespace; no implementation-prescriptive terminology; no known dormant/idealized requirements; documentation-only diff; no relevant source drift between the task base and current `unverified`.
- Remaining risks: absolute completeness cannot be mathematically proved and future production changes can drift from the contract. This audit mitigates current risk through a full source-family inventory, three disjoint source reviews, three independent adversarial reviews, and final cross-feature contradiction review.
- Delivered artifacts: eight `.feature` contracts under `behaviors/display_engine_2/scenarios` plus this delegation, source-boundary, exclusion, and validation record.
