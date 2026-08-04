@display-engine-v2
Feature: Display engine v2 snapshots and golden baselines
  The display engine preserves only complete, eligible display baselines so a
  later restore does not recreate a virtual or partial desktop.

  Rule: Current, previous, and golden baselines are captured only from a stable desktop

    Scenario: Snapshot Current captures the usable physical desktop
      Given the desktop is stable and contains one or more restore-capable physical displays
      When a client requests Snapshot Current
      Then the engine captures a current-session baseline for the usable physical desktop
      And it reports whether that baseline was saved for the requesting session

    Scenario: A current capture preserves the prior baseline until its replacement is valid
      Given a valid current-session baseline already exists
      When the engine captures a replacement current-session baseline
      Then it validates and filters the replacement before rotating the existing current baseline to previous
      And it stores the valid replacement as current
      And the rotated previous baseline retains the prior current desktop for one level of restore history

    Scenario: A failed capture leaves the known-good baseline chain intact
      Given valid current and previous session baselines exist
      When a new capture is malformed, has no restore payload, has invalid topology, or has no eligible devices
      Then the engine rejects the new capture
      And it does not rotate, replace, or delete either known-good session baseline
      And the Snapshot Current result reports failure when the request is correlated

    Scenario: Golden export records an intentional stable restore baseline
      Given the desktop is stable and has a usable physical display baseline
      When a client exports a golden baseline
      Then the engine saves the filtered stable desktop as the golden baseline
      And that baseline is eligible for golden recovery unless later validation or health policy rejects it

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

    Scenario: A loaded baseline containing an active virtual display is rejected
      Given a persisted baseline references a virtual display that is currently active
      When the engine evaluates it for recovery
      Then it rejects that baseline as a physical-desktop restore candidate
      And it continues only with another eligible restore tier

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

  Rule: Persistence and recovery admit only complete baseline candidates

    Scenario: Malformed or incomplete persisted data is not adopted
      Given a saved current, previous, or golden baseline is unreadable, malformed, or lacks topology or modes restore payload
      When the helper starts or recovery evaluates that tier
      Then it does not adopt that data as a restore candidate
      And it leaves a different valid tier available for selection

    Scenario: Persisting a replacement never exposes a torn baseline
      Given a valid baseline is already persisted for a tier
      When the engine saves a replacement baseline
      Then observers see either the complete earlier baseline or the complete replacement baseline
      And a failed save does not replace the earlier known-good baseline with partial data

    Scenario: Normal recovery tries eligible session baselines before golden
      Given current, previous, and golden baselines have been evaluated for eligibility
      When recovery begins with the normal session-first policy
      Then it tries current before previous and uses golden only after the eligible session choices fail

    Scenario: Golden-first recovery tries an eligible golden baseline before session baselines
      Given current, previous, and golden baselines have been evaluated for eligibility
      When recovery begins with golden-first recovery selected
      Then it tries the eligible golden baseline before session baselines

    Scenario: Confirmed current restoration advances its restore history
      Given a valid current-session baseline is selected and restoration is confirmed
      When the engine completes that recovery
      Then the recovered current baseline is advanced to the previous tier
      And it remains available as the one-level session history for later recovery

  Rule: Golden health prevents repeated use of a known bad golden baseline

    Scenario: An unresolved golden restore records unhealthy golden state
      Given an eligible golden baseline was attempted but its topology is invalid or restoration cannot be confirmed
      When recovery accepts a session fallback or reaches its terminal unresolved outcome
      Then the engine records that the golden baseline is unhealthy
      And later golden selection can distinguish that known bad baseline from a healthy one

    Scenario: A known unhealthy golden baseline is skipped until its health is cleared
      Given golden health marks the saved golden baseline as unhealthy
      When recovery evaluates golden candidates
      Then it does not repeatedly apply that known bad golden baseline
      And it continues with another eligible restore candidate or reports that none is available
      And it may reconsider golden only after valid behavior clears its unhealthy status

    Scenario: Confirmed golden restoration clears health and stale session baselines
      Given an eligible golden baseline is restored and confirmed
      When recovery completes
      Then the engine clears the golden unhealthy status
      And it removes current and previous session baselines that no longer describe the restored desktop

    Scenario: A successful new golden export clears the prior unhealthy status
      Given golden health marks an older golden baseline as unhealthy
      When a stable desktop is successfully exported as a new golden baseline
      Then the engine clears the unhealthy status for golden recovery
      And the newly saved valid golden baseline becomes eligible for later recovery
