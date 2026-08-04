# Delegation plan: display engine v2 clean-room Gherkin contract

- Created: 2026-08-03T21:38:17-05:00
- Canonical plan: D:\sources\worktrees\display-engine-v2-scenarios\.codex\delegation-plans\20260803-213817-display-engine-v2-clean-room-contract.md
- Requested outcome: Expand the display engine v2 Gherkin suite into a detailed, implementation-neutral clean-room behavior contract that preserves all source-defined observable behavior, including Windows workarounds, default timing/retry policies, cancellation, ordering, compatibility, and failure outcomes.
- Main-model status: planning complete; execution pending
- Default worker model: Terra High (explicit user request)
- Explorer model: Terra High (explicit user request overrides the skill's default explorer model)
- Maximum active subagents: 3 (four runtime slots including the main model)

## 1. Outcome and completion criteria

### Outcome

The scenario suite will let a new implementation reproduce display engine v2's external behavior and safety semantics without requiring its current architecture. It will state default timings and retry counts as recommended operational defaults while making clear which outcome, ordering, and safety properties must remain invariant if those values are tuned.

### Complete when

- Files 01 through 06 plus a new cross-cutting contract feature describe every reviewed source behavior family, public command, configuration/default, data compatibility rule, Windows workaround, retry/cancellation boundary, and caller-visible result.
- Every timing- or retry-sensitive behavior states its default envelope, trigger, terminal outcome, and the invariant that a clean-room implementation must retain when operational values are changed.
- Every Windows workaround is expressed as an observable compatibility/safety requirement, not a required API, process, thread, or internal object design.
- Every mutating request has a documented validation, pre-mutation safety, cancellation/supersession, retry, verification, and terminal/recovery behavior where the source defines one.
- All source-owned requirements can be traced to exactly one primary feature, with intentional cross-feature references only where caller behavior consumes a helper outcome.
- The combined suite passes structural Gherkin checks, duplicate-title checks, source-to-feature coverage review, `git diff --check`, and a wording scan that rejects implementation prescriptions.
- The documentation changes are committed in the existing task worktree. No source implementation, build, package, or unit-test change is made.

## 2. Main-model inspection findings

- Workspace state: `D:\sources\worktrees\display-engine-v2-scenarios` is clean on `codex/display-engine-v2-scenarios` at `d2de10a4d`; the primary checkout is on `unverified` and must not be changed.
- Applicable instructions: `D:\sources\sunshine\AGENTS.md`; documentation may be changed in this task worktree, no builds/tests are authorized, and eventual landing must preserve the primary `unverified` checkout.
- Current artifact: six Gherkin features contain 299 scenarios. They provide a behavior inventory but intentionally describe many timing and platform contracts only as "bounded" or "source-defined," which is insufficient to regenerate a compatible clean-room implementation.
- Current feature ownership:
  - `behaviors/display_engine_2/scenarios/01_helper_protocol_and_session_lifecycle.feature` - helper startup, protocol, requests, correlation, lifecycle.
  - `behaviors/display_engine_2/scenarios/02_state_and_command_orchestration.feature` - state transitions, command precedence, cancellation, and recovery state.
  - `behaviors/display_engine_2/scenarios/03_snapshots_and_golden_baselines.feature` - capture, filtering, persistence, recovery candidate selection, and golden health.
  - `behaviors/display_engine_2/scenarios/04_apply_topology_and_configuration.feature` - Apply validation, topology, configuration, verification, rollback, and display details.
  - `behaviors/display_engine_2/scenarios/05_recovery_events_watchdog_and_platform.feature` - recovery, Windows events, durable recovery, virtual display, watchdog, and platform behavior.
  - `behaviors/display_engine_2/scenarios/06_client_integration_and_stream_lifecycle.feature` - request builder, helper selection/lifecycle, IPC compatibility, deferred work, capture gates, callers, and watchdog integration.
- Source-of-truth paths and observed behavior:
  - `tools/display_settings_helper_v2.cpp` and `src/platform/windows/ipc/display_settings_client.cpp` define protocol frame alternatives, command types, request/reply correlation, compatibility behavior, and malformed-frame edge cases.
  - `src/display_helper_builder.h` and `src/platform/windows/display_helper_integration.cpp` define request construction, engine selection, caller deadlines, default control timing, deferred apply policy, helper restart/cooldown, watchdog lifecycle, and direct fallback restrictions.
  - `src/platform/windows/display_helper_v2/{types.h,async_dispatcher.*,state_machine.*,runtime_support.h,timing.h}` define public command/status vocabulary, precedence, cancellation/supersession, transition results, event/heartbeat ownership, and retry scheduling.
  - `src/platform/windows/display_helper_v2/{snapshot.*,snapshot_codec.*,golden_health.*}` define persistence payload compatibility, filtering, candidate ordering, best-effort history, golden health, and non-guarantees that a replacement must preserve.
  - `src/platform/windows/display_helper_v2/{operations.*,staged_settings.h,win_display_settings.*}` define Apply/verify/recovery sequences, Windows topology adjustment rules, target identity, rollback, HDR, refresh, positioning, layout, and recovery confirmation.
  - `src/platform/windows/display_helper_v2/{win_event_pump.*,win_scheduled_task_manager.*,win_virtual_display_driver.h,win_platform_workarounds.*}` define observable Windows event, restore-at-logon, virtual identity, shell refresh, and requested HDR blanking compatibility behavior.
  - `src/{nvhttp.cpp,stream.cpp,webrtc_stream.cpp,video.cpp}` consume helper outcomes for HTTP encoder probing, RTSP/WebRTC startup, soft capture gates, HDR/settle waits, capability queries, and stream teardown.
- Default operational values discovered in source include: 6 s topology readiness; 5 s normal helper readiness/ping; 250 ms shutdown-class ping; 150 ms prompt disarm budget/throttle; 5 s disarm grace; 2 s initial deferred apply wait; 500 ms base retry and settling delays; 10 s deferred retry cap; 30 s helper-start cooldown after two failures; six deferred-apply attempts; two topology attempts; three Apply attempts; 5 s topology activation; 200 ms monitor-reposition retry for up to 15 attempts; 2 s stable-read window; 150 ms stable sampling; 750 ms quiet confirmation; 700 ms recovery retry delay; and 3 s virtual-display re-enable cooldown.
- Existing work to preserve: all 299 source-derived scenarios, their correction history, and the preceding audit plan. Requirements must record source behavior including compatibility limits/non-guarantees; this task does not authorize silently changing an implementation defect into a new product guarantee.
- Uncertainties retained for execution: a worker must escalate if a timing value is internal-only with no observable outcome, rather than placing a literal implementation cadence into a scenario. The main model will decide whether it becomes a tunable default contract or is omitted as non-behavioral.

## 3. High-level strategy

1. Re-map every source behavior into a clean-room contract category: input schema/default, observable action, timing/retry envelope, cancellation/supersession priority, platform compatibility/workaround, validation, and terminal result.
2. Expand the helper/client boundary with an explicit cross-cutting compatibility feature and detailed request/reply/default-time behavior, while keeping serialization/transport design open.
3. Expand state/persistence behavior with explicit command ordering, retries, cancellation boundaries, baseline schema compatibility, and recovery candidate rules.
4. Expand Apply/recovery behavior with Windows-facing safety contracts, workaround triggers, default time/retry profiles, and stable terminal outcomes.
5. Integrate the three non-overlapping ownership areas, remove vague placeholders, validate every scenario and source mapping, and commit the complete Gherkin contract.

### Preserve

- The current source behavior as observed, including precise negative behavior and non-guarantees where a clean-room replacement must remain compatible.
- The feature split above; caller-visible behavior belongs in file 06, protocol behavior in file 01, state/persistence in files 02-03, and Windows display/recovery behavior in files 04-05.
- Implementation freedom: scenarios may require cancellation, a timely result, or stable/quiet confirmation, but may not prescribe a particular worker, queue, thread, API, file-write primitive, or IPC transport implementation.
- Flexible operational parameters: defaults and suggested ranges may be specified, but scenarios must state the invariant behavior if an implementation tunes the value.

### Exclude

- C++/Windows implementation changes, tests, builds, packages, release work, external publication, or landing into `unverified`.
- New functionality absent from the reviewed source.
- Requirements that equate current implementation mechanisms with product behavior.

## 4. Delegation topology

| ID | Role | Model | Wave | Depends on | Shared-state notes | Status |
|---|---|---|---:|---|---|---|
| E1 | Clean-room behavior explorer | Terra High | 1 | Plan publication | Read-only over all sources and features | Pending |
| W1 | Protocol, integration, and cross-cutting contract author | Terra High | 1 | Plan publication | Owns files 01, 06, and new file 07 only | Pending |
| W2 | State, timing, and persistence contract author | Terra High | 1 | Plan publication | Owns files 02-03 only | Pending |
| W3 | Windows display, recovery, and workaround contract author | Terra High | 2 | E1 findings | Owns files 04-05 only | Pending |

### Wave rationale

- Wave 1 uses the three available child slots: the mandatory read-only explorer finds source gaps while W1 and W2 expand independent feature groups.
- Wave 2 starts W3 after E1 reports the Windows-specific source map, avoiding a fourth concurrent child and ensuring every workaround has a named observable contract.

### Shared-state protocol

- All work occurs only in `D:\sources\worktrees\display-engine-v2-scenarios`; agents must check `git status --short` before and after work and preserve unrelated changes.
- E1 is read-only. W1 owns files 01, 06, and 07; W2 owns 02 and 03; W3 owns 04 and 05. No worker edits the plan, source files, or another worker's feature.
- Each worker uses `apply_patch` for feature edits and validates only its owned files. No worker stages, commits, builds, runs tests, creates worktrees, or invokes subagents.
- Any source behavior that belongs in another owner's file is reported to the main model with source symbol and proposed feature owner. The main model routes it without allowing concurrent cross-file edits.

## 5. Detailed agent instructions

### E1 - Clean-room behavior explorer

**Model:** Terra High (explicit user-requested model)
**Wave:** 1
**Depends on:** Published delegation plan
**Status:** Pending

#### Objective

Verify the source-to-feature map for a clean-room display engine v2 contract and identify every omitted observable behavior, default timing/retry policy, Windows workaround, data compatibility constraint, caller-visible status, and required ordering/cancellation rule.

#### Why this assignment exists

The existing corpus is broad but deliberately qualitative. The three authors need an independent, precise source map so their detailed Gherkin additions are source-derived rather than inferred.

#### Inputs and established facts

- User requires enough detail to regenerate the behavior from Gherkin while retaining implementation freedom.
- The source paths and default values listed in section 2 are the primary scope.
- Files 01-06 currently contain 299 scenarios; file 07 does not yet exist.

#### Scope and ownership

- May inspect: all paths listed in section 2, `behaviors/display_engine_2/scenarios/*.feature`, relevant `src/config.*`, and `src_assets/common/assets/web/configs/settingsSchema.ts` for public defaults.
- Must not modify: any file, Git state, external state, or plan.
- Concurrent overlap: W1-W2 may be editing their owned features; inspect current content but do not alter it.

#### Required production steps

1. Enumerate all externally meaningful request fields, commands, replies/statuses, settings/defaults, timing/retry values, and platform workaround triggers.
2. For each, identify the current feature/scenario owner or report an omission with exact source symbol and intended owner (W1, W2, or W3).
3. Classify each timing value as a tunable default contract, an observable deadline, or a non-behavioral implementation cadence; provide the outcome invariant for any tunable default.
4. Identify every Windows workaround and state its trigger, intended user-visible/safety result, and condition that prevents it from running.
5. Identify source-defined compatibility/non-guarantee behavior that a clean-room implementation must deliberately preserve or consciously version away.
6. Validate the map against all source modules; return a structured report only.

#### Required behavior and edge cases

- Do not turn a current implementation accident into a required architecture.
- Do identify behavior that may look internal but changes caller outcomes, desktop safety, persistence compatibility, timing, or recovery.
- Separate an exact default from the invariant that must survive a value change.

#### Tools and commands

- Use `rg`, `Get-Content`, and read-only Python summaries.
- Do not run builds/tests, write files, stage, commit, or delegate.

#### Deliverables

- A source-to-feature matrix with exact source paths/symbols, present coverage, missing behavior, timing classification, and worker routing.
- A concise list of contradictions or risks found in existing Gherkin.

#### Completion report

Return outcome, inspected paths, all routed findings, timing/workaround map, validation evidence, and no-change confirmation.

#### Escalate to the main model when

- A behavior has no obvious feature owner, source contradicts a current scenario, or a proposed scenario would require a new product decision.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

### W1 - Protocol, integration, and cross-cutting contract author

**Model:** Terra High (explicit user-requested model)
**Wave:** 1
**Depends on:** Published delegation plan
**Status:** Pending

#### Objective

Make the helper boundary and its consumers reconstructible by revising files 01 and 06 and creating `07_clean_room_contract_and_defaults.feature` for only cross-cutting external contracts.

#### Why this assignment exists

A clean-room implementation needs precise request/reply compatibility, configuration/default selection, caller deadlines, soft-gate outcomes, and observable supervision behavior. This is separate from internal state/persistence and Windows operations.

#### Inputs and established facts

- `tools/display_settings_helper_v2.cpp`, `src/platform/windows/ipc/display_settings_client.cpp`, `src/display_helper_builder.h`, `src/platform/windows/display_helper_integration.cpp`, `src/{nvhttp.cpp,stream.cpp,webrtc_stream.cpp,video.cpp}`, `src/config.*`, and settings schema are source of truth.
- Existing files 01 and 06 already cover high-level commands, response attribution, engine selection, deferred work, capture, and watchdog behavior.
- Default operational values relevant here include 5 s normal readiness/ping, 250 ms shutdown ping, 150 ms prompt disarm budget/throttle, 5 s disarm grace, 2 s helper force-stop wait, 2 s deferred initial wait, 500 ms to 10 s deferred retry interval, 30 s cooldown after two failures, six deferred attempts, and 3 s virtual re-enable cooldown where caller-facing.

#### Scope and ownership

- Primary files: `behaviors/display_engine_2/scenarios/01_helper_protocol_and_session_lifecycle.feature`, `06_client_integration_and_stream_lifecycle.feature`, and new `07_clean_room_contract_and_defaults.feature`.
- May inspect: stated source and configuration files plus other features only for avoiding duplication.
- Must not modify: files 02-05, sources, plan, Git state.
- Concurrent overlap: W2 owns 02-03 and W3 later owns 04-05. Route any overlap to main model.

#### Required production steps

1. Replace vague "bounded" / "source-defined" language in owned behavior with a default timing/retry profile, trigger, deadline result, and tunability invariant.
2. Describe every accepted command/request shape, optional field/default, compatibility form, result/status, correlation rule, and invalid-input behavior needed to interoperate without mandating a transport implementation.
3. State engine-selection/default/persistence, helper availability/restart/cooldown, disarm/stop/revert, connection replacement, and log synchronization outcomes as clean-room requirements.
4. Detail all caller contracts: request construction, deferred resolution ownership, RTSP/WebRTC/HTTP soft gates, HDR/settle behavior, capability query outcomes, watchdog adoption/stop, and safe direct-fallback restrictions.
5. Create file 07 only for cross-cutting public vocabulary, default operational profile, and tunability rules that would otherwise be duplicated; do not restate per-command behavior there.
6. Use scenario outlines/examples where default values and tunable invariants benefit from a compact matrix. Keep requirements outcome-focused: "returns by default deadline" is allowed; "uses a named pipe/worker" is not.
7. Validate all three owned feature files structurally and with `git diff --check`.

#### Required behavior and edge cases

- A changed timing value may not allow stale replies, stale verification, unsafe fallback, an unbounded stream start, or cross-session result attribution.
- Failed, unknown, unavailable, canceled, and verified capture outcomes remain distinguishable at every caller.
- HTTP capability probing remains nonfatal on an unverified soft gate.
- Compatibility includes legacy/raw/framed behavior where source supports it, without requiring a particular parser/connection design.

#### Decision branches

- If E1 finds a source behavior owned by W1 that is absent from this card, add it to the appropriate owned feature and report the routed evidence.
- If a numeric value only controls an invisible polling cadence, do not mandate it; describe only the enclosing observable deadline and cancellation responsiveness.
- If a compatibility behavior is ambiguous in source, stop and report rather than invent a schema.

#### Tools and commands

- Use `rg`, `Get-Content`, `apply_patch`, and read-only Python validation.
- Do not run builds/tests, edit sources, stage, commit, or delegate.

#### Deliverables

- Revised files 01 and 06; new file 07.
- A source-to-scenario/time-default map and self-validation results.

#### Completion report

Return outcome, exact changed files, new behavior/default contracts, validation, E1 findings applied, and risks.

#### Escalate to the main model when

- A needed behavior belongs to W2/W3, source contradicts existing scenario behavior, or a required public value is not determinable from source.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

### W2 - State, timing, and persistence contract author

**Model:** Terra High (explicit user-requested model)
**Wave:** 1
**Depends on:** Published delegation plan
**Status:** Pending

#### Objective

Make state orchestration and baseline persistence reconstructible by revising files 02 and 03 with explicit command precedence, state outcomes, retry/default timing profiles, cancellation boundaries, persistence compatibility, candidate selection, and golden-health behavior.

#### Why this assignment exists

The main clean-room risk is reproducing the correct desktop-safety outcome when commands, async completion, heartbeat, recovery, capture, and persistence states race. The existing scenarios enumerate many cases but do not consistently define their default timeline or structural data contract.

#### Inputs and established facts

- `state_machine.*`, `async_dispatcher.*`, `runtime_support.h`, `timing.h`, `types.h`, `snapshot.*`, `snapshot_codec.*`, `golden_health.*`, and `operations.*` are source of truth.
- Existing file 02 owns state/command behavior; file 03 owns snapshots/golden behavior.
- Source defaults include three pre-apply capture attempts, two topology attempts, three Apply attempts, 500 ms base retry/settle delay, 5 s disconnect grace, 2 s stable read, 150 ms sampling, 750 ms quiet confirmation, 700 ms recovery repeat delay, and all source-defined recovery-window/default scheduler values discovered by inspection.

#### Scope and ownership

- Primary files: `behaviors/display_engine_2/scenarios/02_state_and_command_orchestration.feature` and `03_snapshots_and_golden_baselines.feature`.
- May inspect: source scope above plus file 07 after W1 creates it for terminology only.
- Must not modify: files 01, 04-07, sources, plan, Git state.
- Concurrent overlap: W1 owns external protocol/caller behavior; W3 owns Windows Apply/recovery operation details. Report overlaps to main model.

#### Required production steps

1. Establish Gherkin state vocabulary sufficient to reconstruct valid states, transitions, command precedence, stale-generation handling, and terminal status outcomes without naming implementation classes.
2. For each cancellation/supersession boundary, document whether no mutation started, desktop may have changed, a recovery guard survives, a result is discarded, or the newer command owns the outcome.
3. Add default retry/backoff/settling profiles with examples that distinguish configurable durations from invariant attempt limits, priority, and safe terminal result.
4. Describe baseline/snapshot data compatibility in terms of required/optional fields, filtering, parsing, replacement visibility, history advancement, candidate eligibility, and confirmation. Preserve documented source non-guarantees rather than upgrading them.
5. Expand golden selection/health/cooldown and recovery-window outcomes so a new implementation can reproduce ordering, reset, failure, and reporting behavior.
6. Remove vague statements that cannot guide a clean-room rebuild; replace them with observable precondition, default envelope, completion condition, and fallback/recovery outcome.
7. Validate owned features structurally, search for implementation prescriptions, and run `git diff --check`.

#### Required behavior and edge cases

- Command ordering must describe what wins when Apply, Revert, Disarm, Reset, Refresh Rate, Stop, events, and heartbeat signals overlap.
- A default timing change must not change whether a stale completion can mutate a newer session or disarm recovery after uncertainty.
- Snapshot compatibility must distinguish absence, malformed-but-extractable content, partial/filtered data, and an eventually confirmed restore.
- Golden preference, current-missing preference, health status, session-success cooldown, and fallback order must be directly testable.

#### Decision branches

- If a timer is a polling cadence with no direct caller outcome, document the enclosing deadline/attempt policy rather than its internal interval.
- If source permits a compatibility hazard/non-guarantee, document it explicitly; do not convert it into a desired safety guarantee.

#### Tools and commands

- Use `rg`, `Get-Content`, `apply_patch`, and read-only Python validation.
- Do not run builds/tests, edit sources, stage, commit, or delegate.

#### Deliverables

- Revised files 02-03.
- A state/timing and persistence-compatibility coverage map with validation results.

#### Completion report

Return outcome, exact changed files, scenario additions/corrections, default/invariant matrix, validation, and risks.

#### Escalate to the main model when

- A requirement belongs in protocol/caller or Windows behavior, or source has an irreconcilable ambiguity.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

### W3 - Windows display, recovery, and workaround contract author

**Model:** Terra High (explicit user-requested model)
**Wave:** 2
**Depends on:** E1 findings
**Status:** Pending

#### Objective

Make the Windows-facing Apply, recovery, event, virtual-display, durable-safeguard, HDR, shell, and workaround behavior reconstructible by revising files 04 and 05.

#### Why this assignment exists

Clean-room regressions are most likely where Windows reports partially applied topology, a monitor/virtual display changes identity, HDR needs a workaround, or recovery must preserve a real desktop. These must be stated as outcome contracts rather than incidental platform APIs.

#### Inputs and established facts

- `operations.*`, `staged_settings.h`, `win_display_settings.*`, `win_event_pump.*`, `win_scheduled_task_manager.*`, `win_virtual_display_driver.h`, `win_platform_workarounds.*`, and the state-machine call sites are source of truth.
- Existing file 04 owns Apply/topology/configuration; file 05 owns recovery/events/watchdog/platform.
- Source defaults include 5 s topology activation, two topology attempts, 500 ms recovery settle, up to three Apply attempts with source retry progression, 200 ms positioning retry for up to 15 attempts, 2 s stability capture, 150 ms sampling, 750 ms quiet confirmation, 700 ms recovery second attempt delay, 5 s reconnect/disconnect grace, and 3 s virtual display re-enable cooldown where observable.

#### Scope and ownership

- Primary files: `behaviors/display_engine_2/scenarios/04_apply_topology_and_configuration.feature` and `05_recovery_events_watchdog_and_platform.feature`.
- May inspect: listed source files and E1 report; other features only to avoid duplication.
- Must not modify: files 01-03, 06-07, sources, plan, Git state.
- Concurrent overlap: W1 owns direct caller behavior, W2 owns state/persistence contracts. Report behavior that crosses those boundaries.

#### Required production steps

1. Turn every Windows workaround into a scenario with a trigger, protection/visible outcome, ordering, default timing if meaningful, and no-op/skip condition: HDR blanking, shell refresh, monitor positioning bounds/retry, physical refresh restoration after virtual creation, topology settling/recovery, virtual-display reset/retarget/re-enable, event recovery, and durable restore at logon.
2. Define Apply/recovery request behavior as a clean-room transaction: validation, target selection, topology acceptance/rejection, mutation safety arm, retry, cancellation, rollback, settings application, verification, and terminal/recovery ownership.
3. State default windows/attempts as tunable defaults with hard invariants: never act after cancellation, never treat a possibly changed desktop as safe without confirmation, never retarget an adjusted topology to the wrong display, and never lose the durable recovery path merely because a value is tuned.
4. Expand recovery ordering: baseline choice handoff, stable/quiet confirmation, failure/cancellation, event reopening, heartbeat/disconnect relevance, task/safeguard cleanup, virtual device identity handling, and staged state reset behavior.
5. Preserve actual source limitations and ordering (including golden fallback retirement timing) rather than inventing idealized guarantees.
6. Validate both owned features structurally, with `git diff --check`, and with a source-to-scenario workaround/timing matrix.

#### Required behavior and edge cases

- A topology must meet the source-defined conditions for OS-adjusted acceptance; a non-success status is never rescued by a visually usable topology alone.
- Layout/rotation belongs to recovery only; Apply verification checks only fields it actually accepts.
- Windows event behavior must differentiate stale/irrelevant events, active recovery events, virtual-display events, and event/retry-window results.
- A workaround must be optional only when its source trigger is absent, and its failure must have the source-defined effect on the core operation/recovery state.
- Timing must be described as a default operational profile and must include stop/cancellation behavior, not merely a wait duration.

#### Decision branches

- If E1 identifies a platform behavior owned by W3 but absent here, add it to 04 or 05 based on whether it changes the current Apply or recovery/monitoring behavior.
- If a Windows API call has no observable impact beyond the already-covered outcome, do not prescribe it.

#### Tools and commands

- Use `rg`, `Get-Content`, `apply_patch`, and read-only Python validation.
- Do not run builds/tests, edit sources, stage, commit, or delegate.

#### Deliverables

- Revised files 04-05.
- A Windows-workaround and timing/default coverage map with validation evidence.

#### Completion report

Return outcome, changed files, detailed workarounds/defaults added, validation, E1 findings applied, and risks.

#### Escalate to the main model when

- A behavior belongs in W1/W2, source contradicts a scenario, or a source workaround has no stable observable contract.

#### Delegation prohibition

Do not spawn or delegate to any subagent. Work only on this assignment and report to the main model.

## 6. Main-model integration and validation

### Integration procedure

1. Receive E1's complete mapping and route any newly found gap to exactly one owner before W3 begins.
2. Inspect every worker change and reconcile timing terminology: every default must name the tunable value, its trigger, its terminal outcome, and the invariant if it changes.
3. Confirm every Windows workaround has a trigger, compatibility goal, success/failure behavior, and skip condition without prescribing Windows APIs or concurrency design.
4. Confirm file 07 does not duplicate detailed behavior already owned by files 01-06.
5. Add only small integration wording/coverage corrections directly; return substantive source mismatches to the owner.

### Validation procedure

1. Python structural scan of all `.feature` files - exactly one Feature per file, all scenarios have Given/When/Then, all outlines own valid Examples tables, and scenario titles are unique.
2. Source-to-feature matrix across every `display_helper_v2` source, helper executable, builder/integration/client, direct callers, config, and settings schema - no reviewed behavior family left unowned.
3. Search for vague `source-defined`/unqualified `bounded` wording and for implementation prescriptions; manually classify any remaining hits.
4. `git diff --check` - no whitespace errors.
5. Manual read of each default/tunable timing scenario and each workaround scenario - proves the clean-room contract describes behavior, not a fixed implementation.

### Rework threshold

Return work to its owner for an unowned source behavior, omitted default outcome, invalid/incompatible Gherkin, true duplicate, architecture prescription, timing without a terminal/cancellation rule, workaround without a trigger/skip/failure behavior, or a scenario that upgrades a known source limitation into a guarantee. The main model may make only small wording, ownership, and validation-record edits directly.

## 7. Execution amendments

### A1 - 2026-08-03T21:42:00-05:00 - Explorer timing and workaround evidence routed

- Evidence: E1 verified every planned source module and supplied exact default envelopes, protocol/data compatibility details, Windows workaround trigger/skip/failure behavior, and no unowned behavior family.
- Original assumption: the plan had a representative timing inventory but not the complete source matrix needed by each author.
- Impact: no ownership change; W1 and W2 receive source-specific additions while W3 can begin with the complete Windows contract map.
- Action: require W1 to cover protocol wire/default/caller timing facts, W2 to cover state/snapshot/golden timing facts, and W3 to cover topology/recovery/workaround facts from E1's report.
- User authority: the user explicitly requested all source-defined behavior be captured at clean-room detail while remaining implementation-neutral.

### A2 - 2026-08-03T22:18:00-05:00 - Integration audit corrections and request-construction coverage

- Evidence: Main integration found source-accuracy mismatches in the protocol and Windows drafts, plus Gherkin scenarios that combined mutually exclusive branches. It also found unrepresented session-to-request behavior in `display_helper_request_helpers.cpp`.
- Original assumption: The worker summaries and first structural check were sufficient to establish clean-room completeness.
- Impact: W1 must add the request-construction and layout behavior to its owned client-integration feature. W2 and W3 completed targeted corrections in their original ownership only.
- Action: Correct payload compatibility, runtime logging, Apply retry timing, recovery confirmation/window semantics, durable-guard alternatives, virtual identity, workarounds, and Gherkin causal structure before final validation.
- User authority: The user explicitly required all logical code-defined behavior, including Windows workarounds and timing/cancellation behavior, to be reconstructible without prescribing structure.

### A3 - 2026-08-03T22:42:00-05:00 - Final source-accuracy closure

- Evidence: Follow-up reviews corrected protocol payload compatibility, live log-level behavior, Gherkin alternative-branch semantics, Windows retry/confirmation/event/reset timing, request-helper layout derivation, standard compatibility modes, terminal-status mappings, and target verification scopes.
- Original assumption: Broad scenario coverage could stand without encoding the source's fine-grained status and verification distinctions.
- Impact: No ownership expansion beyond W1-W3's original feature boundaries; each correction stayed within its owner files.
- Action: Proceed with a main-model full-suite structural, causal, wording, timing, and source-to-feature audit before committing.
- User authority: unchanged; this is necessary to meet the clean-room reconstruction standard the user requested.

### A4 - 2026-08-03T23:10:00-05:00 - Independent final clean-room audit

- Evidence: Three fresh Terra High reviews rechecked protocol/caller behavior, state/snapshot/Apply behavior, and Windows recovery/workaround behavior against their current source surfaces.
- Original assumption: The first authoring pass and main integration review were sufficient to establish reconstructibility.
- Impact: Added listener-renewal and watchdog-cadence defaults; made default durations explicitly tunable while retaining stated retry caps and ordering; captured position identity retries, snapshot persistence boundaries, recovery grace/final validation, durable logon fallback, and HDR-workaround non-overlap.
- Action: Remove remaining internal-mechanism wording and split multi-branch scenarios so every scenario has a single causal Given/When/Then path.
- User authority: unchanged; this is the requested final verification that the Gherkin defines behavioral outcomes rather than a particular implementation structure.

## 8. Outcomes

| ID | Result | Deliverables | Validation | Follow-up |
|---|---|---|---|---|
| E1 | Complete | Complete source-to-feature, timing, workaround, compatibility, and non-guarantee map | All planned source modules cross-checked read-only | Routed findings to W1-W3 |
| W1 | Complete | Revised files 01, 06, and new file 07 | 01=88, 06=137, 07=6 scenarios; protocol/client source audit, wording, and diff checks passed | Integrated |
| W2 | Complete | Revised files 02-03 | 02=55 and 03=62 scenarios; state/snapshot/Apply source audit, causal-sequence, status-mapping, and diff checks passed | Integrated |
| W3 | Complete | Revised files 04-05 | 04=38 and 05=48 scenarios; Windows workaround/recovery source audit, target-verification, wording, and diff checks passed | Integrated |

### Final integration status

- Status: complete
- Validation evidence: 434 scenarios across seven feature files; full Given/When/Then, outline, unique-title, and causal-order scan passed; implementation-prescription scan passed; `git diff --check` passed. E1's source-to-feature map plus three independent final reviews covered helper protocol, IPC compatibility, client request construction, stream lifecycle, state and persistence, Windows Apply/recovery/event behavior, and every caller-visible timing/workaround family.
- Remaining risks: No replacement implementation exists to execute as a conformance suite. Internal polling cadence with no observable deadline, attempt, cancellation, or result effect remains intentionally unspecified; every source-visible safety boundary, default profile, retry cap, and workaround outcome is represented.
- Delivered artifacts: revised feature files 01-06, new clean-room/defaults feature 07, and this delegation/audit plan.
