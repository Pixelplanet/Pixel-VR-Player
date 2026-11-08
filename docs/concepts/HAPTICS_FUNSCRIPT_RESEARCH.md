# Interactive devices (Buttplug.io / Intiface, Lovense, The Handy, OSR) and funscripts — research (2026-11-08)

Research only; nothing implemented. "Verified" means checked against the linked docs or the
headset; everything else is an assumption or open question.

## TL;DR

How it works: a **funscript** is a JSON list of timed positions. A **script player** reads the
video player's current time and drives the device a little ahead of it. The video player only
has to **report its playback time, play/pause and file**, or play the script itself.

Recommended in three phases. Each phase is useful on its own, and only phase 2 puts
device-specific code into the app.

1. **Sync API (small, no device code):** a DeoVR-compatible remote-control server (TCP 23554).
   Existing tools then sync immediately and drive every device type they support:
   MultiFunPlayer on a PC, FunFlixDroid on Android, ScriptPlayer, XBVR session tracking.
2. **Built-in funscript player (medium):** load the `.funscript` next to the video. A Buttplug
   client talks to **Intiface Central** (on the user's phone or PC, or later on the headset) and
   covers 750+ devices including Lovense, Kiiroo and The Handy. Includes a heatmap on our timeline.
3. **On-headset engine (optional):** run the BSD-licensed `intiface-engine` built for aarch64 next
   to the player, so no phone or PC is needed. The Frame has BlueZ with BLE central available
   (verified on the headset).

Plugin question: phase 1's API, generalised into a local **"Pixel VR Player Remote API"**
(WebSocket JSON), *is* the plugin system. Plugins are out-of-process, in any language, with
crash isolation. It also enables non-adult uses (Home Assistant lights, phone remote,
Discord status), and lets us ship haptics as a separate download if Steam requires it (§6).

## 1. Funscript (verified)

```json
{ "version": "1.0", "inverted": false, "range": 100,
  "actions": [ { "at": 0, "pos": 10 }, { "at": 400, "pos": 90 } ] }
```
- `at` is in milliseconds from the start of the media. `pos` runs 0–100. Between points the
  device moves in a straight line, so the speed comes from the distance and the time
  ([community spec](https://docs.ananga.ai/funscript/spec/), [launchcontrol](https://github.com/funjack/launchcontrol/blob/master/protocol/funscript/funscript.go)).
- **Players should:** sort the points, clip positions to 0–100, honour `inverted`
  (`100 - pos`), and ignore unknown fields.
- **Multi-axis scripts:** sibling files `video.twist.funscript`, `.roll.`, `.pitch.`, `.surge.`,
  `.sway.`, `.suck.` (TCode axes L0–L2, R0–R2, A1). Newer single-file variants use
  `axes`/`channels` ([funlib](https://github.com/Eroscripts/funlib)).
- **Discovery:** same base name next to the video. MultiFunPlayer also supports script library
  folders.

## 2. Ecosystem

| Piece | What it is | Licence | Notes |
|---|---|---|---|
| [Buttplug](https://buttplug.io) / `intiface-engine` | Device-control protocol + server (Rust), 750+ devices | **BSD-3** | Clients speak JSON over WebSocket to the server (default `ws://127.0.0.1:12345`). Spec v4 is current; the server also accepts older spec versions. |
| [Intiface Central](https://intiface.com/central) | GUI around the engine | GPL-3 / commercial | Windows, macOS, Linux (Ubuntu zip), **Android, iOS**. "Listen on all interfaces" lets a headset connect to the phone. |
| Lovense | Own apps: Lovense Remote "Game Mode" LAN API (`http://<ip>:20010/command`), Socket API, Toy Events API ([dev docs](https://developer.lovense.com/docs/standard-solutions)) | proprietary | Buttplug also drives Lovense directly over BLE, so we don't need Lovense's API. |
| The Handy | Wi-Fi device with its own cloud API (HSSP/HSP, firmware 4) | proprietary | HSSP doesn't host scripts on private networks ([API spec](https://www.handyfeeling.com/api/handy-rest/v3/docs/spec.yaml)). Buttplug/Intiface or MultiFunPlayer cover it. |
| OSR2 / SR6 (TCode) | DIY multi-axis strokers, serial/UDP/WebSocket | open | Best served by MultiFunPlayer (phase 1). |
| [MultiFunPlayer](https://github.com/Yoooi0/MultiFunPlayer) | Script player that follows external video players | open source | Follows DeoVR, HereSphere, MPV, VLC, Whirligig, **Plex, Emby, Jellyfin**. Outputs to Buttplug, TCode, The Handy. Has its own C# plugins. |
| ScriptPlayer, FunFlixDroid (Android), XToys | Other script players | various | FunFlixDroid follows HereSphere's timestamp server from a phone. |
| VR players on **Steam** with toy support | **HereSphere** (paid, Handy/Autoblow/BT toys), **DeoVR** (Handy via SLR) | — | **Precedent:** VR video players with funscript/device support are on Steam ([overview](https://funscripted.com/guides/funscript-players)). |

## 3. Phase 1 — DeoVR-compatible sync API

**Protocol (verified, [DeoVR docs §8](https://deovr.com/documentation)):**
- Clients connect to TCP **23554**.
- Each packet is a 4-byte little-endian length + UTF-8 JSON. An empty packet is a ping.
- Both sides send at least one packet per second; after more than 3 s of silence the
  connection closes.
- The player sends `{"path","duration","currentTime","playbackSpeed","playerState"}`, where
  `playerState` is 0 for play and 1 for pause.
- Clients can send `path` (open a file), `currentTime` (seek), `playbackSpeed` and `playerState`.
- Same structure as XBVR's client ([remote.go](https://github.com/xbapps/xbvr/blob/adfe592b/pkg/session/remote.go)).

**What we'd build:**
- A small server thread in the app, **off by default** (Settings → "Allow sync apps on the
  network").
- It reports the selected screen's media. Send state changes (play, pause, seek) immediately,
  not only on the 1 s tick; clients extrapolate in between.
- Accept seek and play/pause commands. `path` commands only from the local network.
- Test with MultiFunPlayer (Windows PC on the LAN) and FunFlixDroid (Android).
- HereSphere's "timestamp server" uses the same port and a similar idea. It's undocumented, so
  match it by testing later, if a tool needs it.

**Privacy and safety rules:**
- Off by default. Show in the UI when a client is connected.
- **Never report vault items** (send nothing while one plays).
- **Never report Plex URLs with `X-Plex-Token`**: send the Plex `Part.file` base name instead.
  Script tools match by file name, so this works better anyway. SMB/SFTP/WebDAV URIs: send
  without credentials.
- LAN only; maybe an optional pairing code.

**Bonus to test:** MultiFunPlayer can follow Plex sessions. We already report the timeline to
Plex during native playback, so MultiFunPlayer's Plex source might already work. Unverified.

## 4. Phase 2 — built-in funscript player + Buttplug client

- **Scripts:** load `<video>.funscript` and the axis siblings next to the video, also over SMB,
  SFTP and WebDAV (`remote_fs`, same base name). For Plex, match by the server file name we
  already resolve, in a "script folder" setting.
- **Timeline:** draw a heatmap on the scrub bar: speed-coloured, like OpenFunscripter and
  MultiFunPlayer.
- **Buttplug client** in C++:
  - It's a WebSocket + JSON protocol, and the browser helper already has a small WebSocket
    client (`browser/pvr_browser.cpp`, `class WebSocket`) to borrow.
  - Messages (spec v4): `RequestServerInfo` → `ServerInfo` (ping interval), `StartScanning`,
    `DeviceList` (full list each time, v4), output commands, `StopCmd` / stop-all
    ([messages](https://buttplug.io/docs/spec/messages/),
    [connection](https://buttplug.io/docs/spec/identification.md),
    [devices](https://buttplug.io/docs/spec/device_information/)).
  - Linear devices get "move to `pos` within `duration`" for the next point. Vibrators and
    rotators get a speed derived from the script's speed.
- **Sync:** schedule each command `latency` ms ahead of the media clock. Offer a per-device
  offset slider (−300…+300 ms). Resync on seek, rate change and buffering.
- **Where Intiface runs:**
  - User's phone (Android/iOS Intiface Central with "all interfaces" on): easiest, and Bluetooth
    stays on the phone.
  - User's PC.
  - Phase 3: on the headset.
  - The Frame's Android layer (Lepton) reports no `bluetooth_le`
    ([frame-control notes](https://github.com/saphid/frame-control/blob/main/docs/apks.md)), so
    the Intiface Android app on the Frame is not an option.
- **Safety (must-have):**
  - Stop all devices on pause, seek, screen close, app exit, crash or disconnect, and when the
    headset is taken off.
  - Speed limits for strokers, soft start.
  - A big "Stop" in the control panel.

## 5. Phase 3 — Intiface engine on the headset

- `intiface-engine` (BSD-3, in the main Buttplug repo) builds natively on aarch64: Raspberry Pi
  and Home Assistant add-ons do it ([HA add-on](https://pol4rfuchs.github.io/ha-apps/intiface-central/)).
- **The Frame side (verified on the headset):** `bluetooth.service` is active, and the adapter
  is powered with **central and peripheral** roles.
- **Plan:** ship it as a separate process the player starts on demand, listening on localhost only.
- **Open questions:**
  - Can our user session reach BlueZ over D-Bus without extra permissions?
  - Does BLE scanning coexist with the Frame's controllers on the same radio without lag?

## 6. Steam policy (the real risk)

- **July 2025:** Steam added a rule against "content that may violate the rules and standards
  set forth by Steam's payment processors…", and adult titles were removed
  ([Polygon](https://www.polygon.com/news/613411/steam-sex-adult-games-rules-changes-july-2025/),
  [Guardian](https://www.theguardian.com/games/2025/jul/29/why-did-adult-titles-disappear-from-steam-itch-pc-gaming-payment-processors)).
  Valve evaluates "case by case".
- **But:** toy support is on Steam today, in HereSphere and DeoVR (video players) and in adult
  games such as Dominatrix Simulator: Threshold
  ([news](https://steamcommunity.com/app/953270/allnews/)) and JOI Lab.
- **Our app contains no sexual content.** Device support is a feature, not content.

**Mitigations, in order:**
1. Phase 1 is a neutral, generic sync/remote API ("compatible with DeoVR remote control").
2. Name things neutrally in the store and UI: "Interactive devices (Intiface)", "Haptic scripts".
3. Ask Valve through Steamworks support before shipping phase 2 in the Steam build.
4. Fallback: haptics as a separate download (GitHub) that talks to the player through the
   Remote API. Same features, nothing adult inside the Steam depot.
5. No funscripts or adult material on our Workshop (see the Steam Cloud/Workshop doc).

## 7. Plugin system options (if we want more than phase 1)

| Option | Pros | Cons |
|---|---|---|
| **Out-of-process Remote API** (WebSocket JSON: events = playback state, screens, chapters; commands = play/pause/seek/open/notice) | Any language, crash isolation, distributable outside Steam, also serves phone remotes and Home Assistant | Latency (fine for scripts, since the clock is reported), plugins need their own UI |
| In-process C ABI `.so` plugins | Can draw UI, lowest latency | Crashes take down the player, arm64 builds per plugin, security |
| Embedded scripting (Lua/QuickJS) | Sandboxed, small scripts | Engine to maintain, limited I/O |

**Recommendation:** the out-of-process Remote API, plus a small "plugin panel" in our UI that a
plugin can fill with simple controls (toggle, slider, button, label) over the same API.
That gives haptics plugins in-VR settings without in-process code.

## 8. Open questions / tests
- [ ] MultiFunPlayer + phase 1 server: sync accuracy with 1 s ticks plus immediate state
      events. Measure with a camera, as the community does.
- [ ] HereSphere timestamp-server format (only if a tool needs it).
- [ ] Phase 3: intiface-engine on the Frame (BlueZ permissions, BLE next to the controllers).
- [ ] Valve's answer on toy support in a general media player.
- [ ] Multi-screen: which screen drives the devices? Proposal: the selected screen (the one
      audio follows), shown in the UI.
