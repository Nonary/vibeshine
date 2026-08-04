@display-engine-v2
Feature: Display engine v2 clean-room public contract and defaults
  Implementations may vary their internal design while preserving public
  vocabulary, default timing profiles, and the safety properties those defaults protect.

  Rule: Public outcomes remain distinguishable

    Scenario: A verified gate outcome is explicit verified success
      Given a caller receives a verified display-gate outcome
      When it chooses its documented next action
      Then it may treat only that current outcome as verified success
      And it does not transfer that verification to another request or session

    Scenario Outline: A nonverified caller result is not upgraded to verified success
      Given a caller observes <caller result> for a display operation or gate
      When it chooses its documented next action
      Then it can distinguish <caller result> from verified success
      And it does not label <caller result> as verified success

      Examples:
        | caller result |
        | failed        |
        | unknown       |
        | unavailable   |
        | cancelled     |
        | gave-up       |

    Scenario: Compatibility selection is learned from the live reply, not presumed
      Given a helper's response compatibility is unknown at the start of a live session
      When the session receives an attributable apply response
      Then Sunshine uses the observed compatible response form for later compatible requests
      And it never lets an untagged legacy reply satisfy successor work

  Rule: Default timing profiles may be tuned only with equivalent safety

    Scenario Outline: A tunable default retains its externally visible safety invariant
      Given an implementation changes the operational default for <profile>
      When a caller reaches the corresponding deadline, retry boundary, or throttle
      Then it still produces <terminal outcome>
      And it still preserves <invariant>

      Examples:
        | profile                                  | terminal outcome                                      | invariant                                                     |
        | normal helper readiness and ping: 5 seconds | ready or unavailable by the caller deadline          | no late reply certifies a retired helper                       |
        | ordinary control connection: 2 seconds      | connected or unavailable by the caller deadline      | unavailable work does not claim current-session ownership      |
        | ordinary control send: 5 seconds            | dispatched or unavailable by the caller deadline     | a cancelled send cannot later publish success                  |
        | shutdown control connection or send: 500 milliseconds | dispatched or unavailable promptly          | teardown cannot wait indefinitely or extend old ownership      |
        | legacy Apply result: 5 seconds              | Apply result or unsuccessful result                  | an untagged late reply never satisfies successor work          |
        | Refresh Rate connection and result: 600 milliseconds and 12 seconds | result or unsuccessful result | result remains attributable to the requested display and current request |
        | Snapshot Current stage: 3 seconds           | result, dispatch-only compatibility, or failure      | snapshot work cannot consume all later Apply budget            |
        | stale-result retirement: 250 milliseconds   | prior authority is retired promptly                  | a late result cannot satisfy later work                        |
        | topology activation: 6 seconds              | active topology or unsuccessful apply                | incomplete activation is never published as verified success   |
        | normal helper listener renewal: 15 seconds   | listener availability is renewed                     | pending safety work continues while no client is connected     |
        | helper heartbeat: 30-second initial and missed-ping windows, then 2-minute recovery grace | recovery only after the grace boundary | a timely ping cancels an obsolete missed-ping recovery         |
        | active helper supervision: 10 seconds        | next liveness check is due                           | a failed check cannot reapply an obsolete display request       |
        | suspended helper supervision: 20 seconds     | next liveness check is due                           | suspended supervision does not claim a live stream              |
        | shutdown ping: 250 milliseconds          | reachable or unavailable promptly                     | teardown does not extend an earlier ownership                  |
        | prompt disarm budget and throttle: 150 milliseconds | delivered, throttled, or unavailable promptly | repeated requests do not permit uncontrolled recent restore    |
        | disarm grace: 5 seconds                  | recent restore may be stopped; older restore may finish | an older restore is not overwritten by the prompt path       |
        | forced-stop wait: 2 seconds              | cleanup continues after the observation wait          | obsolete helper results never own successor work               |
        | deferred initial delay: 2 seconds        | first attempt occurs after readiness delay            | no ended stream receives a later display change                |
        | deferred retry: 500 milliseconds to 10 seconds | retry or removal after the allowed attempts      | retries remain finite and session-owned                        |
        | helper cooldown: two failures then 30 seconds | unavailable during cooldown                        | one failure remains eligible for its retry                     |
        | deferred attempts: six                   | pending work is removed after the sixth failure       | no indefinite or resurrected deferred display intent           |
        | virtual re-enable: 3 seconds             | repeated re-enable is suppressed during cooldown      | suppression does not certify a different session as healthy    |

    Scenario: A changed timer cannot weaken result attribution or verification ownership
      Given an implementation uses a different operational timing value
      When a reply, verification, restoration, or cleanup arrives after its owner was cancelled or superseded
      Then that event cannot publish a current session result
      And it cannot open capture, mark capture-stable, or change active-session ownership

    Scenario: A changed operational duration cannot create an unlimited caller path
      Given an implementation tunes a documented operational duration
      When helper readiness, deferred resolution, or display verification keeps failing
      Then callers still reach the documented finite retry cap and safe terminal result
      And stream start and capture do not wait indefinitely
