#include "UiGfx.hpp"

#include "Localization.hpp"

#pragma comment(lib, "gdiplus.lib")

#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace ui {
namespace {
ULONG_PTR g_token = 0;
ThemePalette g_theme{};
thread_local int g_themeBypassDepth = 0;
thread_local int g_translationBypassDepth = 0;
std::unordered_map<std::wstring, std::wstring> g_translations;
std::vector<std::pair<std::wstring, std::wstring>> g_translationTemplates;
std::unordered_map<std::wstring, std::wstring> g_translationCache;
std::mutex g_translationMutex;
std::unique_ptr<Gdiplus::Image> g_backgroundImage;
ThemeBackgroundImageOptions g_backgroundImageOptions{};

template <typename T>
T Clamp(T value, T minimum, T maximum) {
    return (std::max)(minimum, (std::min)(maximum, value));
}

const ThemePalette& DefaultTheme() {
    static const ThemePalette palette{};
    return palette;
}

float Luminance(COLORREF color) {
    return (GetRValue(color) * 0.2126f + GetGValue(color) * 0.7152f + GetBValue(color) * 0.0722f) / 255.0f;
}

COLORREF Blend(COLORREF a, COLORREF b, float t) {
    t = Clamp(t, 0.0f, 1.0f);
    const auto mix = [t](BYTE x, BYTE y) { return static_cast<BYTE>(x + (y - x) * t); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

COLORREF RawThemeColor(const ThemePalette& palette, ThemeRole role) {
    switch (role) {
        case ThemeRole::Background: return palette.background;
        case ThemeRole::BackgroundSecondary: return palette.backgroundSecondary;
        case ThemeRole::Sidebar: return palette.sidebar;
        case ThemeRole::Topbar: return palette.topbar;
        case ThemeRole::Surface: return palette.surface;
        case ThemeRole::SurfaceRaised: return palette.surfaceRaised;
        case ThemeRole::SurfaceHover: return palette.surfaceHover;
        case ThemeRole::SurfacePressed: return palette.surfacePressed;
        case ThemeRole::SurfaceSelected: return palette.surfaceSelected;
        case ThemeRole::Input: return palette.input;
        case ThemeRole::Border: return palette.border;
        case ThemeRole::BorderHover: return palette.borderHover;
        case ThemeRole::Focus: return palette.focus;
        case ThemeRole::Text: return palette.text;
        case ThemeRole::TextSecondary: return palette.textSecondary;
        case ThemeRole::Muted: return palette.muted;
        case ThemeRole::Disabled: return palette.disabled;
        case ThemeRole::Accent: return palette.accent;
        case ThemeRole::AccentHover: return palette.accentHover;
        case ThemeRole::AccentPressed: return palette.accentPressed;
        case ThemeRole::AccentSecondary: return palette.accentSecondary;
        case ThemeRole::TextOnAccent: return palette.textOnAccent;
        case ThemeRole::Info: return palette.info;
        case ThemeRole::Success: return palette.success;
        case ThemeRole::Warning: return palette.warning;
        case ThemeRole::Danger: return palette.danger;
        case ThemeRole::Shadow: return palette.shadow;
        case ThemeRole::BackdropColor: return palette.backdropColor;
        case ThemeRole::BackdropColorSecondary: return palette.backdropColorSecondary;
        case ThemeRole::Vignette: return palette.vignetteColor;
        default: return palette.text;
    }
}

COLORREF AdjustThemeColor(COLORREF color, const ThemePalette& palette) {
    const float saturation = static_cast<float>(palette.saturationPercent) / 100.0f;
    const float contrast = static_cast<float>(palette.contrastPercent) / 100.0f;
    const float brightness = static_cast<float>(palette.brightnessPercent) / 100.0f;
    const float r = static_cast<float>(GetRValue(color));
    const float g = static_cast<float>(GetGValue(color));
    const float b = static_cast<float>(GetBValue(color));
    const float gray = r * 0.2126f + g * 0.7152f + b * 0.0722f;
    const auto channel = [&](float value) {
        value = gray + (value - gray) * saturation;
        value = (value - 127.5f) * contrast + 127.5f;
        value *= brightness;
        return static_cast<BYTE>(std::lround(Clamp(value, 0.0f, 255.0f)));
    };
    return RGB(channel(r), channel(g), channel(b));
}

BYTE PercentAlpha(int percent) {
    return static_cast<BYTE>((Clamp(percent, 0, 100) * 255 + 50) / 100);
}

bool SurfaceInScope(ThemeSurface surface, BackdropScope scope) {
    if (scope == BackdropScope::All) return true;
    if (scope == BackdropScope::Main) return surface == ThemeSurface::Main;
    return surface != ThemeSurface::Main;
}

std::uint32_t Hash32(std::uint32_t value) {
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    return value ^ (value >> 16);
}

float UnitHash(std::uint32_t value) {
    return static_cast<float>(Hash32(value) & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

COLORREF SurfaceBaseColor(ThemeSurface surface) {
    switch (surface) {
        case ThemeSurface::Main: return ThemeColor(ThemeRole::Background);
        case ThemeSurface::Panel: return ThemeColor(ThemeRole::Surface);
        case ThemeSurface::Overlay: return ThemeColor(ThemeRole::SurfaceRaised);
        case ThemeSurface::Dialog: return ThemeColor(ThemeRole::Background);
        case ThemeSurface::EditorChrome: return ThemeColor(ThemeRole::Surface);
        default: return ThemeColor(ThemeRole::Background);
    }
}

COLORREF SurfaceSecondaryColor(ThemeSurface surface) {
    switch (surface) {
        case ThemeSurface::Main: return ThemeColor(ThemeRole::BackgroundSecondary);
        case ThemeSurface::Panel: return ThemeColor(ThemeRole::SurfaceRaised);
        case ThemeSurface::Overlay: return ThemeColor(ThemeRole::SurfaceSelected);
        case ThemeSurface::Dialog: return ThemeColor(ThemeRole::Surface);
        case ThemeSurface::EditorChrome: return ThemeColor(ThemeRole::SurfaceRaised);
        default: return ThemeColor(ThemeRole::BackgroundSecondary);
    }
}

COLORREF SemanticTone(COLORREF base, float sourceLuminance, const ThemePalette& palette) {
    if (sourceLuminance < 0.26f) return Blend(palette.background, base, 0.62f);
    if (sourceLuminance > 0.82f) return Blend(base, palette.text, 0.28f);
    return base;
}

void BuildRoundRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    const float d = radius * 2.0f;
    const float maxD = (std::min)(r.Width, r.Height);
    const float dd = (std::min)(d, maxD);
    if (dd <= 0.5f) {
        path.AddRectangle(r);
        path.CloseFigure();
        return;
    }
    const float x = r.X;
    const float y = r.Y;
    const float w = r.Width;
    const float h = r.Height;
    path.AddArc(x, y, dd, dd, 180.0f, 90.0f);
    path.AddArc(x + w - dd, y, dd, dd, 270.0f, 90.0f);
    path.AddArc(x + w - dd, y + h - dd, dd, dd, 0.0f, 90.0f);
    path.AddArc(x, y + h - dd, dd, dd, 90.0f, 90.0f);
    path.CloseFigure();
}
}  // namespace

bool InitGraphics() {
    Gdiplus::GdiplusStartupInput input;
    return Gdiplus::GdiplusStartup(&g_token, &input, nullptr) == Gdiplus::Ok;
}

void SetThemePalette(const ThemePalette& palette) {
    g_theme = palette;
    g_theme.cornerScalePercent = Clamp(g_theme.cornerScalePercent, 50, 160);
    g_theme.borderScalePercent = Clamp(g_theme.borderScalePercent, 50, 250);
    g_theme.fontScalePercent = Clamp(g_theme.fontScalePercent, 75, 150);
    if (g_theme.fontWeight != 0) g_theme.fontWeight = Clamp(g_theme.fontWeight, 100, 900);
    if (g_theme.fontFamily.size() > 31) g_theme.fontFamily.resize(31);
    g_theme.saturationPercent = Clamp(g_theme.saturationPercent, 0, 200);
    g_theme.contrastPercent = Clamp(g_theme.contrastPercent, 50, 200);
    g_theme.brightnessPercent = Clamp(g_theme.brightnessPercent, 25, 200);
    g_theme.motionScalePercent = Clamp(g_theme.motionScalePercent, 0, 200);
    g_theme.backdropOpacityPercent = Clamp(g_theme.backdropOpacityPercent, 0, 100);
    g_theme.backdropScale = Clamp(g_theme.backdropScale, 8, 192);
    g_theme.backdropThickness = Clamp(g_theme.backdropThickness, 1, 8);
    g_theme.backdropSpeedPercent = Clamp(g_theme.backdropSpeedPercent, 0, 200);
    g_theme.backdropSeed = Clamp(g_theme.backdropSeed, 0, 65535);
    g_theme.gradientAngle %= 360;
    if (g_theme.gradientAngle < 0) g_theme.gradientAngle += 360;
    g_theme.gradientCenterXPercent = Clamp(g_theme.gradientCenterXPercent, 0, 100);
    g_theme.gradientCenterYPercent = Clamp(g_theme.gradientCenterYPercent, 0, 100);
    g_theme.gradientSpreadPercent = Clamp(g_theme.gradientSpreadPercent, 25, 200);
    g_theme.vignetteOpacityPercent = Clamp(g_theme.vignetteOpacityPercent, 0, 90);
    g_theme.noiseOpacityPercent = Clamp(g_theme.noiseOpacityPercent, 0, 40);
    g_theme.backgroundImagePositionXPercent = Clamp(g_theme.backgroundImagePositionXPercent, 0, 100);
    g_theme.backgroundImagePositionYPercent = Clamp(g_theme.backgroundImagePositionYPercent, 0, 100);
    g_theme.backgroundImageScalePercent = Clamp(g_theme.backgroundImageScalePercent, 25, 400);
    g_theme.backgroundImageTintOpacityPercent = Clamp(g_theme.backgroundImageTintOpacityPercent, 0, 100);
    g_theme.backgroundImageBrightnessPercent = Clamp(g_theme.backgroundImageBrightnessPercent, 25, 200);
    g_theme.backgroundImageSaturationPercent = Clamp(g_theme.backgroundImageSaturationPercent, 0, 200);
    if (static_cast<unsigned>(g_theme.appearance) > static_cast<unsigned>(ThemeAppearance::Amoled)) {
        g_theme.appearance = ThemeAppearance::Dark;
    }
    if (static_cast<unsigned>(g_theme.backdropPattern) > static_cast<unsigned>(BackdropPattern::Mesh)) {
        g_theme.backdropPattern = BackdropPattern::Solid;
    }
    if (static_cast<unsigned>(g_theme.gradientStyle) > static_cast<unsigned>(GradientStyle::Radial)) {
        g_theme.gradientStyle = GradientStyle::Linear;
    }
    if (static_cast<unsigned>(g_theme.backdropScope) > static_cast<unsigned>(BackdropScope::All)) {
        g_theme.backdropScope = BackdropScope::All;
    }
}

void ResetThemePalette() {
    g_theme = {};
    ResetThemeBackgroundImage();
}

ThemePalette CurrentThemePalette() {
    return g_theme;
}

COLORREF ThemeColor(ThemeRole role) {
    const ThemePalette& palette = (g_themeBypassDepth > 0 || !g_theme.active) ? DefaultTheme() : g_theme;
    return palette.active || &palette == &g_theme
        ? AdjustThemeColor(RawThemeColor(palette, role), palette)
        : RawThemeColor(palette, role);
}

COLORREF ResolveThemeColor(COLORREF color) {
    if (g_themeBypassDepth > 0) return color;
    const ThemePalette& palette = g_theme;
    if (!palette.active || color == RGB(0, 0, 0)) return color;

    // Preserve the meaning of canonical host colors exactly. Less common legacy
    // shades continue through the continuous mapper below.
    const ThemePalette& defaults = DefaultTheme();
    const ThemeRole canonicalRoles[] = {
        ThemeRole::Background, ThemeRole::BackgroundSecondary, ThemeRole::Sidebar, ThemeRole::Topbar,
        ThemeRole::Surface, ThemeRole::SurfaceRaised, ThemeRole::SurfaceHover, ThemeRole::SurfacePressed,
        ThemeRole::SurfaceSelected, ThemeRole::Input, ThemeRole::Border, ThemeRole::BorderHover,
        ThemeRole::Focus, ThemeRole::Text, ThemeRole::TextSecondary, ThemeRole::Muted,
        ThemeRole::Disabled, ThemeRole::Accent, ThemeRole::AccentHover, ThemeRole::AccentPressed,
        ThemeRole::AccentSecondary, ThemeRole::TextOnAccent, ThemeRole::Info, ThemeRole::Success,
        ThemeRole::Warning, ThemeRole::Danger,
    };
    for (ThemeRole role : canonicalRoles) {
        if (color == RawThemeColor(defaults, role)) return ThemeColor(role);
    }

    const int r = GetRValue(color);
    const int g = GetGValue(color);
    const int b = GetBValue(color);
    const int high = (std::max)(r, (std::max)(g, b));
    const int low = (std::min)(r, (std::min)(g, b));
    const float saturation = high == 0 ? 0.0f : static_cast<float>(high - low) / high;
    const float luminance = Luminance(color);
    if (saturation < 0.18f) {
        COLORREF mapped = palette.text;
        if (luminance < 0.075f) mapped = palette.background;
        else if (luminance < 0.12f) mapped = Blend(palette.background, palette.surface, (luminance - 0.075f) / 0.045f);
        else if (luminance < 0.22f) mapped = Blend(palette.surface, palette.surfaceRaised, (luminance - 0.12f) / 0.10f);
        else if (luminance < 0.39f) mapped = Blend(palette.surfaceRaised, palette.border, (luminance - 0.22f) / 0.17f);
        else if (luminance < 0.76f) mapped = Blend(palette.border, palette.muted, (luminance - 0.39f) / 0.37f);
        else mapped = Blend(palette.muted, palette.text, (luminance - 0.76f) / 0.24f);
        return AdjustThemeColor(mapped, palette);
    }
    if (r > g * 1.18f && r > b * 1.12f) {
        if (g > b * 1.35f && g > r * 0.45f) {
            return AdjustThemeColor(SemanticTone(palette.warning, luminance, palette), palette);
        }
        return AdjustThemeColor(SemanticTone(palette.danger, luminance, palette), palette);
    }
    if (g > r * 1.13f && g > b * 0.93f) {
        return AdjustThemeColor(SemanticTone(palette.success, luminance, palette), palette);
    }
    if (r > b * 0.82f && b > g * 1.08f) {
        return AdjustThemeColor(SemanticTone(palette.accentSecondary, luminance, palette), palette);
    }
    return AdjustThemeColor(SemanticTone(luminance > 0.68f ? palette.accentHover : palette.accent,
                                          luminance, palette), palette);
}

int ResolveThemeRadius(int radius) {
    if (g_themeBypassDepth > 0) return radius;
    if (!g_theme.active) return radius;
    return (std::max)(0, (radius * g_theme.cornerScalePercent + 50) / 100);
}

int ResolveThemeStrokeWidth(int width) {
    if (width <= 0 || g_themeBypassDepth > 0 || !g_theme.active) return width;
    return (std::max)(1, (width * g_theme.borderScalePercent + 50) / 100);
}

int ResolveThemeDuration(int durationMs) {
    if (durationMs <= 0 || g_themeBypassDepth > 0 || !g_theme.active) return durationMs;
    const long long scaled = static_cast<long long>(durationMs) * g_theme.motionScalePercent;
    return static_cast<int>((std::min<long long>)((std::numeric_limits<int>::max)(), (scaled + 50) / 100));
}

HFONT CreateThemeFont(int pixels, int weight, bool italic) {
    const ThemePalette& palette = (g_themeBypassDepth > 0 || !g_theme.active) ? DefaultTheme() : g_theme;
    const int scaledPixels = (std::max)(1, (std::abs(pixels) * palette.fontScalePercent + 50) / 100);
    int effectiveWeight = weight;
    if (palette.fontWeight != 0) effectiveWeight = Clamp(weight + palette.fontWeight - FW_NORMAL, 100, 900);
    const wchar_t* family = palette.fontFamily.empty() ? L"Segoe UI" : palette.fontFamily.c_str();
    return CreateFontW(-scaledPixels, 0, 0, 0, effectiveWeight,
                       (italic || palette.fontItalic) ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, family);
}

bool IsThemeBackdropAnimated() noexcept {
    return g_theme.active && g_theme.backdropPattern != BackdropPattern::Solid &&
           g_theme.backdropSpeedPercent > 0 && g_theme.motionScalePercent > 0;
}

bool AreSystemAnimationsEnabled() noexcept {
    // Luna renders its own animations; do not change or inherit the Windows preference.
    return true;
}

void SetTranslations(const std::vector<std::pair<std::wstring, std::wstring>>& translations) {
    const std::scoped_lock lock(g_translationMutex);
    g_translations.clear();
    g_translationTemplates.clear();
    g_translationCache.clear();
    for (const auto& [source, replacement] : translations) {
        if (source.empty() || replacement.empty()) continue;
        if (HasLocalizationPlaceholders(source)) g_translationTemplates.emplace_back(source, replacement);
        else g_translations[source] = replacement;
    }
    // Prefer the most specific template when two patterns could both match the
    // same runtime string. Stable sort keeps source-file order as the tie-break.
    std::stable_sort(g_translationTemplates.begin(), g_translationTemplates.end(),
        [](const auto& left, const auto& right) {
            const auto literalLength = [](const std::wstring& pattern) {
                std::size_t result = pattern.size();
                for (std::size_t position = 0; position + 2 < pattern.size(); ++position) {
                    if (pattern[position] == L'{' && pattern[position + 1] >= L'0' &&
                        pattern[position + 1] <= L'9' && pattern[position + 2] == L'}') {
                        result -= 3;
                        position += 2;
                    }
                }
                return result;
            };
            return literalLength(left.first) > literalLength(right.first);
        });
}

void ResetTranslations() {
    const std::scoped_lock lock(g_translationMutex);
    g_translations.clear();
    g_translationTemplates.clear();
    g_translationCache.clear();
}

std::wstring TranslateText(const std::wstring& source) {
    if (g_translationBypassDepth > 0) return source;
    const std::scoped_lock lock(g_translationMutex);
    const auto found = g_translations.find(source);
    if (found != g_translations.end()) return found->second;
    const auto cached = g_translationCache.find(source);
    if (cached != g_translationCache.end()) return cached->second;
    std::wstring translated;
    bool matched = false;
    for (const auto& [pattern, replacement] : g_translationTemplates) {
        if (ExpandLocalizationTemplate(pattern, replacement, source, translated)) {
            matched = true;
            break;
        }
    }
    if (!matched) translated = source;
    constexpr std::size_t kMaximumTranslationCacheEntries = 2048;
    if (g_translationCache.size() >= kMaximumTranslationCacheEntries) g_translationCache.clear();
    g_translationCache.emplace(source, translated);
    return translated;
}

namespace {
ThemeBackgroundImageOptions NormalizeImageOptions(ThemeBackgroundImageOptions options) {
    options.opacityPercent = Clamp(options.opacityPercent, 0, 100);
    options.positionXPercent = Clamp(options.positionXPercent, 0, 100);
    options.positionYPercent = Clamp(options.positionYPercent, 0, 100);
    options.scalePercent = Clamp(options.scalePercent, 25, 400);
    options.tintOpacityPercent = Clamp(options.tintOpacityPercent, 0, 100);
    options.brightnessPercent = Clamp(options.brightnessPercent, 25, 200);
    options.saturationPercent = Clamp(options.saturationPercent, 0, 200);
    if (static_cast<unsigned>(options.mode) > static_cast<unsigned>(BackgroundImageMode::Center)) {
        options.mode = BackgroundImageMode::Cover;
    }
    return options;
}

void PaintBase(Gdiplus::Graphics& graphics, const Gdiplus::RectF& bounds, ThemeSurface surface) {
    const Gdiplus::Color first = FromColorRefRaw(SurfaceBaseColor(surface));
    const Gdiplus::Color second = FromColorRefRaw(SurfaceSecondaryColor(surface));
    if (g_theme.backdropPattern == BackdropPattern::Solid) {
        Gdiplus::SolidBrush brush(first);
        graphics.FillRectangle(&brush, bounds);
        return;
    }
    if (g_theme.gradientStyle == GradientStyle::Radial) {
        Gdiplus::GraphicsPath path;
        path.AddRectangle(bounds);
        Gdiplus::PathGradientBrush brush(&path);
        brush.SetCenterColor(first);
        Gdiplus::Color surround[1] = {second};
        INT count = 1;
        brush.SetSurroundColors(surround, &count);
        brush.SetCenterPoint(Gdiplus::PointF(
            bounds.X + bounds.Width * g_theme.gradientCenterXPercent / 100.0f,
            bounds.Y + bounds.Height * g_theme.gradientCenterYPercent / 100.0f));
        // PathGradient treats a focus scale of 1 as an entirely flat center
        // region. Map the public 25..200 spread range below that singular value
        // so even the default spread remains visibly radial.
        const float focus = Clamp(g_theme.gradientSpreadPercent / 250.0f, 0.10f, 0.80f);
        brush.SetFocusScales(focus, focus);
        graphics.FillRectangle(&brush, bounds);
        return;
    }
    Gdiplus::LinearGradientBrush brush(bounds, first, second,
        static_cast<Gdiplus::REAL>(g_theme.gradientAngle), TRUE);
    graphics.FillRectangle(&brush, bounds);
}

void PaintBackgroundImage(Gdiplus::Graphics& graphics, const Gdiplus::RectF& bounds) {
    if (!g_backgroundImage || g_backgroundImageOptions.opacityPercent <= 0) return;
    const float imageWidth = static_cast<float>(g_backgroundImage->GetWidth());
    const float imageHeight = static_cast<float>(g_backgroundImage->GetHeight());
    if (imageWidth <= 0.0f || imageHeight <= 0.0f) return;

    const ThemeBackgroundImageOptions& options = g_backgroundImageOptions;
    const float saturation = options.saturationPercent / 100.0f;
    const float brightness = options.brightnessPercent / 100.0f;
    const float tint = options.tintOpacityPercent / 100.0f;
    const float keep = 1.0f - tint;
    constexpr float lr = 0.2126f, lg = 0.7152f, lb = 0.0722f;
    const float inv = 1.0f - saturation;
    Gdiplus::ColorMatrix matrix{};
    matrix.m[0][0] = (lr * inv + saturation) * brightness * keep;
    matrix.m[0][1] = lr * inv * brightness * keep;
    matrix.m[0][2] = lr * inv * brightness * keep;
    matrix.m[1][0] = lg * inv * brightness * keep;
    matrix.m[1][1] = (lg * inv + saturation) * brightness * keep;
    matrix.m[1][2] = lg * inv * brightness * keep;
    matrix.m[2][0] = lb * inv * brightness * keep;
    matrix.m[2][1] = lb * inv * brightness * keep;
    matrix.m[2][2] = (lb * inv + saturation) * brightness * keep;
    matrix.m[3][3] = options.opacityPercent / 100.0f;
    matrix.m[4][0] = GetRValue(options.tint) / 255.0f * tint;
    matrix.m[4][1] = GetGValue(options.tint) / 255.0f * tint;
    matrix.m[4][2] = GetBValue(options.tint) / 255.0f * tint;
    matrix.m[4][4] = 1.0f;
    Gdiplus::ImageAttributes attributes;
    attributes.SetColorMatrix(&matrix, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);

    const float zoom = options.scalePercent / 100.0f;
    const float anchorX = options.positionXPercent / 100.0f;
    const float anchorY = options.positionYPercent / 100.0f;
    const Gdiplus::GraphicsState state = graphics.Save();
    graphics.SetClip(bounds);
    if (options.mode == BackgroundImageMode::Tile) {
        const float tileWidth = (std::max)(16.0f, imageWidth * zoom);
        const float tileHeight = (std::max)(16.0f, imageHeight * zoom);
        const float startX = bounds.X - std::fmod(tileWidth * anchorX, tileWidth);
        const float startY = bounds.Y - std::fmod(tileHeight * anchorY, tileHeight);
        for (float y = startY; y < bounds.GetBottom(); y += tileHeight) {
            for (float x = startX; x < bounds.GetRight(); x += tileWidth) {
                graphics.DrawImage(g_backgroundImage.get(), Gdiplus::RectF(x, y, tileWidth, tileHeight),
                                   0, 0, imageWidth, imageHeight, Gdiplus::UnitPixel, &attributes);
            }
        }
    } else {
        float drawWidth = imageWidth;
        float drawHeight = imageHeight;
        if (options.mode == BackgroundImageMode::Stretch) {
            drawWidth = bounds.Width * zoom;
            drawHeight = bounds.Height * zoom;
        } else {
            float fit = 1.0f;
            if (options.mode == BackgroundImageMode::Cover) {
                fit = (std::max)(bounds.Width / imageWidth, bounds.Height / imageHeight);
            } else if (options.mode == BackgroundImageMode::Contain) {
                fit = (std::min)(bounds.Width / imageWidth, bounds.Height / imageHeight);
            }
            drawWidth = imageWidth * fit * zoom;
            drawHeight = imageHeight * fit * zoom;
        }
        const float x = bounds.X + (bounds.Width - drawWidth) * anchorX;
        const float y = bounds.Y + (bounds.Height - drawHeight) * anchorY;
        graphics.DrawImage(g_backgroundImage.get(), Gdiplus::RectF(x, y, drawWidth, drawHeight),
                           0, 0, imageWidth, imageHeight, Gdiplus::UnitPixel, &attributes);
    }
    graphics.Restore(state);
}

void PaintNoise(Gdiplus::Graphics& graphics, const Gdiplus::RectF& bounds, int opacityPercent,
                std::uint32_t seed) {
    if (opacityPercent <= 0) return;
    const int count = Clamp(static_cast<int>(bounds.Width * bounds.Height / 96.0f), 48, 4096);
    Gdiplus::SolidBrush light(FromColorRefRaw(ThemeColor(ThemeRole::Text), PercentAlpha(opacityPercent)));
    Gdiplus::SolidBrush dark(FromColorRefRaw(ThemeColor(ThemeRole::Shadow), PercentAlpha(opacityPercent)));
    for (int i = 0; i < count; ++i) {
        const std::uint32_t hx = Hash32(seed + static_cast<std::uint32_t>(i) * 0x9E3779B9u);
        const std::uint32_t hy = Hash32(hx ^ 0xA511E9B3u);
        const float x = bounds.X + UnitHash(hx) * bounds.Width;
        const float y = bounds.Y + UnitHash(hy) * bounds.Height;
        graphics.FillRectangle((hx & 1u) ? &light : &dark, x, y, 1.0f, 1.0f);
    }
}

void PaintPattern(Gdiplus::Graphics& graphics, const Gdiplus::RectF& bounds,
                  std::uint64_t timestampMs) {
    const int spacing = g_theme.backdropScale;
    const float thickness = static_cast<float>(g_theme.backdropThickness);
    const int opacity = g_theme.backdropOpacityPercent;
    if (opacity <= 0 || g_theme.backdropPattern == BackdropPattern::Solid ||
        g_theme.backdropPattern == BackdropPattern::Gradient) return;

    const float seconds = static_cast<float>(timestampMs) / 1000.0f;
    const float velocity = g_theme.backdropSpeedPercent / 100.0f * g_theme.motionScalePercent / 100.0f;
    const float phase = seconds * velocity;
    const float shift = std::fmod(phase * spacing, static_cast<float>(spacing));
    const Gdiplus::Color primary = FromColorRefRaw(ThemeColor(ThemeRole::BackdropColor), PercentAlpha(opacity));
    const Gdiplus::Color secondary = FromColorRefRaw(ThemeColor(ThemeRole::BackdropColorSecondary),
                                                     PercentAlpha((opacity * 3) / 4));
    Gdiplus::Pen primaryPen(primary, thickness);
    Gdiplus::Pen secondaryPen(secondary, (std::max)(1.0f, thickness * 0.75f));
    primaryPen.SetLineJoin(Gdiplus::LineJoinRound);
    primaryPen.SetStartCap(Gdiplus::LineCapRound);
    primaryPen.SetEndCap(Gdiplus::LineCapRound);
    Gdiplus::SolidBrush primaryBrush(primary);
    Gdiplus::SolidBrush secondaryBrush(secondary);

    switch (g_theme.backdropPattern) {
        case BackdropPattern::Grid:
            for (float x = bounds.X - spacing + shift; x < bounds.GetRight(); x += spacing) {
                graphics.DrawLine(&primaryPen, x, bounds.Y, x, bounds.GetBottom());
            }
            for (float y = bounds.Y - spacing + shift; y < bounds.GetBottom(); y += spacing) {
                graphics.DrawLine(&secondaryPen, bounds.X, y, bounds.GetRight(), y);
            }
            break;
        case BackdropPattern::Dots: {
            const float diameter = (std::max)(2.0f, thickness + 1.5f);
            for (float y = bounds.Y + spacing * 0.5f + shift; y < bounds.GetBottom(); y += spacing) {
                for (float x = bounds.X + spacing * 0.5f; x < bounds.GetRight(); x += spacing) {
                    graphics.FillEllipse(&primaryBrush, x - diameter / 2, y - diameter / 2, diameter, diameter);
                }
            }
            break;
        }
        case BackdropPattern::Aurora: {
            const float oscillationX = std::sin(phase * 0.9f) * bounds.Width * 0.10f;
            const float oscillationY = std::cos(phase * 0.7f) * bounds.Height * 0.08f;
            Gdiplus::SolidBrush softPrimary(FromColorRefRaw(ThemeColor(ThemeRole::BackdropColor),
                PercentAlpha((opacity * 2) / 3)));
            Gdiplus::SolidBrush softSecondary(FromColorRefRaw(ThemeColor(ThemeRole::BackdropColorSecondary),
                PercentAlpha(opacity / 2)));
            graphics.FillEllipse(&softPrimary, bounds.X - bounds.Width * 0.22f + oscillationX,
                                 bounds.Y - bounds.Height * 0.38f + oscillationY,
                                 bounds.Width * 0.92f, bounds.Height * 1.08f);
            graphics.FillEllipse(&softSecondary, bounds.X + bounds.Width * 0.42f - oscillationX,
                                 bounds.Y + bounds.Height * 0.28f - oscillationY,
                                 bounds.Width * 0.82f, bounds.Height * 0.88f);
            break;
        }
        case BackdropPattern::Diagonal:
            for (float x = bounds.X - bounds.Height - spacing + shift; x < bounds.GetRight(); x += spacing) {
                graphics.DrawLine(&primaryPen, x, bounds.GetBottom(), x + bounds.Height, bounds.Y);
            }
            break;
        case BackdropPattern::Waves: {
            const float amplitude = (std::max)(4.0f, spacing * 0.22f);
            const float step = (std::max)(6.0f, spacing / 5.0f);
            for (float baseY = bounds.Y - spacing + shift; baseY < bounds.GetBottom() + spacing; baseY += spacing) {
                Gdiplus::GraphicsPath wave;
                float previousX = bounds.X;
                float previousY = baseY + std::sin(phase * 2.0f) * amplitude;
                for (float x = bounds.X + step; x <= bounds.GetRight() + step; x += step) {
                    const float y = baseY + std::sin((x - bounds.X) / spacing * 3.14159265f + phase * 2.0f) * amplitude;
                    wave.AddLine(previousX, previousY, x, y);
                    previousX = x;
                    previousY = y;
                }
                graphics.DrawPath(&primaryPen, &wave);
            }
            break;
        }
        case BackdropPattern::Rings: {
            const float centerX = bounds.X + bounds.Width * g_theme.gradientCenterXPercent / 100.0f +
                                  std::sin(phase) * spacing;
            const float centerY = bounds.Y + bounds.Height * g_theme.gradientCenterYPercent / 100.0f +
                                  std::cos(phase * 0.8f) * spacing;
            const float maximum = std::hypot(bounds.Width, bounds.Height);
            for (float radius = (std::max)(1.0f, shift); radius < maximum; radius += spacing) {
                graphics.DrawEllipse(&primaryPen, centerX - radius, centerY - radius, radius * 2, radius * 2);
            }
            break;
        }
        case BackdropPattern::Checker: {
            const int cell = (std::max)(8, spacing);
            const int rowStart = static_cast<int>(std::floor(bounds.Y / cell)) - 1;
            const int rowEnd = static_cast<int>(std::ceil(bounds.GetBottom() / cell)) + 1;
            const int colStart = static_cast<int>(std::floor(bounds.X / cell)) - 1;
            const int colEnd = static_cast<int>(std::ceil(bounds.GetRight() / cell)) + 1;
            for (int row = rowStart; row <= rowEnd; ++row) {
                for (int col = colStart; col <= colEnd; ++col) {
                    if (((row + col) & 1) == 0) {
                        graphics.FillRectangle(&primaryBrush, col * cell + shift, row * cell + shift,
                                               static_cast<float>(cell), static_cast<float>(cell));
                    }
                }
            }
            break;
        }
        case BackdropPattern::Hexagons: {
            const float radius = (std::max)(6.0f, spacing * 0.46f);
            const float stepX = radius * 1.73f;
            const float stepY = radius * 1.5f;
            int row = 0;
            for (float cy = bounds.Y - radius + shift; cy < bounds.GetBottom() + radius; cy += stepY, ++row) {
                for (float cx = bounds.X - radius + (row & 1 ? stepX * 0.5f : 0.0f);
                     cx < bounds.GetRight() + radius; cx += stepX) {
                    Gdiplus::PointF points[7]{};
                    for (int i = 0; i < 6; ++i) {
                        const float angle = (60.0f * i - 30.0f) * 3.14159265f / 180.0f;
                        points[i] = Gdiplus::PointF(cx + std::cos(angle) * radius, cy + std::sin(angle) * radius);
                    }
                    points[6] = points[0];
                    graphics.DrawLines(&primaryPen, points, 7);
                }
            }
            break;
        }
        case BackdropPattern::Stars: {
            const int count = Clamp(static_cast<int>(bounds.Width * bounds.Height /
                static_cast<float>(spacing * spacing) * 1.5f), 12, 1024);
            const std::uint32_t frame = static_cast<std::uint32_t>(phase * 12.0f);
            for (int i = 0; i < count; ++i) {
                const std::uint32_t hash = Hash32(static_cast<std::uint32_t>(g_theme.backdropSeed) +
                                                  static_cast<std::uint32_t>(i) * 0x9E3779B9u);
                const float x = bounds.X + UnitHash(hash) * bounds.Width;
                const float y = bounds.Y + UnitHash(hash ^ 0xB5297A4Du) * bounds.Height;
                const float twinkle = 0.45f + 0.55f * UnitHash(hash ^ frame);
                const float size = (1.0f + UnitHash(hash ^ 0x68E31DA4u) * 2.5f) * twinkle;
                graphics.FillEllipse((hash & 1u) ? &primaryBrush : &secondaryBrush,
                                     x - size / 2, y - size / 2, size, size);
            }
            break;
        }
        case BackdropPattern::Circuit: {
            const float offset = shift;
            int row = 0;
            for (float y = bounds.Y - spacing + offset; y < bounds.GetBottom(); y += spacing, ++row) {
                int column = 0;
                for (float x = bounds.X - spacing + offset; x < bounds.GetRight(); x += spacing, ++column) {
                    const std::uint32_t hash = Hash32(static_cast<std::uint32_t>(g_theme.backdropSeed) ^
                        static_cast<std::uint32_t>(row * 4099 + column));
                    if (hash & 1u) graphics.DrawLine(&primaryPen, x, y, x + spacing, y);
                    if (hash & 2u) graphics.DrawLine(&secondaryPen, x, y, x, y + spacing);
                    if ((hash & 7u) == 0) {
                        graphics.FillEllipse(&primaryBrush, x - 2.0f, y - 2.0f, 4.0f, 4.0f);
                    }
                }
            }
            break;
        }
        case BackdropPattern::Scanlines: {
            const int lineSpacing = (std::max)(3, spacing / 4);
            const float lineShift = std::fmod(phase * lineSpacing, static_cast<float>(lineSpacing));
            for (float y = bounds.Y - lineSpacing + lineShift; y < bounds.GetBottom(); y += lineSpacing) {
                graphics.DrawLine(&primaryPen, bounds.X, y, bounds.GetRight(), y);
            }
            break;
        }
        case BackdropPattern::Noise: {
            const std::uint32_t frame = static_cast<std::uint32_t>(phase * 10.0f);
            PaintNoise(graphics, bounds, opacity, static_cast<std::uint32_t>(g_theme.backdropSeed) ^ frame);
            break;
        }
        case BackdropPattern::Mesh: {
            const float meshSpacing = static_cast<float>((std::max)(12, spacing));
            for (float y = bounds.Y - meshSpacing + shift; y < bounds.GetBottom() + meshSpacing; y += meshSpacing) {
                for (float x = bounds.X - meshSpacing; x < bounds.GetRight() + meshSpacing; x += meshSpacing) {
                    graphics.DrawLine(&primaryPen, x, y, x + meshSpacing, y + meshSpacing);
                    graphics.DrawLine(&secondaryPen, x + meshSpacing, y, x, y + meshSpacing);
                    graphics.FillEllipse(&primaryBrush, x - thickness, y - thickness,
                                         thickness * 2.0f, thickness * 2.0f);
                }
            }
            break;
        }
        default:
            break;
    }
}

void PaintVignette(Gdiplus::Graphics& graphics, const Gdiplus::RectF& bounds) {
    if (g_theme.vignetteOpacityPercent <= 0) return;
    Gdiplus::GraphicsPath path;
    path.AddRectangle(bounds);
    Gdiplus::PathGradientBrush brush(&path);
    brush.SetCenterPoint(Gdiplus::PointF(
        bounds.X + bounds.Width * g_theme.gradientCenterXPercent / 100.0f,
        bounds.Y + bounds.Height * g_theme.gradientCenterYPercent / 100.0f));
    brush.SetCenterColor(FromColorRefRaw(ThemeColor(ThemeRole::Vignette), 0));
    Gdiplus::Color edge[1] = {
        FromColorRefRaw(ThemeColor(ThemeRole::Vignette), PercentAlpha(g_theme.vignetteOpacityPercent))};
    INT count = 1;
    brush.SetSurroundColors(edge, &count);
    brush.SetFocusScales(0.42f, 0.42f);
    graphics.FillRectangle(&brush, bounds);
}
}  // namespace

bool SetThemeBackgroundImage(const std::wstring& path, int opacityPercent, BackgroundImageMode mode) {
    ThemeBackgroundImageOptions options;
    options.opacityPercent = opacityPercent;
    options.mode = mode;
    return SetThemeBackgroundImage(path, options);
}

bool SetThemeBackgroundImage(const std::wstring& path, int opacityPercent, BackgroundImageMode mode,
                             int positionXPercent, int positionYPercent, int scalePercent,
                             COLORREF tint, int tintOpacityPercent,
                             int brightnessPercent, int saturationPercent) {
    ThemeBackgroundImageOptions options;
    options.opacityPercent = opacityPercent;
    options.mode = mode;
    options.positionXPercent = positionXPercent;
    options.positionYPercent = positionYPercent;
    options.scalePercent = scalePercent;
    options.tint = tint;
    options.tintOpacityPercent = tintOpacityPercent;
    options.brightnessPercent = brightnessPercent;
    options.saturationPercent = saturationPercent;
    return SetThemeBackgroundImage(path, options);
}

bool SetThemeBackgroundImage(const std::wstring& path, const ThemeBackgroundImageOptions& options) {
    ResetThemeBackgroundImage();
    auto image = std::make_unique<Gdiplus::Image>(path.c_str(), FALSE);
    if (!image || image->GetLastStatus() != Gdiplus::Ok || image->GetWidth() == 0 || image->GetHeight() == 0) {
        return false;
    }
    constexpr std::uint64_t kMaximumDecodedPixels = 64ULL * 1024ULL * 1024ULL;
    constexpr UINT kMaximumDimension = 16384;
    const UINT width = image->GetWidth();
    const UINT height = image->GetHeight();
    if (width > kMaximumDimension || height > kMaximumDimension ||
        static_cast<std::uint64_t>(width) * height > kMaximumDecodedPixels) return false;
    g_backgroundImageOptions = NormalizeImageOptions(options);
    g_backgroundImage = std::move(image);
    return true;
}

bool SetThemeBackgroundImage(const std::vector<std::uint8_t>& encodedBytes,
                             const ThemeBackgroundImageOptions& options) {
    ResetThemeBackgroundImage();
    constexpr std::size_t kMaximumEncodedBytes = 16ULL * 1024ULL * 1024ULL;
    if (encodedBytes.empty() || encodedBytes.size() > kMaximumEncodedBytes) return false;

    HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, encodedBytes.size());
    if (!storage) return false;
    void* destination = GlobalLock(storage);
    if (!destination) {
        GlobalFree(storage);
        return false;
    }
    std::memcpy(destination, encodedBytes.data(), encodedBytes.size());
    GlobalUnlock(storage);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(storage, TRUE, &stream)) || !stream) {
        GlobalFree(storage);
        return false;
    }
    auto source = std::make_unique<Gdiplus::Image>(stream, FALSE);
    constexpr std::uint64_t kMaximumDecodedPixels = 64ULL * 1024ULL * 1024ULL;
    constexpr UINT kMaximumDimension = 16384;
    const UINT width = source ? source->GetWidth() : 0;
    const UINT height = source ? source->GetHeight() : 0;
    if (!source || source->GetLastStatus() != Gdiplus::Ok || width == 0 || height == 0 ||
        width > kMaximumDimension || height > kMaximumDimension ||
        static_cast<std::uint64_t>(width) * height > kMaximumDecodedPixels) {
        source.reset();
        stream->Release();
        return false;
    }

    // GDI+ requires the source stream to outlive an Image created from it. Draw
    // into an owned bitmap while the stream is alive, then retain only that fully
    // decoded snapshot.
    auto decoded = std::make_unique<Gdiplus::Bitmap>(width, height, PixelFormat32bppPARGB);
    if (!decoded || decoded->GetLastStatus() != Gdiplus::Ok) {
        source.reset();
        stream->Release();
        return false;
    }
    Gdiplus::Graphics graphics(decoded.get());
    const Gdiplus::Status drawn = graphics.DrawImage(source.get(), 0, 0, width, height);
    source.reset();
    stream->Release();
    if (drawn != Gdiplus::Ok) return false;

    g_backgroundImageOptions = NormalizeImageOptions(options);
    g_backgroundImage = std::move(decoded);
    return true;
}

void ResetThemeBackgroundImage() {
    g_backgroundImage.reset();
    g_backgroundImageOptions = {};
    g_backgroundImageOptions.opacityPercent = 0;
}

void PaintThemeBackdrop(HDC dc, RECT rect, ThemeSurface surface, std::uint64_t timestampMs) {
    if (!dc || !g_theme.active || !SurfaceInScope(surface, g_theme.backdropScope)) return;
    const float width = static_cast<float>(rect.right - rect.left);
    const float height = static_cast<float>(rect.bottom - rect.top);
    if (width <= 0.0f || height <= 0.0f) return;
    if (timestampMs == 0) timestampMs = static_cast<std::uint64_t>(GetTickCount64());

    Canvas canvas(dc);
    if (!canvas.ok()) return;
    const Gdiplus::RectF bounds(static_cast<float>(rect.left), static_cast<float>(rect.top), width, height);
    const Gdiplus::GraphicsState state = canvas.g().Save();
    canvas.g().SetClip(bounds);
    PaintBase(canvas.g(), bounds, surface);
    PaintBackgroundImage(canvas.g(), bounds);
    PaintPattern(canvas.g(), bounds, timestampMs);
    if (g_theme.noiseOpacityPercent > 0 && g_theme.backdropPattern != BackdropPattern::Noise) {
        const std::uint32_t frame = IsThemeBackdropAnimated()
            ? static_cast<std::uint32_t>(timestampMs / 100)
            : 0u;
        PaintNoise(canvas.g(), bounds, g_theme.noiseOpacityPercent,
                   static_cast<std::uint32_t>(g_theme.backdropSeed) ^ frame);
    }
    PaintVignette(canvas.g(), bounds);
    canvas.g().Restore(state);
}

ScopedThemeBypass::ScopedThemeBypass() { ++g_themeBypassDepth; }
ScopedThemeBypass::~ScopedThemeBypass() { --g_themeBypassDepth; }
ScopedTranslationBypass::ScopedTranslationBypass() { ++g_translationBypassDepth; }
ScopedTranslationBypass::~ScopedTranslationBypass() { --g_translationBypassDepth; }

void ShutdownGraphics() {
    if (g_token) {
        Gdiplus::GdiplusShutdown(g_token);
        g_token = 0;
    }
}

Gdiplus::Color FromColorRef(COLORREF color, BYTE alpha) {
    color = ResolveThemeColor(color);
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

Gdiplus::Color FromColorRefRaw(COLORREF color, BYTE alpha) {
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

COLORREF MixColor(COLORREF a, COLORREF b, float t) {
    t = (std::max)(0.0f, (std::min)(1.0f, t));
    const auto mix = [t](BYTE x, BYTE y) { return static_cast<BYTE>(x + (y - x) * t); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

Canvas::Canvas(HDC dc) {
    graphics_ = std::make_unique<Gdiplus::Graphics>(dc);
    if (graphics_->GetLastStatus() != Gdiplus::Ok) {
        graphics_.reset();
        return;
    }
    graphics_->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics_->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics_->SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
}

Canvas::~Canvas() {
    if (graphics_) graphics_->Flush(Gdiplus::FlushIntentionSync);
}

void FillRoundRect(HDC dc, RECT rect, int radius, COLORREF color, BYTE alpha) {
    Canvas canvas(dc);
    if (!canvas.ok()) return;
    Gdiplus::RectF r(static_cast<float>(rect.left), static_cast<float>(rect.top),
                     static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top));
    Gdiplus::GraphicsPath path;
    BuildRoundRectPath(path, r, static_cast<float>(ResolveThemeRadius(radius)));
    Gdiplus::SolidBrush brush(FromColorRef(color, alpha));
    canvas.g().FillPath(&brush, &path);
}

void StrokeRoundRect(HDC dc, RECT rect, int radius, COLORREF color, int width, BYTE alpha) {
    Canvas canvas(dc);
    if (!canvas.ok()) return;
    width = ResolveThemeStrokeWidth(width);
    if (width <= 0) return;
    const float inset = width / 2.0f;
    Gdiplus::RectF r(rect.left + inset, rect.top + inset,
                     (rect.right - rect.left) - static_cast<float>(width),
                     (rect.bottom - rect.top) - static_cast<float>(width));
    Gdiplus::GraphicsPath path;
    BuildRoundRectPath(path, r, (std::max)(0.0f, ResolveThemeRadius(radius) - inset));
    Gdiplus::Pen pen(FromColorRef(color, alpha), static_cast<float>(width));
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    canvas.g().DrawPath(&pen, &path);
}

void FillEllipse(HDC dc, RECT rect, COLORREF color, BYTE alpha) {
    Canvas canvas(dc);
    if (!canvas.ok()) return;
    Gdiplus::SolidBrush brush(FromColorRef(color, alpha));
    canvas.g().FillEllipse(&brush, static_cast<float>(rect.left), static_cast<float>(rect.top),
                           static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top));
}

void StrokeEllipse(HDC dc, RECT rect, COLORREF color, int width, BYTE alpha) {
    Canvas canvas(dc);
    if (!canvas.ok()) return;
    width = ResolveThemeStrokeWidth(width);
    if (width <= 0) return;
    const float inset = width / 2.0f;
    Gdiplus::Pen pen(FromColorRef(color, alpha), static_cast<float>(width));
    canvas.g().DrawEllipse(&pen, rect.left + inset, rect.top + inset,
                           (rect.right - rect.left) - static_cast<float>(width),
                           (rect.bottom - rect.top) - static_cast<float>(width));
}

void FillRectAlpha(HDC dc, RECT rect, COLORREF color, BYTE alpha) {
    Canvas canvas(dc);
    if (!canvas.ok()) return;
    Gdiplus::SolidBrush brush(FromColorRef(color, alpha));
    canvas.g().FillRectangle(&brush, static_cast<float>(rect.left), static_cast<float>(rect.top),
                             static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top));
}

void FillRoundRectVGradient(HDC dc, RECT rect, int radius, COLORREF top, COLORREF bottom, BYTE alpha) {
    Canvas canvas(dc);
    if (!canvas.ok()) return;
    Gdiplus::RectF r(static_cast<float>(rect.left), static_cast<float>(rect.top),
                     static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top));
    Gdiplus::GraphicsPath path;
    BuildRoundRectPath(path, r, static_cast<float>(ResolveThemeRadius(radius)));
    Gdiplus::RectF fill(r.X, r.Y - 1.0f, r.Width, r.Height + 2.0f);
    Gdiplus::LinearGradientBrush brush(fill, FromColorRef(top, alpha), FromColorRef(bottom, alpha),
                                       Gdiplus::LinearGradientModeVertical);
    canvas.g().FillPath(&brush, &path);
}

HBITMAP CreateLayeredDib(int width, int height, void** pixels) {
    if (!pixels || width <= 0 || height <= 0) return nullptr;
    *pixels = nullptr;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // Top-down, matching the window coordinate system.
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, pixels, nullptr, 0);
}

void MakeLayeredSurfaceOpaque(void* pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0) return;
    auto* bgra = static_cast<std::uint32_t*>(pixels);
    const std::size_t count = static_cast<std::size_t>(width) * height;
    // DWORD OR is friendly to the compiler's vectorizer and preserves BGR while
    // repairing the reserved byte that legacy GDI operations leave undefined.
    for (std::size_t i = 0; i < count; ++i) bgra[i] |= 0xFF000000u;
}

namespace {
int ClampRoundedRadius(int width, int height, int radius) {
    if (width <= 0 || height <= 0) return 0;
    return (std::max)(0, (std::min)(radius, (std::min)(width, height) / 2));
}

const std::vector<BYTE>& RoundedCornerCoverageMask(int radius) {
    static std::mutex cacheMutex;
    static std::unordered_map<int, std::vector<BYTE>> cache;
    const std::scoped_lock lock(cacheMutex);
    auto [it, inserted] = cache.try_emplace(radius);
    if (!inserted) return it->second;

    auto& mask = it->second;
    mask.resize(static_cast<std::size_t>(radius) * radius);
    constexpr int kSamples = 4;
    for (int y = 0; y < radius; ++y) {
        for (int x = 0; x < radius; ++x) {
            int inside = 0;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    const float px = static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / kSamples;
                    const float py = static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / kSamples;
                    const float dx = px - static_cast<float>(radius);
                    const float dy = py - static_cast<float>(radius);
                    if (dx * dx + dy * dy <= static_cast<float>(radius * radius)) ++inside;
                }
            }
            mask[static_cast<std::size_t>(y) * radius + x] =
                static_cast<BYTE>((inside * 255 + (kSamples * kSamples) / 2) / (kSamples * kSamples));
        }
    }
    return mask;
}

void ApplyRoundedCoverage(std::uint8_t* pixel, BYTE alpha) {
    // UpdateLayeredWindow expects premultiplied BGRA. The back buffer is opaque,
    // so only corner coverage needs to be folded into the color channels.
    if (alpha == 0) {
        pixel[0] = pixel[1] = pixel[2] = 0;
    } else if (alpha != 255) {
        pixel[0] = static_cast<BYTE>((static_cast<unsigned>(pixel[0]) * alpha + 127) / 255);
        pixel[1] = static_cast<BYTE>((static_cast<unsigned>(pixel[1]) * alpha + 127) / 255);
        pixel[2] = static_cast<BYTE>((static_cast<unsigned>(pixel[2]) * alpha + 127) / 255);
    }
    pixel[3] = alpha;
}
}  // namespace

BYTE RoundedRectPixelCoverage(int x, int y, int width, int height, int radius) {
    if (x < 0 || y < 0 || x >= width || y >= height) return 0;
    radius = ClampRoundedRadius(width, height, radius);
    if (radius == 0 || (x >= radius && x < width - radius) ||
        (y >= radius && y < height - radius)) {
        return 255;
    }

    const int cornerX = x < radius ? x : width - 1 - x;
    const int cornerY = y < radius ? y : height - 1 - y;
    const auto& mask = RoundedCornerCoverageMask(radius);
    return mask[static_cast<std::size_t>(cornerY) * radius + cornerX];
}

bool PresentRoundedLayeredWindow(HWND window, HDC source, void* pixels,
                                 int width, int height, int radius, BYTE opacity) {
    if (!window || !source || !pixels || width <= 0 || height <= 0) return false;

    // DrawText/FillRect can clear the reserved alpha byte even when the surface
    // was made opaque before GDI+ composition. Repair that byte with one tight,
    // vectorizable pass; all expensive coverage/premultiplication below remains
    // restricted to the cached corner squares.
    MakeLayeredSurfaceOpaque(pixels, width, height);
    radius = ClampRoundedRadius(width, height, ResolveThemeRadius(radius));
    auto* bgra = static_cast<std::uint8_t*>(pixels);
    if (radius > 0) {
        const auto& mask = RoundedCornerCoverageMask(radius);
        const auto apply = [&](int x, int y, BYTE alpha) {
            std::uint8_t* pixel = bgra + (static_cast<std::size_t>(y) * width + x) * 4;
            ApplyRoundedCoverage(pixel, alpha);
        };
        for (int y = 0; y < radius; ++y) {
            for (int x = 0; x < radius; ++x) {
                const BYTE alpha = mask[static_cast<std::size_t>(y) * radius + x];
                apply(x, y, alpha);
                apply(width - 1 - x, y, alpha);
                apply(x, height - 1 - y, alpha);
                apply(width - 1 - x, height - 1 - y, alpha);
            }
        }
    }

    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return false;
    POINT destination{bounds.left, bounds.top};
    POINT origin{0, 0};
    SIZE size{width, height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA};
    HDC screen = GetDC(nullptr);
    const BOOL updated = UpdateLayeredWindow(window, screen, &destination, &size, source, &origin,
                                             0, &blend, ULW_ALPHA);
    if (screen) ReleaseDC(nullptr, screen);
    return updated != FALSE;
}

bool PointInRoundedRect(POINT point, int width, int height, int radius) {
    if (point.x < 0 || point.y < 0 || point.x >= width || point.y >= height) return false;
    return RoundedRectPixelCoverage(point.x, point.y, width, height, ResolveThemeRadius(radius)) >= 128;
}

}  // namespace ui
