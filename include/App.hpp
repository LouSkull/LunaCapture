#pragma once

#include "CaptureEngine.hpp"
#include "OverlayWindow.hpp"
#include "RegionEditor.hpp"
#include "RecordingEngine.hpp"
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
    enum class Page { Home, Capture, Record, Replay, Audio, Settings };
    enum class Action {
        None,
        Minimize,
        Hide,
        Exit,
        NavHome,
        NavCapture,
        NavRecord,
        NavReplay,
        NavAudio,
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
        ImportNotificationSound,
        ResetNotificationSound,
        ToggleRecording,
        ToggleReplay,
        SaveReplay,
        OpenRecordingFolder,
        ChooseRecordingFolder,
        CycleRecordingFps,
        CycleVideoCodec,
        CycleRecordingBitrate,
        CycleRecordingResolution,
        CycleRecordingMonitor,
        CycleReplaySeconds,
        ConfigureReplayMonitors,
        ReplayProfileAuto,
        ReplayProfilePrimary,
        ReplayProfileAll,
        ReplayMonitor0Toggle, ReplayMonitor1Toggle, ReplayMonitor2Toggle, ReplayMonitor3Toggle,
        ReplayMonitor4Toggle, ReplayMonitor5Toggle, ReplayMonitor6Toggle, ReplayMonitor7Toggle,
        ReplayMonitor0Save, ReplayMonitor1Save, ReplayMonitor2Save, ReplayMonitor3Save,
        ReplayMonitor4Save, ReplayMonitor5Save, ReplayMonitor6Save, ReplayMonitor7Save,
        ReplayMonitor0Bind, ReplayMonitor1Bind, ReplayMonitor2Bind, ReplayMonitor3Bind,
        ReplayMonitor4Bind, ReplayMonitor5Bind, ReplayMonitor6Bind, ReplayMonitor7Bind,
        RecordReplayAllHotkey,
        ToggleRecordAudio,
        ToggleRecordCursor,
        TogglePreview,
        CyclePreviewFps,
        ToggleSystemAudio,
        CycleSystemVolume,
        ToggleApplicationAudio,
        CycleApplicationVolume,
        CycleApplicationTarget,
        RefreshAudioSources,
        AddMicrophone,
        CycleAudioBitrate,
        Mic0Toggle, Mic0Device, Mic0Volume, Mic0Remove,
        Mic1Toggle, Mic1Device, Mic1Volume, Mic1Remove,
        Mic2Toggle, Mic2Device, Mic2Volume, Mic2Remove,
        Mic3Toggle, Mic3Device, Mic3Volume, Mic3Remove,
        RecordRecordingHotkey,
        RecordReplayHotkey,
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
    static LRESULT CALLBACK GlobalLowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateMainWindow(bool startHidden);
    bool CreateUiResources();
    void DestroyUiResources();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowSystemNotification(const std::wstring& title, const std::wstring& detail, bool error = false);
    void ShowTrayMenu();
    void ShowMainWindow(Page page = Page::Home);
    void HideMainWindow();
    void ExitApplication();

    void Paint();
    void PaintSidebar(HDC dc);
    void PaintTopBar(HDC dc);
    void PaintHome(HDC dc);
    void PaintCapture(HDC dc);
    void PaintRecord(HDC dc);
    void PaintReplay(HDC dc);
    void PaintAudio(HDC dc);
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
    bool StartGlobalKeyboardHook();
    void StopGlobalKeyboardHook();
    bool DispatchGlobalHotkey(UINT vk, UINT modifiers, const wchar_t* source);
    void ExecuteGlobalHotkey(int slot, const wchar_t* source);
    void BeginHotkeyRecording(int slot);
    void FinishHotkeyRecording(UINT vk, UINT modifiers);
    bool StartHotkeyRecorder();
    void StopHotkeyRecorder();
    void QueueRecordedHotkey(UINT vk, UINT modifiers);
    void HandleRawKeyboardInput(HRAWINPUT rawInput);
    void CancelHotkeyRecording();
    void TriggerCapture(CaptureMode mode);
    void BeginRecorderFrameCapture(CaptureMode mode);
    void HandleRecorderFrame(std::unique_ptr<EngineEvent> event);
    void CompleteRecorderFrameCapture();
    RecordingEngine* RecorderForMonitor(HMONITOR monitor);
    bool GuardSessionSettingChange(const wchar_t* settingName);
    void HandleEngineEvent(std::unique_ptr<EngineEvent> event);
    void HandleRegionResult(std::unique_ptr<RegionResult> result);
    void HandleRecordingEvent(std::unique_ptr<RecordingEvent> event);
    void HandlePreviewFrame(std::unique_ptr<PreviewFrame> frame);
    void ReleasePreviewFrame();
    void HandleTrayCommand(TrayCommand command);
    void ToggleRecording();
    void ToggleReplayBuffer();
    bool IsReplayBuffering() const;
    void SaveAllReplays();
    void SaveReplayForMonitor(int monitorIndex);
    void StopAuxiliaryReplayEngines();
    void OpenReplayControlCenter();
    void SaveReplayFromChooser();
    void ConfigureReplaySources();
    void ConfigureReplayMonitors();
    void ConfigureReplayHotkey();
    void SetReplaySourceProfile(int profile);
    void ToggleReplayMonitorSource(int monitorIndex);
    void SuspendCaptureForRecording();
    void ResumeCaptureAfterRecording();
    void LoadNotificationSound();
    AppSettings BuildSessionSettings();
    void RefreshAudioSources();
    void CycleApplicationTarget();
    void AddMicrophoneInput();
    void ExecuteMicrophoneAction(Action action);
    void InvalidateUi();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW trayIcon_{};
    bool trayAdded_ = false;
    bool exiting_ = false;
    bool engineReady_ = false;
    bool captureSuspendedForRecording_ = false;
    bool hotkeysRegistered_ = false;
    bool rawInputReady_ = false;
    int recordingHotkeySlot_ = -1;
    HHOOK globalKeyboardHook_ = nullptr;
    HHOOK hotkeyRecordingHook_ = nullptr;
    bool hotkeyCapturePosted_ = false;
    std::array<bool, 256> rawKeysDown_{};
    std::array<bool, 256> lowLevelKeysDown_{};
    std::array<ULONGLONG, 6 + LUNASHOT_MAX_REPLAY_MONITORS> lastHotkeyTrigger_{};
    static App* globalHotkeyInstance_;
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
    std::wstring recordingStatus_ = L"Ready to record";
    std::wstring recordingDetail_ = L"Hardware encoder · system audio · instant replay";
    std::wstring audioWarning_;
    std::wstring lastRecordingPath_;
    std::uint64_t recordingElapsedMs_ = 0;
    std::uint64_t recordingFrames_ = 0;
    std::uint64_t recordingDroppedFrames_ = 0;
    double recordingEncodedFps_ = 0.0;

    OverlayWindow overlay_;
    RegionEditor regionEditor_;
    TrayMenu trayMenu_;
    SoundPlayer soundPlayer_;
    std::unique_ptr<CaptureEngine> engine_;
    std::unique_ptr<RecordingEngine> recordingEngine_;
    struct ReplayChannel {
        int monitorIndex = -1;
        std::unique_ptr<RecordingEngine> engine;
    };
    std::vector<ReplayChannel> auxiliaryReplayEngines_;
    int primaryReplayMonitorIndex_ = -1;
    int activeRecordingMonitorIndex_ = -1;
    struct RecorderCaptureBatch {
        std::uint64_t requestId = 0;
        CaptureMode mode = CaptureMode::ActiveDisplay;
        std::size_t pendingFrames = 0;
        std::vector<std::shared_ptr<FrameBuffer>> frames;
    };
    std::unique_ptr<RecorderCaptureBatch> recorderCaptureBatch_;
    std::uint64_t nextRecorderCaptureRequestId_ = 1;
    std::vector<AudioDeviceInfo> microphones_;
    std::vector<AudioWindowInfo> audioWindows_;
    std::vector<OutputInfo> recordingOutputs_;
    std::array<float, 4> microphonePeaks_{};
    std::array<bool, 4> microphoneMuted_{};
    ULONGLONG lastMicrophoneMeterTick_ = 0;
    std::unique_ptr<PreviewFrame> previewFrame_;
    ULONGLONG lastPreviewRequest_ = 0;

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
