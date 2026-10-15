#pragma once

#include <cstdint>
#include <vector>

namespace pixelvr {

// A decoded RGBA8 base-color texture from the glTF (empty if decode failed).
struct GltfImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // width*height*4
};

// One contiguous draw range that shares a single material/texture.
struct GltfSubmesh {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int image = -1;  // index into GltfMesh::images, or -1 for untextured
    float baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

// A mesh extracted from a glTF/GLB asset: positions + normals + UVs interleaved,
// 32-bit indices, split into submeshes by material, plus the decoded base-color
// textures those submeshes sample.
struct GltfMesh {
    std::vector<float> vertices;  // interleaved: px,py,pz, nx,ny,nz, u,v
    std::vector<uint32_t> indices;
    std::vector<GltfSubmesh> submeshes;
    std::vector<GltfImage> images;
    bool ok = false;
};

// Parses a binary glTF (GLB) buffer, baking node transforms. Returns ok=false if
// nothing usable was found.
GltfMesh loadGlb(const std::vector<uint8_t>& glb);

} // namespace pixelvr
