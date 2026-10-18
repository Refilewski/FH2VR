#include "CameraResearch.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace {

constexpr std::uintptr_t kPlayerManagerRva = 0x034ABA60;
constexpr std::uintptr_t kExpectedViewVtableA = 0x02C66188;
constexpr std::uintptr_t kExpectedViewVtableB = 0x02E6E890;
constexpr std::uintptr_t kExpectedCameraVtable = 0x02C6C840;
constexpr std::uintptr_t kCameraHeadingOffset = 0x1C0;
constexpr std::uintptr_t kCameraBaseFovOffset = 0x1D0;
constexpr std::uintptr_t kCachedViewMatrixOffset = 0x260;
constexpr std::uintptr_t kCameraScanStart = 0x180;
constexpr std::uintptr_t kCameraScanEnd = 0x2A0;
constexpr DWORD kSampleIntervalMs = 1000;
constexpr unsigned kMaxSamples = 300; // Five minutes at 1 Hz.

std::atomic<std::uintptr_t> g_observedCamera{0};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_started{false};
std::uintptr_t g_gameBase = 0;

bool readable_range(const void *address, std::size_t size) {
    if (!address || size == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION information{};
    if (!VirtualQuery(address, &information, sizeof(information)) ||
        information.State != MEM_COMMIT ||
        (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto regionStart = reinterpret_cast<std::uintptr_t>(information.BaseAddress);
    const auto regionEnd = regionStart + information.RegionSize;
    return start >= regionStart && start + size >= start && start + size <= regionEnd;
}

template <typename T>
bool read_value(const void *address, T &output) {
    if (!readable_range(address, sizeof(T))) {
        return false;
    }
    std::memcpy(&output, address, sizeof(T));
    return true;
}

bool read_pointer(const void *address, void *&output) {
    std::uintptr_t value = 0;
    if (!read_value(address, value) || value == 0) {
        output = nullptr;
        return false;
    }
    output = reinterpret_cast<void *>(value);
    return true;
}

std::uintptr_t read_vtable(void *object) {
    void *vtable = nullptr;
    if (!object || !read_pointer(object, vtable)) {
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(vtable);
}

struct CameraChain {
    void *game = nullptr;
    void *simulation = nullptr;
    void *manager = nullptr;
    void *views = nullptr;
    void *view = nullptr;
    void *camera = nullptr;
    std::uintptr_t viewVtable = 0;
    std::uintptr_t cameraVtable = 0;
};

CameraChain resolve_camera_chain() {
    CameraChain chain{};
    if (!g_gameBase) {
        return chain;
    }

    if (!read_pointer(reinterpret_cast<void *>(g_gameBase + kPlayerManagerRva), chain.game) ||
        !read_pointer(static_cast<unsigned char *>(chain.game) + 0x48, chain.simulation) ||
        !read_pointer(static_cast<unsigned char *>(chain.simulation) + 0x98, chain.manager) ||
        !read_pointer(static_cast<unsigned char *>(chain.manager) + 0x2D0, chain.views) ||
        !read_pointer(chain.views, chain.view)) {
        return chain;
    }

    chain.viewVtable = read_vtable(chain.view);
    if (!read_pointer(static_cast<unsigned char *>(chain.view) + 0xC0, chain.camera)) {
        chain.camera = nullptr;
        return chain;
    }
    chain.cameraVtable = read_vtable(chain.camera);
    return chain;
}

std::filesystem::path log_path() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    std::filesystem::path root;
    if (length > 0 && length < buffer.size()) {
        root = buffer.data();
    } else {
        root = std::filesystem::current_path();
    }

    const std::filesystem::path directory = root / L"FH2VR" / L"logs";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return directory / L"FH2VR-CameraResearch.log";
}

std::string pointer_string(const void *value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase
           << reinterpret_cast<std::uintptr_t>(value);
    return stream.str();
}

std::string rva_string(std::uintptr_t absolute) {
    if (!absolute || !g_gameBase || absolute < g_gameBase) {
        return "n/a";
    }
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << (absolute - g_gameBase);
    return stream.str();
}

void write_matrix(std::ofstream &file, const float matrix[16]) {
    file << std::fixed << std::setprecision(6);
    for (int row = 0; row < 4; ++row) {
        file << "    [";
        for (int column = 0; column < 4; ++column) {
            if (column != 0) {
                file << ", ";
            }
            file << std::setw(12) << matrix[row * 4 + column];
        }
        file << "]\n";
    }
}

bool finite_vec3(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

void write_matrix_analysis(std::ofstream &file, const float matrix[16]) {
    const float lookX = matrix[2];
    const float lookY = matrix[6];
    const float lookZ = matrix[10];

    const float rowTx = matrix[12];
    const float rowTy = matrix[13];
    const float rowTz = matrix[14];
    const float colTx = matrix[3];
    const float colTy = matrix[7];
    const float colTz = matrix[11];

    file << "  look third-column = (" << lookX << ", " << lookY << ", " << lookZ << ")\n";
    file << "  translation row[12..14] = (" << rowTx << ", " << rowTy << ", " << rowTz << ")\n";
    file << "  translation col[3,7,11] = (" << colTx << ", " << colTy << ", " << colTz << ")\n";

    // Candidate A matches the common DirectX row-vector view layout where the
    // basis lives in the first three columns and translation is [12..14].
    const float eyeRowX = -(rowTx * matrix[0] + rowTy * matrix[1] + rowTz * matrix[2]);
    const float eyeRowY = -(rowTx * matrix[4] + rowTy * matrix[5] + rowTz * matrix[6]);
    const float eyeRowZ = -(rowTx * matrix[8] + rowTy * matrix[9] + rowTz * matrix[10]);

    // Candidate B is the equivalent check for a column-vector style layout.
    const float eyeColX = -(matrix[0] * colTx + matrix[4] * colTy + matrix[8] * colTz);
    const float eyeColY = -(matrix[1] * colTx + matrix[5] * colTy + matrix[9] * colTz);
    const float eyeColZ = -(matrix[2] * colTx + matrix[6] * colTy + matrix[10] * colTz);

    if (finite_vec3(eyeRowX, eyeRowY, eyeRowZ)) {
        file << "  eye candidate row-layout = (" << eyeRowX << ", " << eyeRowY << ", " << eyeRowZ << ")\n";
    }
    if (finite_vec3(eyeColX, eyeColY, eyeColZ)) {
        file << "  eye candidate col-layout = (" << eyeColX << ", " << eyeColY << ", " << eyeColZ << ")\n";
    }
}

void write_camera_scan(std::ofstream &file, void *camera) {
    constexpr std::size_t floatCount =
        (kCameraScanEnd - kCameraScanStart) / sizeof(float) + 1;
    std::array<float, floatCount> values{};
    const auto *start = static_cast<unsigned char *>(camera) + kCameraScanStart;
    if (!readable_range(start, sizeof(values))) {
        file << "  scan +0x180..+0x2A0 unavailable\n";
        return;
    }
    std::memcpy(values.data(), start, sizeof(values));

    file << "  float scan +0x180..+0x2A0:\n    ";
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::uintptr_t offset = kCameraScanStart + i * sizeof(float);
        const float value = values[i];
        file << "+0x" << std::hex << std::uppercase << offset << std::dec << "=";
        if (std::isfinite(value) && std::fabs(value) < 100000000.0f) {
            file << std::fixed << std::setprecision(5) << value;
        } else {
            file << "<non-finite/outlier>";
        }
        if ((i + 1) % 8 == 0 && i + 1 < values.size()) {
            file << "\n    ";
        } else if (i + 1 < values.size()) {
            file << "  ";
        }
    }
    file << "\n";
}

void write_snapshot(std::ofstream &file, unsigned sampleIndex) {
    const CameraChain chain = resolve_camera_chain();
    void *hookCamera = reinterpret_cast<void *>(
        g_observedCamera.load(std::memory_order_acquire));
    void *camera = chain.camera ? chain.camera : hookCamera;

    file << "\n=== sample " << sampleIndex
         << "  t=" << GetTickCount64() << " ms ===\n";
    file << "pointer chain:\n"
         << "  game=" << pointer_string(chain.game)
         << " simulation=" << pointer_string(chain.simulation)
         << " manager=" << pointer_string(chain.manager) << "\n"
         << "  views=" << pointer_string(chain.views)
         << " view=" << pointer_string(chain.view)
         << " viewVtable=" << pointer_string(reinterpret_cast<void *>(chain.viewVtable))
         << " rva=" << rva_string(chain.viewVtable) << "\n"
         << "  chainCamera=" << pointer_string(chain.camera)
         << " cameraVtable=" << pointer_string(reinterpret_cast<void *>(chain.cameraVtable))
         << " rva=" << rva_string(chain.cameraVtable) << "\n"
         << "  hookCamera=" << pointer_string(hookCamera)
         << " same=" << ((chain.camera && hookCamera == chain.camera) ? "yes" : "no/unknown")
         << "\n";

    const bool viewVtableExpected =
        chain.viewVtable == g_gameBase + kExpectedViewVtableA ||
        chain.viewVtable == g_gameBase + kExpectedViewVtableB;
    const bool cameraVtableExpected =
        chain.cameraVtable == g_gameBase + kExpectedCameraVtable;
    file << "validation: viewVtable=" << (viewVtableExpected ? "expected" : "other/unavailable")
         << " cameraVtable=" << (cameraVtableExpected ? "expected" : "other/unavailable") << "\n";

    if (!camera) {
        file << "camera unavailable\n";
        file.flush();
        return;
    }

    std::array<float, 4> heading{};
    float baseFov = 0.0f;
    if (read_value(static_cast<unsigned char *>(camera) + kCameraHeadingOffset, heading)) {
        file << std::fixed << std::setprecision(6)
             << "heading +0x1C0 = (" << heading[0] << ", " << heading[1]
             << ", " << heading[2] << ", " << heading[3] << ")\n";
    }
    if (read_value(static_cast<unsigned char *>(camera) + kCameraBaseFovOffset, baseFov)) {
        file << "baseFov +0x1D0 = " << baseFov << "\n";
    }

    std::array<float, 16> matrix{};
    if (read_value(static_cast<unsigned char *>(camera) + kCachedViewMatrixOffset, matrix)) {
        file << "cached view matrix +0x260:\n";
        write_matrix(file, matrix.data());
        write_matrix_analysis(file, matrix.data());
    } else {
        file << "cached view matrix +0x260 unavailable\n";
    }

    write_camera_scan(file, camera);
    file.flush();
}

DWORD WINAPI research_thread(void *) {
    const std::filesystem::path path = log_path();
    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (!file) {
        g_started.store(false, std::memory_order_release);
        return 0;
    }

    file << "FH2VR M11 camera research log\n"
         << "Read-only instrumentation; VR runtime is not required.\n"
         << "Game base: " << pointer_string(reinterpret_cast<void *>(g_gameBase)) << "\n"
         << "Known ExtraOptions chain: PlayerManager -> +0x48 simulation -> +0x98 manager -> +0x2D0 views -> *view -> +0xC0 camera.\n"
         << "Expected view vtables: 0x2C66188 / 0x2E6E890; camera vtable: 0x2C6C840.\n"
         << "ExtraOptions identifies camera+0x260 as a cached view matrix and uses indices [2,6,10] as look direction.\n"
         << "Sampling once per second for up to five minutes. Drive around and rotate the normal game camera; no headset is needed.\n";
    file.flush();

    for (unsigned sample = 1; sample <= kMaxSamples && !g_stop.load(std::memory_order_relaxed); ++sample) {
        write_snapshot(file, sample);
        for (DWORD waited = 0; waited < kSampleIntervalMs && !g_stop.load(std::memory_order_relaxed); waited += 50) {
            Sleep(50);
        }
    }

    file << "\nCapture finished.\n";
    file.flush();
    g_started.store(false, std::memory_order_release);
    return 0;
}

} // namespace

namespace fh2vr {

void camera_research_observe(void *camera) {
    if (camera) {
        g_observedCamera.store(reinterpret_cast<std::uintptr_t>(camera),
                               std::memory_order_release);
    }
}

bool start_camera_research(std::uintptr_t gameBase) {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel)) {
        return true;
    }

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
