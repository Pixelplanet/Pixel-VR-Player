#pragma once

#include <cstdint>
#include <vector>

namespace pixelvr {

// A merged mesh extracted from a glTF/GLB asset: positions + normals interleaved,
// plus 32-bit indices (the controller model's combined vertex count exceeds 16
// bits). Materials/textures are ignored (the model is flat-shaded).
struct GltfMesh {
    std::vector<float> vertices;    // interleaved: px,py,pz, nx,ny,nz
    std::vector<uint32_t> indices;
    bool ok = false;
};

// Parses a binary glTF (GLB) buffer, merging every triangle primitive into one
// mesh with baked node transforms. Returns ok=false if nothing usable was found.
GltfMesh loadGlb(const std::vector<uint8_t>& glb);

} // namespace pixelvr
