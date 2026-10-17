#include <MinHook.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {
using PresentFn = HRESULT(WINAPI *)(IDXGISwapChain *, UINT, UINT);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE *)(
    ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);

struct EyeCopyState {
    ID3D12Resource *texture = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    UINT64 fenceValue = 0;
    bool hasFrame = false;
};

PresentFn g_originalPresent = nullptr;
ExecuteCommandListsFn g_originalExecuteCommandLists = nullptr;

std::atomic<bool> g_primarySwapchainChosen{false};
IDXGISwapChain *g_primarySwapchain = nullptr;
IDXGISwapChain3 *g_primarySwapchain3 = nullptr;
std::atomic<ID3D12Device *> g_primaryDevice{nullptr};
std::atomic<ID3D12CommandQueue *> g_primaryQueue{nullptr};

EyeCopyState g_eyeCopies[2]{};
ID3D12GraphicsCommandList *g_copyList = nullptr;
ID3D12Fence *g_copyFence = nullptr;
HANDLE g_fenceEvent = nullptr;
UINT64 g_nextFenceValue = 1;
D3D12_RESOURCE_DESC g_retainedDesc{};
bool g_copyResourcesReady = false;
int g_nextEye = 0;
UINT g_lastBackBufferIndex = 0;

std::atomic<std::uint64_t> g_gameExecuteCalls{0};
std::uint64_t g_presentCountWindow = 0;
std::uint64_t g_copyCountWindow = 0;
ULONGLONG g_lastPresentLogAt = 0;
SRWLOCK g_logLock = SRWLOCK_INIT;
thread_local bool g_internalQueueSubmit = false;

void log_line(const std::string &message) {
    AcquireSRWLockExclusive(&g_logLock);
    FILE *file = nullptr;
    if (fopen_s(&file, "FH2VR_RendererProbe.log", "a") == 0 && file) {
        std::fprintf(file, "[%llu ms] %s\n",
                     static_cast<unsigned long long>(GetTickCount64()), message.c_str());
        std::fclose(file);
    }
    OutputDebugStringA(("[FH2VR D3D12 probe] " + message + "\n").c_str());
    ReleaseSRWLockExclusive(&g_logLock);
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

bool same_com_identity(IUnknown *a, IUnknown *b) {
    if (!a || !b) {
        return false;
    }
    IUnknown *identityA = nullptr;
    IUnknown *identityB = nullptr;
    const HRESULT resultA = a->QueryInterface(IID_PPV_ARGS(&identityA));
    const HRESULT resultB = b->QueryInterface(IID_PPV_ARGS(&identityB));
    const bool same = SUCCEEDED(resultA) && SUCCEEDED(resultB) && identityA == identityB;
    if (identityA) {
        identityA->Release();
    }
    if (identityB) {
        identityB->Release();
    }
    return same;
}

bool queue_matches_primary_device(ID3D12CommandQueue *queue) {
    ID3D12Device *primaryDevice = g_primaryDevice.load(std::memory_order_acquire);
    if (!queue || !primaryDevice) {
        return false;
    }

    const D3D12_COMMAND_QUEUE_DESC queueDesc = queue->GetDesc();
    if (queueDesc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return false;
    }

    ID3D12Device *queueDevice = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || !queueDevice) {
        return false;
    }
    const bool same = same_com_identity(primaryDevice, queueDevice);
    queueDevice->Release();
    return same;
}

void STDMETHODCALLTYPE execute_command_lists_hook(
    ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists) {
    if (!g_originalExecuteCommandLists) {
        return;
    }

    if (!g_internalQueueSubmit) {
        g_gameExecuteCalls.fetch_add(1, std::memory_order_relaxed);

        if (!g_primaryQueue.load(std::memory_order_acquire) &&
            queue_matches_primary_device(queue)) {
            queue->AddRef();
            ID3D12CommandQueue *expected = nullptr;
            if (g_primaryQueue.compare_exchange_strong(
                    expected, queue, std::memory_order_acq_rel)) {
                const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
                char buffer[320]{};
                std::snprintf(buffer, sizeof(buffer),
                              "Captured FH2 DIRECT command queue=%p priority=%d flags=0x%X nodeMask=0x%X",
                              static_cast<void *>(queue), desc.Priority,
                              static_cast<unsigned>(desc.Flags), desc.NodeMask);
                log_line(buffer);
            } else {
                queue->Release();
            }
        }
    }

    g_originalExecuteCommandLists(queue, count, lists);
}

void release_copy_resources() {
    if (g_copyFence) {
        const UINT64 maxFence =
            g_eyeCopies[0].fenceValue > g_eyeCopies[1].fenceValue
                ? g_eyeCopies[0].fenceValue
                : g_eyeCopies[1].fenceValue;
        if (maxFence != 0 && g_copyFence->GetCompletedValue() < maxFence && g_fenceEvent) {
            if (SUCCEEDED(g_copyFence->SetEventOnCompletion(maxFence, g_fenceEvent))) {
                WaitForSingleObject(g_fenceEvent, 1000);
            }
        }
    }

    if (g_copyList) {
        g_copyList->Release();
        g_copyList = nullptr;
    }
    for (auto &eye : g_eyeCopies) {
        if (eye.texture) {
            eye.texture->Release();
            eye.texture = nullptr;
        }
        if (eye.allocator) {
            eye.allocator->Release();
            eye.allocator = nullptr;
        }
        eye.fenceValue = 0;
        eye.hasFrame = false;
    }
    if (g_copyFence) {
        g_copyFence->Release();
        g_copyFence = nullptr;
    }
    if (g_fenceEvent) {
        CloseHandle(g_fenceEvent);
        g_fenceEvent = nullptr;
    }
    g_copyResourcesReady = false;
    g_retainedDesc = {};
    g_nextFenceValue = 1;
}

bool wait_for_eye_copy(EyeCopyState &eye) {
    if (!eye.fenceValue || !g_copyFence) {
        return true;
    }
    if (g_copyFence->GetCompletedValue() >= eye.fenceValue) {
        return true;
    }
    if (!g_fenceEvent || FAILED(g_copyFence->SetEventOnCompletion(eye.fenceValue, g_fenceEvent))) {
        log_line("D3D12 copy wait failed to arm fence event");
        return false;
    }
    const DWORD wait = WaitForSingleObject(g_fenceEvent, 1000);
    if (wait != WAIT_OBJECT_0) {
        log_line("D3D12 copy wait timed out; skipping frame instead of reusing busy resources");
        return false;
    }
    return true;
}

bool resource_desc_matches(const D3D12_RESOURCE_DESC &a, const D3D12_RESOURCE_DESC &b) {
    return a.Dimension == b.Dimension &&
           a.Alignment == b.Alignment &&
           a.Width == b.Width &&
           a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize &&
           a.MipLevels == b.MipLevels &&
           a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality &&
           a.Layout == b.Layout &&
           a.Flags == b.Flags;
}

bool create_copy_resources(ID3D12Resource *backBuffer) {
    ID3D12Device *device = g_primaryDevice.load(std::memory_order_acquire);
    if (!device || !backBuffer) {
        return false;
    }

    const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
    if (g_copyResourcesReady && resource_desc_matches(desc, g_retainedDesc)) {
        return true;
    }

    if (g_copyResourcesReady) {
        log_line("Backbuffer description changed; rebuilding retained D3D12 eye resources");
        release_copy_resources();
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    for (int eye = 0; eye < 2; ++eye) {
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&g_eyeCopies[eye].texture)))) {
            log_line("CreateCommittedResource failed for retained eye texture");
            release_copy_resources();
            return false;
        }
        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&g_eyeCopies[eye].allocator)))) {
            log_line("CreateCommandAllocator failed for retained eye copy");
            release_copy_resources();
            return false;
        }
    }

    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_eyeCopies[0].allocator,
            nullptr, IID_PPV_ARGS(&g_copyList))) || !g_copyList) {
        log_line("CreateCommandList failed for D3D12 eye copy");
        release_copy_resources();
        return false;
    }
    if (FAILED(g_copyList->Close())) {
        log_line("Initial Close failed for D3D12 eye copy command list");
        release_copy_resources();
        return false;
    }

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_copyFence)))) {
        log_line("CreateFence failed for D3D12 eye copy");
        release_copy_resources();
        return false;
    }
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent) {
        log_line("CreateEvent failed for D3D12 eye copy fence");
        release_copy_resources();
        return false;
    }

    g_retainedDesc = desc;
    g_copyResourcesReady = true;
    g_nextEye = 0;

    char buffer[384]{};
    std::snprintf(buffer, sizeof(buffer),
                  "D3D12 retained eye resources ready: %llux%u format=%s(%d) left=%p right=%p",
                  static_cast<unsigned long long>(desc.Width), desc.Height,
                  format_name(desc.Format), static_cast<int>(desc.Format),
                  static_cast<void *>(g_eyeCopies[0].texture),
                  static_cast<void *>(g_eyeCopies[1].texture));
    log_line(buffer);
    return true;
}

bool copy_current_backbuffer() {
    ID3D12CommandQueue *queue = g_primaryQueue.load(std::memory_order_acquire);
    if (!queue || !g_primarySwapchain3) {
        return false;
    }

    const UINT backBufferIndex = g_primarySwapchain3->GetCurrentBackBufferIndex();
    ID3D12Resource *backBuffer = nullptr;
    if (FAILED(g_primarySwapchain3->GetBuffer(
            backBufferIndex, IID_PPV_ARGS(&backBuffer))) || !backBuffer) {
        log_line("GetBuffer failed for current D3D12 swapchain backbuffer");
        return false;
    }

    const bool resourcesReady = create_copy_resources(backBuffer);
    if (!resourcesReady) {
        backBuffer->Release();
        return false;
    }

    EyeCopyState &eye = g_eyeCopies[g_nextEye];
    if (!wait_for_eye_copy(eye)) {
        backBuffer->Release();
        return false;
    }

    if (FAILED(eye.allocator->Reset())) {
        log_line("D3D12 eye copy allocator Reset failed");
        backBuffer->Release();
        return false;
    }
    if (FAILED(g_copyList->Reset(eye.allocator, nullptr))) {
        log_line("D3D12 eye copy command list Reset failed");
        backBuffer->Release();
        return false;
    }

    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barriers[0].Transition.pResource = backBuffer;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    barriers[1] = barriers[0];
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

    g_copyList->ResourceBarrier(1, &barriers[0]);
    g_copyList->CopyResource(eye.texture, backBuffer);
    g_copyList->ResourceBarrier(1, &barriers[1]);

    if (FAILED(g_copyList->Close())) {
        log_line("D3D12 eye copy command list Close failed");
        backBuffer->Release();
        return false;
    }

    ID3D12CommandList *lists[] = {g_copyList};
    g_internalQueueSubmit = true;
    queue->ExecuteCommandLists(1, lists);
    g_internalQueueSubmit = false;

    const UINT64 fenceValue = g_nextFenceValue++;
    if (FAILED(queue->Signal(g_copyFence, fenceValue))) {
        log_line("D3D12 eye copy queue Signal failed");
        backBuffer->Release();
        return false;
    }

    eye.fenceValue = fenceValue;
    eye.hasFrame = true;
    g_lastBackBufferIndex = backBufferIndex;
    ++g_copyCountWindow;

    static bool loggedLeft = false;
    static bool loggedRight = false;
    bool &logged = g_nextEye == 0 ? loggedLeft : loggedRight;
    if (!logged) {
        char buffer[320]{};
        std::snprintf(buffer, sizeof(buffer),
                      "Direct D3D12 backbuffer copy OK: eye=%s bbIndex=%u source=%p retained=%p fence=%llu",
                      g_nextEye == 0 ? "left" : "right", backBufferIndex,
                      static_cast<void *>(backBuffer), static_cast<void *>(eye.texture),
                      static_cast<unsigned long long>(fenceValue));
        log_line(buffer);
        logged = true;
    }

    g_nextEye ^= 1;
    backBuffer->Release();
    return true;
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
    if (SUCCEEDED(swapchain->GetDevice(
            __uuidof(ID3D11Device), reinterpret_cast<void **>(&device11))) && device11) {
        api = "D3D11";
        device11->Release();
    } else if (SUCCEEDED(swapchain->GetDevice(
                   __uuidof(ID3D12Device), reinterpret_cast<void **>(&device12))) && device12) {
        api = "D3D12";
        ID3D12Device *expected = nullptr;
        if (!g_primaryDevice.compare_exchange_strong(
                expected, device12, std::memory_order_acq_rel)) {
            device12->Release();
        }
    }

    IDXGISwapChain3 *swapchain3 = nullptr;
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) && swapchain3) {
        g_primarySwapchain3 = swapchain3;
    }

    UINT width = swapDesc.BufferDesc.Width;
    UINT height = swapDesc.BufferDesc.Height;
    DXGI_FORMAT format = swapDesc.BufferDesc.Format;

    ID3D12Resource *buffer12 = nullptr;
    if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer12))) && buffer12) {
        const D3D12_RESOURCE_DESC desc = buffer12->GetDesc();
        width = static_cast<UINT>(desc.Width);
        height = desc.Height;
        format = desc.Format;
        buffer12->Release();
    }

    char buffer[512]{};
    std::snprintf(buffer, sizeof(buffer),
                  "Present probe locked game swapchain=%p API=%s HWND=%p backbuffer=%ux%u format=%s(%d) buffers=%u swapEffect=%d swapchain3=%s",
                  static_cast<void *>(swapchain), api.c_str(),
                  static_cast<void *>(swapDesc.OutputWindow), width, height,
                  format_name(format), static_cast<int>(format), swapDesc.BufferCount,
                  static_cast<int>(swapDesc.SwapEffect),
                  g_primarySwapchain3 ? "yes" : "no");
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
                        swapchain->AddRef();
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
        copy_current_backbuffer();

        const ULONGLONG now = GetTickCount64();
        if (now - g_lastPresentLogAt >= 1000) {
            const ULONGLONG elapsed = now - g_lastPresentLogAt;
            const double fps = elapsed > 0
                                   ? static_cast<double>(g_presentCountWindow) * 1000.0 /
                                         static_cast<double>(elapsed)
                                   : 0.0;
            const std::uint64_t executeCalls =
                g_gameExecuteCalls.exchange(0, std::memory_order_relaxed);
            ID3D12CommandQueue *queue = g_primaryQueue.load(std::memory_order_acquire);
            char buffer[384]{};
            std::snprintf(buffer, sizeof(buffer),
                          "Present %.2f fps; game ExecuteCommandLists/s=%llu; queue=%p; directCopies/s=%llu; nextEye=%s; lastBB=%u; syncInterval=%u flags=0x%X",
                          fps, static_cast<unsigned long long>(executeCalls),
                          static_cast<void *>(queue),
                          static_cast<unsigned long long>(g_copyCountWindow),
                          g_nextEye == 0 ? "left" : "right", g_lastBackBufferIndex,
                          syncInterval, flags);
            log_line(buffer);
            g_presentCountWindow = 0;
            g_copyCountWindow = 0;
            g_lastPresentLogAt = now;
        }
    }

    return g_originalPresent(swapchain, syncInterval, flags);
}

LRESULT CALLBACK dummy_window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(window, message, wParam, lParam);
}

bool ensure_minhook_initialized() {
    const MH_STATUS status = MH_Initialize();
    if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED) {
        return true;
    }
    log_line("MH_Initialize failed status=" + std::to_string(status));
    return false;
}

bool install_present_hook() {
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
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &desc, &dummySwapchain, &device,
        &featureLevel, &context);
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
    const MH_STATUS createStatus = MH_CreateHook(
        target, reinterpret_cast<void *>(&present_hook),
        reinterpret_cast<void **>(&g_originalPresent));
    const MH_STATUS enableStatus =
        createStatus == MH_OK ? MH_EnableHook(target) : createStatus;

    dummySwapchain->Release();
    context->Release();
    device->Release();
    DestroyWindow(window);

    if (createStatus != MH_OK || enableStatus != MH_OK) {
        log_line("Present hook install failed create=" + std::to_string(createStatus) +
                 " enable=" + std::to_string(enableStatus));
        return false;
    }

    log_line("DXGI Present hook installed from dummy D3D11 swapchain method table");
    return true;
}

bool install_execute_command_lists_hook() {
    ID3D12Device *device = nullptr;
    const HRESULT deviceResult = D3D12CreateDevice(
        nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(deviceResult) || !device) {
        log_line("ExecuteCommandLists probe: D3D12CreateDevice failed");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;

    ID3D12CommandQueue *queue = nullptr;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))) || !queue) {
        device->Release();
        log_line("ExecuteCommandLists probe: dummy CreateCommandQueue failed");
        return false;
    }

    void **vtable = *reinterpret_cast<void ***>(queue);
    void *target = vtable[10];
    const MH_STATUS createStatus = MH_CreateHook(
        target, reinterpret_cast<void *>(&execute_command_lists_hook),
        reinterpret_cast<void **>(&g_originalExecuteCommandLists));
    const MH_STATUS enableStatus =
        createStatus == MH_OK ? MH_EnableHook(target) : createStatus;

    queue->Release();
    device->Release();

    if (createStatus != MH_OK || enableStatus != MH_OK) {
        log_line("ExecuteCommandLists hook install failed create=" +
                 std::to_string(createStatus) + " enable=" +
                 std::to_string(enableStatus));
        return false;
    }

    log_line("ID3D12CommandQueue::ExecuteCommandLists hook installed from dummy DIRECT queue vtable[10]");
    return true;
}

DWORD WINAPI probe_thread(void *) {
    Sleep(3000);

    log_line("Renderer probe starting: direct D3D12 M1; d3d11=" +
             std::string(GetModuleHandleW(L"d3d11.dll") ? "loaded" : "not-loaded") +
             " d3d12=" + std::string(GetModuleHandleW(L"d3d12.dll") ? "loaded" : "not-loaded") +
             " dxgi=" + std::string(GetModuleHandleW(L"dxgi.dll") ? "loaded" : "not-loaded"));

    if (!ensure_minhook_initialized()) {
        return 0;
    }
    install_execute_command_lists_hook();
    install_present_hook();
    return 0;
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
