@display-engine-v2
Feature: Display engine v2 helper protocol and session lifecycle
  The display engine v2 helper accepts one current control session and reports
  command outcomes only to the request and session that own them.

  Rule: One helper owns the active control service

    Scenario: A second helper does not take over an active control service
      Given a display engine v2 helper is already active
      When another helper instance starts
      Then the existing helper remains the control service
      And the new instance exits without taking over display control

    Scenario: The normal helper remains available while it waits for control
      Given no display engine v2 helper is active
      When the helper starts normally before a control client connects
      Then it remains available to accept a later control client
      And it continues any already-required display safety behavior while waiting

    Scenario: A normal helper renews its listener wait without abandoning safety work
      Given the normally started helper has no connected control client
      When its default 15-second control-listener wait ends without a client
      Then it renews availability for a later control client
      And it continues already-required display safety behavior throughout the renewed wait

    Scenario: Temporary control-endpoint unavailability does not abandon safety behavior
      Given the helper has required display safety behavior in progress
      And it cannot currently offer a control endpoint
      When it continues running
      Then it keeps the safety behavior active
      And it retries making control available instead of exiting solely for that reason

    Scenario: Restore-mode startup begins recovery without a control client
      Given a valid restore context is available
      When the helper is started in restore mode
      Then it begins the requested restoration without a control client
      And it remains available for later recovery opportunities until a confirmed restore or shutdown ends restore mode

    Scenario: A confirmed restore ends restore mode
      Given the helper is running in restore mode with a usable recovery candidate
      When restoration and final validation confirm the desktop
      Then it reports the safe completion
      And it exits restore mode

    Scenario: An unconfirmed restore does not end restore mode
      Given the helper is running in restore mode with an existing recovery candidate
      When that candidate's restoration or final validation remains unconfirmed
      Then it retains the recovery opportunity for later eligible events
      And it does not exit merely because its initial restoration did not succeed

    Scenario: Startup adopts a valid current-session restore baseline
      Given a compatible prior context contains a valid current-session restore baseline
      When the helper starts
      Then it makes that current-session baseline available to its restore session

    Scenario: Startup adopts a valid previous-session restore baseline
      Given a compatible prior context contains a valid previous-session restore baseline
      When the helper starts
      Then it makes that previous-session baseline available to its restore session

    Scenario: Startup rejects an unusable prior session baseline
      Given a discovered session baseline has no usable restoration data
      When the helper starts
      Then it is not adopted as a restore candidate
      And it is not retained as a usable session baseline for a later restore

    Scenario: Startup combines saved user exclusions with managed virtual displays
      Given the first usable saved display state names excluded devices
      And that saved state names managed virtual displays
      When the helper starts
      Then it begins with the combined unique exclusion set
      And those displays are not eligible for helper-managed baseline capture or restore

    Scenario: Startup does not invent exclusions when no saved exclusion state is usable
      Given no compatible saved display state provides exclusions
      When the helper starts
      Then it starts without an additional saved exclusion policy

    Scenario Outline: Unusable saved exclusion state does not block helper startup
      Given the saved display state is <state condition>
      When the helper starts
      Then it does not stop solely because the saved state is unusable
      And it does not use the unusable state as an exclusion policy

      Examples:
        | state condition |
        | missing         |
        | corrupt         |

    Scenario Outline: Startup log-level arguments accept case-insensitive names or one digit
      Given no other display engine v2 helper is active
      When the helper starts with <log-level argument>
      Then the effective log level is <result>

      Examples:
        | log-level argument  | result                         |
        | "--log-level debug" | the requested debug level      |
        | "--log-level WARNING" | the requested warning level    |
        | "--log-level=6"     | the requested none level       |
        | "--log-level loud"  | the default informational level |
        | "--log-level=9"     | the default informational level |

  Rule: The control boundary accepts defined frames and rejects unsafe input

    Scenario Outline: A valid command frame retains its caller identity
      Given an active control client with control-session identity A
      When it sends a valid <command> command frame
      Then the helper accepts that command as belonging to control session A
      And any defined reply is sent only to control session A

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

    Scenario Outline: Supported frame formats carry the same control intent
      Given an active control client sends a valid Apply request
      When it uses the <frame format> form of the control protocol
      Then the helper accepts the request with the same command and payload meaning

      Examples:
        | frame format             |
        | legacy raw command-and-data |
        | length-declared command-and-data |

    Scenario Outline: An invalid Apply configuration fails without mutating the session
      Given an active control client sends an Apply request with a valid request identifier
      And the requested display configuration is <invalid configuration>
      When the helper validates the request
      Then it leaves the display session unchanged
      And it returns a failed Apply result carrying that same request identifier

      Examples:
        | invalid configuration |
        | malformed             |
        | incomplete            |

    Scenario Outline: An empty or unrecognized complete command has no control effect
      Given an active control client
      When it sends <frame condition>
      Then the helper does not apply, restore, reset, disarm, or stop the current session
      And it does not invent a success reply for that frame

      Examples:
        | frame condition                                  |
        | an empty frame                                    |
        | a complete frame whose command type is unsupported |

    Scenario Outline: An incomplete refresh-rate request is rejected immediately
      Given an active control client sends Refresh Rate with <missing requirement>
      When the helper validates the refresh-rate request
      Then it returns a failed refresh-rate result immediately
      And a supplied v2 request identifier is echoed in that failed result

      Examples:
        | missing requirement               |
        | no device identifier               |
        | a zero refresh numerator           |
        | a zero refresh denominator         |

    Scenario: A runtime log-level update clamps its first numeric byte to the supported range
      Given an active control client
      When it sends Log Level with a nonempty first numeric byte above the supported range
      Then the helper uses the highest supported log level
      And the update does not act as a display-control command
      And it does not send a completion result

    Scenario: An empty runtime log-level frame preserves the current level
      Given an active control client with an established log level
      When it sends Log Level without a level value
      Then the helper preserves the established log level
      And it does not change display-control intent

    Scenario Outline: Dispatch-only commands do not fabricate an immediate result message
      Given an active control client sends <command>
      When the helper accepts that command
      Then it does not emit an immediate Apply, verification, snapshot, or refresh-rate result solely as an acknowledgement

      Examples:
        | command       |
        | Revert        |
        | Disarm        |
        | Export Golden |
        | Reset         |
        | Log Level     |
        | Stop          |

  Rule: Apply options retain valid intent and default safely

    Scenario: Apply retains a valid requested configuration and topology together
      Given an Apply request contains a valid display configuration and a valid requested topology
      When the helper accepts the request
      Then it retains both as the request's display-change intent

    Scenario: Apply ignores incomplete topology groups without inventing a target
      Given an Apply request contains a valid display configuration
      And its requested topology contains both complete and incomplete display groups
      When the helper accepts the request
      Then it retains only the complete display groups as topology intent
      And it does not derive a topology target from an incomplete group

    Scenario: A non-list topology does not create topology intent
      Given an Apply request contains a non-list topology value
      When the helper accepts the remaining valid request data
      Then it does not attach topology intent to that request

    Scenario: Apply retains complete monitor placement instructions
      Given an Apply request supplies a device identifier and whole-number horizontal and vertical placement
      When the helper accepts the request
      Then it retains that monitor placement as part of the display-change intent

    Scenario Outline: Incomplete monitor placement is not treated as a placement instruction
      Given an Apply request supplies a monitor placement with <incomplete part>
      When the helper accepts the remaining valid request data
      Then it does not retain that incomplete placement as a display-change instruction

      Examples:
        | incomplete part                         |
        | no device placement record               |
        | no horizontal coordinate                 |
        | no vertical coordinate                   |
        | a non-whole-number coordinate            |

    Scenario: Apply retains complete per-device refresh overrides
      Given an Apply request supplies a device identifier with a numeric refresh numerator and denominator
      When the helper accepts the request
      Then it retains that per-device refresh override as part of the display-change intent

    Scenario Outline: Incomplete per-device refresh data is not treated as an override
      Given an Apply request supplies a refresh override with <incomplete part>
      When the helper accepts the remaining valid request data
      Then it does not retain that incomplete override as a display-change instruction

      Examples:
        | incomplete part                |
        | no override record             |
        | no numerator                   |
        | no denominator                 |
        | a nonnumeric rate component    |

    Scenario: Apply retains an explicit HDR blanking request
      Given an Apply request contains a valid explicit HDR blanking choice
      When the helper accepts the request
      Then it retains that HDR transition choice as display-change intent

    Scenario Outline: An invalid Boolean Apply option does not opt into alternate behavior
      Given an Apply request contains <invalid option>
      When the helper accepts the remaining valid request data
      Then it does not opt into that option's alternate behavior

      Examples:
        | invalid option                                      |
        | a non-Boolean HDR blanking choice                   |
        | a non-Boolean golden-baseline preference            |
        | a non-Boolean stream-start HDR option               |

    Scenario: Apply retains a named virtual display arrangement
      Given an Apply request contains a valid named virtual display arrangement
      When the helper accepts the request
      Then it retains that arrangement as display-change intent

    Scenario: A non-text virtual display arrangement does not create arrangement intent
      Given an Apply request contains a non-text virtual display arrangement
      When the helper evaluates the optional arrangement
      Then it does not attach a virtual display arrangement to the request

    Scenario: Apply replaces its baseline exclusion policy with a supplied list
      Given an Apply request supplies recognizable device identities to exclude from its baseline
      When the helper accepts the request
      Then those identities replace the request's baseline exclusion policy

    Scenario: An explicit empty Apply exclusion list clears the request's baseline exclusion policy
      Given an Apply request supplies an explicit empty baseline exclusion list
      When the helper accepts the request
      Then the request's baseline exclusion policy is cleared

    Scenario: Apply retains a valid golden-baseline preference
      Given an Apply request contains a valid preference for golden-baseline restoration
      When the helper accepts the request
      Then it retains that preference as the request's recovery policy

    Scenario: An Apply request without a disconnect policy defaults to safety restoration
      Given an Apply request omits its disconnect restoration policy
      When the helper accepts the request
      Then the request remains eligible for restoration after a broken control connection

    Scenario: An invalid Apply disconnect policy defaults to safety restoration
      Given an Apply request contains an invalid disconnect restoration policy
      When the helper accepts the remaining valid request data
      Then the request remains eligible for restoration after a broken control connection

    Scenario: An Apply request can deliberately retain a paused session after disconnect
      Given an Apply request explicitly disables restoration after disconnect
      When the helper accepts the request
      Then the request retains that paused-session policy

    Scenario: The stream-start option can omit the final initial HDR correction
      Given an Apply request explicitly asks to omit the final initial HDR correction for a capture-gated stream start
      When the helper accepts the request
      Then it retains that stream-start option

    Scenario: An Apply request without the stream-start HDR option uses normal handling
      Given an Apply request omits the stream-start HDR option
      When the helper accepts the request
      Then it retains normal initial HDR handling for that request

    Scenario Outline: A valid Apply policy option preserves unrelated display intent
      Given an Apply request contains a valid display configuration and topology
      And it contains <policy option>
      When the helper accepts the request
      Then it retains <policy option> only for its named policy
      And it preserves the request's unrelated display configuration and topology intent

      Examples:
        | policy option                                      |
        | a golden-baseline recovery preference              |
        | a disconnect restoration preference                |
        | a stream-start HDR handling preference             |
        | a baseline exclusion policy                        |

    Scenario: A numeric Apply identifier establishes reply correlation
      Given an Apply request contains numeric identifier 1001
      When the helper accepts the request
      Then it retains identifier 1001 for its Apply and verification outcomes

    Scenario Outline: An unusable Apply identifier cannot impersonate a caller identifier
      Given an Apply request has <identifier condition>
      When the helper accepts the remaining valid request data
      Then it does not assign an unrelated caller identifier to the request

      Examples:
        | identifier condition     |
        | no identifier            |
        | a nonnumeric identifier  |

  Rule: Revert and baseline commands honor only valid optional policy

    Scenario: Revert defaults to golden fallback when the current baseline is unavailable
      Given an active control client sends Revert without an optional preference
      When the helper accepts the request
      Then it retains the default policy that permits golden fallback if the current baseline is unavailable

    Scenario: Revert can enable golden fallback when the current baseline is unavailable
      Given an active control client sends Revert with a valid enabled golden-fallback preference
      When the helper accepts the request
      Then it retains the enabled golden-fallback preference

    Scenario: Revert can disable golden fallback when the current baseline is unavailable
      Given an active control client sends Revert with a valid disabled golden-fallback preference
      When the helper accepts the request
      Then it retains the disabled golden-fallback preference

    Scenario: Revert retains an explicit golden-first override
      Given an active control client sends Revert with a valid golden-first override
      When the helper accepts the request
      Then it retains that golden-first override

    Scenario Outline: Invalid Revert preferences preserve the safe defaults
      Given an active control client sends Revert with <invalid preference>
      When the helper accepts the request
      Then it preserves the default fallback policy
      And it does not invent a golden-first override

      Examples:
        | invalid preference                |
        | malformed optional preference data |
        | a non-Boolean preference           |

    Scenario Outline: Baseline commands accept current and legacy-compatible exclusion lists
      Given an active control client sends <command> with <list form> of device identities to exclude
      When the helper accepts the command
      Then it retains those identities as that command's exclusion policy

      Examples:
        | command          | list form                                      |
        | Export Golden    | a direct list                                  |
        | Snapshot Current | a list of records that each identify a device |

    Scenario Outline: An explicit empty baseline exclusion list clears command policy
      Given an active control client sends <command> with an explicit empty exclusion list
      When the helper accepts the command
      Then that command's exclusion policy is cleared

      Examples:
        | command          |
        | Export Golden    |
        | Snapshot Current |

    Scenario: Correlation-only Snapshot Current metadata preserves exclusion policy
      Given an active control client has an existing snapshot exclusion policy
      When it sends Snapshot Current with a correlation identifier but no exclusion instruction
      Then the request may be correlated without changing the existing exclusion policy

    Scenario: Malformed Snapshot Current exclusion data preserves exclusion policy
      Given an active control client has an existing snapshot exclusion policy
      When it sends Snapshot Current with malformed exclusion data
      Then it preserves the existing exclusion policy

    Scenario: Malformed Export Golden exclusion data preserves exclusion policy
      Given an active control client has an existing snapshot exclusion policy
      When it sends Export Golden with malformed exclusion data
      Then it preserves the existing exclusion policy

  Rule: Replies are correlated to their request and live control session

    Scenario: A successful Apply status reports a successful Apply result
      Given an active control client owns Apply request identifier 1001
      When that request reaches a successful Apply outcome
      Then it receives a successful Apply result carrying identifier 1001

    Scenario Outline: Any non-success Apply status is reported as a failed Apply result
      Given an active control client owns Apply request identifier 1001
      When that request reaches the <status> outcome
      Then it receives a failed Apply result carrying identifier 1001

      Examples:
        | status                     |
        | helper-unavailable          |
        | invalid-request             |
        | verification-failed         |
        | virtual-display-reset-needed |
        | retryable                   |
        | fatal                       |

    Scenario Outline: Apply acknowledgement and initial verification remain correlated
      Given an active control client sends Apply with request identifier 1001
      When the helper reaches an initial verification outcome of <verification outcome>
      Then the Apply result identifies request 1001
      And the verification result identifies request 1001 with <verification outcome>
      And neither result can satisfy a different Apply request

      Examples:
        | verification outcome |
        | success              |
        | failure              |

    Scenario: Uncorrelated Snapshot Current remains dispatch-only
      Given an active control client sends Snapshot Current without a correlation identifier
      When the snapshot operation completes
      Then no Snapshot Current completion reply is sent

    Scenario: A nonnumeric Snapshot Current identifier remains dispatch-only
      Given an active control client sends Snapshot Current with a nonnumeric correlation identifier
      When the snapshot operation completes
      Then no Snapshot Current completion reply is sent

    Scenario Outline: Correlated Snapshot Current reports its matching outcome
      Given an active control client sends Snapshot Current with nonzero correlation identifier 3003
      When the snapshot operation completes with <snapshot outcome>
      Then its Snapshot Current result identifies 3003 with <snapshot outcome>

      Examples:
        | snapshot outcome |
        | success          |
        | failure          |

    Scenario Outline: Legacy Refresh Rate receives an uncorrelated result
      Given an active control client sends a valid legacy Refresh Rate request
      When the refresh-rate operation completes with <refresh outcome>
      Then it receives a <refresh outcome> refresh-rate result without a request identifier

      Examples:
        | refresh outcome |
        | success         |
        | failure         |

    Scenario Outline: Correlated v2 Refresh Rate receives its matching result
      Given an active control client sends a correlated v2 Refresh Rate request with identifier 2002
      When the refresh-rate operation completes with <refresh outcome>
      Then it receives a <refresh outcome> refresh-rate result carrying identifier 2002

      Examples:
        | refresh outcome |
        | success         |
        | failure         |

    Scenario: A zero v2 Refresh Rate identifier remains uncorrelated
      Given an active control client sends a valid v2 Refresh Rate request with identifier zero
      When the refresh-rate operation completes
      Then it receives a refresh-rate result without a request identifier

    Scenario: Ping acknowledges liveness without waiting for display work
      Given an active control client
      When it sends Ping while display work is pending
      Then the helper records the client as live
      And it returns a Ping acknowledgement without waiting for that display work

    Scenario Outline: A timed-out correlated response cannot be reused
      Given a client is waiting for a correlated <command> response
      When the response deadline expires without that request's matching reply
      Then the client treats the request as unsuccessful
      And it retires the affected connection before a late reply can answer a later request

      Examples:
        | command          |
        | Apply            |
        | Snapshot Current |
        | Refresh Rate     |

  Rule: A replacement client cannot inherit stale control or completion traffic

    Scenario Outline: Commands from a retired connection are ignored
      Given control client A owns control-session identity A
      And client A is retired because of <retirement cause>
      When a pending command from control session A reaches its control boundary
      Then it does not alter the current display session
      And it cannot disarm recovery or replace a restore baseline

      Examples:
        | retirement cause       |
        | a connection error     |
        | a disconnect           |
        | a liveness timeout     |

    Scenario: Late completions cannot reply through a replacement client
      Given client A started a correlated request in control session A
      And client B is now the active control client in replacement control session B
      When the result for client A's request arrives
      Then client B receives no result for client A's request
      And the late result cannot be treated as an outcome for client B's request

    Scenario Outline: Superseded work has no current-session effect
      Given work belongs to <obsolete ownership>
      When that work reaches a command or completion boundary
      Then it produces no current-session transition
      And it produces no reply through the current control connection

      Examples:
        | obsolete ownership                       |
        | a retired control session                |
        | work cancelled by newer control intent   |

    Scenario Outline: Replacement control intent takes precedence over obsolete completion traffic
      Given a completion from an earlier control intent is pending
      And a replacement client supplies <replacement intent>
      When the helper processes the replacement intent
      Then it treats the replacement intent as the current control decision
      And the earlier completion cannot regain control of the session

      Examples:
        | replacement intent |
        | Apply              |
        | Revert             |
        | Disarm             |
        | Reset              |

    Scenario: A reconnect cancels the earlier disconnect grace
      Given a disconnected client has started a pending disconnect grace period
      When a replacement control client reconnects and supplies current control intent
      Then the earlier disconnect does not trigger restoration for the replacement client
      And the replacement client's intent is evaluated as the live session intent

    Scenario: A completed old restore does not terminate a newer live session
      Given a restore originated from a retired control session
      And a newer control client is active
      When that older restore completes successfully
      Then the helper remains available for the newer control client

    Scenario: A broken connection leaves only autonomous safety behavior eligible
      Given an active control connection breaks while the helper has display safety work to perform
      When the helper retires that connection
      Then it removes that connection's pending control intent and reply authority
      And only helper-owned safety behavior may continue for the retired session

  Rule: Stop ends the helper without abandoning required cleanup

    Scenario: Stop requests a clean helper shutdown
      Given an active control client
      When it sends Stop
      Then the helper stops accepting further control work
      And it completes required shutdown cleanup before it exits

  Rule: Public command forms retain their compatibility boundary

    Scenario Outline: Only Apply and Refresh Rate reject invalid required request data
      Given an active control session
      When it sends <command> with <invalid required data>
      Then the helper rejects that request or returns its failed result without display mutation
      And an invalid payload has no display-control side effect
      And any result remains attributable only to the sending session

      Examples:
        | command      | invalid required data                                |
        | Apply        | no parseable display configuration                    |
        | Refresh Rate | missing, zero, or incomplete rate or device identity |

    Scenario Outline: Compatibility controls still dispatch with unexpected or malformed optional data
      Given an active control session sends <command> with <payload condition>
      When the helper receives the command
      Then it preserves the command's compatible control effect
      And it uses current or default optional policy where the metadata is unusable
      And it does not invent a completion result unless that command normally has one

      Examples:
        | command          | payload condition                                      |
        | Revert           | malformed, nonobject, or extra optional metadata       |
        | Reset            | arbitrary extra data                                   |
        | Disarm           | arbitrary extra data                                   |
        | Export Golden    | malformed exclusions or arbitrary extra data           |
        | Snapshot Current | malformed exclusions, unknown metadata, or extra data  |
        | Ping             | arbitrary extra data                                   |
        | Stop             | arbitrary extra data                                   |

    Scenario Outline: A parseable Apply uses the documented optional-field defaults
      Given a parseable Apply configuration omits or has a non-activating <field>
      When the helper accepts the request
      Then the effective <field> is <effective behavior>
      And the request retains no unintended alternate display policy

      Examples:
        | field                         | effective behavior                                     |
        | restore-on-disconnect          | enabled                                                |
        | omit-final-initial-HDR-reapply | disabled                                               |
        | HDR-blanking Boolean           | disabled when absent                                   |
        | golden-first Boolean           | absent when not Boolean                                |
        | virtual-layout text            | absent when not text                                   |
        | topology                       | absent unless it contains at least one nonempty group  |
        | monitor position               | absent unless both coordinates are whole numbers       |
        | per-device refresh override    | absent unless both values are unsigned whole numbers   |
        | request identifier             | uncorrelated when absent, nonnumeric, or zero          |

    Scenario: A malformed Apply optional field retains parser-compatible failure or fallback behavior
      Given an Apply includes malformed optional metadata that is not a documented request form
      When the helper evaluates the complete Apply payload
      Then it may return a failed Apply result if the remaining payload is not accepted
      And if the remaining configuration is accepted, the malformed metadata does not opt into its alternate policy

    Scenario Outline: The result form is compatible with the command's correlation form
      Given a valid <command> was accepted in <compatibility form>
      When it reaches its terminal observable outcome
      Then the helper emits <reply behavior>
      And that reply cannot satisfy a different request or a later control session

      Examples:
        | command          | compatibility form                    | reply behavior                                            |
        | Apply            | nonzero numeric identifier             | Apply result and verification result with that identifier |
        | Apply            | absent, nonnumeric, or zero identifier | Apply result using identifier zero; verification uses zero |
        | Snapshot Current | nonzero unsigned snapshot identifier   | Snapshot result with that identifier                       |
        | Snapshot Current | absent or unusable snapshot identifier | no completion result                                      |
        | Refresh Rate     | v2 marker and nonzero identifier       | Refresh Rate result with that identifier                   |
        | Refresh Rate     | legacy form or zero identifier         | Refresh Rate result without an identifier                  |
        | Revert           | any accepted form                      | dispatch-only; no invented completion result               |
        | Disarm           | any accepted form                      | dispatch-only; no invented completion result               |
        | Reset            | any accepted form                      | dispatch-only; no invented completion result               |
        | Export Golden    | any accepted form                      | dispatch-only; no invented completion result               |
        | Log Level        | any accepted form                      | dispatch-only; no invented completion result               |
        | Stop             | any accepted form                      | dispatch-only; no invented completion result               |

    Scenario: A complete length-declared message never borrows trailing data
      Given a length-declared control message contains one complete command and later bytes
      When the helper reads the command
      Then only the declared command and declared payload determine its control intent
      And later bytes cannot change that command's request identity or outcome

    Scenario: A short or malformed length declaration is compatible with the legacy raw form
      Given a control message does not contain a complete positive length declaration
      When its first byte identifies a supported command
      Then the helper interprets the remaining bytes as that command's legacy raw payload
      And it retains the legacy reply behavior for that payload

    Scenario: A foreign or late reply cannot become a current session result
      Given a control session has ended or has been replaced
      When an earlier request reaches a result boundary
      Then no reply is delivered through the current session
      And the new session retains sole ownership of its own replies

    Scenario Outline: Public command codes retain their established meanings
      Given a peer uses the established control-code vocabulary
      When it sends code <code>
      Then the helper interprets it as <command>
      And no other code is accepted as an alias for that command

      Examples:
        | code | command          |
        | 1    | Apply            |
        | 2    | Revert           |
        | 3    | Reset            |
        | 4    | Export Golden    |
        | 5    | Log Level        |
        | 7    | Disarm           |
        | 8    | Snapshot Current |
        | 10   | Refresh Rate     |
        | 254  | Ping             |
        | 255  | Stop             |

    Scenario Outline: Public result codes retain their established reply meanings
      Given a peer receives a result from the helper
      When the result uses code <code>
      Then it interprets the result as <result>
      And it applies the established correlation rule for that result type

      Examples:
        | code | result              |
        | 6    | Apply Result        |
        | 9    | Verification Result |
        | 11   | Refresh Rate Result |
        | 12   | Snapshot Result     |

    Scenario Outline: Startup log-level parsing maps accepted text and digit values to seven levels
      Given helper startup receives <startup value> through either supported startup argument form
      When the value is parsed case-insensitively
      Then the effective startup level is <level>
      And absent or invalid startup input uses informational level

      Examples:
        | startup value | level         |
        | verbose       | verbose       |
        | DEBUG         | debug         |
        | info          | informational |
        | warning       | warning       |
        | error         | error         |
        | fatal         | fatal         |
        | none          | none          |
        | 0             | verbose       |
        | 6             | none          |

    Scenario Outline: Live Log Level maps its first numeric byte and ignores the rest
      Given an active control session has an established log level
      When it sends a nonempty Log Level payload whose first numeric byte is <first byte>
      Then the live effective level is <level>
      And later bytes do not change that level
      And no completion result is sent

      Examples:
        | first byte | level   |
        | 0          | verbose |
        | 1          | debug   |
        | 6          | none    |
        | 7          | none    |
        | 255        | none    |

    Scenario: Refresh Rate v2 correlation requires its established marker and complete fields
      Given a peer sends a Refresh Rate request using the v2 marker
      When it includes positive numerator and denominator, a nonzero identifier, and a nonempty device identity
      Then the result carries that identifier

    Scenario: Refresh Rate legacy compatibility keeps its untagged result form
      Given a peer sends a Refresh Rate request without the v2 marker
      When it includes positive numerator and denominator and a nonempty device identity
      Then the result has no request identifier
      And it cannot satisfy a later correlated request

    Scenario: Invalid Refresh Rate data produces only an applicable uncorrelated failure
      Given a peer sends Refresh Rate with a missing or zero rate component or no device identity
      When the helper evaluates the request
      Then it returns a failed Refresh Rate result
      And that result carries an identifier only when a valid nonzero v2 identifier was supplied
