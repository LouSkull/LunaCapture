#include "RegionEditor.hpp"
#include "CaptureEngine.hpp"
#include "UiGfx.hpp"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <iostream>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

using Microsoft::WRL::ComPtr;
namespace {
COLORREF backgroundColor = RGB(30, 170, 70);
constexpr COLORREF movingColor = RGB(190, 60, 95);
std::shared_ptr<FrameBuffer> capturedFrame;
std::wstring captureError;
LRESULT CALLBACK BackgroundProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_APP_ENGINE_EVENT) {
        const std::unique_ptr<EngineEvent> event(reinterpret_cast<EngineEvent*>(l));
        if (event->type == EngineEventType::RegionFrame) capturedFrame = event->frame;
        if (event->type == EngineEventType::Error) captureError = event->detail;
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(window, &ps);
        RECT r{}; GetClientRect(window, &r);
        HBRUSH brush = CreateSolidBrush(GetWindowLongPtrW(window, GWLP_USERDATA) ? movingColor : backgroundColor);
        FillRect(dc, &r, brush); DeleteObject(brush); EndPaint(window, &ps);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
void Pump() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_APP_REGION_COMPLETE) delete reinterpret_cast<RegionResult*>(msg.lParam);
        else { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
}
}

// Opt-in integration probe. It paints only synthetic test colors, samples a
// pixel within its own full-screen surface, and never saves screen contents.
int RunLiveDesktopProbe(bool useWgc) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const HWND previousForeground = GetForegroundWindow();
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 77;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC description{};
    bool found = false;
    for (UINT a = 0; !found && factory->EnumAdapters1(a, &adapter) == S_OK; ++a) {
        for (UINT o = 0; adapter->EnumOutputs(o, &output) == S_OK; ++o) {
            output->GetDesc(&description);
            if (description.AttachedToDesktop && description.Monitor == MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY)) { found = true; break; }
            output.Reset();
        }
        if (!found) adapter.Reset();
    }
    if (!found) return 77;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                  nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 77;
    ComPtr<IDXGIOutput1> output1; output.As(&output1);
    ComPtr<IDXGIOutputDuplication> duplication;
    const HRESULT duplicateResult = output1->DuplicateOutput(device.Get(), &duplication);
    if (FAILED(duplicateResult)) { std::cerr << "Desktop duplication unavailable: " << std::hex << duplicateResult << '\n'; return 77; }
    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width = stagingDesc.Height = stagingDesc.MipLevels = stagingDesc.ArraySize = stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) return 77;
    WNDCLASSW klass{}; klass.hInstance = instance; klass.lpfnWndProc = BackgroundProc; klass.lpszClassName = L"LunaLiveDesktopProbe";
    RegisterClassW(&klass);
    const RECT bounds = description.DesktopCoordinates;
    const HWND background = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, klass.lpszClassName,
        L"Luna synthetic capture test", WS_POPUP, bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, instance, nullptr);
    if (!background) return 77;
    ShowWindow(background, SW_SHOWNOACTIVATE); UpdateWindow(background); DwmFlush();
    using namespace winrt::Windows::Graphics;
    Capture::Direct3D11CaptureFramePool pool{nullptr};
    Capture::GraphicsCaptureSession session{nullptr};
    // Keep WGC opt-in: an active WGC session can change DWM's frame cadence,
    // so the DXGI-only run must also be exercised independently.
    if (useWgc) {
    auto interop = winrt::get_activation_factory<Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    Capture::GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForMonitor(description.Monitor, winrt::guid_of<Capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
    ComPtr<IDXGIDevice> dxgiDevice; device.As(&dxgiDevice);
    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
    pool = Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
        inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>(), winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
    session = pool.CreateCaptureSession(item);
    session.StartCapture();
    }
    COLORREF wgcLast = CLR_INVALID;
    COLORREF last = CLR_INVALID;
    const auto sample = [&](const char* label, COLORREF expected, bool rejectBlack = true) {
        int samples = 0, black = 0, wrong = 0, pointerOnly = 0;
        const ULONGLONG end = GetTickCount64() + 650;
        while (GetTickCount64() < end) {
            Pump();
            DXGI_OUTDUPL_FRAME_INFO info{}; ComPtr<IDXGIResource> resource;
            const HRESULT result = duplication->AcquireNextFrame(30, &info, &resource);
            if (result == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (FAILED(result)) { std::cerr << "Acquire failed: " << std::hex << result << '\n'; return false; }
            if (info.LastPresentTime.QuadPart == 0) {
                ++pointerOnly;
                duplication->ReleaseFrame();
                continue;
            }
            ComPtr<ID3D11Texture2D> texture; resource.As(&texture);
            const D3D11_BOX box{32, 32, 0, 33, 33, 1};
            context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture.Get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE map{};
            if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                const auto* pixel = static_cast<const unsigned char*>(map.pData);
                last = RGB(pixel[2], pixel[1], pixel[0]);
                ++samples;
                if (last == RGB(0, 0, 0)) {
                    ++black;
                    std::cout << "  black frame: present=" << info.LastPresentTime.QuadPart
                              << " accumulated=" << info.AccumulatedFrames
                              << " protected=" << info.ProtectedContentMaskedOut << '\n';
                }
                if (last != expected) ++wrong;
                context->Unmap(staging.Get(), 0);
            }
            duplication->ReleaseFrame();
        }
        std::cout << label << ": frames=" << samples << " pointer-only=" << pointerOnly << " black=" << black << " other=" << wrong
                  << " final=" << std::hex << last << " expected=" << expected << std::dec << '\n';
        HDC screen = GetDC(nullptr);
        HDC copy = CreateCompatibleDC(screen);
        HBITMAP pixelBitmap = CreateCompatibleBitmap(screen, 1, 1);
        HGDIOBJ previousBitmap = SelectObject(copy, pixelBitmap);
        for (DWORD operation : {static_cast<DWORD>(SRCCOPY), static_cast<DWORD>(SRCCOPY | CAPTUREBLT)}) {
            BitBlt(copy, 0, 0, 1, 1, screen, bounds.left + 32, bounds.top + 32, operation);
            std::cout << "  GDI " << std::hex << operation << " pixel=" << GetPixel(copy, 0, 0) << std::dec << '\n';
        }
        SelectObject(copy, previousBitmap); DeleteObject(pixelBitmap); DeleteDC(copy); ReleaseDC(nullptr, screen);
        if (!useWgc) return last == expected && (!rejectBlack || black == 0);
        int wgcFrames = 0, wgcBlack = 0;
        const auto wgcEnd = GetTickCount64() + 350;
        while (GetTickCount64() < wgcEnd) {
            Pump();
            auto frame = pool.TryGetNextFrame();
            if (!frame) { Sleep(5); continue; }
            auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
            const D3D11_BOX box{32, 32, 0, 33, 33, 1};
            context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture.Get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE map{};
            winrt::check_hresult(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map));
            const auto* pixel = static_cast<const unsigned char*>(map.pData);
            wgcLast = RGB(pixel[2], pixel[1], pixel[0]);
            ++wgcFrames;
            if (wgcLast == RGB(0, 0, 0)) ++wgcBlack;
            context->Unmap(staging.Get(), 0);
            frame.Close();
        }
        std::cout << "  WGC frames=" << wgcFrames << " black=" << wgcBlack << " final=" << std::hex << wgcLast << std::dec << '\n';
        return last == expected && wgcLast == expected && (!rejectBlack || (black == 0 && wgcBlack == 0));
    };
    bool passed = sample("Before editor", backgroundColor, false);
    // DXGI permits only one duplication of an output per process. Release the
    // observer while exercising Luna's engine, then reconnect it for the demo.
    duplication.Reset();
    AppSettings settings; settings.privateCapture = true; settings.showRegionGuide = false;
    const auto framePixel = [&](const FrameBuffer& frame) {
        const size_t x = static_cast<size_t>(bounds.left + 32 - frame.virtualRect.left);
        const size_t y = static_cast<size_t>(bounds.top + 32 - frame.virtualRect.top);
        if (x >= frame.width || y >= frame.height) return static_cast<COLORREF>(CLR_INVALID);
        const auto* p = frame.bgra.data() + y * frame.stride + x * 4;
        return RGB(p[2], p[1], p[0]);
    };
    for (int attempt = 0; attempt < 3; ++attempt) {
        capturedFrame.reset(); captureError.clear();
        CaptureEngine engine(background);
        engine.Start(settings);
        engine.RequestCapture(CaptureMode::Region, nullptr, nullptr, L"Probe", settings);
        const auto deadline = GetTickCount64() + 1800;
        while (!capturedFrame && captureError.empty() && GetTickCount64() < deadline) { Pump(); Sleep(10); }
        engine.Stop(); Pump();
        const COLORREF pixel = capturedFrame ? framePixel(*capturedFrame) : CLR_INVALID;
        std::cout << "Luna cold screenshot " << attempt << ": " << std::hex << pixel << std::dec << '\n';
        if (!captureError.empty()) std::wcerr << captureError << L'\n';
        passed = pixel == backgroundColor && passed;
    }
    if (FAILED(output1->DuplicateOutput(device.Get(), &duplication))) {
        DestroyWindow(background); if (previousForeground) SetForegroundWindow(previousForeground); return 1;
    }
    {
        RegionEditor editor;
        editor.Create(instance, background);
        auto frame = capturedFrame;
        if (!frame) { DestroyWindow(background); if (previousForeground) SetForegroundWindow(previousForeground); return 1; }
        const COLORREF frozenPixel = framePixel(*frame);
        if (!editor.Show(frame, RegionEditor::SessionMode::Region, settings, L"Probe")) passed = false;
        passed = sample("Hidden editor over static desktop", backgroundColor) && passed;
        duplication.Reset();
        if (SUCCEEDED(output1->DuplicateOutput(device.Get(), &duplication))) {
            last = CLR_INVALID;
            passed = sample("Recorder restarted under hidden editor", backgroundColor) && passed;
        } else passed = false;
        backgroundColor = RGB(45, 85, 220);
        RedrawWindow(background, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        DwmFlush();
        passed = sample("Underlying desktop changed", backgroundColor) && passed;
        passed = sample("Underlying desktop stopped", backgroundColor) && passed;
        const HWND moving = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            klass.lpszClassName, L"Luna moving test window", WS_POPUP, bounds.left + 16, bounds.top + 16,
            160, 160, nullptr, nullptr, instance, nullptr);
        SetWindowLongPtrW(moving, GWLP_USERDATA, 1);
        for (int step = 0; step < 4; ++step) {
            const bool coversSample = (step % 2) == 0;
            SetWindowPos(moving, editor.WindowHandle(), bounds.left + (coversSample ? 16 : 256), bounds.top + 16,
                         160, 160, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            UpdateWindow(moving); DwmFlush();
            passed = sample(coversSample ? "Moving window covered sample" : "Moving window exposed background",
                            coversSample ? movingColor : backgroundColor) && passed;
        }
        DestroyWindow(moving); DwmFlush();
        passed = sample("Moving window closed, desktop static", backgroundColor) && passed;
        passed = framePixel(*frame) == frozenPixel && passed;
        std::cout << "Local frozen screenshot remains " << std::hex << framePixel(*frame) << std::dec << '\n';
        editor.Cancel(); Pump();
    }
    if (session) session.Close();
    if (pool) pool.Close();
    DestroyWindow(background); Pump();
    capturedFrame.reset();
    if (previousForeground) SetForegroundWindow(previousForeground);
    return passed ? 0 : 1;
}
