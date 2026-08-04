@display-engine-v2
Feature: Display engine v2 client integration and stream lifecycle
  Sunshine builds, dispatches, and supervises display-helper work so a stream
  receives a bounded, session-correct display outcome.

  Rule: Session requests preserve the intended display behavior

    Scenario: A request marked Skip is not dispatched
      Given a session cannot produce a usable display request
      When Sunshine builds the display-helper request
      Then it does not send a display command
      And the caller receives a safe unsuccessful apply outcome

    Scenario: A failed virtual-display setup does not alter active displays
      Given a session's virtual display could not be initialized
      When Sunshine builds its display request
      Then it declines to dispatch display configuration work
      And the existing desktop remains the capture fallback

    Scenario: A disabled physical display configuration with an output override remains a capture-target choice
      Given a physical session has a display-output override
      And display configuration is disabled for that session
      When Sunshine builds the session request
      Then it does not create a display-helper apply request
      And the output override remains a capture-target choice rather than an unintended display mutation

    Scenario: A configuration-disabled session requests a revert when source behavior requires restoration
      Given a session resolves to the configured restoration behavior instead of an apply configuration
      When Sunshine builds the session request
      Then the request is a revert intent
      And it does not carry a fabricated display configuration

    Scenario: A session-derived request preserves its display choices
      Given a stream session supplies a target configuration and display preferences
      When Sunshine builds an apply request
      Then the request carries the intended target configuration and session overrides
      And it carries applicable virtual arrangement, topology, placement, and physical refresh preferences

    Scenario: An apply request carries safety choices that affect its session
      Given a display apply has a valid configuration
      When Sunshine dispatches it to the selected helper
      Then the request includes applicable HDR handling, golden restore preference, disconnect policy, and snapshot exclusions
      And a stream-start request asks for bounded capture-verification behavior

    Scenario: A virtual session keeps its resolved target identity before dispatch
      Given a virtual display session has a resolved virtual display identity
      When Sunshine prepares its apply request
      Then the identity is retained for the session and baseline-exclusion policy
      And later session cleanup can distinguish that virtual target from physical displays

    Scenario: A successful apply publishes only the successful session's display state
      Given a session apply succeeds
      When Sunshine records the active display session
      Then it records that session's effective target, dimensions, frame rate, HDR choice, and virtual-display choice
      And a failed or cancelled apply does not publish those values as the active session

    Scenario: A virtual session enables its requested virtual-display supervision
      Given a successful session apply requests virtual-display supervision
      When Sunshine adopts the session display state
      Then virtual-display supervision is enabled for that session
      And the supervision remains associated with the session that requested it

  Rule: Engine selection and helper availability are safe and bounded

    Scenario Outline: The selected engine follows the user's setting and release channel
      Given the display-helper setting is <selection>
      And Sunshine is running a <release channel> build
      When Sunshine starts a helper for a display request
      Then it uses the <selected engine> helper for that control session
      And it records that concrete engine choice for a later restore launch

      Examples:
        | selection | release channel | selected engine |
        | v2        | stable          | v2              |
        | legacy    | prerelease      | legacy          |
        | automatic | prerelease      | v2              |
        | automatic | stable          | legacy          |

    Scenario: A valid explicit apply requests helper availability
      Given a valid display apply request was built for a session
      When Sunshine dispatches that apply
      Then it attempts to reuse or start the helper needed to service the request
      And the caller receives helper unavailability if the helper cannot become ready

    Scenario: A revert may start a helper even when normal display control is disabled
      Given restoration is explicitly requested
      When Sunshine submits the revert
      Then it may start the helper needed to deliver that restoration intent
      And it returns whether the helper accepted the request

    Scenario: A responsive existing helper is reused
      Given a compatible helper is already running and responsive
      When Sunshine needs to dispatch a new request
      Then it reuses that helper as the current control service
      And it does not create a competing helper instance

    Scenario: A nonresponsive helper is not trusted as ready
      Given an existing helper appears available but does not pass its bounded liveness check
      When Sunshine needs display control
      Then it stops relying on the stale control path
      And it reports the helper unavailable until a safe restart or later recovery succeeds

    Scenario: A stream-start restart avoids interrupting an unconfirmed restore
      Given a stream-start request finds an unconfirmed restore still in progress
      When Sunshine prepares the new apply
      Then it allows the new apply to supersede the restore through the live helper
      And it does not prematurely terminate the helper and strand an uncertain desktop

    Scenario: A stream-start request can restart a stale helper when no restore must be preserved
      Given a stream-start request finds a stale helper with no unconfirmed restore to protect
      When the helper cannot prove prompt liveness
      Then Sunshine attempts a safe replacement of the stale helper
      And it proceeds only if the replacement helper becomes ready within the caller's budget

    Scenario: Repeated helper start failures enter a cooldown
      Given helper startup has failed repeatedly without a successful start
      When another request attempts to start the helper during the cooldown
      Then Sunshine returns helper unavailability without a hot restart loop
      And a later successful helper start clears the failure condition

    Scenario: A single recent helper start failure still receives its allowed retry
      Given a previously healthy display-helper path has one start failure
      When Sunshine immediately needs display control again
      Then it allows one immediate retry before cooldown suppression applies

    Scenario: A system context without a user session does not apply to an unsafe desktop
      Given Sunshine is running as the system account without an interactive user session
      When an apply request needs display access
      Then Sunshine prefers the helper path appropriate for that context
      And if that path is unavailable it does not use an unsafe direct display fallback

    Scenario: V2-only helper settings are synchronized only with a selected v2 helper
      Given a helper selected to run the v2 engine has become ready
      When Sunshine synchronizes helper verbosity
      Then it sends the v2-only setting only to that v2 helper
      And reconnecting allows the new helper session to receive its applicable setting

  Rule: Client communication keeps replies attributable to the live request

    Scenario: A v2 apply acknowledgement creates a correlated verification ticket
      Given Sunshine dispatches a valid apply to a v2 helper
      When the helper accepts the apply with a matching v2 acknowledgement
      Then Sunshine records a verification ticket for that exact apply and live helper session
      And later verification must match that ticket before it can affect capture

    Scenario: A legacy apply acknowledgement remains compatible but is not verification proof
      Given Sunshine dispatches an apply to a legacy-compatible helper
      When the helper returns its untagged acknowledgement
      Then Sunshine preserves the compatible synchronous result behavior
      And it does not claim that a separately correlated v2 verification is available

    Scenario: The live apply reply determines response compatibility
      Given helper configuration has changed or a helper was reused
      When Sunshine receives the first apply acknowledgement for the live connection
      Then it determines v2 or legacy response compatibility from that reply
      And it does not infer the live response format only from the configured engine preference

    Scenario: A reply for another request is never consumed as the current result
      Given a current request is awaiting a correlated result
      When a response for a different request becomes available
      Then Sunshine preserves that response only for its matching operation
      And it continues waiting for the current request's own result or deadline

    Scenario: A superseding control request cannot inherit an outstanding untagged reply
      Given a helper has not established v2 response correlation
      And an earlier control request may still have an untagged reply outstanding
      When Sunshine sends a superseding control request
      Then it isolates the successor request from the earlier untagged reply
      And the earlier reply cannot be mistaken for the successor's result

    Scenario: A cancelled or expired apply returns without publishing stale success
      Given an apply has a cancellation condition or caller deadline
      When cancellation is requested or the deadline expires before a matching acknowledgement
      Then Sunshine returns an unsuccessful apply outcome
      And it does not publish active-session state or a verification ticket for that obsolete work

    Scenario: A missing bounded apply acknowledgement retires its stale connection
      Given a bounded apply was sent to a v2-capable helper
      When its matching acknowledgement does not arrive before the caller deadline
      Then Sunshine returns safely within the bounded operation
      And it retires the connection so a late acknowledgement cannot answer a newer request

    Scenario: Resetting a client connection invalidates outstanding waits
      Given an apply or verification wait belongs to a cached helper connection
      When Sunshine retires that connection for failure, replacement, or explicit reset
      Then the outstanding wait returns unavailable rather than accepting a stale reply
      And a later request establishes a new connection identity

    Scenario: A normal liveness probe may establish a healthy helper connection
      Given Sunshine needs an ordinary liveness check
      When it sends the probe within its normal caller budget
      Then it may use a healthy connection or establish one if time permits
      And probe success means only that the liveness request was sent along a healthy send path

    Scenario: A fast liveness probe never spends stream-start time reconnecting
      Given a latency-sensitive stream-start or restore-disarm path needs a fast probe
      When no already-live helper connection is available
      Then the probe fails promptly
      And it does not perform a slow connection setup within that fast budget

  Rule: Caller-side operations share bounded and compatible semantics

    Scenario: A latency-sensitive stream-start apply never enters an unbounded fallback
      Given a stream-start apply cannot reach a ready helper within its shared deadline
      When helper dispatch fails or is cancelled
      Then Sunshine returns a safe unsuccessful apply outcome
      And it does not enter the potentially blocking in-process fallback path

    Scenario: An ordinary uncancelled apply may use the supported local fallback
      Given a non-stream-start apply has no cancellation condition
      And the helper is unavailable outside the system-without-user-session case
      When the session has the context required by the supported fallback
      Then Sunshine may apply through the supported local display path
      And it reports whether that fallback completed successfully

    Scenario: A bounded revert shares one caller deadline
      Given a stream-start or shutdown caller requests revert with a deadline
      When helper connection or command dispatch is delayed
      Then Sunshine stops waiting when the shared deadline expires
      And it does not leave the caller in an unbounded revert path

    Scenario: A prompt disarm uses only a live helper connection
      Given a stream-start path must quickly cancel very recent restore activity
      When no cached helper connection is immediately usable
      Then Sunshine reports that disarm was not delivered within the prompt budget
      And it does not spend the stream-start budget on connection setup

    Scenario: A failed prompt disarm does not leave recent restore activity uncontrolled
      Given a recent restore is expected and prompt disarm could not be delivered
      When intervention is needed to prevent uncontrolled restore activity
      Then Sunshine stops the stale helper rather than allowing uncontrolled restore activity to continue
      And it clears only the restore expectation associated with that intervention

    Scenario: An older restore is allowed to finish instead of being overwritten by prompt disarm
      Given restoration has progressed beyond the short startup grace period
      When a later stream-start path attempts prompt disarm
      Then Sunshine does not cancel that unconfirmed restore through the prompt path
      And the next apply or confirmed recovery determines the desktop outcome

    Scenario: Snapshot Current is skipped while an unconfirmed restore is expected
      Given a live helper is expected to be restoring the desktop
      When Sunshine asks to snapshot the current display state
      Then the snapshot request returns unavailable
      And it does not capture a baseline from the potentially unrestored desktop

    Scenario: A known v2 Snapshot Current waits for its matching result
      Given the live helper is known to support v2 responses
      When Sunshine requests Snapshot Current without a caller deadline
      Then it waits for the matching snapshot outcome
      And it returns failure if that matching outcome does not arrive

    Scenario: A legacy or unknown Snapshot Current remains dispatch-only
      Given the live helper has legacy or unknown response compatibility
      When Sunshine requests Snapshot Current without a caller deadline
      Then it returns the dispatch outcome without inventing a correlated completion
      And a later apply retains its own baseline-capture safety path

    Scenario: A bounded Snapshot Current cannot consume the whole stream-start budget
      Given Snapshot Current is part of a bounded stream-start procedure
      When the helper is slow or unavailable
      Then Sunshine bounds snapshot connection, dispatch, and any v2 completion wait
      And it returns safely so later stream-start work can use the remaining budget

    Scenario: A missing bounded snapshot result cannot contaminate a later request
      Given a bounded v2 Snapshot Current was sent
      When its matching result does not arrive before its bounded wait ends
      Then Sunshine returns failure
      And it retires the stale connection unless the operation was superseded

    Scenario: Refresh-rate control is validated and correlated when supported
      Given a caller requests a nonzero refresh rate for a named display
      When the live helper supports correlated refresh results
      Then Sunshine waits for that refresh request's matching result
      And another display-control request can supersede the wait without receiving its result

    Scenario: Legacy refresh-rate control cannot borrow a v2 result
      Given the live helper uses legacy refresh-rate behavior
      When Sunshine requests a valid refresh-rate change
      Then it uses the compatible untagged result path
      And it does not accept a correlated result intended for another operation

    Scenario: Caller maintenance commands report helper availability without fabricating success
      Given Sunshine requests golden export, persistence reset, disarm, or stop
      When the helper cannot be reached or the command cannot be dispatched
      Then the caller receives failure for that command
      And no command is reported successful merely because a prior helper session existed

    Scenario: Log-level synchronization is scoped to the live helper connection
      Given Sunshine has already synchronized the requested v2 log level to the live helper connection
      When the same log level is requested again
      Then Sunshine need not repeat the control dispatch for that live connection
      And a later connection replacement makes the setting eligible for synchronization again

  Rule: Deferred resolution work runs only for the intended live session

    Scenario: System-context resolution work is deferred only when a user session is unavailable
      Given an apply includes a session resolution request
      And Sunshine is running as the system account without an interactive user session
      When direct display work is unavailable
      Then Sunshine may defer that eligible resolution apply
      And it does not apply the resolution to an arbitrary system-context desktop

    Scenario: Ineligible requests are not placed into deferred resolution work
      Given an apply has no session or no resolution change
      When helper dispatch is unavailable
      Then Sunshine does not create deferred resolution work for that request
      And it returns the immediate caller outcome

    Scenario: Deferred resolution work retains session intent without retaining a dead session
      Given an eligible resolution apply is deferred
      When the original session object later becomes unavailable
      Then the deferred work retains only the display-relevant session intent
      And it cannot dereference or publish a stale session as active

    Scenario: A lock-screen or temporarily unavailable display path may defer eligible resolution work
      Given an eligible session resolution apply cannot complete because the display path is temporarily unavailable
      When the system context lacks interactive display access or the temporary display condition makes that work deferrable
      Then Sunshine retains one pending resolution request for later readiness
      And a newer pending request replaces an older pending request for the same deferred path

    Scenario: Deferred work waits for an interactive session and a still-live stream
      Given a resolution apply is pending
      When an interactive user session becomes available
      Then Sunshine delays and retries according to the bounded readiness policy
      And it claims the request only while the associated stream is active or pending

    Scenario: Session teardown removes orphaned deferred work
      Given a resolution apply is pending for a stream that ends
      When session teardown or a superseding normal apply or revert occurs
      Then Sunshine removes the pending work
      And it does not later change the desktop for that ended or superseded session

    Scenario: Cancellation leaves deferred work safe for the owning lifecycle
      Given Sunshine has claimed a deferred resolution apply
      When its owning shutdown path cancels the attempt before a terminal outcome
      Then Sunshine does not publish a stale successful session
      And it preserves or clears the deferred request only according to current session ownership

    Scenario: Deferred resolution retries stop after their allowed attempts
      Given a live deferred resolution apply repeatedly fails after readiness
      When its retry allowance is exhausted
      Then Sunshine removes the pending request
      And it does not retry indefinitely or resurrect the ended stream's display intent

    Scenario: A successful deferred apply is adopted by the correct active stream
      Given a deferred resolution apply succeeds while its stream is still live
      When Sunshine publishes the successful display session
      Then ongoing supervision is associated with that session's current identity
      And a stale cleanup from an earlier session cannot remove it

  Rule: Stream capture uses verification as a bounded gate, not an assumed success

    Scenario: A stream-start apply failure preserves the existing-display capture fallback
      Given a stream-start display request cannot be applied through the helper
      When stream startup continues after the unavailable display configuration
      Then Sunshine uses the existing display as the capture fallback
      And it does not report the unavailable display request as applied or verified

    Scenario: A verification ticket is invalidated by a newer display-changing request
      Given a prior apply supplied a verification ticket
      When a newer apply, revert, disarm, or reset changes display-control intent
      Then the earlier ticket cannot verify the newer session
      And any wait for that ticket returns unavailable rather than current-session success

    Scenario: A v2 verification result must match the ticket and live helper session
      Given a stream is waiting on a v2 verification ticket
      When a verification result arrives
      Then capture-gate status changes only if the result matches the ticket's request and live session
      And an unmatched or superseded result is not treated as verification

    Scenario: A legacy, unavailable, or timed-out verification result is explicit unknown
      Given an apply has no attributable v2 verification result
      When Sunshine evaluates its verification ticket
      Then it reports the verification status as unknown rather than verified
      And callers can apply their bounded fallback policy explicitly

    Scenario: A verified stream-start ticket opens the capture gate
      Given a stream-start apply has a current v2 verification ticket
      When its matching verification succeeds within the stream-start budget
      Then the capture gate reports that capture may proceed
      And the result belongs only to that stream start

    Scenario: A failed stream-start verification is recorded without hard-blocking capture
      Given a stream-start apply has a current v2 verification ticket
      When its matching verification reports failure
      Then the capture gate reports a failed display target
      And the stream-start path starts capture after the bounded soft gate completes
      And it does not treat that result as capture-ready

    Scenario: An unknown stream-start verification completes the bounded gate without claiming success
      Given a stream-start apply has a legacy, unavailable, superseded, or timed-out verification result
      When the bounded gate completes
      Then the gate records that it gave up waiting rather than that the target was verified
      And the stream-start path proceeds with capture after that bounded soft gate

    Scenario: A recent HDR apply delays capture only until HDR is active or its bounded wait ends
      Given the most recent display apply requested HDR
      When capture is about to start
      Then Sunshine waits for the output to report HDR within the bounded HDR readiness window
      And it starts capture with the observed SDR fallback if HDR does not become active in time

    Scenario: A recent non-HDR display apply waits for verification or the bounded settle fallback
      Given display topology recently changed without a pending HDR request
      When capture is about to start
      Then Sunshine waits only until the apply is capture-stable or the bounded settle window ends
      And it does not extend capture delay indefinitely

    Scenario: Capture stability is proof only for the current eligible apply
      Given a display apply has been verified
      When Sunshine evaluates capture stability
      Then it accepts the stable indication only for the current apply generation and eligible exclusive virtual SDR session
      And prior or unrelated verification cannot mark the new session stable

    Scenario: Ordinary WebRTC preparation treats unverified completion as nonfatal but not verified
      Given a WebRTC display apply was accepted
      When its bounded verification is failed, unknown, or unavailable
      Then Sunshine continues capability preparation
      And it does not report the display target as verified

    Scenario: HTTP capability probing does not turn an unverified display gate into a failed capability response
      Given HTTP capability probing has a virtual-display session with pending, failed, unknown, or timed-out verification
      When it reaches its supplied display-gate deadline
      Then it continues adapter-scoped encoder capability probing without further display-gate delay
      And it does not report a capability failure solely because the display was not verified

  Rule: Helper watchdog lifecycle belongs to the active stream

    Scenario: Starting supervision reuses an existing watchdog for a newer active stream
      Given helper supervision is already running
      When a newer stream becomes the active display session
      Then supervision adopts the newer stream identity
      And Sunshine does not start a duplicate watchdog

    Scenario: A watchdog liveness failure reconnects without reapplying an old display request
      Given helper supervision detects that its current helper connection is unusable
      When it restores helper communication successfully
      Then it confirms helper reachability for ongoing supervision
      And it does not automatically replay a previous stream's display apply

    Scenario: Supervision pauses helper use while display control is disabled
      Given helper supervision is active
      When display control becomes disabled
      Then Sunshine releases the helper connection for that period
      And it does not keep restarting display control until the feature is enabled again

    Scenario: Ordinary watchdog stop preserves supervision for an active or pending replacement stream
      Given a prior stream asks to stop helper supervision
      And another stream is active or pending
      When the ordinary stop is processed
      Then Sunshine leaves supervision running for the live stream
      And it does not clear the new stream's display session

    Scenario: A stale watchdog stop cannot clean up a newer display session
      Given supervision ownership belongs to a newer active stream
      When a stop request from an older stream arrives
      Then Sunshine ignores that stale stop for the newer session
      And it preserves the newer session's helper connection and display state

    Scenario: Forced watchdog stop completes shutdown or explicit restoration cleanup
      Given product shutdown or explicit user-requested restoration requires forced cleanup
      When Sunshine stops helper supervision
      Then it completes the applicable supervision cleanup even if a normal stream remains visible
      And it clears only session state that belongs to the forced cleanup scope

    Scenario: Watchdog shutdown cleanup is never abandoned
      Given helper supervision is stopping or being replaced
      When its prior cleanup cannot finish immediately
      Then Sunshine retains responsibility for completing that cleanup safely
      And a later lifecycle transition cannot expose stale supervision as a healthy new session

  Rule: Read-only integration queries fail safely

    Scenario: No successful apply is treated as no recent display change
      Given Sunshine has not completed a successful display apply
      When capture preparation asks how recently display configuration changed
      Then it treats the prior apply as not recent
      And it does not add a settling delay for a nonexistent successful apply

    Scenario: Device enumeration returns no device list on query failure
      Given Sunshine cannot read the available display devices
      When a caller requests device enumeration
      Then the integration returns no device list
      And its JSON-facing form returns an empty list rather than partial invalid data

    Scenario: Current topology capture returns unavailable on query failure
      Given Sunshine cannot read the active display topology
      When a caller requests the current topology
      Then the integration returns no topology
      And it does not fabricate a topology for a later display request

    Scenario: Frame-generation refresh support reports unknown when display evidence is unavailable
      Given a caller asks whether a display supports requested frame-generation refresh targets
      When the target cannot be resolved or its display evidence is unavailable
      Then Sunshine returns unavailable or an explicit unknown support result
      And it does not claim unsupported or supported status without applicable evidence

    Scenario: Frame-generation refresh support evaluates each resolved target independently
      Given Sunshine resolves a display and can inspect its refresh evidence
      When a caller requests several target refresh rates
      Then Sunshine reports supported, unsupported, or unknown for each target rate
      And the result identifies the resolved display used for that evaluation
