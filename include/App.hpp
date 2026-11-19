#pragma once

#include "CaptureEngine.hpp"
#include "BindingRecorder.hpp"
#include "WindowDrag.hpp"
#include "OverlayWindow.hpp"
#include "RegionEditor.hpp"
#include "SettingsStore.hpp"
#include "SoundPlayer.hpp"
#include "TrayMenu.hpp"
#include "UpdateCheck.hpp"

#include <shellapi.h>
#include <memory>
#include <vector>

class App {
public:
    explicit App(HINSTANCE instance);
    ~App();
    bool Initialize(bool startHidden);
    int Run();

private:
    enum class Page { Capture, Binds, Photos, Settings };
    enum class BindingSlot { None, ActiveDisplay, AllDisplays, Region, ActiveWindow };
    enum class HotkeySource { Registered, Hook, RawInput, AppCommand };
    enum class Action {
        None, Capture, Binds, Photos, Settings,
        CaptureActive, CaptureAll, CaptureRegion, CaptureWindow,
        OpenFolder, ChooseFolder, ToggleFormat, ToggleSound, ToggleNotifications,
        ToggleAutostart, ToggleStartHidden, ToggleRegionGuide, TogglePrivateCapture, CheckUpdates,
        BindActive, BindAll, BindRegion, BindWindow, CancelBind, ResetBinds,
        Minimize, Close,
    };
    struct HitTarget { RECT rect{}; Action action = Action::None; };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    bool CreateMainWindow();
    void Paint();
    void AddHit(RECT rect, Action action);
    void Execute(Action action);
    void TriggerCapture(CaptureMode mode);
    void HandleEngineEvent(std::unique_ptr<EngineEvent> event);
    void HandleRegionResult(std::unique_ptr<RegionResult> result);
    void HandleUpdateResult(std::unique_ptr<UpdateCheckResult> result);
    void BeginBinding(BindingSlot slot);
    void CancelBinding();
    static LRESULT CALLBACK BindingKeyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK PrivacyWindowProc(int code, WPARAM wParam, LPARAM lParam);
    bool ApplyCapturePrivacy();
    void CompleteBinding(UINT virtualKey, UINT modifiers);
    void QueueRecordedBinding(UINT virtualKey, UINT modifiers);
    void HandleRawKeyboard(HRAWINPUT handle);
    BindingSlot MatchBinding(UINT virtualKey, UINT modifiers) const;
    void ExecuteBinding(BindingSlot slot, HotkeySource source);
    void ResetBindings();
    HotkeyBinding* Binding(BindingSlot slot);
    const HotkeyBinding* Binding(BindingSlot slot) const;
    static const wchar_t* BindingName(BindingSlot slot);
    bool RegisterHotkeys(BindingSlot required = BindingSlot::None);
    void UnregisterHotkeys();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();
    void ShowMain(Page page = Page::Capture);
    bool SaveSettings();
    void ToggleCaptureSound();
    void PlayCaptureSound();
    void DrawButton(HDC dc, const RECT& rect, const std::wstring& title, const std::wstring& detail,
                    Action action, UiIcon icon);
    void DrawNav(HDC dc, const RECT& rect, const std::wstring& text, Page page, UiIcon icon);


    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW trayIcon_{};
    bool trayAdded_ = false;
    Page page_ = Page::Capture;
    BindingSlot bindingSlot_ = BindingSlot::None;
    HHOOK bindingHook_ = nullptr;
    inline static App* bindingApp_ = nullptr;
    bool rawKeyboardReady_ = false;
    bool bindingPosted_ = false;
    UINT_PTR bindingGeneration_ = 0;
    KeyboardPressState hookKeys_;
    KeyboardPressState rawKeys_;
    UINT suppressedWinKeys_ = 0;
    UINT recordedKey_ = 0;
    ULONGLONG recordedAt_ = 0;
    std::array<ULONGLONG, 4> lastHotkeyTime_{};
    std::array<HotkeySource, 4> lastHotkeySource_{};
    HHOOK privacyHook_ = nullptr;
    inline static App* privacyApp_ = nullptr;
    bool capturePending_ = false;
    WindowDrag windowDrag_;
    std::wstring privacyError_;
    SettingsStore settingsStore_;
    AppSettings settings_;
    std::unique_ptr<CaptureEngine> engine_;
    OverlayWindow overlay_;
    RegionEditor regionEditor_;
    TrayMenu trayMenu_;
    SoundPlayer soundPlayer_;
    std::wstring status_ = L"Starting";
    std::wstring detail_;
    std::wstring lastCapture_;
    std::wstring updateStatus_;
    bool updateChecking_ = false;
    HFONT fontTitle_ = nullptr;
    HFONT fontHeading_ = nullptr;
    HFONT fontBody_ = nullptr;
    HFONT fontSmall_ = nullptr;
    std::vector<HitTarget> hits_;
};
