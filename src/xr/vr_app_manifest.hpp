#pragma once

namespace pixelvr {

// Makes SteamVR know this non-Steam shortcut as a VR app, so a launch from the
// Steam library gets SteamVR's "Up next" transition (and the dashboard closes by
// itself) instead of ending on "Resume Game".
//
// Steam generates the shortcut's entry in steamapps.vrmanifest with only
// "binary_path_linux"; SteamVR on the ARM Steam Frame wants
// "binary_path_linux_arm" and skips the entry, so Steam's prelaunch check never
// finds the app. This registers a valid manifest for the same app key through
// the public IVRApplications::AddApplicationManifest.
//
// Runs as a short-lived child process (`pixelvr --register-vr-manifest`, started
// by App::init) so the player never holds an extra OpenVR connection. Returns
// a process exit code; logs what it did.
int registerVrAppManifest();

// Starts the helper above in the background (no-op outside a Steam launch).
void spawnVrAppManifestRegistration();

}  // namespace pixelvr
