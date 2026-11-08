# Steam Cloud and Steam Workshop — feasibility research (2026-11-08)

Research only; nothing implemented. Sources are linked inline. "Verified" means checked
against Valve's docs, the SDK changelog or the headset; everything else is marked as an
assumption or open question.

## TL;DR

| | Feasible on Steam Frame? | Effort | Value | Recommendation |
|---|---|---|---|---|
| **Steam Cloud** | Yes. Auto-Cloud needs **no code**; Steamworks SDK ships `linuxarm64` libs since 1.63 | S (Auto-Cloud) / M (Remote Storage API) | Moderate: backup across reinstall/factory reset, second headset, any future PC/Deck build | Do it, via Auto-Cloud on a dedicated sync folder the app fills itself |
| **Steam Workshop** | Yes. `ISteamUGC` works for software titles (Wallpaper Engine is the big example) | M–L, mostly *our* content system, not the Steam part | High **if** we ship more than rooms (see list below) | Build a local "content pack" system first; Workshop is then a thin distribution layer |

## 1. Platform facts (verified)

- **Steamworks SDK on Linux ARM64:** SDK 1.63 (Nov 2025) "Added libs for linuxarm64 and
  androidarm64"; 1.64 changed linuxarm64 libs to 4 KB pages; 1.65 (Jul 2026) added
  `ISteamUtils::IsRunningOnSteamHardware()`.
  ([SDK readme mirror](https://github.com/rlabrecque/SteamworksSDK/blob/main/Readme.txt),
  [Valve: Steam Frame custom engines](https://partner.steamgames.com/doc/steamhardware/steamframe/engines/custom)).
  Valve recommends Linux ARM64 or Android 10 for custom engines on the Frame, which is what we are.
- **Linking:** on Linux you link `libsteam_api.so` and ship it next to the executable
  ([API overview](https://partner.steamgames.com/doc/sdk/api)).
- **Licensing gotcha:** the player is MIT and public on GitHub. The Steamworks SDK can't be
  committed to a public repo. Keep Steam support in an optional module (`PIXELVR_HAVE_STEAM`),
  build it only in a release job that fetches the SDK from a private location, and `dlopen`
  `libsteam_api.so` so the GitHub build runs without Steam.
- **No account age:** Steamworks reports no age. Mature-content filtering for Workshop items
  happens on Steam's side, through the user's own content preferences (see §3.4).

## 2. Steam Cloud

### 2.1 Two ways to do it
1. **Steam Auto-Cloud** (recommended). This is configured on the partner site only, with no
   code: root `LinuxHome` + a subdirectory + file patterns. Steam syncs on launch and on exit,
   and creates a `steam_autocloud.vdf` file in each synced folder
   ([Steam Cloud docs](https://partner.steamgames.com/doc/features/cloud)). Valve's guidance:
   "Avoid machine-specific configurations such as video quality." Keep the OS setting
   *Cross-Platform* so a future Windows/Deck build can add root overrides.
2. **`ISteamRemoteStorage`** (only if needed). It reads and writes files explicitly, can sync
   mid-session, and reports quota. It needs the SDK in the app. Don't mix the two for the same
   files.
- **Quota:** set per app (bytes and file count per user) on the Steam Cloud settings page.
  Workshop preview images also need a cloud quota.

### 2.2 What makes sense to sync

Our current files (from the code):

| File | Sync? | Why |
|---|---|---|
| `~/.config/pixelvr/settings.conf` | **Yes**, mostly | UI choices, bindings, rail shortcuts, curvature, Multiple Screens, download-folder choice. If device-specific values appear later (render scale, MSAA), move them to a local file. |
| `~/.config/pixelvr/bookmarks.json` | **Yes** (opt-out) | Small, personal, useful after a reset. Paths can reveal private folders, so let users turn it off. |
| `~/.config/pixelvr/playback_state.tsv` (resume positions) | **Yes** (opt-out) | Most useful for network paths (`smb://`, `sftp://`, `dav://`), which are identical on every device. Cap its size. **Check first that vault items never get in** (Plex items already don't). |
| `~/.config/pixelvr/smb_shares.json` | **Definitions only** | Share list, protocol, host, port, user, label: yes. **Passwords: never.** Today they're encrypted with a key derived from `/etc/machine-id` (`smb_credentials.cpp`), so they couldn't be decrypted on another device anyway, and Steam Cloud isn't end-to-end encrypted. Needs a split into `shares.json` (synced) plus local secrets; the user re-enters each password once per device. |
| SFTP host keys (`remote_fs.cpp`) | Optional | Keeps trust-on-first-use continuity. Low value, small. |
| `~/.config/pixelvr/vault.conf` | **Never** | Unlock configuration for the hidden vault. |
| `~/.pixelvr_vault/`, videos | **Never** | Private and huge. |
| `~/.config/pixelvr/chromium/` (browser profile) | **Never** | Contains cookies and login tokens (Plex, Netflix…), hundreds of MB. |
| `~/.cache/pixelvr/*` (thumbnails, FFmpeg) | Never | Cache. |
| *Future:* room choice, custom rooms, saved screen arrangements, keyboard layout (K2), per-video funscript offsets | Yes | Small, personal and portable. |

**Design:** don't point Auto-Cloud at `~/.config/pixelvr` as a whole. That folder holds the
vault configuration, the browser profile and secrets. Use a dedicated folder such as
`~/.config/pixelvr/cloud/` that Auto-Cloud syncs. The app then decides what goes there, which
makes the "sync history/bookmarks" opt-outs simple: if it's off, those files aren't written there.

### 2.3 Value, honestly
The Frame is usually a user's only device running the player, so the main value is
**restore after reinstall or reset**, plus a second headset or a future PC/Deck build. That is
modest, but Auto-Cloud is cheap. It's mainly worth it once the shares file is split, so
passwords stay local.

## 3. Steam Workshop

### 3.1 How it works (verified)
- **API:** `ISteamUGC` ([implementation guide](https://partner.steamgames.com/doc/features/workshop/implementation),
  [API](https://partner.steamgames.com/doc/api/ISteamUGC)). Users subscribe on the Workshop
  page or in-app, and Steam downloads the item.
- **Consuming:** the app calls `GetSubscribedItems` / `GetItemInstallInfo`, or handles the
  `ItemInstalled_t` callback, and reads a plain folder.
- **Publishing:** `CreateItem` → `StartItemUpdate` → `SetItemTitle/Description/Tags/Content/Preview`
  → `SubmitItemUpdate`. The result tells you if the user still has to accept the Workshop legal
  agreement.
- **Setup:** "Enable ISteamUGC for file transfer" and set a Steam Cloud quota for the preview
  images.
- **Separate uploader:** a separate app ID (e.g. a PC uploader tool) can be given publish rights
  to our Workshop.
- **Mature-content descriptors:** `AddContentDescriptor` takes nudity/sexual, adult-only sexual,
  gratuitous sexual, violence or general mature. Steam's servers then filter items by each user's
  preferences ([Rust enum docs](https://docs.rs/steamworks/latest/steamworks/enum.UGCContentDescriptorID.html)).
- **Precedent for software titles:** Wallpaper Engine runs its whole content ecosystem on the
  Workshop.

### 3.2 What users could submit (more than rooms)
All of these are **data, not code**: safe to load, easy to validate.

1. **Rooms / environments** (R1): skybox (equirect image), optional low-poly glTF scene,
   lighting, floor, ambient colour, screen anchor points, and the "cinema black" style. Needs
   R1 plus a small scene renderer, with hard limits on triangles and texture size so it never
   costs decode or frame time.
2. **Screen arrangement presets:** named multi-screen layouts (sports wall, 3-screen cockpit,
   "bed mode" ceiling screen). Tiny JSON, immediately useful.
3. **Lens / projection profiles:** custom meshes or UV maps for fisheye 190°/200°, Canon RF
   dual-fisheye, odd VR180 camera rigs. DeoVR ships such meshes too
   ([naming convention section](https://deovr.com/documentation)). A real niche, and creators
   have the cameras.
4. **Keyboard layouts** (K2): community layouts for languages we don't cover. Strong
   justification: people actually need this.
5. **UI themes:** accent colour, glass tint and opacity, rail icon sets.
6. **Colour-grading LUTs** (`.cube`): "cinema", "HDR-ish", "night" looks applied in the video
   shader. Only LUT data, **no user shaders** (SPIR-V from strangers can hang the GPU).
7. **Web shortcut packs:** regional streaming services (ARD/ZDF Mediathek, BBC iPlayer,
   Crunchyroll…) with name, URL and icon. Needs moderation: a URL can be phishing. Show the
   domain in the UI and allow only https.
8. **Room acoustics:** impulse responses / reverb presets ("cinema hall") once we have a reverb
   stage.
9. **Controller binding presets.**
10. **Translations**, once the UI is localised.

**Don't put on the Workshop:** funscripts or other adult material (payment-processor rules,
§4 of the haptics doc), videos or anything copyrighted, executable plugins.

### 3.3 Architecture recommendation
1. **Content packs first, Steam second.** One folder format:
   `pack.json` (type, version, name, author, licence) + assets. It's loaded from
   `~/.local/share/pixelvr/content/` **and** from Workshop install folders. The GitHub build
   gets the same content via manual install, and creators can test locally before publishing.
2. **Workshop layer:** subscribed items → install paths → the same loader. An in-app
   "Get more…" tab using `CreateQueryAllUGCRequest` filtered by type tags, plus
   `SubscribeItem`. On the Frame this is more reliable than relying on the Steam overlay.
3. **Publishing from a PC:** rooms and LUTs are made in Blender or Photoshop, not in the headset.
   Offer a small cross-platform uploader (or document SteamCMD `workshop_build_item`).
   In-headset publishing is only worthwhile for things made in the app: arrangements, themes,
   keyboard layouts.
4. **Validation:** JSON schema per type, size caps, image/glTF decoding in a guarded path,
   reject unknown types.

### 3.4 Responsibilities
- Users must accept the Workshop agreement before their first upload.
- Steam's servers hide items by descriptor based on user preferences.
- We still need report handling and moderation, especially for web shortcut packs.

## 4. Open questions
- [ ] Does the Steam overlay on the Frame let users browse and subscribe to Workshop items in-headset?
- [ ] Ship `libsteam_api.so` in the depot only, and `dlopen` it from the GitHub build (assumed,
      not tested). Check the Steamworks SDK licence terms for GitHub redistribution.
- [ ] Quota sizes: a few MB is plenty for §2.2. It's unclear how Workshop uploads count
      against the app's Cloud quota. Valve's guide only mentions preview images; a
      third-party SDK wrapper says Workshop files count too. Check on the partner site.
