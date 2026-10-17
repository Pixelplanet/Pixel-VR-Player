#pragma once

#include "xr/openxr_headers.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pixelvr {

// Fetches (and, in later stages, renders) the runtime-provided controller models
// via XR_EXT_render_model for both left (0) and right (1) hands.
class RenderModelSystem {
public:
    // Resolves the extension function pointers. `enabled` is whether the three
    // render-model extensions were enabled at instance creation.
    bool init(XrInstance instance, XrSession session, bool enabled);

    bool available() const { return available_; }

    // Lazily enumerates + fetches controller models for both hands.
    // Safe to call every frame.
    bool poll();

    bool assetReady(int hand = 1) const {
        return hand >= 0 && hand < 2 && models_[hand].ready && !models_[hand].gltf.empty();
    }
    const std::vector<uint8_t>& gltf(int hand = 1) const {
        static const std::vector<uint8_t> kEmpty;
        return (hand >= 0 && hand < 2) ? models_[hand].gltf : kEmpty;
    }
    uint32_t animatableNodeCount(int hand = 1) const {
        return (hand >= 0 && hand < 2) ? models_[hand].animatableNodeCount : 0;
    }

    // Locates the render-model space in `base` at `time` for `hand` (0 = left, 1 = right).
    bool locate(int hand, XrSpace base, XrTime time, float outPos[3], float outQuat[4]) const;
    bool locate(XrSpace base, XrTime time, float outPos[3], float outQuat[4]) const {
        return locate(1, base, time, outPos, outQuat);
    }

    // Names of the model's animatable nodes, in the same order as the node
    // states returned by nodeStates() (maps a node state to glTF geometry).
    const std::vector<std::string>& nodeNames(int hand = 1) const {
        static const std::vector<std::string> kEmpty;
        return (hand >= 0 && hand < 2) ? models_[hand].nodeNames : kEmpty;
    }

    // Animated per-node transform the runtime wants applied this frame (drives
    // button-press movement, trigger pull, thumbstick tilt, etc.).
    struct NodeState {
        float pos[3] = {0.0f, 0.0f, 0.0f};
        float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        bool visible = true;
    };
    // Fills `out` (parallel to nodeNames()) for `hand` at `time`. False if unavailable.
    bool nodeStates(int hand, XrTime time, std::vector<NodeState>& out);
    bool nodeStates(XrTime time, std::vector<NodeState>& out) {
        return nodeStates(1, time, out);
    }

    void shutdown();

private:
    bool fetchModels();

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSession session_ = XR_NULL_HANDLE;
    bool available_ = false;
    bool done_ = false;  // all models fetched (or permanently failed)

    struct ModelInfo {
        XrRenderModelIdEXT id = XR_NULL_RENDER_MODEL_ID_EXT;
        XrRenderModelEXT model = XR_NULL_HANDLE;
        XrRenderModelAssetEXT asset = XR_NULL_HANDLE;
        XrSpace space = XR_NULL_HANDLE;
        std::vector<uint8_t> gltf;
        std::vector<std::string> nodeNames;
        std::vector<XrRenderModelNodeStateEXT> nodeStateBuf;  // reused per frame
        uint32_t animatableNodeCount = 0;
        bool ready = false;
    };
    ModelInfo models_[2];  // 0 = left, 1 = right

    PFN_xrEnumerateInteractionRenderModelIdsEXT pfnEnumerateIds_ = nullptr;
    PFN_xrEnumerateRenderModelSubactionPathsEXT pfnEnumerateSubactionPaths_ = nullptr;
    PFN_xrCreateRenderModelEXT pfnCreateModel_ = nullptr;
    PFN_xrGetRenderModelPropertiesEXT pfnGetProps_ = nullptr;
    PFN_xrCreateRenderModelAssetEXT pfnCreateAsset_ = nullptr;
    PFN_xrGetRenderModelAssetDataEXT pfnGetAssetData_ = nullptr;
    PFN_xrGetRenderModelAssetPropertiesEXT pfnGetAssetProps_ = nullptr;
    PFN_xrCreateRenderModelSpaceEXT pfnCreateSpace_ = nullptr;
    PFN_xrGetRenderModelStateEXT pfnGetState_ = nullptr;
    PFN_xrDestroyRenderModelEXT pfnDestroyModel_ = nullptr;
    PFN_xrDestroyRenderModelAssetEXT pfnDestroyAsset_ = nullptr;
};

} // namespace pixelvr
