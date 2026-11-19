#pragma once

#include <Windows.h>
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

struct HotkeyBinding {
    UINT vk = 0;
    UINT modifiers = MOD_NOREPEAT;
};

struct AppSettings {
    std::wstring saveDirectory;
    ImageFormat imageFormat = ImageFormat::Png;
    int outputWidth = 0;
    int outputHeight = 0;
    int jpegQuality = 95;
    bool notifications = true;
    bool privateCapture = false;
    bool sound = true;
    bool startWithWindows = false;
    bool startHiddenToTray = false;
    // The editor can be completely clean for experienced users, or show the
    // centered first-use prompt until a region has been selected.
    bool showRegionGuide = true;
    // Unix epoch seconds of the last update check, so it runs at most once a day instead of
    // hitting the update server on every launch. 0 means "never checked."
    std::uint64_t lastUpdateCheckEpochSeconds = 0;
    bool copyRegionToClipboard = false;
    int overlayDurationMs = 1200;
    std::wstring notificationSoundPath;
    HotkeyBinding activeDisplayHotkey{VK_SNAPSHOT, MOD_NOREPEAT};
    HotkeyBinding allDisplaysHotkey{VK_SNAPSHOT, MOD_CONTROL | MOD_NOREPEAT};
    HotkeyBinding regionHotkey{VK_SNAPSHOT, MOD_SHIFT | MOD_NOREPEAT};
    HotkeyBinding activeWindowHotkey{VK_SNAPSHOT, MOD_ALT | MOD_NOREPEAT};
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

struct ClipboardMemoryDeleter {
    void operator()(void* memory) const noexcept { GlobalFree(memory); }
};

using ClipboardMemory = std::unique_ptr<void, ClipboardMemoryDeleter>;

struct ClipboardImage {
    ClipboardMemory png;
    ClipboardMemory dib;
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
    bool copyToClipboard = false;
};

enum class EngineEventType {
    Ready,
    Reconfiguring,
    Feedback,
    Saved,
    RegionFrame,
    ClipboardImageReady,
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
    ClipboardImage clipboardImage;
    std::uint64_t requestId = 0;
};

constexpr UINT WM_APP_ENGINE_EVENT = WM_APP + 41;
constexpr UINT WM_APP_TRAY = WM_APP + 42;
constexpr UINT WM_APP_TRAY_COMMAND = WM_APP + 43;
constexpr UINT WM_APP_REGION_COMPLETE = WM_APP + 44;
constexpr UINT WM_APP_HOTKEY_CAPTURED = WM_APP + 45;
constexpr UINT WM_APP_GLOBAL_HOTKEY_INPUT = WM_APP + 48;
constexpr UINT WM_APP_UPDATE_CHECK = WM_APP + 51;
constexpr wchar_t kMainWindowClassName[] = L"LunaCaptureMainWindow";

constexpr UINT LUNASHOT_HOTKEY_ACTIVE = 0x4C51;
constexpr UINT LUNASHOT_HOTKEY_ALL = 0x4C52;
constexpr UINT LUNASHOT_HOTKEY_REGION = 0x4C53;
constexpr UINT LUNASHOT_HOTKEY_WINDOW = 0x4C54;
