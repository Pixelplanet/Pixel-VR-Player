#pragma once

#include "xr/openxr_headers.hpp"

#include <cstdint>
#include <vector>

namespace pixelvr {

// Fetches (and, in later stages, renders) the runtime-provided controller models
// via XR_EXT_render_model. Stage 2 scope: resolve the extension entry points,
// enumerate the active interaction render models, and pull the first one's
// glTF/GLB asset bytes so the renderer can parse and draw it.
class RenderModelSystem {
public:
    // Resolves the extension function pointers. `enabled` is whether the three
    // render-model extensions were enabled at instance creation.
    bool init(XrInstance instance, XrSession session, bool enabled);

    bool available() const { return available_; }

    // Lazily enumerates + fetches the first controller model's glTF the first
    // time a controller is active. Safe to call every frame. Returns true once
    // the asset bytes are ready.
    bool poll();

    bool assetReady() const { return !gltf_.empty(); }
    const std::vector<uint8_t>& gltf() const { return gltf_; }
    uint32_t animatableNodeCount() const { return animatableNodeCount_; }

    void shutdown();

private:
    bool fetchFirstModel();

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSession session_ = XR_NULL_HANDLE;
    bool available_ = false;
    bool done_ = false;  // asset fetched (or permanently failed)

    XrRenderModelEXT model_ = XR_NULL_HANDLE;
    XrRenderModelAssetEXT asset_ = XR_NULL_HANDLE;
    XrSpace modelSpace_ = XR_NULL_HANDLE;
    std::vector<uint8_t> gltf_;
    uint32_t animatableNodeCount_ = 0;

    PFN_xrEnumerateInteractionRenderModelIdsEXT pfnEnumerateIds_ = nullptr;
    PFN_xrCreateRenderModelEXT pfnCreateModel_ = nullptr;
    PFN_xrGetRenderModelPropertiesEXT pfnGetProps_ = nullptr;
    PFN_xrCreateRenderModelAssetEXT pfnCreateAsset_ = nullptr;
    PFN_xrGetRenderModelAssetDataEXT pfnGetAssetData_ = nullptr;
    PFN_xrCreateRenderModelSpaceEXT pfnCreateSpace_ = nullptr;
    PFN_xrGetRenderModelStateEXT pfnGetState_ = nullptr;
    PFN_xrDestroyRenderModelEXT pfnDestroyModel_ = nullptr;
    PFN_xrDestroyRenderModelAssetEXT pfnDestroyAsset_ = nullptr;
};

} // namespace pixelvr
