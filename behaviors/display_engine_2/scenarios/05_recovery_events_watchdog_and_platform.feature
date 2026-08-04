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
      Then the helper finishes cleanly instead of retrying forever

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
      Then the event is debounced with the generation that observed it
      And the event reopens a bounded event recovery window
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
      And a golden-baseline recovery clears obsolete current and previous session baselines
      And the helper refreshes the Windows shell so the restored desktop is visible coherently

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

    Scenario: Virtual display events rediscover the correct target before repair
      Given a verified virtual-display session is being monitored
      When the virtual driver resets, recovers, or presents a different resolved display identity
      Then the helper rediscovers the virtual display target before reapplying the request
      And a same-identity event is verified before an unnecessary reapply is attempted
      And a failed verification repairs lost topology, mode, or HDR configuration without retargeting an arbitrary display

  Rule: Platform-visible cleanup happens only for confirmed display outcomes

    Scenario: Successful verification applies requested shell and HDR recovery workarounds
      Given an apply has been initially verified for the active session
      When the session requested HDR blanking
      Then HDR states are temporarily blanked after verification rather than for every intermediate apply attempt
      And the shell is refreshed after the verified display change
      And a confirmed restore also refreshes the shell after it validates the restored desktop
