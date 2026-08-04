# Delegation plan: Exhaustive display engine v2 behavior audit

- Created: 2026-08-03T20:45:00-05:00
- Canonical plan: D:/sources/worktrees/display-engine-v2-scenarios/.codex/delegation-plans/20260803-204500-display-engine-v2-exhaustive-audit.md
- Requested outcome: Audit every observable display engine v2 and integration behavior in source, then expand the Gherkin scenarios until each required outcome and procedure is represented at a high business-behavior level.
- Main-model status: planning complete; execution pending
- Default worker model: Terra High (continuing the user's requested Terra High delegation)
- Explorer model: Terra Medium (Luna is unavailable in this runtime; read-only explorer role retained)
- Maximum active subagents: 3 (four runtime slots less this coordinator)

## 1. Outcome and completion criteria

### Outcome

`behaviors/display_engine_2/scenarios` will be an exhaustive source-derived Gherkin specification of display engine v2: the helper, its caller/integration lifecycle, commands and state transitions, baseline persistence, display application, recovery, and platform safety. It will use business outcomes and required procedures, never prescribe implementation mechanisms merely because the source happens to use them.

### Complete when

- The existing five `.feature` files have been audited and expanded for every source-derived behavior in their assigned domain.
- A new `06_client_integration_and_stream_lifecycle.feature` covers the display-helper caller, builder, helper-launch, connection, deferred-apply, stream-capture gate, and watchdog behavior that was outside the first helper-only feature.
- Every public command, request option, completion/status, failure/cancellation/timeout path, persistence tier, display state, recovery trigger, and integration-facing procedure found in the source inventory is represented by a concrete scenario or scenario outline.
- Each scenario names a single runnable business condition and outcome. It may require bounded, safe, cancellable, correlated, or durable behavior when that matters, but it must not demand classes, threads, locks, pipe implementations, filesystem APIs, Windows API calls, or hard-coded internal algorithm steps.
- The main-model coverage matrix maps every inspected entry point and source behavior family to a scenario file; structural Gherkin and whitespace checks pass.

## 2. Main-model inspection findings

- Workspace state: `D:/sources/worktrees/display-engine-v2-scenarios` is clean on `codex/display-engine-v2-scenarios` at `8ef44dfca`. The primary `D:/sources/sunshine` checkout remains on `unverified` and is not to be touched.
- Applicable instructions: repository `AGENTS.md` requires worktree isolation, source-only validation unless a build is explicitly requested, and prohibits creating/modifying/running unit tests. The user requested documentation only; no build, package, or runtime test is authorized.
- Existing artifact: five committed scenario files currently hold 130 scenarios. They cover the helper core, state machine, snapshots, apply/topology, and recovery but omit major caller-side behavior in `display_helper_integration.cpp` and `display_settings_client.cpp`.
- Source of truth scope:
  - `tools/display_settings_helper_v2.cpp` — helper startup, flags, payload parsing, message acceptance, connection replacement, and callback replies.
  - `src/platform/windows/display_helper_v2/types.h`, `state_machine.cpp`, `runtime_support.h`, and `async_dispatcher.cpp` — requests, results, session state, cancellation, deadlines, sequencing, event and watchdog reactions.
  - `src/platform/windows/display_helper_v2/operations.cpp`, `win_display_settings.cpp`, and `interfaces.h` — topology/configuration application and verification, monitor position, HDR, layout, refresh, rollback, and recovery operations.
  - `src/platform/windows/display_helper_v2/snapshot.cpp`, `snapshot_codec.cpp`, and `golden_health.cpp` — baseline capture, compatibility/filtering/persistence, recovery-tier policy, and recurring golden failure health.
  - `src/platform/windows/display_helper_v2/win_event_pump.cpp`, `win_scheduled_task_manager.cpp`, `win_platform_workarounds.cpp`, and `win_virtual_display_driver.h` — display-event, durable recovery, shell/HDR, and virtual-display safety behavior.
  - `src/display_helper_builder.h`, `src/platform/windows/display_helper_integration.h/.cpp`, and `src/platform/windows/ipc/display_settings_client.cpp` — request construction, engine selection, helper process lifecycle, IPC protocol compatibility, bounded caller waits, session/readiness deferral, stream capture gating, and helper watchdog behavior.
  - `src_assets/common/assets/web/configs/settingsSchema.ts` — user-selectable automatic/v2/legacy engine choices.
- Direct evidence of the omitted client behavior: `display_helper_integration.cpp` contains deferred-resolution apply handling, helper-start failure cooldown, user-session/system-context behavior, bounded stream-start fallback, verification tickets, and watchdog restart/stop rules. `display_settings_client.cpp` owns response protocol detection, response buffering, connection retirement, timeout/cancellation, and legacy/v2 compatibility.
- Existing work to preserve: all prior scenario wording that is source-correct, the five-file organization, and the committed documentation. Do not remove a scenario merely because a more detailed one is added; consolidate only true duplication.
- Uncertainties retained for execution: a source behavior that is solely logging/diagnostic data is not a business requirement and must not produce a scenario. If a behavior is visible only as a literal timing constant but has no user/caller consequence, state it as a bounded deadline rather than copying its number.

## 3. High-level strategy

1. Trace the complete control path from request construction through helper launch/IPC, engine execution, display mutation, recovery, and stream lifecycle; treat source behavior as authoritative.
2. Give each behavior domain one writer and one file owner so corrections do not collide. Add the missing caller/integration feature as a separate specification rather than hiding it in helper internals.
3. Make each writer reconcile its source symbols against its current scenarios, add specific missing outcomes, split ambiguous alternatives, and remove only duplicate or implementation-prescriptive wording from its owned files.
4. Main model merges the independent expansions, performs an explicit source-entrypoint-to-scenario coverage audit, runs structural checks, and commits the documentation-only delta if complete.

### Preserve

- Existing Gherkin format: `@display-engine-v2`, one feature area per file, meaningful `Rule` sections, and concrete Given/When/Then scenarios.
- Compatibility behavior for legacy clients/engine selection when v2 source explicitly supports it.
- Safety properties: no stale response, no unprotected uncertain desktop, no infinite retry, and no lost user/session intent.

### Exclude

- C++ implementation, tests, build/package changes, runtime configuration changes, or external operations.
- Unit-test execution or creation.
- Literal implementation directives such as which APIs, thread primitives, synchronization objects, queues, serialization calls, or task-scheduler calls must be used.

## 4. Delegation topology

| ID | Role | Model | Wave | Depends on | Shared-state notes | Status |
|---|---|---:|---:|---|---|---|
| E1 | Read-only exhaustive source-to-scenario explorer | Terra Medium | 1 | Plan publication | Reads all scope; no edits | Pending |
| W1 | Helper protocol/audit writer | Terra High | 1 | Plan publication | Owns only `01_helper_protocol_and_session_lifecycle.feature` | Pending |
| W2 | Client integration/stream lifecycle writer | Terra High | 1 | Plan publication | Owns new `06_client_integration_and_stream_lifecycle.feature` | Pending |
| W3 | State/orchestration audit writer | Terra High | 2 | E1 findings routed | Owns only `02_state_and_command_orchestration.feature` | Pending |
| W4 | Snapshot/golden audit writer | Terra High | 2 | E1 findings routed | Owns only `03_snapshots_and_golden_baselines.feature` | Pending |
| W5 | Display application/recovery/platform audit writer | Terra High | 2 | E1 findings routed | Owns only `04_apply_topology_and_configuration.feature` and `05_recovery_events_watchdog_and_platform.feature` | Pending |

### Wave rationale

- Wave 1 starts the mandatory source explorer plus the independent control-boundary writers. W2 closes the documented integration gap immediately.
- Wave 2 uses the explorer's complete map to add any uncovered engine behavior. It is dispatched as runtime capacity becomes available; never more than three subagents run at once.

### Shared-state protocol

- Writers may edit only their listed `.feature` file(s); W2 alone creates file 06. They must preserve all other files and the planning artifacts.
- Source files are read-only. Each writer performs its own static Gherkin check and reports exact source-to-scenario additions.
- E1 reports omissions to the main model only. The main model routes overlap, owns wording harmonization, and retains final coverage validation.
- Any scenario with alternative facts must become separate scenarios or a true Scenario Outline; it must not chain incompatible branches through a single `But when` sequence.

## 5. Detailed agent instructions

### E1 — Read-only exhaustive source-to-scenario explorer

**Model:** Terra Medium (runtime substitute for the workflow's Luna Medium explorer)
**Wave:** 1
**Depends on:** Published delegation plan
**Status:** Pending

#### Objective

Produce a complete traceable inventory of business-observable display-engine-v2 behavior and map each item to W1–W5 or identify it as a new coverage gap.

#### Why this assignment exists

The first scenario pass missed the caller/integration lifecycle. This explorer prevents another source boundary from being mistaken for an absence of requirements.

#### Inputs and established facts

- The source scope and existing feature ownership are listed in sections 2 and 4.
- The user requires exhaustive high-level behavior/procedure coverage, not code-design prescriptions.

#### Scope and ownership

- Primary resources: every path in section 2, current five features, and direct callers found with `rg`.
- Must not modify: any file, external state, or plan content.
- Concurrent overlap: report additions to a single W1–W5 owner; do not draft scenarios.

#### Required production steps

1. Enumerate every helper command, payload option, response/result, process/connection condition, and client-visible timeout/cancellation behavior.
2. Trace `display_helper_integration.cpp` from request builder to launch, IPC, fallback, deferred apply, verification/capture gating, revert/disarm/snapshot, and watchdog lifecycle.
3. Trace all `StateMachine` handlers, snapshot/golden paths, topology/settings behaviors, recovery paths, and Windows-support event/virtual driver effects.
4. Compare this inventory against all current feature files; return exact missing outcomes, duplicate/misplaced scenarios, source evidence, and the specific owner for each item.
5. Categorize purely diagnostic details as non-scenarios.

#### Required behavior and edge cases

- Treat a caller-facing deadline, cancellation, fallback restriction, retry exhaustion, or preserved recovery protection as behavioral when it changes the resulting operation or desktop safety.
- Do not elevate source implementation mechanics to requirements.

#### Tools and commands

- Use `rg` and `Get-Content` only.
- Validate with an inventory that includes every source module in section 2.
- Do not run builds/tests, edit, or spawn subagents.

#### Deliverables

- A source-to-scenario gap map with exact source paths/symbols, proposed owner IDs, and key business outcomes.

#### Completion report

Return the inventory, mapped omissions, routing, contradictory evidence, and residual uncertainties.

#### Escalate to the main model when

- A behavior crosses all existing ownership boundaries or source scope would need to expand beyond display engine v2.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

### W1 — Helper protocol and lifecycle exhaustive audit

**Model:** Terra High
**Wave:** 1
**Depends on:** Published delegation plan
**Status:** Pending

#### Objective

Audit and expand `behaviors/display_engine_2/scenarios/01_helper_protocol_and_session_lifecycle.feature` so it specifies every high-level helper-entry, control-frame, option-parsing, result-correlation, connection-replacement, and shutdown behavior in `tools/display_settings_helper_v2.cpp`.

#### Why this assignment exists

The helper is the engine's server-side boundary. It must describe caller-observable handling for every supported request and malformed/obsolete input without exposing pipe/queue implementation.

#### Inputs and established facts

- `tools/display_settings_helper_v2.cpp` defines startup flags, state-file exclusions, Apply and Revert payload parsing, all command handling, response callbacks, connection epochs, reconnect/disconnect behavior, and restore-mode adoption.
- Existing file 01 is the only owned artifact; W2 owns caller-side transport/lifecycle behavior.

#### Scope and ownership

- Primary file: `behaviors/display_engine_2/scenarios/01_helper_protocol_and_session_lifecycle.feature`.
- May inspect: helper source, `types.h`, config schema, direct client framing only to establish protocol contracts, and E1 findings.
- Must not modify: source/tests/other feature files/file 06.
- Concurrent overlap: describe server acceptance/outcome; W2 describes client launch, timeout, fallback, and stream lifecycle.

#### Required production steps

1. Build a checklist from every `MsgType`, helper flag, payload field, and callback result in the helper source.
2. Compare each item to file 01 and add one or more high-level scenarios for missing business outcomes.
3. Cover supported/invalid optional Apply data: configuration/topology, monitor placement, device refresh overrides, HDR toggle, virtual arrangement, baseline exclusions, golden preference, disconnect policy, and capture-gate option.
4. Cover Revert option variants, snapshot exclusion state-file fallback/override behavior, log-level defaults/limits, startup snapshot adoption/search behavior, malformed/unknown frames, result correlation, replacement connection isolation, and clean stop.
5. Retain behavior language: say the helper accepts, rejects, preserves, correlates, or exits; never say how it parses a particular JSON object or implements a pipe/session.
6. Ensure each scenario has one condition/outcome and self-validate Gherkin.

#### Required behavior and edge cases

- Invalid or incomplete input must not mutate the active session or impersonate a successful request.
- An optional field absent/invalid must use the source-defined safe/default behavior without corrupting unrelated request data.
- Obsolete session work must neither alter nor reply into the active session.
- Restore mode must act autonomously yet terminate with a defined safe outcome.

#### Tools and commands

- Use `rg`, `Get-Content`, and `apply_patch` only for file 01.
- Validate with `git diff --check` and a Gherkin keyword/outline scan.
- Do not run builds/tests, edit source or other features, or spawn agents.

#### Deliverables

- Revised file 01 and a source-symbol-to-scenario addition list.

#### Completion report

Return outcome, changed file, behavior additions, validation, exclusions, and preserved concurrent work.

#### Escalate to the main model when

- A required behavior is caller-side/integration-side (W2) or crosses into another owner.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

### W2 — Client integration and stream lifecycle writer

**Model:** Terra High
**Wave:** 1
**Depends on:** Published delegation plan
**Status:** Pending

#### Objective

Create `behaviors/display_engine_2/scenarios/06_client_integration_and_stream_lifecycle.feature` covering all caller-side business procedures of the display helper: request construction, engine choice and launch, communication compatibility/failure, user-session readiness, bounded apply/snapshot/revert work, stream capture gating, and helper watchdog recovery.

#### Why this assignment exists

The v2 engine's external behavior includes the Sunshine code that decides when and how to invoke it. This was absent from the first five helper-internal files and must be stated separately to avoid losing stream/startup safety requirements.

#### Inputs and established facts

- `src/display_helper_builder.h` defines skip/apply/revert intent, configuration, virtual arrangement, topology/position/refresh and active-session overrides.
- `src/platform/windows/display_helper_integration.h/.cpp` defines process lifecycle, engine choice, readiness/fallback, pending Apply, verification tickets/capture gate, snapshot/revert/disarm wrappers, and watchdog management.
- `src/platform/windows/ipc/display_settings_client.cpp` defines v2/legacy response compatibility, matching response buffering, cancellation/deadlines, connection retirement, and request APIs.

#### Scope and ownership

- Primary file: `behaviors/display_engine_2/scenarios/06_client_integration_and_stream_lifecycle.feature` (create it).
- May inspect: builder, integration, client, config schema, and direct stream callers; may read existing files to avoid duplication.
- Must not modify: source/tests/files 01–05 or planning documents.
- Concurrent overlap: cross-reference helper outcomes but do not repeat server frame parsing (W1), engine-state behavior (W3), or desktop mutation details (W5).

#### Required production steps

1. Inventory every public integration/client operation and each decision branch that changes caller, stream, or desktop behavior.
2. Create the feature using the agreed format and high-level terminology; use `Rule` sections for request construction, helper availability, client communication, deferred execution, capture gate, and watchdog lifecycle.
3. Specify engine auto/v2/legacy selection, launch/restart/single-instance/readiness behavior, launch failure cooldown, system-without-user-session safety, and when fallback is permitted or deliberately suppressed.
4. Specify request construction and dispatch for Skip/Apply/Revert, virtual arrangement/topology/overrides, virtual-display readiness, cancellation/deadlines, legacy/v2 response compatibility, correlation, bounded snapshot/refresh/revert/disarm operations, and connection retirement.
5. Specify deferred resolution Apply: queueing only under eligible user-session conditions, no orphaned work after session teardown, cancellation, bounded retry, successful adoption, and stop after retry exhaustion.
6. Specify verification ticket/capture-gate outcomes: verified, failed, or unavailable/timeout; no premature capture when required HDR/topology verification is unresolved; stream-start versus ordinary bounded behavior.
7. Specify watchdog start/reuse/restart/stop behavior across active, pending, replaced, stopped, and shutdown sessions without referring to worker implementation.
8. Validate all scenarios and report source coverage.

#### Required behavior and edge cases

- A request that cannot meet its caller deadline must return safely and must not enter an unbounded fallback path for a latency-sensitive stream start.
- A newer request/session cannot inherit a legacy or v2 reply intended for an earlier request.
- System-context/no-user-session behavior must defer eligible resolution work rather than apply to the wrong desktop or leave stale deferred work after teardown.
- Capture may advance only after the request's required verification level is reached; a compatible legacy/unknown result must be expressed explicitly rather than treated as verified.
- A watchdog stop belonging to an old session must not stop protection for a newer active stream.

#### Tools and commands

- Use `rg`, `Get-Content`, and `apply_patch` only for file 06.
- Validate with `git diff --check` and a manual Gherkin structure/duplication scan.
- Do not run builds/tests, source edits, or subagents.

#### Deliverables

- New file 06 and a source-entrypoint-to-scenario checklist.

#### Completion report

Return outcome, changed file, coverage additions, validation, unresolved source ambiguity, and concurrent work preserved.

#### Escalate to the main model when

- An integration behavior is actually engine-internal or requires a scenario-owned boundary revision.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

### W3 — State and orchestration exhaustive audit

**Model:** Terra High
**Wave:** 2
**Depends on:** E1 findings routed by the main model
**Status:** Pending

#### Objective

Audit and expand `behaviors/display_engine_2/scenarios/02_state_and_command_orchestration.feature` against every `StateMachine` handler and runtime support behavior that changes current session intent, command ordering, cancellation, verification, or observable recovery state.

#### Why this assignment exists

The state machine decides what happens when behaviors collide. Completeness requires every accepted command and completion to have a precise, high-level result even under supersession or failure.

#### Inputs and established facts

- `state_machine.cpp/.h`, `types.h`, `runtime_support.h`, and `async_dispatcher.cpp` are source of truth.
- W1 owns ingress framing; W2 owns caller lifecycle; W4 owns baseline content; W5 owns detailed desktop mutation and autonomous recovery behavior.

#### Scope and ownership

- Primary file: `behaviors/display_engine_2/scenarios/02_state_and_command_orchestration.feature`.
- May inspect: named source and E1 findings.
- Must not modify: source/tests/other feature files.
- Concurrent overlap: state-level procedure only; defer detailed baseline/display/recovery mechanics to owners.

#### Required production steps

1. Inventory every `handle_*` command/completion/event function and `handle_tick`, plus observable cancellation, debounce, grace, backoff, heartbeat, and result-callback effects.
2. Map each behavior to an existing scenario; add missing ones, especially default/override recovery policy, staged state cleanup, reset failure, command/barrier ordering, event state, no-candidate recovery, and terminal exit behavior.
3. State synchronization requirements only as outcomes: current intent wins, previous work becomes harmless, a needed outcome is correlated, and safety remains until confirmation.
4. Split alternative branches into distinct scenarios/valid outlines and remove duplicate prose.
5. Validate file 02 structurally and report coverage.

#### Required behavior and edge cases

- Every command/completion/event must either have a current-session effect or be safely ignored as obsolete.
- Timer/debounce/backoff behavior must be bounded and event-sensitive, not described via implementation scheduling.
- Failed/reset/cancelled work must not leave a future unrelated request with stale state or false verification.

#### Tools and commands

- Use `rg`, `Get-Content`, and `apply_patch` only for file 02.
- Validate with `git diff --check` and Gherkin checks.
- Do not run builds/tests, source edits, or subagents.

#### Deliverables

- Revised file 02 with source-handler coverage notes.

#### Completion report

Return outcome, changed file, additions, validation, risks, and preserved concurrent work.

#### Escalate to the main model when

- A scenario belongs to client, snapshot, apply, or recovery ownership rather than state orchestration.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

### W4 — Snapshot and golden-baseline exhaustive audit

**Model:** Terra High
**Wave:** 2
**Depends on:** E1 findings routed by the main model
**Status:** Pending

#### Objective

Audit and expand `behaviors/display_engine_2/scenarios/03_snapshots_and_golden_baselines.feature` against all snapshot, codec, persistence, filtering, and golden-health behavior.

#### Why this assignment exists

Restore safety is only as correct as baseline eligibility, lifetime, and recovery ordering. This audit catches procedures that are not merely data-format implementation.

#### Inputs and established facts

- `snapshot.cpp/.h`, `snapshot_codec.cpp/.h`, `golden_health.cpp/.h`, and applicable recovery operation paths define the domain.
- Existing file 03 already covers primary tiers/filtering/compatibility but must be reconciled against every public method and recurring failure procedure.

#### Scope and ownership

- Primary file: `behaviors/display_engine_2/scenarios/03_snapshots_and_golden_baselines.feature`.
- May inspect: named source, interfaces, relevant state/operation callers, E1 results.
- Must not modify: source/tests/other features.
- Concurrent overlap: baseline selection/eligibility/lifetime here; W5 owns actual desktop restoration; W3 owns command sequencing.

#### Required production steps

1. Inventory capture, validate, match, enumerate, layout capture, save/load/remove/existence, rotation, filtering, parse/serialize compatibility, and golden-health procedures.
2. Audit existing scenarios and add missing high-level outcomes: unchanged/corrupt/no-payload data, repeated stable reads, exclusion persistence/clearing, tier removal, session history promotion, golden preference/cooldown/fallback threshold, and health marker reset/recurring failure behavior when source establishes them.
3. State data integrity and restore eligibility outcomes without prescribing storage file/JSON primitives.
4. Ensure feature names remain business-facing and each scenario has a single fact pattern/outcome.
5. Validate and report exact additions.

#### Required behavior and edge cases

- Ineligible/corrupt/partial baselines cannot produce partial desktop restoration or erase a known-good candidate.
- A baseline change must preserve the last viable recovery route until its successor is demonstrably safe.
- Repeated golden failures must alter future eligibility/reporting as source dictates, while a confirmed/new golden baseline resets that condition.

#### Tools and commands

- Use `rg`, `Get-Content`, and `apply_patch` only for file 03.
- Validate with `git diff --check` and Gherkin checks.
- Do not run builds/tests, source edits, or subagents.

#### Deliverables

- Revised file 03 with source-method coverage notes.

#### Completion report

Return outcome, changed file, additions, validation, risks, and preserved work.

#### Escalate to the main model when

- A required condition is a detailed recovery operation or client policy owned elsewhere.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

### W5 — Display application, recovery, and platform exhaustive audit

**Model:** Terra High
**Wave:** 2
**Depends on:** E1 findings routed by the main model
**Status:** Pending

#### Objective

Audit and expand `04_apply_topology_and_configuration.feature` and `05_recovery_events_watchdog_and_platform.feature` until every source-derived display mutation, verification, restore, event, durable-protection, virtual-display, and platform-visible safety procedure is represented at the required high level.

#### Why this assignment exists

Applying and recovering the desktop are inseparable safety outcomes. One owner prevents a gap between failed mutation/rollback and the recovery behavior that follows it.

#### Inputs and established facts

- `operations.cpp/.h`, `win_display_settings.cpp/.h`, `interfaces.h`, `async_dispatcher.cpp`, `runtime_support.h`, `win_event_pump.cpp`, `win_scheduled_task_manager.cpp`, `win_platform_workarounds.cpp`, and `win_virtual_display_driver.h` define this behavior.
- File 04 currently covers ordinary apply/topology. File 05 covers major recovery triggers. Both need exhaustive source cross-checking.

#### Scope and ownership

- Primary files: `behaviors/display_engine_2/scenarios/04_apply_topology_and_configuration.feature` and `05_recovery_events_watchdog_and_platform.feature`.
- May inspect: named source, types, source callers, and E1 findings.
- Must not modify: source/tests/files 01–03/06.
- Concurrent overlap: W3 owns state ordering and W4 owns baseline eligibility; reference their outcomes but do not duplicate their detailed assertions.

#### Required production steps

1. Create a source checklist for topology transition, apply policy, apply/verify/recovery operations, Windows display settings, virtual driver, event pump, durable recovery, and platform workarounds.
2. Audit file 04 for every request shape and presentation property: no-op/configuration/topology variants, target resolution, structural/OS acceptance, adjusted target survival, retry/rollback/cancellation, staged state, mode/HDR/primary/origin/layout/refresh, and complete verification.
3. Audit file 05 for every recovery trigger and procedure: explicit/automatic/restoration startup/event/watchdog/virtual-driver cases, tier and candidate behavior as an outcome, grace/quiet/bounded retries/event reopen, cancellation, recovery validation, durable safeguard lifecycle, failure to establish safety, virtual reset/retarget, shell/HDR behavior, and safe terminal cleanup.
4. Add/sculpt scenarios only for externally meaningful behavior. State a requirement such as "protect before a possibly desktop-changing operation" rather than saying which operating-system call or task is used.
5. Split mutually exclusive results and remove duplicated cross-file language.
6. Validate both files and report source coverage.

#### Required behavior and edge cases

- No possibly changed desktop may be treated as safely restored, disarmed, or idle before confirmation.
- An adjusted display result must never silently apply settings to the wrong target.
- A canceled/timed-out/retry-exhausted operation must stop changing the desktop while preserving the source-defined recovery path.
- Event/virtual-device/platform reactions must be relevant to the active session and must not allow stale events to affect a newer desktop session.

#### Tools and commands

- Use `rg`, `Get-Content`, and `apply_patch` only for files 04–05.
- Validate with `git diff --check` and Gherkin checks for both owned files.
- Do not run builds/tests, source edits, or subagents.

#### Deliverables

- Revised files 04–05 and a source-method-to-scenario coverage list.

#### Completion report

Return outcome, changed files, additions, validation, ambiguity, and preserved concurrent work.

#### Escalate to the main model when

- A source behavior belongs only to caller integration, state orchestration, or snapshot eligibility.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

## 6. Main-model integration and validation

### Integration procedure

1. Receive E1's source-to-scenario map and route each gap to exactly one owner before wave 2.
2. Inspect all revisions and new file 06; reconcile duplicated behavior across server/helper, client/integration, state, persistence, display, and recovery layers.
3. Build a coverage matrix for all source modules, every request field/status/command, and all public integration operations, then add any small unowned Gherkin gap directly.
4. Reject scenarios that dictate implementation mechanics or leave mutually exclusive facts in a single scenario.

### Validation procedure

1. Python read-only Gherkin structural scan over `behaviors/display_engine_2/scenarios/*.feature` — proves exactly one Feature per file, valid Scenario/Scenario Outline and Examples ownership, and Given/When/Then in every scenario.
2. `git diff --check` — proves no whitespace errors.
3. `rg` source-to-scenario matrix review — proves every source module and behavior family in section 2 has an owning feature/scenario.
4. Search scenario text for implementation-prescriptive terms and manually inspect any hits — proves the specifications remain business-facing.

### Rework threshold

Return work to its owner for a missing source behavior, an incorrectly stated source outcome, a source implementation prescription, invalid Gherkin, true duplicate, or scenario with incompatible branches. The main model may make only minor wording/branch/coverage-matrix integration edits.

## 7. Execution amendments

- The source audit established that a standalone topology-only Apply is invalid; explicit topology is only a staging override of a configuration request.
- The source audit established that preserving previous history is best effort: a valid new current baseline remains usable if history promotion cannot be completed.
- The source audit established that capture verification is a bounded soft gate: verified, failed, and unavailable results are retained distinctly, while capture proceeds after the gate resolves.
- Independent review corrected source-level distinctions for adjusted topology acceptance, restore-mode outcomes, helper protocol ambiguity, helper/client ownership, HTTP capability probing, baseline persistence/filtering, recovery event handling, and golden fallback cleanup ordering.

### Independent validation wave

Before committing, three independent read-only reviewers will recheck the completed corpus against its owning source layers:

| ID | Scope | Required result |
|---|---|---|
| R1 | Files 01 and 06; helper protocol, builder, integration, IPC, and direct stream callers | Identify any missing public behavior, incorrect outcome, duplicate, or implementation prescription. |
| R2 | Files 02 and 03; state machine, runtime support, baseline persistence, and golden health | Identify any missing state transition, recovery/baseline outcome, duplicate, or implementation prescription. |
| R3 | Files 04 and 05; operations, Windows display behavior, virtual display, events, durable recovery, and workarounds | Identify any missing desktop/recovery outcome, incorrect result, duplicate, or implementation prescription. |

Each reviewer must remain read-only, cite the relevant source and scenario, and report either actionable gaps or an explicit clean result. The main model owns any final wording change and reruns the full corpus validation.

## 8. Outcomes

| ID | Result | Deliverables | Validation | Follow-up |
|---|---|---|---|---|
| E1 | Complete | Exhaustive source-to-scenario gap map covering helper, state, snapshots, display/recovery, client integration, stream gates, and watchdog | Read-only audit completed; identified topology-only, history-promotion, and soft-capture-gate corrections | Routed to W1-W5; W3 dispatched |
| W1 | Complete | Revised file 01 with 72 helper/protocol/lifecycle scenarios | Scenario structure and high-level wording self-checked | Integrated and independently corrected |
| W2 | Complete | New file 06 with 78 client/integration/stream scenarios | Scenario structure and soft-gate wording self-checked | Integrated |
| W3 | Complete | Revised file 02 with 46 state/command scenarios | Scenario structure and state-result coverage self-checked | Integrated |
| W4 | Complete | Revised file 03 with 44 snapshot/golden scenarios | Scenario structure and baseline-result coverage self-checked | Integrated and independently corrected |
| W5 | Complete | Revised files 04–05 with 28 and 31 display/recovery scenarios | Scenario structure and recovery-result coverage self-checked | Integrated and independently corrected |
| R1 | Complete | Independent review and recheck of files 01 and 06 | Corrected five findings plus the no-snapshot versus unconfirmed-candidate distinction | No remaining actionable mismatch |
| R2 | Complete | Independent review and recheck of files 02 and 03 | Corrected four snapshot persistence/filtering findings | No remaining actionable mismatch |
| R3 | Complete | Independent review and recheck of files 04 and 05 | Corrected four topology/recovery findings plus no-baseline restore exit | No remaining actionable mismatch |

### Final integration status

- Status: complete
- Validation evidence: six features, 299 scenarios, 28 Scenario Outlines and 28 Examples tables; independent source review and recheck of every feature, structural Gherkin scan, source-gap marker scan, `git diff --check`, and implementation-wording scan all passed.
- Remaining risks: Scenario specifications are source-derived and describe observable requirements; timing bounds remain qualitative where the code exposes an implementation-specific duration.
- Delivered artifacts: revised files 01–05 and new file 06 in `behaviors/display_engine_2/scenarios/`.
