#pragma once
// Telling 3D / VR layouts apart when the file name doesn't say: from the
// container's stereo / spherical metadata, or from the picture itself.

struct AVStream;
struct AVFrame;

namespace pixelvr {

// Values follow VulkanRenderer's modes; -1 = unknown.
struct FormatHint {
    int projection = -1;  // 0 flat, 1 360, 2 180
    int stereo = -1;      // 0 mono, 1 side-by-side, 2 top-bottom
};

// MKV StereoMode / MP4 st3d+sv3d, as FFmpeg exports them.
FormatHint formatFromMetadata(const AVStream* stream);

// Compares the frame's halves: a stereo pair's halves are nearly the same
// picture (shifted by the eyes' disparity), a normal frame's are not.
// Returns 0 mono, 1 side-by-side, 2 top-bottom, -1 too little detail to tell.
// `score` (optional) receives {side-by-side, top-bottom} similarity scores.
int stereoFromPicture(const AVFrame* frame, float* scores = nullptr);

}  // namespace pixelvr
