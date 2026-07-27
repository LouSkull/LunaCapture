#pragma once

#include "CaptureEngine.hpp"
#include "OverlayWindow.hpp"
#include "RegionEditor.hpp"
#include "SettingsStore.hpp"
#include "SoundPlayer.hpp"
#include "TrayMenu.hpp"
#include "Types.hpp"

#include <shellapi.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class App {
public:
    explicit App(HINSTANCE instance);
    ~App();

    bool Initialize(bool startHidden);
    int Run();

private:
    enum class Page { Home, Capture, Settings };
    enum class Action {
        None,
        Minimize,
        Hide,
        Exit,
        NavHome,
        NavCapture,
        NavSettings,
        CaptureActive,
        CaptureAll,
        CaptureRegion,
        CaptureWindow,
        OpenFolder,
        ChooseFolder,
        ToggleFormat,
        CycleResolution,
        ToggleNotifications,
        ToggleSound,
        ToggleCopyRegion,
        TestOverlay,
        RecordActiveHotkey,
        RecordAllHotkey,
        RecordRegionHotkey,
        RecordWindowHotkey,
        ToggleAutostart,
        CycleOverlayDuration,
        ResetDefaults,
        Count,
    };
    struct HitRegion { RECT rect{}; Action action = Action::None; };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateMainWindow(bool startHidden);
    bool CreateUiResources();
    void DestroyUiResources();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();
    void ShowMainWindow(Page page = Page::Home);
    void HideMainWindow();
    void ExitApplication();

    void Paint();
    void PaintSidebar(HDC dc);
    void PaintTopBar(HDC dc);
    void PaintHome(HDC dc);
    void PaintCapture(HDC dc);
    void PaintSettings(HDC dc);
    void AddRegion(int x, int y, int width, int height, Action action);
    Action HitTestAction(POINT point) const;
    void ExecuteAction(Action action);
    void NavigateTo(Page page);
    void TickAnimations();
    float HoverAmount(Action action) const;
    float PressAmount(Action action) const;

    void SaveSettings(bool applyAutostart = false);
    bool RegisterAllHotkeys();
    void UnregisterAllHotkeys();
    void BeginHotkeyRecording(int slot);
    void FinishHotkeyRecording(UINT vk, UINT modifiers);
    bool StartHotkeyRecorder();
    void StopHotkeyRecorder();
    void QueueRecordedHotkey(UINT vk, UINT modifiers);
    void HandleRawKeyboardInput(HRAWINPUT rawInput);
    void CancelHotkeyRecording();
    void TriggerCapture(CaptureMode mode);
    void HandleEngineEvent(std::unique_ptr<EngineEvent> event);
    void HandleRegionResult(std::unique_ptr<RegionResult> result);
    void HandleTrayCommand(TrayCommand command);
    void InvalidateUi();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW trayIcon_{};
    bool trayAdded_ = false;
    bool exiting_ = false;
    bool engineReady_ = false;
    bool hotkeysRegistered_ = false;
    int recordingHotkeySlot_ = -1;
    HHOOK hotkeyRecordingHook_ = nullptr;
    bool hotkeyCapturePosted_ = false;
    static App* hotkeyRecordingInstance_;
    bool dragging_ = false;
    POINT dragOffset_{};
    Page page_ = Page::Home;

    SettingsStore settingsStore_;
    AppSettings settings_;
    std::vector<HitRegion> hitRegions_;
    Action hoveredAction_ = Action::None;
    Action pressedAction_ = Action::None;
    std::array<float, static_cast<std::size_t>(Action::Count)> hoverAnimation_{};
    std::array<float, static_cast<std::size_t>(Action::Count)> pressAnimation_{};
    float sidebarIndicatorY_ = 102.0f;
    ULONGLONG lastAnimationTick_ = 0;

    std::wstring engineStatus_ = L"Starting";
    std::wstring engineDetail_ = L"Preparing native DXGI capture";
    std::wstring lastCapturePath_;
    double lastResponseMs_ = 0.0;
    double lastSaveMs_ = 0.0;

    OverlayWindow overlay_;
    RegionEditor regionEditor_;
    TrayMenu trayMenu_;
    SoundPlayer soundPlayer_;
    std::unique_ptr<CaptureEngine> engine_;

    HDC backBufferDc_ = nullptr;
    HBITMAP backBufferBitmap_ = nullptr;
    HGDIOBJ backBufferOldBitmap_ = nullptr;
    void* backBufferPixels_ = nullptr;
    HFONT fontLogo_ = nullptr;
    HFONT fontHeadline_ = nullptr;
    HFONT fontTitle_ = nullptr;
    HFONT fontNormal_ = nullptr;
    HFONT fontSmall_ = nullptr;
    HFONT fontControl_ = nullptr;
};
