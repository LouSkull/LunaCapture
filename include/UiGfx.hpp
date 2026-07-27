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
#include <memory>

// GDI+ headers reference std::min / std::max, but the project builds with
// NOMINMAX. Pull them into the Gdiplus namespace before including the header.
namespace Gdiplus {
using std::max;
using std::min;
}
#include <gdiplus.h>

namespace ui {

// Initialise / shut down the GDI+ runtime once for the whole process.
bool InitGraphics();
void ShutdownGraphics();

// Convert a Win32 COLORREF (0x00BBGGRR) to a GDI+ color with an alpha byte.
Gdiplus::Color FromColorRef(COLORREF color, BYTE alpha = 255);

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
