#include "FovMatch.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
constexpr uintptr_t kExpectedGameBase = 0x140000000ull;
constexpr uintptr_t kDriverCameraVtableRva = 0x2E78120;
constexpr uintptr_t kCameraFovSlot = 0x70;
constexpr uintptr_t kMountedBaseFovOffset = 0x1D0;
constexpr ULONGLONG kPreviewFreshMs = 500;

using CameraGetFovFn = float (*)(void *camera);

CameraGetFovFn g_previousFov = nullptr;
std::atomic<float> g_sourceVerticalFovRadians{0.0f};
std::atomic<float> g_sourceHorizontalFovRadians{0.0f};
std::atomic<float> g_sourceAspect{0.0f};
std::atomic<float> g_combinedVerticalFovRadians{0.0f};
std::atomic<float> g_combinedHorizontalFovRadians{0.0f};
std::atomic<ULONGLONG> g_lastPreviewUpdateAt{0};
std::atomic<bool> g_loggedApplied{false};

void log_line(const std::string &message) {
    FILE *file = nullptr;
    if (fopen_s(&file, "FH2VR_RendererProbe.log", "a") == 0 && file) {
        std::fprintf(file, "[%llu ms] %s\n",
                     static_cast<unsigned long long>(GetTickCount64()), message.c_str());
        std::fclose(file);
    }
    OutputDebugStringA(("[FH2VR FOV M5] " + message + "\n").c_str());
}

bool memory_readable(const void *address, size_t size) {
    if (!address || size == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION information{};
    if (!VirtualQuery(address, &information, sizeof(information)) ||
        information.State != MEM_COMMIT ||
        (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t regionStart = reinterpret_cast<uintptr_t>(information.BaseAddress);
    const uintptr_t regionEnd = regionStart + information.RegionSize;
    return start >= regionStart && start + size >= start && start + size <= regionEnd;
}

template <typename T>
bool read_value(const void *address, T &output) {
    if (!memory_readable(address, sizeof(T))) {
        return false;
    }
    std::memcpy(&output, address, sizeof(T));
    return true;
}

float fov_hook(void *camera) {
    if (!g_previousFov) {
        return 0.0f;
    }

    const float original = g_previousFov(camera);
    if (!camera || !std::isfinite(original)) {
        return original;
    }

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG lastUpdate = g_lastPreviewUpdateAt.load(std::memory_order_acquire);
    if (lastUpdate == 0 || now - lastUpdate > kPreviewFreshMs) {
        return original;
    }

    const float target = g_sourceVerticalFovRadians.load(std::memory_order_relaxed);
    if (!std::isfinite(target) || target < 0.4f || target > 2.8f) {
        return original;
    }

    float base = 0.0f;
    if (!read_value(static_cast<unsigned char *>(camera) + kMountedBaseFovOffset, base) ||
        !std::isfinite(base) || base <= 0.01f || base > 3.0f) {
        return original;
    }

    const float scaled = original * (target / base);
    if (!std::isfinite(scaled)) {
        return original;
    }

    if (!g_loggedApplied.exchange(true, std::memory_order_acq_rel)) {
        constexpr float kRadToDeg = 57.2957795130823208768f;
        char buffer[512]{};
        std::snprintf(
            buffer, sizeof(buffer),
            "M5 FH2 FOV match ACTIVE: originalResult=%.7f base=%.7f rad (%.2f deg) targetVertical=%.7f rad (%.2f deg) targetHorizontal=%.2f deg aspect=%.5f scaledResult=%.7f",
            original, base, base * kRadToDeg,
            target, target * kRadToDeg,
            g_sourceHorizontalFovRadians.load(std::memory_order_relaxed) * kRadToDeg,
            g_sourceAspect.load(std::memory_order_relaxed), scaled);
        log_line(buffer);
    }

    return scaled;
}

bool install_fov_hook() {
    auto *slot = reinterpret_cast<CameraGetFovFn *>(
        kExpectedGameBase + kDriverCameraVtableRva + kCameraFovSlot);
    if (!memory_readable(slot, sizeof(*slot))) {
        log_line("M5 FOV hook install failed: Driver FOV slot unreadable");
        return false;
    }

    CameraGetFovFn current = nullptr;
    std::memcpy(&current, slot, sizeof(current));
    if (!current || current == &fov_hook) {
        log_line("M5 FOV hook install failed: invalid/already hooked callback");
        return false;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection)) {
        log_line("M5 FOV hook install failed: VirtualProtect failed");
        return false;
    }

    g_previousFov = current;
    CameraGetFovFn replacement = &fov_hook;
    std::memcpy(slot, &replacement, sizeof(replacement));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), oldProtection, &ignored);

    char buffer[256]{};
    std::snprintf(buffer, sizeof(buffer),
                  "M5 Driver FOV hook installed: slot=%p chained=%p",
                  static_cast<void *>(slot), reinterpret_cast<void *>(current));
    log_line(buffer);
    return true;
}

DWORD WINAPI fov_bootstrap_thread(void *) {
    // Let FH2VR/FH2ML and any camera mods finish their normal startup first,
    // then chain whatever callback currently owns Driver +0x70.
    Sleep(6500);
    install_fov_hook();
    return 0;
}

struct FovBootstrap {
    FovBootstrap() {
        HANDLE thread = CreateThread(nullptr, 0, fov_bootstrap_thread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    }
};

FovBootstrap g_fovBootstrap;
} // namespace

namespace fh2vr {

void update_fov_match_target(float sourceVerticalFovRadians,
                             float sourceHorizontalFovRadians,
                             float sourceAspect,
                             float combinedVerticalFovRadians,
                             float combinedHorizontalFovRadians) {
    if (!std::isfinite(sourceVerticalFovRadians) ||
        !std::isfinite(sourceHorizontalFovRadians) ||
        !std::isfinite(sourceAspect) ||
        !std::isfinite(combinedVerticalFovRadians) ||
        !std::isfinite(combinedHorizontalFovRadians) ||
        sourceVerticalFovRadians <= 0.0f ||
        sourceHorizontalFovRadians <= 0.0f ||
        sourceAspect <= 0.0f) {
        return;
    }

    g_sourceVerticalFovRadians.store(sourceVerticalFovRadians, std::memory_order_relaxed);
    g_sourceHorizontalFovRadians.store(sourceHorizontalFovRadians, std::memory_order_relaxed);
    g_sourceAspect.store(sourceAspect, std::memory_order_relaxed);
    g_combinedVerticalFovRadians.store(combinedVerticalFovRadians, std::memory_order_relaxed);
    g_combinedHorizontalFovRadians.store(combinedHorizontalFovRadians, std::memory_order_relaxed);
    g_lastPreviewUpdateAt.store(GetTickCount64(), std::memory_order_release);
}

} // namespace fh2vr
