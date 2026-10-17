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
> size), transport bar, settings menu, downloads manager, and an encrypted hidden vault. Native Valve
> Steam Frame controller support (`XR_VALVE_frame_controller_interaction`) using
> the runtime's own render model, with per-button animation on the model and
> press-to-bind remapping.

## Screenshots

<p align="center">
  <img src="docs/ui-redesign/mockup-unified-1-files.jpg" width="49%" alt="Spatial File Browser" />
  <img src="docs/ui-redesign/mockup-unified-2-downloads.jpg" width="49%" alt="Downloads Manager" />
</p>
<p align="center">
  <img src="docs/ui-redesign/mockup-unified-3-settings.jpg" width="49%" alt="Settings Panel" />
  <img src="docs/ui-redesign/mockup-unified-4-cinema-hud.jpg" width="49%" alt="Cinema Playback HUD" />
</p>

## Installation

Choose whichever method fits your setup:

### Option A: USB Drive / MicroSD Card (No typing & No Dev Mode needed)

The simplest method for standard users without touching the terminal:

1. Download `pixelvr-linux-arm64.tar.gz` from the [Releases](../../releases) page onto a USB flash drive or MicroSD card.
2. Plug it into your Steam Frame and switch to **Desktop Mode**.
3. Open the USB drive in Dolphin and double-click **`Install Pixel VR Player.desktop`**.
4. A confirmation dialog will appear when finished. Return to Gaming Mode and launch it directly from your Steam Library!

### Option B: One command in headset terminal

If you are already in Desktop Mode on the Steam Frame, open **Konsole** and paste:

```bash
curl -fsSL https://raw.githubusercontent.com/Pixelplanet/Pixel-VR-Player/main/tools/install.sh | sh
```

*(No root access or disabled read-only filesystem needed; it installs entirely into user space).*

### Option C: From your PC over Wi-Fi (Requires SSH or Dev Mode)

If you have SSH enabled or Developer Mode turned on with your headset paired:

- **Windows:** Double-click [`tools/install-from-pc.bat`](tools/install-from-pc.bat) and enter your headset's IP.
- **macOS / Linux:** Run `./tools/install-from-pc.sh`.

The script connects over SSH, installs the app, and registers the Steam shortcut remotely.

Launch **Pixel VR Player** directly from your Steam library, or from terminal:

```bash
~/PixelVRPlayer/run.sh                 # opens the in-headset file browser
~/PixelVRPlayer/run.sh /path/clip.mp4  # …or auto-play a specific clip
```

Installer options:

| Option | Effect |
| --- | --- |
| `--dir DIR` | install somewhere other than `~/PixelVRPlayer` |
| `--version vX.Y.Z` | install a specific release instead of the latest |
| `--no-steam` | skip the Steam library entry |
| `--restart-steam` | close Steam first, then reopen it so the entry appears at once |
| `--uninstall` | remove the app and its Steam shortcut |

Re-run the same command to upgrade. Settings (`~/.config/pixelvr`) and cached
thumbnails (`~/.cache/pixelvr`) live outside the install directory and are kept.

**Offline install:** download `pixelvr-linux-arm64.tar.gz` from the
[Releases](../../releases) page, copy it to the headset, extract it and run
`./install.sh` from the extracted folder — or use `run.sh` directly. The app logs
to `~/pixelvr-run.log`.

Requirements: the SteamVR OpenXR runtime (SteamOS), Vulkan, and FFmpeg with the
V4L2 M2M decoders. For zero-copy dma-buf decoding, build the patched FFmpeg into
`~/ffbuild` — `run.sh` picks it up automatically when present, otherwise it falls
back to the system FFmpeg with CPU colour conversion.


## Controls

The laser pointer and the control bar are **separate** input paths, so the bar
stays usable while a panel is open.

| Input | Action |
| --- | --- |
| **Trigger** | Point and click: open the entry under the laser, or press a control-bar button |
| **A** | Open the highlighted entry — folder navigates, video plays / streams. Outside the browser it is play / pause |
| **X** / **Y** | Download the highlighted entry (folders confirm first, showing their size) |
| **B** | Open or close the file browser; also closes a panel and cancels the download dialog |
| **Grip** | Toggle **that hand's** laser pointer on/off. Turn one off to navigate with the thumbstick or d-pad without the beam interfering, and to choose which controller points |
| Thumbstick | Move the highlight in a panel; left/right seeks while playing |
| **Bumper** | Recenter the view in front of you |

The control bar has a **Download** button (arrow) that downloads the entry
highlighted in the browser, and a **Downloads** button (list icon) that opens the
downloads manager — where you can pause, resume, stop and clear jobs, then play a
finished file or open a finished folder. Opening the downloads manager replaces
the file browser rather than stacking on top of it.

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

Release builds are automated: `.github/workflows/release.yml` builds natively on
an ARM64 runner in a Debian 13 container (FFmpeg 7, matching the headset's
SONAMEs) and attaches `pixelvr-linux-arm64.tar.gz` to each `v*` tag's release.
To package a local build by hand:

```bash
./tools/package-release.sh build-aarch64 pixelvr-linux-arm64.tar.gz
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
