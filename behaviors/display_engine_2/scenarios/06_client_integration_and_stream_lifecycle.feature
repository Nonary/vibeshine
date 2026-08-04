@display-engine-v2
Feature: Display engine v2 client integration and stream lifecycle
  Sunshine builds, dispatches, and supervises display-helper work so a stream
  receives a default-deadline, session-correct display outcome.

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
      And a stream-start request asks for capture-verification behavior within the 8-second shared default budget

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

  Rule: Session-derived virtual requests preserve layout, target, and mode intent

    Scenario Outline: Virtual layout determines activation, primary, and isolation intent
      Given a virtual-display session uses the <layout> layout
      When Sunshine builds its valid virtual Apply request
      Then the request asks to <activation intent>
      And the request <primary intent>
      And physical and virtual pointer space is <pointer-space intent>

      Examples:
        | layout                    | activation intent                 | primary intent                         | pointer-space intent                 |
        | exclusive                 | make the virtual target the only display | does not preserve a physical primary | not extended with physical displays |
        | extended                  | keep the virtual target active    | preserves a physical primary when needed | extended normally                    |
        | extended-primary          | keep the virtual target active    | makes the virtual target primary         | extended normally                    |
        | extended-isolated         | keep the virtual target active    | does not require virtual primary         | isolated safely                      |
        | extended-primary-isolated | keep the virtual target active    | makes the virtual target primary         | isolated safely                      |

    Scenario: A session-specific virtual layout overrides the configured layout
      Given the configured virtual layout differs from a session-specific layout
      When Sunshine builds the session's virtual Apply request
      Then it uses the session-specific layout intent
      And it does not merge activation, primary, or isolation intent from the configured layout

    Scenario Outline: Virtual intent may originate from the session, configured mode, or application metadata
      Given a session has virtual intent from <source>
      When Sunshine builds the display request
      Then it prepares virtual target behavior when a valid virtual configuration can be formed
      And it does not require all three virtual-intent sources to agree

      Examples:
        | source                         |
        | an explicit session request     |
        | a configured per-client mode    |
        | a configured shared mode        |
        | application virtual-screen metadata |

    Scenario: A resolved virtual target remains the target of the virtual Apply
      Given a virtual session has a resolved active virtual target identity
      When Sunshine creates the virtual Apply request
      Then the request and published session retain that virtual target identity
      And it does not redirect the request to an arbitrary physical target

    Scenario: Unavailable virtual identity produces no arbitrary display mutation
      Given a virtual session has no resolved virtual target identity
      And no configured output provides a valid fallback target
      When Sunshine cannot form a valid virtual Apply request
      Then it returns no display request or a safe unsuccessful outcome
      And it does not choose an arbitrary active physical display

    Scenario: A configured physical output is only a constrained fallback target
      Given virtual target resolution cannot identify a live virtual target
      And the configured output names a physical display
      When Sunshine builds the request
      Then it may retain that configured physical output as the explicit fallback target
      And it does not substitute another physical target when that explicit output is unavailable

    Scenario: A valid virtual Apply preserves effective dimensions and frame rate
      Given a virtual request has a valid target configuration and positive session dimensions or frame rate
      When Sunshine creates the Apply request
      Then it carries the effective requested dimensions and frame rate for that virtual target
      And published virtual session state uses the same effective values

    Scenario: Virtual frame-generation refresh never reduces the required display rate
      Given a virtual session has an active frame-generation refresh policy
      When the requested display rate is below the policy's required minimum
      Then Sunshine raises the virtual target rate to at least that minimum
      And it retains the session's frame-generation rate as session state

    Scenario: User-disabled virtual resolution or refresh changes are respected
      Given a virtual session has user-disabled resolution or refresh changes
      When Sunshine builds a valid virtual Apply request
      Then it does not add the disabled resolution or refresh override
      And it retains an already valid requested target mode unless another valid policy requires a minimum frame-generation rate

    Scenario: Virtual supervision is enabled only after a valid virtual Apply is built
      Given a session has virtual intent but its configuration cannot be formed
      When Sunshine builds the display request
      Then it does not enable virtual-display supervision or publish virtual Apply state
      And a valid virtual Apply is required before that supervision can belong to the session

  Rule: Standard display requests preserve mode-specific compatibility behavior

    Scenario: An unconfirmed virtual HDR target uses effective SDR
      Given a session captures from a virtual target that did not confirm HDR activation
      And the otherwise effective standard request asks for HDR
      When Sunshine builds the Apply request
      Then it requests SDR for that target while retaining the valid topology and mode intent
      And it does not claim that HDR is active for the session

    Scenario: RTX HDR uses an SDR display request
      Given RTX HDR is enabled for a standard display request
      When Sunshine builds the Apply request
      Then it requests SDR from the display
      And it leaves HDR conversion to the applicable stream behavior rather than claiming display HDR

    Scenario Outline: Non-desktop dummy-plug compatibility uses its limited HDR mode
      Given the dummy-plug HDR compatibility option is enabled
      And the session targets a <session kind>
      When Sunshine builds a standard Apply request without active frame-generation compatibility fixes
      Then <result>

      Examples:
        | session kind | result                                                              |
        | non-desktop  | it requests the limited 30 Hz HDR configuration                    |
        | desktop      | it does not impose that non-desktop limited configuration          |

    Scenario: Dummy-plug HDR with a frame-generation compatibility fix does not impose 30 Hz
      Given a non-desktop request uses dummy-plug HDR and an applicable frame-generation compatibility fix
      When Sunshine builds the standard Apply request
      Then it requests HDR without imposing the limited 30 Hz mode
      And the applicable frame-generation refresh behavior remains eligible

    Scenario Outline: Compatibility refresh uses the 10,000/1 target only for its eligible trigger
      Given a standard request has <eligible trigger>
      And dummy-plug HDR does not suppress that trigger
      When Sunshine builds the Apply request
      Then it requests the 10,000/1 compatibility refresh target
      And it does not add a user-disabled resolution change

      Examples:
        | eligible trigger                                                        |
        | a generation-one frame-generation compatibility fix                     |
        | a generation-two frame-generation compatibility fix                     |
        | VSync disabled with no NVIDIA GPU or unavailable NVIDIA control         |

    Scenario: Dummy-plug HDR suppresses best-effort refresh without a frame-generation fix
      Given dummy-plug HDR is enabled for a standard request
      And no frame-generation compatibility fix applies
      When Sunshine builds the Apply request
      Then it does not request the 10,000/1 compatibility refresh target solely from best-effort refresh conditions
      And it preserves the dummy-plug-specific mode behavior

    Scenario: Standard requests without refresh conditions preserve normal rate selection
      Given a standard request has no frame-generation compatibility fix and no eligible best-effort refresh condition
      When Sunshine builds the Apply request
      Then it uses the requested or configured normal refresh behavior
      And it does not invent a forced compatibility refresh

    Scenario: A physical output override with disabled configuration remains capture-only
      Given display configuration is disabled and a session explicitly selects a physical output
      When Sunshine builds the request
      Then it retains that output only as the capture choice
      And it does not apply, revert, or supervise a display change for that override

    Scenario: Disabled configuration otherwise requests restoration
      Given a standard display request has display configuration disabled
      And there is no physical-output capture-only exception
      And no valid non-desktop dummy-plug compatibility exception applies
      When Sunshine builds the standard request
      Then it requests Revert without a fabricated display configuration
      And it does not enable virtual-display supervision

    Scenario: The valid non-desktop dummy-plug exception builds only its limited configuration
      Given a standard display request has display configuration disabled
      And a non-desktop session meets the dummy-plug HDR compatibility conditions without a frame-generation compatibility fix
      When Sunshine builds the standard request
      Then it creates only the limited target configuration required by that compatibility behavior
      And it does not reinterpret disabled configuration as a general display-configuration request

  Rule: Session topology preserves explicit display relationships safely

    Scenario: A physical ensure-only request names only its explicit physical target
      Given a physical request explicitly asks to deactivate other displays
      And it has a nonempty explicit physical target identity
      When Sunshine supplies topology intent
      Then it names that physical target as the only display
      And it does not use virtual placement behavior for the physical session

    Scenario: An exclusive virtual layout names only its explicit virtual target
      Given a virtual session uses exclusive layout and has a nonempty target identity
      When Sunshine supplies topology intent without a saved extended topology
      Then it names that virtual target as the only display
      And it does not inject unrelated physical displays

    Scenario: An extended virtual layout merges a saved topology without duplicate virtual membership
      Given a nonexclusive virtual session has a saved topology and a resolved virtual target
      When Sunshine supplies topology intent
      Then it preserves the saved topology and adds the virtual target only if absent
      And it does not duplicate a target already present under case-insensitive identity matching

    Scenario: Extended non-primary layout restores physical primary safety after virtual creation
      Given extended non-primary layout finds that the virtual target became primary
      And physical display evidence is available
      When Sunshine supplies monitor placement intent
      Then it restores one physical display as primary and places the virtual target beside it
      And it leaves placement unchanged when the required display evidence is unavailable

    Scenario: Extended-primary layout does not impose non-primary physical restoration
      Given a virtual session uses extended-primary layout
      When Sunshine supplies monitor placement intent
      Then it preserves the virtual-primary intent
      And it does not apply the extended non-primary physical-primary correction

    Scenario: Isolated layout preserves physical layout while separating virtual pointer space
      Given a virtual session uses isolated non-primary extended layout
      When display evidence is available
      Then Sunshine preserves physical display positions
      And it places the virtual target safely outside their reachable pointer space

    Scenario: Primary isolated layout keeps the virtual target primary and separates physical pointer space
      Given a virtual session uses extended-primary-isolated layout
      And physical display evidence is available
      When Sunshine supplies monitor placement intent
      Then it keeps the virtual target at the primary origin
      And it moves physical displays into safely separated pointer space without coordinate overflow

    Scenario: Missing topology or display evidence leaves placement safe and partial
      Given required virtual identity, saved topology, or display evidence is unavailable
      When Sunshine cannot derive a safe placement adjustment
      Then it omits that adjustment rather than inventing a target or coordinate
      And it retains only independently valid topology intent

    Scenario: Pre-virtual refresh restoration carries physical target rates only
      Given a virtual session retained refresh rates from before virtual display creation
      When Sunshine builds topology-related refresh restoration intent
      Then it includes rates for physical targets only
      And it excludes the current virtual target rate from that restoration

  Rule: Engine selection and helper availability are safe and default-deadline limited

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
      Given an existing helper appears available but does not pass its 5-second ordinary liveness check
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

    Scenario: A missing deadline-limited apply acknowledgement retires its stale connection
      Given an apply was sent to a v2-capable helper with an explicit shared deadline
      When its matching acknowledgement does not arrive before the caller deadline
      Then Sunshine returns unsuccessful by that deadline
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

  Rule: Caller-side operations share default deadlines and compatible semantics

    Scenario: A latency-sensitive stream-start apply never enters an unlimited fallback
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

    Scenario: A deadline-limited revert shares one caller deadline
      Given a stream-start or shutdown caller requests revert with a deadline
      When helper connection or command dispatch is delayed
      Then Sunshine stops waiting when the shared deadline expires
      And it does not leave the caller in an unlimited revert path

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

    Scenario: A deadline-limited Snapshot Current cannot consume the whole stream-start budget
      Given Snapshot Current is part of a stream-start procedure with an explicit shared deadline
      When the helper is slow or unavailable
      Then Sunshine bounds snapshot connection, dispatch, and any v2 completion wait
      And it returns safely so later stream-start work can use the remaining budget

    Scenario: A missing deadline-limited snapshot result cannot contaminate a later request
      Given a deadline-limited v2 Snapshot Current was sent
      When its matching result does not arrive before its explicit wait ends
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
      Then Sunshine waits 2 seconds after readiness before its first attempt and retries at 500 milliseconds, doubling no higher than 10 seconds
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

  Rule: Stream capture uses verification as a default-deadline soft gate, not an assumed success

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
      And callers can apply their explicit default-deadline fallback policy

    Scenario: A verified stream-start ticket opens the capture gate
      Given a stream-start apply has a current v2 verification ticket
      When its matching verification succeeds within the stream-start budget
      Then the capture gate reports that capture may proceed
      And the result belongs only to that stream start

    Scenario: A failed stream-start verification is recorded without hard-blocking capture
      Given a stream-start apply has a current v2 verification ticket
      When its matching verification reports failure
      Then the capture gate reports a failed display target
      And the stream-start path starts capture after the 8-second shared soft gate completes
      And it does not treat that result as capture-ready

    Scenario: An unknown stream-start verification completes the 8-second gate without claiming success
      Given a stream-start apply has a legacy, unavailable, superseded, or timed-out verification result
      When the 8-second shared gate completes
      Then the gate records that it gave up waiting rather than that the target was verified
      And the stream-start path proceeds with capture after that soft gate

    Scenario: A recent HDR apply delays capture only until HDR is active or its 3-second wait ends
      Given the most recent display apply requested HDR
      When capture is about to start
      Then Sunshine waits for the output to report HDR until 3 seconds after the successful apply
      And it starts capture with the observed SDR fallback if HDR does not become active in time

    Scenario: A recent non-HDR display apply waits for verification or the 1.5-second settle fallback
      Given display topology recently changed without a pending HDR request
      When capture is about to start
      Then Sunshine waits only until the apply is capture-stable or 1.5 seconds after the successful apply
      And it does not extend capture delay indefinitely

    Scenario: Capture stability is proof only for the current eligible apply
      Given a display apply has been verified
      When Sunshine evaluates capture stability
      Then it accepts the stable indication only for the current apply request and eligible exclusive virtual SDR session
      And prior or unrelated verification cannot mark the new session stable

    Scenario: Ordinary WebRTC preparation treats unverified completion as nonfatal but not verified
      Given a WebRTC display apply was accepted
      When its default-deadline verification is failed, unknown, or unavailable
      Then Sunshine continues capability preparation
      And it does not report the display target as verified

    Scenario: HTTP capability probing does not turn an unverified display gate into a failed capability response
      Given HTTP capability probing has a virtual-display session with pending, failed, unknown, or timed-out verification
      When it reaches its supplied display-gate deadline
      Then it continues adapter-scoped encoder capability probing without further display-gate delay
      And it does not report a capability failure solely because the display was not verified

  Rule: Helper watchdog lifecycle belongs to the active stream

    Scenario Outline: Helper supervision uses the observed active and suspended check cadences
      Given display-helper supervision has <supervision condition>
      When Sunshine schedules its next liveness check
      Then it uses the default <check cadence>
      And a failed check still follows the same current-session ownership and reconnection rules

      Examples:
        | supervision condition                                      | check cadence |
        | an active stream or no remaining launched process          | 10 seconds    |
        | no active stream while one or more launched processes run  | 20 seconds    |

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

  Rule: Helper selection, readiness, and restart use observed caller defaults

    Scenario Outline: Engine selection persists the accepted setting and resolves automatic mode predictably
      Given the persisted display-helper engine setting is <stored setting>
      And the release channel is <release channel>
      When Sunshine prepares display control
      Then it selects <effective engine>
      And an invalid stored setting is treated as automatic rather than as a new engine choice

      Examples:
        | stored setting | release channel | effective engine |
        | v2             | stable          | v2               |
        | legacy         | prerelease      | legacy           |
        | automatic      | prerelease      | v2               |
        | automatic      | stable          | legacy           |
        | invalid        | prerelease      | v2               |
        | invalid        | stable          | legacy           |

    Scenario Outline: Display-control settings retain their observed product defaults
      Given a new installation has no saved value for <setting>
      When Sunshine creates a display-control request or lifecycle decision
      Then <setting> defaults to <default>
      And the user-visible default remains distinct from any helper wire default

      Examples:
        | setting                              | default                          |
        | display-helper engine                | automatic                        |
        | revert on client disconnect          | disabled                         |
        | always restore from golden           | enabled                          |
        | paused virtual-display cleanup delay | 7200 seconds                    |
        | permanent virtual-display count      | 0                               |
        | dummy-plug HDR workaround            | disabled                         |

    Scenario Outline: Helper readiness uses the caller class's default liveness envelope
      Given Sunshine needs a helper for a <caller class> operation
      When no usable live helper has yet answered its liveness request
      Then Sunshine permits readiness and liveness work for at most <default envelope>
      And cancellation or the caller's earlier deadline returns unavailable without later publishing success

      Examples:
        | caller class             | default envelope |
        | ordinary apply or probe   | 5 seconds        |
        | helper startup readiness  | 5 seconds        |
        | shutdown or teardown      | 250 milliseconds |
        | stream-start fast disarm  | 150 milliseconds |

    Scenario: A ready helper is reused only after current liveness succeeds
      Given the selected helper is already present
      When its 5-second ordinary liveness request succeeds
      Then Sunshine reuses that helper for the new eligible request
      And a failed, cancelled, or expired liveness request does not certify it as ready

    Scenario: Helper readiness failure returns unavailable after the default 5-second envelope
      Given an eligible request needs a helper that cannot become live
      When 5 seconds of readiness work elapse without liveness success
      Then Sunshine returns helper unavailable
      And it does not publish a display session, verification ticket, or successful maintenance result

    Scenario: Two consecutive start failures suppress restart for 30 seconds
      Given two consecutive helper starts have failed
      When another ordinary request arrives before 30 seconds have elapsed since the most recent failure
      Then Sunshine returns helper unavailable without another start attempt
      And a single failure does not enter that cooldown

    Scenario: A restart removes unsafe stale ownership before reuse
      Given a helper is nonresponsive and no protected restoration must continue
      When Sunshine restarts display control for a new request
      Then the earlier helper identity and unresolved replies are retired before the replacement is used
      And a later result from the retired identity cannot publish or verify the new request

    Scenario: Forced stopping waits no longer than 2 seconds for observed exit
      Given Sunshine must force-stop an obsolete helper
      When the stop is issued
      Then Sunshine waits up to 2 seconds for its exit before completing local cleanup
      And a missed exit observation does not let its late reply own later work

    Scenario: Virtual display re-enable is rate-limited for 3 seconds
      Given Sunshine has just re-enabled virtual-display support
      When another re-enable trigger arrives within 3 seconds
      Then Sunshine does not repeat the re-enable action
      And the rate limit does not claim that a different active session is healthy

  Rule: Direct callers preserve shared deadline and ownership rules

    Scenario Outline: Apply callers use the source-visible shared budget
      Given a <caller> begins a display apply and optional verification
      When it has no earlier explicit deadline
      Then acknowledgement and verification together share <default budget>
      And success after that budget cannot be published for that caller's session

      Examples:
        | caller                    | default budget |
        | RTSP stream start          | 8 seconds      |
        | RTSP stream resume         | 8 seconds      |
        | WebRTC preparation         | 15 seconds     |
        | ordinary non-stream apply  | 15 seconds     |

    Scenario: The RTSP capture consumer receives only one second beyond its producer budget
      Given RTSP stream start owns an 8-second apply-verification gate
      When capture waits for the gate
      Then capture waits at most 9 seconds from gate creation
      And a late gate result is recorded as gave-up rather than as verified capture readiness

    Scenario: A failed verification is explicit but does not hard-block RTSP capture
      Given RTSP capture is waiting for a current verification ticket
      When verification is failed
      Then the gate returns failed and capture still starts after the gate

    Scenario Outline: An unavailable RTSP verification is unknown or gave-up, not failed
      Given RTSP capture is waiting for a current verification ticket
      When verification is <unavailable condition>
      Then the gate returns unknown or gave-up and capture still starts after the gate

      Examples:
        | unavailable condition |
        | unavailable           |
        | legacy                |
        | cancelled             |
        | superseded            |
        | late                  |

    Scenario: WebRTC treats a nonverified apply as nonfatal without calling it verified
      Given WebRTC receives an accepted display apply
      When its 15-second shared verification budget ends failed, unknown, unavailable, or cancelled
      Then it continues its capability preparation when other prerequisites allow
      And it does not report the display request as verified

    Scenario: A direct local display fallback is forbidden for latency-sensitive stream start
      Given an RTSP stream-start display request cannot obtain helper service by its 8-second deadline
      When Sunshine chooses the safe caller result
      Then it returns an unsuccessful display apply and retains the existing capture target
      And it does not perform the potentially blocking direct display fallback

    Scenario: A direct local display fallback remains limited to eligible ordinary work
      Given a non-stream-start apply is uncancelled and helper service is unavailable
      When an interactive display context supports direct display work
      Then Sunshine may use that direct fallback and reports its actual result
      And a system context without an interactive user session never applies it to an arbitrary desktop

    Scenario: Prompt disarm uses a 150-millisecond budget and throttle
      Given stream start detects a very recent pending restoration
      When it asks the already-live helper to disarm it
      Then dispatch completes or fails within 150 milliseconds
      And repeated prompt disarm attempts within 150 milliseconds do not create repeated control intent

    Scenario: Prompt disarm respects the 5-second restoration grace boundary
      Given restoration has been pending for less than 5 seconds
      When stream start cannot deliver its prompt disarm
      Then Sunshine may stop the stale helper to prevent uncontrolled restoration

    Scenario: An older restoration is not overwritten by the prompt disarm path
      Given restoration has progressed for 5 seconds or more
      When a later stream start attempts prompt disarm
      Then Sunshine lets that restore finish or be superseded by explicit later display intent
      And it does not stop that restoration through the prompt disarm path

    Scenario: Deferred resolution starts after readiness and ends after six attempts
      Given eligible resolution work was deferred while no interactive user session existed
      When an interactive session becomes available and the stream remains active or pending
      Then Sunshine waits 2 seconds before the first attempt
      And failed attempts retry after 500 milliseconds doubled per attempt and capped at 10 seconds
      And Sunshine removes the deferred work after the sixth failed attempt

    Scenario: A newer lifecycle decision cancels deferred resolution ownership
      Given a deferred resolution apply exists for one stream
      When that stream ends or a normal apply, revert, or replacement deferred request supersedes it
      Then Sunshine removes or replaces the old deferred work before it can alter the desktop
      And an old completion cannot adopt watchdog supervision or active-session state

  Rule: Capture, probing, and supervision retain soft-gate outcomes

    Scenario: Capture settling retains explicit time and fallback outcome
      Given a successful non-HDR display apply was less than 1.5 seconds ago
      When capture initializes
      Then it waits only until the current apply is capture-stable or 1.5 seconds have elapsed
      And it starts capture after the fallback without marking an unverified apply stable

    Scenario: HDR capture settling preserves the observed SDR fallback
      Given a successful apply requested HDR and the output is not yet HDR-active
      When capture initializes
      Then it waits only until HDR becomes active or 3 seconds have elapsed since the apply
      And it starts capture using observed SDR behavior if HDR is still inactive

    Scenario: HTTP capability probing treats the display gate as nonfatal
      Given HTTP capability probing has a current virtual-display session
      When its supplied display-gate deadline ends pending, failed, unknown, unavailable, cancelled, or with an observation error
      Then it continues adapter-scoped encoder capability probing when the selected target is otherwise ready
      And it does not publish a capability failure solely from the uncertain display gate

    Scenario: HTTP probing does not disrupt stream lifecycle ownership
      Given encoder capability evidence is not already cached for the selected adapter
      When HTTP probing finds stream lifecycle work active, stopping, or unavailable for exclusive use
      Then it returns the currently known capabilities marked incomplete
      And it does not create, replace, or remove a display session for the probe

    Scenario: Encoder capability evidence remains scoped to the observed capture adapter
      Given a display target suggests an adapter but capability probing observes another adapter
      When encoder capabilities are recorded
      Then only the observed adapter may own a new successful capability result
      And a pending or virtual-display hint cannot create a cross-adapter positive result

    Scenario: Watchdog adoption and teardown retain current session ownership
      Given supervision changes from one active stream to another
      When the newer stream adopts supervision or the older stream stops
      Then only the current stream may retain supervision and its display state
      And a stale stop, late liveness result, or failed cleanup cannot remove the newer stream's ownership

    Scenario: Log synchronization is repeated only after helper replacement
      Given a selected v2 helper has received the current requested log level
      When the same level is requested on the same live helper identity
      Then Sunshine may omit a duplicate update

    Scenario: A replacement v2 helper receives the requested log level
      Given a selected v2 helper identity was replaced
      When Sunshine relies on the replacement for display control
      Then Sunshine synchronizes the requested level before relying on the replacement's logs
