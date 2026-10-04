# Deploying Pixel VR Player to the Steam Frame

This guide covers how to get the built app running on the headset, the available
options, and which one to pick.

**What you're shipping:** a single self-contained ARM64 binary
(`build-aarch64/bin/pixelvr`, ~11 MB, static OpenXR loader) plus four SPIR-V
shaders. That's it — no `libopenxr_loader.so` or other bundled libraries.

---

## TL;DR

```bash
# 1. Build the ARM64 binary (in WSL / Linux)
./tools/build.sh --aarch64

# 2a. Fastest to iterate — one command (needs pairing done once):
./tools/deploy.sh --launch -- /home/steamos/Videos/clip_360_tb.mp4

# 2b. Or stage a clean folder for the GUI Devkit Client:
./tools/stage.sh          # -> dist/PixelVRPlayer/
```

---

## One-time setup

All three methods below share the same setup. Do this once.

1. **Developer Mode** on the headset: *Settings ▸ System ▸ Enable Developer Mode*.
   This turns on SSH/ADB/RDP.
2. **Set a user password**: *Settings ▸ Developer ▸ Set User Password* (needed for
   SSH).
3. *(Recommended)* **Beta channel**: *Settings ▸ System ▸ System Update Channel ▸
   Beta*, and opt the desktop Steam client + SteamVR into Beta too.
4. **Hostname** defaults to `frame` (*Settings ▸ System ▸ Hostname*). Wherever this
   guide says `frame`, substitute your hostname or the headset's IP.
5. **Install the SteamOS Devkit Client** on your PC from Steam
   (`steam://install/943760`; it's under Library ▸ Software).
6. **Pair**: on the headset, *Settings ▸ Developer ▸ Pair new host*. In the Devkit
   Client's **Devkits** tab, click **Register** next to your frame, then confirm on
   the headset. This installs an SSH key and grants full control.

> **Using the SSH key from WSL.** The Devkit Client (Windows) stores its key at
> `%LOCALAPPDATA%\steamos-devkit\steamos-devkit\devkit_rsa`. Our CLI tools run in
> WSL, so copy it where they can find it:
> ```bash
> mkdir -p ~/.config/steamos-devkit
> cp "/mnt/c/Users/$USER/AppData/Local/steamos-devkit/steamos-devkit/devkit_rsa" \
>    ~/.config/steamos-devkit/devkit_rsa
> chmod 600 ~/.config/steamos-devkit/devkit_rsa
> ```
> (Adjust the Windows username if it differs from `$USER`.)

---

## Which option is easiest?

| Method | Per-deploy effort | Best for |
|--------|-------------------|----------|
| **A. Devkit Client (GUI)** | click Upload → Start | first run; the official, guided path |
| **B. `tools/deploy.py` (CLI)** | one command | fast iteration during development |
| **C. Manual SSH + scp** | a few commands | debugging, capturing logs live |
| ~~ADB / Android~~ | — | APKs only — **not** this app |

- **Easiest to start:** Option A (the GUI does the runtime wiring for you).
- **Easiest to iterate:** Option B (one command builds nothing you didn't ask for
  and launches in the headset).

They all end up in the same place: *Steam ▸ Library ▸ Non-Steam ▸ **Devkit Game:
PixelVRPlayer***.

---

## Option A — SteamOS Devkit Client (GUI)

The official, no-scripting path.

1. Stage a clean upload folder so you don't upload the whole build tree:
   ```bash
   ./tools/stage.sh          # creates dist/PixelVRPlayer/{pixelvr, shaders/, launch.sh}
   ```
2. In the Devkit Client, **Connect to devkit by IP** = `frame` (or your IP), then
   open **Title Upload** and fill in:
   - **Name:** `PixelVRPlayer` — **required**, and must match
     `^[A-Za-z_][A-Za-z0-9_.]+$`: letters/digits/`_`/`.` only, **no spaces or
     hyphens**, starting with a letter or underscore. Leaving it blank (or using
     `Pixel VR Player`) fails with `Title name '' must match pattern ...`.
   - **Local Folder:** `dist/PixelVRPlayer`
   - **Start Command:** `pixelvr` — **required** (the binary's path relative to
     the Local Folder). An empty value fails with `start command is empty /
     missing!`. If launch later errors with "not found", use `./pixelvr`.
   - **Runtime:** `Steam Linux Runtime 3.0 ARM64 (Sniper)`
3. Click **Upload**, then **Start**.
4. To pass a video or environment variables, set them in the game's **Properties ▸
   Launch Options** in Steam. Arguments are appended; env vars use `%command%`:
   ```
   PIXELVR_LOG=debug %command% /home/steamos/Videos/clip.mp4
   ```

---

## Option B — One command (`tools/deploy.py`)

Best for the edit-build-run loop. Copies the binary, shaders and `launch.sh` into
`~/devkit-game/PixelVRPlayer/`, registers the Non-Steam shortcut, and (optionally)
launches it.

```bash
# Copy + register + launch, with a video argument:
./tools/deploy.sh --launch -- /home/steamos/Videos/clip_360_tb.mp4

# Just stage it (no launch):
./tools/deploy.sh

# Different headset:
./tools/deploy.sh --host steamos@192.168.1.50 --launch
```

Overrides: `FRAME_HOST` (default `steamos@frame`) and `FRAME_SSH_KEY`. If `frame`
doesn't resolve from WSL, pass the IP with `--host`.

This registers a shortcut that runs the binary **directly on the SteamOS host**
(see [Runtime choice](#runtime-choice-sniper-vs-on-host) below).

---

## Option C — Manual SSH (debugging)

Most control, and you see stdout/stderr live.

```bash
# Copy the staged folder over:
scp -r dist/PixelVRPlayer steamos@frame:~/

# Run it with the SteamVR OpenXR runtime selected:
ssh steamos@frame '~/PixelVRPlayer/launch.sh /home/steamos/Videos/clip.mp4'
```

`launch.sh` sets `XR_RUNTIME_JSON`, `LD_LIBRARY_PATH` and `PIXELVR_SHADER_DIR` for
you. Note that OpenXR needs the SteamVR runtime/compositor to be up — if a plain
SSH run fails at XR init, launch it once from the Steam library instead (Option A
or B), which brings the runtime up.

---

## Runtime choice: Sniper vs on-host

There are two ways a native ARM64 Linux binary can run on the Frame, and they
differ in which system libraries it sees:

- **Steam Linux Runtime 3.0 ARM64 (Sniper)** — a containerized, stable library
  environment. This is what Valve's docs recommend and what the **GUI (Option A)**
  selects. Good default.
- **Directly on the host** — uses SteamOS's own FFmpeg/SteamVR libraries. This is
  what **`deploy.py` (Option B)** registers, matching how a shipping native OpenXR
  player runs.

Both expose the SteamVR OpenXR runtime, so both can work. If the app fails to load
a library under one, try the other.

---

## The library-compatibility gotcha (important)

The quick `--aarch64` build links against **Ubuntu's arm64 FFmpeg**
(`libavcodec.so.60`). SteamOS is Arch-based and may ship a different FFmpeg
version, so on-device you might see:

```
error while loading shared libraries: libavcodec.so.60: cannot open shared object file
```

**Fix — build against the headset's own libraries:**

```bash
./tools/fetch-sysroot.sh steamos@frame          # rsyncs the device libs into ./sysroot
PIXELVR_SYSROOT="$PWD/sysroot" ./tools/build.sh --aarch64
```

This produces a binary linked against the exact SONAMEs the Frame provides, and
also **restores audio** (the quick multiarch build omits PulseAudio because its
glib dev chain isn't co-installable across architectures).

---

## Preparing test clips

Projection and stereo layout are auto-detected from the **filename**:

| Filename contains | Projection | Stereo |
|-------------------|------------|--------|
| `360`, or `_sbs`/`_lr` | 360° | side-by-side |
| `180`/`vr180`, or `_tb`/`_ou` | 180° | top/bottom |
| (none of the above) | flat 2D | mono |

Example: `nature_360_tb.mp4` → 360° top/bottom stereo.

Requirements:
- **8-bit H.264 or HEVC**, even width/height (4:2:0). 10-bit/Main10 is not
  supported by the hardware decoder path.
- `PIXELVR_FORCE_SW=1` forces software decode (debugging).
- `PIXELVR_LOG=debug` raises log verbosity.

---

## Troubleshooting

| Symptom | Likely cause / fix |
|---------|--------------------|
| `Title name '' must match pattern ^[A-Za-z_]...` | The **Name** field is empty or has spaces/hyphens. Use a bare token like `PixelVRPlayer`. |
| `start command is empty / missing!` | The **Start Command** field is blank. Set it to `pixelvr` (binary path relative to the Local Folder). |
| Headset not listed in Devkit Client | Same LAN + mDNS required; use **Connect by IP** = `frame` or the IP. |
| `ssh: Could not resolve hostname frame` | Use the IP: `--host steamos@<ip>`. |
| `Permission denied (publickey)` | Finish **Pair new host** + Register; point tools at `devkit_rsa` (see setup note). |
| `libavcodec.so.60: cannot open shared object file` | FFmpeg SONAME mismatch — do the [sysroot build](#the-library-compatibility-gotcha-important). |
| XR init fails / `RUNTIME_UNAVAILABLE` over SSH | Launch from the Steam library once so SteamVR is running. |
| `failed to open shader` | Ensure `shaders/` sits next to the binary, or set `PIXELVR_SHADER_DIR`. |
| Black view, no video | Clip must be 8-bit H.264/HEVC, even dimensions; check `PIXELVR_LOG=debug`. |
| No audio | Expected on the quick multiarch build; use the sysroot build. |

---

## References

- [Setting up your Steam Frame for development](https://partner.steamgames.com/doc/steamhardware/steamframe/setup)
- [How to load and run games on Steam Frame](https://partner.steamgames.com/doc/steamhardware/steamframe/loadgames)
- [How to load and run games on Steam Deck/Machine](https://partner.steamgames.com/doc/steamhardware/loadgames) (Devkit Client details)
