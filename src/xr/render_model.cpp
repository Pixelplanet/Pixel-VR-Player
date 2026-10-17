#include "xr/render_model.hpp"

#include "util/logging.hpp"
#include "xr/xr_result.hpp"

#include <cstdio>

namespace pixelvr {

bool RenderModelSystem::init(XrInstance instance, XrSession session, bool enabled) {
    instance_ = instance;
    session_ = session;
    if (!enabled) {
        return false;
    }
    auto load = [&](const char* name, auto& fn) {
        return XR_SUCCEEDED(xrGetInstanceProcAddr(
                   instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn))) &&
               fn != nullptr;
    };
    available_ =
        load("xrEnumerateInteractionRenderModelIdsEXT", pfnEnumerateIds_) &&
        load("xrCreateRenderModelEXT", pfnCreateModel_) &&
        load("xrGetRenderModelPropertiesEXT", pfnGetProps_) &&
        load("xrCreateRenderModelAssetEXT", pfnCreateAsset_) &&
        load("xrGetRenderModelAssetDataEXT", pfnGetAssetData_) &&
        load("xrGetRenderModelAssetPropertiesEXT", pfnGetAssetProps_) &&
        load("xrCreateRenderModelSpaceEXT", pfnCreateSpace_) &&
        load("xrGetRenderModelStateEXT", pfnGetState_) &&
        load("xrDestroyRenderModelEXT", pfnDestroyModel_) &&
        load("xrDestroyRenderModelAssetEXT", pfnDestroyAsset_);

    // Optional helper to map models to hands directly:
    load("xrEnumerateRenderModelSubactionPathsEXT", pfnEnumerateSubactionPaths_);

    if (!available_) {
        PIXELVR_LOG_WARN("XR_EXT_render_model entry points unavailable");
    }
    return available_;
}

bool RenderModelSystem::poll() {
    if (!available_ || done_) {
        return assetReady(0) || assetReady(1);
    }
    fetchModels();
    return assetReady(0) || assetReady(1);
}

bool RenderModelSystem::fetchModels() {
    XrInteractionRenderModelIdsEnumerateInfoEXT enumInfo{
        XR_TYPE_INTERACTION_RENDER_MODEL_IDS_ENUMERATE_INFO_EXT};
    uint32_t idCount = 0;
    if (XR_FAILED(pfnEnumerateIds_(session_, &enumInfo, 0, &idCount, nullptr)) ||
        idCount == 0) {
        return false;  // controllers not bound yet; retry next poll
    }
    std::vector<XrRenderModelIdEXT> ids(idCount, XR_NULL_RENDER_MODEL_ID_EXT);
    if (XR_FAILED(
            pfnEnumerateIds_(session_, &enumInfo, idCount, &idCount, ids.data()))) {
        return false;
    }

    PIXELVR_LOG_INFO("OpenXR enumerated %u render model IDs", idCount);

    for (uint32_t i = 0; i < idCount; ++i) {
        XrRenderModelIdEXT id = ids[i];
        if (id == XR_NULL_RENDER_MODEL_ID_EXT) continue;

        if ((models_[0].ready && models_[0].id == id) ||
            (models_[1].ready && models_[1].id == id)) {
            continue;
        }

        XrRenderModelCreateInfoEXT mci{XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT};
        mci.renderModelId = id;
        XrRenderModelEXT model = XR_NULL_HANDLE;
        if (XR_FAILED(pfnCreateModel_(session_, &mci, &model))) {
            PIXELVR_LOG_WARN("xrCreateRenderModelEXT failed for id %llu",
                             static_cast<unsigned long long>(id));
            continue;
        }

        int hand = -1;
        if (pfnEnumerateSubactionPaths_ != nullptr) {
            XrInteractionRenderModelSubactionPathInfoEXT spi{
                XR_TYPE_INTERACTION_RENDER_MODEL_SUBACTION_PATH_INFO_EXT};
            uint32_t pathCount = 0;
            if (XR_SUCCEEDED(pfnEnumerateSubactionPaths_(model, &spi, 0, &pathCount, nullptr)) && pathCount > 0) {
                std::vector<XrPath> paths(pathCount);
                if (XR_SUCCEEDED(pfnEnumerateSubactionPaths_(model, &spi, pathCount, &pathCount, paths.data()))) {
                    for (XrPath p : paths) {
                        char pathBuf[XR_MAX_PATH_LENGTH];
                        uint32_t len = 0;
                        if (XR_SUCCEEDED(xrPathToString(instance_, p, sizeof(pathBuf), &len, pathBuf))) {
                            std::string ps = pathBuf;
                            if (ps.find("left") != std::string::npos) hand = 0;
                            else if (ps.find("right") != std::string::npos) hand = 1;
                        }
                    }
                }
            }
        }

        XrRenderModelPropertiesGetInfoEXT pgi{
            XR_TYPE_RENDER_MODEL_PROPERTIES_GET_INFO_EXT};
        XrRenderModelPropertiesEXT props{XR_TYPE_RENDER_MODEL_PROPERTIES_EXT};
        if (XR_FAILED(pfnGetProps_(model, &pgi, &props))) {
            PIXELVR_LOG_WARN("xrGetRenderModelPropertiesEXT failed");
            pfnDestroyModel_(model);
            continue;
        }

        XrRenderModelAssetCreateInfoEXT aci{XR_TYPE_RENDER_MODEL_ASSET_CREATE_INFO_EXT};
        aci.cacheId = props.cacheId;
        XrRenderModelAssetEXT asset = XR_NULL_HANDLE;
        if (XR_FAILED(pfnCreateAsset_(session_, &aci, &asset))) {
            PIXELVR_LOG_WARN("xrCreateRenderModelAssetEXT failed");
            pfnDestroyModel_(model);
            continue;
        }

        XrRenderModelAssetDataGetInfoEXT adi{
            XR_TYPE_RENDER_MODEL_ASSET_DATA_GET_INFO_EXT};
        XrRenderModelAssetDataEXT data{XR_TYPE_RENDER_MODEL_ASSET_DATA_EXT};
        if (XR_FAILED(pfnGetAssetData_(asset, &adi, &data))) {  // size query
            PIXELVR_LOG_WARN("xrGetRenderModelAssetDataEXT (size) failed");
            pfnDestroyAsset_(asset);
            pfnDestroyModel_(model);
            continue;
        }
        std::vector<uint8_t> gltf(data.bufferCountOutput);
        data.bufferCapacityInput = static_cast<uint32_t>(gltf.size());
        data.buffer = gltf.data();
        if (XR_FAILED(pfnGetAssetData_(asset, &adi, &data))) {
            PIXELVR_LOG_WARN("xrGetRenderModelAssetDataEXT (data) failed");
            pfnDestroyAsset_(asset);
            pfnDestroyModel_(model);
            continue;
        }

        // If hand is still undetermined, inspect glTF JSON header
        if (hand < 0 && gltf.size() > 50) {
            std::string gltfText(reinterpret_cast<const char*>(gltf.data()),
                                 std::min<std::size_t>(gltf.size(), 4096));
            if (gltfText.find("left") != std::string::npos ||
                gltfText.find("Left") != std::string::npos) {
                hand = 0;
            } else if (gltfText.find("right") != std::string::npos ||
                       gltfText.find("Right") != std::string::npos) {
                hand = 1;
            }
        }

        // Fallback: assign to the first unfilled hand slot
        if (hand < 0) {
            hand = !models_[0].ready ? 0 : 1;
        }

        XrRenderModelSpaceCreateInfoEXT sci{XR_TYPE_RENDER_MODEL_SPACE_CREATE_INFO_EXT};
        sci.renderModel = model;
        XrSpace modelSpace = XR_NULL_HANDLE;
        pfnCreateSpace_(session_, &sci, &modelSpace);

        std::vector<std::string> nodeNames;
        if (pfnGetAssetProps_ != nullptr) {
            XrRenderModelAssetPropertiesGetInfoEXT agi{
                XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_GET_INFO_EXT};
            XrRenderModelAssetPropertiesEXT aprops{
                XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_EXT};
            if (XR_SUCCEEDED(pfnGetAssetProps_(asset, &agi, &aprops))) {
                std::vector<XrRenderModelAssetNodePropertiesEXT> names(
                    aprops.nodePropertyCount);
                aprops.nodePropertyCount = static_cast<uint32_t>(names.size());
                aprops.nodeProperties = names.data();
                if (XR_SUCCEEDED(pfnGetAssetProps_(asset, &agi, &aprops))) {
                    nodeNames.reserve(names.size());
                    for (const auto& np : names) nodeNames.emplace_back(np.uniqueName);
                }
            }
        }

        ModelInfo& mi = models_[hand];
        mi.id = id;
        mi.model = model;
        mi.asset = asset;
        mi.space = modelSpace;
        mi.gltf = std::move(gltf);
        mi.animatableNodeCount = props.animatableNodeCount;
        mi.nodeNames = std::move(nodeNames);
        mi.ready = true;

        PIXELVR_LOG_INFO("Render model for %s hand fetched: %u nodes, glTF %zu bytes",
                         hand == 0 ? "left" : "right",
                         mi.animatableNodeCount, mi.gltf.size());
    }

    if (models_[0].ready && models_[1].ready) {
        done_ = true;
    }
    return models_[0].ready || models_[1].ready;
}

bool RenderModelSystem::locate(int hand, XrSpace base, XrTime time, float outPos[3],
                               float outQuat[4]) const {
    if (hand < 0 || hand >= 2) return false;
    const XrSpace space = models_[hand].space;
    if (space == XR_NULL_HANDLE || time == 0) return false;
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(space, base, time, &loc))) return false;
    constexpr XrSpaceLocationFlags need =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
        XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return false;
    outPos[0] = loc.pose.position.x;
    outPos[1] = loc.pose.position.y;
    outPos[2] = loc.pose.position.z;
    outQuat[0] = loc.pose.orientation.x;
    outQuat[1] = loc.pose.orientation.y;
    outQuat[2] = loc.pose.orientation.z;
    outQuat[3] = loc.pose.orientation.w;
    return true;
}

bool RenderModelSystem::nodeStates(int hand, XrTime time, std::vector<NodeState>& out) {
    if (hand < 0 || hand >= 2) return false;
    ModelInfo& mi = models_[hand];
    if (!available_ || mi.model == XR_NULL_HANDLE || pfnGetState_ == nullptr ||
        time == 0) {
        return false;
    }
    if (mi.nodeStateBuf.size() < mi.animatableNodeCount) {
        mi.nodeStateBuf.resize(mi.animatableNodeCount);
    }
    XrRenderModelStateGetInfoEXT gi{XR_TYPE_RENDER_MODEL_STATE_GET_INFO_EXT};
    gi.displayTime = time;
    XrRenderModelStateEXT st{XR_TYPE_RENDER_MODEL_STATE_EXT};
    st.nodeStateCount = static_cast<uint32_t>(mi.nodeStateBuf.size());
    st.nodeStates = mi.nodeStateBuf.data();
    if (XR_FAILED(pfnGetState_(mi.model, &gi, &st))) {
        return false;
    }
    if (st.nodeStateCount > mi.nodeStateBuf.size()) {  // grew; retry once
        mi.nodeStateBuf.resize(st.nodeStateCount);
        st.nodeStateCount = static_cast<uint32_t>(mi.nodeStateBuf.size());
        st.nodeStates = mi.nodeStateBuf.data();
        if (XR_FAILED(pfnGetState_(mi.model, &gi, &st))) {
            return false;
        }
    }
    out.resize(st.nodeStateCount);
    for (uint32_t i = 0; i < st.nodeStateCount; ++i) {
        const XrRenderModelNodeStateEXT& n = mi.nodeStateBuf[i];
        NodeState& o = out[i];
        o.pos[0] = n.nodePose.position.x;
        o.pos[1] = n.nodePose.position.y;
        o.pos[2] = n.nodePose.position.z;
        o.quat[0] = n.nodePose.orientation.x;
        o.quat[1] = n.nodePose.orientation.y;
        o.quat[2] = n.nodePose.orientation.z;
        o.quat[3] = n.nodePose.orientation.w;
        o.visible = n.isVisible == XR_TRUE;
    }
    return true;
}

void RenderModelSystem::shutdown() {
    for (int h = 0; h < 2; ++h) {
        ModelInfo& mi = models_[h];
        if (mi.space != XR_NULL_HANDLE) {
            xrDestroySpace(mi.space);
            mi.space = XR_NULL_HANDLE;
        }
        if (mi.asset != XR_NULL_HANDLE && pfnDestroyAsset_ != nullptr) {
            pfnDestroyAsset_(mi.asset);
            mi.asset = XR_NULL_HANDLE;
        }
        if (mi.model != XR_NULL_HANDLE && pfnDestroyModel_ != nullptr) {
            pfnDestroyModel_(mi.model);
            mi.model = XR_NULL_HANDLE;
        }
        mi.ready = false;
    }
}

} // namespace pixelvr
