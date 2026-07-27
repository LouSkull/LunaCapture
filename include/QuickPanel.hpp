#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

enum class QuickPanelAction {
    None,
    CaptureActive,
    ToggleRecording,
    ToggleReplay,
    SaveAllReplays,
    SaveMonitor0,
    SaveMonitor1,
    SaveMonitor2,
    OpenPhotos,
    OpenVideos,
    PluginCommand,
    Close,
};

// A plugin-defined command surfaced for one-click access from the Quick Capture
// panel (previously only reachable from the main Plugins page).
struct QuickPanelPluginCommand {
    std::wstring title;
    std::wstring description;
    std::wstring icon;
    std::wstring pluginId;
    std::wstring commandId;
    std::size_t pluginIndex = 0;
    std::size_t commandIndex = 0;
};

struct QuickPanelState {
    bool recording = false;
    bool replay = false;
    int replaySeconds = 30;
    std::array<std::wstring, 3> monitorLabels{L"Monitor 1", L"Monitor 2", L"Monitor 3"};
    std::array<bool, 3> monitorAvailable{false, false, false};
    // Number of real monitors represented in monitorLabels/monitorAvailable (0-3). The
    // per-monitor save pills only render when this is at least 2 - see QuickPanel::Paint.
    int monitorCount = 0;
    std::vector<QuickPanelPluginCommand> pluginCommands;
};

class QuickPanel {
public:
    // The std::size_t argument is the index into QuickPanelState::pluginCommands and is
    // only meaningful when the action is QuickPanelAction::PluginCommand.
    using Callback = std::function<void(QuickPanelAction, std::size_t)>;

    QuickPanel() = default;
    ~QuickPanel();

    bool Create(HINSTANCE instance, Callback callback);
    void Destroy();
    void Show(const QuickPanelState& state, const std::wstring& hotkeyLabel);
    void Hide();
    void Toggle(const QuickPanelState& state, const std::wstring& hotkeyLabel);
    void SetState(const QuickPanelState& state);
    // Repaint with the current theme / language pack (call after a plugin theme
    // or locale is activated while the panel is open).
    void Refresh();
    void RefreshTheme();
    bool IsVisible() const;

private:
    struct HitItem { RECT rect{}; QuickPanelAction action = QuickPanelAction::None; std::size_t dataIndex = 0; };
    struct Selection { QuickPanelAction action = QuickPanelAction::None; std::size_t dataIndex = 0; };
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void Paint();
    void Tick();
    void ReassertTopMost();
    bool RecreateFonts();
    void UpdateVisualTimer();
    bool EnsureBackBuffer(HDC reference, int width, int height);
    void ReleaseBackBuffer();
    const HitItem* HitTest(POINT point) const;
    int PanelHeight() const;
    void ApplyWindowShape(int width, int height);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HDC memDc_ = nullptr;
    HBITMAP memBitmap_ = nullptr;
    HGDIOBJ memOld_ = nullptr;
    int memW_ = 0;
    int memH_ = 0;
    Callback callback_;
    QuickPanelState state_;
    std::wstring hotkeyLabel_ = L"Alt + X";
    std::array<HitItem, 13> hitItems_{};
    Selection hovered_{};
    Selection pressed_{};
    HFONT headlineFont_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    ULONGLONG animationStarted_ = 0;
    int animationDurationMs_ = 145;
    int targetX_ = 20;
    int targetY_ = 100;
    // Where the entrance slide begins. Always inside monitorRect_: an off-monitor
    // start position let the panel - and the compositor shadow around it - paint
    // onto the neighbouring display for the length of the animation.
    int slideStartX_ = 20;
    RECT monitorRect_{};
    int currentHeight_ = 0;
    bool slideAnimationActive_ = false;
    // Set by Show() when it minimized a true-exclusive-fullscreen game to make room for the
    // panel; Hide() restores and refocuses it so the player lands back in the game automatically.
    HWND minimizedGameWindow_ = nullptr;
};
