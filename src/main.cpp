#include "App.hpp"
#include "UiGfx.hpp"

#include <mmsystem.h>

#include <string>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    timeBeginPeriod(1);

    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\LunaCaptureNativeSingleInstance");
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (singleInstance) CloseHandle(singleInstance);
        timeEndPeriod(1);
        MessageBoxW(nullptr, L"Luna Capture is already running. Check the system tray.", L"Luna Capture",
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ui::InitGraphics();
    const bool startHidden = commandLine && std::wstring(commandLine).find(L"--background") != std::wstring::npos;

    int result = 1;
    {
        App app(instance);
        if (app.Initialize(startHidden)) {
            result = app.Run();
        } else {
            MessageBoxW(nullptr, L"Luna Capture could not initialize its native windows.", L"Luna Capture",
                        MB_OK | MB_ICONERROR);
        }
    }

    ui::ShutdownGraphics();
    if (SUCCEEDED(comResult)) CoUninitialize();
    ReleaseMutex(singleInstance);
    CloseHandle(singleInstance);
    timeEndPeriod(1);
    return result;
}
