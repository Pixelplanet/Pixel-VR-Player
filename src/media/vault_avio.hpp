// FFmpeg input for encrypted vault files: an AVIOContext that decrypts through vaultcrypto::Reader.
// Used by every place that opens media (playback, scrub preview, thumbnails).
#pragma once

#include "app/vault_crypto.hpp"

#include <memory>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

namespace pixelvr {

struct VaultAvio {
    vaultcrypto::Reader reader;
    uint64_t pos = 0;
    AVIOContext* ctx = nullptr;
    ~VaultAvio() {
        if (ctx) {
            av_freep(&ctx->buffer);
            avio_context_free(&ctx);
        }
    }
};

inline int vaultAvioRead(void* opaque, uint8_t* buf, int size) {
    auto* v = static_cast<VaultAvio*>(opaque);
    const long n = v->reader.read(v->pos, buf, static_cast<size_t>(size));
    if (n < 0) return AVERROR(EIO);
    if (n == 0) return AVERROR_EOF;
    v->pos += static_cast<uint64_t>(n);
    return static_cast<int>(n);
}

inline int64_t vaultAvioSeek(void* opaque, int64_t offset, int whence) {
    auto* v = static_cast<VaultAvio*>(opaque);
    const int64_t size = static_cast<int64_t>(v->reader.size());
    if (whence & AVSEEK_SIZE) return size;
    int64_t p;
    switch (whence & ~AVSEEK_FORCE) {
        case SEEK_SET: p = offset; break;
        case SEEK_CUR: p = static_cast<int64_t>(v->pos) + offset; break;
        case SEEK_END: p = size + offset; break;
        default: return -1;
    }
    if (p < 0) return -1;
    v->pos = static_cast<uint64_t>(p);
    return p;
}

// Opens `path` (an encrypted vault file) into `fmt`. `holder` must outlive the AVFormatContext:
// close the context first (avformat_close_input), then reset the holder.
inline bool openVaultFormat(const std::string& path, AVFormatContext*& fmt, std::unique_ptr<VaultAvio>& holder) {
    holder = std::make_unique<VaultAvio>();
    if (!holder->reader.open(path)) {
        holder.reset();
        return false;
    }
    constexpr int kBuf = 1 << 18;
    auto* buf = static_cast<unsigned char*>(av_malloc(kBuf));
    holder->ctx = buf ? avio_alloc_context(buf, kBuf, 0, holder.get(), &vaultAvioRead, nullptr, &vaultAvioSeek) : nullptr;
    if (!holder->ctx) {
        av_free(buf);
        holder.reset();
        return false;
    }
    fmt = avformat_alloc_context();
    if (!fmt) {
        holder.reset();
        return false;
    }
    fmt->pb = holder->ctx;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    // The original name helps FFmpeg pick the container.
    if (avformat_open_input(&fmt, holder->reader.name().c_str(), nullptr, nullptr) < 0) {
        fmt = nullptr;  // freed by avformat_open_input on failure
        holder.reset();
        return false;
    }
    return true;
}

}  // namespace pixelvr
