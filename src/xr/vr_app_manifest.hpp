#pragma once

namespace pixelvr {

// Makes a launch of this non-Steam shortcut look to SteamVR like a launch of
// one of Steam's own VR games: SteamVR shows its "Up next" transition and the
// dashboard closes by itself once we connect, instead of ending on
// "Resume Game".
//
// Steam runs IVRApplications::PerformApplicationPrelaunchCheck before starting
// its own VR games, but not for shortcuts. It also writes the shortcut's
// steamapps.vrmanifest entry with only "binary_path_linux", which SteamVR on the
// ARM Steam Frame rejects, so SteamVR does not know the app key at all. The
// helper registers a valid manifest for the same key (public
// IVRApplications::AddApplicationManifest) when needed, then runs the check.
//
// It is a short-lived child process (`pixelvr --steamvr-prelaunch`) without
// Steam's id variables, so SteamVR never takes its connection for the app.

// The helper's entry point (process exit code; logs what it did).
int runSteamVrPrelaunchHelper();

// Runs the helper and waits for it (at most 3 s). Call before the OpenXR
// instance is created. No-op when not launched by Steam.
void steamVrPrelaunch();

// `pixelvr --steamvr-unregister` (run by the uninstaller): removes our manifest
// from SteamVR's app list and deletes it. Settings stay.
int runSteamVrUnregister();

}  // namespace pixelvr
