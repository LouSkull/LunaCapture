#include "App.hpp"
#include "UiGfx.hpp"
#include "WinUtil.hpp"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    const bool startHidden = commandLine && std::wstring(commandLine).find(L"--background") != std::wstring::npos;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\LunaCaptureSingleInstance");
    const bool alreadyRunning = !mutex || GetLastError() == ERROR_ALREADY_EXISTS;
    ui::InitGraphics();
    if (alreadyRunning) {
        if (HWND existing = FindWindowW(kMainWindowClassName, nullptr)) { ShowWindow(existing, SW_SHOWNORMAL); SetForegroundWindow(existing); }
        else ShowNoticeDialog(nullptr, L"Luna Capture", L"Luna Capture is already running in the tray.", false);
        ui::ShutdownGraphics(); if (mutex) CloseHandle(mutex); return 0;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int result = 1;
    { App app(instance); if (app.Initialize(startHidden)) result = app.Run(); }
    if (SUCCEEDED(com)) CoUninitialize();
    ui::ShutdownGraphics();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return result;
}
