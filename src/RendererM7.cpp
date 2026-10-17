#include "RendererPreview.h"
#include "FovMatch.h"

#include <algorithm>
#include <atomic>
#include <cmath>

// Reuse the proven M6 renderer, projection, FOV matching and stable-frame
// reprojection state. M7 changes only the pose handed to OpenVR so it matches
// what FH2 actually rendered: yaw + pitch, but no camera roll.
#define submit_direct_d3d12_mono_preview submit_direct_d3d12_mono_preview_m6
#include "RendererM6.cpp"
#undef submit_direct_d3d12_mono_preview

namespace {

vr::HmdMatrix34_t roll_neutral_pose_m7(const vr::HmdMatrix34_t &input,
                                        float *removedRollDegrees) {
    // OpenVR device-to-absolute rotation columns are the device basis vectors
    // in absolute space. Device forward is -column 2. Preserve that forward
    // direction exactly (therefore preserving yaw + pitch), rebuild right/up
    // against world +Y, and keep the original translation.
    float fx = -input.m[0][2];
    float fy = -input.m[1][2];
    float fz = -input.m[2][2];
    const float fLen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (!std::isfinite(fLen) || fLen < 0.0001f) {
        if (removedRollDegrees) *removedRollDegrees = 0.0f;
        return input;
    }
    fx /= fLen;
    fy /= fLen;
    fz /= fLen;

    // right = normalize(forward x worldUp)
    float rx = -fz;
    float ry = 0.0f;
    float rz = fx;
    const float rLen = std::sqrt(rx * rx + rz * rz);
    if (!std::isfinite(rLen) || rLen < 0.0001f) {
        if (removedRollDegrees) *removedRollDegrees = 0.0f;
        return input;
    }
    rx /= rLen;
    rz /= rLen;

    // up = right x forward
    const float ux = ry * fz - rz * fy;
    const float uy = rz * fx - rx * fz;
    const float uz = rx * fy - ry * fx;

    vr::HmdMatrix34_t output = input;
    output.m[0][0] = rx;
    output.m[1][0] = ry;
    output.m[2][0] = rz;

    output.m[0][1] = ux;
    output.m[1][1] = uy;
    output.m[2][1] = uz;

    output.m[0][2] = -fx;
    output.m[1][2] = -fy;
    output.m[2][2] = -fz;

    if (removedRollDegrees) {
        // Compare the tracked up vector with the roll-neutral up vector around
        // the preserved forward axis. This is diagnostic only.
        float iux = input.m[0][1];
        float iuy = input.m[1][1];
        float iuz = input.m[2][1];
        const float iuLen = std::sqrt(iux * iux + iuy * iuy + iuz * iuz);
        if (std::isfinite(iuLen) && iuLen > 0.0001f) {
            iux /= iuLen;
            iuy /= iuLen;
            iuz /= iuLen;
            const float cosRoll = std::clamp(iux * ux + iuy * uy + iuz * uz, -1.0f, 1.0f);
            const float sinRoll = std::clamp(iux * rx + iuy * ry + iuz * rz, -1.0f, 1.0f);
            *removedRollDegrees = std::atan2(sinRoll, cosRoll) * 57.2957795130823208768f;
        } else {
            *removedRollDegrees = 0.0f;
        }
    }

    return output;
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
        error = "M7 could not build source-aspect-aware OpenVR projection";
        return false;
    }

    update_fov_match_target(
        projection.sourceVerticalFovRadians,
        projection.sourceHorizontalFovRadians,
        projection.sourceAspect,
        projection.combinedVerticalFovRadians,
        projection.combinedHorizontalFovRadians);
    log_projection_once_m6(projection, desc);

    vr::HmdMatrix34_t stableTrackedPose{};
    if (!render_pose_for_frame(compositor, frameFence, stableTrackedPose, error)) {
        texture->Release();
        queue->Release();
        return false;
    }

    float removedRollDegrees = 0.0f;
    const vr::HmdMatrix34_t cameraMatchedPose =
        roll_neutral_pose_m7(stableTrackedPose, &removedRollDegrees);

    vr::D3D12TextureData_t d3d12Texture{texture, queue, 0};
    vr::VRTextureWithPose_t submitted{};
    submitted.handle = &d3d12Texture;
    submitted.eType = vr::TextureType_DirectX12;
    submitted.eColorSpace = vr::ColorSpace_Gamma;
    submitted.mDeviceToAbsoluteTracking = cameraMatchedPose;

    const vr::EVRCompositorError leftResult = compositor->Submit(
        vr::Eye_Left, &submitted, &projection.left, vr::Submit_TextureWithPose);
    if (leftResult != vr::VRCompositorError_None) {
        texture->Release();
        queue->Release();
        error = compositor_error("M7 DirectX12 R8 Submit(left)", leftResult);
        return false;
    }

    const vr::EVRCompositorError rightResult = compositor->Submit(
        vr::Eye_Right, &submitted, &projection.right, vr::Submit_TextureWithPose);
    if (rightResult != vr::VRCompositorError_None) {
        texture->Release();
        queue->Release();
        error = compositor_error("M7 DirectX12 R8 Submit(right)", rightResult);
        return false;
    }

    compositor->PostPresentHandoff();
    g_openVrPairSubmits.fetch_add(1, std::memory_order_relaxed);
    ++g_reprojection.pairSubmits;
    log_reprojection_stats_m6(frameFence);

    static std::atomic<bool> loggedSuccess{false};
    if (!loggedSuccess.exchange(true, std::memory_order_acq_rel)) {
        char buffer[896]{};
        std::snprintf(
            buffer, sizeof(buffer),
            "M7 camera-matched pose submit OK: yaw/pitch preserved, roll neutralized (first-frame tracked roll delta %.2f deg); resource=%p queue=%p fence=%llu format=%s(%d)",
            removedRollDegrees,
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
