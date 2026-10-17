#include "RendererPreview.h"
#include "FovMatch.h"

#include <algorithm>
#include <atomic>
#include <cmath>

// Keep the proven M3 D3D12 capture + R10->R8 conversion path unchanged. M5
// replaces only the projection/submit stage and adds live FH2 Driver FOV match.
#define submit_direct_d3d12_mono_preview submit_direct_d3d12_mono_preview_m3
#include "RendererM3.cpp"
#undef submit_direct_d3d12_mono_preview

namespace {

struct ProjectionM5 {
    vr::VRTextureBounds_t left{};
    vr::VRTextureBounds_t right{};

    float lLeft = 0.0f;
    float lRight = 0.0f;
    float lTop = 0.0f;
    float lBottom = 0.0f;
    float rLeft = 0.0f;
    float rRight = 0.0f;
    float rTop = 0.0f;
    float rBottom = 0.0f;

    float combinedTanHalfX = 0.0f;
    float combinedTanHalfY = 0.0f;
    float sourceTanHalfX = 0.0f;
    float sourceTanHalfY = 0.0f;
    float sourceAspect = 0.0f;

    float combinedHorizontalFovRadians = 0.0f;
    float combinedVerticalFovRadians = 0.0f;
    float sourceHorizontalFovRadians = 0.0f;
    float sourceVerticalFovRadians = 0.0f;
};

bool build_projection_m5(vr::IVRSystem *system,
                         const D3D12_RESOURCE_DESC &desc,
                         ProjectionM5 &out) {
    if (!system || desc.Width == 0 || desc.Height == 0) {
        return false;
    }

    system->GetProjectionRaw(vr::Eye_Left,
                             &out.lLeft, &out.lRight, &out.lTop, &out.lBottom);
    system->GetProjectionRaw(vr::Eye_Right,
                             &out.rLeft, &out.rRight, &out.rTop, &out.rBottom);

    out.combinedTanHalfX =
        std::max({-out.lLeft, out.lRight, -out.rLeft, out.rRight});
    out.combinedTanHalfY =
        std::max({-out.lTop, out.lBottom, -out.rTop, out.rBottom});
    out.sourceAspect = static_cast<float>(desc.Width) /
                       static_cast<float>(desc.Height);

    if (!(out.combinedTanHalfX > 0.0001f) ||
        !(out.combinedTanHalfY > 0.0001f) ||
        !(out.sourceAspect > 0.1f)) {
        return false;
    }

    // FH2 renders a symmetric perspective into its 16:9 swapchain. We cannot
    // pretend that texture itself has the Quest combined-frustum aspect. Pick
    // the smallest symmetric 16:9 frustum that fully contains the combined HMD
    // frustum, then map each eye's raw tangents into that real source frustum.
    //
    // This is the key difference from M4: M4 fixed eye asymmetry, but treated
    // the source texture as if its projection aspect equalled the HMD frustum.
    // That can remove the cross-eye effect while still leaving the world
    // horizontally/vertically compressed.
    out.sourceTanHalfY = std::max(out.combinedTanHalfY,
                                  out.combinedTanHalfX / out.sourceAspect);
    out.sourceTanHalfX = out.sourceAspect * out.sourceTanHalfY;

    out.left.uMin = 0.5f + 0.5f * out.lLeft / out.sourceTanHalfX;
    out.left.uMax = 0.5f + 0.5f * out.lRight / out.sourceTanHalfX;
    out.left.vMin = 0.5f - 0.5f * out.lBottom / out.sourceTanHalfY;
    out.left.vMax = 0.5f - 0.5f * out.lTop / out.sourceTanHalfY;

    out.right.uMin = 0.5f + 0.5f * out.rLeft / out.sourceTanHalfX;
    out.right.uMax = 0.5f + 0.5f * out.rRight / out.sourceTanHalfX;
    out.right.vMin = 0.5f - 0.5f * out.rBottom / out.sourceTanHalfY;
    out.right.vMax = 0.5f - 0.5f * out.rTop / out.sourceTanHalfY;

    out.combinedHorizontalFovRadians = 2.0f * std::atan(out.combinedTanHalfX);
    out.combinedVerticalFovRadians = 2.0f * std::atan(out.combinedTanHalfY);
    out.sourceHorizontalFovRadians = 2.0f * std::atan(out.sourceTanHalfX);
    out.sourceVerticalFovRadians = 2.0f * std::atan(out.sourceTanHalfY);

    return std::isfinite(out.sourceHorizontalFovRadians) &&
           std::isfinite(out.sourceVerticalFovRadians);
}

void log_projection_once_m5(const ProjectionM5 &projection,
                            const D3D12_RESOURCE_DESC &desc) {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    constexpr float kRadToDeg = 57.2957795130823208768f;
    char buffer[1400]{};
    std::snprintf(
        buffer, sizeof(buffer),
        "M5 projection/FOV match: raw L=[%.5f %.5f %.5f %.5f] R=[%.5f %.5f %.5f %.5f]; HMD combined=%.2fx%.2f deg tanHalf=(%.5f,%.5f); FH2 source=%llux%u aspect=%.5f targetFov=%.2fx%.2f deg sourceTanHalf=(%.5f,%.5f); boundsL=[%.5f %.5f %.5f %.5f] boundsR=[%.5f %.5f %.5f %.5f]",
        projection.lLeft, projection.lRight, projection.lTop, projection.lBottom,
        projection.rLeft, projection.rRight, projection.rTop, projection.rBottom,
        projection.combinedHorizontalFovRadians * kRadToDeg,
        projection.combinedVerticalFovRadians * kRadToDeg,
        projection.combinedTanHalfX, projection.combinedTanHalfY,
        static_cast<unsigned long long>(desc.Width), desc.Height,
        projection.sourceAspect,
        projection.sourceHorizontalFovRadians * kRadToDeg,
        projection.sourceVerticalFovRadians * kRadToDeg,
        projection.sourceTanHalfX, projection.sourceTanHalfY,
        projection.left.uMin, projection.left.vMin,
        projection.left.uMax, projection.left.vMax,
        projection.right.uMin, projection.right.vMin,
        projection.right.uMax, projection.right.vMax);
    log_line(buffer);
}

} // namespace

namespace fh2vr {

bool submit_direct_d3d12_mono_preview(vr::IVRCompositor *compositor,
                                      std::string &error) {
    if (!compositor) {
        error = "OpenVR compositor is unavailable";
        return false;
    }

    ID3D12CommandQueue *queue = g_primaryQueue.load(std::memory_order_acquire);
    const int latest = g_latestSlot.load(std::memory_order_acquire);
    if (!queue || latest < 0 || latest > 1) {
        error = "D3D12 converted renderer frame is not ready yet";
        return false;
    }

    ID3D12Resource *texture = nullptr;
    D3D12_RESOURCE_DESC desc{};
    AcquireSRWLockShared(&g_resourceLock);
    if (g_resourcesReady && g_slots[latest].hasFrame && g_slots[latest].submit) {
        texture = g_slots[latest].submit;
        texture->AddRef();
        desc = g_submitDesc;
        queue->AddRef();
    }
    ReleaseSRWLockShared(&g_resourceLock);

    if (!texture) {
        error = "latest converted R8 D3D12 frame is unavailable";
        return false;
    }

    vr::IVRSystem *system = vr::VRSystem();
    ProjectionM5 projection{};
    if (!build_projection_m5(system, desc, projection)) {
        texture->Release();
        queue->Release();
        error = "M5 could not build source-aspect-aware OpenVR projection";
        return false;
    }

    // Refreshing this every successful preview call also acts as the enable
    // signal for FovMatch.cpp. If F9 preview stops, the hook automatically
    // falls back to the game's original FOV after a short freshness timeout.
    update_fov_match_target(
        projection.sourceVerticalFovRadians,
        projection.sourceHorizontalFovRadians,
        projection.sourceAspect,
        projection.combinedVerticalFovRadians,
        projection.combinedHorizontalFovRadians);
    log_projection_once_m5(projection, desc);

    vr::TrackedDevicePose_t renderPose{};
    const vr::EVRCompositorError poseResult = compositor->GetLastPoseForTrackedDeviceIndex(
        vr::k_unTrackedDeviceIndex_Hmd, &renderPose, nullptr);
    if (poseResult != vr::VRCompositorError_None || !renderPose.bPoseIsValid) {
        texture->Release();
        queue->Release();
        error = compositor_error("GetLastPoseForTrackedDeviceIndex", poseResult);
        return false;
    }

    vr::D3D12TextureData_t d3d12Texture{texture, queue, 0};
    vr::VRTextureWithPose_t submitted{};
    submitted.handle = &d3d12Texture;
    submitted.eType = vr::TextureType_DirectX12;
    submitted.eColorSpace = vr::ColorSpace_Gamma;
    submitted.mDeviceToAbsoluteTracking = renderPose.mDeviceToAbsoluteTracking;

    const vr::EVRCompositorError leftResult = compositor->Submit(
        vr::Eye_Left, &submitted, &projection.left, vr::Submit_TextureWithPose);
    if (leftResult != vr::VRCompositorError_None) {
        texture->Release();
        queue->Release();
        error = compositor_error("M5 DirectX12 R8 Submit(left)", leftResult);
        return false;
    }

    const vr::EVRCompositorError rightResult = compositor->Submit(
        vr::Eye_Right, &submitted, &projection.right, vr::Submit_TextureWithPose);
    if (rightResult != vr::VRCompositorError_None) {
        texture->Release();
        queue->Release();
        error = compositor_error("M5 DirectX12 R8 Submit(right)", rightResult);
        return false;
    }

    compositor->PostPresentHandoff();
    g_openVrPairSubmits.fetch_add(1, std::memory_order_relaxed);

    static std::atomic<bool> loggedSuccess{false};
    if (!loggedSuccess.exchange(true, std::memory_order_acq_rel)) {
        char buffer[900]{};
        std::snprintf(
            buffer, sizeof(buffer),
            "M5 source-aspect projection submit OK: resource=%p queue=%p format=%s(%d) left=[%.5f %.5f %.5f %.5f] right=[%.5f %.5f %.5f %.5f] pose=Submit_TextureWithPose",
            static_cast<void *>(texture), static_cast<void *>(queue),
            format_name(desc.Format), static_cast<int>(desc.Format),
            projection.left.uMin, projection.left.vMin,
            projection.left.uMax, projection.left.vMax,
            projection.right.uMin, projection.right.vMin,
            projection.right.uMax, projection.right.vMax);
        log_line(buffer);
    }

    texture->Release();
    queue->Release();
    error.clear();
    return true;
}

} // namespace fh2vr
