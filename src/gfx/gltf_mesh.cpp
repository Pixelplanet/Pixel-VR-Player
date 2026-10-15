#include "gfx/gltf_mesh.hpp"

#include "util/logging.hpp"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

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
            const cgltf_accessor* uv = nullptr;
            for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
                const cgltf_attribute& at = prim->attributes[a];
                if (at.type == cgltf_attribute_type_position) {
                    pos = at.data;
                } else if (at.type == cgltf_attribute_type_normal) {
                    nor = at.data;
                } else if (at.type == cgltf_attribute_type_texcoord &&
                           uv == nullptr) {
                    uv = at.data;
                }
            }
            if (pos == nullptr) continue;

            const uint32_t base = static_cast<uint32_t>(out.vertices.size() / 8);
            for (cgltf_size v = 0; v < pos->count; ++v) {
                float p3[3] = {0.0f, 0.0f, 0.0f};
                float n3[3] = {0.0f, 0.0f, 1.0f};
                float t2[2] = {0.0f, 0.0f};
                cgltf_accessor_read_float(pos, v, p3, 3);
                if (nor != nullptr) cgltf_accessor_read_float(nor, v, n3, 3);
                if (uv != nullptr) cgltf_accessor_read_float(uv, v, t2, 2);
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
                out.vertices.push_back(t2[0]);
                out.vertices.push_back(t2[1]);
            }
            GltfSubmesh sm;
            sm.nodeName = (node->name != nullptr) ? node->name : "";
            sm.firstIndex = static_cast<uint32_t>(out.indices.size());
            for (cgltf_size i = 0; i < prim->indices->count; ++i) {
                out.indices.push_back(
                    base +
                    static_cast<uint32_t>(cgltf_accessor_read_index(prim->indices, i)));
            }
            sm.indexCount =
                static_cast<uint32_t>(out.indices.size()) - sm.firstIndex;
            if (prim->material != nullptr &&
                prim->material->has_pbr_metallic_roughness) {
                const cgltf_pbr_metallic_roughness& pbr =
                    prim->material->pbr_metallic_roughness;
                for (int k = 0; k < 4; ++k)
                    sm.baseColor[k] = pbr.base_color_factor[k];
                const cgltf_texture* t = pbr.base_color_texture.texture;
                if (t != nullptr && t->image != nullptr) {
                    sm.image = static_cast<int>(t->image - data->images);
                }
            }
            out.submeshes.push_back(sm);
        }
    }

    // Decode the embedded base-color PNGs into RGBA8 (indexed to match glTF
    // image order, so submesh.image maps straight through).
    out.images.resize(data->images_count);
    for (cgltf_size i = 0; i < data->images_count; ++i) {
        const cgltf_image* im = &data->images[i];
        if (im->buffer_view == nullptr || im->buffer_view->buffer == nullptr ||
            im->buffer_view->buffer->data == nullptr) {
            continue;
        }
        const cgltf_buffer_view* bv = im->buffer_view;
        const stbi_uc* bytes =
            static_cast<const stbi_uc*>(bv->buffer->data) + bv->offset;
        int w = 0, h = 0, comp = 0;
        stbi_uc* px = stbi_load_from_memory(bytes, static_cast<int>(bv->size), &w,
                                            &h, &comp, 4);
        if (px != nullptr) {
            out.images[i].width = w;
            out.images[i].height = h;
            out.images[i].rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
            stbi_image_free(px);
        } else {
            PIXELVR_LOG_WARN("glTF: image %zu decode failed", i);
        }
    }

    cgltf_free(data);
    out.ok = !out.indices.empty();
    PIXELVR_LOG_INFO("glTF: %zu verts, %zu indices, %zu submeshes, %zu images",
                     out.vertices.size() / 8, out.indices.size(),
                     out.submeshes.size(), out.images.size());
    return out;
}

} // namespace pixelvr
