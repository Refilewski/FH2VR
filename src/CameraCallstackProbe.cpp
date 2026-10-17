#include "GameBaseLocator.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace {

constexpr std::uintptr_t kDriverCameraVtableRva = 0x02E78120;
constexpr std::uintptr_t kCameraUpdateSlot = 0x20;
constexpr std::uintptr_t kPlayerManagerRva = 0x034ABA60;
constexpr std::uintptr_t kViewVtableA = 0x02C66188;
constexpr std::uintptr_t kViewVtableB = 0x02E6E890;
constexpr std::uintptr_t kCachedCameraVtable = 0x02C6C840;
constexpr std::uintptr_t kCachedViewMatrixOffset = 0x260;
constexpr std::uintptr_t kGameImageSize = 0x03A18000;
constexpr ULONGLONG kSampleEveryMs = 500;
constexpr std::size_t kMaxFrames = 20;
constexpr std::size_t kRingSize = 128;

using CameraUpdateFn = void (*)(void *, float, unsigned char);

struct Sample {
    std::array<void *, kMaxFrames> frames{};
    USHORT frameCount = 0;
    DWORD threadId = 0;
    ULONGLONG tick = 0;
    float before[3]{};
    float after[3]{};
    bool haveBefore = false;
    bool haveAfter = false;
};

struct RingSlot {
    std::atomic<std::uint32_t> ready{0};
    Sample sample{};
};

std::array<RingSlot, kRingSize> g_ring{};
std::atomic<std::uint32_t> g_sequence{0};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_stop{false};
std::uintptr_t g_gameBase = 0;
CameraUpdateFn g_previous = nullptr;
ULONGLONG g_lastSampleAt = 0;

bool readable(const void *address, std::size_t size) {
    if (!address || size == 0) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto end = base + info.RegionSize;
    return start >= base && start + size >= start && start + size <= end;
}

template <typename T>
bool read_value(const void *address, T &value) {
    if (!readable(address, sizeof(T))) return false;
    std::memcpy(&value, address, sizeof(T));
    return true;
}

bool read_pointer(const void *address, void *&value) {
    std::uintptr_t pointer = 0;
    if (!read_value(address, pointer) || pointer == 0) {
        value = nullptr;
        return false;
    }
    value = reinterpret_cast<void *>(pointer);
    return true;
}

std::uintptr_t vtable_of(void *object) {
    void *vtable = nullptr;
    return object && read_pointer(object, vtable)
               ? reinterpret_cast<std::uintptr_t>(vtable)
               : 0;
}

float *cached_view_matrix() {
    if (!g_gameBase) return nullptr;
    void *game = nullptr;
    void *simulation = nullptr;
    void *manager = nullptr;
    void *views = nullptr;
    void *view = nullptr;
    void *camera = nullptr;
    if (!read_pointer(reinterpret_cast<void *>(g_gameBase + kPlayerManagerRva), game) ||
        !read_pointer(static_cast<unsigned char *>(game) + 0x48, simulation) ||
        !read_pointer(static_cast<unsigned char *>(simulation) + 0x98, manager) ||
        !read_pointer(static_cast<unsigned char *>(manager) + 0x2D0, views) ||
        !read_pointer(views, view)) {
        return nullptr;
    }
    const auto viewVtable = vtable_of(view);
    if (viewVtable != g_gameBase + kViewVtableA &&
        viewVtable != g_gameBase + kViewVtableB) {
        return nullptr;
    }
    if (!read_pointer(static_cast<unsigned char *>(view) + 0xC0, camera) ||
        vtable_of(camera) != g_gameBase + kCachedCameraVtable) {
        return nullptr;
    }
    auto *matrix = reinterpret_cast<float *>(
        static_cast<unsigned char *>(camera) + kCachedViewMatrixOffset);
    return readable(matrix, sizeof(float) * 16) ? matrix : nullptr;
}

bool read_translation(float output[3]) {
    float *matrix = cached_view_matrix();
    if (!matrix) return false;
    float values[3]{};
    std::memcpy(values, matrix + 12, sizeof(values));
    output[0] = values[0];
    output[1] = values[1];
    output[2] = values[2];
    return true;
}

void enqueue_sample(const Sample &sample) {
    const std::uint32_t sequence =
        g_sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    RingSlot &slot = g_ring[sequence % kRingSize];
    slot.sample = sample;
    slot.ready.store(sequence, std::memory_order_release);
}

void camera_probe_hook(void *camera, float dt, unsigned char reset) {
    float before[3]{};
    const bool haveBefore = read_translation(before);

    if (g_previous) {
        g_previous(camera, dt, reset);
    }

    const ULONGLONG now = GetTickCount64();
    if (g_lastSampleAt != 0 && now - g_lastSampleAt < kSampleEveryMs) {
        return;
    }
    g_lastSampleAt = now;

    Sample sample{};
    sample.tick = now;
    sample.threadId = GetCurrentThreadId();
    sample.haveBefore = haveBefore;
    if (haveBefore) {
        std::memcpy(sample.before, before, sizeof(before));
    }
    sample.haveAfter = read_translation(sample.after);
    sample.frameCount = RtlCaptureStackBackTrace(
        0, static_cast<ULONG>(sample.frames.size()), sample.frames.data(), nullptr);
    enqueue_sample(sample);
}

bool install_probe() {
    auto *slot = reinterpret_cast<CameraUpdateFn *>(
        g_gameBase + kDriverCameraVtableRva + kCameraUpdateSlot);
    if (!readable(slot, sizeof(*slot))) return false;

    CameraUpdateFn current = nullptr;
    std::memcpy(&current, slot, sizeof(current));
    if (!current || current == &camera_probe_hook) return false;

    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection)) {
        return false;
    }
    g_previous = current;
    CameraUpdateFn replacement = &camera_probe_hook;
    std::memcpy(slot, &replacement, sizeof(replacement));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), oldProtection, &ignored);
    g_installed.store(true, std::memory_order_release);
    return true;
}

std::filesystem::path log_path() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    std::filesystem::path root =
        (length > 0 && length < static_cast<DWORD>(buffer.size()))
            ? std::filesystem::path(buffer.data())
            : std::filesystem::current_path();
    const auto directory = root / L"FH2VR" / L"logs";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return directory / L"FH2VR-Callstack-M11.2.log";
}

void write_frame(std::ofstream &file, void *frame) {
    const auto address = reinterpret_cast<std::uintptr_t>(frame);
    file << "0x" << std::hex << std::uppercase << address;
    if (address >= g_gameBase && address < g_gameBase + kGameImageSize) {
        file << " [FH2+0x" << (address - g_gameBase) << "]";
    }
    file << std::dec << "\n";
}

DWORD WINAPI writer_thread(void *) {
    g_gameBase = fh2vr::find_fh2_game_base();
    while (!g_gameBase && !g_stop.load(std::memory_order_relaxed)) {
        Sleep(500);
        g_gameBase = fh2vr::find_fh2_game_base();
    }
    if (!g_gameBase) return 0;

    // Let the normal FH2VR camera hook install first, then chain it rather than
    // replacing the original game callback directly.
    Sleep(7000);
    const bool installed = install_probe();

    std::ofstream file(log_path(), std::ios::out | std::ios::trunc);
    if (!file) return 0;
    file << "FH2VR M11.2 camera call-stack probe\n"
         << "No VR required. Camera data is read-only; this probe only chains the existing update callback.\n"
         << "gameBase=0x" << std::hex << std::uppercase << g_gameBase << std::dec << "\n"
         << "installed=" << (installed ? "yes" : "no") << "\n"
         << "Move/rotate the normal camera and drive for ~15 seconds.\n\n";
    file.flush();
    if (!installed) return 0;

    std::uint32_t next = 1;
    const ULONGLONG finishAt = GetTickCount64() + 20000;
    while (GetTickCount64() < finishAt && !g_stop.load(std::memory_order_relaxed)) {
        const std::uint32_t latest = g_sequence.load(std::memory_order_acquire);
        while (next <= latest) {
            RingSlot &slot = g_ring[next % kRingSize];
            if (slot.ready.load(std::memory_order_acquire) != next) {
                break;
            }
            const Sample sample = slot.sample;
            file << "[t=" << sample.tick << "] tid=" << sample.threadId;
            if (sample.haveBefore && sample.haveAfter) {
                file << std::fixed << std::setprecision(5)
                     << " viewT before=(" << sample.before[0] << "," << sample.before[1] << "," << sample.before[2] << ")"
                     << " after=(" << sample.after[0] << "," << sample.after[1] << "," << sample.after[2] << ")";
            }
            file << "\nstack:\n";
            for (USHORT i = 0; i < sample.frameCount; ++i) {
                file << "  #" << i << " ";
                write_frame(file, sample.frames[i]);
            }
            file << "\n";
            slot.ready.store(0, std::memory_order_release);
            ++next;
        }
        file.flush();
        Sleep(50);
    }

    file << "Capture finished.\n";
    file.flush();
    return 0;
}

struct Bootstrap {
    Bootstrap() {
        HANDLE thread = CreateThread(nullptr, 0, writer_thread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
};

Bootstrap g_bootstrap;

} // namespace
