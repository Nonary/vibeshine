# Steam Big Picture controls

> **Status:** under development. Everything below is built and passed the live checks in
> [Tested with](#tested-with) except a browser (WebRTC) stream. The fixes from the code review
> (2026-10-29) are covered by tests; the header icon fixes were also checked live (Steam build
> Oct 28 2026), the debug-port fixes not yet.

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

Turn on **Show stream controls in Steam Big Picture** and confirm. Vibeshine adds the empty
`.cef-enable-remote-debugging` file to the Steam folder and restarts Steam, reopening Big Picture if
it was open. The status changes to **Working** once Vibeshine connects. If the file is already there,
for example for another Steam mod, the switch turns on without restarting Steam.

Turning the switch off deletes the file and restarts Steam again, but only if it is the very file
Vibeshine added. A file that was already there, or that another program created again after
Vibeshine's was deleted, is left alone, and the switch turns off without a restart. The controls
turn off even if the file can't be deleted; the card then keeps showing that Steam's debug port is
still on (see below).

Restarting Steam closes games running through it, so while a stream or app is running Vibeshine
asks a second time before restarting. If Steam isn't running, nothing restarts and Steam uses the
change the next time it starts. When the controls are on but can't reach Steam yet, a **Steam debug
port** row appears with the fix: **Turn on** when the file is missing, or **Restart Steam** when
Steam was already running when the file was added. The same row appears while the controls are off
but the file Vibeshine added is still there, for example because deleting it failed, with **Turn
off** to try again.

Uninstalling Vibeshine also deletes the file if Vibeshine created it. With nobody signed in, for
example during a remote uninstall, it is deleted only when it is in the folder Steam's installer
registered; otherwise it stays. Steam closes the port at its next
start. The portable build has no uninstaller, so turn the controls off before deleting its folder.

Expand **Compatibility with this Steam version** and select **Check compatibility** to run the
script's check mode in the running Steam without changing it. It lists each control as working,
using a fallback, or off. Run it after a Steam update.

## Known limits

- **Header icon when Big Picture is already open.** React applies Vibeshine's header patch only when
  Big Picture creates its header. If Big Picture opened before Vibeshine first connected to this
  Steam (for example when Steam starts straight into Big Picture), the icon appears the next time
  Big Picture opens; the Power menu entries work straight away. The header icon reports
  `ready; appears the next time Big Picture opens` until then. Once a header has the patch, the icon
  comes back straight away when Vibeshine or its helper restarts, and goes as soon as the controls
  are turned off.
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
  helper stops injecting until the helper starts again: when the controls are turned off and on,
  when Vibeshine restarts, or when the service restarts a helper that exited. A lost connection counts only
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
  UI says so before turning it on. Such a program can also talk to the injected script: it can
  select End Stream (at most once every 2 s) and add lines to Vibeshine's log.
- The flag file is created and deleted as the signed-in user, and only in the folder of the running
  `steam.exe` (or Steam's registered folder when Steam isn't running). Steam is restarted under that
  user's token. The only exception is an uninstall with nobody signed in, which deletes the file as
  SYSTEM, and only in the folder Steam's installer registered for all users.
- A `steam.exe` that runs below medium integrity or in an app container, as one started by a
  sandboxed program would, is never taken as Steam: restarting Steam would start it outside its
  sandbox.
- Only the very file Vibeshine created is ever deleted. Vibeshine records its path and file ID, so a
  file the user or another Steam mod creates at the same path later is left alone. If the record
  can't be saved, Vibeshine removes the file again and reports the error.
- Actions are allowlisted, `disconnect` must name a currently connected client, and requests are
  rate-limited.

## API

- `GET /api/steam/big_picture/status` returns `supported`, `enabled`, `state`, a
  per-feature report (`features`), Steam's folder and whether the flag file exists (`debug_port`),
  and the progress of a Steam restart (`steam_restart`).
- `POST /api/steam/big_picture/debug_port` with `{"enabled": bool, "restart_steam": bool,
  "allow_during_stream": bool}` creates the flag file, or deletes it if Vibeshine created it, and
  optionally restarts Steam.
- `POST /api/steam/big_picture/check` runs the compatibility check.

See [the API reference](api.md) for the details.

## Developing

- **Script tests:** `npm run test:unit` in the web UI runs the script against a fake Steam
  (`scripts/support/fake-steam.ts`), whose small React stand-in redraws like React, so something
  left frozen on screen fails a test.
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
