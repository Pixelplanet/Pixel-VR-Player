# Pixel VR Player — TODO & Handoff

> Native C++/OpenXR/Vulkan/FFmpeg VR video player for the **Valve Steam Frame**
> (Snapdragon 8 Gen 3, Adreno 750 / Turnip, SteamOS/aarch64). This file is the
> handoff for the next agent. Deep device/build facts live in repo memory
> `/memories/repo/steamframe-facts.md` — read that too.

---

## 1. Build / deploy / test (the loop that works)

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
- Controls remapped: A/X=play, B/Y=menu, Trigger=select, Grip=recenter,
  thumbstick=nav/seek/distance.
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

### B1. Video fringing / white-pixel aliasing when the screen is farther away
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

### B2. High-res 360/180 video won't play + stale frame + controls disappear
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

### F1. Real controller models (`XR_EXT_render_model`)  — user's top ask
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

### F3. Controller remap settings UI — DONE
- Implemented `SettingsMenu` model (`src/ui/settings_menu.{hpp,cpp}`) and pointer-driven in-VR
  Settings panel overlay.
- Users can view and cycle button bindings (Play/Pause, Menu, Select, Recenter) and player
  options (Projection Mode, Stereo 3D Mode, Screen Distance, Reset View).
- Settings automatically persist to `~/.config/pixelvr/settings.conf` across restarts.

### F4. Variable-speed joystick fast-forward/rewind
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
