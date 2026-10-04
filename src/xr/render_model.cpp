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

    PIXELVR_LOG_INFO("Render model fetched: %u animatable nodes, glTF %zu bytes",
                     animatableNodeCount_, gltf_.size());
    if (FILE* f = std::fopen("/tmp/controller_model.glb", "wb")) {
        std::fwrite(gltf_.data(), 1, gltf_.size(), f);
        std::fclose(f);
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
