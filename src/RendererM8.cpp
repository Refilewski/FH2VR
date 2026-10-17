#include "RendererPreview.h"
#include "FovMatch.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

#define submit_direct_d3d12_mono_preview submit_direct_d3d12_mono_preview_m3
#include "RendererM3.cpp"
#undef submit_direct_d3d12_mono_preview

namespace {

struct ProjectionM8 {
    vr::VRTextureBounds_t left{};
    vr::VRTextureBounds_t right{};
    float lLeft{}, lRight{}, lTop{}, lBottom{};
    float rLeft{}, rRight{}, rTop{}, rBottom{};
    float combinedTanHalfX{}, combinedTanHalfY{};
    float sourceTanHalfX{}, sourceTanHalfY{}, sourceAspect{};
    float combinedHorizontalFovRadians{}, combinedVerticalFovRadians{};
    float sourceHorizontalFovRadians{}, sourceVerticalFovRadians{};
};

bool build_projection_m8(vr::IVRSystem *system,
                         const D3D12_RESOURCE_DESC &desc,
                         ProjectionM8 &out) {
    if (!system || desc.Width == 0 || desc.Height == 0) return false;

    system->GetProjectionRaw(vr::Eye_Left,
                             &out.lLeft, &out.lRight, &out.lTop, &out.lBottom);
    system->GetProjectionRaw(vr::Eye_Right,
                             &out.rLeft, &out.rRight, &out.rTop, &out.rBottom);

    out.combinedTanHalfX = std::max({-out.lLeft, out.lRight, -out.rLeft, out.rRight});
    out.combinedTanHalfY = std::max({-out.lTop, out.lBottom, -out.rTop, out.rBottom});
    out.sourceAspect = static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
    if (out.combinedTanHalfX <= 0.0001f ||
        out.combinedTanHalfY <= 0.0001f ||
        out.sourceAspect <= 0.1f) {
        return false;
    }

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
    return true;
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
        error = "M8 frame pose unavailable";
        return false;
    }

    if (!g_reprojectionM8.haveFramePose || g_reprojectionM8.frameFence != frameFence) {
        vr::TrackedDevicePose_t renderPose{};
        const vr::EVRCompositorError result = compositor->GetLastPoseForTrackedDeviceIndex(
            vr::k_unTrackedDeviceIndex_Hmd, &renderPose, nullptr);
        if (result != vr::VRCompositorError_None || !renderPose.bPoseIsValid) {
            error = compositor_error("M8 GetLastPoseForTrackedDeviceIndex", result);
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
    float fx = -input.m[0][2];
    float fy = -input.m[1][2];
    float fz = -input.m[2][2];
    const float fLen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (!std::isfinite(fLen) || fLen < 0.0001f) {
        if (removedRollDegrees) *removedRollDegrees = 0.0f;
        return input;
    }
    fx /= fLen; fy /= fLen; fz /= fLen;

    float rx = -fz;
    const float ry = 0.0f;
    float rz = fx;
    const float rLen = std::sqrt(rx * rx + rz * rz);
    if (!std::isfinite(rLen) || rLen < 0.0001f) {
        if (removedRollDegrees) *removedRollDegrees = 0.0f;
        return input;
    }
    rx /= rLen; rz /= rLen;

    const float ux = ry * fz - rz * fy;
    const float uy = rz * fx - rx * fz;
    const float uz = rx * fy - ry * fx;

    vr::HmdMatrix34_t out = input;
    out.m[0][0] = rx; out.m[1][0] = ry; out.m[2][0] = rz;
    out.m[0][1] = ux; out.m[1][1] = uy; out.m[2][1] = uz;
    out.m[0][2] = -fx; out.m[1][2] = -fy; out.m[2][2] = -fz;

    if (removedRollDegrees) {
        float iux = input.m[0][1], iuy = input.m[1][1], iuz = input.m[2][1];
        const float iuLen = std::sqrt(iux * iux + iuy * iuy + iuz * iuz);
        if (std::isfinite(iuLen) && iuLen > 0.0001f) {
            iux /= iuLen; iuy /= iuLen; iuz /= iuLen;
            const float cosRoll = std::clamp(iux * ux + iuy * uy + iuz * uz, -1.0f, 1.0f);
            const float sinRoll = std::clamp(iux * rx + iuy * ry + iuz * rz, -1.0f, 1.0f);
            *removedRollDegrees = std::atan2(sinRoll, cosRoll) * 57.29577951308232f;
        } else {
            *removedRollDegrees = 0.0f;
        }
    }
    return out;
}

struct FlatOverlayStateM8 {
    bool enabled = false;
    bool f5WasDown = false;
    bool visible = false;
    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
};

FlatOverlayStateM8 g_flatOverlayM8{};

std::string overlay_error_m8(const char *operation, vr::EVROverlayError result) {
    const char *name = nullptr;
    if (vr::IVROverlay *overlay = vr::VROverlay()) {
        name = overlay->GetOverlayErrorNameFromEnum(result);
    }
    return std::string(operation) + " failed (OpenVR overlay error " +
           std::to_string(static_cast<int>(result)) +
           (name ? std::string(", ") + name : std::string()) + ")";
}

void hide_flat_overlay_m8() {
    if (g_flatOverlayM8.handle != vr::k_ulOverlayHandleInvalid) {
        if (vr::IVROverlay *overlay = vr::VROverlay()) {
            overlay->HideOverlay(g_flatOverlayM8.handle);
        }
    }
    g_flatOverlayM8.visible = false;
}

bool poll_flat_mode_m8(vr::IVRCompositor *compositor) {
    const bool down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (down && !g_flatOverlayM8.f5WasDown) {
        g_flatOverlayM8.enabled = !g_flatOverlayM8.enabled;
        g_reprojectionM8.haveFramePose = false;
        if (g_flatOverlayM8.enabled) {
            if (compositor) compositor->ClearLastSubmittedFrame();
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
        error = "OpenVR overlay interface unavailable";
        return false;
    }

    if (g_flatOverlayM8.handle != vr::k_ulOverlayHandleInvalid) return true;

    constexpr char kOverlayKey[] = "fh2vr.flat_gui";
    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
    vr::EVROverlayError result = overlay->FindOverlay(kOverlayKey, &handle);
    if (result != vr::VROverlayError_None) {
        result = overlay->CreateOverlay(kOverlayKey, "FH2VR Flat GUI", &handle);
    }
    if (result != vr::VROverlayError_None || handle == vr::k_ulOverlayHandleInvalid) {
        error = overlay_error_m8("Create/FindOverlay", result);
        return false;
    }
    g_flatOverlayM8.handle = handle;

    result = overlay->SetOverlayWidthInMeters(handle, 2.4f);
    if (result != vr::VROverlayError_None) {
        error = overlay_error_m8("SetOverlayWidthInMeters", result);
        return false;
    }

    vr::HmdMatrix34_t relative{};
    relative.m[0][0] = 1.0f;
    relative.m[1][1] = 1.0f;
    relative.m[2][2] = 1.0f;
    relative.m[2][3] = -2.2f;
    result = overlay->SetOverlayTransformTrackedDeviceRelative(
        handle, vr::k_unTrackedDeviceIndex_Hmd, &relative);
    if (result != vr::VROverlayError_None) {
        error = overlay_error_m8("SetOverlayTransformTrackedDeviceRelative", result);
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
    log_line("M8 flat GUI overlay created: width=2.4m distance=2.2m; F5 toggles mode");
    return true;
}

bool submit_flat_overlay_m8(ID3D12Resource *texture,
                            ID3D12CommandQueue *queue,
                            std::string &error) {
    if (!texture || !queue || !ensure_flat_overlay_m8(error)) return false;

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

    vr::EVROverlayError result = overlay->SetOverlayTexture(g_flatOverlayM8.handle, &submitted);
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

void log_projection_once_m8(const ProjectionM8 &p, const D3D12_RESOURCE_DESC &desc) {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true, std::memory_order_acq_rel)) return;
    constexpr float kRadToDeg = 57.29577951308232f;
    char buffer[900]{};
    std::snprintf(buffer, sizeof(buffer),
                  "M8 projection/FOV: HMD=%.2fx%.2f deg; FH2=%llux%u aspect=%.5f target=%.2fx%.2f deg; F5=flat GUI",
                  p.combinedHorizontalFovRadians * kRadToDeg,
                  p.combinedVerticalFovRadians * kRadToDeg,
                  static_cast<unsigned long long>(desc.Width), desc.Height,
                  p.sourceAspect,
                  p.sourceHorizontalFovRadians * kRadToDeg,
                  p.sourceVerticalFovRadians * kRadToDeg);
    log_line(buffer);
}

void log_reprojection_stats_m8(UINT64 latestFence) {
    const ULONGLONG now = GetTickCount64();
    if (g_reprojectionM8.lastLogAt == 0) {
        g_reprojectionM8.lastLogAt = now;
        return;
    }
    if (now - g_reprojectionM8.lastLogAt < 1000) return;

    const ULONGLONG elapsed = now - g_reprojectionM8.lastLogAt;
    const double scale = elapsed ? 1000.0 / static_cast<double>(elapsed) : 0.0;
    char buffer[512]{};
    std::snprintf(buffer, sizeof(buffer),
                  "M8 compositor cadence: %.2f pairs/s; unique FH2=%.2f/s; repeated=%.2f/s; fence=%llu",
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
        error = "OpenVR compositor unavailable";
        return false;
    }

    ID3D12CommandQueue *queue = g_primaryQueue.load(std::memory_order_acquire);
    const int latest = g_latestSlot.load(std::memory_order_acquire);
    if (!queue || latest < 0 || latest > 1) {
        error = "D3D12 converted frame not ready";
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
        error = "latest converted R8 frame unavailable";
        return false;
    }

    if (poll_flat_mode_m8(compositor)) {
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
        error = "M8 projection build failed";
        return false;
    }

    update_fov_match_target(projection.sourceVerticalFovRadians,
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
        error = compositor_error("M8 Submit(left)", leftResult);
        return false;
    }

    const vr::EVRCompositorError rightResult = compositor->Submit(
        vr::Eye_Right, &submitted, &projection.right, vr::Submit_TextureWithPose);
    if (rightResult != vr::VRCompositorError_None) {
        texture->Release();
        queue->Release();
        error = compositor_error("M8 Submit(right)", rightResult);
        return false;
    }

    compositor->PostPresentHandoff();
    g_openVrPairSubmits.fetch_add(1, std::memory_order_relaxed);
    ++g_reprojectionM8.pairSubmits;
    log_reprojection_stats_m8(frameFence);

    static std::atomic<bool> loggedSuccess{false};
    if (!loggedSuccess.exchange(true, std::memory_order_acq_rel)) {
        char buffer[640]{};
        std::snprintf(buffer, sizeof(buffer),
                      "M8 VR world submit OK: roll-neutral pose, first removed roll=%.2f deg; F5=flat GUI",
                      removedRollDegrees);
        log_line(buffer);
    }

    texture->Release();
    queue->Release();
    error.clear();
    return true;
}

} // namespace fh2vr
