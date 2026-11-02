#pragma once
// Shared-memory frame layout between PixelVR and the pvr-browser helper.
// The helper writes RGBA pixels (row stride = width * 4) after the header and
// bumps `seq`: odd while writing, even when a complete frame is ready.

#include <atomic>
#include <cstddef>
#include <cstdint>

constexpr int kBrowserMaxWidth = 1920;
constexpr int kBrowserMaxHeight = 1080;

struct BrowserShmHeader {
    char magic[8];                  // "PVRBRW1"
    uint32_t width;                 // of the last published frame
    uint32_t height;
    std::atomic<uint64_t> seq;
    uint8_t reserved[40];
};
static_assert(sizeof(BrowserShmHeader) == 64, "header must stay 64 bytes");

inline size_t browserShmSize(int width, int height) {
    return sizeof(BrowserShmHeader) + static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
}
