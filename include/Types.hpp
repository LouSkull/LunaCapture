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

// Every codec the Media Foundation sink writer can be asked to produce, paired
// with the container that can actually hold it (see VideoCodecProfile in
// RecordingEngine.cpp). The first three values are the historical order and must
// keep their numbers — settings.ini stores this as a plain integer.
//
// Availability is a property of the machine, not of this list: a codec with no
// encoder MFT installed fails at AddStream and the recorder reports which one to
// pick instead, so listing a codec here never risks a silent bad recording.
enum class VideoCodec {
    H264 = 0,          // MP4  · universal, hardware everywhere
    Hevc = 1,          // MP4  · H.265, ~30% smaller than H.264
    Av1 = 2,           // MP4  · best compression, needs Arc/RTX 40/RX 7000-class hardware
    Vp9 = 3,           // MP4  · royalty-free, vendor encoder MFT required
    H264Fragmented = 4,  // fMP4 · survives a crash mid-recording
    HevcFragmented = 5,  // fMP4 · same, in H.265
    Mpeg4Part2 = 6,    // MP4  · DivX/Xvid-era, plays on very old devices
    H263 = 7,          // 3GP  · legacy mobile
    Wmv3 = 8,          // WMV  · Windows Media Video 9
    Vc1 = 9,           // WMV  · VC-1 Advanced Profile
    MotionJpeg = 10,   // AVI  · every frame a keyframe, huge but trivially editable
    H264Avi = 11,      // AVI  · H.264 for editors that refuse MP4
};
constexpr int kVideoCodecCount = 12;

// Which processor carries the recording pipeline. The capture itself always
// starts on the GPU (Desktop Duplication), but scaling and encoding can be
// pushed either way — on a CPU-limited machine the encoder and the frame
// resize are what steal game frames, and on a GPU-limited one the opposite.
enum class CapturePriority {
    Balanced = 0,  // hardware encoder, GPU resize only when downscaling
    Gpu = 1,       // push everything possible onto the GPU, trim CPU-side work
    Cpu = 2,       // software encoder and CPU resize, leaving the GPU alone
};

// How the encoder spends the bitrate budget. Applied through ICodecAPI, so an
// encoder that does not implement a mode simply keeps its default.
enum class VideoRateControl {
    Cbr = 0,      // constant bitrate — predictable file size, steadiest for streaming
    Vbr = 1,      // variable bitrate — spends more only on complex scenes
    Quality = 2,  // constant quality — bitrate follows the target quality level
};

// Encoder effort. Slower settings look better at the same bitrate but cost more
// of whichever processor is doing the encoding.
enum class VideoEncoderEffort {
    Speed = 0,
    Balanced = 1,
    Quality = 2,
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
    int noiseSuppressionPercent = 0;
    int noiseGateThresholdDb = 50;
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
    bool startHiddenToTray = false;
    // Unix epoch seconds of the last update check, so it runs at most once a day instead of
    // hitting the update server on every launch. 0 means "never checked."
    std::uint64_t lastUpdateCheckEpochSeconds = 0;
    bool copyRegionToClipboard = false;
    int overlayDurationMs = 1200;
    std::wstring notificationSoundPath;
    std::wstring recordingDirectory;
    int recordingFps = 60;
    int recordingBitrateMbps = 16;
    VideoCodec videoCodec = VideoCodec::H264;
    CapturePriority capturePriority = CapturePriority::Balanced;
    VideoRateControl videoRateControl = VideoRateControl::Vbr;
    VideoEncoderEffort videoEncoderEffort = VideoEncoderEffort::Balanced;
    int videoQuality = 70;        // 1-100, used by the constant-quality mode
    int keyframeSeconds = 2;      // 0 = leave the encoder's own GOP length alone
    int recordingHeight = 0;
    int recordingMonitorIndex = 0;
    bool recordingAutoMonitor = true;
    int replaySeconds = 30;
    // Default to the primary monitor only; extra monitors stay off/empty until
    // the user explicitly arms them.
    std::uint32_t replayMonitorMask = 1;
    std::array<HotkeyBinding, 8> replayMonitorHotkeys{};
    // Mirrors whether the replay buffer was running the last time the app had a chance to
    // save settings (start, stop, or exit). Lets a relaunch — especially a silent autostart
    // relaunch — resume the buffer automatically instead of leaving it off until the user
    // notices and re-enables it by hand.
    bool resumeReplayOnLaunch = false;
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
    bool replayPreviewEnabled = true;
    HotkeyBinding activeDisplayHotkey{VK_SNAPSHOT, MOD_NOREPEAT};
    HotkeyBinding allDisplaysHotkey{VK_SNAPSHOT, MOD_CONTROL | MOD_NOREPEAT};
    HotkeyBinding regionHotkey{VK_SNAPSHOT, MOD_SHIFT | MOD_NOREPEAT};
    HotkeyBinding activeWindowHotkey{VK_SNAPSHOT, MOD_ALT | MOD_NOREPEAT};
    HotkeyBinding recordingHotkey{VK_F9, MOD_NOREPEAT};
    HotkeyBinding replaySaveHotkey{VK_F10, MOD_NOREPEAT};
    HotkeyBinding quickPanelHotkey{L'X', MOD_ALT | MOD_NOREPEAT};
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
    // Monotonically identifies one start request on a recording engine.  UI
    // consumers use this to keep a late terminal event from an older session
    // from tearing down a newer session started by an event handler.
    std::uint64_t sessionGeneration = 0;
    bool replaySession = false;
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
constexpr UINT WM_APP_THUMBNAIL_READY = WM_APP + 49;
constexpr UINT WM_APP_MEDIA_STATE_CHANGED = WM_APP + 50;
constexpr UINT WM_APP_UPDATE_CHECK = WM_APP + 51;
// Posted by the media viewer when the user leaves true fullscreen from the video
// surface itself (Esc, F, double-click, or the floating exit button).
constexpr UINT WM_APP_MEDIA_EXIT_FULLSCREEN = WM_APP + 52;
// WM_COPYDATA tag used when a second launch forwards a .luna path to the running
// instance instead of starting a rival copy.
constexpr ULONG_PTR kLunaImportCopyDataId = 0x4C554E41;  // 'LUNA' — import + show UI
// Sent after a plugin was already installed by the opening process: the running
// instance should only refresh its plugin list, not show a second dialog.
constexpr ULONG_PTR kLunaReloadCopyDataId = 0x4C4E5244;  // 'LNRD'
constexpr wchar_t kMainWindowClassName[] = L"LunaCaptureMainWindow";

constexpr UINT LUNASHOT_HOTKEY_ACTIVE = 0x4C51;
constexpr UINT LUNASHOT_HOTKEY_ALL = 0x4C52;
constexpr UINT LUNASHOT_HOTKEY_REGION = 0x4C53;
constexpr UINT LUNASHOT_HOTKEY_WINDOW = 0x4C54;
constexpr UINT LUNASHOT_HOTKEY_RECORD = 0x4C55;
constexpr UINT LUNASHOT_HOTKEY_REPLAY_SAVE = 0x4C56;
constexpr UINT LUNASHOT_HOTKEY_QUICK_PANEL = 0x4C57;
constexpr UINT LUNASHOT_HOTKEY_REPLAY_MONITOR_BASE = 0x4C60;
constexpr std::size_t LUNASHOT_MAX_REPLAY_MONITORS = 8;
constexpr std::size_t LUNASHOT_MAX_ACTIVE_REPLAY_MONITORS = 3;
