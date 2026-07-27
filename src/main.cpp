#include "App.hpp"
#include "PluginInstallPrompt.hpp"
#include "UiGfx.hpp"
#include "WinUtil.hpp"

#include <mfapi.h>
#include <mmsystem.h>

#include <filesystem>
#include <string>
#include <system_error>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    timeBeginPeriod(1);

    // A .luna path on the command line means Explorer opened a plugin with us.
    std::wstring lunaImportPath;
    {
        int argumentCount = 0;
        if (LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount)) {
            for (int index = 1; index < argumentCount; ++index) {
                std::wstring value(arguments[index]);
                if (value.size() > 5) {
                    std::wstring tail = value.substr(value.size() - 5);
                    for (wchar_t& ch : tail) ch = static_cast<wchar_t>(towlower(ch));
                    if (tail == L".luna") { lunaImportPath = value; break; }
                }
            }
            LocalFree(arguments);
        }
    }

    // Claim the instance mutex first, but report the failure only after graphics are
    // up: the themed notice below is drawn with GDI+ and the app's own palette.
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\LunaCaptureNativeSingleInstance");
    const bool alreadyRunning = !singleInstance || GetLastError() == ERROR_ALREADY_EXISTS;

    // Opening a .luna always runs its own small installer window: it shows the
    // plugin's permissions and writes the plugin folder itself. Handing the file
    // to a running instance was unreliable — nothing at all happened when Luna
    // was hidden in the tray, and an elevated instance cannot even receive the
    // message from a normal-integrity process.
    if (!lunaImportPath.empty()) {
        ui::InitGraphics();
        const PluginInstallOutcome outcome = ShowPluginInstallPrompt(lunaImportPath);
        if (outcome.installed) {
            // Nudge a running instance to reload now instead of waiting for its
            // hot-reload tick. Best effort: failure here changes nothing.
            if (HWND existing = FindWindowW(kMainWindowClassName, nullptr)) {
                COPYDATASTRUCT payload{};
                payload.dwData = kLunaReloadCopyDataId;
                payload.cbData = sizeof(wchar_t);
                wchar_t marker = L'\0';
                payload.lpData = &marker;
                SendMessageW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&payload));
            }
            ShowNoticeDialog(nullptr, L"Plugin installed",
                             (outcome.pluginName.empty() ? outcome.fileName : outcome.pluginName) +
                                 L"\n\nEvery new plugin starts disabled. Open Luna Capture → Plugins to "
                                 L"enable it.",
                             false);
        } else if (!outcome.failureReason.empty() && !outcome.cancelled) {
            ShowNoticeDialog(nullptr, L"The plugin was not installed", outcome.failureReason, true);
        }
        ui::ShutdownGraphics();

        // When Luna was not already running, opening a plugin should not also
        // launch the whole app — the user asked to install a plugin.
        if (singleInstance) {
            if (!alreadyRunning) ReleaseMutex(singleInstance);
            CloseHandle(singleInstance);
        }
        timeEndPeriod(1);
        return 0;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const HRESULT mediaFoundationResult = MFStartup(MF_VERSION);
    ui::InitGraphics();

    if (alreadyRunning) {
        ShowNoticeDialog(nullptr, L"Luna Capture is already running",
                         L"Another copy is live in the system tray. Click its tray icon to bring the window back.",
                         false);
        ui::ShutdownGraphics();
        if (SUCCEEDED(mediaFoundationResult)) MFShutdown();
        if (SUCCEEDED(comResult)) CoUninitialize();
        if (singleInstance) CloseHandle(singleInstance);
        timeEndPeriod(1);
        return 0;
    }

    const bool startHidden = commandLine && std::wstring(commandLine).find(L"--background") != std::wstring::npos;

    int result = 1;
    {
        App app(instance);
        if (app.Initialize(startHidden)) {
            result = app.Run();
        } else {
            ShowNoticeDialog(nullptr, L"Luna Capture could not start",
                             L"The native windows could not be created. This usually means the graphics device "
                             L"is unavailable — try signing out and back in, or updating your display driver.",
                             true);
        }
    }

    ui::ShutdownGraphics();
    if (SUCCEEDED(mediaFoundationResult)) MFShutdown();
    if (SUCCEEDED(comResult)) CoUninitialize();
    ReleaseMutex(singleInstance);
    CloseHandle(singleInstance);
    timeEndPeriod(1);
    return result;
}
