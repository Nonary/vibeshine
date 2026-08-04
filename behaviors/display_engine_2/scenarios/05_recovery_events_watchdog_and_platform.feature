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
      And the helper gives a replacement session the configured non-immediate revert grace opportunity to take control
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

    Scenario: Ordinary recovery has a bounded grace and primary opportunity
      Given a protected desktop needs recovery after an ordinary client revert or lost control connection
      When recovery is not an immediate restore-startup request
      Then it gives a replacement session the default 5-second grace opportunity before beginning restoration
      And recovery attempts remain eligible for the default 2-minute primary recovery window
      And an immediate restore-startup request begins without that grace
      And tuning either duration preserves the replacement opportunity, a finite primary retry window, cancellation responsiveness, and the requirement for confirmed restoration

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
      Then it requires two equal nonempty desktop observations separated by the default 150 ms sampling interval
      And it acquires that pair within a default window of up to 2 seconds
      And it requires any recorded layout or rotation to match only during recovery confirmation
      And it requires the restored desktop to remain unchanged through the default 750 ms quiet period
      And a changing or incomplete desktop remains recoverable rather than being reported as restored
      And those defaults may change only when they preserve the equal-nonempty observation gate, quiet-period confirmation, cancellation responsiveness, and the same terminal recovery result

    Scenario: Recovery uses at most two safe restore applications for one candidate
      Given a recovery candidate has not already been stably confirmed
      When recovery attempts that candidate
      Then it may apply the candidate once and validate it
      And before a second and final safe restore application it waits the default 700 ms double-check interval and checks whether the desktop already matches
      And it does not apply that candidate more than twice in the same recovery attempt
      And cancellation during either application, confirmation, or double-check leaves recovery and durable protection armed

    Scenario: Transient OS validation does not discard a structurally valid restore candidate
      Given a saved recovery candidate has valid topology structure
      But an operating-system topology probe is temporarily unavailable while displays are transitioning
      When recovery evaluates that candidate
      Then it retains the candidate for the actual two-application restore path
      And it reports success only if a later topology, settings, and stable-confirmation sequence succeeds

    Scenario: Recovery validation failure returns to protected retry behavior
      Given a restore operation completed with a candidate that appeared successful
      But final recovery validation finds that the desktop no longer matches the candidate
      When the helper handles the validation failure
      Then it keeps recovery protection and the durable safeguard in place
      And it returns to protected retry behavior in the current scheduler window rather than claiming a safe terminal state
      And a later relevant display event may open or extend its default 30-second event window

    Scenario: Final recovery validation waits for the desktop to settle and remains cancellable
      Given recovery has stably confirmed a candidate and must make its final recovery decision
      When final validation starts
      Then it rechecks the current desktop after the default 250 ms settling delay
      And cancellation before that check prevents a restored terminal result
      And tuning that delay may not bypass the final match check, convert cancellation into success, or remove recovery protection before confirmation

  Rule: Lost control connections respect the session's restore policy

    Scenario: A broken connection restores a protected session when restoration is enabled
      Given a verified session has changed the display and restore-on-disconnect is enabled
      When the control connection breaks and its reconnect grace expires
      Then the helper begins recovery in the current primary or event recovery window
      And a replacement control connection cancels the earlier disconnect path before it restores the old session

    Scenario: A deliberately retained stream is not restored merely because its connection breaks
      Given a live session has explicitly disabled restore-on-disconnect
      And no explicit revert is pending
      When its control connection breaks or its heartbeat is lost
      Then the helper does not autonomously restore the paused session's desktop
      And it disarms the disconnected session rather than scheduling further display mutations
      But an explicit revert remains authoritative despite that opt-out

    Scenario: A recent post-apply disconnect settles before deciding to restore
      Given a configuration apply has just started and the connection breaks during its default 5-second startup-churn period
      When the initial apply or verification completes
      Then the helper verifies the requested configuration through its defined disconnected settlement checks
      And it repairs a failed check only while the session remains recoverable
      And a healthy settled configuration keeps its recovery protection without requiring an unreachable client response

    Scenario: Recent post-apply disconnect uses a bounded verify-before-repair ladder
      Given the connection breaks during the default 5-second post-Apply startup-churn period
      And the session remains eligible for disconnect recovery
      When the Apply reaches a point at which its disconnected configuration can be checked
      Then disconnected settlement replaces ordinary delayed stabilization with checks after the default 250 ms and 750 ms slots
      And a failed check repairs the current requested configuration before the next applicable slot
      And no old ordinary stabilization check may race or override that settlement decision
      And tuning those slots preserves bounded checks, repair only for the current recoverable session, and cancellation safety

  Rule: The heartbeat detects a lost helper client without racing a healthy one

    Scenario: Heartbeat monitoring honors the optional startup window
      Given a verified connected session is protected by heartbeat monitoring
      When the initial optional heartbeat window has not elapsed
      Then no recovery is triggered solely because a ping has not yet arrived

    Scenario: Heartbeat monitoring applies missed-ping and recovery grace windows
      Given a verified connected session is protected by heartbeat monitoring
      And its optional startup window has elapsed without a qualifying ping
      When the missed-ping window expires
      Then the helper allows the configured recovery grace window before treating the client as lost
      And a ping during that grace window makes the session healthy again
      And the source default windows are 30 seconds optional startup, 30 seconds without a ping, and 2 minutes of recovery grace
      And tuning those windows may not restore a session before both missed-ping and grace conditions have occurred, or ignore a ping that reestablishes health

    Scenario: A confirmed heartbeat timeout follows the disconnect policy
      Given a protected session has missed its heartbeat beyond the recovery deadline
      When the timeout is confirmed
      Then the stale control connection is retired before recovery is considered
      And the helper starts recovery in the current primary or event recovery window when restoration is enabled or explicitly required
      And it leaves an opted-out paused session un-restored
      And a heartbeat cannot interrupt a recovery or a newer replacement intent that already owns the desktop

  Rule: Windows display events reopen 30-second recovery opportunities

    Scenario Outline: Windows signals a changed display environment
      Given recovery is armed after a failed or unconfirmed restore
      And the ordinary recovery window has expired or is waiting for another opportunity
      When Windows reports <signal>
      Then the event opens or extends a default 30-second event recovery window for the current recovery
      And the next eligible recovery attempt starts without carrying an old backoff delay
      And changing that window may not let an event restore an unarmed or stale session, bypass cancellation, or declare an unconfirmed desktop restored

      Examples:
        | signal |
        | a display configuration change |
        | monitor power resume or monitor power-on |
        | display-device arrival |
        | display-device removal |
        | display-device nodes changed |

    Scenario: Recovery retries only within its allowed windows
      Given recovery is armed and a restore attempt cannot be validated
      When repeated attempts fail within the primary or event recovery window
      Then subsequent attempts use the progressive default backoff sequence of 0, 1, 3, 5, 10, 15, 20, then 30 seconds
      And the helper stops retrying when that window is exhausted
      And it preserves recovery protection while waiting for an eligible new event
      And a later display event can open a new 30-second event window
      And tuning the backoff intervals preserves bounded, progressive retries and lets a relevant new event remove an old backoff delay without bypassing protection or confirmation

    Scenario: Irrelevant or stale display events do not mutate the current desktop
      Given a newer session or cancellation intent has replaced an earlier one
      When a delayed display event associated with the earlier session arrives
      Then it is ignored
      And a display event outside an armed recovery or virtual-display session does not start an unsolicited restore

    Scenario: An event belongs only to the current recovery or virtual-display session
      Given a display configuration, device arrival, device removal, device-nodes-changed, automatic-resume, or monitor-on event arrives
      When it is not stale and the current session is eligible for recovery or virtual-display supervision
      Then it may reopen recovery or begin the applicable current-session virtual-display check
      But an irrelevant, unarmed, superseded, or completed session treats the event as a no-op

    Scenario: A Windows display-event burst retains its notification-time session ownership
      Given a current recovery or virtual-display session is eligible to receive Windows display events
      When one or more applicable Windows display events arrive before the default 500 ms event quiet window ends
      Then the burst is handled as one eligible event opportunity after that window
      And that opportunity retains the recovery or session identity that owned the notification when it arrived
      And changing the quiet window may not relabel an old event for a newer Apply, start a stale restore, or bypass cancellation and target safety

    Scenario: A changed virtual identity during Apply is coalesced before delayed reapply
      Given a current virtual-display Apply or verification observes a changed usable virtual identity
      And no other display mutation is active
      When display events for that change arrive less than the default 250 ms apart
      Then they produce at most one reapply restart for that interval
      And the applicable restart begins after the default 100 ms delay
      And the reapply first retargets the current virtual identity
      And cancellation, a stale event, a same-identity event, or a newer session cannot use that delayed restart to mutate another session's desktop

  Rule: Durable recovery survives helper loss but is cleaned up safely

    Scenario: The durable restore safeguard is armed before a possible desktop mutation
      Given a request is about to alter topology, display settings, or reset a virtual display
      When Windows may first observe that mutation
      Then a durable restore safeguard is armed before the mutation boundary
      And the safeguard starts the helper in restore mode at a later user logon if needed
      And it works for a resolved interactive user or a safe all-users logon fallback

    Scenario: Cancellation before the durable mutation boundary may clear provisional recovery
      Given durable recovery was prepared before a possible desktop mutation
      When cancellation arrives before Windows can observe that mutation
      Then the provisional durable safeguard may be cleared because the desktop was not changed

    Scenario: Cancellation after the durable mutation boundary retains recovery
      Given durable recovery was armed before a possible desktop mutation
      And Windows may already have observed that mutation
      When cancellation arrives before a safe terminal outcome
      Then the safeguard remains until a confirmed recovery, verified replacement, explicit safe disarm, or reset owns the desktop
      And a restore task that cannot be armed never permits a claim that the desktop is safe merely because in-process recovery remains available

    Scenario: Failure to arm durable recovery does not masquerade as a safe desktop
      Given a display mutation may have occurred
      When the durable restore safeguard cannot be created
      Then the helper retains in-process recovery protection and its recovery-required state
      And it does not treat the session as safely restored or safely disarmed solely because task creation failed

    Scenario: Durable restore targets the affected resolved user
      Given a durable restore safeguard is required at a later logon
      When the affected interactive user can be resolved
      Then that user's later logon is eligible to start recovery
      And an unrelated user's logon does not stand in for the resolved user

    Scenario: Durable restore has a safe no-user fallback
      Given a durable restore safeguard is required at a later logon
      When no affected interactive user can be resolved
      Then the next ordinary user logon is eligible to start recovery without relying on localized account-name text
      And the safeguard remains available when logon is delayed or the device is running on battery

    Scenario: Durable safeguard cleanup is idempotent
      Given recovery has reached a safe terminal outcome and no durable safeguard exists
      When cleanup is requested again
      Then cleanup succeeds as a no-op
      And it does not recreate recovery work or change the confirmed desktop

    Scenario: A confirmed recovery cleans up only the safeguards it no longer needs
      Given recovery has restored a saved baseline and validation succeeds
      When the desktop is confirmed to match the restored baseline
      Then heartbeat monitoring and retry scheduling are disarmed
      And the durable restore safeguard is removed
      And the helper refreshes the Windows shell so the restored desktop is visible coherently

    Scenario: Durable cleanup preserves the recovery fallback ordering until confirmation
      Given recovery has eligible Current, Previous, and Golden fallback candidates
      When a candidate fails or remains unconfirmed
      Then recovery preserves the configured candidate order and durable safeguard for the next eligible fallback or event-driven retry
      And it does not delete the durable safeguard, Current, or Previous state merely because an earlier candidate was attempted

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
      And after disabling the driver it waits the default 500 ms before re-enabling it
      And the driver is re-enabled before the requested configuration is retried
      And after re-enabling the driver it waits the default 1,000 ms before Apply continues
      And a virtual-display-reset-needed result may start another reset only after the default 30-second reset cooldown, rather than cycling the driver indefinitely

    Scenario: A canceled or failed virtual-display reset leaves a recoverable outcome
      Given a protected virtual-display reset has disabled the driver
      When the reset is canceled, its wait fails, or the following apply fails
      Then the helper attempts to re-enable the disabled virtual driver before reporting the failure
      And the desktop remains protected for recovery even if re-enabling the driver fails
      And cancellation during either the 500 ms disable wait or 1,000 ms re-enable wait does not begin the following Apply
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

    Scenario: A matching virtual identity is recognized conservatively
      Given virtual-display supervision needs to resolve a Windows display identity
      When an available display has the SudoVDA friendly name or EDID manufacturer "SMK" with product code "D1CE"
      Then it recognizes that display as a compatible virtual display
      And it prefers an active primary compatible identity when more than one compatible identity is available
      And recovery-side supervision respects the separately configured virtual re-enable cooldown without using a cooldown as health confirmation

    Scenario: No matching virtual identity does not create a target
      Given virtual-display supervision needs to resolve a Windows display identity
      When no available display has the SudoVDA friendly name or EDID manufacturer "SMK" with product code "D1CE"
      Then it does not invent a virtual identity or retarget a physical display
      And recovery-side supervision treats the unresolved virtual target as unavailable rather than healthy

  Rule: Platform-visible cleanup happens only for confirmed display outcomes

    Scenario: Successful verification refreshes shell and conditionally blanks HDR
      Given an Apply has been verified successful for the active, current session
      When it completes its platform-visible cleanup
      Then the shell is refreshed after that verified display change
      And if the session requested HDR blanking, HDR states are temporarily blanked after verification using the default 1-second duration rather than for every intermediate Apply attempt
      And if the session did not request HDR blanking, it does not blank HDR states
      And an HDR-workaround failure leaves the verified Apply result unchanged
      And a confirmed restore refreshes the shell only after it validates the restored desktop
      And shell refresh is best effort and does not hold the verified Apply or restore result open
      But stale, cancelled, failed, or unverified Apply and restore completions do not blank HDR or refresh the shell

    Scenario: Consecutive HDR blanking requests remain temporary and non-overlapping
      Given a verified current session is within a requested temporary HDR-blanking interval
      When a later verified current session also requests HDR blanking
      Then the later workaround does not overlap the earlier blanking interval
      And each accepted workaround returns HDR state handling to its normal path after its configured temporary duration
      And failure of either workaround does not revise either verified Apply result
