// M10 keeps the proven M9 capture/UI path and adds positional HMD parallax.
// The scene is still mono, so this is a conservative 6DoF approximation:
// headset translation shifts/scales the source frustum instead of pretending
// to be true depth reprojection or geometric stereo.
#include "RendererM9.cpp"

namespace {

constexpr float kParallaxVirtualPlaneMetersM10 = 0.55f;
constexpr float kParallaxOverscanScaleM10 = 0.76f;

struct ParallaxStateM10 {
    bool haveReference = false;
    vr::HmdMatrix34_t referencePose{};
    ULONGLONG lastSubmitAt = 0;
    bool logged = false;
};

ParallaxStateM10 g_parallaxM10{};

float soft_limit_shift_m10(float value, float negativeRoom, float positiveRoom) {
    negativeRoom = std::max(negativeRoom, 0.0f);
    positiveRoom = std::max(positiveRoom, 0.0f);

    if (value > 0.0f) {
        if (positiveRoom <= 0.00001f) return 0.0f;
        return positiveRoom * std::tanh(value / positiveRoom);
    }
    if (value < 0.0f) {
        if (negativeRoom <= 0.00001f) return 0.0f;
        return -negativeRoom * std::tanh((-value) / negativeRoom);
    }
    return 0.0f;
}

void scaled_axis_m10(float minimum, float maximum, float scale,
                     float &scaledMinimum, float &scaledMaximum) {
    scaledMinimum = 0.5f + (minimum - 0.5f) * scale;
    scaledMaximum = 0.5f + (maximum - 0.5f) * scale;
}

void common_shift_room_m10(const vr::VRTextureBounds_t &left,
                           const vr::VRTextureBounds_t &right,
                           float scale,
                           bool vertical,
                           float &negativeRoom,
                           float &positiveRoom) {
    float lMin = 0.0f, lMax = 0.0f, rMin = 0.0f, rMax = 0.0f;
    if (vertical) {
        scaled_axis_m10(left.vMin, left.vMax, scale, lMin, lMax);
        scaled_axis_m10(right.vMin, right.vMax, scale, rMin, rMax);
    } else {
        scaled_axis_m10(left.uMin, left.uMax, scale, lMin, lMax);
        scaled_axis_m10(right.uMin, right.uMax, scale, rMin, rMax);
    }

    negativeRoom = std::max(0.0f, std::min(lMin, rMin));
    positiveRoom = std::max(0.0f, std::min(1.0f - lMax, 1.0f - rMax));
}

void transform_bounds_m10(vr::VRTextureBounds_t &bounds,
                          float uShift,
                          float vShift,
                          float scale) {
    bounds.uMin = 0.5f + (bounds.uMin - 0.5f) * scale + uShift;
    bounds.uMax = 0.5f + (bounds.uMax - 0.5f) * scale + uShift;
    bounds.vMin = 0.5f + (bounds.vMin - 0.5f) * scale + vShift;
    bounds.vMax = 0.5f + (bounds.vMax - 0.5f) * scale + vShift;

    // Safety only. Normal M10 movement is kept inside the valid texture by
    // soft_limit_shift_m10(), so these clamps should not create a hard stop.
    bounds.uMin = std::clamp(bounds.uMin, 0.0f, 1.0f);
    bounds.uMax = std::clamp(bounds.uMax, 0.0f, 1.0f);
    bounds.vMin = std::clamp(bounds.vMin, 0.0f, 1.0f);
    bounds.vMax = std::clamp(bounds.vMax, 0.0f, 1.0f);
}

void apply_head_position_parallax_m10(ProjectionM9 &projection,
                                      const vr::HmdMatrix34_t &pose) {
    constexpr ULONGLONG kReferenceResetGapMs = 750;
    constexpr float kMaxHorizontalMeters = 0.24f;
    constexpr float kMaxVerticalMeters = 0.22f;
    constexpr float kMaxDepthMeters = 0.24f;

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

    // X is intentionally inverted relative to the previous M10 build. The
    // FH2 camera yaw was already correct; only the screen-space parallax axis
    // had the wrong sign. Y keeps the proven direction.
    const float rawUShift =
        -localX / (2.0f * kParallaxVirtualPlaneMetersM10 * projection.sourceTanHalfX);
    const float rawVShift =
        -localY / (2.0f * kParallaxVirtualPlaneMetersM10 * projection.sourceTanHalfY);

    // Keep a permanent overscan reserve so sideways/up/down movement has real
    // source pixels available instead of immediately colliding with 0..1 UV.
    // Forward/back movement changes the crop inside that reserve.
    const float depthScale = std::clamp(
        1.0f - localForward / kParallaxVirtualPlaneMetersM10,
        0.72f, 1.18f);
    const float scale = kParallaxOverscanScaleM10 * depthScale;

    float uNegativeRoom = 0.0f, uPositiveRoom = 0.0f;
    float vNegativeRoom = 0.0f, vPositiveRoom = 0.0f;
    common_shift_room_m10(projection.left, projection.right, scale, false,
                          uNegativeRoom, uPositiveRoom);
    common_shift_room_m10(projection.left, projection.right, scale, true,
                          vNegativeRoom, vPositiveRoom);

    const float uShift = soft_limit_shift_m10(
        rawUShift, uNegativeRoom, uPositiveRoom);
    const float vShift = soft_limit_shift_m10(
        rawVShift, vNegativeRoom, vPositiveRoom);

    transform_bounds_m10(projection.left, uShift, vShift, scale);
    transform_bounds_m10(projection.right, uShift, vShift, scale);

    if (!g_parallaxM10.logged) {
        g_parallaxM10.logged = true;
        char buffer[384]{};
        std::snprintf(buffer, sizeof(buffer),
                      "M10.2 positional parallax active: corrected X sign, %.2fm virtual plane, %.0f%% overscan, soft UV limits",
                      kParallaxVirtualPlaneMetersM10,
                      (1.0f - kParallaxOverscanScaleM10) * 100.0f);
        log_line(buffer);
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

    // We intentionally render FH2 wider than the headset target. M10.2 then
    // crops that overscan back down while moving the crop window for parallax,
    // preserving roughly the same perceived FOV at the neutral head position.
    const float overscanTanHalfX = projection.sourceTanHalfX / kParallaxOverscanScaleM10;
    const float overscanTanHalfY = projection.sourceTanHalfY / kParallaxOverscanScaleM10;
    const float overscanHorizontalFov = 2.0f * std::atan(overscanTanHalfX);
    const float overscanVerticalFov = 2.0f * std::atan(overscanTanHalfY);

    update_fov_match_target(
        overscanVerticalFov,
        overscanHorizontalFov,
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

    // Real HMD translation drives only our controlled parallax. The OpenVR
    // compositor receives orientation only, preventing a second translation.
    apply_head_position_parallax_m10(projection, cameraMatchedPose);

    vr::HmdMatrix34_t compositorPose = cameraMatchedPose;
    compositorPose.m[0][3] = 0.0f;
    compositorPose.m[1][3] = 0.0f;
    compositorPose.m[2][3] = 0.0f;

    vr::D3D12TextureData_t d3d12Texture{texture, queue, 0};
    vr::VRTextureWithPose_t submitted{};
    submitted.handle = &d3d12Texture;
    submitted.eType = vr::TextureType_DirectX12;
    submitted.eColorSpace = vr::ColorSpace_Gamma;
    submitted.mDeviceToAbsoluteTracking = compositorPose;

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
        char buffer[448]{};
        std::snprintf(buffer, sizeof(buffer),
                      "M10.2 VR scene submit OK: orientation-only compositor pose + stronger positional parallax, first removed roll=%.2f deg",
                      removedRollDegrees);
        log_line(buffer);
    }
    return true;
}

} // namespace

namespace fh2vr {

bool submit_direct_d3d12_mono_preview_m10(vr::IVRCompositor *compositor,
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
