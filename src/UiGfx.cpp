#include "UiGfx.hpp"

#pragma comment(lib, "gdiplus.lib")

#include <cstdint>

namespace ui {
namespace {
ULONG_PTR g_token = 0;

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

void ShutdownGraphics() {
    if (g_token) {
        Gdiplus::GdiplusShutdown(g_token);
        g_token = 0;
    }
}

Gdiplus::Color FromColorRef(COLORREF color, BYTE alpha) {
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
    BuildRoundRectPath(path, r, static_cast<float>(radius));
    Gdiplus::SolidBrush brush(FromColorRef(color, alpha));
    canvas.g().FillPath(&brush, &path);
}

void StrokeRoundRect(HDC dc, RECT rect, int radius, COLORREF color, int width, BYTE alpha) {
    Canvas canvas(dc);
    if (!canvas.ok()) return;
    const float inset = width / 2.0f;
    Gdiplus::RectF r(rect.left + inset, rect.top + inset,
                     (rect.right - rect.left) - static_cast<float>(width),
                     (rect.bottom - rect.top) - static_cast<float>(width));
    Gdiplus::GraphicsPath path;
    BuildRoundRectPath(path, r, (std::max)(0.0f, radius - inset));
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
    BuildRoundRectPath(path, r, static_cast<float>(radius));
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
    auto* bgra = static_cast<std::uint8_t*>(pixels);
    const std::size_t count = static_cast<std::size_t>(width) * height;
    for (std::size_t i = 0; i < count; ++i) bgra[i * 4 + 3] = 255;
}

namespace {
BYTE RoundedCoverage(int x, int y, int width, int height, int radius) {
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
}  // namespace

bool PresentRoundedLayeredWindow(HWND window, HDC source, void* pixels,
                                 int width, int height, int radius, BYTE opacity) {
    if (!window || !source || !pixels || width <= 0 || height <= 0) return false;

    auto* bgra = static_cast<std::uint8_t*>(pixels);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint8_t* pixel = bgra + (static_cast<std::size_t>(y) * width + x) * 4;
            const BYTE alpha = RoundedCoverage(x, y, width, height, radius);
            // UpdateLayeredWindow expects premultiplied BGRA. All UI is first
            // rendered against an opaque surface, so only edge coverage needs
            // to be folded into the color channels here.
            if (alpha == 0) {
                pixel[0] = pixel[1] = pixel[2] = 0;
            } else if (alpha != 255) {
                pixel[0] = static_cast<BYTE>((static_cast<unsigned>(pixel[0]) * alpha + 127) / 255);
                pixel[1] = static_cast<BYTE>((static_cast<unsigned>(pixel[1]) * alpha + 127) / 255);
                pixel[2] = static_cast<BYTE>((static_cast<unsigned>(pixel[2]) * alpha + 127) / 255);
            }
            pixel[3] = alpha;
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
    return RoundedCoverage(point.x, point.y, width, height, radius) >= 128;
}

}  // namespace ui
