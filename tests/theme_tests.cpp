#include "UiGfx.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

void Expect(bool condition, std::string_view message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAILED: " << message << '\n';
}

class DibSurface {
public:
    DibSurface(int width, int height) : width_(width), height_(height) {
        void* bits = nullptr;
        bitmap_ = ui::CreateLayeredDib(width_, height_, &bits);
        pixels_ = static_cast<std::uint32_t*>(bits);
        dc_ = CreateCompatibleDC(nullptr);
        if (dc_ && bitmap_) previous_ = SelectObject(dc_, bitmap_);
    }

    ~DibSurface() {
        if (dc_ && previous_ && previous_ != HGDI_ERROR) SelectObject(dc_, previous_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc_) DeleteDC(dc_);
    }

    DibSurface(const DibSurface&) = delete;
    DibSurface& operator=(const DibSurface&) = delete;

    bool ok() const {
        return dc_ && bitmap_ && pixels_ && previous_ && previous_ != HGDI_ERROR;
    }

    HDC dc() const { return dc_; }
    void* pixels() const { return pixels_; }
    int width() const { return width_; }
    int height() const { return height_; }

    RECT bounds() const { return RECT{0, 0, width_, height_}; }

    void Clear(std::uint32_t pixel) {
        GdiFlush();
        std::fill_n(pixels_, static_cast<std::size_t>(width_) * height_, pixel);
    }

    std::vector<std::uint32_t> Snapshot() const {
        GdiFlush();
        return {pixels_, pixels_ + static_cast<std::size_t>(width_) * height_};
    }

private:
    int width_ = 0;
    int height_ = 0;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    std::uint32_t* pixels_ = nullptr;
};

bool IsNonUniform(const std::vector<std::uint32_t>& pixels) {
    if (pixels.empty()) return false;
    const std::uint32_t first = pixels.front() & 0x00FFFFFFu;
    return std::any_of(pixels.begin() + 1, pixels.end(), [first](std::uint32_t pixel) {
        return (pixel & 0x00FFFFFFu) != first;
    });
}

BYTE LegacyRoundedCoverage(int x, int y, int width, int height, int radius) {
    radius = (std::max)(0, (std::min)(radius, (std::min)(width, height) / 2));
    if (radius == 0 || (x >= radius && x < width - radius) ||
        (y >= radius && y < height - radius)) {
        return 255;
    }

    const float centerX = x < radius ? static_cast<float>(radius) : static_cast<float>(width - radius);
    const float centerY = y < radius ? static_cast<float>(radius) : static_cast<float>(height - radius);
    constexpr int kSamples = 4;
    int inside = 0;
    for (int sy = 0; sy < kSamples; ++sy) {
        for (int sx = 0; sx < kSamples; ++sx) {
            const float px = static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / kSamples;
            const float py = static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / kSamples;
            const float dx = px - centerX;
            const float dy = py - centerY;
            if (dx * dx + dy * dy <= static_cast<float>(radius * radius)) ++inside;
        }
    }
    return static_cast<BYTE>((inside * 255 + (kSamples * kSamples) / 2) / (kSamples * kSamples));
}

void TestRoundedCoverageCacheEquivalence() {
    struct SizeCase { int width; int height; };
    constexpr std::array<SizeCase, 12> sizes{{
        {1, 1}, {2, 3}, {3, 2}, {4, 4}, {5, 7}, {8, 5},
        {13, 17}, {22, 22}, {31, 19}, {48, 64}, {64, 48}, {81, 53},
    }};
    constexpr std::array<int, 10> radii{{-3, 0, 1, 2, 3, 7, 11, 16, 22, 100}};

    for (const auto size : sizes) {
        for (const int radius : radii) {
            bool exact = true;
            for (int y = 0; y < size.height && exact; ++y) {
                for (int x = 0; x < size.width; ++x) {
                    if (ui::RoundedRectPixelCoverage(x, y, size.width, size.height, radius) !=
                        LegacyRoundedCoverage(x, y, size.width, size.height, radius)) {
                        exact = false;
                        break;
                    }
                }
            }
            Expect(exact, "cached rounded coverage exactly matches the legacy supersampling mask");
        }
    }
    Expect(ui::RoundedRectPixelCoverage(-1, 0, 64, 48, 16) == 0 &&
           ui::RoundedRectPixelCoverage(64, 0, 64, 48, 16) == 0,
           "rounded coverage rejects pixels outside the supplied bounds");
}

void TestOpaqueAlphaRepair(DibSurface& surface) {
    surface.Clear(0x00123456u);
    // Exercise the real DIB through a GDI fill, which leaves its reserved alpha
    // byte undefined, then verify the repair preserves every BGR channel.
    HBRUSH brush = CreateSolidBrush(RGB(0x56, 0x34, 0x12));
    const RECT bounds = surface.bounds();
    FillRect(surface.dc(), &bounds, brush);
    DeleteObject(brush);
    const auto before = surface.Snapshot();
    ui::MakeLayeredSurfaceOpaque(surface.pixels(), surface.width(), surface.height());
    const auto after = surface.Snapshot();
    bool exact = before.size() == after.size();
    for (std::size_t i = 0; i < before.size() && exact; ++i) {
        exact = (after[i] & 0x00FFFFFFu) == (before[i] & 0x00FFFFFFu) &&
                (after[i] & 0xFF000000u) == 0xFF000000u;
    }
    Expect(exact, "opaque alpha repair preserves BGR and restores alpha after GDI drawing");
}

void TestExactThemeRoles() {
    ui::ThemePalette palette;
    palette.active = true;
    palette.saturationPercent = 100;
    palette.contrastPercent = 100;
    palette.brightnessPercent = 100;

    struct RoleCase {
        ui::ThemeRole role;
        COLORREF expected;
        const char* name;
    };
    std::vector<RoleCase> roles;
    roles.reserve(30);
    unsigned index = 0;
    const auto assign = [&](COLORREF& destination, ui::ThemeRole role, const char* name) {
        destination = RGB(11 + (index * 37) % 220,
                          17 + (index * 61) % 215,
                          23 + (index * 83) % 210);
        ++index;
        roles.push_back({role, destination, name});
    };

    assign(palette.background, ui::ThemeRole::Background, "Background");
    assign(palette.backgroundSecondary, ui::ThemeRole::BackgroundSecondary, "BackgroundSecondary");
    assign(palette.sidebar, ui::ThemeRole::Sidebar, "Sidebar");
    assign(palette.topbar, ui::ThemeRole::Topbar, "Topbar");
    assign(palette.surface, ui::ThemeRole::Surface, "Surface");
    assign(palette.surfaceRaised, ui::ThemeRole::SurfaceRaised, "SurfaceRaised");
    assign(palette.surfaceHover, ui::ThemeRole::SurfaceHover, "SurfaceHover");
    assign(palette.surfacePressed, ui::ThemeRole::SurfacePressed, "SurfacePressed");
    assign(palette.surfaceSelected, ui::ThemeRole::SurfaceSelected, "SurfaceSelected");
    assign(palette.input, ui::ThemeRole::Input, "Input");
    assign(palette.border, ui::ThemeRole::Border, "Border");
    assign(palette.borderHover, ui::ThemeRole::BorderHover, "BorderHover");
    assign(palette.focus, ui::ThemeRole::Focus, "Focus");
    assign(palette.text, ui::ThemeRole::Text, "Text");
    assign(palette.textSecondary, ui::ThemeRole::TextSecondary, "TextSecondary");
    assign(palette.muted, ui::ThemeRole::Muted, "Muted");
    assign(palette.disabled, ui::ThemeRole::Disabled, "Disabled");
    assign(palette.accent, ui::ThemeRole::Accent, "Accent");
    assign(palette.accentHover, ui::ThemeRole::AccentHover, "AccentHover");
    assign(palette.accentPressed, ui::ThemeRole::AccentPressed, "AccentPressed");
    assign(palette.accentSecondary, ui::ThemeRole::AccentSecondary, "AccentSecondary");
    assign(palette.textOnAccent, ui::ThemeRole::TextOnAccent, "TextOnAccent");
    assign(palette.info, ui::ThemeRole::Info, "Info");
    assign(palette.success, ui::ThemeRole::Success, "Success");
    assign(palette.warning, ui::ThemeRole::Warning, "Warning");
    assign(palette.danger, ui::ThemeRole::Danger, "Danger");
    assign(palette.shadow, ui::ThemeRole::Shadow, "Shadow");
    assign(palette.backdropColor, ui::ThemeRole::BackdropColor, "BackdropColor");
    assign(palette.backdropColorSecondary, ui::ThemeRole::BackdropColorSecondary,
           "BackdropColorSecondary");
    assign(palette.vignetteColor, ui::ThemeRole::Vignette, "Vignette");

    ui::SetThemePalette(palette);
    for (const auto& role : roles) {
        Expect(ui::ThemeColor(role.role) == role.expected,
               std::string("ThemeColor exact role mapping: ") + role.name);
    }
}

void TestPaletteClamping() {
    ui::ThemePalette palette;
    palette.active = true;
    palette.appearance = static_cast<ui::ThemeAppearance>(99);
    palette.cornerScalePercent = -20;
    palette.borderScalePercent = 999;
    palette.fontScalePercent = 1;
    palette.fontWeight = -10;
    palette.fontFamily.assign(50, L'F');
    palette.saturationPercent = -1;
    palette.contrastPercent = -1;
    palette.brightnessPercent = 999;
    palette.motionScalePercent = -1;
    palette.backdropPattern = static_cast<ui::BackdropPattern>(99);
    palette.gradientStyle = static_cast<ui::GradientStyle>(99);
    palette.backdropScope = static_cast<ui::BackdropScope>(99);
    palette.backdropOpacityPercent = 999;
    palette.backdropScale = 0;
    palette.backdropThickness = 999;
    palette.backdropSpeedPercent = 999;
    palette.backdropSeed = -1;
    palette.gradientAngle = -721;
    palette.gradientCenterXPercent = -1;
    palette.gradientCenterYPercent = 999;
    palette.gradientSpreadPercent = 999;
    palette.vignetteOpacityPercent = 999;
    palette.noiseOpacityPercent = 999;
    palette.backgroundImagePositionXPercent = -1;
    palette.backgroundImagePositionYPercent = 999;
    palette.backgroundImageScalePercent = 1;
    palette.backgroundImageTintOpacityPercent = 999;
    palette.backgroundImageBrightnessPercent = 0;
    palette.backgroundImageSaturationPercent = 999;

    ui::SetThemePalette(palette);
    const ui::ThemePalette actual = ui::CurrentThemePalette();
    Expect(actual.active, "SetThemePalette preserves active state");
    Expect(actual.appearance == ui::ThemeAppearance::Dark, "invalid appearance clamps to Dark");
    Expect(actual.cornerScalePercent == 50, "corner scale lower clamp");
    Expect(actual.borderScalePercent == 250, "border scale upper clamp");
    Expect(actual.fontScalePercent == 75, "font scale lower clamp");
    Expect(actual.fontWeight == 100, "font weight lower clamp");
    Expect(actual.fontFamily.size() == 31, "font family length clamp");
    Expect(actual.saturationPercent == 0, "saturation lower clamp");
    Expect(actual.contrastPercent == 50, "contrast lower clamp");
    Expect(actual.brightnessPercent == 200, "brightness upper clamp");
    Expect(actual.motionScalePercent == 0, "motion lower clamp");
    Expect(actual.backdropPattern == ui::BackdropPattern::Solid, "invalid pattern clamps to Solid");
    Expect(actual.gradientStyle == ui::GradientStyle::Linear, "invalid gradient style clamps to Linear");
    Expect(actual.backdropScope == ui::BackdropScope::All, "invalid scope clamps to All");
    Expect(actual.backdropOpacityPercent == 100, "backdrop opacity upper clamp");
    Expect(actual.backdropScale == 8, "backdrop scale lower clamp");
    Expect(actual.backdropThickness == 8, "backdrop thickness upper clamp");
    Expect(actual.backdropSpeedPercent == 200, "backdrop speed upper clamp");
    Expect(actual.backdropSeed == 0, "backdrop seed lower clamp");
    Expect(actual.gradientAngle == 359, "negative gradient angle normalizes");
    Expect(actual.gradientCenterXPercent == 0 && actual.gradientCenterYPercent == 100,
           "gradient center clamps");
    Expect(actual.gradientSpreadPercent == 200, "gradient spread upper clamp");
    Expect(actual.vignetteOpacityPercent == 90, "vignette opacity upper clamp");
    Expect(actual.noiseOpacityPercent == 40, "noise opacity upper clamp");
    Expect(actual.backgroundImagePositionXPercent == 0 &&
           actual.backgroundImagePositionYPercent == 100, "image position clamps");
    Expect(actual.backgroundImageScalePercent == 25, "image scale lower clamp");
    Expect(actual.backgroundImageTintOpacityPercent == 100, "image tint opacity upper clamp");
    Expect(actual.backgroundImageBrightnessPercent == 25, "image brightness lower clamp");
    Expect(actual.backgroundImageSaturationPercent == 200, "image saturation upper clamp");

    palette = {};
    palette.active = true;
    palette.fontWeight = 0;
    ui::SetThemePalette(palette);
    Expect(ui::CurrentThemePalette().fontWeight == 0, "zero font weight keeps host default");
}

void TestAppearanceState() {
    constexpr std::array<ui::ThemeAppearance, 3> appearances = {
        ui::ThemeAppearance::Dark,
        ui::ThemeAppearance::Light,
        ui::ThemeAppearance::Amoled,
    };
    for (std::size_t index = 0; index < appearances.size(); ++index) {
        ui::ThemePalette palette;
        palette.active = true;
        palette.appearance = appearances[index];
        palette.background = RGB(20 + static_cast<int>(index), 40, 60);
        ui::SetThemePalette(palette);
        const ui::ThemePalette current = ui::CurrentThemePalette();
        Expect(current.appearance == appearances[index], "Dark/Light/AMOLED appearance state");
        Expect(ui::ThemeColor(ui::ThemeRole::Background) == palette.background,
               "appearance preserves exact authored color");
    }
}

void TestMetricScaling() {
    ui::ThemePalette palette;
    palette.active = true;
    palette.cornerScalePercent = 150;
    palette.borderScalePercent = 250;
    palette.motionScalePercent = 75;
    ui::SetThemePalette(palette);

    Expect(ui::ResolveThemeRadius(10) == 15, "radius scaling");
    Expect(ui::ResolveThemeStrokeWidth(4) == 10, "stroke scaling");
    Expect(ui::ResolveThemeDuration(400) == 300, "duration scaling");
    Expect(ui::ResolveThemeRadius(0) == 0, "zero radius remains zero");
    Expect(ui::ResolveThemeStrokeWidth(0) == 0, "zero stroke remains zero");
    Expect(ui::ResolveThemeDuration(-5) == -5, "negative duration remains sentinel");

    palette.motionScalePercent = 0;
    ui::SetThemePalette(palette);
    Expect(ui::ResolveThemeDuration(400) == 0, "zero motion scale disables duration");
}

void TestBackdropScopes(DibSurface& surface) {
    constexpr std::uint32_t sentinel = 0xFFCC335Au;
    ui::ThemePalette palette;
    palette.active = true;
    palette.backdropPattern = ui::BackdropPattern::Solid;
    palette.background = RGB(8, 21, 37);
    palette.surface = RGB(51, 68, 85);
    palette.surfaceRaised = RGB(102, 119, 136);

    const auto renderChanges = [&](ui::BackdropScope scope, ui::ThemeSurface target) {
        palette.backdropScope = scope;
        ui::SetThemePalette(palette);
        surface.Clear(sentinel);
        const auto before = surface.Snapshot();
        ui::PaintThemeBackdrop(surface.dc(), surface.bounds(), target, 123456u);
        return surface.Snapshot() != before;
    };

    Expect(!renderChanges(ui::BackdropScope::Main, ui::ThemeSurface::Panel),
           "Main scope is a no-op for panels");
    Expect(renderChanges(ui::BackdropScope::Main, ui::ThemeSurface::Main),
           "Main scope paints main surface");
    Expect(!renderChanges(ui::BackdropScope::Panels, ui::ThemeSurface::Main),
           "Panels scope is a no-op for main surface");
    Expect(renderChanges(ui::BackdropScope::Panels, ui::ThemeSurface::Panel),
           "Panels scope paints panel surface");
    Expect(renderChanges(ui::BackdropScope::All, ui::ThemeSurface::Overlay),
           "All scope paints overlay surface");
}

void TestBackdropPatterns(DibSurface& surface) {
    struct PatternCase {
        ui::BackdropPattern pattern;
        const char* name;
    };
    constexpr std::array<PatternCase, 14> patterns = {{
        {ui::BackdropPattern::Gradient, "Gradient"},
        {ui::BackdropPattern::Grid, "Grid"},
        {ui::BackdropPattern::Dots, "Dots"},
        {ui::BackdropPattern::Aurora, "Aurora"},
        {ui::BackdropPattern::Diagonal, "Diagonal"},
        {ui::BackdropPattern::Waves, "Waves"},
        {ui::BackdropPattern::Rings, "Rings"},
        {ui::BackdropPattern::Checker, "Checker"},
        {ui::BackdropPattern::Hexagons, "Hexagons"},
        {ui::BackdropPattern::Stars, "Stars"},
        {ui::BackdropPattern::Circuit, "Circuit"},
        {ui::BackdropPattern::Scanlines, "Scanlines"},
        {ui::BackdropPattern::Noise, "Noise"},
        {ui::BackdropPattern::Mesh, "Mesh"},
    }};

    ui::ThemePalette palette;
    palette.active = true;
    palette.backdropScope = ui::BackdropScope::All;
    palette.background = RGB(4, 9, 19);
    palette.backgroundSecondary = RGB(92, 37, 133);
    palette.backdropColor = RGB(38, 210, 245);
    palette.backdropColorSecondary = RGB(244, 91, 187);
    palette.backdropOpacityPercent = 90;
    palette.backdropScale = 24;
    palette.backdropThickness = 3;
    palette.backdropSpeedPercent = 80;
    palette.backdropSeed = 4242;
    palette.gradientStyle = ui::GradientStyle::Linear;
    palette.gradientAngle = 37;
    palette.gradientCenterXPercent = 43;
    palette.gradientCenterYPercent = 58;
    palette.gradientSpreadPercent = 100;
    palette.noiseOpacityPercent = 0;
    palette.vignetteOpacityPercent = 0;

    constexpr std::uint64_t timestamp = 987654321u;
    const auto render = [&](ui::BackdropPattern pattern) {
        palette.backdropPattern = pattern;
        ui::SetThemePalette(palette);
        surface.Clear(0xFF010203u);
        ui::PaintThemeBackdrop(surface.dc(), surface.bounds(), ui::ThemeSurface::Main, timestamp);
        return surface.Snapshot();
    };

    const auto solid = render(ui::BackdropPattern::Solid);
    Expect(!IsNonUniform(solid), "Solid backdrop remains uniform");

    std::vector<std::uint32_t> gradientBaseline;
    for (const auto& test : patterns) {
        const auto first = render(test.pattern);
        const auto second = render(test.pattern);
        Expect(first == second, std::string(test.name) + " fixed-timestamp rendering is deterministic");
        Expect(IsNonUniform(first), std::string(test.name) + " rendering is non-uniform");
        if (test.pattern == ui::BackdropPattern::Gradient) {
            gradientBaseline = first;
        } else {
            Expect(first != gradientBaseline,
                   std::string(test.name) + " draws pattern content beyond the base gradient");
        }
    }

    palette.gradientStyle = ui::GradientStyle::Radial;
    palette.backdropPattern = ui::BackdropPattern::Gradient;
    ui::SetThemePalette(palette);
    surface.Clear(0xFF010203u);
    ui::PaintThemeBackdrop(surface.dc(), surface.bounds(), ui::ThemeSurface::Main, timestamp);
    const auto radial = surface.Snapshot();
    Expect(IsNonUniform(radial), "radial gradient rendering is non-uniform");
    Expect(radial != gradientBaseline, "radial and linear gradients render differently");
}

void TestAnimatedFlag() {
    ui::ResetThemePalette();
    Expect(!ui::IsThemeBackdropAnimated(), "inactive theme is not animated");

    ui::ThemePalette palette;
    palette.active = true;
    palette.backdropPattern = ui::BackdropPattern::Solid;
    palette.backdropSpeedPercent = 100;
    palette.motionScalePercent = 100;
    ui::SetThemePalette(palette);
    Expect(!ui::IsThemeBackdropAnimated(), "solid backdrop is not animated");

    palette.backdropPattern = ui::BackdropPattern::Grid;
    ui::SetThemePalette(palette);
    Expect(ui::IsThemeBackdropAnimated(), "moving non-solid backdrop is animated");

    palette.backdropSpeedPercent = 0;
    ui::SetThemePalette(palette);
    Expect(!ui::IsThemeBackdropAnimated(), "zero backdrop speed is not animated");

    palette.backdropSpeedPercent = 100;
    palette.motionScalePercent = 0;
    ui::SetThemePalette(palette);
    Expect(!ui::IsThemeBackdropAnimated(), "zero motion scale is not animated");
}

void TestResetBehavior(DibSurface& surface) {
    ui::ThemePalette palette;
    palette.active = true;
    palette.appearance = ui::ThemeAppearance::Amoled;
    palette.background = RGB(200, 10, 90);
    palette.cornerScalePercent = 150;
    palette.borderScalePercent = 200;
    palette.motionScalePercent = 50;
    palette.backdropPattern = ui::BackdropPattern::Grid;
    palette.backdropSpeedPercent = 100;
    ui::SetThemePalette(palette);
    ui::ResetThemePalette();

    const ui::ThemePalette reset = ui::CurrentThemePalette();
    Expect(!reset.active, "reset disables theme");
    Expect(reset.appearance == ui::ThemeAppearance::Dark, "reset restores Dark appearance default");
    Expect(reset.background == RGB(14, 16, 20), "reset restores default background");
    Expect(reset.cornerScalePercent == 100 && reset.borderScalePercent == 100 &&
           reset.motionScalePercent == 100, "reset restores metric defaults");
    Expect(ui::ThemeColor(ui::ThemeRole::Accent) == RGB(96, 149, 244),
           "reset restores exact default role colors");
    Expect(ui::ResolveThemeRadius(7) == 7 && ui::ResolveThemeStrokeWidth(3) == 3 &&
           ui::ResolveThemeDuration(250) == 250, "reset disables metric transforms");
    Expect(!ui::IsThemeBackdropAnimated(), "reset disables animation flag");

    surface.Clear(0xFF765432u);
    const auto before = surface.Snapshot();
    ui::PaintThemeBackdrop(surface.dc(), surface.bounds(), ui::ThemeSurface::Main, 123456u);
    Expect(surface.Snapshot() == before, "reset theme makes backdrop painting a no-op");
}

}  // namespace

int main() {
    if (!ui::InitGraphics()) {
        std::cerr << "FAILED: GDI+ initialization\n";
        return 1;
    }

    DibSurface surface(192, 128);
    if (!surface.ok()) {
        std::cerr << "FAILED: in-memory DIB/DC creation\n";
        ui::ShutdownGraphics();
        return 1;
    }

    TestRoundedCoverageCacheEquivalence();
    TestOpaqueAlphaRepair(surface);
    TestExactThemeRoles();
    TestPaletteClamping();
    TestAppearanceState();
    TestMetricScaling();
    TestBackdropScopes(surface);
    TestBackdropPatterns(surface);
    TestAnimatedFlag();
    TestResetBehavior(surface);

    ui::ResetThemePalette();
    ui::ShutdownGraphics();
    if (g_failures != 0) {
        std::cerr << g_failures << " theme renderer test(s) failed\n";
        return 1;
    }
    std::cout << "Theme renderer tests passed\n";
    return 0;
}
