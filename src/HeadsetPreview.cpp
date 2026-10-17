#include "HeadsetPreview.h"

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>

namespace fh2vr {
namespace {
using Microsoft::WRL::ComPtr;

constexpr ULONGLONG kCaptureIntervalMs = 16; // ~60 FPS max for the temporary GDI path.

struct PreviewState {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::array<ComPtr<ID3D11Texture2D>, 2> eyeTextures;
    std::array<bool, 2> eyeHasFrame{false, false};
    HDC memoryDc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    void *pixels = nullptr;
    HWND window = nullptr;
    int width = 0;
    int height = 0;
    ULONGLONG lastCaptureAt = 0;
};

PreviewState g_preview;

std::string hresult_error(const char *operation, HRESULT result) {
    return std::string(operation) + " failed (HRESULT " +
           std::to_string(static_cast<long long>(result)) + ")";
}

BOOL CALLBACK enum_process_window(HWND window, LPARAM parameter) {
    auto *best = reinterpret_cast<std::pair<HWND, std::int64_t> *>(parameter);
    if (!best || !IsWindowVisible(window) || IsIconic(window) || GetWindow(window, GW_OWNER)) {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId()) {
        return TRUE;
    }

    RECT client{};
    if (!GetClientRect(window, &client)) {
        return TRUE;
    }

    const auto width = static_cast<std::int64_t>(client.right - client.left);
    const auto height = static_cast<std::int64_t>(client.bottom - client.top);
    if (width < 320 || height < 200) {
        return TRUE;
    }

    const std::int64_t area = width * height;
    if (area > best->second) {
        best->first = window;
        best->second = area;
    }
    return TRUE;
}

HWND find_game_window() {
    std::pair<HWND, std::int64_t> best{nullptr, 0};
    EnumWindows(enum_process_window, reinterpret_cast<LPARAM>(&best));
    return best.first;
}

void release_capture_resources() {
    for (auto &texture : g_preview.eyeTextures) {
        texture.Reset();
    }
    g_preview.eyeHasFrame = {false, false};

    if (g_preview.memoryDc && g_preview.previousBitmap) {
        SelectObject(g_preview.memoryDc, g_preview.previousBitmap);
        g_preview.previousBitmap = nullptr;
    }
    if (g_preview.bitmap) {
        DeleteObject(g_preview.bitmap);
        g_preview.bitmap = nullptr;
    }
    if (g_preview.memoryDc) {
        DeleteDC(g_preview.memoryDc);
        g_preview.memoryDc = nullptr;
    }

    g_preview.pixels = nullptr;
    g_preview.width = 0;
    g_preview.height = 0;
    g_preview.lastCaptureAt = 0;
}

bool ensure_device(std::string &error) {
    if (g_preview.device && g_preview.context) {
        return true;
    }

    D3D_FEATURE_LEVEL featureLevel{};
    const HRESULT result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, g_preview.device.GetAddressOf(), &featureLevel,
        g_preview.context.GetAddressOf());
    (void)featureLevel;

    if (FAILED(result)) {
        error = hresult_error("D3D11CreateDevice", result);
        return false;
    }
    return true;
}

bool ensure_capture_resources(int width, int height, std::string &error) {
    if (width <= 0 || height <= 0) {
        error = "game client area has an invalid size";
        return false;
    }
    if (g_preview.eyeTextures[0] && g_preview.eyeTextures[1] && g_preview.bitmap &&
        g_preview.memoryDc && g_preview.width == width && g_preview.height == height) {
        return true;
    }

    release_capture_resources();

    g_preview.memoryDc = CreateCompatibleDC(nullptr);
    if (!g_preview.memoryDc) {
        error = "CreateCompatibleDC failed";
        return false;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height); // top-down BGRA
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    g_preview.bitmap =
        CreateDIBSection(g_preview.memoryDc, &bitmapInfo, DIB_RGB_COLORS, &g_preview.pixels, nullptr, 0);
    if (!g_preview.bitmap || !g_preview.pixels) {
        error = "CreateDIBSection failed";
        release_capture_resources();
        return false;
    }

    g_preview.previousBitmap = SelectObject(g_preview.memoryDc, g_preview.bitmap);
    if (!g_preview.previousBitmap || g_preview.previousBitmap == HGDI_ERROR) {
        error = "SelectObject failed for capture bitmap";
        release_capture_resources();
        return false;
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = static_cast<UINT>(width);
    textureDesc.Height = static_cast<UINT>(height);
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    for (auto &texture : g_preview.eyeTextures) {
        const HRESULT result = g_preview.device->CreateTexture2D(&textureDesc, nullptr,
                                                                  texture.GetAddressOf());
        if (FAILED(result)) {
            error = hresult_error("CreateTexture2D", result);
            release_capture_resources();
            return false;
        }
    }

    g_preview.width = width;
    g_preview.height = height;
    g_preview.lastCaptureAt = 0;
    g_preview.eyeHasFrame = {false, false};
    return true;
}

bool capture_game_client(int eyeIndex, std::string &error) {
    if (eyeIndex < 0 || eyeIndex > 1) {
        error = "invalid eye index";
        return false;
    }

    if (!g_preview.window || !IsWindow(g_preview.window) || !IsWindowVisible(g_preview.window) ||
        IsIconic(g_preview.window)) {
        g_preview.window = find_game_window();
    }
    if (!g_preview.window) {
        error = "could not find the FH2 top-level window";
        return false;
    }

    RECT client{};
    if (!GetClientRect(g_preview.window, &client)) {
        error = "GetClientRect failed";
        return false;
    }

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (!ensure_capture_resources(width, height, error)) {
        return false;
    }

    POINT origin{0, 0};
    if (!ClientToScreen(g_preview.window, &origin)) {
        error = "ClientToScreen failed";
        return false;
    }

    HDC desktopDc = GetDC(nullptr);
    if (!desktopDc) {
        error = "GetDC for desktop failed";
        return false;
    }

    const BOOL copied = BitBlt(g_preview.memoryDc, 0, 0, width, height, desktopDc, origin.x, origin.y,
                               SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, desktopDc);
    if (!copied) {
        error = "BitBlt failed while capturing the FH2 client area";
        return false;
    }

    g_preview.context->UpdateSubresource(g_preview.eyeTextures[eyeIndex].Get(), 0, nullptr,
                                         g_preview.pixels, static_cast<UINT>(width * 4), 0);
    g_preview.eyeHasFrame[eyeIndex] = true;
    return true;
}

vr::VRTextureBounds_t aspect_correct_bounds() {
    vr::VRTextureBounds_t bounds{0.0f, 0.0f, 1.0f, 1.0f};
    vr::IVRSystem *system = vr::VRSystem();
    if (!system || g_preview.width <= 0 || g_preview.height <= 0) {
        return bounds;
    }

    std::uint32_t recommendedWidth = 0;
    std::uint32_t recommendedHeight = 0;
    system->GetRecommendedRenderTargetSize(&recommendedWidth, &recommendedHeight);
    if (recommendedWidth == 0 || recommendedHeight == 0) {
        return bounds;
    }

    const float sourceAspect =
        static_cast<float>(g_preview.width) / static_cast<float>(g_preview.height);
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

std::string compositor_error(const char *eye, vr::EVRCompositorError result) {
    return std::string("OpenVR compositor Submit(") + eye + ") failed (error " +
           std::to_string(static_cast<int>(result)) + ")";
}
} // namespace

bool submit_headset_preview(vr::IVRCompositor *compositor, bool alternatingEyes, int sourceEye,
                            bool swapEyes, std::string &error) {
    if (!compositor) {
        error = "OpenVR compositor is unavailable";
        return false;
    }
    if (!ensure_device(error)) {
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    if (now - g_preview.lastCaptureAt >= kCaptureIntervalMs ||
        (!g_preview.eyeHasFrame[0] && !g_preview.eyeHasFrame[1])) {
        const int captureEye = alternatingEyes ? (sourceEye & 1) : 0;
        if (!capture_game_client(captureEye, error)) {
            return false;
        }
        g_preview.lastCaptureAt = now;
    }

    const vr::VRTextureBounds_t bounds = aspect_correct_bounds();

    ID3D11Texture2D *leftTexture = g_preview.eyeTextures[0].Get();
    ID3D11Texture2D *rightTexture = alternatingEyes ? g_preview.eyeTextures[1].Get() : leftTexture;

    if (alternatingEyes) {
        if (!g_preview.eyeHasFrame[0] && g_preview.eyeHasFrame[1]) {
            leftTexture = g_preview.eyeTextures[1].Get();
        }
        if (!g_preview.eyeHasFrame[1] && g_preview.eyeHasFrame[0]) {
            rightTexture = g_preview.eyeTextures[0].Get();
        }
        if (swapEyes) {
            std::swap(leftTexture, rightTexture);
        }
    }

    vr::Texture_t left{leftTexture, vr::TextureType_DirectX, vr::ColorSpace_Gamma};
    vr::Texture_t right{rightTexture, vr::TextureType_DirectX, vr::ColorSpace_Gamma};

    const vr::EVRCompositorError leftResult =
        compositor->Submit(vr::Eye_Left, &left, &bounds, vr::Submit_Default);
    if (leftResult != vr::VRCompositorError_None) {
        error = compositor_error("left", leftResult);
        return false;
    }

    const vr::EVRCompositorError rightResult =
        compositor->Submit(vr::Eye_Right, &right, &bounds, vr::Submit_Default);
    if (rightResult != vr::VRCompositorError_None) {
        error = compositor_error("right", rightResult);
        return false;
    }

    compositor->PostPresentHandoff();
    error.clear();
    return true;
}

void shutdown_headset_preview() {
    release_capture_resources();
    g_preview.context.Reset();
    g_preview.device.Reset();
    g_preview.window = nullptr;
}

} // namespace fh2vr