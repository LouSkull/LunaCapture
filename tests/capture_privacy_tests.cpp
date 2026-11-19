#include "CapturePrivacy.hpp"
#include "OverlayWindow.hpp"
#include "TrayMenu.hpp"
#include "RegionEditor.hpp"
#include "UiGfx.hpp"
#include "WindowDrag.hpp"
#include "WinUtil.hpp"
#include <cstring>
#include <iostream>
#include <string>

int RunLiveDesktopProbe(bool useWgc);

int main(int argc, char** argv) {
    if (!capturePrivacy::Supported()) { std::cout << "SKIP: desktop composition / exclusion unavailable\n"; return 77; }
    if (!ui::InitGraphics()) return 1;
    if (argc > 1 && (std::string(argv[1]) == "--live-desktop" || std::string(argv[1]) == "--live-wgc")) {
        const int result = RunLiveDesktopProbe(std::string(argv[1]) == "--live-wgc");
        ui::ShutdownGraphics();
        return result;
    }
    const auto instance = GetModuleHandleW(nullptr);
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Luna privacy test", WS_POPUP,
                                      -20000, -20000, 200, 100, nullptr, nullptr, instance, nullptr);
    int failures = 0;
    const auto expect = [&](bool ok, const char* description) {
        if (!ok) { ++failures; std::cerr << description << " (" << GetLastError() << ")\n"; }
    };
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        SaveJob job;
        job.width = 1;
        job.height = 2;
        job.stride = 4;
        job.bgra = {0, 0, 255, 0, 255, 0, 0, 0};
        auto image = PrepareClipboardImage(job);
        expect(image.png && image.dib, "Clipboard provides both PNG and Bitmap formats");
        if (image.png) {
            const BYTE signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
            const auto* png = static_cast<const BYTE*>(GlobalLock(image.png.get()));
            expect(png && GlobalSize(image.png.get()) >= sizeof(signature) &&
                   std::memcmp(png, signature, sizeof(signature)) == 0, "Clipboard PNG has a valid signature");
            if (png) GlobalUnlock(image.png.get());
        }
        if (image.dib) {
            const auto* dib = static_cast<const BITMAPINFOHEADER*>(GlobalLock(image.dib.get()));
            const BYTE expectedPixels[] = {255, 0, 0, 0, 0, 0, 255, 0};
            expect(dib && dib->biWidth == 1 && dib->biHeight == 2 && dib->biBitCount == 24 &&
                   std::memcmp(dib + 1, expectedPixels, sizeof(expectedPixels)) == 0,
                   "Clipboard Bitmap preserves pixel order and aligns rows");
            if (dib) GlobalUnlock(image.dib.get());
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    BOOL dragPreferenceBefore = FALSE, dragPreferenceAfter = FALSE;
    SystemParametersInfoW(SPI_GETDRAGFULLWINDOWS, 0, &dragPreferenceBefore, 0);
    WindowDrag drag;
    drag.Begin(owner, POINT{-1900, 100});
    expect(drag.Active(), "Drag captures mouse");
    drag.Move(POINT{-1850, 175});
    RECT moved{};
    GetWindowRect(owner, &moved);
    expect(moved.left == -19950 && moved.top == -19925 && moved.right - moved.left == 200, "Window moves before mouse release with negative screen coordinates");
    drag.End(true);
    GetWindowRect(owner, &moved);
    expect(moved.left == -20000 && moved.top == -20000 && GetCapture() != owner, "Escape restores position and releases capture");
    drag.Begin(owner, POINT{0, 0});
    drag.CaptureLost();
    ReleaseCapture();
    drag.Move(POINT{100, 100});
    GetWindowRect(owner, &moved);
    expect(moved.left == -20000 && moved.top == -20000, "Losing capture stops movement");
    SystemParametersInfoW(SPI_GETDRAGFULLWINDOWS, 0, &dragPreferenceAfter, 0);
    expect(dragPreferenceBefore == dragPreferenceAfter, "Drag leaves the Windows setting unchanged");
    {
        OverlayWindow overlay;
        TrayMenu tray;
        RegionEditor editor;
        expect(owner && overlay.Create(instance) && tray.Create(instance, owner) && editor.Create(instance, owner), "Create real application windows");
        std::wstring error;
        for (bool hidden : {false, false, true, true, false, false, true, false}) {
            const DWORD expectedAffinity = hidden ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
            expect(capturePrivacy::Apply(owner, hidden, L"Owner", error), "Apply owner mode");
            expect(overlay.SetCapturePrivacy(hidden, error), "Apply notification mode after painting / switching");
            expect(tray.SetCapturePrivacy(hidden, error), "Apply tray mode after painting / switching");
            for (HWND window : {overlay.Handle(), tray.Handle()}) {
                // Exercise the real renderer, including UpdateLayeredWindow in
                // normal mode, without showing UI or reading the user's screen.
                SendMessageW(window, WM_PAINT, 0, 0);
                DWORD affinity = 0;
                expect(GetWindowDisplayAffinity(window, &affinity) && affinity == expectedAffinity, "Renderer preserves exact capture mode");
                if (hidden) expect((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED) == 0, "Private renderer is redirected by DWM");
            }
            auto frame = std::make_shared<FrameBuffer>();
            frame->width = 120; frame->height = 80; frame->stride = 480;
            frame->virtualRect = {-20000, -20000, -19880, -19920};
            frame->bgra.resize(frame->stride * frame->height, 128);
            AppSettings settings;
            settings.privateCapture = hidden;
            settings.showRegionGuide = false;
            expect(editor.Show(frame, RegionEditor::SessionMode::Region, settings, L"Test"), "Editor opens in requested capture mode");
            DWORD affinity = 0;
            expect(GetWindowDisplayAffinity(editor.WindowHandle(), &affinity) && affinity == expectedAffinity, "Editor resets affinity on every session");
            editor.Cancel();
            MSG message{};
            while (PeekMessageW(&message, owner, WM_APP_REGION_COMPLETE, WM_APP_REGION_COMPLETE, PM_REMOVE))
                delete reinterpret_cast<RegionResult*>(message.lParam);
            if (!error.empty()) { std::wcerr << error << L'\n'; error.clear(); }
        }
    }
    DestroyWindow(owner);
    ui::ShutdownGraphics();
    if (failures) return 1;
    std::cout << "Application privacy renderer round trips passed\n";
    return 0;
}
