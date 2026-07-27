#pragma once

#include "Types.hpp"

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct IntegerPreset {
    int value = 0;
    std::wstring label;
};

std::wstring GetLocalAppDataDirectory();
std::wstring GetDefaultScreenshotDirectory();
std::wstring GetDefaultRecordingDirectory();
std::wstring GetSoundLibraryDirectory();
std::wstring GetExecutablePath();
bool EnsureDirectoryExists(const std::wstring& path);
// Pins an output folder to Explorer's generic template so it sorts by name.
// Capture filenames lead with a zero-padded timestamp, so name order is date
// order — unlike the inherited Pictures/Videos templates, which sort by
// "Date taken" metadata that screen captures never carry.
void ApplyChronologicalFolderView(const std::wstring& directory);
void OpenFolderInExplorer(const std::wstring& path);
std::wstring FormatHotkey(UINT modifiers, UINT vk);
// Themed replacements for MessageBoxW. The stock message box ignores the app's
// dark chrome and theme entirely, so every confirmation and notice goes through
// these instead.
bool ConfirmAction(HWND owner, const std::wstring& title, const std::wstring& detail,
                   const std::wstring& confirmLabel, bool destructive = true);
void ShowNoticeDialog(HWND owner, const std::wstring& title, const std::wstring& detail, bool error = false);

bool ChooseFolder(HWND owner, const std::wstring& initialPath, std::wstring& selectedPath);
bool ChooseWaveFile(HWND owner, std::wstring& selectedPath);
bool PromptForInteger(HWND owner, const std::wstring& title, const std::wstring& detail,
                      int initialValue, int minimum, int maximum, int& selectedValue);
bool PromptForText(HWND owner, const std::wstring& title, const std::wstring& detail,
                   const std::wstring& initialValue, std::size_t maximumLength,
                   std::wstring& selectedValue);
bool ChooseIntegerPreset(HWND owner, const std::wstring& title, const std::wstring& detail,
                         const std::vector<IntegerPreset>& presets, int initialValue,
                         int minimum, int maximum, int& selectedValue, bool allowCustom = true);
int ChooseNamedItem(HWND owner, const std::vector<std::wstring>& items, int selectedIndex = -1,
                    const std::wstring& title = L"Choose item", const std::wstring& detail = {});
bool ChooseMultipleNamedItems(HWND owner, const std::vector<std::wstring>& items, std::uint32_t initialMask,
                              const std::wstring& title, const std::wstring& detail,
                              std::uint32_t& selectedMask);
bool ImportSoundToLibrary(const std::wstring& sourcePath, std::wstring& importedPath);
bool ChooseScreenshotSavePath(HWND owner, const AppSettings& settings, const std::wstring& filenamePrefix, std::wstring& selectedPath, ImageFormat& selectedFormat);
std::wstring FormatResolution(int width, int height);
std::wstring FormatMilliseconds(double value);
HMONITOR GetPriorityMonitor(HWND applicationWindow);
HWND GetPriorityCaptureWindow(HWND applicationWindow);
HWND GetCapturableWindowAtPoint(POINT screenPoint, HWND applicationWindow);
RECT GetWindowCaptureBounds(HWND window);
std::wstring GetWindowCaptureName(HWND window);
RECT GetVirtualDesktopRect();
std::vector<AudioWindowInfo> EnumerateAudioWindows(HWND applicationWindow);
void AppendDiagnosticLog(const std::wstring& message);
bool CopyFrameToClipboard(HWND owner, const FrameBuffer& frame);
bool IsExclusiveFullscreenAppActive();
