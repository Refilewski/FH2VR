#include <algorithm>
#include <atomic>
#include <cmath>

// Reuse the proven M3 capture + conversion path unchanged, but rename its
// submit entry point inside this translation unit so M4 can replace only the
// compositor projection/bounds stage.
#define submit_direct_d3d12_mono_preview submit_direct_d3d12_mono_preview_m3
#include "RendererM3.cpp"
#undef submit_direct_d3d12_mono_preview

namespace {

struct ProjectionBoundsM4 {
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
    float tanHalfX = 0.0f;
    float tanHalfY = 0.0f;
    float horizontalFovDegrees = 0.0f;
    float verticalFovDegrees = 0.0f;
};

bool build_projection_bounds_m4(vr::IVRSystem *system, ProjectionBoundsM4 &out) {
    if (!system) {
        return false;
    }

    system->GetProjectionRaw(vr::Eye_Left,
                             &out.lLeft, &out.lRight, &out.lTop, &out.lBottom);
    system->GetProjectionRaw(vr::Eye_Right,
                             &out.rLeft, &out.rRight, &out.rTop, &out.rBottom);

    // This is the same combined-frustum construction Valve uses in the
    // SteamVR Unity sample. Each eye then receives its own sub-rectangle of
    // the shared render texture.
    out.tanHalfX = std::max({-out.lLeft, out.lRight, -out.rLeft, out.rRight});
    out.tanHalfY = std::max({-out.lTop, out.lBottom, -out.rTop, out.rBottom});
    if (!(out.tanHalfX > 0.0001f) || !(out.tanHalfY > 0.0001f)) {
        return false;
    }

    out.left.uMin = 0.5f + 0.5f * out.lLeft / out.tanHalfX;
    out.left.uMax = 0.5f + 0.5f * out.lRight / out.tanHalfX;
    out.left.vMin = 0.5f - 0.5f * out.lBottom / out.tanHalfY;
    out.left.vMax = 0.5f - 0.5f * out.lTop / out.tanHalfY;

    out.right.uMin = 0.5f + 0.5f * out.rLeft / out.tanHalfX;
    out.right.uMax = 0.5f + 0.5f * out.rRight / out.tanHalfX;
    out.right.vMin = 0.5f - 0.5f * out.rBottom / out.tanHalfY;
    out.right.vMax = 0.5f - 0.5f * out.rTop / out.tanHalfY;

    constexpr float kRadToDeg = 57.2957795130823208768f;
    out.horizontalFovDegrees = 2.0f * std::atan(out.tanHalfX) * kRadToDeg;
    out.verticalFovDegrees = 2.0f * std::atan(out.tanHalfY) * kRadToDeg;
    return true;
}

void log_projection_once_m4(const ProjectionBoundsM4 &projection,
                            const D3D12_RESOURCE_DESC &desc) {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    char buffer[1024]{};
    std::snprintf(
        buffer, sizeof(buffer),
        "M4 OpenVR projection raw: L=[%.5f %.5f %.5f %.5f] R=[%.5f %.5f %.5f %.5f]; combinedFov=%.2fx%.2f deg tanHalf=(%.5f,%.5f); source=%llux%u; boundsL=[%.5f %.5f %.5f %.5f] boundsR=[%.5f %.5f %.5f %.5f]",
        projection.lLeft, projection.lRight, projection.lTop, projection.lBottom,
        projection.rLeft, projection.rRight, projection.rTop, projection.rBottom,
        projection.horizontalFovDegrees, projection.verticalFovDegrees,
        projection.tanHalfX, projection.tanHalfY,
        static_cast<unsigned long long>(desc.Width), desc.Height,
        projection.left.uMin, projection.left.vMin,
        projection.left.uMax, projection.left.vMax,
        projection.right.uMin, projection.right.vMin,
        projection.right.uMax, projection.right.vMax);
    log_line(buffer);
}

} // namespace

namespace fh2vr {

bool submit_direct_d3d12_mono_preview(vr::IVRCompositor *compositor, std::string &error) {
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

    vr::TrackedDevicePose_t renderPose{};
    const vr::EVRCompositorError poseResult = compositor->GetLastPoseForTrackedDeviceIndex(
        vr::k_unTrackedDeviceIndex_Hmd, &renderPose, nullptr);
    if (poseResult != vr::VRCompositorError_None || !renderPose.bPoseIsValid) {
        texture->Release();
        queue->Release();
        error = compositor_error("GetLastPoseForTrackedDeviceIndex", poseResult);
        return false;
    }

    ProjectionBoundsM4 projection{};
    if (!build_projection_bounds_m4(vr::VRSystem(), projection)) {
        texture->Release();
        queue->Release();
        error = "OpenVR GetProjectionRaw returned an invalid combined frustum";
        return false;
    }
    log_projection_once_m4(projection, desc);

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
        error = compositor_error("M4 DirectX12 R8 Submit(left)", leftResult);
        return false;
    }

    const vr::EVRCompositorError rightResult = compositor->Submit(
        vr::Eye_Right, &submitted, &projection.right, vr::Submit_TextureWithPose);
    if (rightResult != vr::VRCompositorError_None) {
        texture->Release();
        queue->Release();
        error = compositor_error("M4 DirectX12 R8 Submit(right)", rightResult);
        return false;
    }

    compositor->PostPresentHandoff();
    g_openVrPairSubmits.fetch_add(1, std::memory_order_relaxed);

    static std::atomic<bool> loggedSuccess{false};
    if (!loggedSuccess.exchange(true, std::memory_order_acq_rel)) {
        char buffer[768]{};
        std::snprintf(
            buffer, sizeof(buffer),
            "M4 OpenVR per-eye projection submit OK: resource=%p queue=%p format=%s(%d) left=[%.5f %.5f %.5f %.5f] right=[%.5f %.5f %.5f %.5f] pose=Submit_TextureWithPose",
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
