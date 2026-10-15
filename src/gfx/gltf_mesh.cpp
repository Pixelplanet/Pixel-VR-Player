#include "gfx/gltf_mesh.hpp"

#include "util/logging.hpp"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

namespace pixelvr {

GltfMesh loadGlb(const std::vector<uint8_t>& glb) {
    GltfMesh out;
    if (glb.empty()) return out;

    cgltf_options opt{};
    cgltf_data* data = nullptr;
    if (cgltf_parse(&opt, glb.data(), glb.size(), &data) != cgltf_result_success) {
        PIXELVR_LOG_WARN("glTF: parse failed");
        return out;
    }
    if (cgltf_load_buffers(&opt, data, nullptr) != cgltf_result_success) {
        PIXELVR_LOG_WARN("glTF: load_buffers failed");
        cgltf_free(data);
        return out;
    }

    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        cgltf_node* node = &data->nodes[n];
        if (node->mesh == nullptr) continue;

        cgltf_float world[16];
        cgltf_node_transform_world(node, world);  // bakes the node hierarchy

        for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
            const cgltf_primitive* prim = &node->mesh->primitives[p];
            if (prim->type != cgltf_primitive_type_triangles ||
                prim->indices == nullptr) {
                continue;
            }
            const cgltf_accessor* pos = nullptr;
            const cgltf_accessor* nor = nullptr;
            for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
                if (prim->attributes[a].type == cgltf_attribute_type_position) {
                    pos = prim->attributes[a].data;
                } else if (prim->attributes[a].type == cgltf_attribute_type_normal) {
                    nor = prim->attributes[a].data;
                }
            }
            if (pos == nullptr) continue;

            const uint32_t base = static_cast<uint32_t>(out.vertices.size() / 6);
            for (cgltf_size v = 0; v < pos->count; ++v) {
                float p3[3] = {0.0f, 0.0f, 0.0f};
                float n3[3] = {0.0f, 0.0f, 1.0f};
                cgltf_accessor_read_float(pos, v, p3, 3);
                if (nor != nullptr) cgltf_accessor_read_float(nor, v, n3, 3);
                // Bake the node world transform into positions (full 4x4) and
                // normals (upper 3x3; render models have no non-uniform scale).
                out.vertices.push_back(world[0] * p3[0] + world[4] * p3[1] +
                                       world[8] * p3[2] + world[12]);
                out.vertices.push_back(world[1] * p3[0] + world[5] * p3[1] +
                                       world[9] * p3[2] + world[13]);
                out.vertices.push_back(world[2] * p3[0] + world[6] * p3[1] +
                                       world[10] * p3[2] + world[14]);
                out.vertices.push_back(world[0] * n3[0] + world[4] * n3[1] +
                                       world[8] * n3[2]);
                out.vertices.push_back(world[1] * n3[0] + world[5] * n3[1] +
                                       world[9] * n3[2]);
                out.vertices.push_back(world[2] * n3[0] + world[6] * n3[1] +
                                       world[10] * n3[2]);
            }
            for (cgltf_size i = 0; i < prim->indices->count; ++i) {
                out.indices.push_back(
                    base +
                    static_cast<uint32_t>(cgltf_accessor_read_index(prim->indices, i)));
            }
        }
    }

    cgltf_free(data);
    out.ok = !out.indices.empty();
    PIXELVR_LOG_INFO("glTF: %zu vertices, %zu indices", out.vertices.size() / 6,
                     out.indices.size());
    return out;
}

} // namespace pixelvr
