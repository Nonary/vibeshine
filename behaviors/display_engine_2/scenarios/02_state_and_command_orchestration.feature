@display-engine-v2
Feature: Display engine v2 state and command orchestration
  The display engine keeps each display-changing session coherent while newer
  client intent, delayed outcomes, and recovery protection overlap.

  Rule: A new apply request establishes the current session intent

    Scenario: An apply request without a configuration is rejected without changing the session
      Given the display engine is waiting for a session request
      When a client applies a request that has no display configuration
      Then the client receives an invalid-request apply result for that request
      And the engine remains ready for a later valid request

    Scenario: A valid apply request supersedes unfinished autonomous recovery
      Given recovery protection is pending from an earlier session
      When a client submits a valid apply request
      Then the new request becomes the current session intent
      And unfinished recovery work cannot restore the desktop in place of that new intent
      And the request's restore-on-disconnect and snapshot-exclusion choices take effect before later recovery decisions

    Scenario: A later session intent supersedes an older deferred session intent
      Given a display-changing request is still being completed
      And an apply, revert, or disarm intent is already waiting behind it
      When a client submits a newer apply, revert, or disarm intent
      Then only the newer session intent remains to be honored after the active change reaches a safe boundary
      And an ordered reset request remains a barrier rather than being discarded as an older session intent

    Scenario: Replacement intent waits until a possibly mutating change reports its outcome
      Given a display-changing request may already have changed the desktop
      When a replacement apply, revert, or disarm request arrives
      Then the replacement request cancels obsolete future work where safe
      But it does not take control until the active change reports whether the desktop was touched
      And any required recovery protection is retained before the replacement request begins

    Scenario: A command from a retired session cannot alter the current session
      Given a newer client session owns the display engine
      When a command from an earlier session arrives
      Then the command is ignored
      And it cannot mutate display state, recovery protection, or the current request

  Rule: Apply outcomes are confirmed before a capture-gated client proceeds

    Scenario: A successful apply receives an initial verification gate
      Given a valid apply request is current
      When the requested display change completes successfully
      Then the client receives one successful apply result for its request
      And the engine verifies that the requested result is active before opening that request's verification gate
      And a successful initial verification establishes a live, protected session

    Scenario: A failed initial verification is repaired within the allowed settling attempts
      Given a valid apply request has not yet passed its initial verification gate
      When an initial verification reports that the requested result is not active
      Then the engine makes the allowed immediate or delayed repair attempts
      And it verifies each repaired result before treating the session as confirmed
      And the client receives a failed verification result if no allowed repair confirms the request

    Scenario: A capture-gated request may omit only the final initial HDR repair
      Given an HDR apply request is waiting to open a capture gate
      And the request asks to omit the final initial HDR reapply
      When earlier apply or verification attempts have not confirmed the result
      Then the engine does not delay the initial gate for that final HDR repair
      And it reports the terminal verification outcome for the originating request
      And later post-confirmation stabilization may still use its allowed repairs

    Scenario: A retryable apply outcome is retried only within policy
      Given a valid apply request is current
      When the display operation reports a retryable outcome or a verification-related failure
      Then the engine retries the request only while an allowed attempt remains
      And it reports the terminal apply and verification outcomes when the retry allowance is exhausted

    Scenario: A virtual-display reset outcome is handled before ordinary failure completion
      Given a valid apply request requires a virtual display
      When the display operation reports that a virtual-display reset is needed
      Then the engine performs the allowed reset-and-reapply policy before treating the request as terminal
      And the eventual result remains associated with the original request

    Scenario: A virtual HDR request may fall back to SDR after bounded failed attempts
      Given a virtual-display apply request asks for HDR
      And the allowed HDR attempts have been exhausted without confirmation
      When the request is still eligible for the one-time HDR fallback
      Then the engine retries the display request with effective SDR
      And it still requires confirmation before reporting success

    Scenario: A failure after a possible mutation keeps the desktop protected
      Given an apply attempt may already have changed the desktop
      When the attempt reaches a fatal or exhausted failure outcome
      Then the engine retains or enters recovery protection until a safe terminal outcome is confirmed
      And it does not present the display as an unprotected idle session merely because the request failed

    Scenario: A failure before any display mutation does not leave provisional session state behind
      Given an apply attempt is rejected or fails before changing the desktop
      When no retry will be made
      Then the engine reports the failure for that request
      And it clears provisional session state that could incorrectly affect a later independent apply request

    Scenario: A successful verified session is not revoked by a later best-effort settling repair failure
      Given an apply request has already passed its verification gate
      When a later stabilization repair cannot be confirmed
      Then the original successful verification result remains valid
      And the engine preserves the live session and its recovery protection

  Rule: Stale epoch or generation work produces no current-session transition or reply

    Scenario: A stale apply or verification completion cannot open a newer request's gate
      Given a newer apply request has superseded an earlier request
      When a delayed apply or verification completion for the earlier request arrives
      Then the completion cannot change the current session state
      And it cannot send an apply or verification result for the newer request
      And the newer request keeps its own request identity and session ownership

    Scenario: A stale session result is not delivered as a current-session reply
      Given a request belongs to a retired client session or cancellation generation
      When an asynchronous result for that request becomes available
      Then it produces no current-session transition or reply
      And it cannot be mistaken for a result of the current request

    Scenario: A cancelled apply that may have mutated the desktop still reports its safety boundary
      Given an apply request is cancelled by newer intent
      When its completion shows that the display may have changed
      Then recovery protection is retained for the desktop
      And the newer intent waits for that safety outcome before it is honored

    Scenario: A stale callback cannot respond through a replacement client session
      Given a request's client session has been replaced
      When a delayed result or callback from the retired session occurs
      Then it cannot transition the active session
      And it cannot open a response gate or deliver a result through the replacement session

    Scenario: A refresh-rate change invalidates older settling checks for its active request
      Given a verified apply request has a pending stabilization check
      When an allowed refresh-rate change targets that request's display scope
      Then older settling checks cannot decide the outcome using the pre-refresh request
      And the engine restarts the applicable settling verification for the refresh result

  Rule: Post-apply stabilization protects a session after its first confirmation

    Scenario: A confirmed session is checked again while Windows settles
      Given an apply request has passed its initial verification gate
      When the display may still be settling after the change
      Then the engine performs its bounded stabilization checks
      And a failed stabilization check causes an allowed repair and recheck
      And a successful stabilization check preserves the confirmed session

    Scenario: A recent disconnect uses the transient settlement path instead of an ordinary repair race
      Given a connection breaks immediately after an apply request begins
      And the session is eligible to defer disconnect recovery
      When the in-progress apply or its initial verification reaches a decision point
      Then the engine checks whether the requested display result stuck before restoring the desktop
      And it repairs only when those bounded checks show that the result did not stick
      And the transient checks do not race ordinary post-apply stabilization work

  Rule: Non-apply commands preserve the same mutation and response boundaries

    Scenario: An explicit revert requests recovery even when disconnect restoration is disabled
      Given a live session has disabled automatic restore on disconnect
      When the client explicitly requests a revert
      Then the engine begins recovery after any active display mutation reaches a safe boundary
      And the explicit revert is not discarded as a disconnect-policy decision

    Scenario: A revert with no available restore candidate ends cleanly
      Given no restore candidate is available for the session
      When a revert is requested
      Then the engine does not retry restoration indefinitely
      And it completes the no-op recovery lifecycle cleanly

    Scenario: Disarm does not abandon an unconfirmed restore
      Given an attempted restore has not yet been confirmed
      When a client requests disarm
      Then the engine keeps the restore protection and does not cancel that unconfirmed restore
      And a later safe recovery outcome determines when protection may be removed

    Scenario: Disarm clears an ordinary protected session
      Given no unconfirmed mutation or restore is pending
      When a client requests disarm
      Then the engine cancels obsolete work
      And it clears recovery protection and returns to a waiting session state

    Scenario: Snapshot and golden export do not overwrite a changing display baseline
      Given an apply or revert is actively changing the display
      When a client requests the current snapshot or golden export
      Then the engine does not capture a baseline from the changing desktop
      And a current-snapshot request receives its correlated failure result

    Scenario: Snapshot current applies requested exclusions and returns a correlated outcome
      Given no display mutation or recovery is active
      When a client requests a current snapshot with an exclusion update
      Then the updated exclusion policy is used for the snapshot
      And the client receives the success or failure result for the same request identity

    Scenario: Golden export is accepted only while the desktop is stable
      Given no display mutation or recovery is active
      When a client requests a golden baseline export
      Then the engine captures the golden baseline using the current exclusion policy
      And a successful export makes the new golden baseline eligible for future recovery decisions

    Scenario: Refresh rate rejects invalid or conflicting mutation requests
      Given a client asks to change a refresh rate
      When the request has no device, a zero rate component, or conflicts with an active display mutation
      Then the engine returns a failed refresh-rate result for that request
      And it does not let the refresh request race the active display transaction

    Scenario: Refresh rate is serialized and correlated when it is allowed
      Given the display engine is not applying or restoring
      When a client requests a valid refresh-rate change
      Then the refresh change is serialized as a display mutation
      And the client receives its success or failure result with the originating request identity
      And a successful change in the active request's scope becomes part of later settling behavior

    Scenario: Reset waits for a display mutation without cancelling it
      Given a display-changing request is active
      When a client requests reset
      Then reset waits until that display mutation reaches its completion boundary
      And it does not cancel the active display change or discard its recovery protection

    Scenario: A failed reset does not admit deferred display mutations into stale state
      Given reset is clearing staged display-session state before a deferred mutation
      When reset fails
      Then the engine does not start the deferred display mutation against that stale state
      And it ends the current helper lifecycle so a fresh session can start cleanly

    Scenario: Ping maintains the current live session without replacing its intent
      Given a live display session is awaiting liveness confirmation
      When the client sends ping
      Then the session liveness is refreshed
      And the ping does not replace the current display request or its recovery policy

    Scenario: Stop ends the helper lifecycle without accepting further work
      Given the display engine is running
      When the client requests stop
      Then the helper begins a clean exit
      And it does not continue as an active session after exit is requested
