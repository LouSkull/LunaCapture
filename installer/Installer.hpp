#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Luna Capture installer — shared types.
//
// The installer is a standalone executable. It never links the app; it only
// downloads a published release, places it, and registers the OS integration
// points (shortcuts, autostart, and uninstall entry).
// ---------------------------------------------------------------------------

namespace installer {

inline constexpr wchar_t kProductName[] = L"Luna Capture";
inline constexpr wchar_t kExecutableName[] = L"LunaCapture.exe";
// The release asset the installer expects. build_release.bat produces exactly
// this name; anything else is only accepted as a fallback.
inline constexpr wchar_t kPortableArchiveName[] = L"LunaCapture-portable.zip";
inline constexpr wchar_t kUninstallKeyName[] = L"LunaCapture";
// Logon task used when the user wants autostart *and* administrator rights; a
// Startup-folder shortcut cannot do both.
inline constexpr wchar_t kElevatedTaskName[] = L"Luna Capture (elevated autostart)";
inline constexpr wchar_t kRepositoryOwner[] = L"LouSkull";
inline constexpr wchar_t kRepositoryName[] = L"LunaCapture";

// Messages posted from the worker thread to the installer window.
inline constexpr UINT WM_APP_INSTALL_PROGRESS = WM_APP + 1;   // wParam = percent, lParam = wstring*
inline constexpr UINT WM_APP_INSTALL_FINISHED = WM_APP + 2;   // wParam = 1 on success, lParam = wstring*

struct InstallOptions {
    std::wstring installDirectory;
    bool desktopShortcut = true;
    bool startMenuShortcut = true;
    bool startWithWindows = false;
    bool runAsAdmin = false;
};

// Result of probing the machine for everything Luna Capture needs at runtime.
struct DependencyReport {
    bool windowsVersionOk = false;
    bool direct3DOk = false;
    std::wstring direct3DHint;
    std::wstring windowsHint;

    bool AllSatisfied() const noexcept {
        return windowsVersionOk && direct3DOk;
    }
    bool BlocksInstall() const noexcept { return !windowsVersionOk; }
};

struct ReleaseInfo {
    std::wstring version;        // e.g. "1.0.0.1"
    std::wstring downloadUrl;    // direct asset URL
    std::wstring assetName;
    bool isArchive = false;      // .zip that must be expanded
};

// Reads the pinned target version from installer.ini next to the executable.
// Empty result means "whatever the newest published release is".
std::wstring ReadPinnedVersion();

std::wstring DefaultInstallDirectory();
std::wstring InstalledExecutablePath(const std::wstring& installDirectory);

DependencyReport CheckDependencies();

// Queries the GitHub releases API. `pinnedVersion` empty selects the newest
// release; otherwise the matching one is returned. `newerAvailable` reports the
// newest version when it is ahead of the pinned one, so the UI can offer it.
bool ResolveRelease(const std::wstring& pinnedVersion, ReleaseInfo& release,
                    std::wstring& newerAvailable, std::wstring& error);

using ProgressCallback = std::function<void(int percent, const std::wstring& message)>;

// Full install: download, place files, apply every selected option. `warning`
// receives a non-fatal note (e.g. elevated autostart needed rights we did not
// have) that the caller should show alongside the success message.
bool RunInstall(const InstallOptions& options, const ReleaseInfo& release,
                const ProgressCallback& progress, std::wstring& error, std::wstring& warning);

// Full uninstall. `keepUserData` preserves settings and captures.
bool RunUninstall(bool keepUserData, const ProgressCallback& progress, std::wstring& error);

// Where a previous install put itself, or empty when not installed.
std::wstring DetectExistingInstall();
std::wstring DetectInstalledVersion();

}  // namespace installer
