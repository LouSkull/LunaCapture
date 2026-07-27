#pragma once

#include <Windows.h>

#include <string>

#ifndef APP_VERSION_STRING
// Fallback for building this header standalone; real builds always get the define from CMake.
#define APP_VERSION_STRING L"0.0.0.0"
#endif

// The running build's version. CMake reads the VERSION file at the repo root and passes it in
// as APP_VERSION_STRING - that file is the single place a release bump ever edits.
constexpr const wchar_t* kAppVersion = APP_VERSION_STRING;

enum class UpdateCheckOutcome { UpdateAvailable, UpToDate, Failed };

struct UpdateCheckResult {
    UpdateCheckOutcome outcome = UpdateCheckOutcome::Failed;
    std::wstring latestVersion;
    // True when the user pressed "Check for updates" and expects an answer either way;
    // automatic background checks stay quiet unless an update actually exists.
    bool manual = false;
};

constexpr const wchar_t* kUpdateReleasesUrl = L"https://github.com/LouSkull/LunaCapture/releases";

// Runs entirely on a detached background thread. Always posts `message` to `notifyWindow` with a
// heap-allocated UpdateCheckResult* as lParam (the receiver takes ownership) carrying the outcome:
// a newer release, already up to date, or the check itself failed (network/parse).
void StartUpdateCheck(HWND notifyWindow, UINT message, bool manual = false);
