#include "RendererPreview.h"
#include "FovMatch.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

// M8 is intentionally standalone on top of the proven M3 capture/conversion
// path. This avoids wrapper-on-wrapper macro collisions while preserving the
// working native D3D12 -> R8 OpenVR path.
#define submit_direct_d3d12_mono_preview submit_direct_d3d12_mono_preview_m3
#include "RendererM3.cpp"
#undef submit_direct_d3d12_mono_preview

namespace {

struct ProjectionM8 {
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

bool build_projection_m8(vr::IVRSystem *system,
                         const D3D12_RESOURCE_DESC &desc,
                         ProjectionM8 &out) {
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

    // Preserve the working M5 geometry: fit the combined HMD frustum into the
    // real 16:9 FH2 source projection, then derive per-eye texture bounds.
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

struct ReprojectionStateM8 {
    UINT64 frameFence = 0;
    vr::HmdMatrix34_t frameRenderPose{};
    bool haveFramePose = false;
    std::uint64_t pairSubmits = 0;
    std::uint64_t uniqueFrames = 0;
    std::uint64_t repeatedFrames = 0;
    ULONGLONG lastLogAt = 0;
};

ReprojectionStateM8 g_reprojectionM8{};

bool render_pose_for_frame_m8(vr::IVRCompositor *compositor,
                              UINT64 frameFence,
                              vr::HmdMatrix34_t &pose,
                              std::string &error) {
    if (!compositor || frameFence == 0) {
        error = "M8 frame pose is unavailable";
        return false;
    }

    if (!g_reprojectionM8.haveFramePose ||
        g_reprojectionM8.frameFence != frameFence) {
        vr::TrackedDevicePose_t renderPose{};
        const vr::EVRCompositorError poseResult =
            compositor->GetLastPoseForTrackedDeviceIndex(
                vr::k_unTrackedDeviceIndex_Hmd, &renderPose, nullptr);
        if (poseResult != vr::VRCompositorError_None || !renderPose.bPoseIsValid) {
            error = compositor_error("M8 GetLastPoseForTrackedDeviceIndex", poseResult);
            return false;
        }

        g_reprojectionM8.frameFence = frameFence;
        g_reprojectionM8.frameRenderPose = renderPose.mDeviceToAbsoluteTracking;
        g_reprojectionM8.haveFramePose = true;
        ++g_reprojectionM8.uniqueFrames;
    } else {
        ++g_reprojectionM8.repeatedFrames;
    }

    pose = g_reprojectionM8.frameRenderPose;
    return true;
}

vr::HmdMatrix34_t roll_neutral_pose_m8(const vr::HmdMatrix34_t &input,
                                        float *removedRollDegrees) {
    // FH2's current camera hook renders yaw + pitch only. Preserve the tracked
    // forward direction exactly and rebuild right/up against world +Y, which
    // removes roll from the render pose handed to SteamVR.
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

    float rx = -fz;
    const float ry = 0.0f;
    float rz = fx;
    const float rLen = std::sqrt(rx * rx + rz * rz);
    if (!std::isfinite(rLen) || rLen < 0.0001f) {
        if (removedRollDegrees) *removedRollDegrees = 0.0f;
        return input;
    }
    rx /= rLen;
    rz /= rLen;

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
        float iux = input.m[0][1];
        float iuy = input.m[1][1];
        float iuz = input.m[2][1];
        const float iuLen = std::sqrt(iux * iux + iuy * iuy + iuz * iuz);
        if (std::isfinite(iuLen) && iuLen > 0.0001f) {
            iux /= iuLen;
            iuy /= iuLen;
            iuz /= iuLen;
            const float cosRoll =
                std::clamp(iux * ux + iuy * uy + iuz * uz, -1.0f, 1.0f);
            const float sinRoll =
                std::clamp(iux * rx + iuy * ry + iuz * rz, -1.0f, 1.0f);
            *removedRollDegrees =
                std::atan2(sinRoll, cosRoll) * 57.2957795130823208768f;
        } else {
            *removedRollDegrees = 0.0f;
        }
    }

    return output;
}

struct FlatOverlayStateM8 {
    bool enabled = false;
    bool f5WasDown = false;
    bool visible = false;
    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
};

FlatOverlayStateM8 g_flatOverlayM8{};

std::string overlay_error_m8(const char *operation,
                             vr::EVROverlayError result) {
    const char *name = nullptr;
    if (vr::IVROverlay *overlay = vr::VROverlay()) {
        name = overlay->GetOverlayErrorNameFromEnum(result);
    }
    return std::string(operation) + " failed (OpenVR overlay error " +
           std::to_string(static_cast<int>(result)) +
           (name ? std::string(", ") + name : std::string()) + ")";
}

void hide_flat_overlay_m8() {
    if (g_flatOverlayM8.handle == vr::k_ulOverlayHandleInvalid) {
        g_flatOverlayM8.visible = false;
        return;
    }
    if (vr::IVROverlay *overlay = vr::VROverlay()) {
        overlay->HideOverlay(g_flatOverlayM8.handle);
    }
    g_flatOverlayM8.visible = false;
}

bool poll_flat_mode_m8(vr::IVRCompositor *compositor) {
    const bool down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (down && !g_flatOverlayM8.f5WasDown) {
        g_flatOverlayM8.enabled = !g_flatOverlayM8.enabled;
        g_reprojectionM8.haveFramePose = false;

        if (g_flatOverlayM8.enabled) {
            if (compositor) {
                // Explicitly remove the previous full-FOV scene frame. SteamVR
                // then shows its neutral compositor background behind our quad.
                compositor->ClearLastSubmittedFrame();
            }
            log_line("M8 F5: FLAT GUI overlay mode enabled");
        } else {
            hide_flat_overlay_m8();
            log_line("M8 F5: VR world mode restored");
        }
    }
    g_flatOverlayM8.f5WasDown = down;
    return g_flatOverlayM8.enabled;
}

bool ensure_flat_overlay_m8(std::string &error) {
    vr::IVROverlay *overlay = vr::VROverlay();
    if (!overlay) {
        error = "OpenVR overlay interface is unavailable";
        return false;
    }

    if (g_flatOverlayM8.handle == vr::k_ulOverlayHandleInvalid) {
        constexpr char kOverlayKey[] = "fh2vr.flat_gui";
        vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
        vr::EVROverlayError result = overlay->FindOverlay(kOverlayKey, &handle);
        if (result != vr::VROverlayError_None) {
            result = overlay->CreateOverlay(kOverlayKey, "FH2VR Flat GUI", &handle);
        }
        if (result != vr::VROverlayError_None ||
            handle == vr::k_ulOverlayHandleInvalid) {
            error = overlay_error_m8("Create/FindOverlay", result);
            return false;
        }
        g_flatOverlayM8.handle = handle;

        result = overlay->SetOverlayWidthInMeters(handle, 2.4f);
        if (result != vr::VROverlayError_None) {
            error = overlay_error_m8("SetOverlayWidthInMeters", result);
            return false;
        }

        // First manual GUI test: keep the 16:9 screen comfortably in front of
        // the headset. Automatic menu detection/world anchoring comes next.
        vr::HmdMatrix34_t relative{};
        relative.m[0][0] = 1.0f;
        relative.m[1][1] = 1.0f;
        relative.m[2][2] = 1.0f;
        relative.m[2][3] = -2.2f;
        result = overlay->SetOverlayTransformTrackedDeviceRelative(
            handle, vr::k_unTrackedDeviceIndex_Hmd, &relative);
        if (result != vr::VROverlayError_None) {
            error = overlay_error_m8(
                "SetOverlayTransformTrackedDeviceRelative", result);
            return false;
        }

        vr::VRTextureBounds_t bounds{0.0f, 0.0f, 1.0f, 1.0f};
        result = overlay->SetOverlayTextureBounds(handle, &bounds);
        if (result != vr::VROverlayError_None) {
            error = overlay_error_m8("SetOverlayTextureBounds", result);
            return false;
        }

        overlay->SetOverlayAlpha(handle, 1.0f);
        overlay->SetOverlayInputMethod(handle, vr::VROverlayInputMethod_None);
        overlay->SetHighQualityOverlay(handle);
        log_line("M8 flat GUI overlay created: width=2.4m distance=2.2m F5 toggles mode");
    }

    return true;
}

bool submit_flat_overlay_m8(ID3D12Resource *texture,
                            ID3D12CommandQueue *queue,
                            std::string &error) {
    if (!texture || !queue || !ensure_flat_overlay_m8(error)) {
        return false;
    }

    vr::IVROverlay *overlay = vr::VROverlay();
    if (!overlay) {
        error = "OpenVR overlay interface disappeared";
        return false;
    }

    vr::D3D12TextureData_t d3d12Texture{texture, queue, 0};
    vr::Texture_t submitted{};
    submitted.handle = &d3d12Texture;
    submitted.eType = vr::TextureType_DirectX12;
    submitted.eColorSpace = vr::ColorSpace_Gamma;

    vr::EVROverlayError result =
        overlay->SetOverlayTexture(g_flatOverlayM8.handle, &submitted);
    if (result != vr::VROverlayError_None) {
        error = overlay_error_m8("SetOverlayTexture", result);
        return false;
    }

    result = overlay->ShowOverlay(g_flatOverlayM8.handle);
    if (result != vr::VROverlayError_None) {
        error = overlay_error_m8("ShowOverlay", result);
        return false;
    }

    if (!g_flatOverlayM8.visible) {
        g_flatOverlayM8.visible = true;
        log_line("M8 flat GUI overlay submit OK: native D3D12 R8 texture on virtual 16:9 screen");
    }

    error.clear();
    return true;
}

void log_projection_once_m8(const ProjectionM8 &projection,
                            const D3D12_RESOURCE_DESC &desc) {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    constexpr float kRadToDeg = 57.2957795130823208768f;
    char buffer[1100]{};
    std::snprintf(
        buffer, sizeof(buffer),
        "M8 projection/FOV: HMD combined=%.2fx%.2f deg; FH2 source=%llux%u aspect=%.5f targetFov=%.2fx%.2f deg; boundsL=[%.5f %.5f %.5f %.5f] boundsR=[%.5f %.5f %.5f %.5f]",
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

void log_reprojection_stats_m8(UINT64 latestFence) {
    const ULONGLONG now = GetTickCount64();
    if (g_reprojectionM8.lastLogAt == 0) {
        g_reprojectionM8.lastLogAt = now;
        return;
    }
    if (now - g_reprojectionM8.lastLogAt < 1000) {
        return;
    }

    const ULONGLONG elapsed = now - g_reprojectionM8.lastLogAt;
    const double scale = elapsed ? 1000.0 / static_cast<double>(elapsed) : 0.0;
    char buffer[512]{};
    std::snprintf(
        buffer, sizeof(buffer),
        "M8 compositor cadence: %.2f pairs/s; unique FH2 frames=%.2f/s; repeated reprojection submits=%.2f/s; latestFence=%llu",
        static_cast<double>(g_reprojectionM8.pairSubmits) * scale,
        static_cast<double>(g_reprojectionM8.uniqueFrames) * scale,
        static_cast<double>(g_reprojectionM8.repeatedFrames) * scale,
        static_cast<unsigned long long>(latestFence));
    log_line(buffer);

    g_reprojectionM8.pairSubmits = 0;
    g_reprojectionM8.uniqueFrames = 0;
    g_reprojectionM8.repeatedFrames = 0;
    g_reprojectionM8.lastLogAt = now;
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

    const bool flatMode = poll_flat_mode_m8(compositor);
    if (flatMode) {
        const bool ok = submit_flat_overlay_m8(texture, queue, error);
        texture->Release();
        queue->Release();
        return ok;
    }

    hide_flat_overlay_m8();

    ProjectionM8 projection{};
    if (!build_projection_m8(vr::VRSystem(), desc, projection)) {
        texture->Release();
        queue->Release();
        error = "M8 could not build source-aspect-aware OpenVR projection";
        return false;
    }

    update_fov_match_target(
        projection.sourceVerticalFovRadians,
        projection.sourceHorizontalFovRadians,
        projection.sourceAspect,
        projection.combinedVerticalFovRadians,
        projection.combinedHorizontalFovRadians);
    log_projection_once_m8(projection, desc);

    vr::HmdMatrix34_t stableTrackedPose{};
    if (!render_pose_for_frame_m8(compositor, frameFence, stableTrackedPose, error)) {
        texture->Release();
        queue->Release();
        return false;
    }

    float removedRollDegrees = 0.0f;
    const vr::HmdMatrix34_t cameraMatchedPose =
        roll_neutral_pose_m8(stableTrackedPose, &removedRollDegrees);

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
        error = compositor_error("M8 DirectX12 R8 Submit(left)", leftResult);
        return false;
    }

    const vr::EVRCompositorError rightResult = compositor->Submit(
        vr::Eye_Right, &submitted, &projection.right, vr::Submit_TextureWithPose);
    if (rightResult != vr::VRCompositorError_None) {
        texture->Release();
        queue->Release();
        error = compositor_error("M8 DirectX12 R8 Submit(right)", rightResult);
        return false;
    }

    compositor->PostPresentHandoff();
    g_openVrPairSubmits.fetch_add(1, std::memory_order_relaxed);
    ++g_reprojectionM8.pairSubmits;
    log_reprojection_stats_m8(frameFence);

    static std::atomic<bool> loggedSuccess{false};
    if (!loggedSuccess.exchange(true, std::memory_order_acq_rel)) {
        char buffer[896]{};
        std::snprintf(
            buffer, sizeof(buffer),
            "M8 VR world submit OK: roll-neutral render pose (first tracked roll delta %.2f deg); resource=%p queue=%p fence=%llu format=%s(%d); F5=flat GUI",
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
