#include "CameraFrameTimeline.h"
#include "GameBaseLocator.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace {
constexpr std::size_t kCapacity = 32768;
constexpr DWORD kStartupDelayMs = 9000;
constexpr DWORD kCaptureDurationMs = 22000;
constexpr DWORD kWaitForCameraMs = 45000;

enum class Kind : std::uint8_t { Camera, Execute, Present };
struct Event {
    LONGLONG qpc = 0;
    DWORD tid = 0;
    Kind kind = Kind::Camera;
    unsigned argA = 0;
    unsigned argB = 0;
    float tx = 0.0f;
    float ty = 0.0f;
    float tz = 0.0f;
};
struct Slot {
    std::atomic<bool> ready{false};
    Event event{};
};

std::array<Slot, kCapacity> g_events{};
std::atomic<std::uint32_t> g_next{0};
std::atomic<bool> g_capture{false};
std::atomic<bool> g_probeInstalled{false};
std::atomic<std::uint64_t> g_cameraCalls{0};
std::atomic<std::uint64_t> g_cameraValidCalls{0};
LONGLONG g_startQpc = 0;

void record(Kind kind, unsigned argA, unsigned argB,
            float tx, float ty, float tz) {
    if (!g_capture.load(std::memory_order_acquire)) return;

    const std::uint32_t index = g_next.fetch_add(1, std::memory_order_relaxed);
    if (index >= kCapacity) return;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    Slot &slot = g_events[index];
    slot.event = Event{now.QuadPart, GetCurrentThreadId(), kind, argA, argB, tx, ty, tz};
    slot.ready.store(true, std::memory_order_release);
}

std::filesystem::path file_path() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    const std::filesystem::path root =
        (length > 0 && length < static_cast<DWORD>(buffer.size()))
            ? std::filesystem::path(buffer.data())
            : std::filesystem::current_path();
    const std::filesystem::path dir = root / L"FH2VR" / L"logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / L"FH2VR-FrameTimeline-M11.4.csv";
}

const char *event_name(Kind kind) {
    switch (kind) {
    case Kind::Camera: return "camera-post-update";
    case Kind::Execute: return "game-direct-execute";
    case Kind::Present: return "game-present";
    }
    return "unknown";
}

DWORD WINAPI timeline_writer(void *) {
    // This sampler is only for the supported game image and is never
    // enabled merely because Azimuth loaded the DLL.
    while (!fh2vr::find_fh2_game_base()) Sleep(500);
    Sleep(kStartupDelayMs);

    // Do not silently measure a menu/loading screen instead of gameplay.
    // Wait for actual Driver camera callbacks, but time out with diagnostics.
    DWORD waitedForCamera = 0;
    while (waitedForCamera < kWaitForCameraMs &&
           g_cameraValidCalls.load(std::memory_order_acquire) < 5) {
        Sleep(100);
        waitedForCamera += 100;
    }
    const bool cameraWasActive =
        g_cameraValidCalls.load(std::memory_order_acquire) >= 5;
    if (cameraWasActive) Sleep(500);

    LARGE_INTEGER frequency{}, start{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    g_startQpc = start.QuadPart;
    g_capture.store(true, std::memory_order_release);
    Sleep(kCaptureDurationMs);
    g_capture.store(false, std::memory_order_release);
    Sleep(100); // Allow any producer that observed the old flag to finish.

    std::ofstream file(file_path(), std::ios::out | std::ios::trunc);
    if (!file) return 0;
    const std::uint32_t claimed = g_next.load(std::memory_order_acquire);
    const std::size_t count = claimed < kCapacity ? claimed : kCapacity;
    file << "# FH2VR M11.4 camera-aware frame timing capture, no VR required\n"
         << "# Passive timestamps from existing game camera and D3D12 hooks.\n"
         << "# camera-post-update = after Driver camera callback; "
            "game-direct-execute = non-mod DIRECT queue; "
            "game-present = primary swapchain before copy/conversion.\n"
         << "# Separate threads can overlap; timing correlation does NOT identify world pass by itself.\n"
         << "# camera_ready_before_capture=" << (cameraWasActive ? "yes" : "no")
         << " camera_wait_ms=" << waitedForCamera
         << " probe_installed=" << (g_probeInstalled.load() ? "yes" : "no")
         << " camera_hook_calls=" << g_cameraCalls.load()
         << " camera_view_valid_calls=" << g_cameraValidCalls.load() << "\n"
         << "# camera-post-update arg_a: 1=view matrix available, 0=unavailable.\n"
         << "# samples=" << count << " dropped="
         << (claimed > kCapacity ? claimed - kCapacity : 0)
         << " capture_ms=" << kCaptureDurationMs << "\n"
         << "index,time_us,thread_id,event,arg_a,arg_b,view_tx,view_ty,view_tz\n";

    for (std::size_t i = 0; i < count; ++i) {
        const Slot &slot = g_events[i];
        if (!slot.ready.load(std::memory_order_acquire)) continue;
        const Event &event = slot.event;
        const LONGLONG dt = event.qpc - g_startQpc;
        const double us = frequency.QuadPart > 0
            ? static_cast<double>(dt) * 1000000.0 /
                  static_cast<double>(frequency.QuadPart)
            : 0.0;
        file << i << ',' << std::fixed << std::setprecision(2) << us << ','
             << event.tid << ',' << event_name(event.kind) << ','
             << event.argA << ',' << event.argB << ','
             << std::setprecision(5) << event.tx << ','
             << event.ty << ',' << event.tz << '\n';
    }
    file.flush();
    return 0;
}

struct Bootstrap {
    Bootstrap() {
        HANDLE worker = CreateThread(nullptr, 0, timeline_writer, nullptr, 0, nullptr);
        if (worker) CloseHandle(worker);
    }
};
Bootstrap g_bootstrap;

} // namespace

namespace fh2vr {
void timeline_note_probe_installed(bool installed) {
    g_probeInstalled.store(installed, std::memory_order_release);
}
void timeline_note_camera(bool valid, float x, float y, float z) {
    g_cameraCalls.fetch_add(1, std::memory_order_relaxed);
    if (valid) g_cameraValidCalls.fetch_add(1, std::memory_order_release);
    record(Kind::Camera, valid ? 1u : 0u, 0, x, y, z);
}
void timeline_note_execute(unsigned count) {
    record(Kind::Execute, count, 0, 0.0f, 0.0f, 0.0f);
}
void timeline_note_present(unsigned syncInterval, unsigned flags) {
    record(Kind::Present, syncInterval, flags, 0.0f, 0.0f, 0.0f);
}
} // namespace fh2vr
