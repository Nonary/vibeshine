@display-engine-v2
Feature: Display engine v2 helper protocol and session lifecycle
  The display engine v2 helper accepts one current control session and reports
  command outcomes only to the request and session that own them.

  Rule: Startup selects an observable engine mode and a safe restore context

    Scenario: A user can select the display helper engine
      Given the Windows display settings offer automatic, current-engine, and legacy-engine choices
      When the user selects the current engine
      Then Sunshine starts the display engine v2 helper for subsequent display control
      And when the user selects the legacy engine Sunshine starts the legacy helper instead
      And automatic selection uses the current engine only on prerelease builds

    Scenario: The helper starts one normal control service
      Given no other display engine v2 helper is active
      When the helper starts normally with a valid optional log-level override
      Then it accepts a control client when one becomes available
      And it continues to provide any already-required display safety behavior while waiting
      And another helper instance does not take over the same control service

    Scenario: Restore-mode startup does not wait for a client
      Given a valid restore context is available
      When the helper is started in restore mode
      Then it begins the requested restoration without a control client
      And it exits after the restoration reaches its terminal outcome

    Scenario: Startup adopts only usable prior session snapshots
      Given a prior execution left a session snapshot in a compatible search location
      When the snapshot contains usable restoration data
      Then the new helper makes that snapshot available to its restore session

    Scenario: Startup rejects an unusable prior session snapshot
      Given a discovered session snapshot has no usable restoration data
      When the helper starts
      Then it is not adopted as a restore candidate

    Scenario Outline: Startup accepts only supported log-level overrides
      Given no other display engine v2 helper is active
      When the helper starts with the log-level override <override>
      Then the effective log level is <result>

      Examples:
        | override  | result                              |
        | "debug"  | the requested debug level           |
        | "6"      | the requested none level            |
        | "loud"   | the default informational level      |
        | "9"      | the default informational level      |

  Rule: The control boundary accepts defined command frames

    Scenario Outline: A valid command frame is accepted without changing its caller identity
      Given an active control client with session epoch 41
      When it sends a valid <command> command frame
      Then the helper accepts that command as belonging to epoch 41
      And any outcome that has a defined reply is sent only to that epoch

      Examples:
        | command          |
        | Apply            |
        | Revert           |
        | Reset            |
        | Export Golden    |
        | Disarm           |
        | Snapshot Current |
        | Refresh Rate     |
        | Ping             |
        | Log Level        |
        | Stop             |

    Scenario: An invalid Apply configuration fails without mutating the session
      Given an active control client sends an Apply request with a valid request identifier
      And the requested display configuration is malformed or incomplete
      When the helper validates the request
      Then it leaves the display session unchanged
      And it returns a failed Apply result carrying that same request identifier

    Scenario: A malformed or unsupported frame has no control effect
      Given an active control client
      When it sends an empty, malformed, or unsupported command frame
      Then the helper does not apply, restore, reset, disarm, or stop the current session
      And it does not invent a success reply for that frame

    Scenario: Snapshot metadata does not accidentally replace exclusion policy
      Given an active control client has an existing snapshot exclusion policy
      When it sends Snapshot Current with a correlation identifier but without an exclusion list
      Then the request may be correlated without changing the existing exclusion policy
      And malformed optional exclusion data does not replace that policy

    Scenario: An incomplete refresh-rate request is rejected immediately
      Given an active control client sends Refresh Rate without a device identifier or a nonzero numerator and denominator
      When the helper validates the refresh-rate frame
      Then it returns a failed refresh-rate result immediately
      And a supplied v2 request identifier is echoed in that failed result

    Scenario: Log-level updates stay within the supported range
      Given an active control client
      When it sends a Log Level value outside the supported range
      Then the helper uses the nearest supported level
      And the update does not act as a display-control command

  Rule: Replies are correlated to their request and live control session

    Scenario: Apply acknowledgement and verification remain correlated
      Given an active control client sends Apply with request identifier 1001
      When the helper accepts the request and later reaches its initial verification outcome
      Then the Apply result identifies request 1001
      And the verification result identifies request 1001
      And neither result can satisfy a different Apply request

    Scenario: Uncorrelated Snapshot Current remains dispatch-only
      Given an active control client sends Snapshot Current without a correlation identifier
      When the snapshot operation completes
      Then no Snapshot Current completion reply is sent

    Scenario: Correlated Snapshot Current reports its matching outcome
      Given an active control client sends Snapshot Current with a nonzero correlation identifier
      When the snapshot operation completes
      Then its Snapshot Current result includes that identifier and its success or failure outcome

    Scenario: Legacy Refresh Rate receives an uncorrelated result
      Given an active control client sends a valid legacy refresh-rate request
      When the refresh-rate operation completes
      Then it receives a refresh-rate success or failure result without a request identifier

    Scenario: Correlated v2 Refresh Rate receives its matching result
      Given an active control client sends the correlated v2 Refresh Rate form with identifier 2002
      When the refresh-rate operation completes
      Then the result includes identifier 2002

    Scenario: Ping acknowledges liveness without waiting for display work
      Given an active control client
      When it sends Ping while display work is pending
      Then the helper records the client as live
      And it returns a Ping acknowledgement without waiting for that display work

    Scenario: A missing or superseded response cannot be reused
      Given a client is waiting for a correlated Apply, Snapshot Current, or Refresh Rate response
      When the response deadline expires without that request's matching reply
      Then the client treats the request as unsuccessful
      And it retires the affected connection before a late reply can answer a later request

  Rule: A replacement client cannot inherit stale control or completion traffic

    Scenario: Commands from a retired connection are ignored
      Given control client A owns session epoch 41
      And client A disconnects or its connection reports an error
      When a queued command from epoch 41 is encountered
      Then it does not alter the current display session
      And it cannot disarm recovery or replace a restore baseline

    Scenario: Late completions cannot reply through a replacement client
      Given client A started a correlated request in session epoch 41
      And client B is now the active control client in a later session epoch
      When the result for client A's request arrives
      Then client B receives no result for client A's request
      And the late result cannot be treated as an outcome for client B's request

    Scenario: Stale epoch or cancelled-generation work has no current-session effect
      Given work belongs to a retired control epoch or a generation cancelled by newer intent
      When that work reaches a command or completion boundary
      Then it produces no current-session transition
      And it produces no reply through the current control connection

    Scenario: Replacement control intent takes precedence over obsolete completion traffic
      Given a completion from an earlier control intent is pending
      And a replacement client supplies Apply, Revert, Disarm, or Reset intent
      When the helper processes the replacement intent
      Then it treats the replacement intent as the current control decision
      And the earlier completion cannot regain control of the session

    Scenario: A reconnect cancels the earlier disconnect grace
      Given a disconnected client has started a pending disconnect grace period
      When a replacement control client reconnects and supplies current control intent
      Then the earlier disconnect does not trigger restoration for the replacement client
      And the replacement client's intent is evaluated as the live session intent

    Scenario: A broken connection leaves only autonomous safety behavior eligible
      Given an active control connection breaks while the helper has display safety work to perform
      When the helper retires that connection
      Then it removes that connection's queued control commands and reply channel
      And only recovery behavior owned by the helper may continue for the retired session

  Rule: Stop ends the helper without abandoning required cleanup

    Scenario: Stop requests a clean helper shutdown
      Given an active control client
      When it sends Stop
      Then the helper stops accepting further control work
      And it completes required shutdown cleanup before it exits
