#include "RendererPreview.h"

#include <MinHook.h>

#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
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

struct FrameSlot {
    ID3D12Resource *source = nullptr;      // Native FH2 format (R10G10B10A2).
    ID3D12Resource *submit = nullptr;      // OpenVR-friendly R8G8B8A8.
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

FrameSlot g_slots[2]{};
ID3D12GraphicsCommandList *g_commandList = nullptr;
ID3D12Fence *g_fence = nullptr;
HANDLE g_fenceEvent = nullptr;
UINT64 g_nextFenceValue = 1;
D3D12_RESOURCE_DESC g_sourceDesc{};
D3D12_RESOURCE_DESC g_submitDesc{};
bool g_resourcesReady = false;
int g_nextSlot = 0;
std::atomic<int> g_latestSlot{-1};
UINT g_lastBackBufferIndex = 0;

ID3D12DescriptorHeap *g_descriptorHeap = nullptr;
ID3D12RootSignature *g_rootSignature = nullptr;
ID3D12PipelineState *g_convertPso = nullptr;
UINT g_descriptorSize = 0;

std::atomic<std::uint64_t> g_gameExecuteCalls{0};
std::atomic<std::uint64_t> g_openVrPairSubmits{0};
std::uint64_t g_presentCountWindow = 0;
std::uint64_t g_convertCountWindow = 0;
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
    OutputDebugStringA(("[FH2VR D3D12 M3] " + message + "\n").c_str());
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
    if (identityA) identityA->Release();
    if (identityB) identityB->Release();
    return same;
}

bool queue_matches_primary_device(ID3D12CommandQueue *queue) {
    ID3D12Device *primaryDevice = g_primaryDevice.load(std::memory_order_acquire);
    if (!queue || !primaryDevice) {
        return false;
    }
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
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
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

void release_resources_unlocked() {
    if (g_fence) {
        const UINT64 maxFence = g_slots[0].fenceValue > g_slots[1].fenceValue
                                    ? g_slots[0].fenceValue
                                    : g_slots[1].fenceValue;
        if (maxFence && g_fence->GetCompletedValue() < maxFence && g_fenceEvent) {
            if (SUCCEEDED(g_fence->SetEventOnCompletion(maxFence, g_fenceEvent))) {
                WaitForSingleObject(g_fenceEvent, 1000);
            }
        }
    }

    if (g_commandList) {
        g_commandList->Release();
        g_commandList = nullptr;
    }
    for (auto &slot : g_slots) {
        if (slot.source) {
            slot.source->Release();
            slot.source = nullptr;
        }
        if (slot.submit) {
            slot.submit->Release();
            slot.submit = nullptr;
        }
        if (slot.allocator) {
            slot.allocator->Release();
            slot.allocator = nullptr;
        }
        slot.fenceValue = 0;
        slot.hasFrame = false;
    }
    if (g_fence) {
        g_fence->Release();
        g_fence = nullptr;
    }
    if (g_fenceEvent) {
        CloseHandle(g_fenceEvent);
        g_fenceEvent = nullptr;
    }
    if (g_descriptorHeap) {
        g_descriptorHeap->Release();
        g_descriptorHeap = nullptr;
    }
    if (g_convertPso) {
        g_convertPso->Release();
        g_convertPso = nullptr;
    }
    if (g_rootSignature) {
        g_rootSignature->Release();
        g_rootSignature = nullptr;
    }

    g_descriptorSize = 0;
    g_latestSlot.store(-1, std::memory_order_release);
    g_resourcesReady = false;
    g_sourceDesc = {};
    g_submitDesc = {};
    g_nextFenceValue = 1;
    g_nextSlot = 0;
}

bool source_desc_matches(const D3D12_RESOURCE_DESC &a, const D3D12_RESOURCE_DESC &b) {
    return a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels &&
           a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality;
}

bool create_conversion_pipeline(ID3D12Device *device) {
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = params;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob *serialized = nullptr;
    ID3DBlob *rootErrors = nullptr;
    const HRESULT serializeResult = D3D12SerializeRootSignature(
        &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &rootErrors);
    if (FAILED(serializeResult) || !serialized) {
        if (rootErrors) {
            log_line(std::string("D3D12SerializeRootSignature failed: ") +
                     static_cast<const char *>(rootErrors->GetBufferPointer()));
            rootErrors->Release();
        } else {
            log_line("D3D12SerializeRootSignature failed");
        }
        if (serialized) serialized->Release();
        return false;
    }
    if (rootErrors) rootErrors->Release();

    const HRESULT rootResult = device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&g_rootSignature));
    serialized->Release();
    if (FAILED(rootResult) || !g_rootSignature) {
        log_line("CreateRootSignature failed for R10->R8 conversion");
        return false;
    }

    static const char shaderSource[] = R"(
Texture2D<float4> SourceTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    OutputTexture.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    OutputTexture[id.xy] = SourceTexture.Load(int3(id.xy, 0));
}
)";

    ID3DBlob *shader = nullptr;
    ID3DBlob *shaderErrors = nullptr;
    const HRESULT compileResult = D3DCompile(
        shaderSource, sizeof(shaderSource) - 1, "FH2VR_R10_to_R8", nullptr, nullptr,
        "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &shaderErrors);
    if (FAILED(compileResult) || !shader) {
        if (shaderErrors) {
            log_line(std::string("D3DCompile R10->R8 failed: ") +
                     static_cast<const char *>(shaderErrors->GetBufferPointer()));
            shaderErrors->Release();
        } else {
            log_line("D3DCompile R10->R8 failed");
        }
        if (shader) shader->Release();
        return false;
    }
    if (shaderErrors) shaderErrors->Release();

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_rootSignature;
    psoDesc.CS.pShaderBytecode = shader->GetBufferPointer();
    psoDesc.CS.BytecodeLength = shader->GetBufferSize();
    const HRESULT psoResult = device->CreateComputePipelineState(
        &psoDesc, IID_PPV_ARGS(&g_convertPso));
    shader->Release();
    if (FAILED(psoResult) || !g_convertPso) {
        log_line("CreateComputePipelineState failed for R10->R8 conversion");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 4;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_descriptorHeap))) ||
        !g_descriptorHeap) {
        log_line("CreateDescriptorHeap failed for R10->R8 conversion");
        return false;
    }
    g_descriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool ensure_resources(ID3D12Resource *backBuffer) {
    ID3D12Device *device = g_primaryDevice.load(std::memory_order_acquire);
    if (!device || !backBuffer) {
        return false;
    }

    const D3D12_RESOURCE_DESC sourceDesc = backBuffer->GetDesc();
    AcquireSRWLockExclusive(&g_resourceLock);
    if (g_resourcesReady && source_desc_matches(sourceDesc, g_sourceDesc)) {
        ReleaseSRWLockExclusive(&g_resourceLock);
        return true;
    }

    if (g_resourcesReady) {
        log_line("Backbuffer description changed; rebuilding D3D12 conversion resources");
        release_resources_unlocked();
    }

    if (!create_conversion_pipeline(device)) {
        release_resources_unlocked();
        ReleaseSRWLockExclusive(&g_resourceLock);
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC submitDesc = sourceDesc;
    submitDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    submitDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    bool ok = true;
    for (int slotIndex = 0; slotIndex < 2 && ok; ++slotIndex) {
        FrameSlot &slot = g_slots[slotIndex];
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &sourceDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&slot.source)))) {
            log_line("CreateCommittedResource failed for native R10 retained texture");
            ok = false;
            break;
        }
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &submitDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&slot.submit)))) {
            log_line("CreateCommittedResource failed for R8 OpenVR submit texture");
            ok = false;
            break;
        }
        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)))) {
            log_line("CreateCommandAllocator failed for D3D12 conversion slot");
            ok = false;
            break;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = sourceDesc.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = submitDesc.Format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            g_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(slotIndex * 2) * g_descriptorSize;
        device->CreateShaderResourceView(slot.source, &srv, cpu);
        cpu.ptr += g_descriptorSize;
        device->CreateUnorderedAccessView(slot.submit, nullptr, &uav, cpu);
    }

    if (ok && (FAILED(device->CreateCommandList(
                   0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_slots[0].allocator,
                   g_convertPso, IID_PPV_ARGS(&g_commandList))) || !g_commandList)) {
        log_line("CreateCommandList failed for D3D12 R10->R8 conversion");
        ok = false;
    }
    if (ok && FAILED(g_commandList->Close())) {
        log_line("Initial Close failed for D3D12 conversion command list");
        ok = false;
    }
    if (ok && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
        log_line("CreateFence failed for D3D12 conversion");
        ok = false;
    }
    if (ok) {
        g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_fenceEvent) {
            log_line("CreateEvent failed for D3D12 conversion fence");
            ok = false;
        }
    }

    if (!ok) {
        release_resources_unlocked();
        ReleaseSRWLockExclusive(&g_resourceLock);
        return false;
    }

    g_sourceDesc = sourceDesc;
    g_submitDesc = submitDesc;
    g_resourcesReady = true;
    g_nextSlot = 0;
    g_latestSlot.store(-1, std::memory_order_release);

    char buffer[512]{};
    std::snprintf(buffer, sizeof(buffer),
                  "D3D12 conversion resources ready: source=%llux%u %s(%d) -> submit=%s(%d)",
                  static_cast<unsigned long long>(sourceDesc.Width), sourceDesc.Height,
                  format_name(sourceDesc.Format), static_cast<int>(sourceDesc.Format),
                  format_name(submitDesc.Format), static_cast<int>(submitDesc.Format));
    log_line(buffer);

    ReleaseSRWLockExclusive(&g_resourceLock);
    return true;
}

bool wait_for_slot_unlocked(FrameSlot &slot) {
    if (!slot.fenceValue || !g_fence) {
        return true;
    }
    if (g_fence->GetCompletedValue() >= slot.fenceValue) {
        return true;
    }
    if (!g_fenceEvent || FAILED(g_fence->SetEventOnCompletion(slot.fenceValue, g_fenceEvent))) {
        log_line("D3D12 conversion wait failed to arm fence event");
        return false;
    }
    const DWORD wait = WaitForSingleObject(g_fenceEvent, 1000);
    if (wait != WAIT_OBJECT_0) {
        log_line("D3D12 conversion wait timed out; skipping busy slot");
        return false;
    }
    return true;
}

bool copy_and_convert_current_backbuffer() {
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

    if (!ensure_resources(backBuffer)) {
        backBuffer->Release();
        return false;
    }

    AcquireSRWLockExclusive(&g_resourceLock);
    FrameSlot &slot = g_slots[g_nextSlot];
    if (!wait_for_slot_unlocked(slot)) {
        ReleaseSRWLockExclusive(&g_resourceLock);
        backBuffer->Release();
        return false;
    }

    if (FAILED(slot.allocator->Reset()) ||
        FAILED(g_commandList->Reset(slot.allocator, g_convertPso))) {
        log_line("D3D12 conversion command list Reset failed");
        ReleaseSRWLockExclusive(&g_resourceLock);
        backBuffer->Release();
        return false;
    }

    if (slot.hasFrame) {
        const D3D12_RESOURCE_BARRIER sourceToCopy = transition_barrier(
            slot.source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
        const D3D12_RESOURCE_BARRIER submitToUav = transition_barrier(
            slot.submit, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_RESOURCE_BARRIER reuseBarriers[2]{sourceToCopy, submitToUav};
        g_commandList->ResourceBarrier(2, reuseBarriers);
    }

    const D3D12_RESOURCE_BARRIER bbToCopy = transition_barrier(
        backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_commandList->ResourceBarrier(1, &bbToCopy);
    g_commandList->CopyResource(slot.source, backBuffer);

    const D3D12_RESOURCE_BARRIER bbToPresent = transition_barrier(
        backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    const D3D12_RESOURCE_BARRIER sourceToCompute = transition_barrier(
        slot.source, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_RESOURCE_BARRIER preCompute[2]{bbToPresent, sourceToCompute};
    g_commandList->ResourceBarrier(2, preCompute);

    ID3D12DescriptorHeap *heaps[] = {g_descriptorHeap};
    g_commandList->SetDescriptorHeaps(1, heaps);
    g_commandList->SetComputeRootSignature(g_rootSignature);
    g_commandList->SetPipelineState(g_convertPso);

    D3D12_GPU_DESCRIPTOR_HANDLE gpu =
        g_descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(g_nextSlot * 2) * g_descriptorSize;
    g_commandList->SetComputeRootDescriptorTable(0, gpu);
    gpu.ptr += g_descriptorSize;
    g_commandList->SetComputeRootDescriptorTable(1, gpu);

    const UINT groupsX = static_cast<UINT>((g_submitDesc.Width + 7) / 8);
    const UINT groupsY = (g_submitDesc.Height + 7) / 8;
    g_commandList->Dispatch(groupsX, groupsY, 1);

    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = slot.submit;
    g_commandList->ResourceBarrier(1, &uavBarrier);

    const D3D12_RESOURCE_BARRIER submitToShader = transition_barrier(
        slot.submit, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_commandList->ResourceBarrier(1, &submitToShader);

    if (FAILED(g_commandList->Close())) {
        log_line("D3D12 conversion command list Close failed");
        ReleaseSRWLockExclusive(&g_resourceLock);
        backBuffer->Release();
        return false;
    }

    ID3D12CommandList *lists[] = {g_commandList};
    g_internalQueueSubmit = true;
    queue->ExecuteCommandLists(1, lists);
    g_internalQueueSubmit = false;

    const UINT64 fenceValue = g_nextFenceValue++;
    if (FAILED(queue->Signal(g_fence, fenceValue))) {
        log_line("D3D12 conversion queue Signal failed");
        ReleaseSRWLockExclusive(&g_resourceLock);
        backBuffer->Release();
        return false;
    }

    const int convertedSlot = g_nextSlot;
    slot.fenceValue = fenceValue;
    slot.hasFrame = true;
    g_lastBackBufferIndex = backBufferIndex;
    g_latestSlot.store(convertedSlot, std::memory_order_release);
    ++g_convertCountWindow;

    static bool loggedA = false;
    static bool loggedB = false;
    bool &logged = convertedSlot == 0 ? loggedA : loggedB;
    if (!logged) {
        char buffer[448]{};
        std::snprintf(buffer, sizeof(buffer),
                      "D3D12 R10->R8 frame OK: slot=%s bbIndex=%u source=%p submit=%p fence=%llu finalFormat=%s",
                      convertedSlot == 0 ? "A" : "B", backBufferIndex,
                      static_cast<void *>(slot.source), static_cast<void *>(slot.submit),
                      static_cast<unsigned long long>(fenceValue),
                      format_name(g_submitDesc.Format));
        log_line(buffer);
        logged = true;
    }

    g_nextSlot ^= 1;
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
    if (!recommendedWidth || !recommendedHeight) {
        return bounds;
    }

    const float sourceAspect = static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
    const float eyeAspect = static_cast<float>(recommendedWidth) /
                            static_cast<float>(recommendedHeight);
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
        log_line("M3: IDXGISwapChain::GetDesc failed");
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
                  "M3 locked game swapchain=%p API=%s backbuffer=%ux%u format=%s(%d) buffers=%u swapEffect=%d swapchain3=%s",
                  static_cast<void *>(swapchain), api.c_str(), width, height,
                  format_name(format), static_cast<int>(format), swapDesc.BufferCount,
                  static_cast<int>(swapDesc.SwapEffect), g_primarySwapchain3 ? "yes" : "no");
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
        copy_and_convert_current_backbuffer();

        const ULONGLONG now = GetTickCount64();
        if (now - g_lastPresentLogAt >= 1000) {
            const ULONGLONG elapsed = now - g_lastPresentLogAt;
            const double fps = elapsed
                                   ? static_cast<double>(g_presentCountWindow) * 1000.0 /
                                         static_cast<double>(elapsed)
                                   : 0.0;
            const std::uint64_t executeCalls =
                g_gameExecuteCalls.exchange(0, std::memory_order_relaxed);
            const std::uint64_t openVrPairs =
                g_openVrPairSubmits.exchange(0, std::memory_order_relaxed);
            char buffer[448]{};
            std::snprintf(buffer, sizeof(buffer),
                          "M3 Present %.2f fps; game ExecuteCommandLists/s=%llu; convertedFrames/s=%llu; OpenVR mono pairs/s=%llu; latestSlot=%d; lastBB=%u; syncInterval=%u flags=0x%X",
                          fps, static_cast<unsigned long long>(executeCalls),
                          static_cast<unsigned long long>(g_convertCountWindow),
                          static_cast<unsigned long long>(openVrPairs),
                          g_latestSlot.load(std::memory_order_relaxed), g_lastBackBufferIndex,
                          syncInterval, flags);
            log_line(buffer);
            g_presentCountWindow = 0;
            g_convertCountWindow = 0;
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
    const wchar_t className[] = L"FH2VR_RendererM3Window";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = dummy_window_proc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    RegisterClassW(&windowClass);

    HWND window = CreateWindowExW(0, className, L"", WS_OVERLAPPEDWINDOW,
                                  0, 0, 64, 64, nullptr, nullptr,
                                  windowClass.hInstance, nullptr);
    if (!window) {
        log_line("M3 Present: failed to create dummy window");
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
        log_line("M3 Present: D3D11 dummy swapchain creation failed");
        return false;
    }

    void **vtable = *reinterpret_cast<void ***>(dummySwapchain);
    void *target = vtable[8];
    const MH_STATUS createStatus = MH_CreateHook(
        target, reinterpret_cast<void *>(&present_hook),
        reinterpret_cast<void **>(&g_originalPresent));
    const MH_STATUS enableStatus = createStatus == MH_OK ? MH_EnableHook(target) : createStatus;

    dummySwapchain->Release();
    context->Release();
    device->Release();
    DestroyWindow(window);

    if (createStatus != MH_OK || enableStatus != MH_OK) {
        log_line("M3 Present hook install failed create=" + std::to_string(createStatus) +
                 " enable=" + std::to_string(enableStatus));
        return false;
    }
    log_line("M3 DXGI Present hook installed");
    return true;
}

bool install_execute_command_lists_hook() {
    ID3D12Device *device = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) ||
        !device) {
        log_line("M3 ExecuteCommandLists: D3D12CreateDevice failed");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;

    ID3D12CommandQueue *queue = nullptr;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))) || !queue) {
        device->Release();
        log_line("M3 ExecuteCommandLists: dummy CreateCommandQueue failed");
        return false;
    }

    void **vtable = *reinterpret_cast<void ***>(queue);
    void *target = vtable[10];
    const MH_STATUS createStatus = MH_CreateHook(
        target, reinterpret_cast<void *>(&execute_command_lists_hook),
        reinterpret_cast<void **>(&g_originalExecuteCommandLists));
    const MH_STATUS enableStatus = createStatus == MH_OK ? MH_EnableHook(target) : createStatus;

    queue->Release();
    device->Release();

    if (createStatus != MH_OK || enableStatus != MH_OK) {
        log_line("M3 ExecuteCommandLists hook install failed create=" +
                 std::to_string(createStatus) + " enable=" + std::to_string(enableStatus));
        return false;
    }
    log_line("M3 ID3D12CommandQueue::ExecuteCommandLists hook installed");
    return true;
}

DWORD WINAPI renderer_thread(void *) {
    Sleep(3000);
    log_line("Renderer M3 starting: D3D12 R10G10B10A2 -> R8G8B8A8 GPU conversion -> OpenVR");
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
        if (thread) CloseHandle(thread);
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
    const int latest = g_latestSlot.load(std::memory_order_acquire);
    if (!queue || latest < 0 || latest > 1) {
        error = "D3D12 converted renderer frame is not ready yet";
        return false;
    }

    ID3D12Resource *texture = nullptr;
    D3D12_RESOURCE_DESC desc{};
    AcquireSRWLockShared(&g_resourceLock);
    if (g_resourcesReady && g_slots[latest].hasFrame && g_slots[latest].submit) {
        texture = g_slots[latest].submit;
        texture->AddRef();
        desc = g_submitDesc;
        queue->AddRef();
    }
    ReleaseSRWLockShared(&g_resourceLock);

    if (!texture) {
        error = "latest converted R8 D3D12 frame is unavailable";
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
        error = compositor_error("DirectX12 R8 Submit(left)", leftResult);
        return false;
    }
    const vr::EVRCompositorError rightResult = compositor->Submit(
        vr::Eye_Right, &submitted, &bounds, vr::Submit_TextureWithPose);
    if (rightResult != vr::VRCompositorError_None) {
        texture->Release();
        queue->Release();
        error = compositor_error("DirectX12 R8 Submit(right)", rightResult);
        return false;
    }

    compositor->PostPresentHandoff();
    g_openVrPairSubmits.fetch_add(1, std::memory_order_relaxed);

    static std::atomic<bool> loggedSuccess{false};
    if (!loggedSuccess.exchange(true, std::memory_order_acq_rel)) {
        char buffer[512]{};
        std::snprintf(buffer, sizeof(buffer),
                      "OpenVR DirectX12 R8 mono submit OK: resource=%p queue=%p format=%s(%d) bounds=[%.4f %.4f %.4f %.4f] pose=Submit_TextureWithPose",
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
