#include "FH2ML.h"
#include "HeadsetPreview.h"

#include <openvr.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace {
constexpr char kModId[] = "fh2vr";
constexpr char kVersion[] = "0.4.0-dev";

// Forza Horizon 2 Xbox One 1.0.0.64 Driver-camera layout.
constexpr uintptr_t kDriverCameraVtableRva = 0x2E78120;
constexpr uintptr_t kCameraUpdateSlot = 0x20;
constexpr uintptr_t kCameraHeadingOffset = 0x1C0;
constexpr uintptr_t kCameraViewDirtyOffset = 0x21A;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kPitchLimit = 1.39626340159546366154f; // 80 degrees
constexpr ULONGLONG kVrRetryMs = 2000;

using CameraUpdate = void (*)(void *camera, float dt, unsigned char reset);

const FH2ML_HostAPI *g_api = nullptr;
uintptr_t g_gameBase = 0;
CameraUpdate g_originalCameraUpdate = nullptr;

std::atomic<bool> g_stop{false};
std::atomic<bool> g_vrEnabled{false};
std::atomic<bool> g_poseValid{false};
std::atomic<bool> g_recenterRequested{true};
std::atomic<float> g_yaw{0.0f};
std::atomic<float> g_pitch{0.0f};

void log_message(const std::string &message) {
    if (g_api && g_api->Log) {
        g_api->Log(kModId, message.c_str());
    }
}

float wrap_angle(float angle) {
    while (angle > kPi) {
        angle -= 2.0f * kPi;
    }
    while (angle < -kPi) {
        angle += 2.0f * kPi;
    }
    return angle;
}

bool key_pressed(int virtualKey, bool &wasDown) {
    const bool down = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    const bool pressed = down && !wasDown;
    wasDown = down;
    return pressed;
}

bool write_memory(void *address, const void *data, size_t size) {
    if (!address || !data || size == 0) {
        return false;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(address, size, PAGE_READWRITE, &oldProtection)) {
        return false;
    }

    std::memcpy(address, data, size);

    DWORD ignored = 0;
    const BOOL restored = VirtualProtect(address, size, oldProtection, &ignored);
    return restored != FALSE;
}

void apply_head_look(float forward[4], float yaw, float pitch) {
    // Match the mounted-camera yaw convention already proven by Extra Options.
    const float yawSine = std::sin(-yaw);
    const float yawCosine = std::cos(-yaw);
    const float originalX = forward[0];
    const float originalZ = forward[2];
    forward[0] = yawCosine * originalX + yawSine * originalZ;
    forward[2] = yawCosine * originalZ - yawSine * originalX;

    const float horizontal =
        std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
    if (horizontal < 0.0001f) {
        return;
    }

    const float rightX = -forward[2] / horizontal;
    const float rightZ = forward[0] / horizontal;
    const float pitchSine = std::sin(pitch);
    const float pitchCosine = std::cos(pitch);

    const float x = forward[0];
    const float y = forward[1];
    const float z = forward[2];

    const float crossX = -rightZ * y;
    const float crossY = rightZ * x - rightX * z;
    const float crossZ = rightX * y;

    forward[0] = x * pitchCosine + crossX * pitchSine;
    forward[1] = y * pitchCosine + crossY * pitchSine;
    forward[2] = z * pitchCosine + crossZ * pitchSine;
}

void camera_update_hook(void *camera, float dt, unsigned char reset) {
    g_originalCameraUpdate(camera, dt, reset);

    if (!camera || !g_vrEnabled.load(std::memory_order_relaxed) ||
        !g_poseValid.load(std::memory_order_acquire)) {
        return;
    }

    float forward[4]{};
    auto *heading = static_cast<unsigned char *>(camera) + kCameraHeadingOffset;
    std::memcpy(forward, heading, sizeof(forward));

    if (!std::isfinite(forward[0]) || !std::isfinite(forward[1]) ||
        !std::isfinite(forward[2])) {
        return;
    }

    apply_head_look(forward,
                    g_yaw.load(std::memory_order_relaxed),
                    g_pitch.load(std::memory_order_relaxed));

    std::memcpy(heading, forward, sizeof(forward));
    *(static_cast<unsigned char *>(camera) + kCameraViewDirtyOffset) = 1;
}

void publish_pose(const vr::HmdMatrix34_t &matrix,
                  float &referenceYaw,
                  float &referencePitch,
                  bool &hasReference) {
    // OpenVR is right-handed: +Y up, +X right, -Z forward.
    const float forwardX = -matrix.m[0][2];
    const float forwardY = -matrix.m[1][2];
    const float forwardZ = -matrix.m[2][2];
    const float horizontal =
        std::sqrt(forwardX * forwardX + forwardZ * forwardZ);

    if (!std::isfinite(forwardX) || !std::isfinite(forwardY) ||
        !std::isfinite(forwardZ) || horizontal < 0.0001f) {
        g_poseValid.store(false, std::memory_order_release);
        return;
    }

    const float yaw = std::atan2(forwardX, -forwardZ);
    const float pitch = std::atan2(forwardY, horizontal);

    if (g_recenterRequested.exchange(false, std::memory_order_acq_rel) ||
        !hasReference) {
        referenceYaw = yaw;
        referencePitch = pitch;
        hasReference = true;
        log_message("Head pose recentered");
    }

    g_yaw.store(wrap_angle(referenceYaw - yaw), std::memory_order_relaxed);
    g_pitch.store(std::clamp(pitch - referencePitch, -kPitchLimit, kPitchLimit),
                  std::memory_order_relaxed);
    g_poseValid.store(g_vrEnabled.load(std::memory_order_relaxed),
                      std::memory_order_release);
}

DWORD WINAPI tracking_thread(void *) {
    bool f1WasDown = false;
    std::string lastInitError;
    std::string lastPreviewError;
    std::string lastCompositorError;
    vr::IVRSystem *system = nullptr;
    vr::IVRCompositor *compositor = nullptr;
    ULONGLONG nextVrRetryAt = 0;
    float referenceYaw = 0.0f;
    float referencePitch = 0.0f;
    bool hasReference = false;

    auto shutdown_runtime = [&]() {
        fh2vr::shutdown_headset_preview();
        compositor = nullptr;
        if (system) {
            vr::VR_Shutdown();
            system = nullptr;
        }
    };

    while (!g_stop.load(std::memory_order_relaxed)) {
        const ULONGLONG now = GetTickCount64();

        if (key_pressed(VK_F1, f1WasDown)) {
            const bool enabled = !g_vrEnabled.load(std::memory_order_relaxed);
            g_vrEnabled.store(enabled, std::memory_order_relaxed);
            g_poseValid.store(false, std::memory_order_release);
            shutdown_runtime();
            hasReference = false;
            g_recenterRequested.store(true, std::memory_order_relaxed);
            nextVrRetryAt = 0;
            lastInitError.clear();
            lastPreviewError.clear();
            lastCompositorError.clear();

            log_message(enabled
                            ? "F1: VR enabled; starting OpenVR scene + head tracking"
                            : "F1: VR disabled");
        }

        if (!g_vrEnabled.load(std::memory_order_relaxed)) {
            g_poseValid.store(false, std::memory_order_release);
            Sleep(10);
            continue;
        }

        if (!system) {
            if (now >= nextVrRetryAt) {
                vr::EVRInitError initError = vr::VRInitError_None;
                system = vr::VR_Init(&initError, vr::VRApplication_Scene);
                if (!system || initError != vr::VRInitError_None) {
                    system = nullptr;
                    const char *description =
                        vr::VR_GetVRInitErrorAsEnglishDescription(initError);
                    const std::string error =
                        description ? description : "unknown OpenVR error";
                    if (error != lastInitError) {
                        log_message("SteamVR/OpenVR unavailable: " + error);
                        lastInitError = error;
                    }
                    nextVrRetryAt = now + kVrRetryMs;
                } else {
                    compositor = vr::VRCompositor();
                    if (!compositor) {
                        log_message(
                            "OpenVR scene initialized but VRCompositor is unavailable; retrying");
                        shutdown_runtime();
                        nextVrRetryAt = now + kVrRetryMs;
                    } else {
                        lastInitError.clear();
                        hasReference = false;
                        g_recenterRequested.store(true, std::memory_order_relaxed);
                        log_message(
                            "SteamVR scene/compositor connected; F1 VR, F2 GUI/HUD overlay");
                    }
                }
            }

            if (!system || !compositor) {
                Sleep(4);
                continue;
            }
        }

        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        const vr::EVRCompositorError waitResult =
            compositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount,
                                     nullptr, 0);
        if (waitResult != vr::VRCompositorError_None) {
            const std::string error =
                "OpenVR WaitGetPoses failed (error " +
                std::to_string(static_cast<int>(waitResult)) + ")";
            if (error != lastCompositorError) {
                log_message(error);
                lastCompositorError = error;
            }
            g_poseValid.store(false, std::memory_order_release);
            hasReference = false;
            g_recenterRequested.store(true, std::memory_order_relaxed);
            Sleep(1);
            continue;
        }
        lastCompositorError.clear();

        const vr::TrackedDevicePose_t &hmd =
            poses[vr::k_unTrackedDeviceIndex_Hmd];
        if (hmd.bDeviceIsConnected && hmd.bPoseIsValid) {
            publish_pose(hmd.mDeviceToAbsoluteTracking,
                         referenceYaw, referencePitch, hasReference);
        } else {
            g_poseValid.store(false, std::memory_order_release);
            hasReference = false;
            g_recenterRequested.store(true, std::memory_order_relaxed);
        }

        std::string previewError;
        if (!fh2vr::submit_headset_preview(
                compositor, false, 0, false, previewError)) {
            if (previewError != lastPreviewError) {
                log_message("Headset preview: " + previewError);
                lastPreviewError = previewError;
            }
        } else if (!lastPreviewError.empty()) {
            log_message("Headset preview recovered");
            lastPreviewError.clear();
        }
    }

    g_poseValid.store(false, std::memory_order_release);
    shutdown_runtime();
    return 0;
}

bool install_camera_hook(uintptr_t gameBase) {
    auto *slot = reinterpret_cast<unsigned char *>(
        gameBase + kDriverCameraVtableRva + kCameraUpdateSlot);
    CameraUpdate replacement = &camera_update_hook;
    CameraUpdate current = nullptr;
    std::memcpy(&current, slot, sizeof(current));
    if (!current || current == replacement) {
        log_message("Driver camera update slot is invalid or already hooked");
        return false;
    }

    unsigned char originalBytes[sizeof(CameraUpdate)]{};
    unsigned char replacementBytes[sizeof(CameraUpdate)]{};
    std::memcpy(originalBytes, &current, sizeof(current));
    std::memcpy(replacementBytes, &replacement, sizeof(replacement));

    const FH2ML_Patch patch{
        sizeof(FH2ML_Patch),
        kModId,
        "camera-vtable",
        reinterpret_cast<uintptr_t>(slot),
        static_cast<uint32_t>(sizeof(CameraUpdate)),
        originalBytes,
        replacementBytes,
    };

    if (!g_api->RegisterPatch(&patch)) {
        log_message("FH2ML rejected driver camera patch registration");
        return false;
    }

    g_originalCameraUpdate = current;
    if (!write_memory(slot, &replacement, sizeof(replacement))) {
        g_originalCameraUpdate = nullptr;
        log_message("Failed to write driver camera hook");
        return false;
    }

    log_message("Driver camera hook installed; current camera callback is chained");
    return true;
}

bool restore_camera_hook(uintptr_t gameBase) {
    if (!g_originalCameraUpdate) {
        return true;
    }
    auto *slot = reinterpret_cast<unsigned char *>(
        gameBase + kDriverCameraVtableRva + kCameraUpdateSlot);
    CameraUpdate original = g_originalCameraUpdate;
    return write_memory(slot, &original, sizeof(original));
}
} // namespace

extern "C" __declspec(dllexport) int FH2ML_Initialize(
    const FH2ML_HostAPI *api) {
    if (!api || api->size < sizeof(FH2ML_HostAPI) ||
        api->version != FH2ML_API_VERSION || !api->Log ||
        !api->RegisterMod || !api->RegisterPatch || !api->GetGameModule) {
        return 0;
    }

    g_api = api;
    if (!api->RegisterMod(kModId, kVersion)) {
        return 0;
    }

    const uintptr_t gameBase = api->GetGameModule();
    if (!gameBase) {
        log_message("FH2ML did not provide a validated FH2 game module");
        return 0;
    }
    g_gameBase = gameBase;

    if (!install_camera_hook(gameBase)) {
        return 0;
    }

    HANDLE thread = CreateThread(nullptr, 0, tracking_thread, nullptr, 0, nullptr);
    if (!thread) {
        log_message("Failed to create tracking worker");
        if (!restore_camera_hook(gameBase)) {
            log_message(
                "WARNING: camera hook restore failed after worker startup failure");
        }
        return 0;
    }
    CloseHandle(thread);

    log_message("FH2VR 0.4.0-dev initialized: F1 VR toggle, F2 GUI/HUD overlay; all legacy debug keybinds removed");
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH) {
        // FH2ML ABI v1 has no hot-unload contract. Signal only; never wait
        // while running under the Windows loader lock.
        g_stop.store(true, std::memory_order_relaxed);
    }
    return TRUE;
}
