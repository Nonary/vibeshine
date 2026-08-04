# Delegation plan: Display engine v2 Gherkin behavior specification

- Created: 2026-08-03T00:00:00-05:00
- Canonical plan: D:/sources/worktrees/display-engine-v2-scenarios/.codex/delegation-plans/20260803-000000-display-engine-v2-gherkin.md
- Requested outcome: Create a complete, behavior-focused Gherkin specification for the Windows display engine v2 under `behaviors/display_engine_2/scenarios`.
- Main-model status: planning complete; execution pending
- Default worker model: Terra High (user-requested)
- Explorer model: Terra Medium (Luna is unavailable in this runtime; read-only explorer role retained)
- Maximum active subagents: 3 (four runtime slots less this coordinating agent)

## 1. Outcome and completion criteria

### Outcome

The task worktree will contain a coherent set of UTF-8 `.feature` files that specify every observable display engine v2 behavior currently represented by its helper entry point, commands, state machine, display operations, persisted baselines, event/watchdog recovery, and Windows-facing support. The specifications will state required outcomes and failure handling, not prescribe internal class/thread/file architecture.

### Complete when

- `behaviors/display_engine_2/scenarios` contains the five scoped `.feature` files listed in this plan.
- Every engine-v2 command, request option, completion/reply, recovery path, persisted baseline tier, topology/configuration behavior, and event-driven path discovered in the named source scope is covered by at least one scenario.
- Scenarios use the agreed Gherkin style: one behavior area per file; `@display-engine-v2` tag; `Feature`, behavior-facing `Rule` sections where helpful, and concrete `Scenario`/`Given`/`When`/`Then`/`And` steps. No implementation class names, thread names, or algorithm instructions unless essential to an externally required behavior.
- The combined scenarios contain no duplicate ownership conflict, invalid Gherkin structure, merge markers, or whitespace errors, and the main-model coverage review finds no uncovered item in the source inventory.

## 2. Main-model inspection findings

- Workspace or system state: the primary checkout at `D:/sources/sunshine` is clean but currently on `unverified` at `44e3796b564baf71865344bf2da7388d269bf996`. It is not modified. The task checkout is `D:/sources/worktrees/display-engine-v2-scenarios`, branch `codex/display-engine-v2-scenarios`, at the identical base and clean when created.
- Applicable instructions: repository `AGENTS.md` requires task worktrees for new source changes, source-only validation unless a build is explicitly requested, and prohibits unit-test creation/modification/runs. No build was requested.
- Existing Gherkin convention: there are no repository `.feature` files or `behaviors` directory. This plan therefore establishes a minimal standard Gherkin layout rather than copying an absent local convention.
- Current behavior: `tools/display_settings_helper_v2.cpp` runs the v2 named-pipe helper, parses commands and payloads, enters restore mode when requested, manages connection replacement, and forwards events/commands to `StateMachine`.
- Relevant paths and symbols:
  - `tools/display_settings_helper_v2.cpp` — protocol message types, command parsing, snapshot adoption, lifecycle, replies, reconnect/disconnect behavior.
  - `src/platform/windows/display_helper_v2/types.h` — command/request/result vocabulary and observable statuses.
  - `src/platform/windows/display_helper_v2/state_machine.cpp` / `.h` — command handling, apply/verification/recovery states, supersession, cancellation, restore leases, post-apply settling, watchdog and display-event behavior.
  - `src/platform/windows/display_helper_v2/operations.cpp` / `.h` — topology transition, apply policy, monitor/refresh overrides, verification, recovery tier selection and confirmation.
  - `src/platform/windows/display_helper_v2/snapshot.cpp`, `snapshot_codec.cpp`, `golden_health.cpp` — baseline capture/filtering, file compatibility, atomic persistence, golden health and fallback eligibility.
  - `src/platform/windows/display_helper_v2/win_display_settings.cpp` — Windows topology/configuration, settings, layout, HDR, and refresh-rate behavior.
  - `src/platform/windows/display_helper_v2/async_dispatcher.cpp`, `runtime_support.h`, `win_event_pump.cpp`, `win_scheduled_task_manager.cpp`, `win_platform_workarounds.cpp`, and `win_virtual_display_driver.h` — cancellation, timing, event, watchdog, durable-recovery, shell/HDR, and driver availability behavior.
  - `src/platform/windows/ipc/display_settings_client.cpp`, `src/platform/windows/display_helper_integration.h`, and `src_assets/common/assets/web/configs/settingsSchema.ts` — v2 consumer/configuration selection surface.
- Dependencies and consumers: Sunshine uses this helper across its display-settings IPC and stream display lifecycle; its v2/legacy selection is user-visible in the web settings schema.
- Existing work to preserve: no existing task-worktree edits; do not modify engine code, unit tests, packaging, settings UX, or the primary checkout.
- Uncertainties retained for execution: any protocol behavior not visible in the helper must be traced to its IPC client before a scenario is written. If a source behavior is purely diagnostic and has no required externally observable result, omit it rather than turning logging mechanics into a requirement.

## 3. High-level strategy

1. Establish a single plain-language Gherkin style and divide the engine by non-overlapping behavior domains, not implementation files.
2. Produce five independent scenario files: protocol/lifecycle; state and command orchestration; snapshots and golden baselines; apply/topology/configuration; recovery/events/watchdog/platform behavior.
3. Use the explorer to find cross-file behavior and route omissions to the owning writer without editing artifacts.
4. Main model integrates the files, reconciles terminology and overlap, performs a source-to-scenario coverage pass, and runs source-only structure/whitespace checks.

### Preserve

- The current engine v2 behavior as source of truth, including legacy-compatibility behavior exposed through v2.
- Failure, cancellation, timeout, retry, restore, and correlation semantics when they change what callers or users can observe.
- The distinction between a required behavior and a preferred internal implementation.

### Exclude

- Changes to C++ code, tests, build files, package artifacts, runtime configuration, or production data.
- Scenarios that prescribe classes, worker threads, scheduler primitives, storage APIs, or other nonessential architecture.
- Legacy-engine behavior except where v2 explicitly preserves or interoperates with it.

## 4. Delegation topology

| ID | Role | Model | Wave | Depends on | Shared-state notes | Status |
|---|---|---|---:|---|---|---|
| E1 | Read-only engine-v2 coverage explorer | Terra Medium | 1 | Plan publication | Reads all source; no edits | Pending |
| W1 | Helper protocol and session-lifecycle Gherkin writer | Terra High | 1 | Plan publication | Owns `01_helper_protocol_and_session_lifecycle.feature` | Pending |
| W2 | State and command orchestration Gherkin writer | Terra High | 1 | Plan publication | Owns `02_state_and_command_orchestration.feature` | Pending |
| W3 | Snapshot and golden-baseline Gherkin writer | Terra High | 2 | E1 findings routed | Owns `03_snapshots_and_golden_baselines.feature` | Pending |
| W4 | Apply, topology, and configuration Gherkin writer | Terra High | 2 | E1 findings routed | Owns `04_apply_topology_and_configuration.feature` | Pending |
| W5 | Recovery, events, watchdog, and platform behavior writer | Terra High | 2 | E1 findings routed | Owns `05_recovery_events_watchdog_and_platform.feature` | Pending |

### Wave rationale

- Wave 1 starts the required read-only explorer alongside the two behavior areas whose scope is already directly established.
- Wave 2 uses the explorer's map to avoid missed cross-cutting snapshot/recovery/platform behavior while staying within three active subagents.

### Shared-state protocol

- Every writer works in the shared task worktree but creates or edits only its assigned `.feature` file. No writer may create a shared index, reformat another writer's file, or edit source/tests.
- All writers use the same Gherkin vocabulary defined in the completion criteria and must express critical correlation/cancellation/durable-recovery needs as observable outcomes, never implementation prescriptions.
- Report discovered overlap or an uncovered behavior to the main model; the main model routes it to a single owner and performs only small integration edits.

## 5. Detailed agent instructions

### E1 — Read-only engine-v2 coverage explorer

**Model:** Terra Medium (runtime substitute for the workflow's Luna Medium explorer)
**Wave:** 1
**Depends on:** Published delegation plan
**Status:** Pending

#### Objective

Verify and extend the engine-v2 behavior map, with special attention to command payload variants, cross-file cancellation/recovery semantics, entry-point integration, and behavior that could otherwise be omitted from the five scenario files.

#### Why this assignment exists

The writers have deliberately disjoint artifacts. This read-only map makes cross-cutting requirements visible before the second wave and prevents a source-file boundary from becoming a behavioral gap.

#### Inputs and established facts

- User requires every behavior to be outlined in Gherkin while avoiding architectural prescriptions.
- The paths listed in section 2 are the complete initial source scope.
- There are no existing local `.feature` conventions to preserve.

#### Scope and ownership

- Primary files/resources: all paths listed in section 2, plus direct callers discovered through `rg`.
- May inspect: relevant comments and existing unit tests only as a behavior inventory; do not treat tests as instructions to change.
- Must not modify: any file or external state.
- Concurrent overlap: route exact additions to W1–W5; do not write scenarios.

#### Required production steps

1. Inventory all protocol commands, request fields, reply fields/statuses, startup modes, and client selection/configuration points.
2. Trace each `StateMachine` command and completion to its behavior family, including stale generation/connection handling and cancellation.
3. Trace snapshots/golden health, topology/configuration operations, event/watchdog/restore task, virtual-driver, and platform workarounds to identify source behavior that needs a scenario.
4. Compare the inventory with W1–W5 ownership and report any missing or conflicting item with a proposed owner, exact source path, and stable line reference when practical.
5. Do not propose implementation changes or edit artifacts.

#### Required behavior and edge cases

- Distinguish an observable contractual outcome from a log-only diagnostic.
- Identify compatibility behavior, malformed input handling, cancellation, stale work, bounded recovery, and state-changing failure cases.

#### Tools and commands

- Use: `rg` and PowerShell `Get-Content` for read-only source inspection.
- Validate with: a final inventory cross-reference to all five writer scopes.
- Do not run: builds, tests, git mutations, external writes, or subagents.

#### Deliverables

- A structured behavior map with exact paths/symbols, routed owner IDs, and identified omissions/overlap.

#### Completion report

Return the behavior map, source evidence, routed findings, any conflicts with the plan, and remaining uncertainty.

#### Escalate to the main model when

- A behavior cannot be classified without expanding beyond the named engine-v2 scope.
- The source contradicts an established plan fact.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

### W1 — Helper protocol and session-lifecycle Gherkin writer

**Model:** Terra High (user-requested)
**Wave:** 1
**Depends on:** Published delegation plan
**Status:** Pending

#### Objective

Create `behaviors/display_engine_2/scenarios/01_helper_protocol_and_session_lifecycle.feature`, specifying the helper's externally observable startup, command protocol, reply correlation, connection, and shutdown behavior.

#### Why this assignment exists

The helper is the engine's control boundary. Its acceptance behavior must be unambiguous without dictating how named pipes, queues, or responders are implemented.

#### Inputs and established facts

- `tools/display_settings_helper_v2.cpp` defines `Apply`, `Revert`, `Reset`, `ExportGolden`, `Disarm`, `SnapshotCurrent`, `RefreshRate`, `Ping`, `LogLevel`, and `Stop`, along with correlated result message types.
- `src/platform/windows/display_helper_v2/types.h` defines request fields and caller-observable status/result data.
- `src/platform/windows/ipc/display_settings_client.cpp`, `src/platform/windows/display_helper_integration.h`, and `src_assets/common/assets/web/configs/settingsSchema.ts` identify consumer and engine-selection behavior.

#### Scope and ownership

- Primary file: `behaviors/display_engine_2/scenarios/01_helper_protocol_and_session_lifecycle.feature`.
- May inspect: helper, IPC client/integration, relevant types, and E1 findings.
- Must not modify: source, tests, settings schema, or any other scenario file.
- Concurrent overlap: W2 owns state consequences after an accepted command; this file owns ingress, validation, correlation, session replacement, and replies.

#### Required production steps

1. Create the assigned UTF-8 `.feature` file with `@display-engine-v2`, a clear `Feature`, and `Rule` sections only when they make outcomes clearer.
2. Cover normal and restore-mode startup; accepted command frames; malformed/unsupported/incomplete payload behavior; configuration and log-level validation; and command-specific immediate replies where present.
3. Cover Apply/verification/snapshot/refresh-rate correlation, including request IDs and connection epochs so late results cannot be delivered to a replacement client.
4. Cover ping, stop, reconnect, broken/error connection behavior, grace handling, replacement control intent precedence, and snapshot adoption/validation at startup where these are externally meaningful.
5. Mention legacy engine selection only as user-observable configuration behavior, not UI implementation.
6. Check every scenario has Given/When/Then semantics and no internal class, lock, thread, or method names.

#### Required behavior and edge cases

- A malformed or invalid command must not mutate the session and must receive the defined failure outcome where a reply exists.
- A stale connection's command or completion must not affect or reply through the current connection.
- A newly connected control client must cancel the pending disconnect grace instead of triggering restoration from an earlier disconnect.
- A stop request must end the helper cleanly after required cleanup behavior.

#### Tools and commands

- Use: `rg`, `Get-Content`, and `apply_patch` only for the assigned scenario file.
- Validate with: `git diff --check` and a manual scan that every scenario uses valid Gherkin keywords.
- Do not run: builds, unit tests, source edits, or subagents.

#### Deliverables

- The assigned `.feature` file and a self-validation report naming source areas covered.

#### Completion report

Return the outcome, changed file, behavior families covered, validation result, remaining ambiguities, and concurrent changes preserved.

#### Escalate to the main model when

- A protocol behavior appears to require a scenario owned by W2–W5.
- Required response semantics are not determinable from helper/client source.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

### W2 — State and command orchestration Gherkin writer

**Model:** Terra High (user-requested)
**Wave:** 1
**Depends on:** Published delegation plan
**Status:** Pending

#### Objective

Create `behaviors/display_engine_2/scenarios/02_state_and_command_orchestration.feature`, specifying v2 command effects, state progression, supersession, cancellation, verification, deferred mutations, and response-gate behavior.

#### Why this assignment exists

The central behavioral contract is the engine's sequencing: new client intent must produce the correct stateful outcome even while earlier display work is in progress.

#### Inputs and established facts

- `state_machine.cpp` handles every command and completion, state progression, stale generation/connection checks, deferred mutations, stabilizing verification, transient disconnect settlement, and callbacks.
- `runtime_support.h` exposes the bounded scheduler, heartbeat, debouncing, cancellation, and reconnect semantics used by this behavior.
- W1 owns frame parsing and pipe lifecycle; W3–W5 own detailed storage, display, and platform outcomes.

#### Scope and ownership

- Primary file: `behaviors/display_engine_2/scenarios/02_state_and_command_orchestration.feature`.
- May inspect: `state_machine.*`, `types.h`, `runtime_support.h`, `async_dispatcher.*`, and E1 findings.
- Must not modify: source/tests/other scenario files.
- Concurrent overlap: express only high-level apply/recovery outcomes needed to define orchestration; W3/W4/W5 own their detailed behavior.

#### Required production steps

1. Create the assigned Gherkin file using the shared style.
2. Specify Apply admission, replacement/supersession, invalid/stale command rejection, generation-aware cancellation, and the ordering barrier/deferred command outcomes.
3. Specify apply status handling: success/initial verification, retryable behavior, virtual-display-reset policy, fatal handling, and required preservation/recovery when a mutation may already have changed the display.
4. Specify post-apply stabilization and the capture-gated first-video behavior, including success, failure, bounded repair, and a request that omits the final initial HDR reapply.
5. Specify Revert, Disarm, Reset, Export Golden, Snapshot Current, Refresh Rate, Ping, and Stop command effects at the stateful level, deferring their storage/Windows-specific detail to their owners.
6. Specify that stale asynchronous completions, stale callbacks, and superseded connection work cannot transition the current session or open a newer request's gate.
7. Self-check for behavior language rather than state-machine implementation prose.

#### Required behavior and edge cases

- Only one display-mutating intent may control the active session at a time; later replacement intent has defined precedence and is eventually honored.
- Cancellation must prevent obsolete delayed work from producing a current-session result.
- A failure after possible display mutation must retain or enter recovery until a safe terminal outcome is confirmed.
- A client-visible completion must be associated with the originating request/session, never merely the most recent one.

#### Tools and commands

- Use: `rg`, `Get-Content`, `apply_patch` only for assigned file.
- Validate with: `git diff --check` and manual Gherkin/coverage scan.
- Do not run: builds, tests, source edits, or subagents.

#### Deliverables

- The assigned `.feature` file and mapped source behavior inventory.

#### Completion report

Return the outcome, changed file, covered transitions/edge cases, validation, risks, and preserved concurrent work.

#### Escalate to the main model when

- A detailed behavior belongs uniquely to W1/W3/W4/W5 or conflicts with their scope.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

### W3 — Snapshot and golden-baseline Gherkin writer

**Model:** Terra High (user-requested)
**Wave:** 2
**Depends on:** E1 findings routed by main model
**Status:** Pending

#### Objective

Create `behaviors/display_engine_2/scenarios/03_snapshots_and_golden_baselines.feature`, specifying captured display baselines, filtering, version compatibility, persistence guarantees, tier selection, and golden-health behavior.

#### Why this assignment exists

The restore contract depends on trustworthy persisted display state. The scenarios must define which snapshots are eligible and what is retained or discarded after success/failure without exposing serialization implementation.

#### Inputs and established facts

- `snapshot.cpp`, `snapshot_codec.cpp`, `snapshot.h`, `golden_health.cpp`, and relevant sections of `operations.cpp` govern this domain.
- Existing invariants include order-independent topology equivalence; compatibility with pre-layout snapshots; virtual-display and exclusion filtering; preservation of a previous baseline until a new current snapshot is known good; atomic on-disk updates; and golden health gating.
- E1 findings may add exact behavior or route adjustment.

#### Scope and ownership

- Primary file: `behaviors/display_engine_2/scenarios/03_snapshots_and_golden_baselines.feature`.
- May inspect: snapshot/codec/golden source and relevant tests as inventory evidence.
- Must not modify: source/tests/other scenario files.
- Concurrent overlap: W4 owns applying topology/settings; W5 owns recovery-window/event trigger behavior. This file owns selection/eligibility/persistence of the baseline itself.

#### Required production steps

1. Create the assigned Gherkin file using the shared style.
2. Cover capture of current, previous, and golden baselines; current-to-previous rotation only after a valid replacement capture; and explicit snapshot/export commands at behavior level.
3. Cover filtering: virtual display exclusion, caller-provided case-insensitive exclusions, absent non-excluded devices, inactive-but-connected physical devices, and removal of excluded data from all relevant baseline fields.
4. Cover structural validity, canonical equivalence, identity/layout/HDR/mode/origin semantics, and backward-compatible snapshots that lack layout/origin metadata.
5. Cover unreadable/malformed/structurally invalid snapshot behavior, atomic persistence expectation, and startup adoption only of valid session snapshots.
6. Cover golden health recording/clearing, when unhealthy golden data is skipped, and cleanup of session snapshots after confirmed golden restoration.
7. Verify every scenario describes observable baseline behavior, not JSON schemas, helper functions, or filesystem-call mechanics.

#### Required behavior and edge cases

- A capture/filter failure must not destroy the last known valid restore baseline.
- A baseline containing an active virtual display must not become a physical-desktop restore target.
- A missing, non-excluded baseline device must invalidate that restore candidate; an explicitly excluded missing device may be removed from it.
- Golden restoration must not repeatedly use a known unhealthy golden snapshot until its health is cleared by valid behavior.

#### Tools and commands

- Use: `rg`, `Get-Content`, and `apply_patch` only for assigned file.
- Validate with: `git diff --check` and Gherkin structure review.
- Do not run: builds, tests, source edits, or subagents.

#### Deliverables

- The assigned `.feature` file and source-to-scenario coverage notes.

#### Completion report

Return the outcome, changed file, covered baseline behavior, validation, ambiguity, and preserved concurrent work.

#### Escalate to the main model when

- Recovery tier ordering or event timing cannot be stated without overlapping W5.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

### W4 — Apply, topology, and configuration Gherkin writer

**Model:** Terra High (user-requested)
**Wave:** 2
**Depends on:** E1 findings routed by main model
**Status:** Pending

#### Objective

Create `behaviors/display_engine_2/scenarios/04_apply_topology_and_configuration.feature`, specifying valid display-configuration application, topology activation/adjustment, mode/HDR/layout/position/refresh behavior, verification, retries, and rollback requirements.

#### Why this assignment exists

This file defines what a successful or failed display change means to the caller, including Windows-adjusted topologies, while avoiding demands about SettingsManager calls or display API sequencing.

#### Inputs and established facts

- `operations.cpp` and `win_display_settings.cpp` implement the apply/topology/verification behavior.
- `types.h` distinguishes explicit-device targets from an omitted device ID that represents the original primary duplicate group.
- The engine validates topology, may recover the display stack and retry, preserves a baseline before mutation, supports monitor positions and refresh-rate overrides, and verifies configuration/topology/HDR/layout afterward.

#### Scope and ownership

- Primary file: `behaviors/display_engine_2/scenarios/04_apply_topology_and_configuration.feature`.
- May inspect: operations, Windows display settings, types/interfaces, and E1 findings.
- Must not modify: source/tests/other scenarios.
- Concurrent overlap: W2 owns stateful scheduling/reply gating; W3 owns storage eligibility; W5 owns recovery/event/task triggers. Write only the detailed display-change behavior.

#### Required production steps

1. Create the assigned Gherkin file using the shared style.
2. Cover configuration-only, topology-only, combined, and no-op requests, including invalid requests and topology structural/OS validation failure.
3. Cover topology activation attempts, bounded retry after display-stack recovery, usable OS-adjusted topology acceptance, and rejection if the requested target does not survive adjustment.
4. Cover explicit device targeting and omitted-device primary duplicate-group semantics, ensuring Windows reordering cannot silently retarget the request to an arbitrary display.
5. Cover baseline preservation/rollback after failed mutation; durable recovery protection when mutation may have changed the desktop; virtual-display reset request behavior; and cancellation before/within settling steps.
6. Cover mode, HDR blanking/reapplication, position clamping/reposition eligibility, rotation/layout restore and verification, and physical refresh-rate overrides/validation.
7. Cover verification success/failure and the distinction between an accepted request and a configuration actually confirmed active.

#### Required behavior and edge cases

- A structurally invalid or OS-invalid topology must not be activated as a successful display change.
- A failed settings stage after a captured baseline must restore that baseline when possible and report the appropriate non-success outcome.
- A device position outside the usable range must be constrained safely; an inactive/non-repositionable device must not be repositioned.
- A requested configuration must be verified against the intended explicit display or complete primary duplicate group, including HDR/layout/refresh requirements that apply.

#### Tools and commands

- Use: `rg`, `Get-Content`, `apply_patch` only for assigned file.
- Validate with: `git diff --check` and manual Gherkin review.
- Do not run: builds, tests, source edits, or subagents.

#### Deliverables

- The assigned `.feature` file and source-coverage notes.

#### Completion report

Return the outcome, changed file, apply behavior covered, validation, uncertainty, and preserved concurrent work.

#### Escalate to the main model when

- A scenario requires recovery scheduling, snapshot eligibility, or command ordering owned by another writer.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

### W5 — Recovery, events, watchdog, and platform behavior writer

**Model:** Terra High (user-requested)
**Wave:** 2
**Depends on:** E1 findings routed by main model
**Status:** Pending

#### Objective

Create `behaviors/display_engine_2/scenarios/05_recovery_events_watchdog_and_platform.feature`, specifying autonomous recovery, bounded retry windows, event/watchdog responses, durable restore behavior, virtual-display monitoring, and user-visible platform workarounds.

#### Why this assignment exists

This domain protects the physical desktop when a stream or helper becomes unhealthy. It requires exhaustive failure-path scenarios, but the specification must not prescribe timers, task-scheduler APIs, worker loops, or implementation threading.

#### Inputs and established facts

- `operations.cpp` recovery operations, `state_machine.cpp` event handling, `runtime_support.h`, `async_dispatcher.cpp`, `win_event_pump.cpp`, `win_scheduled_task_manager.cpp`, `win_platform_workarounds.cpp`, and `win_virtual_display_driver.h` implement this behavior.
- Restore behavior has a primary recovery window, event-driven reopening, progressive backoff, heartbeat optional/miss/recovery windows, restore-on-disconnect policy, and cancellation-aware delayed work.
- E1 findings may refine route/ownership.

#### Scope and ownership

- Primary file: `behaviors/display_engine_2/scenarios/05_recovery_events_watchdog_and_platform.feature`.
- May inspect: listed recovery/event/platform sources and direct source callers.
- Must not modify: source/tests/other scenarios.
- Concurrent overlap: W2 owns generic state command sequencing; W3 owns baseline content/eligibility; W4 owns actual display-change semantics. This file owns the trigger, bounded retry, safety, and autonomous-recovery outcomes.

#### Required production steps

1. Create the assigned Gherkin file using the shared style.
2. Cover explicit revert, disconnected client, heartbeat timeout, restore-mode startup, display-change/power-resume/device-arrival/device-removal events, and virtual-display failure as recovery triggers where source supports each.
3. Cover restore-on-disconnect opt-out, reconnection/grace behavior, transient post-apply disconnect settlement, and the rule that an active/recent display mutation retains recovery protection rather than prematurely abandoning it.
4. Cover bounded primary and event recovery windows, progressive retry/backoff, quiet-period/confirmation requirements, cancellation, window exhaustion, and the next-event reopening behavior.
5. Cover durable restore task presence/creation/removal/cleanup behavior as the observable safety guarantee around a potentially changed desktop, including failure to arm it.
6. Cover virtual-display availability/reset/retargeting behavior and platform-visible HDR/shell recovery effects only when a source path establishes them.
7. Cover successful recovery validation, failed recovery validation, session-snapshot cleanup after golden recovery, and terminal safe state behavior.

#### Required behavior and edge cases

- The engine must not restore a deliberately pause-retained stream solely because its connection broke.
- A recovery attempt canceled or superseded by current session intent must not later alter the desktop, while any already-mutated display remains protected for safe restoration.
- Recovery attempts must stop after their allowed window and wait for an eligible new trigger rather than retry forever.
- A durable recovery safeguard must be removed only after restoration/disarm is safely confirmed; failure to establish it must be reflected in recovery behavior.

#### Tools and commands

- Use: `rg`, `Get-Content`, and `apply_patch` only for assigned file.
- Validate with: `git diff --check` and Gherkin structure review.
- Do not run: builds, tests, source edits, or subagents.

#### Deliverables

- The assigned `.feature` file and coverage notes.

#### Completion report

Return the outcome, changed file, recovery/event behavior covered, validation, ambiguity, and preserved concurrent work.

#### Escalate to the main model when

- A behavior requires detailed protocol/state/storage/apply language owned by another writer.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

## 6. Main-model integration and validation

### Integration procedure

1. Inspect E1's inventory and route its findings to W3–W5 before wave 2; amend this plan if ownership or scope materially changes.
2. Inspect every produced `.feature` file, reconcile duplicated semantics and terminology, and add only small missing integration wording directly when no writer ownership remains.
3. Cross-reference the combined Gherkin files against the command/request/result/types inventory and every engine-v2 source module in section 2.

### Validation procedure

1. `git diff --check` — proves the added specifications have no whitespace errors.
2. PowerShell scan of `behaviors/display_engine_2/scenarios/*.feature` — proves all five planned files exist, each has a feature declaration, scenarios, and no conflict markers.
3. `rg` source-to-scenario coverage review — proves every identified behavior family has an owning scenario. No build or unit test is permitted or needed.

### Rework threshold

Return work to its owner when a source behavior family is omitted, scenarios prescribe internal architecture, an ownership boundary is violated, malformed Gherkin is introduced, or a required failure/cancellation/recovery outcome is absent. The main model may perform only terminology harmonization, ordering, and small unowned gap fixes directly.

## 7. Execution amendments

### A1 — 2026-08-03T00:00:00-05:00 — Explorer runtime substitution

- Evidence: the available subagent models are Terra and Sol; Luna is not callable.
- Original assumption: the delegation workflow template names Luna Medium for the mandatory read-only explorer.
- Impact: E1 uses Terra Medium rather than Luna Medium; its read-only scope, deliverables, and no-delegation constraint are unchanged.
- Action: dispatch E1 as Terra Medium and retain Terra High for every writing worker, as the user requested.
- User authority: the user explicitly requested Terra High parallel workers and authorized broad delegated scenario work.

### A2 — 2026-08-03T00:00:00-05:00 — Integration split for mutually exclusive outcomes

- Evidence: integration review found three scenarios that used `But when` to express a second, mutually exclusive precondition; W4 had completed and could not be reactivated because the runtime agent-thread limit was reached.
- Original assumption: each writer's self-check would catch semantic branch mixing as well as keyword structure.
- Impact: six narrow scenarios were split into distinct runnable paths; no source scope, behavior ownership, or user-visible requirement changed.
- Action: W1 and W3 corrected their own files. The main model performed the remaining small W4/W5 integration splits: adjusted-topology target survival/loss, rollback confirmation/loss, verification match/mismatch, and restore startup with/without a baseline.
- User authority: the changes are required to deliver valid, specific Gherkin for the requested behavior inventory.

## 8. Outcomes

| ID | Result | Deliverables | Validation | Follow-up |
|---|---|---|---|---|
| E1 | Complete | Structured behavior map covering W1-W5, including protocol validation, stale-work gating, recovery-lease safety, baseline eligibility, topology targeting, and bounded recovery | Inspected all planned source areas; no files changed, builds, or tests | Findings routed to W1/W2 and embedded in W3-W5 dispatch briefs |
| W1 | Complete | `01_helper_protocol_and_session_lifecycle.feature` — 26 scenarios covering all helper commands, startup, protocol validation, correlation, reconnection, and shutdown | Writer self-check plus integration correction of Scenario Outline and mutually exclusive branches | Integrated |
| W2 | Complete | `02_state_and_command_orchestration.feature` — 34 scenarios covering command ordering, cancellation, stale work, response gates, stabilization, and command effects | Writer self-check and main structural scan | Integrated |
| W3 | Complete | `03_snapshots_and_golden_baselines.feature` — 26 scenarios covering baseline capture/filtering/compatibility/persistence and golden health | Writer self-check plus integration split of alternate branches | Integrated |
| W4 | Complete | `04_apply_topology_and_configuration.feature` — 23 scenarios covering apply shapes, target preservation, rollback/cancellation, presentation details, and verification | Writer self-check plus main split of adjusted-topology/rollback/verification branches | Integrated |
| W5 | Complete | `05_recovery_events_watchdog_and_platform.feature` — 21 scenarios covering recovery triggers, bounded attempts, durable safety, virtual display, shell, and HDR behavior | Writer self-check plus main split of restore-startup branches | Integrated |

### Final integration status

- Status: complete
- Validation evidence: five `.feature` files and 130 unique scenario titles; each scenario has Given/When/Then; `Examples` attaches only to Scenario Outline; no mutually branching `But when`, implementation-prescriptive terminology, conflict markers, tabs, or whitespace errors; `git diff --check` passed.
- Remaining risks: Scenarios are a source-derived behavioral specification, not live Windows display validation; source code, tests, builds, and packaging were intentionally not changed or run.
- Delivered artifacts: `behaviors/display_engine_2/scenarios/01_helper_protocol_and_session_lifecycle.feature`, `02_state_and_command_orchestration.feature`, `03_snapshots_and_golden_baselines.feature`, `04_apply_topology_and_configuration.feature`, and `05_recovery_events_watchdog_and_platform.feature`.
