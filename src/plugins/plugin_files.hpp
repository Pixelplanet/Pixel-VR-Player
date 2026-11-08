#pragma once
// Small blocking file access for plugin sidecars (the .funscript next to a
// video): local paths, SMB (libsmbclient, serialised on smbGlobalLock), and
// SFTP / WebDAV through remote_fs. Called only from PluginHost's worker thread.

#include <cstddef>
#include <string>
#include <vector>

namespace pixelvr::pluginfiles {

// "smb://h/s/dir/a.mkv" -> "smb://h/s/dir"; "/x/a.mkv" -> "/x".
std::string parentOf(const std::string& path);
// "dir/My Movie.mkv" -> "My Movie" (last extension removed, query ignored).
std::string baseNameOf(const std::string& path);
std::string join(const std::string& dir, const std::string& name);

// Regular files directly inside `dir` (names only).
bool listFiles(const std::string& dir, std::vector<std::string>& names, std::string& err);
// Whole file, at most `maxBytes` (larger files fail).
bool readFile(const std::string& uri, std::size_t maxBytes, std::string& out, std::string& err);

// Sidecar patterns start with "{base}", the rest is a glob ("{base}.*.funscript");
// matching ignores case.
bool sidecarMatch(const std::string& pattern, const std::string& base, const std::string& name);

bool isUtf8(const std::string& s);
std::string base64(const std::string& s);

}  // namespace pixelvr::pluginfiles
