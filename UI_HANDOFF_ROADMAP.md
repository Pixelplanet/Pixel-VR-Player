# PixelVRPlayer — Spatial UI Visual Polish, UX & Technical Handoff Roadmap

This document is a complete, implementation-ready handoff guide for **PixelVRPlayer** so any new agent or developer can pick up immediately.

---

## 1. Visual Concept Mockups (Spatial VR UI Gallery)

Generated 16:9 high-resolution concept mockups (stored in the artifact directory `C:\Users\tomwi\.gemini\antigravity\brain\0772bbab-53a0-49f5-b094-22a628d153cf\`):
- **Mockup 1 — File Browser:** `mockup_ui_file_browser_1792865434894.jpg` (Glassmorphic cards, header Up button, breadcrumb pills, folder stack lip & right scrollbar)
- **Mockup 2 — Settings View:** `mockup_ui_settings_1792865444847.jpg` (Visual toggle switches, stepped progress bars & color swatch dots)
- **Mockup 3 — Cinema Transport Dock:** `mockup_ui_transport_dock_1792865461315.jpg` (Timeline hover timestamp bubble, icon pills & hero glow ring)
- **Mockup 4 — Cinema Context & Options:** `pixelvrplayer_spatial_ui_mockup_1792865752605.jpg` (3D projection format pills, audio/subtitle drawer & theater ambilight)

---

## 2. Key Codebase Map

| Subsystem | Files | Key Symbols / Line Ranges |
| :--- | :--- | :--- |
| **UI & Text Renderer** | `src/gfx/vulkan_renderer.hpp`, `src/gfx/vulkan_renderer.cpp` | `VulkanRenderer::renderView`, `VulkanRenderer::updatePointer` |
| **Procedural SDF UI Shader** | `shaders/ui.frag`, `shaders/ui.vert` | `sdRoundedBox`, `Push` constants (`params.x` = shape, `params.y` = radius, `params.z` = borderWidth) |
| **Thumbnail Shader** | `shaders/thumb.frag` | Rounded-corner SDF mask + sRGB-to-linear conversion |
| **SDF Font Atlas (UTF-8 / Latin-1)** | `src/gfx/text_atlas.hpp`, `src/gfx/text_atlas.cpp` | `buildFontAtlas` (`glyphs[256]`) |
| **File Browser & SMB** | `src/ui/browser.hpp`, `src/ui/browser.cpp` | `Browser::scanLocalInto`, `Browser::scanSmbInto`, `folderHistory_` |
| **Thumbnail & Metadata Cache** | `src/ui/thumbnail_cache.hpp`, `src/ui/thumbnail_cache.cpp` | `ThumbnailCache::extractThumbnailAndMeta` |
| **SteamVR Virtual Keyboard** | `src/xr/vr_keyboard.hpp`, `src/xr/vr_keyboard.cpp` | `VrKeyboard::show`, `VrKeyboard::poll` |
| **Main App Loop & Input** | `src/app/app.hpp`, `src/app/app.cpp` | `App::run` |

---

## 3. Visual UI Improvements (Step-by-Step Implementation Specs)

### 3.1. Frosted Glass Gradient & Top Specular Rim in `shaders/ui.frag`
Right now, `Shape 0` (`UiShape::RoundedBox`) in `shaders/ui.frag` outputs a flat solid color (`pc.color`) and uniform border.
- **Implementation in `shaders/ui.frag`:**
  ```glsl
  vec4 fillLin = vec4(pow(max(pc.color.rgb, vec3(0.0)), vec3(2.2)), pc.color.a);
  if (int(pc.params.x) == 0) {
      // Subtle vertical luminance gradient across panels/pills (lit from top)
      float topLight = mix(0.92, 1.10, clamp(0.5 * (vCoord.y + 1.0), 0.0, 1.0));
      fillLin.rgb *= topLight;
  }
  vec4 finalColor = fillLin;
  if (hasBorder) {
      float borderWidth = pc.params.z;
      float innerD = d + borderWidth;
      float borderAlpha = 1.0 - smoothstep(-aa, aa, innerD);
      vec4 bordLin = vec4(pow(max(pc.borderColor.rgb, vec3(0.0)), vec3(2.2)), pc.borderColor.a);
      if (int(pc.params.x) == 0) {
          // Specular catchlight on upper chamfer/rim
          float rimLight = mix(0.85, 1.38, smoothstep(-0.2, 0.95, vCoord.y));
          bordLin.rgb *= rimLight;
      }
      finalColor = mix(bordLin, fillLin, borderAlpha);
  }
  ```

### 3.2. Thumbnail Vignette Scrims (`shaders/thumb.frag`) & Card Depth
In `shaders/thumb.frag`, thumbnails are drawn without scrims, causing white duration and badge text to wash out against bright scenes.
- **Implementation in `shaders/thumb.frag`:**
  ```glsl
  vec3 linRgb = pow(max(col.rgb, vec3(0.0)), vec3(2.2));
  // Top scrim (for format badges) and bottom scrim (for title / duration pills)
  float topScrim = mix(0.68, 1.0, smoothstep(0.0, 0.26, vUV.y));
  float botScrim = mix(0.58, 1.0, smoothstep(1.0, 0.70, vUV.y));
  linRgb *= (topScrim * botScrim);
  outColor = vec4(linRgb * pc.color.rgb, col.a * pc.color.a * alpha);
  ```
- **Folder Stack Visual Cue (`src/gfx/vulkan_renderer.cpp`):**
  - When `item.isDir` is `true` in `BigCards` or `MiniCards`, draw a secondary rounded box `0.008m` above `lay.thumbCy + lay.thumbHalfH` (`width = lay.thumbHalfW * 0.88f, height = 0.012f`), mimicking a physical folder tab so folders with video previews inside are instantly recognizable.
- **Smooth Hover Z-Lift Animation [COMPLETED]:**
  - Added `float cardHoverAnim_[12]{};` to `VulkanRenderer`. Each frame, lerps `cardHoverAnim_[i]` toward `1.0f` if `entryIdx == browserHovered_` else `0.0f` (`speed ~ 14.0f * dt`).
  - Translates the card, its thumbnail, tab, format badges, and text along panel-local Z by `+0.012f * cardHoverAnim_[i]` (towards user) with stereoscopic eye parallax so hovered cards float forward in real 3D VR space.

### 3.3. Header Navigation & Breadcrumbs (`src/gfx/vulkan_renderer.cpp`)
- **Dedicated `[ ↑ Up ]` Button in Header:**
  - Add `browserPageBtnHovered_ = 4` at `cx = -0.43f, cy = 0.455f, halfW = 0.042f, halfH = 0.028f` labeled `↑ Up`.
  - In `src/app/app.cpp`, clicking it navigates up if `entries()[0].isBack` is present, allowing one-click parent folder return from any scroll page.
- **Segmented Breadcrumb Styling:**
  - Replace raw path strings (`/home/steamos/Videos/Sub`) with pill segments: `Home › Videos › Subfolder` or `SMB › MediaServer › 3D Movies`.
- **Right-Edge Scrollbar Track & Thumb:**
  - When `total > perPage`, draw a slim vertical track at `cx = 0.875f, cy = -0.02f, halfW = 0.004f, halfH = 0.34f` with an accent thumb sized to `perPage / total`.

### 3.4. Settings View Visual Controls (`src/gfx/vulkan_renderer.cpp`)
- **Toggle Switches:** Draw pill switches (`halfW = 0.036f, halfH = 0.018f`) with sliding round knobs for `ON` / `OFF` and `Visible` / `Hidden` settings instead of plain text.
- **Color Swatches:** In `Settings > UI & Appearance`, render circular color dots for `Text Color` and `Accent Theme` with a white halo ring on the active selection.
- **Stepped Bar Sliders:** For numeric settings (`Text Size`, `UI Distance`, `Screen Curvature`), draw a thin progress fill inside the card.

### 3.5. Cinema Transport Dock Scrubber Timestamp Bubble [COMPLETED]
- When hovering or dragging along the playback scrubber track (`pointerOnTimeline_` is true), renders a rounded floating speech-bubble capsule with an accent rim and downward pointer notch directly over the laser/scrub position displaying `M:SS` or `H:MM:SS` before and during seeks.

---

## 4. Architectural & Performance Improvements

### 4.1. High-Priority Visible Thumbnail Loading (`ThumbnailCache`)
- **Problem:** In large folders (50+ videos), `thumbnailCache_.request()` queues items in FIFO order. Visible cards on page 3 or 4 must wait behind 30 unseen videos.
- **Solution:**
  1. In `src/ui/thumbnail_cache.hpp`, change `std::queue<std::string> queue_` to `std::deque<std::string> queue_`.
  2. In `ThumbnailCache::request(const std::string& videoPath, bool highPriority = false)`:
     ```cpp
     if (highPriority) {
         queue_.erase(std::remove(queue_.begin(), queue_.end(), videoPath), queue_.end());
         queue_.push_front(videoPath);
     } else {
         queue_.push_back(videoPath);
     }
     ```
  3. In `src/app/app.cpp`, query visible slot indices `curFirst .. curFirst + numSlots - 1` and pass `highPriority = true`.

### 4.2. Top-Bottom (Over-Under) 3D Thumbnail Un-Squishing
- In `ThumbnailCache::extractThumbnailAndMeta` (`src/ui/thumbnail_cache.cpp`):
  ```cpp
  const bool isTb = (lowerPath.find("tb") != std::string::npos ||
                     lowerPath.find("ou") != std::string::npos ||
                     lowerPath.find("overunder") != std::string::npos ||
                     lowerPath.find("htab") != std::string::npos);
  if (isSbs && srcW >= 2 * kThumbW) {
      srcW = frame->width / 2;
  } else if (isTb && srcH >= 2 * kThumbH) {
      srcH = frame->height / 2;
  }
  ```

### 4.3. Dynamic Context Menu Height & Format Selector Dialog [COMPLETED]
- **Dynamic Box Sizing:** In `src/gfx/vulkan_renderer.cpp`, computes context menu container `mHalfH` dynamically as `std::max(0.20f, 0.5f * menuSpanY + 0.130f)` so buttons never overflow outside the background box.
- **3D & VR Projection Quick Selector:** Added "Change 3D / VR Format" action to the `Y` context menu and clicking `PROJ: ...` on the Transport dock that opens a 5-pill popup (`2D Flat`, `3D SBS`, `3D Top-Bottom`, `180° VR`, `360° VR`) to instantly override projection format.

### 4.4. Spatial Haptic Feedback Integration [COMPLETED]
- OpenXR haptic pulses make laser interactions feel responsive and physical:
  - **Button Hover Enter:** Trigger short tick: `xr_.triggerHaptic(hand, 0.008f, 0.0f, settings_.hoverHapticsStrength())`.
  - **Button Click / Toggle:** Trigger crisp confirmation click: `xr_.triggerHaptic(hand, 0.025f, 0.0f, 0.45f)`.
  - **Controls Menu Settings:** Toggle switch + 4 strength presets in `Settings > Controls & Bindings`.

### 4.5. Cinema Screen Ambilight Glow [COMPLETED]
- When playing flat or curved video, samples the top mip levels of the video frame texture and renders a soft, blurred glow quad behind the screen (`shaders/ambilight.frag` and `shaders/ambilight_nv12.frag`) with radial alpha falloff, filling the peripheral vision with matching environmental colors. Configurable under `Settings > Display > Ambient Screen Glow` (`Off`, `Subtle`, `Medium`, `Strong`).

### 4.6. OpenXR Swapchain Supersampling & Panel-Native Resolution Anchoring [COMPLETED]
- Anchored baseline resolution directly to the headset's physical panel dimensions (**2160×2160 per eye**), preventing SteamVR's conservative runtime default (1728×1728) from downsampling and degrading text and video clarity.
- 5 tuned presets in `Settings > Display > Render Resolution`:
  - `1.00x (Panel 2160p)`: Exactly 2160×2160 physical panel 1:1 pixel grid.
  - `1.15x (Enhanced 2484p)`: 2484×2484 per eye.
  - `1.25x (Balanced 2700p)`: 2700×2700 per eye.
  - `1.40x (Ultra 3024p, Default)`: 3024×3024 per eye (optical fovea 1:1 match after lens barrel distortion warp).
  - `1.50x (Extreme 3240p)`: 3240×3240 per eye for extreme edge clarity.
- Tightened FreeType SDF font shader kernel in `shaders/text.frag` (`smoothstep(-0.45, 0.45, ...)` + `0.18px` stem darkening bias) for crisp, printed-type clarity.

### 4.7. OpenXR Composition Layer Quad Migration [IN PROGRESS]
- Architectural migration to separate the 2D planar elements (Flat Cinema Screen and Spatial UI) from the 3D projection eye swapchain into dedicated OpenXR Composition Layer Quads (`XrCompositionLayerQuad`).
- By submitting quads directly to the OpenXR compositor in `xrEndFrame()`, the SteamVR runtime distortion scanout pass samples the video and UI textures with single-pass high-order anisotropic/bicubic filtering, completely bypassing projection layer downsampling and geometric perspective blur.

---

## 5. Build, Test & Headset Deployment Reference

- **Device IP:** `192.168.178.61`
- **SSH Key:** `$HOME\AppData\Local\steamos-devkit\steamos-devkit\devkit_rsa`
- **One-Line Remote Build & Deploy Command:**
  ```powershell
  ssh -i "$HOME\AppData\Local\steamos-devkit\steamos-devkit\devkit_rsa" -o StrictHostKeyChecking=no steamos@192.168.178.61 "cmake --build ~/pixelvr-src/build --parallel && cp ~/pixelvr-src/build/bin/pixelvr ~/devkit-game/PixelVRPlayer/pixelvr.real && cp ~/pixelvr-src/build/bin/pixelvr ~/PixelVRPlayer/pixelvr && cp ~/pixelvr-src/build/shaders/*.spv ~/devkit-game/PixelVRPlayer/shaders/ && cp ~/pixelvr-src/build/shaders/*.spv ~/PixelVRPlayer/shaders/ && echo '=== BUILD & DEPLOY COMPLETE ==='"
  ```
