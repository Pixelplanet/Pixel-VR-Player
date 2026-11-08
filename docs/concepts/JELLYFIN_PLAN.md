# Jellyfin like Plex (TODO J1): implementation plan and progress

Decided 2026-11-08: approach A from `docs/concepts/JELLYFIN_AUTOFOCUS_PASSTHROUGH_RESEARCH.md`
(branch `research/jellyfin-autofocus-passthrough`). Jellyfin Web runs in a browser screen; pressing
play there hands the original file to our player (hardware decode), and closing it returns to the
same page. Branch `feature/jellyfin`, worktree `C:\Projects\PixelVRPlayer-jellyfin`, based on
v1.14.0 (`05ce7d7`).

Test server (the user's): `http://192.168.178.10:8096`, "TheVault", Jellyfin 12.4.0
(`/System/Info/Public` answers). The same machine runs Plex (`:32400`). Login credentials are needed
from the user for an end-to-end test.

## Design: mirror the Plex hand-off in `browser/pvr_browser.cpp`
- **Trigger:**
  - `Network.requestWillBeSent` for `…/Items/{id}/PlaybackInfo` (POST), with `IsPlayback=true`
    in the query or the JSON body.
  - Gated on a real play press in the last 4 s. `notePlayIntent` must also recognise Jellyfin Web:
    `data-action="play|resume"`, `btnPlay` / `btnResume` / `playButton` class names; the existing
    regex on class / aria-label mostly covers it.
- **Server base:** the URL part before `/Items/`, which keeps a reverse-proxy base path.
- **Token:** one of the following, never logged:
  - the `Authorization` / `X-Emby-Authorization` header (`MediaBrowser … Token="…"`)
  - the `X-Emby-Token` / `X-MediaBrowser-Token` header
  - the `api_key` / `ApiKey` query parameter
- **User id:** the `UserId` query parameter, else `GET /Users/Me`.
- **Start position:** `StartTimeTicks` (query or body; 10,000,000 ticks = 1 s), else 0.
  Jellyfin Web asks "resume / from start" itself, so the ticks reflect the user's choice.
- **Our own `POST {base}/Items/{id}/PlaybackInfo?UserId=…&IsPlayback=true&AutoOpenLiveStream=true`:**
  - Sent from the page (fetch) with our `Authorization: MediaBrowser Client="Pixel VR Player",
    Device="Steam Frame", DeviceId="pixelvr-<pid>", Version="…", Token="…"`.
  - Its DeviceProfile matches our decoders. Direct Play for mkv/mp4/webm/mov with H.264, HEVC
    (8-bit; 10-bit only on the DRM-PRIME path), VP9, AV1 (software). Audio: aac, ac3, eac3, mp3,
    opus, flac, dts, truehd. Transcoding: hls/ts, HEVC + aac.
  - The result gives `MediaSources[0]` (Id, SupportsDirectPlay/DirectStream, TranscodingUrl,
    RunTimeTicks) and `PlaySessionId`.
- **URL:** Direct Play is `{base}/Videos/{id}/stream?static=true&MediaSourceId={msid}&api_key={token}`;
  otherwise `{base}{TranscodingUrl}`.
- **Title:** `GET /Items/{id}?userId=…` gives `SeriesName - Name` or `Name`.
- **Emit:** reuse the `plex {...}` event (url, title, offset, duration) so `App::startPlexPlay` works
  unchanged. The helper remembers `service = jellyfin`; the dock hint says "Jellyfin".
- **Timeline:** `plextimeline` becomes a dispatch:
  - Jellyfin: `POST {base}/Sessions/Playing` once (`playing`), then `/Sessions/Playing/Progress`.
  - `stopped` goes to `/Sessions/Playing/Stopped`.
  - Body: `{ItemId, MediaSourceId, PlaySessionId, PositionTicks, IsPaused, CanSeek:true,
    PlayMethod:"DirectPlay"|"Transcode"}`.
  - The stopped report is sent *after* the page's own player is closed (the existing
    `returnCheckAt` order), so ours wins over the web player's.
- **Return:**
  - Jellyfin Web's video OSD closes with Escape or browser back. The existing `plexreturn` path
    sends Escape, then `returnCheckAt` → `history.back()` if the URL differs.
  - Jellyfin has no `[data-testid=closeButton]`, so the `closeAt` click must be service-specific:
    for Jellyfin use `history.back()`.
- **Token redaction:** extend `redactToken` (helper) and `redact_token` (`src/util/logging.cpp`) to
  `api_key=`, `ApiKey=`, `Token="…"` and `X-Emby-Token`.
- **Rail shortcut "Jellyfin":** needs a per-user server address, so it's step 2. A settings text
  field "Jellyfin address" (default empty → the shortcut opens the browser with
  `http://<host>:8096`), using the keyboard. Step 1: it works from the normal Browser screen with
  any Jellyfin URL.

## Findings against TheVault (2026-11-08, API from the PC; test user from the user, kept out of the repo)
- `POST /Users/AuthenticateByName` works; the library has 1245 movies / episodes: 1080p H.264
  8-bit and 4K HEVC 10-bit mkv.
- **`PlaybackInfo` with a DeviceProfile:** 37 of 40 movies get `SupportsDirectPlay`.
  - The profile: Direct Play for mkv/mp4/m4v/mov/webm/ts with h264, hevc, vp9, av1. Audio: aac,
    ac3, eac3, mp3, opus, flac, dts, dca, truehd, vorbis, pcm. Every subtitle format declared
    `Method: Embed`.
  - Refused:
    - 1× `ContainerNotSupported, VideoCodecNotSupported`: a real transcode case.
    - 2× `AudioCodecNotSupported, SubtitleCodecNotSupported`: DTS / DTS-HD MA with DVD subs.
      Our FFmpeg plays those directly.
- **Decision:** we decide Direct Play **ourselves** from `MediaStreams`: the video codec is in
  h264 / hevc / vp9 / av1 and the container is one we demux. Then use
  `/Videos/{id}/stream?static=true&MediaSourceId=…&api_key=…`, whatever Jellyfin's audio / subtitle
  verdict. Only use `TranscodingUrl` when the video can't be decoded.
  - The static stream answers `206 video/x-matroska` with byte ranges, also for a file Jellyfin
    refused for Direct Play.
- HEVC 10-bit (4K) needs our DRM-PRIME P010 path, which the release wrapper turns on.

## Progress
- [x] Plan (this file).
- [x] Helper: detection (IsPlayback + press within 4 s), own PlaybackInfo, Direct Play decision, emit, /Sessions/Playing* timeline, return, token redaction (helper + app log). Written, not yet built.
- [x] App: `PlexPlay.service`, dock hint "return to Jellyfin".
- [x] Built on the headset (`~/pixelvr-src-jellyfin/build`).
- [x] **Standalone helper test against TheVault passed (2026-11-08):**
  - Login via Jellyfin Web's form, then a real click on the film page's Play (`btnPlay`, data-action resume).
  - Hand-off: `plex {url: …/Videos/{id}/stream?static=true&MediaSourceId=…&api_key=***, title: "8 Mile",
    duration: 6627.8, service: jellyfin}`. The stream answers `206 video/x-matroska`.
  - The server lists a session "Pixel VR Player" now playing 8 Mile.
  - After `plexreturn 600000` the resume point is 600 s (ours wins over the web player's), and the
    page is back on the details page.
  - No token in the helper's stdout (outside the hand-off line) or stderr.
  - The test's resume point was reset to 0 afterwards. The test script lived in a temp folder and isn't kept.
- [x] Rail shortcut "Jellyfin" (8th shortcut, `rail_shortcuts` bit 7) + "Jellyfin address" row in
      Settings > UI & Appearance > Shortcuts (keyboard; `jellyfin_url` in settings.conf). It is normalised:
      `192.168.1.10:8096` / `…/web/#/home` become `http://host:port`. The shortcut opens `<address>/web/`
      as a kiosk page.
  - **Fix 2026-11-08 (user report):** with no address it used to jump to the Shortcuts page, which looked
    broken (the user had just switched the shortcut on). Now it opens the keyboard right away ("Jellyfin server
    address"), saves the address and opens the server. Log lines `Jellyfin shortcut: …`. Redeployed.
- [x] **Downloads:** `Browser.downloadWillBegin` for `…/Items/{id}/Download?api_key|ApiKey=…` cancels Chromium's
      download and emits `savefile` to our download manager.
  - The name is the server file's own name (Jellyfin Web's `ApiClient.getItem` → `Path`).
  - **Jellyfin 12 answers `/Download?api_key=` with 401** (Chromium saved that error as "Download.txt").
    `ApiKey=` and the login header work, so the hand-off rewrites to `ApiKey=`. Playback URLs use
    `ApiKey=` too (both work for `/Videos/…/stream`).
  - **Tested against TheVault:** name `8 Mile (2002) - 2002 - 1080p - x264 - DTS.mkv`; the handed-over URL
    answers 206 with the same Content-Disposition name; no file left in ~/Downloads; no token in the log.
    The app side (`savefile` → `startDownload`) is the generic path Plex already uses.
- [x] **Deployed to the headset 2026-11-08** for the user's test: `~/pixelvr-src-jellyfin/build-embed` (bundled
      FFmpeg from `~/ffbuild8/prefix`, like `~/pixelvr-src/build-embed`), copied to `~/PixelVRPlayer/pixelvr`
      and its shaders. `--ffmpeg-info`: bundled libavcodec 62.11.100, zero-copy yes.
  - Backup of v1.14.0: `~/pixelvr-backup-v1.14.0/` (pixelvr + shaders). To revert, copy both back.
- [ ] In-headset test by the user.

## User report 2026-11-08 (second test): freeze + crash on Jellyfin play, shortcut rework request
- **Freeze:** `loadMedia` opened the HTTP stream on the main thread; the server took ~15 s
  (17:46:24 → 17:46:39, its disk spinning up), so the frame loop stalled (watchdog `stalled=1`).
  - **Fix (5a505d4):** `App::beginWebPlay` wakes the server on a worker (first 64 KB via `remoteOpen`)
    with a "Waiting for Jellyfin / Plex to start the video..." notice. `startPlexPlay` runs once bytes
    arrived (90 s limit, then `plexRelease` and a notice).
  - The helper keeps the page's own player paused for 90 s (was 15 s).
- **Crash:** SIGSEGV in `swr_convert` (libswresample.so.6) on the decode thread, ~0.3 s after `seek 3243.76s
  reopen=1` (resume) of "2 Fast 2 Furious" (DTS-HD MA + DTS:X 7.1, s32p).
  - Not reproduced standalone: same URL, same swr setup, same seek and flush, 600 frames, stable 8 ch
    s32p, no crash (a test program against the bundled FFmpeg on the headset). The core dump is
    root-only (inaccessible).
  - **Hardening (5a505d4, 278d92b):** swr follows the decoded frame's layout / format / rate (rebuilt and
    logged as "audio input changed" when they differ), and frames with missing planes are dropped. If
    it crashes again, the log shows whether a format change happened.
  - **Not yet deployed** (the player was running).
- **Shortcut rework (requested):** compact rows; each shortcut = name + URL (+ on the rail or not); an
  "Add shortcut" button; the user can pre-fill Jellyfin. Planned storage: `~/.config/pixelvr/shortcuts.json`,
  migrated from `rail_shortcuts` + `jellyfin_url`.
- **Reordering (requested 2026-11-08):** the shortcut list gets a way to reorder entries. Drag and drop
  if the settings panel can do it, otherwise up / down arrows. The order on the rail follows the
  order in the list (the `shortcut=` lines are saved in list order). First version: "Move up" /
  "Move down" on a shortcut's edit page (works with the stick and the laser). Drag and drop needs
  pointer-drag support in the settings card grid, which doesn't exist yet: a follow-up.
- **Shortcut rework, built + deployed 2026-11-08 (afa730f), not yet seen in the headset:**
  - Model: `SettingsMenu::Shortcut {name, url, kiosk, onRail}` in list order, saved as
    `shortcut=onRail|kiosk|name|url` lines in settings.conf. The first start migrates the built-in
    list with the old `rail_shortcuts` bits and `jellyfin_url` (+ `/web/`).
  - The rail shows the first 5 shortcuts that are on (`kMaxRailShortcuts`), in list order.
  - Settings > UI & Appearance > Shortcuts (section 22): dense cards (7 rows of 2, `setSettingsCompact`),
    one per shortcut (name; "On the rail" + address), then "Add shortcut".
  - A shortcut's page (section 17): Name, Address (keyboard), Show on the Rail, Opens as Page only /
    Full browser, Move Up / Move Down (the list order is the rail order), Remove Shortcut, Back.
  - Add creates "New shortcut" (on the rail, full browser) and opens the name keyboard at once.
  - Any shortcut without an address asks for it on first use, then opens it. A Jellyfin address gets
    `/web/` added.
  - Drag and drop: not done (the settings grid has no pointer drag); the arrows are there instead.
- **Crash / freeze fixes deployed** with the same build.

## User report 2026-11-08 (third test)
- Jellyfin **playback works**.
- **Scrubbing** showed nothing until release. The timeline preview (picture in the bubble) was
  switched off for Plex / Jellyfin streams (`!webSuspended`). Now on for http streams
  (`scrub_preview.cpp` opens them with `rw_timeout` 10 s). The video itself still jumps on release,
  as for local files.
- **Downloads slow?** The file was 57.6 GB (4K remux). Measured on the headset:
  - our download 52 MB/s; `curl` on `/Download` 56 MB/s; `/Videos/stream` 88 MB/s.
  - Python with 4 MB ranges: 46 MB/s; with 32 MB ranges: 72 MB/s (Jellyfin pays a lookup per request).
  - The http reader (`DavFile`, `remote_fs.cpp`) now doubles its range up to 32 MB while reading on,
    and goes back to 4 MB after a jump. The new reader: 62.6 MB/s *while the old download still ran
    in parallel*.
  - Not a Jellyfin setting: no throttle; the link is WiFi 6E 160 MHz.
- **Shortcut list redesigned (eb61c2d)** to the user's spec, one line per shortcut:
  - grab handle (hold the trigger and drag; an accent bar shows where it lands; on release the list
    = rail order changes)
  - on / off switch (on the rail)
  - name, address
  - Edit (the shortcut's page: Name, Address, Show on the Rail, Opens as, Delete, Back)
  - Delete (first press "Sure?" in red, a second within 4 s deletes)
  - Then "Add shortcut" (max 10) and Back. Renderer: `kSettingsKindShortcut` lines in the compact
    single-column layout (12 lines), `hub::shortcutZone`, `setSettingsDrag`. App: drag in the
    settings block (`shortcutDragFrom_/To_`, `in.selectHeld`).
  - **Built and deployed; not yet seen in the headset.**
- **Download speed follow-up (user: Plex is faster from the same server):** once the old download had
  finished, the new reader measured **107 MB/s on `/Items/{id}/Download` and 120 MB/s on
  `/Videos/{id}/stream?static=true`**, with the same size and first / last 4 MB identical (8.25 GB file).
  The old 52 MB/s was mostly our 4 MB ranges. Downloads are now handed over as the static stream
  (the name still comes from the server path).
- **Shortcut list polish (efa2afb, after the user's screenshot):** the borders were ~8x the card
  border (wrong `drawUiElement` radius / border arguments), now 0.0012 like the cards. Lines are taller
  (pitch 0.066) and the text is larger (scale 1.0).
  - Dragging: the grabbed line lifts (shadow + glow) and follows the laser smoothly; the lines in
    between glide aside to open the gap (`advanceListAnim`, per-line eased offsets); on release it
    slides from where it was held into its slot.
  - Built + deployed, not yet seen.
- **Still to test in the headset:**
  - TV episodes (series > season > episode) and what happens at the end of an episode.
  - Resume vs. start from the beginning.
  - "2 Fast 2 Furious" with Resume again (the earlier crash).
  - The one film Jellyfin must convert (transcode path).
  - Watched state / resume point in Jellyfin after stopping.
  - Sign-in kept after restarting the player.
- **Known gap:** audio language / subtitles picked in Jellyfin's page are not passed on; our player uses
  its own default / preferred tracks.
- **Other download paths the reader change touches:** Plex "Download" / "Save File" (same http reader:
  should be faster now) and WebDAV file and folder downloads (growing ranges; seeking in WebDAV playback
  starts small again). SMB and SFTP use other readers (unchanged). Ordinary website downloads are
  Chromium's own (unchanged).

## User report 2026-11-08 (fourth test): episode ends, audio tracks, TODO additions
- The new shortcut list looks good.
- **Episode end (Plex and Jellyfin): back to the page only after ~15-20 s, no next episode.**
  Cause in the log: at the real end of a stream FFmpeg's `reconnect_at_eof=1` reconnects twice
  (0+1+3 s), then the engine's own network retry runs 6 more times (300 ms × n), because the error is
  -5 (EIO), not AVERROR_EOF. Plan:
  - `reconnect_at_eof` off (mid-file drops are still covered by `reconnect` while off < filesize).
  - No engine retries once the I/O position has reached the file size.
  - **Next episode:** at the end of an episode the app sends `plexnext <ms>`. The helper reports
    "stopped" for the current one, finds the next and emits a new `plex` hand-off on the same screen,
    or "plexnext none", and then the app returns to the page.
    - Jellyfin: `/Shows/{SeriesId}/Episodes?startItemId=…&limit=2`.
    - Plex: `/library/metadata/{grandparentRatingKey}/allLeaves`.
- **Audio track like the page chose:** Jellyfin `AudioStreamIndex` from the PlaybackInfo request; Plex:
  the part's selected audio stream (`Stream.selected`, `index` = container index). Passed as `audio` in
  the hand-off, applied with `setPreferredAudioStream`.
- TODO: PX1 (skip intro / credits), ST1 (subtitle picker) added.
- **Built + deployed (episode end, next episode, audio tracks):**
  - Engine: no `reconnect_at_eof`, and no retries once the I/O position reached the file size.
    Not yet seen: whether the end now comes at once.
  - Helper: `jfStart()` (shared by the first play and the next episode), `plexEmit()` / `kPlexInfoJs`
    (Plex: part, title, duration, `type`, the selected audio stream `index`), `plexnext <ms>`.
  - **Jellyfin tested standalone against TheVault:** an episode's hand-off carries `"episode":true,"audio":1`;
    `plexnext 0` found and handed off the next episode ("Futurama - Neutopia"); no token in the log.
    - Jellyfin Web sends `"AudioStreamIndex":null` when nothing was picked (it parsed as 0 at first,
      now digits only). Then Jellyfin's `DefaultAudioStreamIndex` (the user's language preference) is used.
  - **Plex next episode and Plex audio: not tested** (no Plex test session outside the headset).
  - App: `plexEpisode` / `plexAwaitingNext` per screen. At the end it shows "Starting the next
    episode...", the next hand-off plays on the same screen, "none" (or 20 s) → back to the page.
    Stop drops a next episode still on its way.
  - No setting to turn auto-play off yet.
