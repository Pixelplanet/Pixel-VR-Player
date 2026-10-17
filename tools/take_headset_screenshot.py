#!/usr/bin/env python3
"""
Automated in-headset screenshot capture tool for Valve Steam Frame (SteamOS).
Uses OpenVR IVRScreenshots via SteamVR's running compositor to capture
both 2D preview and stereo VR framebuffers.
"""

import sys
import os
import time
import argparse
import subprocess
import base64
from datetime import datetime

DEFAULT_SSH_KEY = os.path.expanduser(r"~\AppData\Local\steamos-devkit\steamos-devkit\devkit_rsa")
DEFAULT_HOST = "steamos@192.168.178.61"

CPP_CODE = r"""#include <iostream>
#include <thread>
#include <chrono>
#include "/opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers/openvr.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <preview_png> <stereo_png>" << std::endl;
        return 1;
    }
    const char* preview = argv[1];
    const char* vr_file = argv[2];

    vr::EVRInitError err = vr::VRInitError_None;
    vr::IVRSystem* vrSys = vr::VR_Init(&err, vr::VRApplication_Overlay);
    if (!vrSys || err != vr::VRInitError_None) {
        std::cerr << "VR_Init failed: " << vr::VR_GetVRInitErrorAsEnglishDescription(err) << std::endl;
        return 1;
    }
    
    vr::IVRScreenshots* vrScreenshots = (vr::IVRScreenshots*)vr::VR_GetGenericInterface("IVRScreenshots_001", &err);
    if (!vrScreenshots || err != vr::VRInitError_None) {
        std::cerr << "GetGenericInterface IVRScreenshots_001 failed: " << err << std::endl;
        vr::VR_Shutdown();
        return 1;
    }

    vr::ScreenshotHandle_t handle = 0;
    vr::EVRScreenshotError scErr = vrScreenshots->TakeStereoScreenshot(&handle, preview, vr_file);
    if (scErr != vr::VRScreenshotError_None) {
        std::cerr << "TakeStereoScreenshot failed with error: " << scErr << std::endl;
        vr::VR_Shutdown();
        return 1;
    }
    std::cout << "Screenshot requested successfully, handle: " << handle << std::endl;

    // Allow compositor frame loop to flush the write
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    vr::VR_Shutdown();
    return 0;
}
"""

def main():
    parser = argparse.ArgumentParser(description="Capture in-headset screenshots from Valve Steam Frame")
    parser.add_argument("--host", default=DEFAULT_HOST, help="Steam Frame SSH destination")
    parser.add_argument("--key", default=DEFAULT_SSH_KEY, help="Path to devkit private key")
    parser.add_argument("--delay", type=int, default=0, help="Seconds to wait before taking screenshot")
    parser.add_argument("--out-dir", default="docs/screenshots", help="Directory to save downloaded screenshots")
    args = parser.parse_args()

    if args.delay > 0:
        print(f"Waiting {args.delay} seconds before capture (put headset on or position UI)...")
        time.sleep(args.delay)

    b64_cpp = base64.b64encode(CPP_CODE.encode('utf-8')).decode('utf-8')
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    remote_preview = f"/tmp/pixelvr_snap_{timestamp}_preview.png"
    remote_stereo = f"/tmp/pixelvr_snap_{timestamp}_stereo.png"

    remote_cmd = (
        f"if [ ! -f /tmp/pixelvr_screenshot_bin ]; then "
        f"  echo '{b64_cpp}' | base64 -d > /tmp/pixelvr_screenshot.cpp && "
        f"  g++ -O2 /tmp/pixelvr_screenshot.cpp -L/opt/steamvr/bin/linuxarm64 -lopenvr_api -Wl,-rpath,/opt/steamvr/bin/linuxarm64 -o /tmp/pixelvr_screenshot_bin ; "
        f"fi ; "
        f"/tmp/pixelvr_screenshot_bin {remote_preview} {remote_stereo}"
    )

    print(f"Triggering screenshot capture on {args.host}...")
    res = subprocess.run([
        "ssh", "-i", args.key, "-o", "StrictHostKeyChecking=no", args.host, remote_cmd
    ], capture_output=True, text=True)

    if res.returncode != 0:
        print("Error capturing screenshot:\n", res.stderr)
        sys.exit(1)

    print(res.stdout.strip())

    os.makedirs(args.out_dir, exist_ok=True)
    local_preview = os.path.join(args.out_dir, f"screenshot_{timestamp}_preview.png")
    local_stereo = os.path.join(args.out_dir, f"screenshot_{timestamp}_stereo.png")

    print(f"Downloading captured frames to {args.out_dir}...")
    subprocess.run(["scp", "-i", args.key, "-o", "StrictHostKeyChecking=no", f"{args.host}:{remote_preview}", local_preview], check=True)
    subprocess.run(["scp", "-i", args.key, "-o", "StrictHostKeyChecking=no", f"{args.host}:{remote_stereo}", local_stereo], check=True)

    print(f"Saved: {local_preview}")
    print(f"Saved: {local_stereo}")

if __name__ == "__main__":
    main()
