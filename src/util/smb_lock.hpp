#pragma once

#include <mutex>

namespace pixelvr {

// libsmbclient (Samba) is NOT thread-safe: it keeps process-global talloc, name
// cache, and connection state. Calling it from more than one thread at once (the
// file browser's directory scans, async downloads, and the media engine's SMB
// streaming prefetch) corrupts that state and aborts inside Samba name
// resolution (talloc_get_type_abort -> SIGABRT). Serialize EVERY libsmbclient
// entry point on this one process-wide lock. Hold it only around the actual
// libsmbclient calls (per read/seek chunk for streaming), never across waits, so
// the threads still interleave.
inline std::mutex& smbGlobalLock() {
    static std::mutex lock;
    return lock;
}

} // namespace pixelvr
