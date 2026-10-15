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
    if (!available_) {
        PIXELVR_LOG_WARN("XR_EXT_render_model entry points unavailable");
    }
    return available_;
}

bool RenderModelSystem::poll() {
    if (!available_ || done_) {
        return assetReady();
    }
    fetchFirstModel();
    return assetReady();
}

bool RenderModelSystem::fetchFirstModel() {
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

    XrRenderModelIdEXT id = XR_NULL_RENDER_MODEL_ID_EXT;
    for (auto v : ids) {
        if (v != XR_NULL_RENDER_MODEL_ID_EXT) {
            id = v;
            break;
        }
    }
    if (id == XR_NULL_RENDER_MODEL_ID_EXT) {
        return false;
    }
    done_ = true;  // from here on, success or hard failure — do not retry

    XrRenderModelCreateInfoEXT mci{XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT};
    mci.renderModelId = id;
    if (XR_FAILED(pfnCreateModel_(session_, &mci, &model_))) {
        PIXELVR_LOG_WARN("xrCreateRenderModelEXT failed");
        return false;
    }

    XrRenderModelPropertiesGetInfoEXT pgi{
        XR_TYPE_RENDER_MODEL_PROPERTIES_GET_INFO_EXT};
    XrRenderModelPropertiesEXT props{XR_TYPE_RENDER_MODEL_PROPERTIES_EXT};
    if (XR_FAILED(pfnGetProps_(model_, &pgi, &props))) {
        PIXELVR_LOG_WARN("xrGetRenderModelPropertiesEXT failed");
        return false;
    }
    animatableNodeCount_ = props.animatableNodeCount;

    XrRenderModelAssetCreateInfoEXT aci{XR_TYPE_RENDER_MODEL_ASSET_CREATE_INFO_EXT};
    aci.cacheId = props.cacheId;
    if (XR_FAILED(pfnCreateAsset_(session_, &aci, &asset_))) {
        PIXELVR_LOG_WARN("xrCreateRenderModelAssetEXT failed");
        return false;
    }

    XrRenderModelAssetDataGetInfoEXT adi{
        XR_TYPE_RENDER_MODEL_ASSET_DATA_GET_INFO_EXT};
    XrRenderModelAssetDataEXT data{XR_TYPE_RENDER_MODEL_ASSET_DATA_EXT};
    if (XR_FAILED(pfnGetAssetData_(asset_, &adi, &data))) {  // size query
        PIXELVR_LOG_WARN("xrGetRenderModelAssetDataEXT (size) failed");
        return false;
    }
    gltf_.resize(data.bufferCountOutput);
    data.bufferCapacityInput = static_cast<uint32_t>(gltf_.size());
    data.buffer = gltf_.data();
    if (XR_FAILED(pfnGetAssetData_(asset_, &adi, &data))) {
        PIXELVR_LOG_WARN("xrGetRenderModelAssetDataEXT (data) failed");
        gltf_.clear();
        return false;
    }

    XrRenderModelSpaceCreateInfoEXT sci{XR_TYPE_RENDER_MODEL_SPACE_CREATE_INFO_EXT};
    sci.renderModel = model_;
    pfnCreateSpace_(session_, &sci, &modelSpace_);  // used by later stages

    // Animatable node names, in the same order as xrGetRenderModelStateEXT node
    // states, so an animated node can be mapped back to glTF geometry.
    if (pfnGetAssetProps_ != nullptr) {
        XrRenderModelAssetPropertiesGetInfoEXT agi{
            XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_GET_INFO_EXT};
        XrRenderModelAssetPropertiesEXT props{
            XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_EXT};
        if (XR_FAILED(pfnGetAssetProps_(asset_, &agi, &props))) {
            PIXELVR_LOG_WARN("xrGetRenderModelAssetPropertiesEXT (size) failed");
        } else {
            std::vector<XrRenderModelAssetNodePropertiesEXT> names(
                props.nodePropertyCount);
            props.nodePropertyCount = static_cast<uint32_t>(names.size());
            props.nodeProperties = names.data();
            if (XR_SUCCEEDED(pfnGetAssetProps_(asset_, &agi, &props))) {
                nodeNames_.reserve(names.size());
                for (const auto& np : names) nodeNames_.emplace_back(np.uniqueName);
                std::string joined;
                for (const auto& n : nodeNames_) {
                    joined += n;
                    joined += ' ';
                }
                PIXELVR_LOG_INFO("Render model nodes: %s", joined.c_str());
            } else {
                PIXELVR_LOG_WARN("xrGetRenderModelAssetPropertiesEXT (data) failed");
            }
        }
    }

    PIXELVR_LOG_INFO("Render model fetched: %u animatable nodes, glTF %zu bytes",
                     animatableNodeCount_, gltf_.size());
    if (FILE* f = std::fopen("/tmp/controller_model.glb", "wb")) {
        std::fwrite(gltf_.data(), 1, gltf_.size(), f);
        std::fclose(f);
    }
    return true;
}

bool RenderModelSystem::locate(XrSpace base, XrTime time, float outPos[3],
                               float outQuat[4]) const {
    if (modelSpace_ == XR_NULL_HANDLE || time == 0) return false;
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(modelSpace_, base, time, &loc))) return false;
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

bool RenderModelSystem::nodeStates(XrTime time, std::vector<NodeState>& out) {
    if (!available_ || model_ == XR_NULL_HANDLE || pfnGetState_ == nullptr ||
        time == 0) {
        return false;
    }
    if (nodeStateBuf_.size() < animatableNodeCount_) {
        nodeStateBuf_.resize(animatableNodeCount_);
    }
    XrRenderModelStateGetInfoEXT gi{XR_TYPE_RENDER_MODEL_STATE_GET_INFO_EXT};
    gi.displayTime = time;
    XrRenderModelStateEXT st{XR_TYPE_RENDER_MODEL_STATE_EXT};
    st.nodeStateCount = static_cast<uint32_t>(nodeStateBuf_.size());
    st.nodeStates = nodeStateBuf_.data();
    if (XR_FAILED(pfnGetState_(model_, &gi, &st))) {
        return false;
    }
    if (st.nodeStateCount > nodeStateBuf_.size()) {  // grew; retry once
        nodeStateBuf_.resize(st.nodeStateCount);
        st.nodeStateCount = static_cast<uint32_t>(nodeStateBuf_.size());
        st.nodeStates = nodeStateBuf_.data();
        if (XR_FAILED(pfnGetState_(model_, &gi, &st))) {
            return false;
        }
    }
    out.resize(st.nodeStateCount);
    for (uint32_t i = 0; i < st.nodeStateCount; ++i) {
        const XrRenderModelNodeStateEXT& n = nodeStateBuf_[i];
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
    if (modelSpace_ != XR_NULL_HANDLE) {
        xrDestroySpace(modelSpace_);
        modelSpace_ = XR_NULL_HANDLE;
    }
    if (asset_ != XR_NULL_HANDLE && pfnDestroyAsset_ != nullptr) {
        pfnDestroyAsset_(asset_);
        asset_ = XR_NULL_HANDLE;
    }
    if (model_ != XR_NULL_HANDLE && pfnDestroyModel_ != nullptr) {
        pfnDestroyModel_(model_);
        model_ = XR_NULL_HANDLE;
    }
}

} // namespace pixelvr
