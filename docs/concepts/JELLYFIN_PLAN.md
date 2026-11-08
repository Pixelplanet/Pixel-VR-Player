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
- [ ] Helper: detection, PlaybackInfo, URL, emit, timeline dispatch, return, redaction.
- [ ] App: dock hint text by service; no other app change expected.
- [ ] Build on the headset (separate dir `~/pixelvr-src-jellyfin`), standalone helper test
      against TheVault (needs a Jellyfin login).
- [ ] Rail shortcut + address setting.
- [ ] In-headset test by the user.
