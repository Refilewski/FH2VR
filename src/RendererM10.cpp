// M10 keeps the proven M9 capture/UI path and adds positional HMD parallax.
// The scene is still mono, so this is a conservative 6DoF approximation:
// headset translation shifts/scales the source frustum instead of pretending
// to be true depth reprojection or geometric stereo.
#define submit_direct_d3d12_mono_preview submit_direct_d3d12_mono_preview_m9
#include "RendererM9.cpp"
#undef submit_direct_d3d12_mono_preview

namespace {

struct ParallaxStateM10 {
    bool haveReference = false;
    vr::HmdMatrix34_t referencePose{};
    ULONGLONG lastSubmitAt = 0;
    bool logged = false;
};

ParallaxStateM10 g_parallaxM10{};

void clamp_bounds_axis_m10(float &minimum, float &maximum) {
    const float span = maximum - minimum;
    if (!(span > 0.0f) || !std::isfinite(span)) {
        return;
    }
    if (span >= 1.0f) {
        minimum = 0.0f;
        maximum = 1.0f;
        return;
    }
    if (minimum < 0.0f) {
        maximum -= minimum;
        minimum = 0.0f;
    }
    if (maximum > 1.0f) {
        const float excess = maximum - 1.0f;
        minimum -= excess;
        maximum = 1.0f;
    }
    minimum = std::clamp(minimum, 0.0f, 1.0f);
    maximum = std::clamp(maximum, 0.0f, 1.0f);
}

void transform_bounds_m10(vr::VRTextureBounds_t &bounds,
                          float uShift,
                          float vShift,
                          float scale) {
    bounds.uMin = 0.5f + (bounds.uMin - 0.5f) * scale + uShift;
    bounds.uMax = 0.5f + (bounds.uMax - 0.5f) * scale + uShift;
    bounds.vMin = 0.5f + (bounds.vMin - 0.5f) * scale + vShift;
    bounds.vMax = 0.5f + (bounds.vMax - 0.5f) * scale + vShift;
    clamp_bounds_axis_m10(bounds.uMin, bounds.uMax);
    clamp_bounds_axis_m10(bounds.vMin, bounds.vMax);
}

void apply_head_position_parallax_m10(ProjectionM9 &projection,
                                      const vr::HmdMatrix34_t &pose) {
    constexpr ULONGLONG kReferenceResetGapMs = 750;
    constexpr float kVirtualPlaneMeters = 1.60f;
    constexpr float kMaxHorizontalMeters = 0.18f;
    constexpr float kMaxVerticalMeters = 0.18f;
    constexpr float kMaxDepthMeters = 0.20f;

    const ULONGLONG now = GetTickCount64();
    if (!g_parallaxM10.haveReference ||
        (g_parallaxM10.lastSubmitAt &&
         now - g_parallaxM10.lastSubmitAt > kReferenceResetGapMs)) {
        g_parallaxM10.referencePose = pose;
        g_parallaxM10.haveReference = true;
        g_parallaxM10.logged = false;
    }
    g_parallaxM10.lastSubmitAt = now;

    const vr::HmdMatrix34_t &reference = g_parallaxM10.referencePose;
    const float dx = pose.m[0][3] - reference.m[0][3];
    const float dy = pose.m[1][3] - reference.m[1][3];
    const float dz = pose.m[2][3] - reference.m[2][3];

    // Convert tracking-space translation into the recentered headset basis.
    const float localX = std::clamp(
        dx * reference.m[0][0] + dy * reference.m[1][0] + dz * reference.m[2][0],
        -kMaxHorizontalMeters, kMaxHorizontalMeters);
    const float localY = std::clamp(
        dx * reference.m[0][1] + dy * reference.m[1][1] + dz * reference.m[2][1],
        -kMaxVerticalMeters, kMaxVerticalMeters);
    const float localForward = std::clamp(
        -(dx * reference.m[0][2] + dy * reference.m[1][2] + dz * reference.m[2][2]),
        -kMaxDepthMeters, kMaxDepthMeters);

    if (!std::isfinite(localX) || !std::isfinite(localY) ||
        !std::isfinite(localForward) || projection.sourceTanHalfX <= 0.0001f ||
        projection.sourceTanHalfY <= 0.0001f) {
        return;
    }

    // Moving the head right/up should move the virtual world left/down.
    // Sampling farther right/up from the source produces that screen-space
    // response. Z translation adds a restrained zoom around source center.
    const float uShift =
        localX / (2.0f * kVirtualPlaneMeters * projection.sourceTanHalfX);
    const float vShift =
        -localY / (2.0f * kVirtualPlaneMeters * projection.sourceTanHalfY);
    const float scale = std::clamp(
        1.0f - localForward / kVirtualPlaneMeters, 0.84f, 1.16f);

    transform_bounds_m10(projection.left, uShift, vShift, scale);
    transform_bounds_m10(projection.right, uShift, vShift, scale);

    if (!g_parallaxM10.logged) {
        g_parallaxM10.logged = true;
        log_line("M10 positional parallax active: HMD X/Y/Z -> source-frustum shift/scale, 1.60m virtual plane");
    }
}

bool submit_scene_m10(vr::IVRCompositor *compositor,
                      ID3D12Resource *texture,
                      ID3D12CommandQueue *queue,
                      const D3D12_RESOURCE_DESC &desc,
                      UINT64 frameFence,
                      std::string &error) {
    ProjectionM9 projection{};
    if (!build_projection_m9(vr::VRSystem(), desc, projection)) {
        error = "M10 projection build failed";
        return false;
    }

    update_fov_match_target(
        projection.sourceVerticalFovRadians,
        projection.sourceHorizontalFovRadians,
        projection.sourceAspect,
        projection.combinedVerticalFovRadians,
        projection.combinedHorizontalFovRadians);
    log_projection_once_m9(projection, desc);

    vr::HmdMatrix34_t stableTrackedPose{};
    if (!render_pose_for_frame_m9(
            compositor, frameFence, stableTrackedPose, error)) {
        return false;
    }

    float removedRollDegrees = 0.0f;
    const vr::HmdMatrix34_t cameraMatchedPose =
        roll_neutral_pose_m9(stableTrackedPose, &removedRollDegrees);

    apply_head_position_parallax_m10(projection, cameraMatchedPose);

    vr::D3D12TextureData_t d3d12Texture{texture, queue, 0};
    vr::VRTextureWithPose_t submitted{};
    submitted.handle = &d3d12Texture;
    submitted.eType = vr::TextureType_DirectX12;
    submitted.eColorSpace = vr::ColorSpace_Gamma;
    submitted.mDeviceToAbsoluteTracking = cameraMatchedPose;

    const vr::EVRCompositorError leftResult = compositor->Submit(
        vr::Eye_Left, &submitted, &projection.left, vr::Submit_TextureWithPose);
    if (leftResult != vr::VRCompositorError_None) {
        error = compositor_error("M10 Submit(left)", leftResult);
        return false;
    }
    const vr::EVRCompositorError rightResult = compositor->Submit(
        vr::Eye_Right, &submitted, &projection.right, vr::Submit_TextureWithPose);
    if (rightResult != vr::VRCompositorError_None) {
        error = compositor_error("M10 Submit(right)", rightResult);
        return false;
    }

    compositor->PostPresentHandoff();
    g_openVrPairSubmits.fetch_add(1, std::memory_order_relaxed);
    ++g_reprojectionM9.pairSubmits;
    log_reprojection_stats_m9(frameFence);

    static std::atomic<bool> loggedSuccess{false};
    if (!loggedSuccess.exchange(true, std::memory_order_acq_rel)) {
        char buffer[384]{};
        std::snprintf(buffer, sizeof(buffer),
                      "M10 VR scene submit OK: roll-neutral + positional parallax, first removed roll=%.2f deg",
                      removedRollDegrees);
        log_line(buffer);
    }
    return true;
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
    queue->AddRef();

    const bool showOverlay = poll_gui_m9();
    if (!showOverlay) {
        hide_overlay_m9();
    }

    if (g_guiM9.automaticEnabled && !g_frozenM9.active) {
        std::string freezeError;
        if (!freeze_previous_gameplay_m9(latest, freezeError) && !freezeError.empty()) {
            log_line(freezeError);
        }
    }

    ID3D12Resource *latestTexture = nullptr;
    D3D12_RESOURCE_DESC desc{};
    UINT64 latestFence = 0;
    if (!acquire_slot_texture_m9(latest, latestTexture, desc, latestFence)) {
        queue->Release();
        error = "latest converted R8 frame unavailable";
        return false;
    }

    ID3D12Resource *sceneTexture = latestTexture;
    UINT64 sceneFence = latestFence;
    if (g_guiM9.automaticEnabled && g_frozenM9.active && g_frozenM9.hasContent) {
        sceneTexture = g_frozenM9.texture;
        sceneTexture->AddRef();
        sceneFence = g_frozenM9.sourceFrameFence;
    } else {
        sceneTexture->AddRef();
    }

    const bool sceneOk = submit_scene_m10(
        compositor, sceneTexture, queue, desc, sceneFence, error);
    sceneTexture->Release();
    if (!sceneOk) {
        latestTexture->Release();
        queue->Release();
        return false;
    }

    if (showOverlay && !submit_overlay_once_m9(latestTexture, queue, error)) {
        latestTexture->Release();
        queue->Release();
        return false;
    }

    latestTexture->Release();
    queue->Release();
    error.clear();
    return true;
}

} // namespace fh2vr
