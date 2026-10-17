#include "RendererPreview.h"
#include "FovMatch.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

// Reuse the proven native D3D12 capture + R10->R8 conversion path. M6 keeps
// the M5 source-aspect/FOV projection, but fixes compositor timing: a captured
// game frame keeps one stable render pose for every repeated OpenVR submit.
// That gives SteamVR a real pose delta to asynchronously reproject/timewarp
// the same ~30-40 Hz FH2 frame at the HMD compositor cadence.
#define submit_direct_d3d12_mono_preview submit_direct_d3d12_mono_preview_m3
#include "RendererM3.cpp"
#undef submit_direct_d3d12_mono_preview

namespace {

struct ProjectionM6 {
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

bool build_projection_m6(vr::IVRSystem *system,
                         const D3D12_RESOURCE_DESC &desc,
                         ProjectionM6 &out) {
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

    // FH2 renders a symmetric perspective into its 16:9 source. Fit the
    // combined Quest frustum inside that real source aspect, then map each eye
    // into its correct sub-rectangle. This preserves the M5 FOV fix.
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

struct ReprojectionState {
    UINT64 frameFence = 0;
    vr::HmdMatrix34_t frameRenderPose{};
    bool haveFramePose = false;
    std::uint64_t pairSubmits = 0;
    std::uint64_t uniqueFrames = 0;
    std::uint64_t repeatedFrames = 0;
    ULONGLONG lastLogAt = 0;
};

ReprojectionState g_reprojection{};

bool render_pose_for_frame(vr::IVRCompositor *compositor,
                           UINT64 frameFence,
                           vr::HmdMatrix34_t &pose,
                           std::string &error) {
    if (!compositor || frameFence == 0) {
        error = "M6 frame pose is unavailable";
        return false;
    }

    // A fence value is unique for every captured/converted FH2 frame. Sample
    // the OpenVR pose only once when a new game frame first reaches the submit
    // thread, then keep that pose frozen while the same texture is resubmitted
    // at compositor cadence. Reusing a continuously-updated pose here would
    // incorrectly tell SteamVR that an old texture was rendered at the latest
    // head pose and would largely defeat rotational reprojection.
    if (!g_reprojection.haveFramePose || g_reprojection.frameFence != frameFence) {
        vr::TrackedDevicePose_t renderPose{};
        const vr::EVRCompositorError poseResult =
            compositor->GetLastPoseForTrackedDeviceIndex(
                vr::k_unTrackedDeviceIndex_Hmd, &renderPose, nullptr);
        if (poseResult != vr::VRCompositorError_None || !renderPose.bPoseIsValid) {
            error = compositor_error("M6 GetLastPoseForTrackedDeviceIndex", poseResult);
            return false;
        }

        g_reprojection.frameFence = frameFence;
        g_reprojection.frameRenderPose = renderPose.mDeviceToAbsoluteTracking;
        g_reprojection.haveFramePose = true;
        ++g_reprojection.uniqueFrames;
    } else {
        ++g_reprojection.repeatedFrames;
    }

    pose = g_reprojection.frameRenderPose;
    return true;
}

void log_projection_once_m6(const ProjectionM6 &projection,
                            const D3D12_RESOURCE_DESC &desc) {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    constexpr float kRadToDeg = 57.2957795130823208768f;
    char buffer[1400]{};
    std::snprintf(
        buffer, sizeof(buffer),
        "M6 projection/FOV: HMD combined=%.2fx%.2f deg; FH2 source=%llux%u aspect=%.5f targetFov=%.2fx%.2f deg; boundsL=[%.5f %.5f %.5f %.5f] boundsR=[%.5f %.5f %.5f %.5f]",
        projection.combinedHorizontalFovRadians * kRadToDeg,
        projection.combinedVerticalFovRadians * kRadToDeg,
        static_cast<unsigned long long>(desc.Width), desc.Height,
        projection.sourceAspect,
        projection.sourceHorizontalFovRadians * kRadToDeg,
        projection.sourceVerticalFovRadians * kRadToDeg,
        projection.left.uMin, projection.left.vMin,
        projection.left.uMax, projection.left.vMax,
        projection.right.uMin, projection.right.vMin,
        projection.right.uMax, projection.right.vMax);
    log_line(buffer);
}

void log_reprojection_stats_m6(UINT64 latestFence) {
    const ULONGLONG now = GetTickCount64();
    if (g_reprojection.lastLogAt == 0) {
        g_reprojection.lastLogAt = now;
        return;
    }
    if (now - g_reprojection.lastLogAt < 1000) {
        return;
    }

    const ULONGLONG elapsed = now - g_reprojection.lastLogAt;
    const double scale = elapsed ? 1000.0 / static_cast<double>(elapsed) : 0.0;
    const double submitHz = static_cast<double>(g_reprojection.pairSubmits) * scale;
    const double uniqueHz = static_cast<double>(g_reprojection.uniqueFrames) * scale;
    const double repeatedHz = static_cast<double>(g_reprojection.repeatedFrames) * scale;

    char buffer[512]{};
    std::snprintf(
        buffer, sizeof(buffer),
        "M6 compositor cadence: %.2f pairs/s; unique FH2 frames=%.2f/s; repeated reprojection submits=%.2f/s; latestFence=%llu",
        submitHz, uniqueHz, repeatedHz,
        static_cast<unsigned long long>(latestFence));
    log_line(buffer);

    g_reprojection.pairSubmits = 0;
    g_reprojection.uniqueFrames = 0;
    g_reprojection.repeatedFrames = 0;
    g_reprojection.lastLogAt = now;
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
    UINT64 frameFence = 0;
    AcquireSRWLockShared(&g_resourceLock);
    if (g_resourcesReady && g_slots[latest].hasFrame && g_slots[latest].submit) {
        texture = g_slots[latest].submit;
        texture->AddRef();
        desc = g_submitDesc;
        frameFence = g_slots[latest].fenceValue;
        queue->AddRef();
    }
    ReleaseSRWLockShared(&g_resourceLock);

    if (!texture || frameFence == 0) {
        if (texture) texture->Release();
        error = "latest converted R8 D3D12 frame is unavailable";
        return false;
    }

    ProjectionM6 projection{};
    if (!build_projection_m6(vr::VRSystem(), desc, projection)) {
        texture->Release();
        queue->Release();
        error = "M6 could not build source-aspect-aware OpenVR projection";
        return false;
    }

    update_fov_match_target(
        projection.sourceVerticalFovRadians,
        projection.sourceHorizontalFovRadians,
        projection.sourceAspect,
        projection.combinedVerticalFovRadians,
        projection.combinedHorizontalFovRadians);
    log_projection_once_m6(projection, desc);

    vr::HmdMatrix34_t stableRenderPose{};
    if (!render_pose_for_frame(compositor, frameFence, stableRenderPose, error)) {
        texture->Release();
        queue->Release();
        return false;
    }

    vr::D3D12TextureData_t d3d12Texture{texture, queue, 0};
    vr::VRTextureWithPose_t submitted{};
    submitted.handle = &d3d12Texture;
    submitted.eType = vr::TextureType_DirectX12;
    submitted.eColorSpace = vr::ColorSpace_Gamma;
    submitted.mDeviceToAbsoluteTracking = stableRenderPose;

    const vr::EVRCompositorError leftResult = compositor->Submit(
        vr::Eye_Left, &submitted, &projection.left, vr::Submit_TextureWithPose);
    if (leftResult != vr::VRCompositorError_None) {
        texture->Release();
        queue->Release();
        error = compositor_error("M6 DirectX12 R8 Submit(left)", leftResult);
        return false;
    }

    const vr::EVRCompositorError rightResult = compositor->Submit(
        vr::Eye_Right, &submitted, &projection.right, vr::Submit_TextureWithPose);
    if (rightResult != vr::VRCompositorError_None) {
        texture->Release();
        queue->Release();
        error = compositor_error("M6 DirectX12 R8 Submit(right)", rightResult);
        return false;
    }

    compositor->PostPresentHandoff();
    g_openVrPairSubmits.fetch_add(1, std::memory_order_relaxed);
    ++g_reprojection.pairSubmits;
    log_reprojection_stats_m6(frameFence);

    static std::atomic<bool> loggedSuccess{false};
    if (!loggedSuccess.exchange(true, std::memory_order_acq_rel)) {
        char buffer[768]{};
        std::snprintf(
            buffer, sizeof(buffer),
            "M6 stable-frame-pose submit OK: resource=%p queue=%p fence=%llu format=%s(%d) pose=Submit_TextureWithPose",
            static_cast<void *>(texture), static_cast<void *>(queue),
            static_cast<unsigned long long>(frameFence),
            format_name(desc.Format), static_cast<int>(desc.Format));
        log_line(buffer);
    }

    texture->Release();
    queue->Release();
    error.clear();
    return true;
}

} // namespace fh2vr
