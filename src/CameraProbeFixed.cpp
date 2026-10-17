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
constexpr uintptr_t kCameraUpdateSlot = 0x20;
constexpr uintptr_t kCameraFovSlot = 0x70;
constexpr uintptr_t kMountedBaseFovOffset = 0x1D0;
constexpr uintptr_t kPlayerManagerRva = 0x34ABA60;
constexpr uintptr_t kViewVtableA = 0x2C66188;
constexpr uintptr_t kViewVtableB = 0x2E6E890;
constexpr uintptr_t kCachedCameraVtable = 0x2C6C840;
constexpr uintptr_t kCachedViewMatrixOffset = 0x260;
constexpr size_t kDriverScanBytes = 0x400;

using CameraUpdateFn = void (*)(void *, float, unsigned char);
using CameraGetFovFn = float (*)(void *);

CameraUpdateFn g_previousCameraUpdate = nullptr;
uintptr_t g_gameBase = 0;
std::atomic<bool> g_scanRequested{false};
std::atomic<std::uint64_t> g_cameraUpdates{0};
std::atomic<std::uint64_t> g_persisted{0};
std::atomic<std::uint64_t> g_rebuilt{0};
float g_lastMatrix12AfterHook = 0.0f;
bool g_haveLastMatrix12 = false;
ULONGLONG g_lastStatsLogAt = 0;

void log_line(const std::string &message) {
    FILE *file = nullptr;
    if (fopen_s(&file, "FH2VR_RendererProbe.log", "a") == 0 && file) {
        std::fprintf(file, "[%llu ms] %s\n",
                     static_cast<unsigned long long>(GetTickCount64()), message.c_str());
        std::fclose(file);
    }
    OutputDebugStringA(("[FH2VR camera probe] " + message + "\n").c_str());
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

bool read_pointer(const void *address, void **output) {
    return output && read_value(address, *output) && *output;
}

bool has_vtable(void *object, uintptr_t expectedRva) {
    void *vtable = nullptr;
    return object && read_pointer(object, &vtable) &&
           reinterpret_cast<uintptr_t>(vtable) == g_gameBase + expectedRva;
}

float *resolve_cached_view_matrix() {
    if (!g_gameBase) {
        return nullptr;
    }

    void *game = nullptr;
    void *simulation = nullptr;
    void *manager = nullptr;
    void *views = nullptr;
    void *view = nullptr;
    void *camera = nullptr;

    if (!read_pointer(reinterpret_cast<void *>(g_gameBase + kPlayerManagerRva), &game) ||
        !read_pointer(static_cast<unsigned char *>(game) + 0x48, &simulation) ||
        !read_pointer(static_cast<unsigned char *>(simulation) + 0x98, &manager) ||
        !read_pointer(static_cast<unsigned char *>(manager) + 0x2D0, &views) ||
        !read_pointer(views, &view) ||
        (!has_vtable(view, kViewVtableA) && !has_vtable(view, kViewVtableB)) ||
        !read_pointer(static_cast<unsigned char *>(view) + 0xC0, &camera) ||
        !has_vtable(camera, kCachedCameraVtable)) {
        return nullptr;
    }

    auto *matrix = reinterpret_cast<float *>(static_cast<unsigned char *>(camera) +
                                             kCachedViewMatrixOffset);
    return memory_readable(matrix, sizeof(float) * 16) ? matrix : nullptr;
}

bool read_matrix12(float &value) {
    float *matrix = resolve_cached_view_matrix();
    return matrix && read_value(matrix + 12, value) && std::isfinite(value);
}

bool read_driver_fov_diagnostics(void *camera, float &callbackFov,
                                 float &mountedBaseRadians,
                                 void **callbackAddress) {
    callbackFov = 0.0f;
    mountedBaseRadians = 0.0f;
    if (callbackAddress) {
        *callbackAddress = nullptr;
    }
    if (!camera) {
        return false;
    }

    void *vtable = nullptr;
    if (!read_pointer(camera, &vtable)) {
        return false;
    }

    void *callback = nullptr;
    if (!read_pointer(static_cast<unsigned char *>(vtable) + kCameraFovSlot, &callback) ||
        !callback) {
        return false;
    }

    if (!read_value(static_cast<unsigned char *>(camera) + kMountedBaseFovOffset,
                    mountedBaseRadians) ||
        !std::isfinite(mountedBaseRadians) || mountedBaseRadians <= 0.0f) {
        return false;
    }

    const auto getFov = reinterpret_cast<CameraGetFovFn>(callback);
    callbackFov = getFov(camera);
    if (!std::isfinite(callbackFov)) {
        return false;
    }

    if (callbackAddress) {
        *callbackAddress = callback;
    }
    return true;
}

void scan_driver_camera(void *camera) {
    if (!camera || !memory_readable(camera, kDriverScanBytes)) {
        log_line("Fixed camera probe: Driver object is not readable for 0x400 bytes");
        return;
    }

    float *matrix = resolve_cached_view_matrix();
    if (!matrix || !memory_readable(matrix, sizeof(float) * 16)) {
        log_line("Fixed camera probe: cached 0x260 matrix unavailable for Driver scan");
        return;
    }

    const float mx = matrix[12];
    const float my = matrix[13];
    const float mz = matrix[14];
    const auto *values = static_cast<const float *>(camera);
    const size_t floatCount = kDriverScanBytes / sizeof(float);
    unsigned matches = 0;

    char start[256]{};
    std::snprintf(start, sizeof(start),
                  "Fixed Driver scan: cached translation=(%.5f, %.5f, %.5f)",
                  mx, my, mz);
    log_line(start);

    for (size_t i = 0; i + 2 < floatCount && matches < 32; ++i) {
        const float x = values[i];
        const float y = values[i + 1];
        const float z = values[i + 2];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            continue;
        }
        if (std::fabs(x - mx) < 30.0f && std::fabs(z - mz) < 30.0f) {
            char buffer[256]{};
            std::snprintf(buffer, sizeof(buffer),
                          "Fixed Driver pos-like triple at +0x%zX: %.5f %.5f %.5f",
                          i * sizeof(float), x, y, z);
            log_line(buffer);
            ++matches;
        }
    }

    if (matches == 0) {
        log_line("Fixed Driver scan: no X/Z candidate within +/-30 units");
    }
}

void camera_probe_hook(void *camera, float dt, unsigned char reset) {
    float before = 0.0f;
    const bool haveBefore = read_matrix12(before);
    if (g_haveLastMatrix12 && haveBefore) {
        const float tolerance =
            0.00001f * (std::fabs(g_lastMatrix12AfterHook) > 1.0f
                            ? std::fabs(g_lastMatrix12AfterHook)
                            : 1.0f);
        if (std::fabs(before - g_lastMatrix12AfterHook) <= tolerance) {
            g_persisted.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_rebuilt.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (g_previousCameraUpdate) {
        g_previousCameraUpdate(camera, dt, reset);
    }

    g_cameraUpdates.fetch_add(1, std::memory_order_relaxed);

    float after = 0.0f;
    if (read_matrix12(after)) {
        g_lastMatrix12AfterHook = after;
        g_haveLastMatrix12 = true;
    }

    if (g_scanRequested.exchange(false, std::memory_order_acq_rel)) {
        scan_driver_camera(camera);
    }

    const ULONGLONG now = GetTickCount64();
    if (g_lastStatsLogAt == 0) {
        g_lastStatsLogAt = now;
    } else if (now - g_lastStatsLogAt >= 1000) {
        const ULONGLONG elapsed = now - g_lastStatsLogAt;
        const std::uint64_t updates = g_cameraUpdates.exchange(0, std::memory_order_relaxed);
        const std::uint64_t persisted = g_persisted.exchange(0, std::memory_order_relaxed);
        const std::uint64_t rebuilt = g_rebuilt.exchange(0, std::memory_order_relaxed);
        const double hz = elapsed > 0
                              ? static_cast<double>(updates) * 1000.0 /
                                    static_cast<double>(elapsed)
                              : 0.0;
        char buffer[384]{};
        std::snprintf(buffer, sizeof(buffer),
                      "Fixed camera %.2f updates/s; 0x260 persisted=%llu rebuilt=%llu",
                      hz,
                      static_cast<unsigned long long>(persisted),
                      static_cast<unsigned long long>(rebuilt));
        log_line(buffer);

        float callbackFov = 0.0f;
        float mountedBaseRadians = 0.0f;
        void *fovCallback = nullptr;
        if (read_driver_fov_diagnostics(camera, callbackFov,
                                        mountedBaseRadians, &fovCallback)) {
            constexpr float kRadToDeg = 57.2957795130823208768f;
            const float mountedBaseDegrees = mountedBaseRadians * kRadToDeg;
            char fovBuffer[384]{};
            std::snprintf(
                fovBuffer, sizeof(fovBuffer),
                "Driver FOV diagnostic: callback=%p result=%.7f camera+0x1D0=%.7f rad (%.3f deg)",
                fovCallback, callbackFov, mountedBaseRadians, mountedBaseDegrees);
            log_line(fovBuffer);
        } else {
            log_line("Driver FOV diagnostic: unavailable for current camera object");
        }

        g_lastStatsLogAt = now;
    }
}

bool install_camera_probe() {
    g_gameBase = kExpectedGameBase;

    auto *slot = reinterpret_cast<CameraUpdateFn *>(
        g_gameBase + kDriverCameraVtableRva + kCameraUpdateSlot);
    if (!memory_readable(slot, sizeof(*slot))) {
        char buffer[192]{};
        std::snprintf(buffer, sizeof(buffer),
                      "Fixed camera probe failed: slot %p at FH2 base 0x%llX is unreadable",
                      static_cast<void *>(slot),
                      static_cast<unsigned long long>(g_gameBase));
        log_line(buffer);
        return false;
    }

    CameraUpdateFn current = nullptr;
    std::memcpy(&current, slot, sizeof(current));
    if (!current || current == &camera_probe_hook) {
        log_line("Fixed camera probe failed: invalid/already hooked callback");
        return false;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection)) {
        log_line("Fixed camera probe failed: VirtualProtect on Driver slot failed");
        return false;
    }

    g_previousCameraUpdate = current;
    CameraUpdateFn replacement = &camera_probe_hook;
    std::memcpy(slot, &replacement, sizeof(replacement));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), oldProtection, &ignored);

    char buffer[256]{};
    std::snprintf(buffer, sizeof(buffer),
                  "Fixed camera probe installed at FH2 base 0x%llX; chained callback=%p",
                  static_cast<unsigned long long>(g_gameBase),
                  reinterpret_cast<void *>(current));
    log_line(buffer);
    return true;
}

DWORD WINAPI camera_probe_thread(void *) {
    Sleep(5000);
    install_camera_probe();

    bool f12WasDown = false;
    for (;;) {
        const bool f12Down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (f12Down && !f12WasDown) {
            g_scanRequested.store(true, std::memory_order_release);
            log_line("F12 observed by fixed camera probe: Driver scan requested");
        }
        f12WasDown = f12Down;
        Sleep(4);
    }
}

struct CameraProbeBootstrap {
    CameraProbeBootstrap() {
        HANDLE thread = CreateThread(nullptr, 0, camera_probe_thread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    }
};

CameraProbeBootstrap g_cameraProbeBootstrap;
} // namespace
