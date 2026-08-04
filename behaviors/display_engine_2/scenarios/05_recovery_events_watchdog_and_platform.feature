@display-engine-v2
Feature: Recovery, events, watchdogs, and Windows platform safety
  The display engine protects the user's desktop while a streamed display
  configuration may be active, then restores it safely when a session or the
  operating system becomes unhealthy.

  Rule: A desktop that may have changed remains recoverable

    Scenario: An explicit revert protects the desktop and restores it
      Given a session baseline or golden baseline is available
      And a stream configuration may have changed the desktop
      When the client explicitly requests a revert
      Then recovery protection remains armed before restoration begins
      And the helper gives a replacement session a short opportunity to take control when the revert is not immediate
      And restoration proceeds immediately for a restore-startup request
      And the protection is removed only after the restored desktop is confirmed

    Scenario: Restore startup does not leave an orphaned recovery attempt
      Given the helper starts in restore mode after a reboot or later logon
      When a usable saved baseline is available
      Then it restores and validates that baseline without waiting for a control connection
      And a confirmed restore reaches a safe terminal state

    Scenario: Restore startup without a baseline finishes cleanly
      Given the helper starts in restore mode after a reboot or later logon
      And no current, previous, or golden baseline exists
      When the helper evaluates available restore candidates
      Then it clears the no-op recovery state and completes the restore launch
      And it exits instead of waiting indefinitely for a candidate that does not exist

    Scenario: A replacement during revert grace prevents an obsolete restore mutation
      Given a non-immediate revert is waiting for a replacement session opportunity
      When a newer Apply or disarm supersedes the revert before restoration begins
      Then the old recovery does not change the desktop
      And the newer session intent becomes responsible for the next safe outcome

    Scenario: A canceled recovery keeps protection for an uncertain desktop
      Given recovery has passed the point at which topology, modes, HDR, primary display, or layout may change
      And durable recovery protection is present
      When recovery or its validation is canceled or superseded
      Then no obsolete completion may declare the desktop restored
      And recovery protection remains armed because the exact mutation boundary is unknown
      And the durable safeguard remains until a confirmed recovery, verified replacement session, explicit disarm, or reset safely owns the result

    Scenario: A canceled apply retains protection when it may have touched the desktop
      Given an apply is superseded after it may have changed the desktop
      When its late completion is observed
      Then the old apply cannot control the replacement session
      And the desktop remains protected for recovery
      And a newly verified replacement may take over that protection without an unsafe gap

    Scenario: A disarm does not discard an unconfirmed restore boundary
      Given a restore attempt has changed or may have changed the desktop but is not yet confirmed
      When a later disarm is requested
      Then the helper does not falsely claim that recovery is complete
      And recovery protection remains until a safe terminal confirmation is available

  Rule: Recovery applies a candidate in a safe, stable sequence

    Scenario: Restore waits for a complete usable desktop before later restore stages
      Given a recovery candidate contains topology, display settings, and optional layout data
      When recovery activates the candidate topology
      Then it waits until the candidate's required display paths are usable before applying later settings
      And it restores display modes and HDR before primary-display and origin settings
      And it applies recorded rotation or layout only after the preceding restore stages succeed
      And failure at any stage does not report the desktop as restored

    Scenario: A restore candidate is confirmed only after stable and quiet observations
      Given a recovery candidate appears to match the current desktop at one observation
      When recovery decides whether that candidate is already restored or has just been restored
      Then it requires repeated stable desktop observations and any required layout match
      And it requires the restored desktop to remain unchanged through a bounded quiet period
      And a changing or incomplete desktop remains recoverable rather than being reported as restored

    Scenario: Transient OS validation does not discard a structurally valid restore candidate
      Given a saved recovery candidate has valid topology structure
      But an operating-system topology probe is temporarily unavailable while displays are transitioning
      When recovery evaluates that candidate
      Then it retains the candidate for the actual bounded restore path
      And it reports success only if a later topology, settings, and stable-confirmation sequence succeeds

    Scenario: Recovery validation failure returns to protected retry behavior
      Given a restore operation completed with a candidate that appeared successful
      But final recovery validation finds that the desktop no longer matches the candidate
      When the helper handles the validation failure
      Then it keeps recovery protection and the durable safeguard in place
      And it returns to bounded event-driven recovery rather than claiming a safe terminal state

  Rule: Lost control connections respect the session's restore policy

    Scenario: A broken connection restores a protected session when restoration is enabled
      Given a verified session has changed the display and restore-on-disconnect is enabled
      When the control connection breaks and its reconnect grace expires
      Then the helper begins bounded recovery of the protected desktop
      And a replacement control connection cancels the earlier disconnect path before it restores the old session

    Scenario: A deliberately retained stream is not restored merely because its connection breaks
      Given a live session has explicitly disabled restore-on-disconnect
      And no explicit revert is pending
      When its control connection breaks or its heartbeat is lost
      Then the helper does not autonomously restore the paused session's desktop
      And it disarms the disconnected session rather than scheduling further display mutations
      But an explicit revert remains authoritative despite that opt-out

    Scenario: A recent post-apply disconnect settles before deciding to restore
      Given a configuration apply has just started and the connection breaks during its short startup-churn period
      When the initial apply or verification completes
      Then the helper verifies the requested configuration through its bounded disconnected settlement checks
      And it repairs a failed check only while the session remains recoverable
      And a healthy settled configuration keeps its recovery protection without requiring an unreachable client response

  Rule: The heartbeat detects a lost helper client without racing a healthy one

    Scenario: Heartbeat monitoring allows startup and missed-ping grace windows
      Given a verified connected session is protected by heartbeat monitoring
      When the initial optional heartbeat window has not elapsed
      Then no recovery is triggered solely because a ping has not yet arrived
      When the missed-ping window expires
      Then the helper allows the configured recovery grace window before treating the client as lost
      And a ping during that grace window makes the session healthy again

    Scenario: A confirmed heartbeat timeout follows the disconnect policy
      Given a protected session has missed its heartbeat beyond the recovery deadline
      When the timeout is confirmed
      Then the stale control connection is retired before recovery is considered
      And the helper starts bounded recovery when restoration is enabled or explicitly required
      And it leaves an opted-out paused session un-restored
      And a heartbeat cannot interrupt a recovery or a newer replacement intent that already owns the desktop

  Rule: Windows display events reopen only bounded recovery opportunities

    Scenario Outline: Windows signals a changed display environment
      Given recovery is armed after a failed or unconfirmed restore
      And the ordinary recovery window has expired or is waiting for another opportunity
      When Windows reports <signal>
      Then the event opens a bounded event recovery window for the current recovery
      And the next eligible recovery attempt starts without carrying an old backoff delay

      Examples:
        | signal |
        | a display configuration change |
        | monitor power resume or monitor power-on |
        | display-device arrival |
        | display-device removal |

    Scenario: Recovery retries only within its allowed windows
      Given recovery is armed and a restore attempt cannot be validated
      When repeated attempts fail within the primary or event recovery window
      Then subsequent attempts use progressive bounded backoff
      And the helper stops retrying when that window is exhausted
      And it preserves recovery protection while waiting for an eligible new event
      And a later display event can open a new bounded event window

    Scenario: Irrelevant or stale display events do not mutate the current desktop
      Given a newer session or cancellation generation has replaced an earlier one
      When a delayed display event from the earlier generation arrives
      Then it is ignored
      And a display event outside an armed recovery or virtual-display session does not start an unsolicited restore

  Rule: Durable recovery survives helper loss but is cleaned up safely

    Scenario: The durable restore safeguard is armed before a possible desktop mutation
      Given a request is about to alter topology, display settings, or reset a virtual display
      When Windows may first observe that mutation
      Then a durable restore safeguard is armed before the mutation boundary
      And the safeguard starts the helper in restore mode at a later user logon if needed
      And it works for a resolved interactive user or a safe all-users logon fallback

    Scenario: Failure to arm durable recovery does not masquerade as a safe desktop
      Given a display mutation may have occurred
      When the durable restore safeguard cannot be created
      Then the helper retains in-process recovery protection and its recovery-required state
      And it does not treat the session as safely restored or safely disarmed solely because task creation failed

    Scenario: A confirmed recovery cleans up only the safeguards it no longer needs
      Given recovery has restored a saved baseline and validation succeeds
      When the desktop is confirmed to match the restored baseline
      Then heartbeat monitoring and retry scheduling are disarmed
      And the durable restore safeguard is removed
      And the helper refreshes the Windows shell so the restored desktop is visible coherently

    Scenario: A confirmed golden restoration retires session fallbacks before final recovery validation
      Given golden recovery has restored and stably confirmed a golden baseline
      And current and previous session baselines were available as fallback candidates
      When the helper proceeds to final recovery validation
      Then it retires the obsolete current and previous session baselines before that validation completes
      And a later validation failure keeps recovery protection but does not recreate those retired fallbacks

    Scenario: Successful post-recovery session cleanup permits a fresh Apply
      Given recovery validation has confirmed that the desktop is restored
      And the session retains prior Apply state that must be cleared before another configuration request
      When that cleanup succeeds
      Then a later Apply starts without stale session display state

    Scenario: Failed post-recovery session cleanup cannot serve another Apply
      Given recovery validation has confirmed that the desktop is restored
      And the session retains prior Apply state that must be cleared before another configuration request
      When that cleanup cannot be completed
      Then the helper does not run a subsequent Apply against that stale state
      And it finishes the current helper lifecycle so a fresh session can own the next Apply

  Rule: Virtual display changes are recovered without losing the physical desktop

    Scenario: A virtual-display reset protects the desktop before cycling the driver
      Given a virtual-display apply requests a reset because the virtual display needs recovery
      When the reset starts
      Then durable recovery protection is armed before the virtual driver is disabled
      And the driver is re-enabled before the requested configuration is retried
      And repeated reset requests are bounded rather than cycling the driver indefinitely

    Scenario: A canceled or failed virtual-display reset leaves a recoverable outcome
      Given a protected virtual-display reset has disabled the driver
      When the reset is canceled, its wait fails, or the following apply fails
      Then the helper attempts to re-enable the disabled virtual driver before reporting the failure
      And the desktop remains protected for recovery even if re-enabling the driver fails
      And a later recovery or verified replacement is required before the safeguard is removed

    Scenario: A same-identity virtual display event verifies before reapplying
      Given a verified virtual-display session is being monitored
      And the virtual display continues to resolve to the session's current device identity
      When a virtual display event arrives
      Then the helper verifies the existing target before attempting a reapply
      And a healthy configuration is retained without an unnecessary display reset or reapply

    Scenario: Failed same-identity virtual display verification repairs the existing target
      Given a verified virtual-display session is being monitored
      And the virtual display continues to resolve to the session's current device identity
      When a virtual display event verification fails
      Then it repairs lost topology, mode, or HDR configuration without retargeting an arbitrary display

    Scenario: A changed-identity virtual display event retargets before reapplying
      Given a verified virtual-display session is being monitored
      And the virtual driver now resolves to a different usable device identity
      When a virtual display event arrives
      Then the helper rediscovers and retargets the session to that virtual display identity
      And it reapplies the requested configuration only to the rediscovered virtual target
      And a late event from an earlier session cannot retarget the newer desktop session

  Rule: Platform-visible cleanup happens only for confirmed display outcomes

    Scenario: Successful verification applies requested shell and HDR recovery workarounds
      Given an apply has been initially verified for the active session
      When the session requested HDR blanking
      Then HDR states are temporarily blanked after verification rather than for every intermediate apply attempt
      And the shell is refreshed after the verified display change
      And a confirmed restore also refreshes the shell after it validates the restored desktop
