#include "CameraResearch.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {
constexpr std::uintptr_t kPlayerManagerRva = 0x034ABA60;
constexpr std::uintptr_t kExpectedViewVtableA = 0x02C66188;
constexpr std::uintptr_t kExpectedViewVtableB = 0x02E6E890;
constexpr std::uintptr_t kExpectedCameraVtable = 0x02C6C840;
constexpr std::uintptr_t kPositionOffset = 0x220;
constexpr std::uintptr_t kForwardOffset = 0x240;
constexpr std::uintptr_t kUpOffset = 0x250;
constexpr std::uintptr_t kViewMatrixOffset = 0x260;
constexpr std::uintptr_t kFovOffset = 0x2A0;
constexpr DWORD kSampleIntervalMs = 200;
constexpr DWORD kHeartbeatMs = 2000;
constexpr DWORD kCaptureMs = 180000;

std::atomic<bool> g_stop{false};
std::atomic<bool> g_started{false};
std::atomic<std::uintptr_t> g_observedCamera{0};
std::uintptr_t g_gameBase = 0;

bool readable_range(const void *address, std::size_t size) {
    if (!address || size == 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(address, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto region = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto end = region + mbi.RegionSize;
    return start >= region && start + size >= start && start + size <= end;
}

template <typename T>
bool read_value(const void *address, T &out) {
    if (!readable_range(address, sizeof(T))) return false;
    std::memcpy(&out, address, sizeof(T));
    return true;
}

bool read_pointer(const void *address, void *&out) {
    std::uintptr_t p = 0;
    if (!read_value(address, p) || p == 0) {
        out = nullptr;
        return false;
    }
    out = reinterpret_cast<void *>(p);
    return true;
}

std::uintptr_t read_vtable(void *object) {
    void *vtable = nullptr;
    return object && read_pointer(object, vtable)
        ? reinterpret_cast<std::uintptr_t>(vtable) : 0;
}

struct Chain {
    void *game{};
    void *simulation{};
    void *manager{};
    void *views{};
    void *view{};
    void *camera{};
    std::uintptr_t viewVtable{};
    std::uintptr_t cameraVtable{};
};

Chain resolve_chain() {
    Chain c{};
    if (!g_gameBase ||
        !read_pointer(reinterpret_cast<void *>(g_gameBase + kPlayerManagerRva), c.game) ||
        !read_pointer(static_cast<unsigned char *>(c.game) + 0x48, c.simulation) ||
        !read_pointer(static_cast<unsigned char *>(c.simulation) + 0x98, c.manager) ||
        !read_pointer(static_cast<unsigned char *>(c.manager) + 0x2D0, c.views) ||
        !read_pointer(c.views, c.view)) return c;
    c.viewVtable = read_vtable(c.view);
    if (!read_pointer(static_cast<unsigned char *>(c.view) + 0xC0, c.camera)) return c;
    c.cameraVtable = read_vtable(c.camera);
    return c;
}

std::filesystem::path log_path() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(),
                                             static_cast<DWORD>(buffer.size()));
    std::filesystem::path root =
        (n > 0 && n < buffer.size()) ? std::filesystem::path(buffer.data())
                                      : std::filesystem::current_path();
    const auto dir = root / L"FH2VR" / L"logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / L"FH2VR-CameraResearch-M11.1.log";
}

std::string hex_ptr(const void *p) {
    std::ostringstream s;
    s << "0x" << std::hex << std::uppercase << reinterpret_cast<std::uintptr_t>(p);
    return s.str();
}

using Vec4 = std::array<float, 4>;
using Mat4 = std::array<float, 16>;

struct Snapshot {
    void *camera{};
    std::uintptr_t viewVtable{};
    std::uintptr_t cameraVtable{};
    Vec4 position{};
    Vec4 forward{};
    Vec4 up{};
    Mat4 view{};
    float fov{};
    bool valid{};
};

Snapshot capture() {
    Snapshot s{};
    const Chain c = resolve_chain();
    s.camera = c.camera ? c.camera : reinterpret_cast<void *>(g_observedCamera.load(std::memory_order_acquire));
    s.viewVtable = c.viewVtable;
    s.cameraVtable = c.cameraVtable;
    if (!s.camera) return s;
    auto *base = static_cast<unsigned char *>(s.camera);
    s.valid = read_value(base + kPositionOffset, s.position) &&
              read_value(base + kForwardOffset, s.forward) &&
              read_value(base + kUpOffset, s.up) &&
              read_value(base + kViewMatrixOffset, s.view) &&
              read_value(base + kFovOffset, s.fov);
    return s;
}

float vec_delta(const Vec4 &a, const Vec4 &b) {
    float d = 0.0f;
    for (int i = 0; i < 3; ++i) d = (std::max)(d, std::fabs(a[i] - b[i]));
    return d;
}

float matrix_delta(const Mat4 &a, const Mat4 &b) {
    float d = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) d = (std::max)(d, std::fabs(a[i] - b[i]));
    return d;
}

Vec4 eye_from_view(const Mat4 &m) {
    return {
        -(m[12] * m[0] + m[13] * m[1] + m[14] * m[2]),
        -(m[12] * m[4] + m[13] * m[5] + m[14] * m[6]),
        -(m[12] * m[8] + m[13] * m[9] + m[14] * m[10]),
        1.0f
    };
}

Vec4 right_from_up_forward(const Vec4 &up, const Vec4 &forward) {
    return {
        up[1] * forward[2] - up[2] * forward[1],
        up[2] * forward[0] - up[0] * forward[2],
        up[0] * forward[1] - up[1] * forward[0],
        0.0f
    };
}

void write_vec(std::ofstream &f, const char *name, const Vec4 &v) {
    f << name << "=(" << v[0] << ", " << v[1] << ", " << v[2] << ", " << v[3] << ")";
}

void write_snapshot(std::ofstream &f, const Snapshot &s, const Snapshot *previous,
                    ULONGLONG now, bool heartbeat) {
    f << "\n[t=" << now << "] " << (heartbeat ? "heartbeat" : "change")
      << " camera=" << hex_ptr(s.camera) << "\n";
    f << "  viewVtableRva=0x" << std::hex << std::uppercase
      << (s.viewVtable >= g_gameBase ? s.viewVtable - g_gameBase : 0)
      << " cameraVtableRva=0x"
      << (s.cameraVtable >= g_gameBase ? s.cameraVtable - g_gameBase : 0)
      << std::dec << "\n";
    if (!s.valid) {
        f << "  snapshot unavailable\n";
        f.flush();
        return;
    }

    const Vec4 eye = eye_from_view(s.view);
    const Vec4 right = right_from_up_forward(s.up, s.forward);
    f << std::fixed << std::setprecision(6) << "  ";
    write_vec(f, "position+220", s.position);
    f << "\n  "; write_vec(f, "eyeFromView", eye);
    f << "\n  "; write_vec(f, "forward+240", s.forward);
    f << "\n  "; write_vec(f, "up+250", s.up);
    f << "\n  "; write_vec(f, "derivedRight", right);
    f << "\n  fov+2A0=" << s.fov
      << " rad / " << (s.fov * 57.29577951308232f) << " deg\n";
    f << "  viewTranslation+290=(" << s.view[12] << ", " << s.view[13]
      << ", " << s.view[14] << ")\n";

    if (previous && previous->valid) {
        f << "  delta: pos=" << vec_delta(s.position, previous->position)
          << " forward=" << vec_delta(s.forward, previous->forward)
          << " up=" << vec_delta(s.up, previous->up)
          << " fov=" << std::fabs(s.fov - previous->fov)
          << " matrix=" << matrix_delta(s.view, previous->view) << "\n";
    }
    f.flush();
}

DWORD WINAPI research_thread(void *) {
    std::ofstream f(log_path(), std::ios::out | std::ios::trunc);
    if (!f) {
        g_started.store(false, std::memory_order_release);
        return 0;
    }
    f << "FH2VR M11.1 focused camera verification log\n"
      << "READ ONLY. No VR required. No camera writes.\n"
      << "Candidates from M11: +0x220 position, +0x240 forward, +0x250 up, +0x260 view matrix, +0x2A0 FOV.\n"
      << "Sampling at 5 Hz; writes on changes plus a 2-second heartbeat for up to 3 minutes.\n"
      << "Move/rotate the normal game camera and drive around.\n";

    Snapshot previous{};
    bool havePrevious = false;
    const ULONGLONG start = GetTickCount64();
    ULONGLONG lastWrite = 0;

    while (!g_stop.load(std::memory_order_relaxed) && GetTickCount64() - start < kCaptureMs) {
        const ULONGLONG now = GetTickCount64();
        const Snapshot current = capture();
        bool changed = !havePrevious || current.camera != previous.camera || current.valid != previous.valid;
        if (current.valid && havePrevious && previous.valid) {
            changed = changed || vec_delta(current.position, previous.position) > 0.0001f ||
                      vec_delta(current.forward, previous.forward) > 0.0001f ||
                      vec_delta(current.up, previous.up) > 0.0001f ||
                      std::fabs(current.fov - previous.fov) > 0.00001f ||
                      matrix_delta(current.view, previous.view) > 0.0001f;
        }
        const bool heartbeat = !changed && (lastWrite == 0 || now - lastWrite >= kHeartbeatMs);
        if (changed || heartbeat) {
            write_snapshot(f, current, havePrevious ? &previous : nullptr, now, heartbeat);
            lastWrite = now;
        }
        previous = current;
        havePrevious = true;
        Sleep(kSampleIntervalMs);
    }

    f << "\nCapture finished.\n";
    f.flush();
    g_started.store(false, std::memory_order_release);
    return 0;
}
} // namespace

namespace fh2vr {
void camera_research_observe(void *camera) {
    if (camera) g_observedCamera.store(reinterpret_cast<std::uintptr_t>(camera), std::memory_order_release);
}

bool start_camera_research(std::uintptr_t gameBase) {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return true;
    g_gameBase = gameBase;
    g_stop.store(false, std::memory_order_release);
    HANDLE thread = CreateThread(nullptr, 0, research_thread, nullptr, 0, nullptr);
    if (!thread) {
        g_started.store(false, std::memory_order_release);
        return false;
    }
    CloseHandle(thread);
    return true;
}

void stop_camera_research() {
    g_stop.store(true, std::memory_order_release);
}
} // namespace fh2vr
