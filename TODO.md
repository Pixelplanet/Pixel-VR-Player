# Pixel VR Player — TODO & Handoff

> Native C++/OpenXR/Vulkan/FFmpeg VR video player for the **Valve Steam Frame**
> (Snapdragon 8 Gen 3, Adreno 750 / Turnip, SteamOS/aarch64). This file is the
> handoff for the next agent. Deep device/build facts live in repo memory
> `/memories/repo/steamframe-facts.md` — read that too.

---

## OPEN NOW (2026-11-08) — start here

Priority: everything in v1.14.0 is done and user-verified on the headset (2026-11-08, also the
in-app updater). N1/N2, L1, R1, K1/K2 are done and user-verified. Next candidates:
- **C2. Steam Input actions for the sticks** (new, user request 2026-11-08), see "Controls" below.
- **C3. Controls Stage 2:** Options menus while watching / on web pages, previous / next file in a
  folder, small volume / seek indicators, vault page showing the bound button for each combo step.
- **C4. On-screen controls rethink** (parked by the user): music view dock buttons = prev / next
  track instead of ±10 s.
- D1 2D→3D (needs a model/runtime decision; research branch below).
Foveated rendering is NOT needed for now (8K60 180° = ~2-3 ms GPU at 2160 eyes after the
single-sample 360/180 pass + visibility mask). Y*/S1 are parked; M5 is low priority.

**New 2026-11-08:** plugin system (P1), Jellyfin (J1), stereo convergence / autofocus (V1/V2),
passthrough (PT0–PT3), Steam Cloud / Workshop (SC1) and the D1 research live in separate
worktrees / branches. See "New items 2026-11-08" below for where each one is documented.
**Those branches fork from `be3a940` (v1.13.x)** and are 12+ commits behind `main` (v1.14.0
reworked input: `XrInputState` → semantic `InputAction`s, in-app remapping removed, the
Controls settings pages replaced, controller models / labels moved to quad layers in
`vulkan_renderer.cpp`). Rebase / merge onto `main` before building on them; P1 especially
touches `app.cpp`, `settings_menu.*` and `vulkan_renderer.*`.

State: released as **v1.14.0** (2026-11-08; notes in `docs/release-notes/v1.14.0.md`, tag push
triggers `.github/workflows/release.yml`): SteamOS-native controls (one action set, SteamVR is the
only place buttons are assigned; src/xr/xr_context.cpp kActionDefs), floating button labels
(src/ui/button_labels.*, layout/draw in vulkan_renderer.cpp, controller models + labels in cropped
per-eye quad layers), exact seeks (media_engine), adaptive eye resolution (src/xr/render_scale.hpp,
floor 2160), GPU timestamps in PERF, single-sample 360/180 eye pass, XR_KHR_visibility_mask.
The launch fix ("Up next" instead of "Resume Game") lives in `src/xr/vr_app_manifest.cpp` and
`tools/install.sh`. Headset: `ssh -i "$HOME/AppData/Local/steamos-devkit/steamos-devkit/devkit_rsa"
steamos@192.168.178.61`; installed app is `~/PixelVRPlayer`.

### Controls (after v1.14.0)
How input works now: one OpenXR action set `gameplay` ("Player Controls"); every action is in
`kActionDefs` (`src/xr/xr_context.cpp`) with one `InputAction` enum entry each; SteamVR is the
only place buttons are assigned (Settings › Controllers › Manage Controller Bindings); the app
reads live bindings with `xrEnumerateBoundSourcesForAction` for the Controller Buttons pages and
the floating labels (`src/ui/button_labels.*`). SteamVR shows the raw `actionName` (it drops
`localizedActionName`), so names must read well on their own (`play-ok`, `main-menu`, …) and may
not start with a digit. Renaming an action breaks users' saved custom bindings: say so in the
release notes.

- [ ] **C2. Steam Input actions for the sticks** (user request 2026-11-08). Today both sticks are
  only exposed as two vector2 actions, `scrub-distance` (right) and `skip-distance` (left), so in
  SteamVR's binding UI a stick can only be bound "as joystick" to one of those; scrub, fast
  forward, skip or screen distance cannot be picked as separate actions, and a stick set to
  D-pad / buttons mode has nothing sensible to bind to. Plan:
  - Split the meaning out of the stick: e.g. `scrub` (vector1 or vector2 X, variable speed),
    `screen-distance` (vector1), `zoom` / `tilt` for 360/180, and boolean actions per step
    (`back-10s` / `forward-10s` exist as `left-back-10s` / `right-forward-10s`, `back-30s` /
    `forward-30s`, `screen-closer` / `screen-farther`, `scrub-back` / `scrub-forward` held =
    scrub at a fixed speed).
  - Keep today's behaviour as the default binding (right stick = scrub X + distance Y, left =
    skip 30 s + distance; 360/180 right = tilt/zoom, left X = scrub), so nothing changes for
    users who never open the binding UI.
  - The app logic in `app.cpp` (search `scrubTarget_`, `thumbstickRX`, `sphereMode`) reads
    `in.thumbstick*`; it must instead combine the semantic actions (and the context rules:
    stick latched by web pages `webStickLatched_`, menus scrolling, 360/180 swap).
  - Check what SteamVR's binding UI offers per input mode (joystick / D-pad / buttons / scroll /
    "touch" / gestures the user found) for vector1 vs vector2 vs boolean actions before choosing.
  - Update the floating labels (stick card text) and Controller Buttons pages for the new
    actions; mention renamed / split actions in the release notes.
- [ ] **C3. Controls Stage 2** — see Priority above.
- [ ] **C4. On-screen controls rethink** — parked by the user; music dock prev / next track.
- Known limitation: label cards (quad layers at ~0.2–1.5 m) and 3D / 360 content are at
  different depths, so eyes refocus; mitigated by near-opaque cards over 3D (user OK with it).
- Gesture bindings (SteamVR "hold and rotate" etc.) did not work for the user yet; untested by us.

- [x] **N1. Play from the Downloads list ignores multi-screen:** FIXED 2026-11-07, user-verified: the Downloads row and "Open Share/Bookmark" go through `playFromBrowser` (picker / VR question in the Files hub; the click is consumed so the picker cannot take it). Original report: pressing Play on a
  finished item in the Downloads window plays it immediately on the current screen. It
  must behave like opening a file from the file browser: with Multiple Screens on and
  screens open, show the screen picker (and 180/360 media replace everything, as elsewhere).
  Look at the Downloads-window play action in `src/app/app.cpp` (downloads window
  buttons/rows near the `drows`/`dbtns` code ~line 3430) vs. the file-browser play path
  (`loadMedia` / screen-picker logic ~lines 1897-1910, 2286-2303).
- [x] **N2. Thumbstick input leaks to screen movement while using a Plex menu:** FIXED 2026-11-07, user-verified: two leaks — the page took the stick only above 0.2 while the screen push starts at 0.15, and a frame where the hit test slipped off the screen pushed it. Now the page always takes stick Y, latched while held and for 400 ms after the laser leaves (`webStickLatched_` / `webStickUntil_`). Original report: with
  Plex's "…" submenu open on a browser screen, moving the stick up/down repeatedly
  sometimes nudges the screen Plex is on by a tiny amount, as if the laser were beside the
  screen (move/zoom-screen stick handling). Stick input while the laser is on a web
  screen should go only to the page (scroll). Suspects: the frame(s) where the hit test
  flickers off the screen edge / onto the menu's transparent area, or stick deadzone/
  hysteresis in the screen-move code — check `webMouseSlot_` / pointer-on-screen gating
  in `src/app/app.cpp` and `screensPointable` in `src/gfx/vulkan_renderer.cpp` ~2231.
  Fix should latch "stick belongs to the page" while the pointer is on (or just left)
  the web screen.

---

## Next major features (planned 2026-11-01, after v1.8.0)

Order: **Step 1 = Multi-screen**, **Step 2 = YouTube** (embedded mobile-site UI + yt-dlp
playback + login/Premium), then other streaming sources.

**Decisions (2026-11-01):**
- Nothing is reused from StreamXR; everything is our own implementation (no licensing to track).
- YouTube: show the real **YouTube mobile site** (m.youtube.com) in an embedded browser panel
  for browsing, search and login; when the user opens a video, intercept the watch URL and
  play it in our own player via yt-dlp instead of in the browser.
- Login is part of Step 2: Premium users get no ads and the higher-bitrate 1080p formats.
- Multi-screen: one shared control dock for the selected screen.

### Facts gathered (2026-11-01, on the Steam Frame)
- Hardware decoder `qcom-iris-decoder` (`/dev/video22`) accepts **H.264, HEVC and VP9** only
  (no AV1 in hardware). FFmpeg exposes `h264_v4l2m2m`, `hevc_v4l2m2m`, `vp9_v4l2m2m`;
  `MediaEngine` already picks them (`media_engine.cpp`, codec→decoder map).
- AV1 only in software (`libdav1d`) — fine for ≤1080p, too slow for 4K+.
- `MediaEngine` has **no global state** and opens its own PulseAudio stream per instance,
  so several engines can run in one process.
- Coupling to break for multi-screen: `App` has a single `media_` (≈60 call sites) and
  `VulkanRenderer` has exactly one set of video resources (NV12/dma-buf images, YCbCr
  conversion + pipelines, resolve/mip target, screen pose, projection/stereo, ambilight).
- `python3` 3.12 and `curl` exist on the device; `yt-dlp` is **not** installed.
- StreamXR (a Chrome extension built on WebXR) was looked at for reference only; we do not
  reuse any of its code or assets.

### M. Multi-screen playback (up to 3 screens)
Scope: flat 2D and every 3D format (SBS / half-SBS / TB) on flat or curved screens.
180°/360° are immersive and always play alone.

- [x] **M0. Settings & defaults**
  - Settings › Display: **Multiple Screens** (Off / On, default Off). Off = exactly today's
    single-screen behaviour.
  - **Max screens** 2 / 3 (default 3, hard cap 3).
  - **Audio**: *Selected screen only* (default) / *Mix all screens*.
- [x] **M1. Renderer: per-screen video state**
  - Extract a `VideoScreen` struct: video images (CPU NV12 + dma-buf import), YCbCr
    conversion/pipelines, resolve image + mips, pose (pivot, yaw/pitch, distance), size,
    curvature, projection/stereo, swap eyes, ambilight.
  - `std::vector<VideoScreen>` (≤3); draw far→near; one resolve pass per screen per frame.
  - Ambilight: only for the selected screen (or off) when more than one screen is open.
  - Keep the single-screen path identical when Multiple Screens is Off.
- [x] **M2. App: per-screen engines**
  - `Screen { MediaEngine engine; ScrubPreview preview; path; muted; volume; ... }` replaces the
    single `media_`; route the ≈60 `media_` call sites through "the selected screen".
  - Resume/format memory already works per file; reuse it per screen.
  - Vault lock closes only screens playing vault files.
- [x] **M3. Selecting & controlling screens**
  - One shared transport dock controls the **selected** screen (decided: simpler than one
    dock per screen). Point at a screen + trigger to select it; selected screen gets an accent
    outline; dock shows a "Screen 1/2/3" pill and per-screen **mute** toggle.
  - Optional later: "Pause all / Play all" on the dock.
- [x] **M4. Adding & placing screens**
  - Hub: **Add Screen** (Files tab header or a "Screens" quick filter) → pick a file → screen
    spawns where you are looking, offset from existing screens.
  - **Move mode**: grip on a screen (or "Move" in its menu) → it follows the laser at its
    current distance; thumbstick Y = distance, thumbstick X = size; trigger drops it.
  - Per-screen menu (Y on a screen): Move, Resize, Curvature, Format, Mute, Close.
  - Optional: remember the layout between sessions.
- [ ] **M5. Decoder capacity / admission control** (still to do, LOW priority) — budget + "no decoder capacity" message exist; 180/360 now simply replace all screens (no picker). Remaining: calibrate the budget on device. — a first rough budget (1000 Mpx/s,
  logged as "Decoder budget: …") is in `App::decoderBudgetAllows`; still to calibrate.
  - Measure on device: how many concurrent `*_v4l2m2m` sessions open, and the aggregate
    pixel-rate budget (e.g. 3× 4K30 HEVC, 2× 4K60, 8K + 1080p…). Record results here.
  - Before opening another screen, estimate width × height × fps against the measured budget;
    if it doesn't fit, show "Not enough decoder capacity" and suggest closing a screen.
  - Opening a 180°/360° file while several screens are open: ask "Play immersive (pauses other
    screens)?".
  - Watch GPU cost: one resolve + mip pass per screen per frame at 1.5× supersampling.
- [ ] **M6. Test matrix** — 2 and 3 screens with mixed H.264/HEVC/VP9, 1080p/4K, SBS/TB;
  audio modes; vault lock; recenter (should recenter the whole arrangement around the user).

### Y. YouTube (Step 2)
> **Concept (2026-11-02, for decision):** [docs/concepts/YOUTUBE_CONCEPT.md](docs/concepts/YOUTUBE_CONCEPT.md) —
> recommends a native VR YouTube hub + sign-in-only web view instead of full m.youtube.com,
> covers live/DVR, dock controls, PO tokens/SABR, EAC/mesh VR formats and a new Y1–Y7 phase plan.

Approach: embedded browser shows m.youtube.com (browse, search, login); watch URLs are
intercepted and played natively — **yt-dlp** resolves the streams, FFmpeg plays them with
hardware VP9/H.264 in our player (works with multi-screen).

- [ ] **Y0. Legal/ToS note** — PARKED 2026-11-07 (current in-page playback is good enough; only revisit if it causes problems) — personal use; no downloading of YouTube content by default.
- [ ] **Y1. Play a YouTube URL natively** — PARKED 2026-11-07 (current in-page playback is good enough; only revisit if it causes problems)
  - Ship the standalone `yt-dlp_linux_aarch64` binary (Unlicense) via the installer into the
    app dir; self-update with `yt-dlp -U` (old versions break often).
  - `yt-dlp -J <url>` → pick VP9 or H.264 video (never AV1 above 1080p) up to the chosen
    quality + Opus/AAC audio; pass yt-dlp's `http_headers` to FFmpeg.
  - `MediaEngine`: **separate video + audio inputs** (two demuxers, one clock), or a generated
    local DASH MPD for FFmpeg's dash demuxer. Reconnect/timeouts, buffering indicator,
    seeking over HTTP.
  - Temporary "Open URL" field (in-app keyboard) for testing.
- [x] **Y2. Embedded browser panel** — DONE (kept as is). Native watch-URL intercept PARKED 2026-11-07, only if the in-page setup shows problems. PANEL DONE (headless Chromium browser screens, see W1; YouTube shortcut opens m.youtube.com and plays IN the page). NOT done: intercepting watch URLs to play natively (no yt-dlp anywhere in the code yet — Y1/Y3 still open).
  - Glass panel showing m.youtube.com with a mobile user agent; laser → touch/mouse events,
    thumbstick → scroll, in-app keyboard → text input; back / reload / home.
  - **Intercept navigation** to `youtube.com/watch?v=…`, `youtu.be/…`, `/shorts/…`,
    `/live/…`: cancel it in the browser and play the URL in our player (selected or new screen).
  - Pause/mute any page audio while our player runs.
- [ ] **Y3. Login & Premium** — PARKED 2026-11-07 (current in-page playback is good enough; only revisit if it causes problems) (browser login itself works)
  - Sign in on the mobile site inside the browser (Google login works in a normal web view).
  - Export the browser's YouTube cookies to a Netscape cookie file for `yt-dlp --cookies`
    so Premium accounts get the higher-bitrate 1080p formats (and no ads — our player never
    shows ads anyway). Store with 0600 permissions; "Sign out" clears browser + cookie file.
- [ ] **Y4. VR formats** — PARKED 2026-11-07 (current in-page playback is good enough; only revisit if it causes problems) — VR180 (equirect 180 + SBS), 360 equirect mono/TB, and YouTube's
  **EAC cubemap** projection (new sphere shader path); auto-select from FFmpeg spherical /
  stereo3d side data and yt-dlp metadata.
- [ ] **Y5. Player features** — PARKED 2026-11-07 (current in-page playback is good enough; only revisit if it causes problems) — quality picker in the HUD (warn when a quality needs software
  decode), captions (WebVTT → text overlay), chapters on the timeline, playback speed.

### S. Other streaming sources (after YouTube)
- [ ] **S1. yt-dlp generic** — PARKED 2026-11-07 (current in-page playback is good enough; only revisit if it causes problems) — the same Open URL path works for many sites yt-dlp supports
  (Vimeo, Twitch VODs, …); live streams (HLS) via FFmpeg.
- [x] **S2. Plex — DONE & user-verified 2026-11-07** (native original-quality playback only after a real play click, return to the page, slot following, timeline reports, reconnect, "Stop or Close returns to Plex" hint; Plex downloads go through the app's Downloads window with the server file name). Original plan: Plex Web runs
  in the browser screen (works, transcodes to 1080p in Chromium). Instead of a Plex client
  of our own, intercept playback like YouTube: the helper watches the page's requests
  (`Network.requestWillBeSent`) for `/library/metadata/<id>` or the `transcode/universal`
  start, takes server base + `X-Plex-Token` from the URL, asks
  `/library/metadata/<id>?X-Plex-Token=` for `Media/Part.key`, and the app opens
  `base + part.key + ?X-Plex-Token=` in MediaEngine on the same screen (original file,
  hardware decode, zero-copy, our 3D/180 detection, chapters) while pausing the page's
  player. Needs HTTP(S) in the media path: either `--enable-network` for our FFmpeg
  (gnutls is already on) or a libcurl read-ahead reader shared with WebDAV. Setting
  "Plex: play with the player" (default on). Jellyfin: same pattern afterwards.
  **Seamless return:** the browser page is kept (paused, hidden behind the video) while the
  file plays on that screen; closing the video (stop / ✕ / end of file) brings the same
  Plex page back exactly where it was, like the YouTube flow. Report the watched position
  back to Plex (`/:/timeline`) so Continue Watching stays right. The hidden page belongs
  to the screen slot, not to a position: when the user moves / resizes / re-arranges
  screens while the video plays, the page must follow (same slot, same placement), so it
  is never out of place when it comes back.
- [x] **S3. Browser bugs found with Plex (2026-11-06) — ALL FIXED** (Enter closes keyboard 69feaf3, iframe keyboard 7c419bb, laser layer 81e66ae, Plex theme = software page rendering by default 859b6f8; `PVR_BROWSER_GPU=1` restores GPU):
  - typing a URL: the keyboard stayed open after Enter although the address bar had taken
    it; Enter on the address bar should navigate and close the keyboard.
  - no keyboard for Plex's sign-in fields: they live in a cross-origin iframe
    (plex.tv/vendors) and our focus detection only runs in the main frame — attach to
    child frames too (`Target.setAutoAttach` with flatten, run the focus script there).
  - laser beam looked like it passed through the dock: the beam is in the projection
    layer, the dock is a composition layer drawn over it. Fix: draw the beam itself as a
    thin quad layer (gradient texture, billboarded from controller to hit) — SteamVR
    depth-sorts layers, so it is then occluded / visible correctly.
  - Plex rendered its light theme (white page, unreadable): headless has no system theme →
    `Emulation.setEmulatedMedia prefers-color-scheme: dark` for all pages (done).
- [x] **S4. More share protocols — DONE** (SFTP via libssh, WebDAV/https via libcurl; "SMB Shares" renamed "Network"; type chosen with 4 buttons; SMB re-verified by user). Still open: testing SFTP/WebDAV against REAL servers (only loopback-tested; release notes ask users to report). Original plan: SFTP first (libssh: `sftp_opendir`
  listing + pipelined `sftp_async_read` reader, 8–16 requests in flight; password auth;
  "SFTP" quick filter beside SMB Shares with the same connect dialog / credential store),
  then WebDAV over libcurl (PROPFIND listing, ranged GET read-ahead; shares the HTTP reader
  with S2). NFS (libnfs, host-based auth) and DLNA only if asked. Check on the headset which
  of libssh / libssh2 / libnfs / libcurl SteamOS ships.
- [x] **L1. Laser smoothing:** DONE 2026-11-07, user-verified: One Euro filter on the aim pose (`src/util/one_euro.hpp`, applied in `App::run` right after `xr_.input()`, so hit tests, beam and grabbing all use it); Settings › Controls › Laser Smoothing Off/Low/Medium/High (default Medium, `laser_smoothing`). Original note: SteamOS's own laser is steady even with a shaky
  hand; ours uses the raw aim pose. Filter the aim ray (One Euro filter on direction +
  origin: strong smoothing when slow, little lag on fast moves), applied to the ray used
  for hit testing and drawing alike; button presses keep the hit of the press frame.
- [ ] **D1. Real-time 2D → 3D (idea, 2026-11-06):** Moonlight XR
  (github.com/Gilleece/moonlight-android-xr) converts mono video to stereo on the headset:
  downscale → small depth model on the GPU on its own thread (MiDaS small ~22 ms; v0.4 uses
  "ZipDepth") → guided depth upsample → occlusion-aware warp, one view per eye → two quad
  layers. That pipeline fits ours (we already submit one layer per eye for SBS/TB).
  **Licence:** Moonlight XR is a Moonlight fork → GPLv3. Copying its code would force the
  player to GPLv3 (selling stays allowed, but the full source must be offered and the MIT
  licence would have to go). To keep selling closed/MIT: reimplement the technique from
  the description (not the code) and pick a permissive model — MiDaS v2.1 small (MIT),
  Depth Anything V2 *Small* (Apache-2.0; Base/Large are CC-BY-NC → not usable); check
  ZipDepth's licence before using it. Inference runtime on the Frame: Vulkan compute (ncnn,
  BSD-3) or the Adreno NPU if exposed. Prototype on 1080p flat first.
- [x] **R1. Room colour:** DONE 2026-11-07, user-verified: Settings › Display › Room Midnight/Black/Slate/Warm (`room`); the screen glow stays on in Black (user wish: it looks best on black). Environment scenes not done. Original note: user-selectable room backdrop —
  presets (current navy, pure black "cinema" with no distractions, a few tinted / themed
  rooms), later optional environment scenes. Applies to the projection-layer clear colour
  and the floor/ambient; pure black must also drop the ambilight glow when chosen.
- [x] **K1. Keyboard closer to the Steam Frame default:** DONE 2026-11-07, user-verified, modelled on the SteamOS/PC layout (number row + Backspace, Enter on the home row, two Shifts, bottom row ?123 / @ / Space / arrows); compare with the real Frame keyboard. Original note: make our in-app
  keyboard's key arrangement, sizes and special keys (shift, symbols, backspace, enter,
  space) match the SteamOS/Steam Frame on-screen keyboard so it feels familiar.
- [x] **K2. More keyboard layouts:** DONE 2026-11-07, user-verified: US, Deutsch, UK, Français (AZERTY), Nordic (SE/FI, æ ø on the symbols page); `keyboard_layout` 0..4 keeps old values. Original note: add a layout choice (e.g. QWERTY US/UK,
  QWERTZ German, AZERTY French, Nordic) with the matching umlauts/accents; remember the
  choice in settings.conf.
- DRM services: Prime Video verified working in the browser screen (Widevine L3, 1080p);
  Netflix / Disney+ use the same path.

### New items 2026-11-08 — research and work in their own worktrees / branches
All of these live **outside `main`** until merged. Each branch has its doc under `docs/concepts/`.
On the PC the worktrees sit next to the main checkout (`C:\Projects\PixelVRPlayer-*`); the
branches are local (not pushed) unless noted.

- [~] **P1. Plugin system + first-party plugins:** BUILT, not yet merged or tried in the headset.
  - Branch `feature/plugin-system`, worktree `C:\Projects\PixelVRPlayer-plugins`.
  - Spec, decisions and **progress log**: `docs/concepts/PLUGIN_SYSTEM_SPEC.md` (start here).
  - Out-of-process JSON-lines plugins, Settings › Controls & Input › Plugins, vault opt-in per
    plugin, `.pvrplugin` install from the file browser, plugins as separate release downloads.
  - Plugins:
    - `plugins/funscript` (Intiface/Buttplug v3; 25/25 against `tools/fake_intiface.py`).
    - `plugins/deovr-remote` (DeoVR-compatible sync for MultiFunPlayer & co, 11/11).
    - `plugins/example`.
  - Headset builds of this branch go to `~/pixelvr-src-plugins` (never `~/pixelvr-src`).
  - Touches `app.cpp`, `settings_menu.*`, `vulkan_renderer.*`, `browser.*`, `release.yml`, so
    expect merge work with newer `main`.
- [ ] **H1. Interactive devices (funscript / Buttplug / Lovense / The Handy) research:** DONE as
  research. Branch `research/steam-cloud-workshop-haptics`, worktree
  `C:\Projects\PixelVRPlayer-research`, `docs/concepts/HAPTICS_FUNSCRIPT_RESEARCH.md`. Implemented
  as plugins in P1. Open: multi-axis scripts, The Handy API, TCode, a real-hardware test by a user.
- [ ] **SC1. Steam Cloud + Workshop:** researched, not started. Same branch / worktree as H1,
  `docs/concepts/STEAM_CLOUD_WORKSHOP_RESEARCH.md`.
  - Auto-Cloud on a dedicated sync folder (never passwords, vault or browser profile).
  - Workshop: "content packs" first (rooms, arrangements, lens profiles, keyboard layouts,
    themes, LUTs …).
- [x] **J1. Jellyfin like Plex:** DONE, released in **v1.15.0** (user-verified 2026-11-08).
  - Covers: playback hand-off, audio track, progress, downloads, the shortcut with its address, the
    next / previous episode, and the episode title on the screen.
  - Implementation and every test: `docs/concepts/JELLYFIN_PLAN.md`.
  - Original note (decided: approach A): Jellyfin Web in a browser screen, play
  is handed to our player (Direct Play URL from our own `PlaybackInfo` + DeviceProfile), progress
  through `/Sessions/Playing*`.
  - Reuses the Plex intercept in `browser/pvr_browser.cpp` / `App::startPlexPlay`.
  - Branch `research/jellyfin-autofocus-passthrough`, worktree `C:\Projects\PixelVRPlayer-research2`,
    `docs/concepts/JELLYFIN_AUTOFOCUS_PASSTHROUGH_RESEARCH.md` §1 (API flows from Stereonaut,
    MIT but AI-generated and unreviewed: reference only).
  - A native Jellyfin library (approach B) is not planned.
- [ ] **V1. Manual stereo depth / convergence per title (180/360):** horizontal shift of the two
  eye pictures in the sphere shader. Same doc §2. Comes first; it is the base for V2.
- [ ] **V2. Eye-tracked stereo "autofocus" (experimental, opt-in):** automatic convergence for
  close subjects.
  - Gaze from `XR_EXT_eye_gaze_interaction` (v2, verified on the headset), GPU disparity
    measurement, threshold / hysteresis / fade.
  - Same doc §2. Needs in-headset tuning by the user. Gaze is never exposed to plugins.
- [ ] **PT0. Check passthrough support first:** extend `pixelvr --xrext` to print
  `xrEnumerateEnvironmentBlendModes` (instance + system, no session). Every PT item needs
  `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND`, which is not verified by us. Same doc §3.
- [ ] **PT1. Real-room preset:** a room option showing the camera view around the screens / hub,
  with a dimming slider.
- [ ] **PT2. Quick peek:** a button combo that shows the room for a moment.
- [ ] **PT3. Passthrough videos:** per-title chroma key (green / blue / picked colour) so
  green-screen subjects stand in the room. Same doc §3 for PT1–PT3.
- [ ] **D1 research (see D1 above):** how Moonlight XR does 2D→3D, and which depth model runs fast
  enough on the Frame. Branch `research/2d-to-3d`, worktree `C:\Projects\PixelVRPlayer-2d3d`,
  `docs/concepts/2D_TO_3D_RESEARCH.md` (findings and headset benchmark numbers).
  - First result (2026-11-08, ncnn, inference only): **ZipDepth** (MIT, Moonlight XR's default)
    takes ~14 ms at 512x288 on the GPU and about the same on 4 CPU threads (9 ms at 256²).
    MiDaS small takes 22 ms GPU. Depth Anything V2 Small is unusable (279 ms CPU).
  - Still open: real-weight output check, measurements under playback load / sustained, the warp
    pipeline. Benchmark files are in `~/depth-bench` on the headset.
- [ ] **PX1. Skip intro / skip credits in our player (Plex, later Jellyfin) (asked for 2026-11-08):**
  investigate taking Plex's intro and credits markers into the native player and showing "Skip intro"
  / "Skip credits" (and starting the next episode at the credits).
  - Plex: `GET /library/metadata/{id}?includeMarkers=1` lists `Marker` entries
    (type intro / credits, startTimeOffset / endTimeOffset in ms) when the server has analysed them.
  - Jellyfin: Media Segments (`GET /MediaSegments/{itemId}`, Intro / Outro / Recap…, 10.10+), filled by
    plugins such as Intro Skipper.
- [~] **TV1. Series controls (asked for 2026-11-08):** PARTLY DONE in v1.15.0 (previous / next arrows on the screen, episode title on the screen); the rest is still open. rework the player UI / controls for TV series
  (Plex, Jellyfin): previous / next episode from the player.
  - Idea: an arrow at the left and right edge of the screen while the laser aims at the screen.
  - Also possible: dock buttons, or a stick gesture.
  - Builds on the next-episode hand-off (`plexnext`; a previous one needs `plexprev`).
  - The episode title card (series · S1 E2 · name, 5 s) exists since 2026-11-08 and could become a
    proper overlay.
  - See also PX1 (skip intro / credits).
- [ ] **ST1. Subtitle picker (asked for 2026-11-08):** we lack one for Plex and Jellyfin, and in general.
  - Pick embedded subtitle tracks in the player, plus "off".
  - Take the subtitle chosen on the Plex / Jellyfin page into our player (Plex: the part's selected
    stream; Jellyfin: `SubtitleStreamIndex` in the PlaybackInfo request).
  - Plex / Jellyfin external subtitles come as separate URLs.
- [ ] **SD1. SD cards and a free download target (asked for 2026-11-08):** users of the 256 GB Steam
  Frame need the SD card's space.
  - Check that the player works with SD cards: file browser, playback, thumbnails, the vault, removing
    the card while it is in use.
  - Offer the SD card as the download folder, maybe as the default when one is present.
  - Let the user pick any download folder, not only the three fixed choices
    (`kDownloadDirs` = ~/media, ~/Downloads, ~/Videos in `src/ui/settings_menu.cpp`).
  - Facts (2026-11-08, the test headset has no card in it): SteamOS mounts cards under `/run/media/…`.
    `/run/media/sdcard` (with Steam's `libraryfolder.vdf` + `steamapps`) and `/run/media/steamos/sdcard`
    exist; `lsblk` shows no `mmcblk` device, so they are leftovers. Detect a card by its mount
    (`/proc/mounts`), not the folder. Free space and "card removed" must be shown in the Downloads window.

### W. Browser engine (needed by Y2)
- [x] **W1. DECIDED/DONE: headless system Chromium driven over CDP** (`browser/pvr_browser.cpp`, helper compiled into `pixelvr`; Widevine DRM works: Prime/Netflix/Disney+). Original options were: **WPE WebKit** (fdo backend exports dma-buf frames → zero-copy
  Vulkan import, lighter) vs **CEF** offscreen rendering (Chromium, heavy, CPU copy). Check
  aarch64 SteamOS availability/packaging, memory, Google login support, touch/mouse/key input
  injection and navigation interception hooks.
- [x] **W2. General browsing — DONE** (toolbar with URL bar, back/forward/reload, tabs, popups as tabs, downloads, configurable shortcut rail incl. Plex; B never goes back to the initial about:blank).

---

## Active backlog (2026-10-20 — Completed)

- [x] **Virtual Keyboard Tilt Inversion, Raycast Auto-Focus & UI Hide Refinements**
  - **Symptom 1:** Keyboard spawned tilted with the top edge pitched forward (slanted away from user).
  - **Resolution 1:** Corrected tilt angle to \-0.38f\ (-21.8 degrees). The top edge slants backward away from user; face normal tilts upward directly toward the user's eyes.
  - **Symptom 2:** Laser beam shot 2.5m past the keyboard with no cursor dot on the keys; laser wasn't focused.
  - **Resolution 2:** Added keyboard plane collision test in \VulkanRenderer::updatePointer()\ using \KeyboardModalData::transform\. Laser ray terminates directly on the keyboard surface with glowing cursor dot on hovered keys. Also ensured active controller laser turns on automatically when keyboard opens, and guarded background menus from eating keyboard trigger clicks.
  - **Symptom 3:** Trigger pull while HUD dismissed didn't reactivate the UI unless hitting a hidden button; empty-space clicks re-dismissed the UI in the same frame.
  - **Resolution 3:** Recorded \wasHudDismissed\ at frame start and guarded dock interactions with \!wasHudDismissed\. Trigger click cleanly un-hides the HUD without clicking hidden buttons or re-dismissing in empty space.
  - **Symptom 4:** Analog grip squeeze was overly sensitive and accidentally reactivated the dismissed UI.
  - **Resolution 4:** Ignored soft analog grip (\in.gripDown\ and \BtnGrip\); only digital full grip click stop (\in.gripFullPressDown\) un-hides the HUD.

## Active backlog (2026-10-19 — Completed)

- [x] **1. Fix Virtual Keyboard Laser Pointer Focus & Tracking**
  - **Symptom:** Virtual keyboard opened, but laser pointer disappeared or wasn't focused on the keyboard; user had to squeeze/press the controller grip fully to activate it. Also, the keyboard centered where the Steam menu was centered when the app launched instead of where the user centered their view.
  - **Root Cause:**
    - SteamVR's virtual keyboard overlay key is `VRKeyboard_uid0` (from CEF `webhelper`), not `valve.steam.gamepadui.keyboard`. Previous lookups failed, so `MakeOverlaysInteractiveIfVisible` and `VROverlayInputMethod_Mouse` were never applied to the actual keyboard overlay.
    - `VulkanRenderer::updatePointer()` was returning early (`if (keyboardModal_.visible) return;`), which prevented raycasts to the modal plane and left `pointerActive_` false with no cursor dot.
    - `SetKeyboardTransformAbsolute` was never called with the user's current centered view orientation/position, so SteamVR defaulted to the launch standing origin.
  - **Resolution:**
    - Implemented dynamic overlay discovery across handles 1..128 and known keys (`VRKeyboard*`, `valve.steam.gamepadui.keyboard`, `system.keyboard`).
    - Configured `MakeOverlaysInteractiveIfVisible`, `VROverlayInputMethod_Mouse`, and `SendVRDiscreteScrollEvents` on the keyboard overlay.
    - Calculated world matrix in `TrackingUniverseStanding` directly from the user's HMD pose (or OpenXR `headPos` and `headForward`): placed 1.05m in front of eyes, 0.38m below eye level, tilted 28° up toward the eyes.
    - Applied transform via `SetKeyboardTransformAbsolute(vr::TrackingUniverseStanding, &kbMat)` and `SetOverlayTransformAbsolute`.
    - Updated `VulkanRenderer::updatePointer()` to raycast against the modal plane when `keyboardModal_.visible` is active, ensuring sharp laser pointer focus and a visible cursor dot.
    - Guarded `VrKeyboard::shutdown()` to prevent crash on exit.

- [x] **2. Controller Movement UI Dismissal Fix**
  - **Symptom:** When the user clicked "Hide UI" or clicked outside controls, any hand movement immediately reactivated the UI.
  - **Requirement:** Once dismissed, the UI must stay hidden regardless of controller movement/rotation, and only reactivate when the user explicitly presses a physical button on the controller.
  - **Resolution:**
    - In `App::run()`, when `hudDismissed_` is true, controller movement (`moveDist`) and rotation (`rotDiff`) are blocked from clearing `hudDismissed_` or setting `activity = true`.
    - Only physical button presses (`in.togglePlay`, `in.browserToggle`, `in.recenter`, `in.buttonDown[...]`, `in.gripDown`, `in.gripFullPressDown`, `in.select` outside cooldown, or thumbstick movement) reactivate the HUD.

- [x] **2. Strict IPv4 Validation & Live Sanitization**
  - **Symptom:** Host field accepted invalid characters, spaces, and did not strip leading zeros.
  - **Resolution:**
    - Created `src/util/ipv4.hpp` with `sanitizeIpv4()` and `isValidIpv4()`.
    - Live typing sanitization: automatically strips non-digits/dots, discards duplicate dots, clamps octets to 255, drops leading zeros in each octet (e.g. `001` -> `1`, `010` -> `10`, `00` -> `0`), limits to 4 octets, and preserves trailing dots while typing.
    - Added automated unit test suite `tests/test_ipv4.cpp` and verified all test cases pass on the device.
    - Verified strict `isValidIpv4()` validation before saving an SMB share in `saveSmbModal()`.

- [x] **3. Fix `???` Glyphs (Unicode Star Elimination)**
  - **Symptom:** Sidebar and header button displayed `???` instead of the star icon.
  - **Root Cause:** FreeType SDF text atlas was initialized only for ASCII codepoints 32..127. UTF-8 multi-byte sequence `★` (`\xe2\x98\x85`) produced 3 replacement `?` characters.
  - **Resolution:** Replaced all UTF-8 star glyphs with clean ASCII strings: `"Bookmarks"`, `"[ Bookmarked ]"`, and `"[ + Bookmark ]"`. Adjusted header breadcrumb max width to `0.46f` to prevent collision with the header button.

- [x] **4. Rename Quick Path to Bookmarks & Implement Multi-Bookmark Overview (`bookmarks://`)**
  - **Requirement:** Rename Quick Path to Bookmarks, and have the Bookmarks sidebar tab open an overview showing all bookmarked folders and files in regular browser views.
  - **Resolution:**
    - Added `Bookmark` struct and multi-bookmark storage in `Browser` with persistent JSON serialization to `~/.config/pixelvr/bookmarks.json`.
    - Implemented `bookmarks://` virtual directory navigation displaying all bookmarked paths with full cards (BigCards, MiniCards) and CompactList support, thumbnails, and direct activation.
    - Sidebar subtab 4 navigates to `bookmarks://` with proper active tab highlighting.
    - Header button toggles bookmark for the current open directory (`[ + Bookmark ]` / `[ Bookmarked ]`).
    - Action Menu (Y button) provides `"Add to Bookmarks"` / `"Remove from Bookmarks"` / `"Go to Bookmarks"`, and in `bookmarks://` provides `"Open Bookmark"` / `"Remove Bookmark"`.
    - Updated Settings Menu shortcut label to `"Bookmarks Shortcut"`.

- [x] **5. Seamless 1-Click Update System & Streamlined Distribution**
  - **Requirement:** Provide easy, friction-free updating so users do not need to memorize terminal commands or worry about losing their settings.
  - **Resolution:**
    - Added standalone local updater `tools/update.sh` and `--update` flag to `tools/install.sh`.
    - Added KDE Plasma `Update Pixel VR Player.desktop` desktop shortcut deployed automatically to `~/Desktop/`.
    - Added remote Wi-Fi 1-click updaters for PC users (`tools/update-from-pc.bat` for Windows and `tools/update-from-pc.sh` for macOS/Linux).
    - Updated `tools/package-release.sh` to package updater scripts in the release tarball.
    - Updated `README.md` with comprehensive update instructions across all platforms.

- [x] **6. Fix UI Dismissal Flicker and Recessed Island Alignment**
  - **Symptom:** Clicking outside the controls or clicking the Hide UI button caused the UI to briefly flicker for 1 frame and immediately reappear. The Hide UI button was also visually misaligned (half outside the dark island).
  - **Root Cause:**
    - `in.selectHeld` and trigger activity handling in `App::tick()` continuously reset `hudDismissed_ = false` and set `activity = true` on subsequent frames of the trigger pull, fighting the dismissal call.
    - Controller activity anchors were not reset on dismissal, and there was no dismissal grace cooldown, causing slight hand tremor during trigger release to immediately re-trigger movement thresholds.
    - The center transport recessed island background in `VulkanRenderer` was rendered at `cx = -0.05f, hw = 0.30f`, clipping through the center of the Hide UI button at `+0.26f`.
  - **Resolution:**
    - Created `App::dismissHud()` which sets `hudDismissed_ = true`, records `hudDismissTime_`, zeroes out activity, hides the control bar and laser ray, and snaps controller activity anchors to their exact current positions.
    - Added a 500ms dismissal cooldown preventing accidental re-engagement from trigger release tremors.
    - Blocked `in.select` and `in.selectHeld` from clearing `hudDismissed_` while the HUD is dismissed.
    - Re-centered the transport recessed island background to `cx = 0.0f, hw = 0.34f`, creating perfect 34mm symmetrical padding for all 5 buttons (`Stop`, `SeekBack`, `PlayPause`, `SeekFwd`, `HideHud`).
    - Added hover tooltip pills for `[Hide UI]` and `[Stop]`.


---

## Carried over & Completed

- [ ] A. Full-pitch recenter (usable lying down) — reorient screen + controls + 360/180 to full head orientation, not just horizon yaw.
- [x] B. Real controller models — DONE (see F1).
- [x] C. Dynamic controller button labels from the user's bindings — DONE in v1.14.0 (floating
  labels follow the live SteamVR bindings; Settings › Controls & Input › Controller Buttons).

Completed (2026-10-18):
- [x] I. Laser auto-release when pressing Steam button (unfocused session gates controller pointer).
- [x] J. Native SteamVR / SteamOS Virtual Keyboard integration (OpenVR GamepadTextInput via dlopen).
- [x] K. Protected SMB credentials manager: hardware-keyed AES-GCM encryption, strict permissions, full CRUD.
- [x] L. Settings > Network & SMB Shares: manual IP/hostname input, authentication settings, auto-listed in Network browser.
- [x] M. UI Follow Mode: Mode 0 (Rock-solid World Lock default, zero drift/tilt) and Mode 1 (Smooth Follow).
- [x] N. Player UI dismissal: `[ ✕ Hide ]` button, empty space click-outside dismissal, B/Y toggle, and micro-tremor filtering (>0.06m threshold).
- [x] O. Quick Path Whirligig-style bookmarking header button + R3 controller shortcut button.



---

## 1. Build / deploy / test (the loop that works)

**Current loop (2026-11-08, used for v1.13–v1.14):** from Git Bash on Windows, no WSL.
- Key: `~/AppData/Local/steamos-devkit/steamos-devkit/devkit_rsa`, host `steamos@192.168.178.61`.
- Source on the headset: `~/pixelvr-src` (keep it at `main` / your branch HEAD), build dir
  `build-embed` (our embedded FFmpeg). Install = copy `build-embed/bin/pixelvr` and
  `build-embed/shaders/*.spv` into `~/PixelVRPlayer` (the real install, not `~/devkit-game`).
- `tools/headset-build.sh [deploy]` does exactly that: it streams the files that differ from
  HEAD (git status) under src/browser/shaders/cmake/tests/third_party to `~/pixelvr-src`,
  builds, and with `deploy` installs. It assumes `~/pixelvr-src` already matches HEAD; after a
  commit / branch switch, sync the whole tree first.
- Feature branches in other worktrees build in their own dir (P1: `~/pixelvr-src-plugins`),
  never in `~/pixelvr-src`.
- The user starts the app from the headset; don't launch it over SSH without a reason. Log:
  `~/pixelvr-run.log` (installer: `~/pixelvr-install.log`); a `PERF` line every few seconds
  (eye size, GPU ms per part, MSAA, mask). Env switches: `PIXELVR_FIXED_RES=1`,
  `PIXELVR_SPHERE_MSAA=1`, `PIXELVR_VISMASK=0`.
- Releases: the version comes from the tag (`PIXELVR_VERSION` in CI); write `docs/release-notes/vX.Y.Z.md`, fast-forward `main`, push
  tag `vX.Y.Z` → `.github/workflows/release.yml` builds and attaches
  `pixelvr-linux-arm64.tar.gz(.sha256)`. Test with `tools/install.sh --update --no-steam` into a
  temp dir on the headset. `gh` is not installed on the PC; use the GitHub REST API via curl.

**Older loop (WSL, `~/devkit-game`), kept for reference — paths below are outdated.**

**Build NATIVELY ON THE DEVICE** (cross-compiling risks a libstdc++/FFmpeg SONAME
mismatch — device is GCC 15 + FFmpeg n7.0 `libavcodec.so.61`).

- Device: `steamos@192.168.178.61` (mDNS `frame` may not resolve from WSL — use IP).
- SSH key (WSL): `$HOME/.config/steamos-devkit/devkit_rsa` (from Windows
  `%LOCALAPPDATA%\steamos-devkit\steamos-devkit\devkit_rsa`).
- All commands run from Windows via `wsl -d Ubuntu -- bash -lc '...'`.

**Deploy one-liner** (rsync → build → deploy):
```bash
wsl -d Ubuntu -- bash -lc 'rsync -az -e "ssh -i $HOME/.config/steamos-devkit/devkit_rsa -o BatchMode=yes" \
  --exclude "build*" --exclude dist --exclude sysroot --exclude .git --exclude .vscode \
  "/mnt/c/Projects/Pixel VR Player/" steamos@192.168.178.61:~/pixelvr-src/ && \
  ssh -i "$HOME/.config/steamos-devkit/devkit_rsa" -o BatchMode=yes steamos@192.168.178.61 \
  "cd ~/pixelvr-src && cmake --build build --parallel 2>&1 | tail -3; \
   pkill -x pixelvr.real 2>/dev/null; sleep 1; \
   cp build/bin/pixelvr ~/devkit-game/PixelVRPlayer/pixelvr.real; \
   cp build/shaders/*.spv ~/devkit-game/PixelVRPlayer/shaders/; echo DEPLOYED"'
```
(First time only: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo`.)

**Fast compile-check without the device** (WSL x86): `cmake --build ~/pixelvr-build --parallel`
(configure once: `cmake -S . -B ~/pixelvr-build -G Ninja -DPIXELVR_ENABLE_MEDIA=ON`).

**Launch (needs headset AWAKE — see gotchas):**
`ssh … "python3 ~/devkit-utils/steam-devkit-rpc run-game gameid=PixelVRPlayer"`
- `run-game` sometimes BLOCKS → run it in an async terminal, read logs with a
  separate ssh. App logs to `~/pixelvr-run.log` (wrapper sets `PIXELVR_LOG=debug`).
- `~/devkit-game/PixelVRPlayer/pixelvr` is a sh WRAPPER; real binary is
  `pixelvr.real`. Initial clip = path in `~/media/which.txt`.

**Headless diagnostics (NO headset needed):**
- `pixelvr --probe <path-or-smb-url>` → opens media, samples 5 frames, prints
  chroma `min/max/mean` (mean ~128 good, 0 = broken). Also `tools/probe_smb.sh`,
  `tools/chroma_check.py`.
- `pixelvr --xrext` → lists OpenXR runtime instance extensions.

**Gotchas (bit us repeatedly):**
- NEVER `pkill -f pixelvr` from the ssh cmd — the pattern matches the ssh
  session's own args and kills it (no output, exit 1). Use `pkill -x pixelvr.real`.
- Triple-nested quoting (WSL `'…'` > ssh `"…"` > python/labels) breaks. Put
  scripts in a file and `scp` them (see `tools/chroma_check.py`).
- Intermediate shell vars in `bash -lc` don't expand reliably — INLINE
  `"$HOME/.config/…"` and `steamos@192.168.178.61` every time.
- The run_in_terminal tool rewrites `&&`/`||` to `;`.
- **Video frames only decode with the headset WORN** — remote launches with the
  HMD asleep reach "Session running" but `process_video` never runs. Use `--probe`
  for headless decode testing.
- OpenXR `apiVersion` MUST be `XR_MAKE_VERSION(1,0,0)` (SteamVR rejects 1.1 with
  XrResult(-4)).

---

## 2. DONE (deployed & verified)

- VR playback: HW decode (`h264_v4l2m2m` → NV12), PulseAudio audio, MSAA 4x.
- **GREEN BUG FIXED** — libswscale drops chroma on NV12→NV12 on this platform.
  Fix in `media_engine.cpp process_video`: when `srcFmt==AV_PIX_FMT_NV12`, repack
  planes with a direct `memcpy` honoring `linesize` (skip sws; sws only for real
  conversions like SW yuv420p). Verified via `--probe` (chroma 0/255/mean126).
- **SMB streaming by default** — `media_engine` opens `smb://` via a custom
  `AVIOContext` backed by libsmbclient (`SmbIO`, guest auth). Browser `activate()`
  streams `smb://` directly; `downloadFocused()` (A) is the explicit download.
  Verified headless (streamed a 15 GB file off the NAS).
- In-VR file browser (FreeType text, local + SMB scan + avahi/mDNS discovery).
- Controls: superseded by v1.14.0 (see "Controls (after v1.14.0)" at the top and the README
  Controls table).
- Transport HUD **world-locked** below the flat video (was head-locked), auto-hide.
- **Tracked controllers** — OpenXR aim-pose actions + per-hand action spaces
  (`xr_context.cpp`); `XrInputState.controllers[2]`. Laser pointer + cursor +
  (basic) controller stub in `vulkan_renderer.cpp`. Pointer raycasts the browser
  panel → hover row; Trigger selects. Math helpers added in `math.hpp`.
- **Media control bar** — point-and-click `<< · play/pause · >> · Files` below the
  flat video; always-visible laser (2.5 m, snaps to UI hit). Files button opens the
  browser via the pointer (no reliance on face-button bindings).

---

## 3. TODO — BUGS (from latest in-headset testing)

### B1. Video fringing / white-pixel aliasing when the screen is farther away — DONE (video mip chain; user-verified 2026-11-07)
- **Symptom:** high-res video shows white-pixel fringing around objects when the
  screen is a bit further; gone when moved much closer.
- **Cause:** the NV12 video textures have NO mipmaps (`mipLevels=1`) and the sampler
  is linear-only → minification aliasing when the plane is small on screen.
- **Fix:** create luma/chroma images with a full mip chain; generate mipmaps after
  each upload (`vkCmdBlitImage` down the chain in `uploadCmd_` — R8/R8G8 support
  LINEAR blit on Turnip); sampler `mipmapMode=LINEAR`, `maxLod=mipCount`, and enable
  anisotropy (`samplerAnisotropy` feature + `maxAnisotropy`). Files:
  `vulkan_renderer.cpp` `createVideoResources` (mipLevels + image views),
  `updateVideoTexture` (blit mip gen), `createSamplerAndDescriptors` (sampler).
  Watch the per-frame blit cost; only regenerate when a new frame is uploaded.

### B2. High-res 360/180 video won't play + stale frame + controls disappear — DONE (user-verified 2026-11-07; 8K immersive closes other screens + HW decoder retry, d714ea8)
- **Symptom:** a ~3600p video (meant to be 180°/360°) shows the LAST FRAME of the
  previous test clip and never plays; and the control bar/transport vanish (they
  only render for `ProjectionMode::Flat`).
- **Likely causes / actions:**
  1. Decode fails at that resolution (HW decoder session limits, or HEVC — see K1).
     Run `pixelvr --probe <file>` to see codec/res and whether frames decode.
     Probably need a **software-decode fallback** for too-high-res / HEVC, or reject
     with a message. (Iris HW decode is fragile ≥6K and floods POLLERR on 4K HEVC).
  2. **Stale frame on failure:** when `media_.open()` fails, the renderer keeps the
     last uploaded frame (`hasVideo_` stays true). `app.loadMedia` must detect the
     failure, clear the video (add a `renderer_.clearVideo()`), and show an error.
  3. **Controls in 360/180 (DONE):** Added floating, gaze-anchored control panel & transport
     HUD for 360/180 at distance 1.6 m, tilted towards eye level, fully raycastable by the
     laser pointer with clickable transport buttons.

### B3. SMB browser scrolling is buggy (feedback loop) — DONE
- **Fixed:** decoupled pointer hover from selection and scroll offset. Added `browserHovered_`
  for laser aiming and `browserFirst_` for stable windowing. Clamped pointer row mapping to the
  currently-visible window. Added a sleek, visible scrollbar track and thumb indicator on the
  right side of the panel.

---

## 4. TODO — FEATURES

### F1. Real controller models (`XR_EXT_render_model`) — DONE (RenderModelSystem + `renderer_.loadControllerModel`, animated nodes in `app.cpp`); plan kept for reference
Runtime supports `XR_EXT_render_model` + `XR_EXT_interaction_render_model` (verified
via `--xrext`). Our pinned OpenXR SDK **1.1.43** only declares the older
`XR_FB_render_model` (which this runtime does NOT support). Phased plan:
1. **Bump OpenXR SDK** — `CMakeLists.txt` `PIXELVR_OPENXR_TAG` (currently
   `release-1.1.43`) → a release that declares `XR_EXT_render_model` (verify by
   grepping the fetched `openxr.h` for `xrCreateRenderModelEXT`). Or hand-declare it.
2. **Add a DEPTH buffer** to the render pass (3D models must self-occlude; current
   UI is painter-ordered, no depth). Add depth image/attachment + clear; give the
   model pipeline depth test+write, leave the flat UI pipelines depth-test-off.
3. **Lit model pipeline** — pos+normal vertex input + N·L shader (`model.vert/frag`),
   register in `cmake/CompileShaders` + `CMakeLists` shader list.
4. **Load models** — enable exts; enumerate per-controller interaction render models;
   fetch the glTF/GLB asset; parse with **cgltf** (single-header) → positions/normals/
   indices (bake node transforms); render at the grip pose each frame. Skip textures
   first (flat gray + lighting). Big + blind → iterate in-headset.
- Also consider adding the native `/interaction_profiles/valve/frame_controller`
  bindings (runtime lists `XR_VALVE_frame_controller_interaction`) for correct
  Frame button mapping — but a single wrong component path fails the WHOLE profile
  suggestion (graceful fallback), so get all paths right or skip.

### F2. Better button / UI graphics — DONE
- Replaced text labels (`<<`, `II`, `>>`, `Files`) with GPU-procedural Signed Distance Field
  (SDF) vector icons rendered in `shaders/ui.frag` (Rewind, Play, Pause, Fast-Forward,
  Folder/Files, Settings Gear, Recenter Target, Reticle Cursor).
- Upgraded button styling to rounded glass capsules with antialiased borders and glowing hover states.
- Transport HUD redesigned as a rounded pill progress track with glowing fill and thumb position dot.
- Laser pointer cursor upgraded from a flat quad to a high-tech circular reticle.

### F3. Controller remap settings UI — DONE, then REMOVED in v1.14.0 (buttons are assigned in SteamVR only; the Controls pages are read-only)
- Implemented `SettingsMenu` model (`src/ui/settings_menu.{hpp,cpp}`) and pointer-driven in-VR
  Settings panel overlay.
- Users can view and cycle button bindings (Play/Pause, Menu, Select, Recenter) and player
  options (Projection Mode, Stereo 3D Mode, Screen Distance, Reset View).
- Settings automatically persist to `~/.config/pixelvr/settings.conf` across restarts.

### F4. Variable-speed joystick fast-forward/rewind — DONE in v1.14.0 as scrubbing (user-verified)
- Right stick X (left stick X in 360/180) scrubs at `t² × 60 s/s`; the bar and preview follow and
  ONE exact seek commits on release (`app.cpp`, `scrubTarget_`; exact seek = decode-and-discard to
  the target in `media_engine`). Left stick X steps 30 s. Separate Steam Input actions: C2.
- Original plan, kept for reference:
- First EVALUATE feasibility across all sources (local, SMB/SFTP/WebDAV, Plex https, browser
  pages) — continuous seeking over network sources may thrash. Flat video only: in 180/360
  the right stick already tilts/zooms the view.
- Replace the discrete ±10 s edge-triggered seek (thumbstick X) with CONTINUOUS seek
  whose rate scales with deflection: `rate = sign(x) * (deadzone(|x|))^2 * maxRate`,
  applied per-frame (`* dt`). Show the seek speed on the HUD. Files: `app.cpp` (per-
  frame `dt`, continuous seek), `media_engine` (`seekRelative` exists; may want a
  scrub that doesn't thrash the decoder — coalesce seeks).

---

## 5. Known issues / notes

- **K1. HEVC 4K HW decode is broken** — `hevc_v4l2m2m` floods `output POLLERR` +
  STREAMOFF on 2160p HEVC (e.g. the NAS "…2160p HEVC….mkv"); only a few frames
  decode. Many NAS movies are 4K HEVC. Needs SW fallback for HEVC/4K (overlaps B2).
- **Sharpness** — partly the mipmap issue (B1); 1080p on a big VR screen is inherently
  soft (magnification). Re-judge after B1.
- The `pixelvr` deploy target is still a **wrapper** (`pixelvr` → `pixelvr.real`,
  reads `~/media/which.txt`). Restore a plain binary for shipping.
- Temporary diagnostics: `--probe` / `--xrext` modes in `main.cpp` (harmless, keep or
  remove later). The green-bug per-frame diagnostics were already removed from
  `media_engine.cpp`.

---

## 6. Source map (where things live)

- `src/xr/xr_context.{hpp,cpp}` — OpenXR instance/session/actions; `XrInputState`
  (buttons + `controllers[2]` aim poses); aim-pose action + action spaces; renderFrame.
- `src/gfx/vulkan_renderer.{hpp,cpp}` — all Vulkan. NV12 pipeline, MSAA, pipelines
  (quad/sphere/ui/text), transport HUD, control bar, browser panel, controller lasers/
  cursor, `updatePointer` raycast, screen placement (`screenPivot_/Forward_/Yaw_/Distance_`).
- `src/media/media_engine.{hpp,cpp}` — FFmpeg PIMPL; HW/SW decode; `process_video`
  (NV12 direct-copy fix); SMB AVIO streaming; pause/seek/position; PulseAudio.
- `src/ui/browser.{hpp,cpp}` — file browser model (local + SMB + mDNS, async download).
- `src/app/app.{hpp,cpp}` — run loop, input handling, browser + control-bar wiring.
- `src/main.cpp` — entry + `--probe` / `--xrext` diag modes.
- `src/util/math.hpp` — Mat4/Quat/Vec3 + ray/billboard helpers.
- `shaders/` — quad (NV12 YUV→RGB), sphere (360/180), ui (solid), text (glyph atlas).
- `CMakeLists.txt` — `PIXELVR_OPENXR_TAG`, shader list, FFmpeg/FreeType/smbclient/pulse.
