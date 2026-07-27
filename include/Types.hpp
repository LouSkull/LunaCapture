#pragma once

#include <Windows.h>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class ImageFormat {
    Png = 0,
    Jpeg = 1,
};

enum class CaptureMode {
    ActiveDisplay = 0,
    AllDisplays = 1,
    Region = 2,
    ActiveWindow = 3,
};

enum class VideoCodec {
    H264 = 0,
    Hevc = 1,
    Av1 = 2,
};

struct HotkeyBinding {
    UINT vk = 0;
    UINT modifiers = MOD_NOREPEAT;
};

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

struct AudioMeterState {
    float peak = 0.0f;
    bool muted = false;
    bool available = false;
};

struct AudioInputSetting {
    std::wstring deviceId;
    std::wstring name;
    int volumePercent = 100;
    bool enabled = true;
};

struct AudioWindowInfo {
    DWORD processId = 0;
    HWND window = nullptr;
    std::wstring title;
    std::wstring executableName;
};

struct AppSettings {
    std::wstring saveDirectory;
    ImageFormat imageFormat = ImageFormat::Png;
    int outputWidth = 0;
    int outputHeight = 0;
    int jpegQuality = 95;
    bool notifications = true;
    bool sound = true;
    bool startWithWindows = false;
    bool copyRegionToClipboard = false;
    int overlayDurationMs = 1200;
    std::wstring notificationSoundPath;
    std::wstring recordingDirectory;
    int recordingFps = 60;
    int recordingBitrateMbps = 16;
    VideoCodec videoCodec = VideoCodec::H264;
    int recordingHeight = 0;
    int recordingMonitorIndex = 0;
    bool recordingAutoMonitor = true;
    int replaySeconds = 30;
    std::uint32_t replayMonitorMask = 0;
    std::array<HotkeyBinding, 8> replayMonitorHotkeys{};
    bool recordSystemAudio = true;
    int systemAudioVolumePercent = 100;
    bool recordApplicationAudio = false;
    int applicationAudioVolumePercent = 100;
    std::wstring recordingApplicationName;
    DWORD recordingProcessId = 0;
    std::vector<AudioInputSetting> microphoneInputs;
    int audioBitrateKbps = 192;
    bool recordCursor = true;
    bool previewEnabled = true;
    int previewFps = 30;
    HotkeyBinding activeDisplayHotkey{VK_SNAPSHOT, MOD_NOREPEAT};
    HotkeyBinding allDisplaysHotkey{VK_SNAPSHOT, MOD_CONTROL | MOD_NOREPEAT};
    HotkeyBinding regionHotkey{VK_SNAPSHOT, MOD_SHIFT | MOD_NOREPEAT};
    HotkeyBinding activeWindowHotkey{VK_SNAPSHOT, MOD_ALT | MOD_NOREPEAT};
    HotkeyBinding recordingHotkey{VK_F9, MOD_NOREPEAT};
    HotkeyBinding replaySaveHotkey{VK_F10, MOD_NOREPEAT};
};

struct OutputInfo {
    int linearIndex = 0;
    int adapterIndex = 0;
    int outputIndex = 0;
    HMONITOR monitor = nullptr;
    RECT desktopRect{};
    std::wstring name;
    std::wstring adapterName;
    UINT width = 0;
    UINT height = 0;
};

struct FrameBuffer {
    std::vector<std::uint8_t> bgra;
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    RECT virtualRect{};
};

enum class RegionResultAction {
    Save,
    Copy,
    SaveAndCopy,
    Cancel,
};

struct RegionResult {
    RegionResultAction action = RegionResultAction::Cancel;
    CaptureMode sourceMode = CaptureMode::Region;
    std::wstring savePath;
    ImageFormat saveFormat = ImageFormat::Png;
    std::wstring filenamePrefix;
    FrameBuffer frame;
};

struct CaptureRequest {
    std::uint64_t id = 0;
    std::int64_t requestedQpc = 0;
    CaptureMode mode = CaptureMode::ActiveDisplay;
    HMONITOR preferredMonitor = nullptr;
    HWND targetWindow = nullptr;
    std::wstring filenamePrefix;
    AppSettings settings;
};

struct SaveJob {
    std::uint64_t id = 0;
    std::wstring path;
    std::vector<std::uint8_t> bgra;
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    ImageFormat format = ImageFormat::Png;
    int outputWidth = 0;
    int outputHeight = 0;
    int jpegQuality = 95;
    double responseMs = 0.0;
};

enum class EngineEventType {
    Ready,
    Reconfiguring,
    Feedback,
    Saved,
    RegionFrame,
    RecorderFrame,
    Error,
};

struct EngineEvent {
    EngineEventType type = EngineEventType::Error;
    CaptureMode mode = CaptureMode::ActiveDisplay;
    std::wstring detail;
    std::wstring path;
    std::wstring filenamePrefix;
    double responseMs = 0.0;
    double saveMs = 0.0;
    HMONITOR monitor = nullptr;
    std::shared_ptr<FrameBuffer> frame;
    std::uint64_t requestId = 0;
    int channelId = 0;
};

enum class RecordingEventType {
    Starting,
    RecordingStarted,
    ReplayStarted,
    Recovering,
    Recovered,
    AudioWarning,
    Stats,
    RecordingStopped,
    ReplayStopped,
    RecordingSaved,
    ReplaySaved,
    Error,
};

struct RecordingEvent {
    RecordingEventType type = RecordingEventType::Error;
    std::wstring detail;
    std::wstring path;
    std::uint64_t elapsedMs = 0;
    std::uint64_t framesWritten = 0;
    std::uint64_t droppedFrames = 0;
    double encodedFps = 0.0;
    int channelId = 0;
};

struct PreviewFrame {
    std::vector<std::uint8_t> bgra;
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    bool liveRecording = false;
    int channelId = 0;
};

constexpr UINT WM_APP_ENGINE_EVENT = WM_APP + 41;
constexpr UINT WM_APP_TRAY = WM_APP + 42;
constexpr UINT WM_APP_TRAY_COMMAND = WM_APP + 43;
constexpr UINT WM_APP_REGION_COMPLETE = WM_APP + 44;
constexpr UINT WM_APP_HOTKEY_CAPTURED = WM_APP + 45;
constexpr UINT WM_APP_RECORDING_EVENT = WM_APP + 46;
constexpr UINT WM_APP_PREVIEW_FRAME = WM_APP + 47;
constexpr UINT WM_APP_GLOBAL_HOTKEY_INPUT = WM_APP + 48;

constexpr UINT LUNASHOT_HOTKEY_ACTIVE = 0x4C51;
constexpr UINT LUNASHOT_HOTKEY_ALL = 0x4C52;
constexpr UINT LUNASHOT_HOTKEY_REGION = 0x4C53;
constexpr UINT LUNASHOT_HOTKEY_WINDOW = 0x4C54;
constexpr UINT LUNASHOT_HOTKEY_RECORD = 0x4C55;
constexpr UINT LUNASHOT_HOTKEY_REPLAY_SAVE = 0x4C56;
constexpr UINT LUNASHOT_HOTKEY_REPLAY_MONITOR_BASE = 0x4C60;
constexpr std::size_t LUNASHOT_MAX_REPLAY_MONITORS = 8;
