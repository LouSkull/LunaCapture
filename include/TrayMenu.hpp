#pragma once

#include "Types.hpp"
#include "UiIcons.hpp"

#include <Windows.h>

#include <cstddef>
#include <string>
#include <vector>

enum class TrayCommand : UINT {
    Open = 1,
    CaptureActive,
    CaptureAll,
    CaptureRegion,
    CaptureWindow,
    OpenFolder,
    ToggleRecording,
    ToggleReplay,
    SaveReplay,
    OpenReplayCenter,
    OpenRecordings,
    ToggleSound,
    ToggleNotifications,
    Exit,
    SaveReplayMonitor0,
    SaveReplayMonitor1,
    SaveReplayMonitor2,
    SaveReplayMonitor3,
    SaveReplayMonitor4,
    SaveReplayMonitor5,
    SaveReplayMonitor6,
    SaveReplayMonitor7,
    PluginCommand,
};

struct TrayPluginCommand {
    std::wstring title;
    std::wstring icon;
    std::wstring pluginId;
    std::wstring commandId;
    std::size_t pluginIndex = 0;
    std::size_t commandIndex = 0;
};

class TrayMenu {
public:
    TrayMenu() = default;
    ~TrayMenu();

    bool Create(HINSTANCE instance, HWND notifyWindow);
    void Destroy();
    void Show(POINT screenPoint, const AppSettings& settings, bool recording, bool replayBuffering,
              std::uint32_t activeReplayMask, const std::vector<TrayPluginCommand>& pluginCommands);
    void Hide();
    void RefreshTheme();

private:
    struct Item {
        TrayCommand command{};
        UiIcon icon = UiIcon::Capture;
        std::wstring label;
        std::wstring shortcut;
        bool separatorBefore = false;
        bool checked = false;
        std::size_t dataIndex = 0;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void Paint();
    bool RecreateFonts();
    void UpdateThemeTimer();
    bool EnsureBackBuffer(int width, int height);
    void ReleaseGraphics();
    int HitTest(POINT point) const;
    RECT ItemRect(int index) const;

    HINSTANCE instance_ = nullptr;
    HWND notifyWindow_ = nullptr;
    HWND hwnd_ = nullptr;
    std::vector<Item> items_;
    int hovered_ = -1;
    int pressed_ = -1;
    int width_ = 312;
    int rowHeight_ = 34;
    HDC backBufferDc_ = nullptr;
    HBITMAP backBufferBitmap_ = nullptr;
    HGDIOBJ backBufferOldBitmap_ = nullptr;
    void* backBufferPixels_ = nullptr;
    int backBufferWidth_ = 0;
    int backBufferHeight_ = 0;
    HFONT titleFont_ = nullptr;
    HFONT normalFont_ = nullptr;
    HFONT smallFont_ = nullptr;
};
