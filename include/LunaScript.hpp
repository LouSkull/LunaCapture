#pragma once

#include <Windows.h>

#include <cstddef>
#include <string>
#include <vector>

enum class LunaPermission {
    Theme,
    Notifications,
    Capture,
    Recording,
    Replay,
    Audio,
    Settings,
    Localization,
    OpenFolders,
    Diagnostics,
    Ui,
};

enum class LunaInstructionType {
    Notify,
    CaptureActive,
    CaptureAll,
    CaptureRegion,
    CaptureWindow,
    CaptureMonitor,
    RecordingToggle,
    RecordingStart,
    RecordingStop,
    ReplayToggle,
    ReplayStart,
    ReplayStop,
    ReplaySaveAll,
    ReplaySaveMonitor,
    SettingPreview,
    SettingPreviewFps,
    SettingCursor,
    SettingNotifications,
    SettingSound,
    SettingReplaySeconds,
    SettingRecordingFps,
    SettingVideoBitrate,
    SettingRecordingHeight,
    SettingVideoCodec,
    SettingImageFormat,
    SettingJpegQuality,
    SettingOutputWidth,
    SettingOutputHeight,
    SettingCopyRegion,
    SettingOverlayDuration,
    SettingReplayPreview,
    SettingSystemAudio,
    SettingSystemAudioVolume,
    SettingApplicationAudio,
    SettingApplicationAudioVolume,
    SettingAudioBitrate,
    AudioAddMicrophone,
    AudioRemoveMicrophone,
    AudioMicrophoneEnable,
    AudioMicrophoneGain,
    AudioNoiseSuppression,
    AudioNoiseGate,
    OpenScreenshots,
    OpenRecordings,
    OpenPlugins,
    OpenReplayCache,
    OpenLogs,
    UiShow,
    UiHide,
    UiToggle,
    UiNavigate,
    QuickPanelShow,
    QuickPanelHide,
    QuickPanelToggle,
    ThemeActivate,
    ThemeReset,
    LocaleActivate,
    LocaleReset,
    Log,
};

struct LunaInstruction {
    LunaInstructionType type = LunaInstructionType::Log;
    std::wstring first;
    std::wstring second;
    int value = 0;
    int line = 0;
};

enum class LunaThemeAppearance { Dark, Light, Amoled };
enum class LunaBackdropPattern {
    Solid,
    Gradient,
    Grid,
    Dots,
    Aurora,
    Diagonal,
    Waves,
    Rings,
    Checker,
    Hexagons,
    Stars,
    Circuit,
    Scanlines,
    Noise,
    Mesh,
};
enum class LunaGradientStyle { Linear, Radial };
enum class LunaBackdropScope { Main, Panels, All };
enum class LunaBackgroundImageMode { Cover, Contain, Tile, Stretch, Center };

struct LunaThemeDefinition {
    bool present = false;
    std::wstring name;
    LunaThemeAppearance appearance = LunaThemeAppearance::Dark;
    COLORREF background = RGB(14, 16, 20);
    COLORREF sidebar = RGB(16, 19, 24);
    COLORREF topbar = RGB(18, 21, 27);
    COLORREF surface = RGB(20, 23, 29);
    COLORREF surfaceRaised = RGB(29, 35, 44);
    COLORREF surfaceHover = RGB(35, 42, 53);
    COLORREF surfacePressed = RGB(24, 29, 38);
    COLORREF surfaceSelected = RGB(32, 47, 70);
    COLORREF input = RGB(18, 22, 28);
    COLORREF border = RGB(39, 44, 54);
    COLORREF borderHover = RGB(63, 74, 91);
    COLORREF focus = RGB(123, 177, 255);
    COLORREF text = RGB(244, 246, 251);
    COLORREF textSecondary = RGB(190, 197, 210);
    COLORREF muted = RGB(137, 144, 158);
    COLORREF disabled = RGB(87, 94, 107);
    COLORREF accent = RGB(96, 149, 244);
    COLORREF accentHover = RGB(119, 171, 250);
    COLORREF accentPressed = RGB(75, 126, 219);
    COLORREF accentSecondary = RGB(164, 118, 247);
    COLORREF textOnAccent = RGB(255, 255, 255);
    COLORREF info = RGB(83, 180, 235);
    COLORREF success = RGB(78, 205, 139);
    COLORREF warning = RGB(246, 180, 72);
    COLORREF danger = RGB(243, 112, 126);
    COLORREF shadow = RGB(0, 0, 0);
    int cornerScalePercent = 100;
    int borderScalePercent = 100;
    int fontScalePercent = 100;
    int fontWeight = 0;
    bool fontItalic = false;
    std::wstring fontFamily;
    int saturationPercent = 100;
    int contrastPercent = 100;
    int brightnessPercent = 100;
    int motionScalePercent = 100;
    COLORREF backgroundSecondary = RGB(24, 31, 48);
    LunaBackdropPattern backdropPattern = LunaBackdropPattern::Solid;
    LunaGradientStyle gradientStyle = LunaGradientStyle::Linear;
    LunaBackdropScope backdropScope = LunaBackdropScope::All;
    COLORREF backdropColor = RGB(96, 149, 244);
    COLORREF backdropColorSecondary = RGB(164, 118, 247);
    int backdropOpacityPercent = 24;
    int backdropScale = 36;
    int backdropThickness = 1;
    int backdropSpeedPercent = 0;
    int backdropSeed = 1337;
    int gradientAngle = 135;
    int gradientCenterXPercent = 50;
    int gradientCenterYPercent = 50;
    int gradientSpreadPercent = 100;
    COLORREF vignetteColor = RGB(0, 0, 0);
    int vignetteOpacityPercent = 0;
    int noiseOpacityPercent = 0;
    std::wstring backgroundImage;
    int backgroundImageOpacityPercent = 24;
    LunaBackgroundImageMode backgroundImageMode = LunaBackgroundImageMode::Cover;
    int backgroundImagePositionXPercent = 50;
    int backgroundImagePositionYPercent = 50;
    int backgroundImageScalePercent = 100;
    COLORREF backgroundImageTint = RGB(96, 149, 244);
    int backgroundImageTintOpacityPercent = 0;
    int backgroundImageBrightnessPercent = 100;
    int backgroundImageSaturationPercent = 100;
};

struct LunaTranslation {
    std::wstring source;
    std::wstring replacement;
};

struct LunaLocaleDefinition {
    bool present = false;
    std::wstring name;
    std::vector<LunaTranslation> translations;
};

enum class LunaCommandSurface {
    QuickPanel,
    TrayMenu,
};

struct LunaCommandDefinition {
    std::wstring id;
    std::wstring title;
    std::wstring description;
    std::wstring icon = L"plugin";
    // Commands always remain available from the plugin manager. These are
    // optional, explicitly requested shortcuts on compact host surfaces.
    std::vector<LunaCommandSurface> surfaces;
    int order = 500;
    std::vector<LunaInstruction> instructions;
};

struct LunaEventDefinition {
    std::wstring eventName;
    std::vector<LunaInstruction> instructions;
    // >0 only for "on timer every <seconds> { ... }" blocks: how often the host
    // re-fires this handler while the plugin stays enabled (5..86400 seconds).
    int intervalSeconds = 0;
};

struct LunaPluginDefinition {
    int languageVersion = 0;
    // Files written before the version contract existed are legacy Host API 4
    // plugins. Host API 5-only syntax must opt in with `requires host_api 5;`.
    int requiredHostApi = 4;
    bool hostApiRequirementPresent = false;
    std::wstring id;
    std::wstring name;
    std::wstring version;
    std::wstring author;
    std::wstring description;
    std::wstring homepage;
    std::vector<LunaPermission> permissions;
    LunaThemeDefinition theme;
    LunaLocaleDefinition locale;
    std::vector<LunaCommandDefinition> commands;
    std::vector<LunaEventDefinition> events;
};

struct LunaDiagnostic {
    int line = 0;
    int column = 0;
    std::wstring message;
};

class LunaScript {
public:
    static constexpr int kSupportedHostApi = 5;
    static bool Parse(const std::wstring& source, LunaPluginDefinition& plugin,
                      std::vector<LunaDiagnostic>& diagnostics);
    static const wchar_t* PermissionName(LunaPermission permission);
    static bool HasPermission(const LunaPluginDefinition& plugin, LunaPermission permission);
    static const wchar_t* CommandSurfaceName(LunaCommandSurface surface);
    static bool HasCommandSurface(const LunaCommandDefinition& command, LunaCommandSurface surface);
};
