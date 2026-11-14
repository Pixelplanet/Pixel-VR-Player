// A second, light reader on the file that is playing: used to fetch the subtitles around a seek
// target without touching playback's own reader. Opens local files and web addresses directly,
// vault files through the decrypting reader and files on a network share over their own connection.
#pragma once

#include "media/media_engine.hpp"
#include "media/vault_avio.hpp"
#include "net/remote_fs.hpp"

#include <memory>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

namespace pixelvr {

struct SubtitleCatchUpSource {
    AVFormatContext* fmt = nullptr;
    std::unique_ptr<VaultAvio> vaultIo;  // (outlives fmt)
    // A file on a share: read straight from its connection (a few seeks and short reads).
    std::unique_ptr<RemoteFile> remote;
    int64_t remotePos = 0;
    AVIOContext* remoteCtx = nullptr;

    ~SubtitleCatchUpSource() { close(); }

    void close() {
        if (fmt != nullptr) avformat_close_input(&fmt);
        if (remoteCtx != nullptr) {
            av_freep(&remoteCtx->buffer);
            avio_context_free(&remoteCtx);
        }
        vaultIo.reset();
        remote.reset();
    }

    static int remoteRead(void* opaque, uint8_t* buf, int size) {
        auto* s = static_cast<SubtitleCatchUpSource*>(opaque);
        const int n = s->remote->read(buf, size);
        if (n < 0) return AVERROR(EIO);
        if (n == 0) return AVERROR_EOF;
        s->remotePos += n;
        return n;
    }

    static int64_t remoteSeek(void* opaque, int64_t offset, int whence) {
        auto* s = static_cast<SubtitleCatchUpSource*>(opaque);
        const int64_t size = s->remote->size();
        if (whence & AVSEEK_SIZE) return size >= 0 ? size : AVERROR(ENOSYS);
        int64_t p;
        switch (whence & ~AVSEEK_FORCE) {
            case SEEK_SET: p = offset; break;
            case SEEK_CUR: p = s->remotePos + offset; break;
            case SEEK_END: if (size < 0) return -1; p = size + offset; break;
            default: return -1;
        }
        if (p < 0 || !s->remote->seek(p)) return -1;
        s->remotePos = p;
        return p;
    }

    // `interrupt` (with `opaque`) lets the owner break off a read that is under way.
    bool open(const std::string& path, int (*interrupt)(void*), void* opaque) {
        close();
        if (vaultcrypto::isEncrypted(path)) {
            if (!openVaultFormat(path, fmt, vaultIo)) {
                fmt = nullptr;
                return false;
            }
        } else if (isRemotePath(path)) {
            std::string err;
            remote = openRemoteMedia(path, err);
            constexpr int kBuf = 1 << 16;
            auto* buf = remote ? static_cast<unsigned char*>(av_malloc(kBuf)) : nullptr;
            remoteCtx = buf ? avio_alloc_context(buf, kBuf, 0, this, &remoteRead, nullptr, &remoteSeek) : nullptr;
            if (remoteCtx == nullptr) {
                av_free(buf);
                close();
                return false;
            }
            fmt = avformat_alloc_context();
            if (fmt == nullptr) {
                close();
                return false;
            }
            fmt->pb = remoteCtx;
            fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
            fmt->interrupt_callback.callback = interrupt;
            fmt->interrupt_callback.opaque = opaque;
            if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
                fmt = nullptr;  // (freed by avformat_open_input)
                close();
                return false;
            }
        } else {
            fmt = avformat_alloc_context();
            if (fmt == nullptr) return false;
            fmt->interrupt_callback.callback = interrupt;
            fmt->interrupt_callback.opaque = opaque;
            if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
                fmt = nullptr;
                return false;
            }
        }
        fmt->interrupt_callback.callback = interrupt;
        fmt->interrupt_callback.opaque = opaque;
        return true;
    }
};

}  // namespace pixelvr
