#pragma once

#include "CaptureEngine.hpp"
#include "CoreUtils.hpp"
#include "MediaViewerWindow.hpp"
#include "OverlayWindow.hpp"
#include "PluginSystem.hpp"
#include "QuickPanel.hpp"
#include "RegionEditor.hpp"
#include "RecordingEngine.hpp"
#include "SettingsStore.hpp"
#include "SoundPlayer.hpp"
#include "ThumbnailCache.hpp"
#include "TrayMenu.hpp"
#include "Types.hpp"
#include "UpdateCheck.hpp"
#include "WinUtil.hpp"

#include <shellapi.h>

#include <array>
#include <cstddef>
#include <filesystem>
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
    enum class Page { Home, Capture, Photos, Videos, Record, Replay, Encoder, Audio, Plugins, Settings, Count };
    enum class Action {
        None,
        Hide,
        Exit,
        NavHome,
        NavCapture,
        NavPhotos,
        NavVideos,
        NavRecord,
        NavReplay,
        NavEncoder,
        NavAudio,
        NavPlugins,
        NavSettings,
        PluginsReload,
        PluginsOpenFolder,
        PluginsOpenDocs,
        PluginsBrowse,
        PluginsResetTheme,
        Plugin0Toggle, Plugin1Toggle, Plugin2Toggle,
        Plugin0Manage, Plugin1Manage, Plugin2Manage,
        ToggleStartHiddenToTray,
        PhotosOpenFolder,
        VideosOpenFolder,
        RefreshMediaLibrary,
        OpenMediaItem,
        MediaDetailClose,
        MediaDetailPrev,
        MediaDetailNext,
        MediaDetailPlayPause,
        MediaDetailSkipBack,
        MediaDetailSkipForward,
        MediaDetailMute,
        MediaDetailFullscreen,
        MediaDetailDelete,
        MediaDetailFolder,
        CaptureActive,
        CaptureAll,
        CaptureRegion,
        CaptureWindow,
        OpenFolder,
        ChooseFolder,
        ToggleFormat,
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
        CycleCapturePriority,
        CycleRateControl,
        CycleEncoderEffort,
        CycleVideoQuality,
        CycleKeyframeInterval,
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
        ToggleReplayPreview,
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
        RecordQuickPanelHotkey,
        ToggleActiveHotkey,
        ToggleAllHotkey,
        ToggleRegionHotkey,
        ToggleWindowHotkey,
        ToggleRecordingHotkey,
        ToggleReplayHotkey,
        ToggleQuickPanelHotkey,
        ToggleAutostart,
        CycleOverlayDuration,
        CheckForUpdates,
        OpenUpdatePage,
        ResetDefaults,
        Count,
    };
    struct HitRegion { RECT rect{}; Action action = Action::None; int data = -1; };
    struct MediaEntry {
        std::filesystem::path path;
        std::wstring name;
        std::uintmax_t sizeBytes = 0;
        std::filesystem::file_time_type modified{};
        std::vector<std::uint8_t> thumbnailBgra;
        int thumbnailWidth = 0;
        int thumbnailHeight = 0;
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK GlobalLowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateMainWindow(bool startHidden);
    bool CreateUiResources();
    void RefreshThemeFonts();
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
    void PaintMediaLibrary(HDC dc, bool videos);
    void PaintMediaDetail(HDC dc);
    RECT MediaDetailCanvasRect() const;
    void SetMediaDetailFullscreen(bool enable);
    void PaintRecord(HDC dc);
    void PaintReplay(HDC dc);
    void PaintEncoder(HDC dc);
    void PaintAudio(HDC dc);
    void PaintPlugins(HDC dc);
    void PaintSettings(HDC dc);
    int PageContentHeight(Page page) const;
    int PageMaximumScroll(Page page) const;
    void ScrollCurrentPage(int delta);
    void PaintPageScrollbar(HDC dc);
    void AddRegion(int x, int y, int width, int height, Action action, int data = -1);
    const HitRegion* HitTestRegion(POINT point) const;
    Action HitTestAction(POINT point) const;
    void ExecuteAction(Action action, int data = -1);
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
    void DisableHotkeySlot(int slot);
    void ToggleHotkeySlot(int slot);
    bool StartHotkeyRecorder();
    void StopHotkeyRecorder();
    void QueueRecordedHotkey(UINT vk, UINT modifiers);
    void HandleRawKeyboardInput(HRAWINPUT rawInput);
    void CancelHotkeyRecording();
    void TriggerCapture(CaptureMode mode, int requestedMonitorIndex = -1);
    bool BeginRecorderFrameCapture(CaptureMode mode, int requestedMonitorIndex = -1);
    void HandleRecorderFrame(std::unique_ptr<EngineEvent> event);
    void CompleteRecorderFrameCapture();
    RecordingEngine* RecorderForMonitor(HMONITOR monitor);
    bool GuardSessionSettingChange(const wchar_t* settingName);
    void HandleEngineEvent(std::unique_ptr<EngineEvent> event);
    void HandleRegionResult(std::unique_ptr<RegionResult> result);
    void HandleRecordingEvent(std::unique_ptr<RecordingEvent> event);
    void HandlePreviewFrame(std::unique_ptr<PreviewFrame> frame);
    void HandleUpdateCheckResult(std::unique_ptr<UpdateCheckResult> result);
    void ReleasePreviewFrame();
    void ReleaseReplayPreviewFrames();
    RecordingEngine* ReplayPreviewEngine(int monitorIndex);
    void HandleTrayCommand(TrayCommand command, std::size_t dataIndex = 0);
    void RefreshMediaLibrary(bool videos);
    void OpenMediaItem(bool videos, std::size_t index);
    void OpenMediaDetail(bool videos, std::size_t index);
    void CloseMediaDetail();
    void StepMediaDetail(int delta);
    void DeleteMediaDetailItem();
    bool LoadMediaDetailImage(const std::filesystem::path& path);
    void UpdateMediaDetailVideoBounds();
    void SeekMediaDetailFromPoint(int x);
    void VolumeMediaDetailFromPoint(int x);
    void HandleThumbnailReady(std::unique_ptr<ThumbnailResult> result);
    QuickPanelState BuildQuickPanelState();
    std::vector<TrayPluginCommand> BuildTrayPluginCommands() const;
    void ToggleQuickPanel();
    void HandleQuickPanelAction(QuickPanelAction action, std::size_t dataIndex);
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
    bool FinishActiveRecordingSession(const RecordingEvent& event);
    void LoadNotificationSound();
    AppSettings BuildSessionSettings();
    void RefreshAudioSources();
    void CycleApplicationTarget();
    void AddMicrophoneInput();
    void ExecuteMicrophoneAction(Action action);
    void ApplyPluginTheme();
    void ReloadPlugins(bool automatic = false);
    void TogglePlugin(std::size_t index);
    void ManagePlugin(std::size_t index);
    void BrowsePlugins();
    // Installs a .luna file the user opened from Explorer (file association) or
    // that a second instance forwarded to us.
    void ImportLunaFile(const std::wstring& sourcePath);
    void ExecutePluginExecution(const PluginExecution& execution);
    bool AllowPluginMediaEventAction(const PluginExecution& execution);
    bool ApplyPluginSettingInstruction(const LunaInstruction& instruction);
    bool ApplyPluginAudioInstruction(const LunaInstruction& instruction);
    void DispatchPluginEvent(const std::wstring& eventName);
    void OpenPluginDocumentation();
    void InvalidateUi();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW trayIcon_{};
    bool trayAdded_ = false;
    bool exiting_ = false;
    bool engineReady_ = false;
    bool captureSuspendedForRecording_ = false;
    InteractiveCaptureGate interactiveCaptureGate_;
    bool hotkeysRegistered_ = false;
    bool rawInputReady_ = false;
    int recordingHotkeySlot_ = -1;
    HHOOK globalKeyboardHook_ = nullptr;
    HHOOK hotkeyRecordingHook_ = nullptr;
    bool hotkeyCapturePosted_ = false;
    std::array<bool, 256> rawKeysDown_{};
    std::array<bool, 256> lowLevelKeysDown_{};
    std::array<ULONGLONG, 7 + LUNASHOT_MAX_REPLAY_MONITORS> lastHotkeyTrigger_{};
    static App* globalHotkeyInstance_;
    static App* hotkeyRecordingInstance_;
    bool dragging_ = false;
    POINT dragOffset_{};
    Page page_ = Page::Home;

    SettingsStore settingsStore_;
    AppSettings settings_;
    PluginSystem pluginSystem_;
    std::wstring pluginStatus_ = L"Plugin host is starting";
    std::wstring themeAssetStatus_;
    struct PluginFeedbackGuardState {
        std::wstring pluginId;
        PluginFeedbackCircuitBreaker breaker;
    };
    std::vector<PluginFeedbackGuardState> pluginFeedbackGuards_;
    std::wstring currentPluginEventName_;
    int pluginDispatchDepth_ = 0;
    int pluginExecutionScopeDepth_ = 0;
    int pluginModalDepth_ = 0;
    std::size_t pluginInstructionBudgetRemaining_ = 0;
    ULONGLONG lastPluginScanTick_ = 0;
    ULONGLONG lastPluginTimerTick_ = 0;
    std::vector<HitRegion> hitRegions_;
    Action hoveredAction_ = Action::None;
    Action pressedAction_ = Action::None;
    int hoveredActionData_ = -1;
    int pressedActionData_ = -1;
    std::array<float, static_cast<std::size_t>(Action::Count)> hoverAnimation_{};
    std::array<float, static_cast<std::size_t>(Action::Count)> pressAnimation_{};
    float sidebarIndicatorY_ = 73.0f;
    ULONGLONG lastAnimationTick_ = 0;
    std::array<int, static_cast<std::size_t>(Page::Count)> pageScrollY_{};
    // Written by PaintEncoder, read by PageContentHeight: the encoder rows size
    // themselves from measured text, so the scrollable height is only knowable
    // after a paint. Seeded with the untranslated single-line layout height.
    mutable int encoderContentHeight_ = 700;
    bool paintingPageContent_ = false;
    int activePagePaintScrollY_ = 0;
    RECT pageScrollTrack_{};
    RECT pageScrollThumb_{};
    bool pageScrollDragging_ = false;
    int pageScrollDragOffset_ = 0;

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

    enum class UpdateUiState { Idle, Checking, UpToDate, UpdateAvailable, Failed };
    UpdateUiState updateUiState_ = UpdateUiState::Idle;
    std::wstring updateLatestVersion_;

    OverlayWindow overlay_;
    MediaViewerWindow mediaViewer_;
    QuickPanel quickPanel_;
    RegionEditor regionEditor_;
    TrayMenu trayMenu_;
    SoundPlayer soundPlayer_;
    std::vector<MediaEntry> photoLibrary_;
    std::vector<MediaEntry> videoLibrary_;
    ThumbnailCache thumbnailCache_;

    bool mediaDetailOpen_ = false;
    bool mediaDetailIsVideo_ = false;
    std::size_t mediaDetailIndex_ = 0;
    std::wstring mediaDetailStatus_;
    std::vector<std::uint8_t> mediaDetailImageBgra_;
    UINT mediaDetailImageWidth_ = 0;
    UINT mediaDetailImageHeight_ = 0;
    bool mediaSeekDragging_ = false;
    bool mediaVolumeDragging_ = false;
    RECT mediaProgressRect_{};
    RECT mediaVolumeRect_{};
    bool mediaDetailFullscreen_ = false;
    std::array<int, 3> quickPanelMonitorIndices_{-1, -1, -1};
    std::vector<QuickPanelPluginCommand> quickPanelPluginCommands_;
    std::vector<TrayPluginCommand> trayPluginCommands_;
    std::unique_ptr<CaptureEngine> engine_;
    std::unique_ptr<RecordingEngine> recordingEngine_;
    struct ReplayChannel {
        int monitorIndex = -1;
        std::unique_ptr<RecordingEngine> engine;
    };
    std::vector<ReplayChannel> auxiliaryReplayEngines_;
    int primaryReplayMonitorIndex_ = -1;
    int activeRecordingMonitorIndex_ = -1;
    std::uint64_t activeRecordingSessionGeneration_ = 0;
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
    std::array<std::unique_ptr<PreviewFrame>, LUNASHOT_MAX_REPLAY_MONITORS> replayPreviewFrames_{};
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
    HFONT fontTiny_ = nullptr;
    HFONT fontControl_ = nullptr;
};
