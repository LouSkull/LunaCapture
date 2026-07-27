#pragma once

// Central GDI+ include and a small set of anti-aliased drawing helpers.
//
// The whole UI used to be painted with raw GDI (CreatePen/LineTo/RoundRect),
// which has no anti-aliasing, so every icon, rounded corner and circle came out
// jagged. Everything visual now routes through GDI+ with SmoothingModeAntiAlias,
// which is what makes the interface look clean.

#include <Windows.h>
#include <objidl.h>  // IStream / PROPID — GDI+ needs these; WIN32_LEAN_AND_MEAN omits them.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// GDI+ headers reference std::min / std::max, but the project builds with
// NOMINMAX. Pull them into the Gdiplus namespace before including the header.
namespace Gdiplus {
using std::max;
using std::min;
}
#include <gdiplus.h>

namespace ui {

enum class ThemeAppearance { Dark, Light, Amoled };
enum class ThemeSurface { Main, Panel, Overlay, Dialog, EditorChrome };
enum class BackdropPattern {
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
enum class GradientStyle { Linear, Radial };
enum class BackdropScope { Main, Panels, All };
enum class BackgroundImageMode { Cover, Contain, Tile, Stretch, Center };

// Explicit semantic roles are the stable contract for new UI. ResolveThemeColor
// remains available for the older RGB-authored controls while those call sites
// are migrated incrementally.
enum class ThemeRole {
    Background,
    BackgroundSecondary,
    Sidebar,
    Topbar,
    Surface,
    SurfaceRaised,
    SurfaceHover,
    SurfacePressed,
    SurfaceSelected,
    Input,
    Border,
    BorderHover,
    Focus,
    Text,
    TextSecondary,
    Muted,
    Disabled,
    Accent,
    AccentHover,
    AccentPressed,
    AccentSecondary,
    TextOnAccent,
    Info,
    Success,
    Warning,
    Danger,
    Shadow,
    BackdropColor,
    BackdropColorSecondary,
    Vignette,
};

struct ThemePalette {
    bool active = false;
    ThemeAppearance appearance = ThemeAppearance::Dark;
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
    BackdropPattern backdropPattern = BackdropPattern::Solid;
    GradientStyle gradientStyle = GradientStyle::Linear;
    BackdropScope backdropScope = BackdropScope::All;
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
    int backgroundImagePositionXPercent = 50;
    int backgroundImagePositionYPercent = 50;
    int backgroundImageScalePercent = 100;
    COLORREF backgroundImageTint = RGB(96, 149, 244);
    int backgroundImageTintOpacityPercent = 0;
    int backgroundImageBrightnessPercent = 100;
    int backgroundImageSaturationPercent = 100;
};

struct ThemeBackgroundImageOptions {
    int opacityPercent = 24;
    BackgroundImageMode mode = BackgroundImageMode::Cover;
    int positionXPercent = 50;
    int positionYPercent = 50;
    int scalePercent = 100;
    COLORREF tint = RGB(96, 149, 244);
    int tintOpacityPercent = 0;
    int brightnessPercent = 100;
    int saturationPercent = 100;
};

void SetThemePalette(const ThemePalette& palette);
void ResetThemePalette();
ThemePalette CurrentThemePalette();
COLORREF ThemeColor(ThemeRole role);
COLORREF ResolveThemeColor(COLORREF color);
int ResolveThemeRadius(int radius);
int ResolveThemeStrokeWidth(int width);
int ResolveThemeDuration(int durationMs);
HFONT CreateThemeFont(int pixels, int weight = FW_NORMAL, bool italic = false);
bool IsThemeBackdropAnimated() noexcept;
void SetTranslations(const std::vector<std::pair<std::wstring, std::wstring>>& translations);
void ResetTranslations();
std::wstring TranslateText(const std::wstring& source);
bool SetThemeBackgroundImage(const std::wstring& path, int opacityPercent, BackgroundImageMode mode);
bool SetThemeBackgroundImage(const std::wstring& path, const ThemeBackgroundImageOptions& options);
// Decodes an already bounded snapshot. Callers that enforce a package boundary
// should prefer this overload so a reparse point cannot be swapped between path
// validation and GDI+ opening the file.
bool SetThemeBackgroundImage(const std::vector<std::uint8_t>& encodedBytes,
                             const ThemeBackgroundImageOptions& options);
bool SetThemeBackgroundImage(const std::wstring& path, int opacityPercent, BackgroundImageMode mode,
                             int positionXPercent, int positionYPercent, int scalePercent,
                             COLORREF tint, int tintOpacityPercent,
                             int brightnessPercent, int saturationPercent);
void ResetThemeBackgroundImage();
void PaintThemeBackdrop(HDC dc, RECT rect, ThemeSurface surface = ThemeSurface::Main,
                        std::uint64_t timestampMs = 0);

class ScopedThemeBypass {
public:
    ScopedThemeBypass();
    ~ScopedThemeBypass();
    ScopedThemeBypass(const ScopedThemeBypass&) = delete;
    ScopedThemeBypass& operator=(const ScopedThemeBypass&) = delete;
};

// Security-sensitive host copy (permission review, plugin management) must not
// be relabelled by a third-party locale pack while the user is making a choice.
class ScopedTranslationBypass {
public:
    ScopedTranslationBypass();
    ~ScopedTranslationBypass();
    ScopedTranslationBypass(const ScopedTranslationBypass&) = delete;
    ScopedTranslationBypass& operator=(const ScopedTranslationBypass&) = delete;
};

// Initialise / shut down the GDI+ runtime once for the whole process.
bool InitGraphics();
void ShutdownGraphics();

// Convert a Win32 COLORREF (0x00BBGGRR) to a GDI+ color with an alpha byte.
Gdiplus::Color FromColorRef(COLORREF color, BYTE alpha = 255);
Gdiplus::Color FromColorRefRaw(COLORREF color, BYTE alpha = 255);

// Linearly interpolate two COLORREF values (t in [0,1]).
COLORREF MixColor(COLORREF a, COLORREF b, float t);

// A RAII anti-aliased graphics context bound to an existing HDC. Destroying it
// flushes all pending GDI+ output to the DC, after which plain GDI text drawing
// on the same DC is safe again.
class Canvas {
public:
    explicit Canvas(HDC dc);
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    Gdiplus::Graphics& g() { return *graphics_; }
    bool ok() const { return graphics_ != nullptr; }

private:
    std::unique_ptr<Gdiplus::Graphics> graphics_;
};

// Convenience helpers that each create a short-lived anti-aliased Canvas.
void FillRoundRect(HDC dc, RECT rect, int radius, COLORREF color, BYTE alpha = 255);
void StrokeRoundRect(HDC dc, RECT rect, int radius, COLORREF color, int width = 1, BYTE alpha = 255);
void FillEllipse(HDC dc, RECT rect, COLORREF color, BYTE alpha = 255);
void StrokeEllipse(HDC dc, RECT rect, COLORREF color, int width = 1, BYTE alpha = 255);
void FillRectAlpha(HDC dc, RECT rect, COLORREF color, BYTE alpha);

// Vertical two-stop gradient inside a rounded rectangle (used for badges/headers).
void FillRoundRectVGradient(HDC dc, RECT rect, int radius, COLORREF top, COLORREF bottom, BYTE alpha = 255);

// Create a top-down 32-bit surface suitable for UpdateLayeredWindow. The
// returned pixel pointer remains valid until the bitmap is deleted.
HBITMAP CreateLayeredDib(int width, int height, void** pixels);

// Mark a freshly background-painted 32-bit surface as opaque before any
// translucent GDI+ primitives are composited onto it.
void MakeLayeredSurfaceOpaque(void* pixels, int width, int height);

// Present an opaque GDI/GDI+ back buffer through a supersampled rounded alpha
// mask. Unlike SetWindowRgn this keeps the outside pixels genuinely transparent
// and gives Windows 10 smooth, non-black corner pixels.
bool PresentRoundedLayeredWindow(HWND window, HDC source, void* pixels,
                                 int width, int height, int radius, BYTE opacity = 255);

// Hit-test helper matching the visual alpha mask above.
bool PointInRoundedRect(POINT point, int width, int height, int radius);

}  // namespace ui
