#pragma once
#include <Windows.h>
#include <dwmapi.h>
#include <string>

namespace capturePrivacy {
inline bool Supported() {
    using VersionProc = LONG (WINAPI*)(OSVERSIONINFOW*);
    const auto versionProc = reinterpret_cast<VersionProc>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    BOOL composed = FALSE;
    return versionProc && versionProc(&version) == 0 && version.dwBuildNumber >= 19041 &&
           SUCCEEDED(DwmIsCompositionEnabled(&composed)) && composed;
}

// WDA_MONITOR produces black rectangles; require the exact requested value.
inline bool Apply(HWND window, bool enabled, const wchar_t* name, std::wstring& error) {
    if (!window) return true;
    const DWORD requested = enabled ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    DWORD actual = WDA_NONE;
    SetLastError(ERROR_SUCCESS);
    if (SetWindowDisplayAffinity(window, requested) && GetWindowDisplayAffinity(window, &actual) && actual == requested)
        return true;
    const DWORD code = GetLastError();
    error = std::wstring(name) + L": capture exclusion failed (Windows error " + std::to_wstring(code) + L").";
    return false;
}
}
