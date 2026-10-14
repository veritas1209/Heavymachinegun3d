#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"HeavyGunnerWindowsPortWindow";
constexpr wchar_t kWindowTitle[] = L"Heavy Gunner 3D - Windows Port Reconstruction";

struct AssetEntry {
    const wchar_t* file;
    const wchar_t* label;
};

constexpr std::array<AssetEntry, 7> kAssets{{
    {L"logo.png", L"Original 480x320 logo screen"},
    {L"loading-en.png", L"Original loading screen atlas"},
    {L"title-ui.png", L"Original title UI texture"},
    {L"title-overlay-en.png", L"Original English title overlay"},
    {L"title-player.png", L"Original title player texture"},
    {L"title-ground.png", L"Original title ground texture"},
    {L"title-sky.png", L"Original title sky texture"},
}};

ComPtr<ID2D1Factory> g_d2dFactory;
ComPtr<IDWriteFactory> g_writeFactory;
ComPtr<IWICImagingFactory> g_wicFactory;
ComPtr<ID2D1HwndRenderTarget> g_renderTarget;
ComPtr<ID2D1Bitmap> g_bitmap;
ComPtr<ID2D1SolidColorBrush> g_textBrush;
ComPtr<IDWriteTextFormat> g_textFormat;
std::size_t g_assetIndex = 0;
std::wstring g_status;

std::filesystem::path executableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

void releaseDeviceResources() {
    g_textBrush.Reset();
    g_bitmap.Reset();
    g_renderTarget.Reset();
}

HRESULT loadCurrentAsset() {
    g_bitmap.Reset();
    if (!g_renderTarget || !g_wicFactory) {
        return E_UNEXPECTED;
    }

    const auto path = executableDirectory() / L"assets" / kAssets[g_assetIndex].file;
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = g_wicFactory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        g_status = L"Could not load: " + path.wstring();
        return hr;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        g_status = L"WIC could not decode the selected asset.";
        return hr;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = g_wicFactory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut);
    }
    if (SUCCEEDED(hr)) {
        hr = g_renderTarget->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &g_bitmap);
    }

    if (SUCCEEDED(hr)) {
        g_status = std::wstring(kAssets[g_assetIndex].label) +
            L"  [Left/Right: browse, Esc: quit]";
    } else {
        g_status = L"Direct2D could not create a bitmap for the selected asset.";
    }
    return hr;
}

HRESULT createDeviceResources(HWND window) {
    if (g_renderTarget) {
        return S_OK;
    }

    RECT client{};
    GetClientRect(window, &client);
    const D2D1_SIZE_U size = D2D1::SizeU(
        std::max<LONG>(1, client.right - client.left),
        std::max<LONG>(1, client.bottom - client.top));

    HRESULT hr = g_d2dFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(window, size),
        &g_renderTarget);
    if (SUCCEEDED(hr)) {
        hr = g_renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(0.82f, 0.9f, 1.0f, 1.0f), &g_textBrush);
    }
    if (SUCCEEDED(hr)) {
        hr = loadCurrentAsset();
    }
    return hr;
}

void render(HWND window) {
    if (FAILED(createDeviceResources(window))) {
        return;
    }

    g_renderTarget->BeginDraw();
    g_renderTarget->Clear(D2D1::ColorF(0x07101A));

    const D2D1_SIZE_F target = g_renderTarget->GetSize();
    if (g_bitmap) {
        const D2D1_SIZE_F source = g_bitmap->GetSize();
        const float availableHeight = std::max(1.0f, target.height - 54.0f);
        const float scale = std::min(target.width / source.width, availableHeight / source.height);
        const float width = source.width * scale;
        const float height = source.height * scale;
        const float left = (target.width - width) * 0.5f;
        const float top = 42.0f + (availableHeight - height) * 0.5f;
        const D2D1_RECT_F destination = D2D1::RectF(left, top, left + width, top + height);
        g_renderTarget->DrawBitmap(
            g_bitmap.Get(), destination, 1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }

    const std::wstring heading = L"HEAVY GUNNER 3D / WINDOWS RECONSTRUCTION  |  asset " +
        std::to_wstring(g_assetIndex + 1) + L" / " + std::to_wstring(kAssets.size());
    g_renderTarget->DrawTextW(
        heading.c_str(), static_cast<UINT32>(heading.size()), g_textFormat.Get(),
        D2D1::RectF(14.0f, 9.0f, target.width - 14.0f, 29.0f), g_textBrush.Get());
    g_renderTarget->DrawTextW(
        g_status.c_str(), static_cast<UINT32>(g_status.size()), g_textFormat.Get(),
        D2D1::RectF(14.0f, target.height - 27.0f, target.width - 14.0f, target.height - 5.0f),
        g_textBrush.Get());

    const HRESULT hr = g_renderTarget->EndDraw();
    if (static_cast<unsigned long>(hr) == D2DERR_RECREATE_TARGET) {
        releaseDeviceResources();
        InvalidateRect(window, nullptr, FALSE);
    }
}

void selectAsset(HWND window, int direction) {
    const int count = static_cast<int>(kAssets.size());
    g_assetIndex = static_cast<std::size_t>(
        (static_cast<int>(g_assetIndex) + direction + count) % count);
    if (g_renderTarget) {
        loadCurrentAsset();
    }
    InvalidateRect(window, nullptr, FALSE);
}

void playEngineSound(HWND window) {
    const auto path = executableDirectory() / L"assets" / L"engine-start.wav";
    if (PlaySoundW(path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
        g_status = L"Playing recovered Engine_Start WAV  [Space: replay, Left/Right: browse]";
    } else {
        g_status = L"Could not play the recovered Engine_Start WAV.";
    }
    InvalidateRect(window, nullptr, FALSE);
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        if (g_renderTarget) {
            g_renderTarget->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(window);
        } else if (wParam == VK_LEFT || wParam == VK_UP || wParam == VK_PRIOR) {
            selectAsset(window, -1);
        } else if (wParam == VK_RIGHT || wParam == VK_DOWN || wParam == VK_NEXT || wParam == VK_RETURN) {
            selectAsset(window, 1);
        } else if (wParam == VK_SPACE) {
            playEngineSound(window);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        render(window);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return 1;
    }

    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2dFactory.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&g_wicFactory));
    }
    if (SUCCEEDED(hr)) {
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &g_writeFactory);
    }
    if (SUCCEEDED(hr)) {
        hr = g_writeFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            14.0f, L"en-us", &g_textFormat);
    }
    if (FAILED(hr)) {
        CoUninitialize();
        return 2;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = windowClass.hIcon;
    if (!RegisterClassExW(&windowClass)) {
        CoUninitialize();
        return 3;
    }

    RECT desired{0, 0, 960, 640};
    AdjustWindowRectEx(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND window = CreateWindowExW(
        0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        desired.right - desired.left, desired.bottom - desired.top,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        CoUninitialize();
        return 4;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    releaseDeviceResources();
    g_textFormat.Reset();
    g_writeFactory.Reset();
    g_wicFactory.Reset();
    g_d2dFactory.Reset();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
