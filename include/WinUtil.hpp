#pragma once

#include "Types.hpp"

#include <Windows.h>
#include <cstddef>
#include <string>

std::wstring GetLocalAppDataDirectory();
std::wstring GetDefaultScreenshotDirectory();
std::wstring GetExecutablePath();
bool EnsureDirectoryExists(const std::wstring& path);
void OpenFolderInExplorer(const std::wstring& path);
std::wstring FormatHotkey(UINT modifiers, UINT vk);
bool ChooseFolder(HWND owner, const std::wstring& initialPath, std::wstring& selectedPath);
bool ChooseScreenshotSavePath(HWND owner, const AppSettings& settings, const std::wstring& filenamePrefix, std::wstring& selectedPath, ImageFormat& selectedFormat);
std::wstring FormatResolution(int width, int height);
std::wstring FormatMilliseconds(double value);
HMONITOR GetPriorityMonitor(HWND applicationWindow);
HWND GetPriorityCaptureWindow(HWND applicationWindow);
HWND GetCapturableWindowAtPoint(POINT screenPoint, HWND applicationWindow);
RECT GetWindowCaptureBounds(HWND window);
std::wstring GetWindowCaptureName(HWND window);
RECT GetVirtualDesktopRect();
bool CopyFrameToClipboard(HWND owner, const FrameBuffer& frame);
