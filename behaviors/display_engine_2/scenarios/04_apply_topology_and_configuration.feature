@display-engine-v2
Feature: Display engine v2 applies and verifies display topology and configuration
  A display change is successful only when its intended usable display state has
  been confirmed; accepting a request alone never proves that Windows applied it.

  Rule: An Apply request can change configuration, topology, both, or neither

    Scenario: A configuration-only request derives and applies the required topology
      Given the current desktop has a valid topology
      And an Apply request specifies a display configuration without an explicit topology
      When the engine applies the request
      Then it derives the topology needed for that configuration from the current desktop
      And it applies and verifies the requested configuration against its intended target

    Scenario: A topology-only request preserves the current display settings
      Given the current desktop has a valid topology and existing display settings
      And an Apply request supplies an explicit valid topology and no mode, HDR, or primary-setting change
      When the engine applies the request
      Then it activates the requested topology
      And it preserves the applicable current display settings
      And it confirms that the resulting topology is usable before reporting success

    Scenario: A combined request applies its accepted topology before its settings
      Given an Apply request specifies both a valid topology and a target display configuration
      When the engine applies the request
      Then it makes the topology usable before applying mode, HDR, primary-display, or position settings
      And it verifies the requested configuration against the topology Windows accepted

    Scenario: An already satisfied request is still verified without needless topology activation
      Given the requested topology is already active and its target is usable
      And the requested configuration is already active
      When the engine applies the request
      Then it does not activate the same topology again solely because it was requested
      And it reports success only after the existing configuration is confirmed stable

    Scenario: A request without the required configuration is rejected before display mutation
      Given an Apply request has no display configuration
      When the engine validates the request
      Then it rejects the request as invalid
      And it does not activate a topology or change display settings

  Rule: Topology activation accepts only a usable result for the intended target

    Scenario: A structurally invalid topology is rejected without activation
      Given an Apply request contains a topology with invalid structure
      When the engine validates the topology
      Then it rejects the request as invalid
      And it does not attempt to activate that topology

    Scenario: An OS-invalid topology is retried only after display-stack recovery
      Given an Apply request contains a structurally valid topology
      But the operating system initially rejects that topology during validation
      When the engine applies the request
      Then it protects the desktop before recovery can affect it
      And it performs one bounded display-stack recovery and settling attempt before retrying validation
      And it reports a retryable non-success result if validation is still rejected

    Scenario: A topology activation is retried only within its bounded recovery policy
      Given an Apply request contains a topology that passed validation
      And its first activation does not become usable
      When the engine applies the request
      Then it waits only a bounded time for the topology to become usable
      And it performs bounded recovery before a further activation attempt
      And it reports a retryable non-success result when the allowed attempts are exhausted

    Scenario: A Windows-adjusted topology is accepted when the intended target survives
      Given an Apply request has a target display or duplicate group
      And Windows activates a usable topology that differs from the requested topology
      And the intended target remains active and enumerable in that adjusted topology
      When the engine evaluates the activation result
      Then it accepts the adjusted topology
      And it applies settings only to the intended target

    Scenario: A Windows-adjusted topology is rejected when the intended target is lost
      Given an Apply request has a target display or duplicate group
      And Windows activates a usable topology that differs from the requested topology
      And the intended target is no longer active or enumerable in that adjusted topology
      When the engine evaluates the activation result
      Then it treats the activation as unconfirmed and does not apply settings to an arbitrary replacement display

  Rule: Target identity remains stable when Windows reorders display paths

    Scenario: An explicit device identifier remains the configuration target
      Given an Apply request names a specific device identifier
      And Windows reorders unrelated display paths while applying the topology
      When the engine applies and verifies the configuration
      Then the named device remains the target for mode, HDR, and primary-display checks
      And a different display is never selected merely because it appears first after reordering

    Scenario: An empty device identifier targets the original primary duplicate group
      Given the original primary display belongs to a duplicate group
      And an Apply request omits its device identifier
      When Windows accepts the requested or adjusted topology
      Then the engine resolves the target as the original primary duplicate group
      And it checks required mode, refresh, HDR, and primary status against that accepted group
      And it does not collapse the request to an arbitrary duplicate-group member

  Rule: Failed or cancelled mutations preserve a path back to a safe desktop

    Scenario: A failed settings stage rolls back the captured baseline when possible
      Given the engine captured a valid pre-Apply baseline before display mutation
      And topology activation succeeded
      But applying the requested mode, HDR, or primary-display settings fails
      When the engine handles the failed settings stage
      Then it attempts to restore the captured baseline topology and settings
      And it treats the desktop as no longer changed only after that baseline is confirmed restored

    Scenario: An unconfirmed rollback retains desktop recovery protection
      Given a requested settings stage failed after topology activation
      And the engine attempted to restore the captured baseline
      And that rollback cannot be confirmed
      When the failed Apply reaches its terminal outcome
      Then it retains recovery protection and reports a non-success outcome

    Scenario: Cancellation before a mutation leaves the desktop and recovery state unmodified
      Given an Apply request is waiting before any topology or settings mutation
      When the request is cancelled or superseded
      Then the engine does not start another display-changing stage for that request
      And it does not claim that the desktop changed

    Scenario: Cancellation after possible mutation retains recovery protection
      Given an Apply request has entered topology activation, display-stack recovery, or settings application
      When the request is cancelled or superseded before its result is confirmed
      Then the engine stops later cancellable stages for that request
      And it treats the desktop as possibly changed
      And it retains durable recovery protection until a safe terminal outcome is confirmed

  Rule: Apply restores requested presentation details within safe limits

    Scenario: HDR is applied and blanked only when requested
      Given an Apply request specifies an HDR state for its resolved target
      When the engine applies the request
      Then it applies and verifies that HDR state for the target scope
      And it performs a temporary HDR blanking workaround only when the request explicitly asks for it

    Scenario: Position overrides are constrained to a usable desktop coordinate range
      Given an Apply request contains a position override for an active display with known dimensions
      And the requested origin would place part of the display outside the usable coordinate range
      When the engine applies the position override
      Then it constrains the origin so the whole display fits within the supported range
      And it retries a transient positioning failure only for a bounded settling window

    Scenario: Inactive or non-repositionable displays are not repositioned
      Given an Apply request contains a position override for a missing or inactive display
      When the engine applies position overrides
      Then it does not attempt to reposition that display
      And it leaves any still-unapplied override unresolved after the bounded retry window

    Scenario: Layout and rotation are restored and confirmed when layout data is present
      Given a display restoration includes layout or rotation data for active displays
      When the engine restores the desktop layout
      Then it applies the recorded layout and rotation after topology and display settings are usable
      And it does not report the restoration confirmed until the required layout and rotation match

    Scenario: Refresh-rate overrides restore physical displays without changing the virtual target
      Given an Apply request includes valid refresh-rate overrides after virtual-display creation
      And one override identifies the configured virtual display and another identifies a physical display
      When the engine applies refresh-rate overrides
      Then it skips the virtual display override
      And it restores the requested physical display refresh rate when that display is available
      And it ignores empty device identifiers or invalid zero-valued rates

    Scenario: Refresh and configuration verification require the requested active state
      Given an Apply request requires a resolution, refresh rate, HDR state, primary status, or layout
      And every required property matches on the resolved active target
      When the engine verifies the completed display change
      Then it verifies the topology and resolved target remain active
      And it verifies every requested resolution, refresh, HDR, and primary-display property in that target scope
      And it verifies required layout or rotation data when such data was restored

    Scenario: A repeated configuration mismatch fails verification
      Given an Apply request requires a resolution, refresh rate, HDR state, primary status, or layout
      And a required property does not match on repeated verification
      When the engine verifies the completed display change
      Then it reports verification failure rather than treating the accepted request as a successful display change
