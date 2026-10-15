# Pixel VR Player

A native VR video player for the **Valve Steam Frame**, playing flat 2D, 180°, and
360° video (mono and stereoscopic 3D) from local storage and the local network.

- **Platform:** SteamOS (Arch Linux, ARM64) on Snapdragon 8 Gen 3
- **Stack:** C++20 · OpenXR (SteamVR runtime) · Vulkan · FFmpeg (V4L2 M2M HW decode)
- **Targets:** native Linux `x86_64` (for SteamVR-streamed desktop dev) and
  `aarch64` (on-device, run directly on the SteamOS host)

> Status: **Feature-complete player.** OpenXR + Vulkan per-eye rendering with a
> shared depth buffer; hardware video decode (`h264_v4l2m2m` / `hevc_v4l2m2m` →
> NV12) with zero-copy dma-buf import; flat / 180° / 360° projection and mono /
> SBS (including half-SBS) / TB stereo, auto-detected from the filename; spatial
> audio with a master clock. In-headset UI: file browser (with file type and
> size), transport bar, settings menu, and a downloads manager. Native Valve
> Steam Frame controller support (`XR_VALVE_frame_controller_interaction`) using
> the runtime's own render model, with per-button animation on the model and
> press-to-bind remapping.

## Install (prebuilt release)

Download `pixelvr-linux-arm64.tar.gz` from the
[Releases](../../releases) page and copy it to your Steam Frame, then:

```bash
tar -xzf pixelvr-linux-arm64.tar.gz -C ~
chmod +x ~/PixelVRPlayer/run.sh
~/PixelVRPlayer/run.sh                 # opens the in-headset file browser
~/PixelVRPlayer/run.sh /path/clip.mp4  # …or auto-play a specific clip
```

To launch it from the headset library, add `~/PixelVRPlayer/run.sh` as a
**Non-Steam Game** (Steam ▸ Add a Non-Steam Game). SteamVR must be running; the
app logs to `~/pixelvr-run.log`.

Requirements: the SteamVR OpenXR runtime (SteamOS), Vulkan, and FFmpeg with the
V4L2 M2M decoders. For zero-copy dma-buf decoding, build the patched FFmpeg into
`~/ffbuild` — `run.sh` picks it up automatically when present, otherwise it falls
back to the system FFmpeg with CPU colour conversion.


## Controls

The laser pointer and the control bar are **separate** input paths, so the bar
stays usable while a panel is open.

| Input | Action |
| --- | --- |
| Thumbstick | Move the highlight in a panel; left/right seeks while playing |
| **A** | Open the highlighted browser entry — folder navigates, video plays / streams. Outside the browser it is play / pause |
| **B** | Open or close the file browser; also closes the settings panel and cancels the download dialog |
| **Trigger** | Laser pointer: click a panel row, or a control-bar button |
| **Grip** | Recenter the view in front of you |

The control bar has a dedicated **Download** button (arrow) that downloads the
entry highlighted in the browser — folders ask for confirmation and show their
size first. The **Downloads** button (list icon) opens the downloads manager,
where you can pause, resume, stop and clear jobs, then play a finished file or
open a finished folder.

All button bindings can be changed in **Settings ▸ Controls** (press-to-bind).


## Repository layout

```
src/
  util/      logging, math, small helpers
  xr/        OpenXR instance/system/session, swapchains, frame loop, input
  gfx/       Vulkan device (from OpenXR), render pass, pipelines, per-eye render
  scene/     projection meshes (flat / 180 hemisphere / 360 sphere), stereo material
  media/     demux + decode, frame queue, A/V clock, audio out   (Phase 2+)
  ui/        in-VR panel: file/library browser, transport, settings  (Phase 2+)
  sources/   local FS + SMB/DLNA/HTTP media sources                 (Phase 4)
  app/       app lifecycle, config, wiring
shaders/     GLSL sources compiled to SPIR-V at build time
cmake/       toolchain + shader-compile helpers
tools/       build & deploy scripts
```

## Prerequisites (Linux build environment)

On Windows, use **WSL2 (Ubuntu 22.04+)** or a Linux container. Install:

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git pkg-config \
    libvulkan-dev vulkan-tools glslang-tools spirv-tools \
    libx11-dev libxrandr-dev libxxf86vm-dev mesa-common-dev libgl1-mesa-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev
```

The OpenXR loader is fetched and built from source via CMake `FetchContent`, so no
system OpenXR package is required. It is built **statically** into the executable
(no `libopenxr_loader.so` to ship) and configured for the **xlib** presentation
backend only (XCB/Wayland disabled), so the `libx11`/`mesa` headers above are the
only windowing dependencies needed to build it.

> On WSL, VS Code cannot answer `sudo`'s password prompt; run apt as root instead:
> `wsl -u root -- apt-get install -y ...`.

### aarch64 cross-compilation

Two options, in order of runtime reliability:

1. **Device sysroot (recommended).** Capture the headset's own libraries so the
   binary links against the exact FFmpeg / Vulkan / X11 SONAMEs it will load:

   ```bash
   ./tools/fetch-sysroot.sh steamos@frame        # rsyncs into ./sysroot
   PIXELVR_SYSROOT="$PWD/sysroot" ./tools/build.sh --aarch64
   ```

2. **Debian/Ubuntu arm64 multiarch (quick validation).** Enable the arm64 ports
   archive and install the `:arm64` `-dev` packages, then build without a sysroot:

   ```bash
   sudo dpkg --add-architecture arm64
   # add http://ports.ubuntu.com/ubuntu-ports/ as an arm64 source, then:
   sudo apt-get update
   sudo apt-get install -y g++-aarch64-linux-gnu \
       libavcodec-dev:arm64 libavformat-dev:arm64 libavutil-dev:arm64 \
       libswscale-dev:arm64 libswresample-dev:arm64 \
       libvulkan-dev:arm64 libx11-dev:arm64 libxrandr-dev:arm64 libxxf86vm-dev:arm64
   ./tools/build.sh --aarch64
   ```

   Ubuntu's arm64 FFmpeg may differ in SONAME from the headset's; if the app
   fails to load a library on-device, use the sysroot path above. Audio
   (`libpulse-dev`) is omitted here because its glib dev chain is not
   co-installable across architectures — the sysroot build restores it.

## Build

Native (desktop, for SteamVR-streamed iteration):

```bash
./tools/build.sh            # configures + builds build/ for host arch
```

aarch64 cross (for the Steam Frame):

```bash
./tools/build.sh --aarch64  # configures + builds build-aarch64/
```

## Deploy to Steam Frame

See the full walkthrough in
[docs/deploy-to-steam-frame.md](docs/deploy-to-steam-frame.md) (options, runtime
choice, and troubleshooting). In short, after a one-time Developer-Mode pairing:

- **GUI (easiest to start):** `./tools/stage.sh` to assemble `dist/PixelVRPlayer/`,
  then upload it in the **SteamOS Devkit Client** (Runtime: *Steam Linux Runtime
  3.0 ARM64 (Sniper)*, Start Command: `pixelvr`).
- **CLI (easiest to iterate):** one command copies the binary + shaders and
  launches it in the headset:

  ```bash
  ./tools/deploy.sh --launch -- /home/steamos/Videos/clip_360_tb.mp4
  # override the target with --host steamos@<ip> or FRAME_HOST
  ```

Either way the app appears in **Steam ▸ Library ▸ Non-Steam** as *Devkit Game:
PixelVRPlayer*. To run it manually over SSH for debugging:

```bash
ssh steamos@frame '~/devkit-game/PixelVRPlayer/launch.sh /path/to/video.mp4'
```

## Test on device

Projection and stereo layout are inferred from the filename, so name test clips
accordingly (or pass a plain 2D clip for flat playback):

| Filename contains        | Projection | Stereo        |
|--------------------------|------------|---------------|
| `...360...`, `_sbs`/`_lr`| 360°       | side-by-side  |
| `...180...`/`vr180`, `_tb`/`_ou` | 180° | top-bottom |
| (none)                   | flat 2D    | mono          |

Requirements and tips:

- **8-bit H.264 or HEVC** with even width/height (4:2:0). 10-bit / Main10 is not
  supported by the V4L2 M2M decoder path.
- `PIXELVR_FORCE_SW=1` forces software decode (useful for debugging).
- `PIXELVR_LOG=debug` raises log verbosity.

## License

TBD.
