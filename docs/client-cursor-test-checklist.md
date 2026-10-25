# Client-side cursor: manual test checklist

Manual qualification for the client-side cursor and automatic mouse mode
([design](client-cursor-design.md)). Unit tests cover the wire format, bitmap
conversion, geometry, session policy, the client state machine and settings; this
list covers what they cannot: real cursors, real games, and real windowing.

## Setup

- **Host:** Vibeshine built from `feature/client-side-cursor`, on Windows, running
  as the service (SYSTEM) unless a step says otherwise. Note whether capture uses
  Desktop Duplication or WGC (the host log says which); run the key steps with both.
- **Client:** moonlight-qt built from `feature/client-side-cursor`, "Automatic
  mouse mode on supported hosts" on (the default), and the stats overlay on
  (Ctrl+Alt+Shift+S) so the `Mouse mode:` line is visible.
- **Logs:**
  - Host: `Client-side cursor:` lines.
  - Client: `Host cursor` and `Mouse mode:` lines in
    `%LOCALAPPDATA%\Temp\Moonlight-<epoch>.log`.

For each step, record: pass or fail, the capture backend, the client OS, and anything
odd in the logs.

## Desktop

- [ ] **Desktop and browser.**
  - The overlay reads `absolute, local cursor (automatic)`.
  - The cursor moves with no perceptible lag, has the host's shape, and there is no
    second cursor in the video.
  - Hovering links shows the hand, and the shape changes immediately.
- [ ] **Text I-beam and resize cursors.**
  - The I-beam in a text field appears black with a white outline and stays visible
    on both light and dark backgrounds.
  - Window edges show the resize arrows, and the hotspot is accurate (resize grabs
    the border, not a pixel beside it).
  - Hidden-pointer-while-typing (Windows setting) hides and shows the local cursor.
- [ ] **Large or high-DPI cursors.**
  - Set the Windows cursor size to a large value, or use a 150–200% scaled host
    monitor.
  - The local cursor matches the size it had in the video.
  - The same holds after resizing the client window and after a fullscreen toggle.
- [ ] **Busy cursor.** The spinner shows its current frame (animation is not
  expected in v1).

## Games

- [ ] **FPS (hidden and locked cursor).**
  - Entering gameplay switches the overlay to `relative (automatic)` within about
    0.1 s.
  - Camera movement is raw and unaccelerated.
  - No cursor is visible on either side.
- [ ] **Menu to gameplay and back.**
  - Pausing (Esc) shows the menu cursor and returns to absolute mode.
  - The local cursor appears where the game put it, not where the client cursor was
    before.
  - Resuming returns to relative mode. Repeat about 10 times: no flapping or stuck
    states.
- [ ] **RTS with a confined visible cursor.**
  - The overlay stays absolute.
  - The local cursor cannot leave the game's area (the mapped clip), and edge
    scrolling works.
  - Alt-tab on the host releases the confinement.
- [ ] **Game with a software cursor.**
  - The game hides the OS cursor and draws its own, so the overlay shows relative
    mode and the game's cursor stays in the video.
  - If the game expects absolute input, Ctrl+Alt+Shift+M switches to absolute and
    locks it, and Ctrl+Alt+Shift+A returns to automatic.
- [ ] **MMO/ARPG camera drag.**
  - A quick click that briefly hides the cursor does not switch modes.
  - Holding right-drag for camera rotation switches to relative once, and releasing
    returns to absolute once, with the cursor where the game restored it.
  - Hold-and-release repeatedly does not flap.
- [ ] **Recentring game.** If a title keeps a *visible* cursor pinned to the centre,
  it switches to relative mode. Note the title.

## Host desktop changes

- [ ] **UAC prompt (service mode).**
  - The secure desktop streams and the cursor stays usable.
  - If the host log reports cursor information unavailable, the overlay shows
    `fallback: host cursor unavailable` and the cursor is in the video.
  - After the prompt closes, the overlay returns to automatic.
- [ ] **UAC prompt with Vibeshine run as the user (not the service).** The overlay
  shows the fallback state, the host draws the cursor, and it recovers afterwards.
- [ ] **Lock screen (Win+L) and sign-in.** Same expectations as UAC.
- [ ] **Alt-tab on the client.**
  - Absolute mode keeps working when focus returns.
  - In relative mode in a window, focus loss releases the mouse, and a click
    recaptures it (same as game mode today).
  - No warps happen while the window is unfocused.
- [ ] **Alt-tab on the host.** Switching between a game and the desktop switches
  modes as above.

## Client window

- [ ] **Windowed client with a letterboxed aspect mismatch.**
  - Use a 16:9 stream in a square-ish window.
  - The local cursor's position matches the host's exactly across the video.
  - Outside the video (black bars) the normal client cursor shows, and clicks there
    do nothing.
  - Moving out of the window and back works.
- [ ] **Resize and fullscreen toggle (Ctrl+Alt+Shift+X).** The cursor rescales and
  stays aligned.
- [ ] **Capture toggle (Ctrl+Alt+Shift+Z).**
  - Releasing shows the normal cursor, and the video shows the host cursor again.
  - Recapturing restores the local cursor.
- [ ] **Shortcut overrides.**
  - Ctrl+Alt+Shift+M flips the mode and locks it; the overlay shows `(locked)`.
  - Ctrl+Alt+Shift+A locks the current mode, and pressing it again returns to
    automatic.
  - The lock ends with the session.
- [ ] **Gamepad mouse emulation** (long-press Start). The local cursor follows the
  gamepad-driven host cursor.

## Sessions

- [ ] **Two clients on one host.**
  - With one automatic client and one stock client, the host keeps drawing the
    cursor, the automatic client shows no second cursor (COMPOSITED), and both work.
  - With both clients automatic, the host stops drawing it.
  - When the stock client disconnects, the host stops drawing it for the remaining
    automatic client.
- [ ] **Reconnect mid-session.**
  - Quit the client and resume.
  - The new session gets a full resync (host log: subscribed), and the cursor shape
    is correct immediately.

## Compatibility

- [ ] **Stock Moonlight against this host.**
  - Behaviour is identical to the base Vibeshine: the cursor is in the video,
    absolute and relative modes behave as before.
  - The host log shows no `Client-side cursor` subscription.
  - The host's CPU usage is unchanged (the tracker never starts).
- [ ] **This client against stock hosts** (Sunshine, Apollo, Vibepollo, an older
  Vibeshine):
  - The overlay shows `fallback: host does not support automatic mode`.
  - With "Optimize mouse for remote desktop" off, the mouse is in game mode as today.
  - With it on, the mouse is in remote-desktop mode as today.
  - Ctrl+Alt+Shift+M toggles as before.
- [ ] **This client with automatic mode off.**
  - No `Mouse mode:` overlay line appears.
  - Ctrl+Alt+Shift+A reaches the host.
  - Behaviour is identical to the base moonlight-qt.
- [ ] **Linux host.** The feature is not advertised and the client uses the fallback.
