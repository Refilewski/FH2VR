#include "RendererPreview.h"

#include <MinHook.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_4.h>
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
std::atomic<int> g_latestCopiedEye{-1};
UINT g_lastBackBufferIndex = 0;

std::atomic<std::uint64_t> g_gameExecuteCalls{0};
std::atomic<std::uint64_t> g_openVrPairSubmits{0};
std::uint64_t g_presentCountWindow = 0;
std::uint64_t g_copyCountWindow = 0;
ULONGLONG g_lastPresentLogAt = 0;
SRWLOCK g_logLock = SRWLOCK_INIT;
SRWLOCK g_resourceLock = SRWLOCK_INIT;
thread_local bool g_internalQueueSubmit = false;

void log_line(const std::string &message) {
    AcquireSRWLockExclusive(&g_logLock);
    FILE *file = nullptr;
    if (fopen_s(&file, "FH2VR_RendererProbe.log", "a") == 0 && file) {
        std::fprintf(file, "[%llu ms] %s\n",
                     static_cast<unsigned long long>(GetTickCount64()), message.c_str());
        std::fclose(file);
    }
    OutputDebugStringA(("[FH2VR D3D12 M2] " + message + "\n").c_str());
    ReleaseSRWLockExclusive(&g_logLock);
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

bool current_process_window(HWND window) {
    if (!window) {
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId();
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
        ID3D12CommandQueue *primary = g_primaryQueue.load(std::memory_order_acquire);
        if (!primary && queue_matches_primary_device(queue)) {
            queue->AddRef();
            ID3D12CommandQueue *expected = nullptr;
            if (g_primaryQueue.compare_exchange_strong(
                    expected, queue, std::memory_order_acq_rel)) {
                primary = queue;
                const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
                char buffer[320]{};
                std::snprintf(buffer, sizeof(buffer),
                              "Captured FH2 DIRECT command queue=%p priority=%d flags=0x%X nodeMask=0x%X",
                              static_cast<void *>(queue), desc.Priority,
                              static_cast<unsigned>(desc.Flags), desc.NodeMask);
                log_line(buffer);
            } else {
                queue->Release();
                primary = expected;
            }
        }
        if (primary && queue == primary) {
            g_gameExecuteCalls.fetch_add(1, std::memory_order_relaxed);
        }
    }

    g_originalExecuteCommandLists(queue, count, lists);
}

D3D12_RESOURCE_BARRIER transition_barrier(
    ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

void release_copy_resources_unlocked() {
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

    g_latestCopiedEye.store(-1, std::memory_order_release);
    g_copyResourcesReady = false;
    g_retainedDesc = {};
    g_nextFenceValue = 1;
    g_nextEye = 0;
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

bool ensure_copy_resources(ID3D12Resource *backBuffer) {
    ID3D12Device *device = g_primaryDevice.load(std::memory_order_acquire);
    if (!device || !backBuffer) {
        return false;
    }

    const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
    AcquireSRWLockExclusive(&g_resourceLock);

    if (g_copyResourcesReady && resource_desc_matches(desc, g_retainedDesc)) {
        ReleaseSRWLockExclusive(&g_resourceLock);
        return true;
    }

    if (g_copyResourcesReady) {
        log_line("Backbuffer description changed; rebuilding retained D3D12 textures");
        release_copy_resources_unlocked();
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    bool ok = true;
    for (int eye = 0; eye < 2 && ok; ++eye) {
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&g_eyeCopies[eye].texture)))) {
            log_line("CreateCommittedResource failed for retained D3D12 texture");
            ok = false;
            break;
        }
        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&g_eyeCopies[eye].allocator)))) {
            log_line("CreateCommandAllocator failed for retained D3D12 copy");
            ok = false;
            break;
        }
    }

    if (ok && (FAILED(device->CreateCommandList(
                   0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_eyeCopies[0].allocator,
                   nullptr, IID_PPV_ARGS(&g_copyList))) || !g_copyList)) {
        log_line("CreateCommandList failed for retained D3D12 copy");
        ok = false;
    }
    if (ok && FAILED(g_copyList->Close())) {
        log_line("Initial Close failed for retained D3D12 command list");
        ok = false;
    }
    if (ok && FAILED(device->CreateFence(
                   0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_copyFence)))) {
        log_line("CreateFence failed for retained D3D12 copy");
        ok = false;
    }
    if (ok) {
        g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_fenceEvent) {
            log_line("CreateEvent failed for retained D3D12 fence");
            ok = false;
        }
    }

    if (!ok) {
        release_copy_resources_unlocked();
        ReleaseSRWLockExclusive(&g_resourceLock);
        return false;
    }

    g_retainedDesc = desc;
    g_copyResourcesReady = true;
    g_nextEye = 0;
    g_latestCopiedEye.store(-1, std::memory_order_release);

    char buffer[384]{};
    std::snprintf(buffer, sizeof(buffer),
                  "D3D12 retained textures ready: %llux%u format=%s(%d) left=%p right=%p",
                  static_cast<unsigned long long>(desc.Width), desc.Height,
                  format_name(desc.Format), static_cast<int>(desc.Format),
                  static_cast<void *>(g_eyeCopies[0].texture),
                  static_cast<void *>(g_eyeCopies[1].texture));
    log_line(buffer);

    ReleaseSRWLockExclusive(&g_resourceLock);
    return true;
}

bool wait_for_eye_copy_unlocked(EyeCopyState &eye) {
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
        log_line("D3D12 copy wait timed out; skipping busy retained texture");
        return false;
    }
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
        log_line("GetBuffer failed for current D3D12 backbuffer");
        return false;
    }

    if (!ensure_copy_resources(backBuffer)) {
        backBuffer->Release();
        return false;
    }

    AcquireSRWLockExclusive(&g_resourceLock);
    EyeCopyState &eye = g_eyeCopies[g_nextEye];
    if (!wait_for_eye_copy_unlocked(eye)) {
        ReleaseSRWLockExclusive(&g_resourceLock);
        backBuffer->Release();
        return false;
    }

    if (FAILED(eye.allocator->Reset()) ||
        FAILED(g_copyList->Reset(eye.allocator, nullptr))) {
        log_line("D3D12 retained copy Reset failed");
        ReleaseSRWLockExclusive(&g_resourceLock);
        backBuffer->Release();
        return false;
    }

    if (eye.hasFrame) {
        const D3D12_RESOURCE_BARRIER toCopyDest = transition_barrier(
            eye.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
        g_copyList->ResourceBarrier(1, &toCopyDest);
    }

    const D3D12_RESOURCE_BARRIER sourceToCopy = transition_barrier(
        backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_copyList->ResourceBarrier(1, &sourceToCopy);
    g_copyList->CopyResource(eye.texture, backBuffer);

    const D3D12_RESOURCE_BARRIER sourceToPresent = transition_barrier(
        backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    const D3D12_RESOURCE_BARRIER retainedToShader = transition_barrier(
        eye.texture, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D3D12_RESOURCE_BARRIER finishBarriers[2]{sourceToPresent, retainedToShader};
    g_copyList->ResourceBarrier(2, finishBarriers);

    if (FAILED(g_copyList->Close())) {
        log_line("D3D12 retained copy command list Close failed");
        ReleaseSRWLockExclusive(&g_resourceLock);
        backBuffer->Release();
        return false;
    }

    ID3D12CommandList *lists[] = {g_copyList};
    g_internalQueueSubmit = true;
    queue->ExecuteCommandLists(1, lists);
    g_internalQueueSubmit = false;

    const UINT64 fenceValue = g_nextFenceValue++;
    if (FAILED(queue->Signal(g_copyFence, fenceValue))) {
        log_line("D3D12 retained copy queue Signal failed");
        ReleaseSRWLockExclusive(&g_resourceLock);
        backBuffer->Release();
        return false;
    }

    const int copiedEye = g_nextEye;
    eye.fenceValue = fenceValue;
    eye.hasFrame = true;
    g_lastBackBufferIndex = backBufferIndex;
    g_latestCopiedEye.store(copiedEye, std::memory_order_release);
    ++g_copyCountWindow;

    static bool loggedLeft = false;
    static bool loggedRight = false;
    bool &logged = copiedEye == 0 ? loggedLeft : loggedRight;
    if (!logged) {
        char buffer[320]{};
        std::snprintf(buffer, sizeof(buffer),
                      "Direct D3D12 backbuffer copy OK: slot=%s bbIndex=%u source=%p retained=%p fence=%llu finalState=PIXEL_SHADER_RESOURCE",
                      copiedEye == 0 ? "A" : "B", backBufferIndex,
                      static_cast<void *>(backBuffer), static_cast<void *>(eye.texture),
                      static_cast<unsigned long long>(fenceValue));
        log_line(buffer);
        logged = true;
    }

    g_nextEye ^= 1;
    ReleaseSRWLockExclusive(&g_resourceLock);
    backBuffer->Release();
    return true;
}

vr::VRTextureBounds_t mono_aspect_bounds(const D3D12_RESOURCE_DESC &desc) {
    vr::VRTextureBounds_t bounds{0.0f, 0.0f, 1.0f, 1.0f};
    vr::IVRSystem *system = vr::VRSystem();
    if (!system || desc.Width == 0 || desc.Height == 0) {
        return bounds;
    }

    std::uint32_t recommendedWidth = 0;
    std::uint32_t recommendedHeight = 0;
    system->GetRecommendedRenderTargetSize(&recommendedWidth, &recommendedHeight);
    if (recommendedWidth == 0 || recommendedHeight == 0) {
        return bounds;
    }

    const float sourceAspect =
        static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
    const float eyeAspect =
        static_cast<float>(recommendedWidth) / static_cast<float>(recommendedHeight);

    if (sourceAspect > eyeAspect) {
        const float visibleWidth = eyeAspect / sourceAspect;
        const float margin = (1.0f - visibleWidth) * 0.5f;
        bounds.uMin = margin;
        bounds.uMax = 1.0f - margin;
    } else if (sourceAspect < eyeAspect) {
        const float visibleHeight = sourceAspect / eyeAspect;
        const float margin = (1.0f - visibleHeight) * 0.5f;
        bounds.vMin = margin;
        bounds.vMax = 1.0f - margin;
    }
    return bounds;
}

std::string compositor_error(const char *operation, vr::EVRCompositorError result) {
    return std::string(operation) + " failed (OpenVR compositor error " +
           std::to_string(static_cast<int>(result)) + ")";
}

void inspect_swapchain_once(IDXGISwapChain *swapchain) {
    DXGI_SWAP_CHAIN_DESC swapDesc{};
    if (FAILED(swapchain->GetDesc(&swapDesc))) {
        log_line("Present M2: IDXGISwapChain::GetDesc failed");
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
                  "M2 locked game swapchain=%p API=%s HWND=%p backbuffer=%ux%u format=%s(%d) buffers=%u swapEffect=%d swapchain3=%s",
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
            const std::uint64_t openVrPairs =
                g_openVrPairSubmits.exchange(0, std::memory_order_relaxed);
            ID3D12CommandQueue *queue = g_primaryQueue.load(std::memory_order_acquire);
            char buffer[448]{};
            std::snprintf(buffer, sizeof(buffer),
                          "M2 Present %.2f fps; game ExecuteCommandLists/s=%llu; queue=%p; directCopies/s=%llu; OpenVR mono pairs/s=%llu; latestSlot=%d; lastBB=%u; syncInterval=%u flags=0x%X",
                          fps, static_cast<unsigned long long>(executeCalls),
                          static_cast<void *>(queue),
                          static_cast<unsigned long long>(g_copyCountWindow),
                          static_cast<unsigned long long>(openVrPairs),
                          g_latestCopiedEye.load(std::memory_order_relaxed),
                          g_lastBackBufferIndex, syncInterval, flags);
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
    const wchar_t className[] = L"FH2VR_RendererM2Window";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = dummy_window_proc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    RegisterClassW(&windowClass);

    HWND window = CreateWindowExW(0, className, L"", WS_OVERLAPPEDWINDOW,
                                  0, 0, 64, 64, nullptr, nullptr,
                                  windowClass.hInstance, nullptr);
    if (!window) {
        log_line("M2 Present: failed to create dummy window");
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
        log_line("M2 Present: D3D11 dummy swapchain creation failed");
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
        log_line("M2 Present hook install failed create=" + std::to_string(createStatus) +
                 " enable=" + std::to_string(enableStatus));
        return false;
    }
    log_line("M2 DXGI Present hook installed");
    return true;
}

bool install_execute_command_lists_hook() {
    ID3D12Device *device = nullptr;
    const HRESULT deviceResult = D3D12CreateDevice(
        nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(deviceResult) || !device) {
        log_line("M2 ExecuteCommandLists: D3D12CreateDevice failed");
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
        log_line("M2 ExecuteCommandLists: dummy CreateCommandQueue failed");
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
        log_line("M2 ExecuteCommandLists hook install failed create=" +
                 std::to_string(createStatus) + " enable=" +
                 std::to_string(enableStatus));
        return false;
    }
    log_line("M2 ID3D12CommandQueue::ExecuteCommandLists hook installed");
    return true;
}

DWORD WINAPI renderer_thread(void *) {
    Sleep(3000);
    log_line("Renderer M2 starting: direct D3D12 backbuffer -> OpenVR mono path");
    if (!ensure_minhook_initialized()) {
        return 0;
    }
    install_execute_command_lists_hook();
    install_present_hook();
    return 0;
}

struct RendererBootstrap {
    RendererBootstrap() {
        HANDLE thread = CreateThread(nullptr, 0, renderer_thread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    }
};

RendererBootstrap g_rendererBootstrap;
} // namespace

namespace fh2vr {

bool submit_direct_d3d12_mono_preview(vr::IVRCompositor *compositor, std::string &error) {
    if (!compositor) {
        error = "OpenVR compositor is unavailable";
        return false;
    }

    ID3D12CommandQueue *queue = g_primaryQueue.load(std::memory_order_acquire);
    const int latest = g_latestCopiedEye.load(std::memory_order_acquire);
    if (!queue || latest < 0 || latest > 1) {
        error = "D3D12 renderer frame is not ready yet";
        return false;
    }

    ID3D12Resource *texture = nullptr;
    D3D12_RESOURCE_DESC desc{};
    AcquireSRWLockShared(&g_resourceLock);
    if (g_copyResourcesReady && g_eyeCopies[latest].hasFrame && g_eyeCopies[latest].texture) {
        texture = g_eyeCopies[latest].texture;
        texture->AddRef();
        desc = g_retainedDesc;
        queue->AddRef();
    }
    ReleaseSRWLockShared(&g_resourceLock);

    if (!texture) {
        error = "latest retained D3D12 frame is unavailable";
        return false;
    }

    vr::TrackedDevicePose_t renderPose{};
    const vr::EVRCompositorError poseResult = compositor->GetLastPoseForTrackedDeviceIndex(
        vr::k_unTrackedDeviceIndex_Hmd, &renderPose, nullptr);
    if (poseResult != vr::VRCompositorError_None || !renderPose.bPoseIsValid) {
        texture->Release();
        queue->Release();
        error = compositor_error("GetLastPoseForTrackedDeviceIndex", poseResult);
        return false;
    }

    vr::D3D12TextureData_t d3d12Texture{texture, queue, 0};
    vr::VRTextureWithPose_t submitted{};
    submitted.handle = &d3d12Texture;
    submitted.eType = vr::TextureType_DirectX12;
    submitted.eColorSpace = vr::ColorSpace_Gamma;
    submitted.mDeviceToAbsoluteTracking = renderPose.mDeviceToAbsoluteTracking;

    const vr::VRTextureBounds_t bounds = mono_aspect_bounds(desc);
    const vr::EVRCompositorError leftResult = compositor->Submit(
        vr::Eye_Left, &submitted, &bounds, vr::Submit_TextureWithPose);
    if (leftResult != vr::VRCompositorError_None) {
        texture->Release();
        queue->Release();
        error = compositor_error("DirectX12 Submit(left)", leftResult);
        return false;
    }

    const vr::EVRCompositorError rightResult = compositor->Submit(
        vr::Eye_Right, &submitted, &bounds, vr::Submit_TextureWithPose);
    if (rightResult != vr::VRCompositorError_None) {
        texture->Release();
        queue->Release();
        error = compositor_error("DirectX12 Submit(right)", rightResult);
        return false;
    }

    compositor->PostPresentHandoff();
    g_openVrPairSubmits.fetch_add(1, std::memory_order_relaxed);

    static std::atomic<bool> loggedSuccess{false};
    if (!loggedSuccess.exchange(true, std::memory_order_acq_rel)) {
        char buffer[512]{};
        std::snprintf(buffer, sizeof(buffer),
                      "OpenVR DirectX12 mono submit OK: resource=%p queue=%p format=%s(%d) bounds=[%.4f %.4f %.4f %.4f] pose=Submit_TextureWithPose",
                      static_cast<void *>(texture), static_cast<void *>(queue),
                      format_name(desc.Format), static_cast<int>(desc.Format),
                      bounds.uMin, bounds.vMin, bounds.uMax, bounds.vMax);
        log_line(buffer);
    }

    texture->Release();
    queue->Release();
    error.clear();
    return true;
}

} // namespace fh2vr
