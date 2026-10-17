#include <MinHook.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
constexpr uintptr_t kDriverCameraVtableRva = 0x2E78120;
constexpr uintptr_t kCameraUpdateSlot = 0x20;
constexpr uintptr_t kPlayerManagerRva = 0x34ABA60;
constexpr uintptr_t kViewVtableA = 0x2C66188;
constexpr uintptr_t kViewVtableB = 0x2E6E890;
constexpr uintptr_t kCachedCameraVtable = 0x2C6C840;
constexpr uintptr_t kCachedViewMatrixOffset = 0x260;
constexpr size_t kDriverScanBytes = 0x400;

using PresentFn = HRESULT(WINAPI *)(IDXGISwapChain *, UINT, UINT);
using CameraUpdateFn = void (*)(void *, float, unsigned char);

PresentFn g_originalPresent = nullptr;
CameraUpdateFn g_previousCameraUpdate = nullptr;
uintptr_t g_gameBase = 0;
std::atomic<std::uint64_t> g_cameraUpdates{0};
std::atomic<std::uint64_t> g_persisted{0};
std::atomic<std::uint64_t> g_rebuilt{0};
std::atomic<bool> g_scanDriverRequested{false};
std::atomic<bool> g_primarySwapchainChosen{false};
IDXGISwapChain *g_primarySwapchain = nullptr;
float g_lastMatrix12AfterHook = 0.0f;
bool g_haveLastMatrix12 = false;
ULONGLONG g_lastPresentLogAt = 0;
std::uint64_t g_presentCountWindow = 0;
SRWLOCK g_logLock = SRWLOCK_INIT;

void log_line(const std::string &message) {
    AcquireSRWLockExclusive(&g_logLock);
    FILE *file = nullptr;
    if (fopen_s(&file, "FH2VR_RendererProbe.log", "a") == 0 && file) {
        std::fprintf(file, "[%llu ms] %s\n",
                     static_cast<unsigned long long>(GetTickCount64()), message.c_str());
        std::fclose(file);
    }
    OutputDebugStringA(("[FH2VR probe] " + message + "\n").c_str());
    ReleaseSRWLockExclusive(&g_logLock);
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

void scan_driver_camera(void *camera) {
    if (!camera || !memory_readable(camera, kDriverScanBytes)) {
        log_line("Driver camera scan skipped: object is not readable for 0x400 bytes");
        return;
    }

    float *matrix = resolve_cached_view_matrix();
    if (!matrix || !memory_readable(matrix, sizeof(float) * 16)) {
        log_line("Driver camera scan skipped: cached 0x260 matrix unavailable");
        return;
    }

    const float mx = matrix[12];
    const float mz = matrix[14];
    const auto *values = static_cast<const float *>(camera);
    const size_t floatCount = kDriverScanBytes / sizeof(float);
    unsigned matches = 0;

    log_line("Driver camera known-answer scan: cached X=" + std::to_string(mx) +
             " Z=" + std::to_string(mz));

    for (size_t i = 0; i + 2 < floatCount && matches < 16; ++i) {
        const float x = values[i];
        const float y = values[i + 1];
        const float z = values[i + 2];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
            continue;
        }
        if (std::fabs(x - mx) < 30.0f && std::fabs(z - mz) < 30.0f) {
            char buffer[256]{};
            std::snprintf(buffer, sizeof(buffer),
                          "pos-like triple at +0x%zX: %.5f %.5f %.5f",
                          i * sizeof(float), x, y, z);
            log_line(buffer);
            ++matches;
        }
    }

    if (matches == 0) {
        log_line("Driver camera known-answer scan: no X/Z candidate within +/-30 units");
    }
}

void camera_probe_hook(void *camera, float dt, unsigned char reset) {
    float before = 0.0f;
    if (g_haveLastMatrix12 && read_matrix12(before)) {
        const float tolerance = 0.00001f * ((std::fabs(g_lastMatrix12AfterHook) > 1.0f)
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

    if (g_scanDriverRequested.exchange(false, std::memory_order_acq_rel)) {
        scan_driver_camera(camera);
    }
}

bool install_camera_probe() {
    if (!g_gameBase) {
        return false;
    }
    auto *slot = reinterpret_cast<CameraUpdateFn *>(
        g_gameBase + kDriverCameraVtableRva + kCameraUpdateSlot);
    if (!memory_readable(slot, sizeof(*slot))) {
        log_line("Camera probe: driver vtable slot is unreadable");
        return false;
    }

    CameraUpdateFn current = nullptr;
    std::memcpy(&current, slot, sizeof(current));
    if (!current || current == &camera_probe_hook) {
        log_line("Camera probe: invalid/already hooked slot");
        return false;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection)) {
        log_line("Camera probe: VirtualProtect failed");
        return false;
    }
    g_previousCameraUpdate = current;
    CameraUpdateFn replacement = &camera_probe_hook;
    std::memcpy(slot, &replacement, sizeof(replacement));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), oldProtection, &ignored);
    log_line("Camera probe installed outside the existing FH2VR hook; F12 requests Driver-object scan");
    return true;
}

bool current_process_window(HWND window) {
    if (!window) {
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId();
}

const char *format_name(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    default: return "other";
    }
}

void inspect_swapchain_once(IDXGISwapChain *swapchain) {
    DXGI_SWAP_CHAIN_DESC swapDesc{};
    if (FAILED(swapchain->GetDesc(&swapDesc))) {
        log_line("Present probe: IDXGISwapChain::GetDesc failed");
        return;
    }

    std::string api = "unknown";
    ID3D11Device *device11 = nullptr;
    ID3D12Device *device12 = nullptr;
    if (SUCCEEDED(swapchain->GetDevice(__uuidof(ID3D11Device),
                                       reinterpret_cast<void **>(&device11))) && device11) {
        api = "D3D11";
        device11->Release();
    } else if (SUCCEEDED(swapchain->GetDevice(__uuidof(ID3D12Device),
                                              reinterpret_cast<void **>(&device12))) && device12) {
        api = "D3D12";
        device12->Release();
    }

    UINT width = swapDesc.BufferDesc.Width;
    UINT height = swapDesc.BufferDesc.Height;
    DXGI_FORMAT format = swapDesc.BufferDesc.Format;

    ID3D11Texture2D *buffer11 = nullptr;
    if (SUCCEEDED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                       reinterpret_cast<void **>(&buffer11))) && buffer11) {
        D3D11_TEXTURE2D_DESC desc{};
        buffer11->GetDesc(&desc);
        width = desc.Width;
        height = desc.Height;
        format = desc.Format;
        buffer11->Release();
    } else {
        ID3D12Resource *buffer12 = nullptr;
        if (SUCCEEDED(swapchain->GetBuffer(0, __uuidof(ID3D12Resource),
                                           reinterpret_cast<void **>(&buffer12))) && buffer12) {
            const D3D12_RESOURCE_DESC desc = buffer12->GetDesc();
            width = static_cast<UINT>(desc.Width);
            height = desc.Height;
            format = desc.Format;
            buffer12->Release();
        }
    }

    char buffer[512]{};
    std::snprintf(buffer, sizeof(buffer),
                  "Present probe locked game swapchain=%p API=%s HWND=%p backbuffer=%ux%u format=%s(%d) buffers=%u swapEffect=%d",
                  static_cast<void *>(swapchain), api.c_str(), static_cast<void *>(swapDesc.OutputWindow),
                  width, height, format_name(format), static_cast<int>(format), swapDesc.BufferCount,
                  static_cast<int>(swapDesc.SwapEffect));
    log_line(buffer);
}

HRESULT WINAPI present_hook(IDXGISwapChain *swapchain, UINT syncInterval, UINT flags) {
    if (!swapchain || !g_originalPresent) {
        return E_FAIL;
    }

    if (!g_primarySwapchainChosen.load(std::memory_order_acquire)) {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (SUCCEEDED(swapchain->GetDesc(&desc)) && current_process_window(desc.OutputWindow)) {
            RECT client{};
            if (GetClientRect(desc.OutputWindow, &client)) {
                const LONG width = client.right - client.left;
                const LONG height = client.bottom - client.top;
                if (width >= 640 && height >= 360) {
                    bool expected = false;
                    if (g_primarySwapchainChosen.compare_exchange_strong(
                            expected, true, std::memory_order_acq_rel)) {
                        g_primarySwapchain = swapchain;
                        inspect_swapchain_once(swapchain);
                        g_lastPresentLogAt = GetTickCount64();
                    }
                }
            }
        }
    }

    if (swapchain == g_primarySwapchain) {
        ++g_presentCountWindow;
        const ULONGLONG now = GetTickCount64();
        if (now - g_lastPresentLogAt >= 1000) {
            const ULONGLONG elapsed = now - g_lastPresentLogAt;
            const double fps = elapsed > 0
                                   ? static_cast<double>(g_presentCountWindow) * 1000.0 /
                                         static_cast<double>(elapsed)
                                   : 0.0;
            const std::uint64_t camera = g_cameraUpdates.exchange(0, std::memory_order_relaxed);
            const std::uint64_t persisted = g_persisted.exchange(0, std::memory_order_relaxed);
            const std::uint64_t rebuilt = g_rebuilt.exchange(0, std::memory_order_relaxed);
            char buffer[320]{};
            std::snprintf(buffer, sizeof(buffer),
                          "Present %.2f fps; camera updates/s=%llu; 0x260 persisted=%llu rebuilt=%llu; syncInterval=%u flags=0x%X",
                          fps,
                          static_cast<unsigned long long>(camera),
                          static_cast<unsigned long long>(persisted),
                          static_cast<unsigned long long>(rebuilt), syncInterval, flags);
            log_line(buffer);
            g_presentCountWindow = 0;
            g_lastPresentLogAt = now;
        }
    }

    return g_originalPresent(swapchain, syncInterval, flags);
}

LRESULT CALLBACK dummy_window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(window, message, wParam, lParam);
}

bool install_present_probe() {
    const wchar_t className[] = L"FH2VR_RendererProbeWindow";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = dummy_window_proc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    RegisterClassW(&windowClass);

    HWND window = CreateWindowExW(0, className, L"", WS_OVERLAPPEDWINDOW,
                                  0, 0, 64, 64, nullptr, nullptr,
                                  windowClass.hInstance, nullptr);
    if (!window) {
        log_line("Present probe: failed to create dummy window");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = 64;
    desc.BufferDesc.Height = 64;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain *dummySwapchain = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    D3D_FEATURE_LEVEL featureLevel{};
    const HRESULT createResult = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &desc, &dummySwapchain, &device, &featureLevel, &context);
    (void)featureLevel;
    if (FAILED(createResult) || !dummySwapchain) {
        if (context) context->Release();
        if (device) device->Release();
        DestroyWindow(window);
        log_line("Present probe: D3D11CreateDeviceAndSwapChain dummy creation failed");
        return false;
    }

    void **vtable = *reinterpret_cast<void ***>(dummySwapchain);
    void *target = vtable[8];

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        dummySwapchain->Release();
        context->Release();
        device->Release();
        DestroyWindow(window);
        log_line("Present probe: MH_Initialize failed status=" + std::to_string(initStatus));
        return false;
    }

    const MH_STATUS createStatus = MH_CreateHook(
        target, reinterpret_cast<void *>(&present_hook),
        reinterpret_cast<void **>(&g_originalPresent));
    if (createStatus != MH_OK) {
        dummySwapchain->Release();
        context->Release();
        device->Release();
        DestroyWindow(window);
        log_line("Present probe: MH_CreateHook failed status=" + std::to_string(createStatus));
        return false;
    }

    const MH_STATUS enableStatus = MH_EnableHook(target);
    dummySwapchain->Release();
    context->Release();
    device->Release();
    DestroyWindow(window);

    if (enableStatus != MH_OK) {
        log_line("Present probe: MH_EnableHook failed status=" + std::to_string(enableStatus));
        return false;
    }

    log_line("DXGI Present hook installed from dummy D3D11 swapchain method table");
    return true;
}

DWORD WINAPI probe_thread(void *) {
    Sleep(3000); // Let FH2ML/FH2VR finish installing their normal camera hook first.
    g_gameBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    log_line("Renderer probe starting; d3d11=" +
             std::string(GetModuleHandleW(L"d3d11.dll") ? "loaded" : "not-loaded") +
             " d3d12=" + std::string(GetModuleHandleW(L"d3d12.dll") ? "loaded" : "not-loaded") +
             " dxgi=" + std::string(GetModuleHandleW(L"dxgi.dll") ? "loaded" : "not-loaded"));

    install_camera_probe();
    install_present_probe();

    bool f12WasDown = false;
    for (;;) {
        const bool f12Down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (f12Down && !f12WasDown) {
            g_scanDriverRequested.store(true, std::memory_order_release);
            log_line("F12 observed by renderer probe: Driver camera scan requested");
        }
        f12WasDown = f12Down;
        Sleep(4);
    }
}

struct ProbeBootstrap {
    ProbeBootstrap() {
        HANDLE thread = CreateThread(nullptr, 0, probe_thread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    }
};

ProbeBootstrap g_probeBootstrap;
} // namespace
