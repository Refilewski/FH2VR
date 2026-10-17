#include "RendererPreview.h"
#include "FovMatch.h"
#include "GameBaseLocator.h"
#include "UiModeDetector.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

// Keep the proven native D3D12 capture + R10->R8 conversion path unchanged.
#define submit_direct_d3d12_mono_preview submit_direct_d3d12_mono_preview_m3
#include "RendererM3.cpp"
#undef submit_direct_d3d12_mono_preview

namespace {

struct ProjectionM9 {
    vr::VRTextureBounds_t left{};
    vr::VRTextureBounds_t right{};
    float lLeft{}, lRight{}, lTop{}, lBottom{};
    float rLeft{}, rRight{}, rTop{}, rBottom{};
    float combinedTanHalfX{}, combinedTanHalfY{};
    float sourceTanHalfX{}, sourceTanHalfY{}, sourceAspect{};
    float combinedHorizontalFovRadians{}, combinedVerticalFovRadians{};
    float sourceHorizontalFovRadians{}, sourceVerticalFovRadians{};
};

bool build_projection_m9(vr::IVRSystem *system,
                         const D3D12_RESOURCE_DESC &desc,
                         ProjectionM9 &out) {
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
    return std::isfinite(out.sourceHorizontalFovRadians) &&
           std::isfinite(out.sourceVerticalFovRadians);
}

struct ReprojectionStateM9 {
    UINT64 frameFence = 0;
    vr::HmdMatrix34_t frameRenderPose{};
    bool haveFramePose = false;
    std::uint64_t pairSubmits = 0;
    std::uint64_t uniqueFrames = 0;
    std::uint64_t repeatedFrames = 0;
    ULONGLONG lastLogAt = 0;
};

ReprojectionStateM9 g_reprojectionM9{};

bool render_pose_for_frame_m9(vr::IVRCompositor *compositor,
                              UINT64 frameFence,
                              vr::HmdMatrix34_t &pose,
                              std::string &error) {
    if (!compositor || frameFence == 0) {
        error = "M9 frame pose unavailable";
        return false;
    }

    if (!g_reprojectionM9.haveFramePose ||
        g_reprojectionM9.frameFence != frameFence) {
        vr::TrackedDevicePose_t renderPose{};
        const vr::EVRCompositorError result =
            compositor->GetLastPoseForTrackedDeviceIndex(
                vr::k_unTrackedDeviceIndex_Hmd, &renderPose, nullptr);
        if (result != vr::VRCompositorError_None || !renderPose.bPoseIsValid) {
            error = compositor_error("M9 GetLastPoseForTrackedDeviceIndex", result);
            return false;
        }
        g_reprojectionM9.frameFence = frameFence;
        g_reprojectionM9.frameRenderPose = renderPose.mDeviceToAbsoluteTracking;
        g_reprojectionM9.haveFramePose = true;
        ++g_reprojectionM9.uniqueFrames;
    } else {
        ++g_reprojectionM9.repeatedFrames;
    }

    pose = g_reprojectionM9.frameRenderPose;
    return true;
}

vr::HmdMatrix34_t roll_neutral_pose_m9(const vr::HmdMatrix34_t &input,
                                        float *removedRollDegrees) {
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

    vr::HmdMatrix34_t out = input;
    out.m[0][0] = rx; out.m[1][0] = ry; out.m[2][0] = rz;
    out.m[0][1] = ux; out.m[1][1] = uy; out.m[2][1] = uz;
    out.m[0][2] = -fx; out.m[1][2] = -fy; out.m[2][2] = -fz;

    if (removedRollDegrees) {
        float iux = input.m[0][1];
        float iuy = input.m[1][1];
        float iuz = input.m[2][1];
        const float iuLen = std::sqrt(iux * iux + iuy * iuy + iuz * iuz);
        if (std::isfinite(iuLen) && iuLen > 0.0001f) {
            iux /= iuLen; iuy /= iuLen; iuz /= iuLen;
            const float cosRoll =
                std::clamp(iux * ux + iuy * uy + iuz * uz, -1.0f, 1.0f);
            const float sinRoll =
                std::clamp(iux * rx + iuy * ry + iuz * rz, -1.0f, 1.0f);
            *removedRollDegrees =
                std::atan2(sinRoll, cosRoll) * 57.29577951308232f;
        } else {
            *removedRollDegrees = 0.0f;
        }
    }
    return out;
}

struct GuiStateM9 {
    bool manualEnabled = false;
    bool f5WasDown = false;
    bool automaticEnabled = false;
    bool visible = false;
    std::uintptr_t gameBase = 0;
    ULONGLONG nextLocateAt = 0;
    ULONGLONG nextUiSampleAt = 0;
    ULONGLONG unknownSince = 0;
    fh2vr::UiMode lastMode = fh2vr::UiMode::Unknown;
    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
};

GuiStateM9 g_guiM9{};

struct FrozenWorldM9 {
    ID3D12Resource *texture = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *commandList = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE eventHandle = nullptr;
    UINT64 nextFence = 1;
    UINT64 sourceFrameFence = 0;
    D3D12_RESOURCE_DESC desc{};
    bool hasContent = false;
    bool active = false;
};

FrozenWorldM9 g_frozenM9{};

std::string overlay_error_m9(const char *operation,
                             vr::EVROverlayError result) {
    const char *name = nullptr;
    if (vr::IVROverlay *overlay = vr::VROverlay()) {
        name = overlay->GetOverlayErrorNameFromEnum(result);
    }
    return std::string(operation) + " failed (OpenVR overlay error " +
           std::to_string(static_cast<int>(result)) +
           (name ? std::string(", ") + name : std::string()) + ")";
}

void invalidate_overlay_handle_m9(bool destroyIfPossible) {
    const vr::VROverlayHandle_t old = g_guiM9.handle;
    if (old != vr::k_ulOverlayHandleInvalid && destroyIfPossible) {
        if (vr::IVROverlay *overlay = vr::VROverlay()) {
            overlay->HideOverlay(old);
            overlay->DestroyOverlay(old);
        }
    }
    g_guiM9.handle = vr::k_ulOverlayHandleInvalid;
    g_guiM9.visible = false;
}

void hide_overlay_m9() {
    if (g_guiM9.handle != vr::k_ulOverlayHandleInvalid) {
        if (vr::IVROverlay *overlay = vr::VROverlay()) {
            const vr::EVROverlayError result = overlay->HideOverlay(g_guiM9.handle);
            if (result == vr::VROverlayError_InvalidHandle) {
                invalidate_overlay_handle_m9(false);
                return;
            }
        }
    }
    g_guiM9.visible = false;
}

bool ensure_overlay_m9(std::string &error) {
    vr::IVROverlay *overlay = vr::VROverlay();
    if (!overlay) {
        error = "OpenVR overlay interface unavailable";
        return false;
    }
    if (g_guiM9.handle != vr::k_ulOverlayHandleInvalid) {
        return true;
    }

    constexpr char kOverlayKey[] = "fh2vr.flat_gui";
    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
    vr::EVROverlayError result = overlay->FindOverlay(kOverlayKey, &handle);
    if (result != vr::VROverlayError_None) {
        result = overlay->CreateOverlay(kOverlayKey, "FH2VR Flat GUI", &handle);
    }
    if (result != vr::VROverlayError_None ||
        handle == vr::k_ulOverlayHandleInvalid) {
        error = overlay_error_m9("Create/FindOverlay", result);
        return false;
    }
    g_guiM9.handle = handle;

    result = overlay->SetOverlayWidthInMeters(handle, 2.4f);
    if (result != vr::VROverlayError_None) {
        invalidate_overlay_handle_m9(false);
        error = overlay_error_m9("SetOverlayWidthInMeters", result);
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
        invalidate_overlay_handle_m9(false);
        error = overlay_error_m9("SetOverlayTransformTrackedDeviceRelative", result);
        return false;
    }

    vr::VRTextureBounds_t bounds{0.0f, 0.0f, 1.0f, 1.0f};
    result = overlay->SetOverlayTextureBounds(handle, &bounds);
    if (result != vr::VROverlayError_None) {
        invalidate_overlay_handle_m9(false);
        error = overlay_error_m9("SetOverlayTextureBounds", result);
        return false;
    }

    overlay->SetOverlayAlpha(handle, 1.0f);
    overlay->SetOverlayInputMethod(handle, vr::VROverlayInputMethod_None);
    log_line("M9 GUI overlay created/recreated: width=2.4m distance=2.2m");
    return true;
}

bool submit_overlay_once_m9(ID3D12Resource *texture,
                            ID3D12CommandQueue *queue,
                            std::string &error) {
    if (!texture || !queue || !ensure_overlay_m9(error)) {
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
        overlay->SetOverlayTexture(g_guiM9.handle, &submitted);
    if (result == vr::VROverlayError_InvalidHandle) {
        // F6/F9 restarts OpenVR. Overlay handles are session-local, so never
        // keep using the old numeric handle after a runtime restart.
        invalidate_overlay_handle_m9(false);
        if (!ensure_overlay_m9(error)) {
            return false;
        }
        result = overlay->SetOverlayTexture(g_guiM9.handle, &submitted);
    }
    if (result != vr::VROverlayError_None) {
        error = overlay_error_m9("SetOverlayTexture", result);
        return false;
    }

    result = overlay->ShowOverlay(g_guiM9.handle);
    if (result == vr::VROverlayError_InvalidHandle) {
        invalidate_overlay_handle_m9(false);
        if (!ensure_overlay_m9(error)) {
            return false;
        }
        result = overlay->SetOverlayTexture(g_guiM9.handle, &submitted);
        if (result == vr::VROverlayError_None) {
            result = overlay->ShowOverlay(g_guiM9.handle);
        }
    }
    if (result != vr::VROverlayError_None) {
        error = overlay_error_m9("ShowOverlay", result);
        return false;
    }

    if (!g_guiM9.visible) {
        g_guiM9.visible = true;
        log_line("M9 flat GUI overlay submit OK");
    }
    error.clear();
    return true;
}

bool desc_matches_m9(const D3D12_RESOURCE_DESC &a,
                     const D3D12_RESOURCE_DESC &b) {
    return a.Dimension == b.Dimension &&
           a.Width == b.Width &&
           a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize &&
           a.MipLevels == b.MipLevels &&
           a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality;
}

void release_frozen_world_m9() {
    if (g_frozenM9.commandList) {
        g_frozenM9.commandList->Release();
        g_frozenM9.commandList = nullptr;
    }
    if (g_frozenM9.allocator) {
        g_frozenM9.allocator->Release();
        g_frozenM9.allocator = nullptr;
    }
    if (g_frozenM9.texture) {
        g_frozenM9.texture->Release();
        g_frozenM9.texture = nullptr;
    }
    if (g_frozenM9.fence) {
        g_frozenM9.fence->Release();
        g_frozenM9.fence = nullptr;
    }
    if (g_frozenM9.eventHandle) {
        CloseHandle(g_frozenM9.eventHandle);
        g_frozenM9.eventHandle = nullptr;
    }
    g_frozenM9.nextFence = 1;
    g_frozenM9.sourceFrameFence = 0;
    g_frozenM9.desc = {};
    g_frozenM9.hasContent = false;
    g_frozenM9.active = false;
}

bool ensure_frozen_resources_m9(ID3D12Device *device,
                                const D3D12_RESOURCE_DESC &desc) {
    if (!device) {
        return false;
    }
    if (g_frozenM9.texture &&
        g_frozenM9.allocator &&
        g_frozenM9.commandList &&
        g_frozenM9.fence &&
        g_frozenM9.eventHandle &&
        desc_matches_m9(g_frozenM9.desc, desc)) {
        return true;
    }

    release_frozen_world_m9();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&g_frozenM9.texture))) || !g_frozenM9.texture) {
        release_frozen_world_m9();
        return false;
    }
    if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&g_frozenM9.allocator))) || !g_frozenM9.allocator) {
        release_frozen_world_m9();
        return false;
    }
    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frozenM9.allocator,
            nullptr, IID_PPV_ARGS(&g_frozenM9.commandList))) ||
        !g_frozenM9.commandList) {
        release_frozen_world_m9();
        return false;
    }
    if (FAILED(g_frozenM9.commandList->Close())) {
        release_frozen_world_m9();
        return false;
    }
    if (FAILED(device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&g_frozenM9.fence))) || !g_frozenM9.fence) {
        release_frozen_world_m9();
        return false;
    }
    g_frozenM9.eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_frozenM9.eventHandle) {
        release_frozen_world_m9();
        return false;
    }

    g_frozenM9.desc = desc;
    log_line("M9 allocated frozen gameplay texture for menu background");
    return true;
}

bool wait_frozen_fence_m9(UINT64 value) {
    if (!value || !g_frozenM9.fence) {
        return true;
    }
    if (g_frozenM9.fence->GetCompletedValue() >= value) {
        return true;
    }
    if (!g_frozenM9.eventHandle ||
        FAILED(g_frozenM9.fence->SetEventOnCompletion(value, g_frozenM9.eventHandle))) {
        return false;
    }
    return WaitForSingleObject(g_frozenM9.eventHandle, 1000) == WAIT_OBJECT_0;
}

bool freeze_previous_gameplay_m9(int latestSlot, std::string &error) {
    if (latestSlot < 0 || latestSlot > 1) {
        return false;
    }

    const int candidateIndex = latestSlot ^ 1;
    ID3D12CommandQueue *queue = g_primaryQueue.load(std::memory_order_acquire);
    ID3D12Device *device = g_primaryDevice.load(std::memory_order_acquire);
    if (!queue || !device) {
        return false;
    }
    queue->AddRef();
    device->AddRef();

    ID3D12Resource *candidate = nullptr;
    UINT64 candidateFence = 0;
    D3D12_RESOURCE_DESC desc{};

    AcquireSRWLockShared(&g_resourceLock);
    if (g_resourcesReady &&
        g_slots[candidateIndex].hasFrame &&
        g_slots[candidateIndex].submit) {
        candidate = g_slots[candidateIndex].submit;
        candidate->AddRef();
        candidateFence = g_slots[candidateIndex].fenceValue;
        desc = g_submitDesc;
    }
    ReleaseSRWLockShared(&g_resourceLock);

    if (!candidate || !candidateFence) {
        if (candidate) candidate->Release();
        device->Release();
        queue->Release();
        return false;
    }

    if (!ensure_frozen_resources_m9(device, desc)) {
        candidate->Release();
        device->Release();
        queue->Release();
        error = "M9 failed to create frozen gameplay resources";
        return false;
    }

    const UINT64 previousCopy = g_frozenM9.nextFence - 1;
    if (!wait_frozen_fence_m9(previousCopy)) {
        candidate->Release();
        device->Release();
        queue->Release();
        error = "M9 frozen gameplay fence wait timed out";
        return false;
    }

    AcquireSRWLockShared(&g_resourceLock);
    if (FAILED(g_frozenM9.allocator->Reset()) ||
        FAILED(g_frozenM9.commandList->Reset(g_frozenM9.allocator, nullptr))) {
        ReleaseSRWLockShared(&g_resourceLock);
        candidate->Release();
        device->Release();
        queue->Release();
        error = "M9 frozen gameplay command list reset failed";
        return false;
    }

    if (g_frozenM9.hasContent) {
        const D3D12_RESOURCE_BARRIER frozenToCopy = transition_barrier(
            g_frozenM9.texture,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
        g_frozenM9.commandList->ResourceBarrier(1, &frozenToCopy);
    }

    const D3D12_RESOURCE_BARRIER sourceToCopy = transition_barrier(
        candidate,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_frozenM9.commandList->ResourceBarrier(1, &sourceToCopy);
    g_frozenM9.commandList->CopyResource(g_frozenM9.texture, candidate);

    const D3D12_RESOURCE_BARRIER sourceBack = transition_barrier(
        candidate,
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const D3D12_RESOURCE_BARRIER frozenToShader = transition_barrier(
        g_frozenM9.texture,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D3D12_RESOURCE_BARRIER endBarriers[2]{sourceBack, frozenToShader};
    g_frozenM9.commandList->ResourceBarrier(2, endBarriers);

    if (FAILED(g_frozenM9.commandList->Close())) {
        ReleaseSRWLockShared(&g_resourceLock);
        candidate->Release();
        device->Release();
        queue->Release();
        error = "M9 frozen gameplay command list close failed";
        return false;
    }

    ID3D12CommandList *lists[] = {g_frozenM9.commandList};
    g_internalQueueSubmit = true;
    queue->ExecuteCommandLists(1, lists);
    g_internalQueueSubmit = false;
    const UINT64 copyFence = g_frozenM9.nextFence++;
    const HRESULT signalResult = queue->Signal(g_frozenM9.fence, copyFence);
    ReleaseSRWLockShared(&g_resourceLock);

    candidate->Release();
    device->Release();
    if (FAILED(signalResult) || !wait_frozen_fence_m9(copyFence)) {
        queue->Release();
        error = "M9 frozen gameplay GPU copy failed";
        return false;
    }
    queue->Release();

    g_frozenM9.sourceFrameFence = candidateFence;
    g_frozenM9.hasContent = true;
    g_frozenM9.active = true;
    log_line("M9 froze previous gameplay frame for background behind automatic GUI");
    return true;
}

void sample_automatic_ui_m9() {
    constexpr ULONGLONG kSampleIntervalMs = 50;
    constexpr ULONGLONG kUnknownReleaseMs = 500;
    constexpr ULONGLONG kLocateRetryMs = 2000;

    const ULONGLONG now = GetTickCount64();
    if (now < g_guiM9.nextUiSampleAt) {
        return;
    }
    g_guiM9.nextUiSampleAt = now + kSampleIntervalMs;

    if (!g_guiM9.gameBase && now >= g_guiM9.nextLocateAt) {
        const std::uintptr_t found = fh2vr::find_fh2_game_base();
        if (found) {
            g_guiM9.gameBase = found;
            fh2vr::set_ui_game_base(found);
            char buffer[192]{};
            std::snprintf(buffer, sizeof(buffer),
                          "M9 FH2 UI detector attached to game image base 0x%llX",
                          static_cast<unsigned long long>(found));
            log_line(buffer);
        } else {
            g_guiM9.nextLocateAt = now + kLocateRetryMs;
        }
    }

    if (!g_guiM9.gameBase) {
        return;
    }

    const fh2vr::UiMode mode = fh2vr::read_ui_mode();
    if (mode != g_guiM9.lastMode) {
        char buffer[192]{};
        std::snprintf(buffer, sizeof(buffer),
                      "M9 FH2 UI state: %s -> %s",
                      fh2vr::ui_mode_name(g_guiM9.lastMode),
                      fh2vr::ui_mode_name(mode));
        log_line(buffer);
        g_guiM9.lastMode = mode;
    }

    if (fh2vr::ui_mode_wants_flat_overlay(mode)) {
        g_guiM9.unknownSince = 0;
        if (!g_guiM9.automaticEnabled) {
            g_guiM9.automaticEnabled = true;
            g_frozenM9.active = false;
            std::string message = "M9 automatic GUI enabled: ";
            message += fh2vr::ui_mode_name(mode);
            log_line(message.c_str());
        }
        return;
    }

    if (!g_guiM9.automaticEnabled) {
        g_guiM9.unknownSince = 0;
        return;
    }

    if (g_guiM9.unknownSince == 0) {
        g_guiM9.unknownSince = now;
        return;
    }

    if (now - g_guiM9.unknownSince >= kUnknownReleaseMs) {
        g_guiM9.automaticEnabled = false;
        g_guiM9.unknownSince = 0;
        g_frozenM9.active = false;
        g_reprojectionM9.haveFramePose = false;
        log_line("M9 automatic GUI released after 500ms stable gameplay/Unknown");
    }
}

bool poll_gui_m9() {
    const bool down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (down && !g_guiM9.f5WasDown) {
        g_guiM9.manualEnabled = !g_guiM9.manualEnabled;
        log_line(g_guiM9.manualEnabled
                     ? "M9 F5: manual HUD/GUI overlay force enabled"
                     : "M9 F5: manual HUD/GUI overlay force disabled");
    }
    g_guiM9.f5WasDown = down;
    sample_automatic_ui_m9();
    return g_guiM9.manualEnabled || g_guiM9.automaticEnabled;
}

bool acquire_slot_texture_m9(int slotIndex,
                             ID3D12Resource *&texture,
                             D3D12_RESOURCE_DESC &desc,
                             UINT64 &frameFence) {
    texture = nullptr;
    frameFence = 0;
    if (slotIndex < 0 || slotIndex > 1) {
        return false;
    }

    AcquireSRWLockShared(&g_resourceLock);
    if (g_resourcesReady &&
        g_slots[slotIndex].hasFrame &&
        g_slots[slotIndex].submit) {
        texture = g_slots[slotIndex].submit;
        texture->AddRef();
        desc = g_submitDesc;
        frameFence = g_slots[slotIndex].fenceValue;
    }
    ReleaseSRWLockShared(&g_resourceLock);
    return texture != nullptr && frameFence != 0;
}

void log_projection_once_m9(const ProjectionM9 &p,
                            const D3D12_RESOURCE_DESC &desc) {
    static std::atomic<bool> logged{false};
    if (logged.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    constexpr float kRadToDeg = 57.29577951308232f;
    char buffer[900]{};
    std::snprintf(
        buffer, sizeof(buffer),
        "M9 projection/FOV: HMD=%.2fx%.2f deg; FH2=%llux%u aspect=%.5f target=%.2fx%.2f deg; GUI=auto frozen-world + F5 live HUD",
        p.combinedHorizontalFovRadians * kRadToDeg,
        p.combinedVerticalFovRadians * kRadToDeg,
        static_cast<unsigned long long>(desc.Width), desc.Height,
        p.sourceAspect,
        p.sourceHorizontalFovRadians * kRadToDeg,
        p.sourceVerticalFovRadians * kRadToDeg);
    log_line(buffer);
}

void log_reprojection_stats_m9(UINT64 latestFence) {
    const ULONGLONG now = GetTickCount64();
    if (g_reprojectionM9.lastLogAt == 0) {
        g_reprojectionM9.lastLogAt = now;
        return;
    }
    if (now - g_reprojectionM9.lastLogAt < 1000) {
        return;
    }
    const ULONGLONG elapsed = now - g_reprojectionM9.lastLogAt;
    const double scale = elapsed ? 1000.0 / static_cast<double>(elapsed) : 0.0;
    char buffer[512]{};
    std::snprintf(
        buffer, sizeof(buffer),
        "M9 compositor cadence: %.2f pairs/s; unique scene=%.2f/s; repeated=%.2f/s; fence=%llu",
        static_cast<double>(g_reprojectionM9.pairSubmits) * scale,
        static_cast<double>(g_reprojectionM9.uniqueFrames) * scale,
        static_cast<double>(g_reprojectionM9.repeatedFrames) * scale,
        static_cast<unsigned long long>(latestFence));
    log_line(buffer);
    g_reprojectionM9.pairSubmits = 0;
    g_reprojectionM9.uniqueFrames = 0;
    g_reprojectionM9.repeatedFrames = 0;
    g_reprojectionM9.lastLogAt = now;
}

bool submit_scene_m9(vr::IVRCompositor *compositor,
                     ID3D12Resource *texture,
                     ID3D12CommandQueue *queue,
                     const D3D12_RESOURCE_DESC &desc,
                     UINT64 frameFence,
                     std::string &error) {
    ProjectionM9 projection{};
    if (!build_projection_m9(vr::VRSystem(), desc, projection)) {
        error = "M9 projection build failed";
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

    vr::D3D12TextureData_t d3d12Texture{texture, queue, 0};
    vr::VRTextureWithPose_t submitted{};
    submitted.handle = &d3d12Texture;
    submitted.eType = vr::TextureType_DirectX12;
    submitted.eColorSpace = vr::ColorSpace_Gamma;
    submitted.mDeviceToAbsoluteTracking = cameraMatchedPose;

    const vr::EVRCompositorError leftResult = compositor->Submit(
        vr::Eye_Left, &submitted, &projection.left, vr::Submit_TextureWithPose);
    if (leftResult != vr::VRCompositorError_None) {
        error = compositor_error("M9 Submit(left)", leftResult);
        return false;
    }
    const vr::EVRCompositorError rightResult = compositor->Submit(
        vr::Eye_Right, &submitted, &projection.right, vr::Submit_TextureWithPose);
    if (rightResult != vr::VRCompositorError_None) {
        error = compositor_error("M9 Submit(right)", rightResult);
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
                      "M9 VR scene submit OK: roll-neutral pose, first removed roll=%.2f deg",
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

    // On automatic menu entry, snapshot the previous R8 slot once. The menu
    // continues updating on the overlay while the VR world behind it stays on
    // the last gameplay frame, eliminating the old "menu on menu" duplication.
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

    const bool sceneOk = submit_scene_m9(
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
