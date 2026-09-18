# TinyProcess

Desktop tray app that displays a web page and plays API-selected videos on an
ESP32 display. See [firmware setup and wiring](../esp32/README.md).

## Run

Use Node.js 22 or later. From `tinyprocess/`:

```sh
npm ci
npm start
```

In **Settings**, enter the config endpoint URL. Leave the USB port blank to
select a single connected Espressif device, or enter a port such as `/dev/ttyACM0`
or `COM4`. Connect the ESP's native USB port and sign in through the app window.
API requests and downloads share that login session.

Closing the window hides it to the tray. Choose **Quit** to stop the app.

## Config endpoint

Return the page and info URLs, plus any button actions:

```json
{
  "success": true,
  "data": {
    "endpoints": {
      "page": "/pmt/gadget/tinytask/display",
      "info": "/pmt/api/gadget/tinytask",
      "button1Pressed": { "action": "request", "type": "POST", "url": "/pmt/api/gadget/tinytask/toggle" },
      "button1Held": null,
      "button1DoubleClicked": null,
      "button2Pressed": null,
      "button2Held": null,
      "button2DoubleClicked": null,
      "bothButtonsPressed": { "action": "toggleWindow" },
      "bothButtonsHeld": null
    }
  },
  "error": null
}
```

`page` and `info` are required. URLs may be absolute HTTP(S) links or relative
to the config URL. Make the config endpoint accessible before login.
Use **Reconnect**, save Settings, or restart the app to reload the config.

## Buttons and window actions

Button 1 connects GPIO23 to GND; button 2 connects GPIO24 to GND. Use normally
open momentary switches with the current firmware.

- Requests: `{ "action": "request", "type": "GET", "url": "/path" }` (or `POST`).
- Window actions: `{ "action": "toggleWindow" }`, `showWindow`, or `hideWindow`.
- Omit an action or set it to `null` to disable it.

Holds trigger after one second. Double clicks and holds suppress single presses;
combined gestures suppress individual actions. There is no combined double click.

Button requests send no body. After an action finishes, the app fetches fresh info;
the button response body does not control playback. Neither button actions nor
info polling automatically reload the web page.

## Info endpoint

The server chooses the MP4 and optional overlays:

```json
{
  "success": true,
  "serverTime": "2026-09-18T12:00:05.000Z",
  "data": {
    "video": "green-static.mp4",
    "downloadUrl": "/pmt/api/gadget/tinytask/videos/green-static.mp4",
    "checksum": "c37b9fed6f30a8ff19d8850dc4eb9ac18688c5b6d0caea0a2fb89a7d809e4488",
    "overlays": [
      {
        "type": "timer",
        "id": "work-timer",
        "direction": "up",
        "state": "running",
        "anchorTime": "2026-09-18T12:00:00.000Z",
        "valueSeconds": 0,
        "format": "mm:ss",
        "position": "center",
        "fontSize": 24,
        "color": "#FFFFFF",
        "backgroundColor": "#00000080"
      }
    ]
  },
  "error": null
}
```

Use the actual MP4's SHA-256 checksum. Downloads are verified and cached; a cached
video is reused when its checksum matches. Relative download URLs resolve against
the info URL. Videos loop at 160×160 pixels and 10 fps.

Info is fetched roughly every two seconds. Invalid data, failed videos, or logout
show the idle animation.

### Overlays

Omit `overlays` or use `[]` to clear them. For text, use an entry such as:

```json
{ "type": "text", "text": "Working", "position": "center", "fontSize": 24, "color": "#FFFFFF" }
```

Text and timers support `top`, `center`, or `bottom` positioning, font sizes from
8 to 64, and colors in `#RRGGBB` or `#RRGGBBAA` format. `backgroundColor` is optional.

Timers require top-level `serverTime` and an `anchorTime`, both UTC timestamps
ending in `Z`. `valueSeconds` is the value at the anchor. Timers advance locally
between requests; keep the ID, anchor, and value stable during normal polling.

Use `up` or `down`, `running` or `paused`, and `mm:ss` or `hh:mm:ss`.
To pause, send `state: "paused"` with the current value. To resume, send that
value with a new anchor and `state: "running"`.

## Packaging

```sh
npm run dist -- --publish never
```

Build on the target operating system. Packages include FFmpeg and the idle
animation. Linux Debian builds need a project homepage; CI supplies the repository
URL. For local builds without Git remote metadata, add
`-c.extraMetadata.homepage=https://github.com/OWNER/REPOSITORY` with your actual URL.

If you change `esp32/media/colors-idle.mp4`, regenerate the bundled desktop
animation with `npm run build:media` (requires FFmpeg).
