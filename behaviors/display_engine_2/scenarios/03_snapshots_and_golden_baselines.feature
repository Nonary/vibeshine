@display-engine-v2
Feature: Display engine v2 snapshots and golden baselines
  The display engine captures, filters, and prioritizes eligible display
  baselines so later recovery can protect the physical desktop from managed
  virtual displays.

  Rule: Current, previous, and golden baselines are captured only from a stable desktop

    Scenario: Snapshot Current captures the usable physical desktop
      Given the desktop is stable and contains one or more restore-capable physical displays
      When a client requests Snapshot Current
      Then the engine captures a current-session baseline for the usable physical desktop
      And it reports whether that baseline was saved for the requesting session

    Scenario: A current capture prepares a valid replacement before refreshing history
      Given a valid current-session baseline already exists
      When the engine captures a replacement current-session baseline
      Then it validates, filters, and captures applicable layout data for the replacement before refreshing history
      And it stores the valid replacement as current
      And when history refresh succeeds, previous retains the prior current desktop for one level of restore history

    Scenario: A valid current replacement is saved even when history promotion fails
      Given a valid current-session baseline already exists
      And a replacement current-session baseline has been captured and filtered successfully
      But saving the prior current baseline as previous fails
      When the engine refreshes the current-session baseline
      Then it still saves the valid replacement as current
      And it reports the refresh result from the new current save rather than failing solely because history promotion failed

    Scenario: An unchanged valid current capture remains a usable baseline
      Given the current desktop already matches the current-session baseline
      When the engine refreshes that current-session baseline
      Then it accepts the equivalent valid capture as a usable current baseline
      And it does not report failure merely because the desktop did not change

    Scenario: A failed capture leaves the known-good baseline chain intact
      Given valid current and previous session baselines exist
      When a new capture is malformed, has no restore payload, has invalid topology, or has no eligible devices
      Then the engine rejects the new capture
      And it does not rotate, replace, or delete either known-good session baseline
      And the Snapshot Current result reports failure when the request is correlated

    Scenario: Baseline capture retries transient capture or save failures only within a bounded attempt budget
      Given a Snapshot Current or golden export request has a structurally valid physical desktop candidate
      But capturing or saving that candidate fails transiently
      When the engine records the baseline
      Then it retries the capture-and-save operation only a bounded number of times
      And it reports failure without replacing an earlier valid baseline when no attempt succeeds

    Scenario: Snapshot capture rejects a structurally invalid topology
      Given a captured desktop has an empty or structurally invalid topology
      When the engine evaluates it for baseline saving
      Then it rejects the capture before saving a baseline
      And it preserves any earlier valid baseline for recovery

    Scenario: A transient operating-system topology probe does not invalidate a structural baseline
      Given a saved baseline has a structurally valid topology and eligible devices
      And an operating-system topology probe is temporarily unavailable while the desktop is changing
      When the engine evaluates that baseline for recovery
      Then it does not reject the baseline as structurally invalid solely because of that transient probe failure
      And it leaves the actual recovery attempt to determine whether the desktop can be restored

    Scenario: Golden export records an intentional stable restore baseline
      Given the desktop is stable and has a usable physical display baseline
      When a client exports a golden baseline
      Then the engine saves the filtered stable desktop as the golden baseline
      And that baseline is available for later golden recovery subject to current topology, device, cooldown, and confirmation checks

    Scenario: Baseline capture is not taken from a changing desktop
      Given apply or restore activity is changing the display configuration
      When a client requests Snapshot Current or golden export
      Then the engine does not save a baseline from that transitional desktop
      And an affected correlated Snapshot Current request reports failure

  Rule: Filtering keeps only restore-capable physical display data

    Scenario: Saving omits active virtual displays without discarding usable physical displays
      Given a captured desktop contains a Sunshine-compatible virtual display and physical displays
      When the engine prepares a baseline for saving
      Then the virtual display is excluded from the saved restore baseline
      And the remaining usable physical displays are retained

    Scenario: Exclusions are case-insensitive and remove every related field
      Given a captured baseline contains a device named "DISPLAY-A" in its topology, modes, HDR states, origins, layouts, and primary selection
      And the caller excludes "display-a" using different casing
      When the engine prepares that baseline for saving or recovery
      Then "DISPLAY-A" is removed from topology, modes, HDR states, origins, layouts, and primary selection
      And no excluded device remains an implicit restore target

    Scenario: An explicit empty exclusion update clears earlier caller exclusions
      Given the active baseline policy excludes a physical display
      When Snapshot Current or golden export explicitly supplies an empty exclusion list
      Then the engine replaces the earlier caller exclusion list with the empty list
      And later baseline capture and recovery do not exclude that display solely because of the earlier list

    Scenario: An all-excluded or otherwise non-restorable capture is rejected
      Given every captured display is virtual, excluded, missing a safe display identity, or absent from the topology
      When the engine prepares a baseline for saving
      Then it rejects the capture instead of persisting an empty or partial restore baseline
      And it preserves any previously saved valid baseline

    Scenario: An inactive but connected physical display remains eligible on load
      Given a saved physical baseline contains a device that is currently connected but inactive
      When the engine evaluates the baseline for recovery
      Then the baseline is not rejected merely because that connected device has no active display name
      And the device may remain part of the complete restore candidate

    Scenario: A missing non-excluded baseline device invalidates the candidate
      Given a saved baseline contains a physical device that is no longer present
      And that device is not explicitly excluded
      When the engine evaluates the baseline for recovery
      Then it rejects the entire baseline rather than restoring only the remaining paths

    Scenario: A missing explicitly excluded device is removed instead of invalidating the candidate
      Given a saved baseline contains a device that is currently absent
      And that device is explicitly excluded by the caller
      When the engine evaluates the baseline for recovery
      Then it removes that device and all of its related baseline fields
      And it may use the remaining non-empty physical baseline

    Scenario: A loaded baseline whose topology contains an active virtual display is rejected
      Given a persisted baseline topology references a virtual display that is currently active
      When the engine evaluates it for recovery
      Then it rejects that baseline as a physical-desktop restore candidate
      And it continues only with another eligible restore tier

    Scenario: An ancillary active virtual-display reference is removed from an otherwise physical candidate
      Given a persisted baseline has a physical topology
      And an active virtual-display identifier appears only in its mode, HDR, origin, layout, or primary-display details
      When the engine evaluates the baseline for recovery
      Then it removes the virtual-display-only details from that candidate
      And the remaining physical baseline may continue through ordinary eligibility checks

  Rule: Compatible baseline content is compared by desktop meaning

    Scenario: Topology equality ignores Windows enumeration order
      Given two baselines contain the same duplicate groups and devices in different group or member orders
      And their modes, HDR states, primary selection, and required origins are the same
      When the engine compares either baseline with the current desktop
      Then it treats the topologies as equal
      And it does not reapply a restore solely because Windows reordered its enumeration

    Scenario: Missing origins remain compatible with a legacy baseline
      Given a legacy baseline contains the same topology, modes, HDR states, and primary selection as the current desktop
      And either the legacy baseline or the current capture has no origin metadata
      When the engine compares the baseline with the current desktop
      Then it treats the origin metadata as compatible

    Scenario: Different origins make two origin-aware baselines unequal
      Given two baselines contain the same topology, modes, HDR states, and primary selection
      And both baselines provide origin metadata that differs
      When the engine compares the baselines
      Then the comparison reports that the desktops differ

    Scenario: Layout metadata remains optional for pre-layout baselines
      Given a baseline written before layout metadata was available has valid restore payload
      When the engine loads that baseline
      Then it remains compatible for recovery without invented layout requirements
      And when layout metadata is present it is retained only for eligible non-excluded devices

    Scenario: Invalid layout metadata does not turn an otherwise compatible legacy baseline into a new layout contract
      Given a compatible legacy baseline has no usable layout rotation metadata
      When the engine evaluates that baseline
      Then it treats layout information as unavailable rather than requiring an inferred rotation
      And it still requires the baseline's topology and display settings to be valid

  Rule: Persistence and recovery distinguish usable payload from candidate quality

    Scenario: Persisted data without a usable topology is not adopted
      Given a saved current, previous, or golden baseline is unreadable or has no usable topology restore payload
      When the helper starts or recovery evaluates that tier
      Then it does not adopt that data as a restore candidate
      And it leaves a different valid tier available for selection

    Scenario: Parseable restore fields may be retained from a syntactically damaged baseline
      Given a saved baseline has damaged encoding but still contains extractable topology and display-settings fields
      When recovery evaluates that tier
      Then it may retain the extracted candidate for the remaining eligibility checks
      And it requires later restore and confirmation before declaring the desktop restored

    Scenario: Replacing an existing baseline does not guarantee concurrent reader isolation
      Given a valid baseline is already persisted for a tier
      When the engine saves a replacement baseline
      Then it completes the replacement data before attempting to overwrite the saved baseline
      And a concurrent recovery reader is not guaranteed to observe only a complete earlier or replacement version

    Scenario: A topology and settings membership mismatch is not rejected solely for being incomplete
      Given a persisted baseline topology and its display-settings records name different sets of devices
      When recovery evaluates that tier
      Then it does not reject the candidate solely because those device sets differ
      And it requires later application and confirmation before declaring the desktop restored

    Scenario: Normal recovery tries eligible session baselines before golden
      Given current, previous, and golden baselines have been evaluated for eligibility
      And no current-missing preference applies
      When recovery begins with the normal session-first policy
      Then it tries current before previous and uses golden only after the eligible session choices fail

    Scenario: A missing current baseline can place golden before previous
      Given current is unavailable
      And previous and golden baselines exist
      And recovery is configured to prefer golden when current is missing
      When normal recovery begins
      Then it evaluates golden before attempting previous
      And it falls through to previous if golden is not confirmed

    Scenario: A disabled current-missing preference leaves previous ahead of golden
      Given current is unavailable
      And previous and golden baselines exist
      And recovery is not configured to prefer golden when current is missing
      When normal recovery begins
      Then it attempts previous before golden

    Scenario: Golden-first recovery tries an eligible golden baseline before session baselines
      Given current, previous, and golden baselines have been evaluated for eligibility
      When recovery begins with golden-first recovery selected
      Then it tries the eligible golden baseline before session baselines

    Scenario: Confirmed current restoration advances its restore history
      Given a valid current-session baseline is selected and restoration is confirmed
      When the engine completes that recovery
      Then it attempts to advance the recovered current baseline to the previous tier
      And it retains the confirmed desktop outcome even if the optional history advancement cannot be completed

    Scenario: A confirmed previous restoration retires an attempted current baseline
      Given a current-session baseline was available but could not be restored
      And a previous-session baseline is restored and confirmed
      When the engine updates session baseline history
      Then it removes the attempted current baseline
      And it retains the confirmed restore outcome rather than retrying that stale current baseline

    Scenario: Baseline confirmation requires repeated stable desktop observations
      Given the engine has applied an eligible baseline during recovery
      When it verifies whether the desktop matches that baseline
      Then it requires repeated stable non-empty observations before confirming the result
      And a single, empty, or changing observation does not confirm the baseline

  Rule: Golden ordering and health report unresolved golden candidates accurately

    Scenario: A recent session restoration temporarily suppresses golden recovery
      Given a session baseline was restored successfully very recently
      And an otherwise eligible golden baseline is available
      When recovery evaluates golden candidates
      Then it skips golden during the short session-success cooldown
      And it does not treat that temporary suppression as a golden restore failure

    Scenario: A golden baseline with a currently missing device is skipped safely
      Given an otherwise eligible golden baseline references a device that is not currently present
      When recovery evaluates golden candidates
      Then it skips that golden baseline without attempting a partial restore
      And it does not record a golden restore failure solely because the device is currently absent

    Scenario: Golden-first recovery allows only bounded pending session fallbacks
      Given golden-first recovery is selected
      And the golden baseline remains present but is not confirmed
      And an eligible session baseline is restored successfully
      When this golden-first fallback repeats
      Then the engine keeps golden pending for only a bounded number of consecutive session fallbacks
      And it eventually accepts the confirmed session fallback and resets the pending fallback count

    Scenario: An unresolved golden restore records recurring health information
      Given an eligible golden baseline was attempted but its topology is invalid or restoration cannot be confirmed
      When recovery reaches an outcome that records the unresolved golden issue
      Then it records the latest failure reason and accumulates unresolved restore attempts across requests
      And a transient first failure does not by itself report the golden baseline as out of date

    Scenario: Repeated unresolved golden issues eventually report a stale baseline
      Given unresolved golden restore issues recur across the configured extended observation window
      When both the recurrence threshold and observation window are reached
      Then the engine reports the golden baseline as possibly out of date
      And it retains the accumulated reason and failure history for diagnosis

    Scenario: A golden health report does not by itself disqualify a currently eligible candidate
      Given prior unresolved golden restore history has been recorded
      And the golden baseline currently satisfies topology, device, cooldown, and filtering checks
      When recovery evaluates golden candidates
      Then it evaluates the candidate under the current recovery policy
      And it does not skip that candidate solely because the prior health report exists

    Scenario: A new recovery request does not reuse an uncommitted prior issue
      Given a prior recovery request noted a golden issue but did not record it as unresolved history
      When a new explicit recovery request begins
      Then only an issue from the new request can be added to unresolved health history
      And previously recorded health history remains until a confirmed golden restoration or new golden export clears it

    Scenario: Confirmed golden restoration clears health and stale session baselines
      Given an eligible golden baseline is restored and confirmed
      When recovery completes
      Then the engine clears the recurring golden health report
      And it removes current and previous session baselines that no longer describe the restored desktop

    Scenario: A successful new golden export clears the prior unhealthy status
      Given unresolved golden health history exists for an older golden baseline
      When a stable desktop is successfully exported as a new golden baseline
      Then the engine clears the prior golden health history
      And the newly saved valid golden baseline becomes eligible for later recovery
