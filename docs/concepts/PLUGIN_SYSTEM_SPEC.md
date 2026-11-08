# Pixel VR Player plugin system — spec v1 (draft, 2026-11-08)

Status: **design for review**, nothing implemented yet. Background:
[HAPTICS_FUNSCRIPT_RESEARCH.md](HAPTICS_FUNSCRIPT_RESEARCH.md) and
[STEAM_CLOUD_WORKSHOP_RESEARCH.md](STEAM_CLOUD_WORKSHOP_RESEARCH.md).

## 1. Goals and non-goals

**Goals**
1. Optional features (interactive devices, sync bridges, home automation…) ship and update
   **separately from the player**. The Steam build contains only the generic host and no plugin.
2. A plugin can never crash, freeze or slow the player. Plugins run **out of process** and the
   host never blocks on them.
3. **Privacy by default.** A plugin only receives what its permissions grant. Never vault items
   (unless the user allows it per plugin, §6), never credentials or tokens.
4. **Testable without hardware.** The protocol is plain JSON lines, so a plugin can be driven by
   a simulator on a PC, and fake devices exist for every output we build (§9). We have no
   devices and must build blind.
5. **Language-agnostic.** A plugin is any program that reads stdin and writes stdout.
   - **First-party plugins are native C++** (decided 2026-11-08). They are built by our CMake/CI
     for aarch64 in `plugins/<name>/`, using the header-only SDK `plugins/sdk/pvr_plugin.hpp`
     (JSONL I/O, media-clock extrapolation; reuses `browser/mini_json.hpp`).
   - Third-party plugins may use anything. SteamOS also ships `/usr/bin/python3` (3.14.3,
     stdlib only, verified on the headset).
   - Dev and test tools (simulator, fake Intiface) may be Python; they are never shipped.

**Non-goals for v1**
- In-process plugins (`.so`), embedded scripting, plugins drawing their own 3D or textures.
- Sandboxing. A plugin is a normal program running with the user's rights. Permissions decide
  what *the host shares*; they cannot stop a plugin from reading files itself. The UI says so
  when a plugin is enabled ("Only enable plugins you trust").

## 2. Architecture

```
 Pixel VR Player (host)                         plugin process (python3 main.py)
 ┌───────────────────────────┐  stdin  (JSONL) ┌─────────────────────────────┐
 │ PluginHost                │ ──────────────▶ │ reads events, extrapolates   │
 │  • discovery / manifests  │                 │ the media clock, drives      │
 │  • spawn / restart / stop │ ◀────────────── │ devices / network / files    │
 │  • event fan-out          │  stdout (JSONL) └─────────────────────────────┘
 │  • request handling       │  stderr → ~/.cache/pixelvr/plugins/<id>.log
 └───────────────────────────┘
```
- **Code:** `src/plugins/plugin_host.{hpp,cpp}` and `plugin_protocol.{hpp,cpp}`, reusing
  `browser/mini_json.hpp`. Spawning copies `BrowserHost`'s `posix_spawn` + pipes pattern
  (`src/app/browser_host.cpp:46-86`).
- **Polling:** `PluginHost::poll()` runs once per frame with non-blocking reads, like `BrowserHost`.
- **Output limits:** writes go to a bounded queue per plugin. If a plugin stops reading, its
  state updates are coalesced (only the latest `media.state` per screen is kept) and it is
  restarted after the queue overflows.
- **App hooks** (few, so they merge easily with ongoing work in `app.cpp`): media opened/closed,
  play/pause/seek/rate/buffering, EOF, selected screen / audio focus, XR session focus
  (`XrContext::sessionFocused`), headset worn (`XrContext::userPresent`), app exit.

## 3. Files and lifecycle

| What | Where |
|---|---|
| Installed plugins | `~/.local/share/pixelvr/plugins/<id>/plugin.json` + files |
| Plugin data (writable, kept across updates) | `~/.local/share/pixelvr/plugin-data/<id>/` (passed as `PIXELVR_PLUGIN_DATA`) |
| Logs (stderr, rotated at 1 MB) | `~/.cache/pixelvr/plugins/<id>.log` |
| Enabled flags, grants, setting values | `~/.config/pixelvr/plugins.json` (host-owned) |

- **Discovery:** at startup and when the Plugins page opens.
- **Starting:** enabled plugins start once the app is running, with `cwd` = plugin dir.
  Environment: `PIXELVR_PLUGIN_API=1`, `PIXELVR_PLUGIN_ID`, `PIXELVR_PLUGIN_DATA`.
- **Handshake:** the plugin must answer `hello` within 5 s, otherwise it is killed and marked
  "not responding".
- **Crash:** restart after 1 s, 5 s and 30 s, then give up and show "stopped (see log)" on the
  Plugins page.
- **Stop or app exit:** host sends `shutdown` → waits 2 s → SIGTERM → waits 1 s → SIGKILL.
  **Plugins must stop all devices on `shutdown`, on stdin EOF and on SIGTERM.**
- **Install:**
  - v1: a `*.pvrplugin` file (a gzip tar containing one plugin folder, unpacked with the
    system `tar` into a staging dir and validated before it is moved) opened in the file browser shows
    "Install plugin X (permissions …)?", then it is unpacked into the plugins dir. The flow:
    download in our browser → Downloads → click it.
  - Copying the folder by hand also works.
  - Update = install a newer version over the old one (the data dir is kept).
- **Uninstall:** a button on the plugin's page; it asks whether to keep its data.

## 4. Manifest (`plugin.json`)

```json
{
  "id": "org.pixelvr.funscript",
  "name": "Funscript devices",
  "version": "0.1.0",
  "api": 1,
  "description": "Plays .funscript files on Intiface (Buttplug.io) devices in sync with the video.",
  "author": "Pixel VR Player",
  "homepage": "https://github.com/Pixelplanet/Pixel-VR-Player",
  "license": "MIT",
  "exec": ["python3", "main.py"],
  "permissions": ["playback", "files.sidecar", "ui", "network"],
  "sidecars": ["{base}.funscript", "{base}.*.funscript"],
  "settings": [
    { "key": "server", "type": "text",   "label": "Intiface address", "default": "ws://127.0.0.1:12345" },
    { "key": "offset", "type": "choice", "label": "Sync offset",
      "options": ["-200 ms","-100 ms","0 ms","+100 ms","+200 ms"], "default": 2 },
    { "key": "scan",   "type": "action", "label": "Scan for devices" }
  ]
}
```
- `exec[0]` is looked up next to the manifest first, then in `PATH`.
- Setting types map 1:1 onto the existing `SettingsMenu::Row` kinds
  (`src/ui/settings_menu.hpp:73`): `toggle` → Toggle, `choice` → Choice (stepped),
  `segmented` → Segmented, `action` → Action (button), `text` → the existing keyboard / text entry.
- A plugin can replace its setting list at runtime (`ui.settings`), e.g. to fill a device choice.

## 5. Protocol (api 1)

- **Encoding:** UTF-8, one JSON object per line, at most 1 MiB per line. Every message has
  `"type"`. Requests from the plugin may carry `"id"` (int); the host answers with
  `{"type":"result","id":…,"ok":true,"data":…}` or `"ok":false,"error":"…"`.
- **Versioning:** unknown fields and unknown message types **must be ignored** by both sides.
  The `api` major version must match; additions bump `apiMinor` in `hello`.

### 5.1 Host → plugin events

| type | fields | when |
|---|---|---|
| `hello` | `api`, `apiMinor`, `app` (version), `granted` [perms], `settings` {key: value} | first line |
| `screens` | `screens`: [{`screen`, `kind`: video/web/empty, `selected`, `audio`}] | after hello and on every change |
| `media.open` | `screen`, `media` (opaque id), `fileName`, `title`, `source` (local/smb/sftp/webdav/plex/http), `duration` (s), `projection` (flat/180/360), `stereo` (mono/sbs/tb), `sidecars` [names found], `path` (only with `playback.path`) | file opened on a screen |
| `media.state` | `screen`, `media`, `playing`, `buffering`, `position` (s), `rate`, `clock` (ms, CLOCK_MONOTONIC when `position` was sampled), `reason` (play/pause/seek/rate/buffer/tick) | **immediately** on every change, plus a `tick` every 250 ms while playing |
| `media.sidecars` | `screen`, `media`, `sidecars` [names this plugin may read] | the folder listing finished (it runs on a worker thread, so `media.open` usually carries `[]`) |
| `media.close` | `screen`, `media`, `reason` (stop/eof/replaced/error) | file closed |
| `app.state` | `focused` (XR session focused), `worn` (headset on), `exiting` | on change |
| `settings` | `settings` {key: value} | the user changed a value |
| `action` | `action` (key of an action setting or quick button) | user pressed it |
| `shutdown` | — | plugin disabled or app exiting |

**Clock rule:** host and plugin run on the same machine, so the plugin extrapolates exactly
with `time.monotonic()`:
`now_pos = position + (monotonic_ms() - clock)/1000 * rate` while `playing && !buffering`.
`position` must be the presentation time of the frame being shown, not the demuxer position.
Remaining display latency is covered by the plugin's offset setting.

### 5.2 Plugin → host requests

| type | fields | permission | notes |
|---|---|---|---|
| `hello` | `api`, `name`, `version` | — | reply to the host's hello |
| `status` | `text`, `level` (ok/warn/error) | — | one line on the plugin page and in the Plugins list ("Connected – 2 devices") |
| `notice` | `text`, `seconds` | `ui` | short toast in VR; rate-limited to 1 per 3 s |
| `ui.settings` | `settings` [definitions as in the manifest] | `ui` | replaces the settings list |
| `ui.timeline` | `screen`, `media`, `tracks`: [{`id`, `label`, `segments`: [[startMs, endMs, heat 0..1], …]}] | `ui` | heat strip drawn on the timeline under the chapter marks (≤ 2000 segments per track); `[]` clears it |
| `ui.gauge` | `id`, `label`, `value` 0..1 | `ui` | small live meter above the control panel, ≤ 30 updates/s. Also the **virtual device** for blind testing |
| `ui.button` | `id`, `label`, `icon` (stop/play/link/…) | `ui` | one quick button per plugin in the control panel; presses arrive as `action` |
| `control` | `id`, `screen`, `media`, `do` (play/pause/seek), `position` | `control` | v1: play, pause, seek. `rate` and `open` are answered "unsupported" (the engine has no rate; opening files from plugins needs a security review) |
| `files.read` | `id`, `screen`, `media`, `name` | `files.sidecar` | returns `{text}` (UTF-8) or `{base64}`, max 16 MiB. Only files in the **same folder** as the open media whose names match the manifest's `sidecars` patterns. The host reads them through its own SMB/SFTP/WebDAV code, so plugins never need credentials. |
| `log` | `level`, `text` | — | written to the plugin log (stderr works too) |

## 6. Permissions and privacy

| Permission | Grants |
|---|---|
| `playback` | `screens`, `media.*` events with file name, title, source, duration, format |
| `playback.path` | adds the full local path or network URI (**credentials always removed**) |
| `control` | `control` requests |
| `files.sidecar` | `files.read` for declared sidecar patterns |
| `ui` | `notice`, `ui.*` |
| `network` | informational only: the plugin opens network connections or ports. Shown to the user, can't be enforced |

**Host-enforced rules (not overridable):**
- **Never:** passwords, SMB credentials, Plex tokens, browser cookies or URLs with tokens.
  - For Plex the host reports the server's file name (`Part.file` base name, already resolved for
    downloads) and `source: "plex"`.
  - Web screens are reported as `kind: "web"` only (no URL or title in v1).
- **Vault:** media from the vault is reported as `media.open` with `private: true` and no name,
  path or sidecars. Devices see the screen as busy but get nothing to play.
  - A per-plugin grant **"Use with vault videos"** (off by default, only shown while the vault is
    unlocked) lifts this for that plugin.
  - It is a likely use case: private videos with scripts next to them.
- `app.state` with `worn:false` or `focused:false` is a hard pause signal: first-party plugins
  stop devices immediately.

## 7. UI in the host
- **Settings → Controls & Input → "Plugins ›"** (a sub-page like Shortcuts; a rail tab of its own
  can come later):
  - One row per installed plugin: name, version, status line, enable switch.
  - A plugin's page: description, permission list, status, its settings (rendered with the
    existing row kinds), the vault grant (§6), "Restart", "Uninstall".
- **Control panel:** at most one quick button per enabled plugin (`ui.button`), and the gauge
  strip (`ui.gauge`) above the panel while a gauge is active.
- **Timeline:** heat tracks (`ui.timeline`) drawn under the chapter marks
  (`VulkanRenderer::setTransportChapters` is the neighbour, `src/gfx/vulkan_renderer.hpp:263`).

## 8. What plugins can do (why the system is worth it beyond devices)

| Plugin | Uses | Notes |
|---|---|---|
| **Funscript devices** (first-party, §10) | playback, files.sidecar, ui, network | the reason we start |
| **DeoVR-compatible sync bridge** (first-party) | playback, control, network | TCP 23554 server ([DeoVR protocol](https://deovr.com/documentation) §8). MultiFunPlayer, FunFlixDroid, ScriptPlayer and XBVR then work with every device they support. Also lets testers who own MultiFunPlayer test before our own plugin is finished. |
| Cinema lights (Home Assistant / Hue / WLED) | playback, network | dim on play, restore on pause/stop |
| Phone remote | playback, control, network | serves a small web page: play/pause/seek/volume from the phone |
| Watch together | playback, control, network | two headsets keep the same file in sync through a relay |
| Scrobbling (Trakt / Simkl), Jellyfin/Emby progress | playback.path, network | watch history |
| Sleep timer / stats | playback, control, ui | |
| Later API additions: `video.ambient` (ambilight colour at 10 Hz → smart lights), `files.write.sidecar` (subtitle downloaders) | — | not in v1 |

## 9. Building blind: test tooling (first-party, stdlib Python, runs on Windows / CI / headset)
- **`tools/plugin_sim.py`: host simulator.**
  - Starts a plugin and plays a scripted session: open a 120 s video with sidecars from a
    folder, play, seek to 30 s, pause, resume, rate 1.5×, buffering, headset off, close,
    shutdown.
  - Records everything the plugin sends and asserts protocol rules: hello in time, no output
    after shutdown, etc.
- **`tools/fake_buttplug_server.py`: fake Intiface.**
  - A WebSocket server speaking Buttplug spec v3 and v4 with virtual devices: 1 linear (stroker),
    1 vibrator, 1 rotator.
  - Logs each command with a timestamp, so tests can check that commands match the funscript
    within a tolerance and that stop commands arrive on pause, seek, headset-off and shutdown.
- **`tools/fake_deovr_client.py`:** checks the sync bridge the way MultiFunPlayer would.
- **In the headset:** the funscript plugin always drives a **gauge** (virtual device). Play any
  video with a funscript next to it, and the bar should move with the action. That is how you
  check sync without hardware.
- **Real hardware:** testers with Intiface Central on their phone (Android/iOS, "listen on all
  interfaces"). The release notes ask for reports with the device model and the plugin log.

## 10. First-party plugin: Funscript devices (built on top)
- **Scripts:**
  - Sidecars `{base}.funscript` plus axis files `{base}.{surge|sway|twist|roll|pitch|suck}.funscript`,
    read through `files.read`, so they also work on SMB, SFTP and WebDAV.
  - A setting "Script folders" (local folders on the headset) matched by `fileName`, which also
    covers Plex.
- **Parsing:** sort, clamp to 0–100, honour `inverted`, ignore unknown fields. Multi-axis
  single-file formats come later.
- **Outputs:**
  - **Gauge** (always on).
  - **Buttplug client:** our own minimal stdlib WebSocket client. Spec v4 first, falling back to
    v3. Linear devices get "position over duration" for the next point. Vibrators and rotators
    get intensity from the script's speed, with min/max and curve settings.
  - Later: TCode over UDP/serial (OSR2/SR6), and The Handy via its API (needs a Handy app key).
- **Sync:** a 100 Hz loop. Commands are scheduled `offset` ms ahead of the extrapolated clock,
  with a soft start of about 1 s after play or seek.
- **Safety:** stop all on pause, seek, close, `focused:false`, `worn:false`, shutdown, lost
  connection and plugin errors. Global stroke range and speed limits. A "Stop devices" quick button.
- **UI:** heat track on the timeline. Status ("Intiface: 2 devices"). Settings: address, scan,
  device toggles, offset, range, speed limit, vibration mapping, script folders.

## 11. Implementation phases
1. **Host core:** discovery, manifest, spawn/restart/stop, JSONL, `hello`, `screens`/`media.*`/
   `app.state` events, `status`/`log`/`notice`, Plugins page with the enable switch.
   - Built with `tools/plugin_sim.py` and an example plugin (`plugins/example/` logs everything
     and shows a gauge).
2. **Settings and files:** manifest settings rendered as rows, `ui.settings`, text settings
   through the keyboard, `files.read` (local + `remote_fs` + SMB), `control`.
3. **UI extensions:** timeline heat track, gauge strip, quick button.
4. **First-party plugins:** DeoVR sync bridge (small, a second consumer of the API), then
   Funscript devices with the fake Buttplug server tests.
5. **Packaging:** `.pvrplugin` install from the file browser; plugins published as separate
   release assets, never in the Steam depot.

**Coordination:** another agent is changing `app.cpp` / the renderer on `feature/browser-screen`.
Keep the host in new files, add the app hooks last, and build on the headset in a **separate
source dir** (e.g. `~/pixelvr-src-plugins`). Don't deploy over `~/PixelVRPlayer` without
coordination.

## 12. Decisions (2026-11-08, by the user)
- **Vault:** per-plugin opt-in, as in §6. Off by default, only offered while the vault is unlocked.
- **First-party plugins:** native C++ (see §1.5). The funscript plugin's WebSocket client is a
  small copy of the browser helper's `class WebSocket` (`browser/pvr_browser.cpp:207`), not a
  shared refactor, to avoid conflicts with the browser work.
- **Distribution:** separate `.pvrplugin` downloads for every build. The Steam depot and the
  GitHub tarball contain only the host.
- **No hardware in the team:** everything is built blind. The fake Intiface, the simulator and the
  in-VR gauge (§9) are the acceptance tests until users report back.

## 13. Progress log (keep updated — another agent may continue from here)
Branch `feature/plugin-system`, worktree `C:\Projects\PixelVRPlayer-plugins` (based on
`research/steam-cloud-workshop-haptics`, which is `origin/main` + research docs). Headset builds
go to `~/pixelvr-src-plugins` (never `~/pixelvr-src`, which belongs to the other agent).
- [x] Spec v1 written and decisions recorded.
- [~] Phase 1 host core.
  - [x] `src/plugins/plugin_host.{hpp,cpp}`: discovery, manifests, config, spawn (own process
    group, env scrubbed, stderr → log), restart backoff, hello timeout, shutdown → SIGTERM →
    SIGKILL, snapshot diffing (open / play / pause / seek / rate / buffer / tick / close), vault
    rule, credential stripping, notices, gauge storage, `ui.settings`, settings values.
  - [x] SDK `plugins/sdk/pvr_plugin.hpp` (Io, MediaClock, Screens) + `plugins/example/`.
  - [x] `tools/plugin_host_test.cpp`: 30 checks, **all pass on the headset** (quick g++ build in
    `~/pixelvr-src-plugins/qb`):
    `g++ -std=c++20 -Isrc tools/plugin_host_test.cpp src/plugins/plugin_host.cpp src/util/logging.cpp`.
  - [x] App hook: `App::updatePlugins()` builds the snapshot each frame (also while no XR
    session runs, so plugins see `focused:false`); `~App` calls `shutdownAll()` first; plugin
    notices use the existing notice line; Plex screens report the Plex title, never the URL.
  - [x] Settings → Controls & Input → "Plugins" row → list (section 30) → plugin page
    (section 31): Enabled, its settings (toggle / choice / segmented / action / text via
    the keyboard), "Use with vault videos" (only while unlocked), Restart, Permissions.
    The page refreshes every 0.5 s for live status.
  - [x] Full CMake build on the headset in `~/pixelvr-src-plugins/build` (deps reused from
    `~/pixelvr-src/build/_deps` via `FETCHCONTENT_SOURCE_DIR_*`, read-only); `plugin_host_test`
    passes against the CMake-built example plugin.
  - [ ] **In-headset check (needs the user):** install the example plugin
    (`cp -r build/plugins/example ~/.local/share/pixelvr/plugins/`), run this build, enable it
    under Controls & Input > Plugins, play a video: a notice "Opened …", status "Watching …".
    The gauge is stored by the host but not drawn yet (phase 3).
- [x] Phase 2 settings / files / control.
  - `src/plugins/plugin_files.{hpp,cpp}`: list / read for local, SMB (own context per job like
    MediaEngine; the global `smbc_init` only exists after the file browser browsed SMB),
    SFTP and WebDAV. Runs on PluginHost's worker thread.
  - Sidecar listing on open → `media.sidecars`; `files.read` (16 MiB cap, text or base64)
    only for names matching the plugin's own patterns; `control` play / pause / seek, applied
    in `App::updatePlugins`.
  - Text settings via the in-app keyboard (`Action::EditPluginText`).
  - Host test: all checks pass (sidecar filtering, content, control). **SMB sidecars not yet
    tried against a real share.**
- [ ] Phase 3 timeline heat, gauge, quick button.
- [ ] Phase 4 DeoVR bridge plugin, funscript plugin + fake Intiface tests.
- [ ] Phase 5 `.pvrplugin` install + release assets.
