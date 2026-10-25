# Steam Big Picture controls

> **Status:** under development. Everything below is built and passed the live checks in
> [Tested with](#tested-with) except a browser (WebRTC) stream; a code review is still to do.

When enabled with `steam_big_picture_controls`, Vibeshine shows stream controls inside Steam
Big Picture on the Windows host:

- **Power menu**, at the top:
  - **End Stream (Vibeshine)** ends every stream, the running app and virtual displays, like the
    end-stream hotkey (`nvhttp::terminate_all_streams_from_host`).
  - **Stop Streaming To &lt;client&gt; (Vibeshine)** drops one client and leaves the app running
    (`nvhttp::disconnect_client`).
- **Header icon** left of the clock: green while a client is connected, amber while the app runs with
  no client, hidden when idle. Selecting it opens a menu with the same actions and the stream details.

## Turning it on

On the **Integrations** page, in the Steam card's **Big Picture controls** section:

1. Turn on **Show stream controls in Steam Big Picture**.
2. Under **Steam debug port**, select **Turn on** and confirm. Vibeshine adds the empty
   `.cef-enable-remote-debugging` file to the Steam folder and restarts Steam, reopening Big Picture
   if it was open. The status changes to **Working** once Vibeshine is connected to Steam again.

Restarting Steam closes games running through it, so while a stream or app is running Vibeshine
asks a second time before restarting. If Steam isn't running, nothing restarts and Steam uses the
debug port the next time it starts. **Turn off** deletes the file and restarts Steam the same way.
Uninstalling Vibeshine also deletes the file if Vibeshine created it; a file that was already there,
for example for another Steam mod, is left alone. Steam closes the port at its next start.

**Check compatibility** runs the script's check mode in the running Steam without changing it and
lists each control as working, using a fallback, or off. Run it after a Steam update.

## Known limits

- **Header icon when Big Picture is already open.** React applies Vibeshine's header patch only when
  Big Picture creates its header. If Big Picture is already open when Vibeshine connects (for
  example after Vibeshine restarts), the icon appears the next time Big Picture opens; the Power
  menu entries work straight away. The header icon reports
  `ready; appears the next time Big Picture opens` until then.
- **End Stream's log label.** End Stream shares the end-stream hotkey's code, so its display cleanup
  logs `reason=end_stream_hotkey`. The line before it says `End Stream selected in Steam`.
- **Console session only.** The helper runs in the PC's own (console) session, where Steam is on
  screen. When Vibeshine runs outside it without its service, for example over Remote Desktop, the
  Steam card shows **Not on the PC's own session** and explains why. Any other reason the helper
  can't start is shown there too, and logged once.

## Tested with

Steam client build Oct 22 2026 14:39:25 (React 19.1.1), Windows 11, 2026-10-24. Check compatibility:
all 5 controls work.

| Check | Result |
|---|---|
| Moonlight stream | The icon turns green with the client's name. Stop Streaming drops that client and leaves the app running (the icon turns amber); End Stream ends the app and the icon hides. |
| Browser (WebRTC) stream | Not tested live; covered by the script and web UI tests. |
| Fault injection (`--simulate-missing`, `--simulate-crash`, `--max-errors`) | With every lookup missing, nothing is injected. A crashing Power menu entry or header icon disappears on its own. After the error budget the script switches itself off. Steam's own menus and header stay intact every time. |
| Steam's UI reloaded 3 times right after injection | Re-injected after the first two; the breaker stopped injecting after the third. Restarting the helper re-armed it. |
| Steam's web helper killed | Steam restarted it; the controls were back after 3.5 s. |
| Vibeshine's helper killed | Restarted after 5 s and injected again. |
| Debug-port buttons | Turn off and Turn on restarted Steam (in Big Picture when it was open) and the controls reconnected within 3 s. A stream kept running throughout. |

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
- The helper checks in with the script every 5 s. If the script hears nothing for 15 s, because the
  helper was stopped, crashed or was removed with Vibeshine, it hides its entries until the helper is
  back, so Steam never offers controls that nothing would act on.
- If Steam's UI reloads three times within five minutes, each time within 30 s of an injection, the
  helper stops injecting until the controls are turned off and on. A lost connection counts only
  when the same `steam.exe` is still running, meaning its web helper restarted; restarting Steam,
  including from the Integrations page, never counts.

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
- The flag file is created and deleted as the signed-in user, never as SYSTEM, and only in the folder
  of the running `steam.exe` (or Steam's registered folder when Steam isn't running). Steam is
  restarted under that user's token.
- Actions are allowlisted, `disconnect` must name a currently connected client, and requests are
  rate-limited.

## API

- `GET /api/steam/big_picture/status` returns `supported`, `enabled`, `state`, a
  per-feature report (`features`), Steam's folder and whether the flag file exists (`debug_port`),
  and the progress of a Steam restart (`steam_restart`).
- `POST /api/steam/big_picture/debug_port` with `{"enabled": bool, "restart_steam": bool,
  "allow_during_stream": bool}` creates or deletes the flag file and optionally restarts Steam.
- `POST /api/steam/big_picture/check` runs the compatibility check.

See [the API reference](api.md) for the details.

## Developing

- **Script tests:** `npm run test:unit` in the web UI runs the script against a fake Steam
  (`scripts/support/fake-steam.ts`).
- **Helper tests:** `test_component_steam_big_picture_controls` (decisions) and
  `test_component_steam_ui_helper_controller` (injection lifecycle against a fake Steam).
- **Web UI tests:** the Big Picture tests in `e2e/maintenance-parity.spec.ts` cover the debug-port
  confirmations and the compatibility report against a mocked API.
- **Check a Steam build:** `tools/sunshine_steam_ui_helper.exe --check` finds everything in the running
  Steam without patching it and prints the script's report.
- **Try it without the service:**
  `tools/sunshine_steam_ui_helper.exe --fixture tools/steam_ui_helper/fixtures/streaming.json`.
  The status comes from the fixture file, which is re-read when it changes. Button presses are printed
  and applied to the fixture status in memory, so Steam shows their effect. Ctrl+C removes the patches
  from Steam. Add `--simulate-missing=…`, `--simulate-crash=…` or `--max-errors=N` for fault tests,
  and `--script <path>` to use a script from a source checkout.
