# Steam Big Picture controls

> **Status:** under development. The setting, the service module (helper supervision, pipe, status
> and actions), the helper and the injected script exist; the status endpoint reports the helper's
> state and each feature. Turning on Steam's debug port from the web UI and the compatibility check
> button are not built yet: create the empty `.cef-enable-remote-debugging` file in the Steam folder
> and restart Steam by hand.

## Developing

- **Script tests:** `npm run test:unit` in the web UI runs the script against a fake Steam
  (`scripts/support/fake-steam.ts`).
- **Helper tests:** `test_component_steam_big_picture_controls` (decisions) and
  `test_component_steam_ui_helper_controller` (injection lifecycle against a fake Steam).
- **Check a Steam build:** `tools/sunshine_steam_ui_helper.exe --check` finds everything in the running
  Steam without patching it and prints the script's report.
- **Try it without the service:**
  `tools/sunshine_steam_ui_helper.exe --fixture tools/steam_ui_helper/fixtures/streaming.json`.
  The status comes from the fixture file, which is re-read when it changes. Button presses are printed
  and applied to the fixture status in memory, so Steam shows their effect. Ctrl+C removes the patches
  from Steam. Add `--simulate-missing=…`, `--simulate-crash=…` or `--max-errors=N` for fault tests,
  and `--script <path>` to use a script from a source checkout.

When enabled with `steam_big_picture_controls`, Vibeshine shows stream controls inside Steam
Big Picture on the Windows host:

- **Power menu**, at the top:
  - **End Stream (Vibeshine)** ends every stream, the running app and virtual displays, like the
    end-stream hotkey (`nvhttp::terminate_all_streams_from_host`).
  - **Stop Streaming To &lt;client&gt; (Vibeshine)** drops one client and leaves the app running
    (`nvhttp::disconnect_client`).
- **Header icon** left of the clock: green while a client is connected, amber while the app runs with
  no client, hidden when idle. Selecting it opens a menu with the same actions and the stream details.

## How it works

```
Steam client (user)  <-- CDP over 127.0.0.1:8080 -->  sunshine_steam_ui_helper.exe (user, console session)
                                                            ^
                                                            | named pipe, SID-restricted, PID + session verified
                                                            v
                                         sunshine.exe (SYSTEM): steam_big_picture_controls module
```

- Steam's UI is a React app in one shared JavaScript context (`SharedJSContext`). The helper
  injects `assets/steam/big_picture_controls.js` there over Steam's CEF debug port. The debug port
  is enabled by an empty `.cef-enable-remote-debugging` file in the Steam folder, followed by a
  Steam restart.
- The helper only connects when the port belongs to Steam's own `steamwebhelper.exe` in the same
  session. Untrusted debugger traffic stays in the helper; the service reads only small framed pipe
  messages from a verified helper.
- The service builds the status from non-blocking snapshots. It is woken by stream, client and app
  lifecycle hooks, with a slow fallback poll. Actions run on the module's own thread.
- The injected script finds Steam components by stable names only (text IDs such as
  `#StopStreamingTo`, property names, CSS keys and method names), and fails per feature: if a lookup
  breaks after a Steam update, that feature switches off or falls back, and Steam's own menus and
  header stay intact.

## Message formats

All formats are defined in `src/steam_big_picture_controls_protocol.h`:

- **Pipe framing:** a 4-byte little-endian length, then UTF-8 JSON with a `"type"`. Frames are
  limited to 64 KiB.
- **Message types:**
  - service → helper: `config`, `status`, `check`
  - helper → service: `hello`, `state`, `report`, `feature`, `error`, `action`, `check_result`, `log`
- **Status payload** (`window.VibeshineBPM.setStatus`): built by
  `platf::steam::big_picture::policy::status_payload`. It carries the app state, connected clients
  (`moonlight` or `browser`), stream details, capability flags, and the labels to show.
- **Actions** from Steam: `end_stream`, or `disconnect` with a client id. They are validated by
  `policy::parse_action`.

## Security

- Off by default. While Steam's debug port is on, any program on the PC can control Steam; the web
  UI says so before turning it on.
- Actions are allowlisted, `disconnect` must name a currently connected client, and requests are
  rate-limited.

## Status endpoint

`GET /api/steam/big_picture/status` returns `supported`, `enabled`, `implemented`, `state` and a
per-feature report (`features`).
