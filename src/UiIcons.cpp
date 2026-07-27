#include "UiIcons.hpp"
#include "UiGfx.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

// Every icon is designed on a 24x24 grid (Feather/Lucide style) and rendered
// with anti-aliased GDI+ strokes with round caps and joins, then scaled into the
// requested bounds. This replaces the old raw-GDI line drawing that produced the
// jagged, "pixel-crooked" icons.

namespace {
using namespace Gdiplus;

struct IconCtx {
    Graphics& g;
    float ox;
    float oy;
    float scale;
    Color color;
    float pen;

    PointF P(float gx, float gy) const { return PointF(ox + gx * scale, oy + gy * scale); }
    float S(float v) const { return v * scale; }
};

void Stroke(const IconCtx& c, GraphicsPath& path) {
    Pen pen(c.color, c.pen);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    c.g.DrawPath(&pen, &path);
}

void Line(const IconCtx& c, float x1, float y1, float x2, float y2) {
    Pen pen(c.color, c.pen);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    c.g.DrawLine(&pen, c.P(x1, y1), c.P(x2, y2));
}

void Polyline(const IconCtx& c, const std::vector<PointF>& pts, bool close) {
    GraphicsPath path;
    for (size_t i = 1; i < pts.size(); ++i) path.AddLine(pts[i - 1], pts[i]);
    if (close) path.CloseFigure();
    Stroke(c, path);
}

std::vector<PointF> Grid(const IconCtx& c, std::initializer_list<float> coords) {
    std::vector<PointF> pts;
    auto it = coords.begin();
    while (it != coords.end()) {
        const float x = *it++;
        const float y = (it != coords.end()) ? *it++ : 0.0f;
        pts.push_back(c.P(x, y));
    }
    return pts;
}

void RoundRect(const IconCtx& c, float x, float y, float w, float h, float r) {
    GraphicsPath path;
    const float d = r * 2.0f * c.scale;
    RectF rc(c.P(x, y).X, c.P(x, y).Y, c.S(w), c.S(h));
    path.AddArc(rc.X, rc.Y, d, d, 180, 90);
    path.AddArc(rc.X + rc.Width - d, rc.Y, d, d, 270, 90);
    path.AddArc(rc.X + rc.Width - d, rc.Y + rc.Height - d, d, d, 0, 90);
    path.AddArc(rc.X, rc.Y + rc.Height - d, d, d, 90, 90);
    path.CloseFigure();
    Stroke(c, path);
}

void Dot(const IconCtx& c, float gx, float gy, float gr) {
    SolidBrush brush(c.color);
    const PointF p = c.P(gx, gy);
    const float r = c.S(gr);
    c.g.FillEllipse(&brush, p.X - r, p.Y - r, r * 2, r * 2);
}

void FilledTriangle(const IconCtx& c, PointF a, PointF b, PointF d) {
    SolidBrush brush(c.color);
    PointF pts[3] = {a, b, d};
    c.g.FillPolygon(&brush, pts, 3);
}

void Arc(const IconCtx& c, float x, float y, float w, float h, float start, float sweep) {
    GraphicsPath path;
    path.AddArc(c.P(x, y).X, c.P(x, y).Y, c.S(w), c.S(h), start, sweep);
    Stroke(c, path);
}
}  // namespace

void DrawUiIcon(HDC dc, UiIcon icon, const RECT& bounds, COLORREF color, int stroke) {
    const int bw = bounds.right - bounds.left;
    const int bh = bounds.bottom - bounds.top;
    if (!dc || bw <= 0 || bh <= 0) return;

    ui::Canvas canvas(dc);
    if (!canvas.ok()) return;

    const float box = static_cast<float>((std::min)(bw, bh));
    const float scale = box / 24.0f;
    const float cx = (bounds.left + bounds.right) / 2.0f;
    const float cy = (bounds.top + bounds.bottom) / 2.0f;
    const float strokeScale = static_cast<float>(ui::ResolveThemeStrokeWidth(100)) / 100.0f;
    IconCtx c{canvas.g(), cx - box / 2.0f, cy - box / 2.0f, scale,
              ui::FromColorRef(color), (std::max)(1.5f, box * 0.093f) *
              (stroke <= 1 ? 0.85f : 1.0f) * strokeScale};

    switch (icon) {
        case UiIcon::Home:
            Polyline(c, Grid(c, {3.5f, 11.5f, 12, 3.5f, 20.5f, 11.5f}), false);
            Polyline(c, Grid(c, {5.5f, 10, 5.5f, 20.5f, 18.5f, 20.5f, 18.5f, 10}), false);
            Polyline(c, Grid(c, {9.7f, 20.5f, 9.7f, 14.5f, 14.3f, 14.5f, 14.3f, 20.5f}), false);
            break;
        case UiIcon::Capture:
            RoundRect(c, 3, 7, 18, 13.5f, 2.6f);
            Polyline(c, Grid(c, {8, 7, 9.6f, 4.4f, 14.4f, 4.4f, 16, 7}), false);
            Arc(c, 8.4f, 9.9f, 7.2f, 7.2f, 0, 360);
            break;
        case UiIcon::Settings: {
            Arc(c, 8.6f, 8.6f, 6.8f, 6.8f, 0, 360);
            for (int i = 0; i < 8; ++i) {
                const double a = i * 3.14159265 / 4.0;
                Line(c, 12 + static_cast<float>(std::cos(a)) * 6.6f, 12 + static_cast<float>(std::sin(a)) * 6.6f,
                     12 + static_cast<float>(std::cos(a)) * 9.4f, 12 + static_cast<float>(std::sin(a)) * 9.4f);
            }
            break;
        }
        case UiIcon::Monitor:
            RoundRect(c, 3, 4, 18, 11.5f, 2);
            Line(c, 12, 15.5f, 12, 18.6f);
            Line(c, 8.4f, 18.9f, 15.6f, 18.9f);
            break;
        case UiIcon::Window:
            RoundRect(c, 3, 4.5f, 18, 15, 2.2f);
            Line(c, 3, 8.6f, 21, 8.6f);
            Dot(c, 5.9f, 6.55f, 0.7f);
            Dot(c, 8.3f, 6.55f, 0.7f);
            break;
        case UiIcon::AllDisplays:
            RoundRect(c, 7.5f, 3.5f, 13, 10.5f, 2);
            RoundRect(c, 3, 9, 13, 11.5f, 2);
            break;
        case UiIcon::Region:
            Polyline(c, Grid(c, {4, 8.5f, 4, 4, 8.5f, 4}), false);
            Polyline(c, Grid(c, {15.5f, 4, 20, 4, 20, 8.5f}), false);
            Polyline(c, Grid(c, {4, 15.5f, 4, 20, 8.5f, 20}), false);
            Polyline(c, Grid(c, {15.5f, 20, 20, 20, 20, 15.5f}), false);
            break;
        case UiIcon::Folder:
            Polyline(c, Grid(c, {3, 7.5f, 9, 7.5f, 11, 10, 21, 10, 21, 19, 3, 19, 3, 7.5f}), false);
            break;
        case UiIcon::Sound:
            Polyline(c, Grid(c, {3.5f, 9.5f, 7, 9.5f, 11, 5.5f, 11, 18.5f, 7, 14.5f, 3.5f, 14.5f, 3.5f, 9.5f}), false);
            Arc(c, 12.5f, 8.5f, 5, 7, -70, 140);
            Arc(c, 12.5f, 5.6f, 8.6f, 12.8f, -62, 124);
            break;
        case UiIcon::Bell:
            Arc(c, 7, 6, 10, 10, 180, 180);
            Line(c, 7, 11, 7, 16.4f);
            Line(c, 17, 11, 17, 16.4f);
            Line(c, 5.4f, 16.7f, 18.6f, 16.7f);
            Polyline(c, Grid(c, {10.4f, 16.9f, 12, 19.6f, 13.6f, 16.9f}), false);
            break;
        case UiIcon::Power:
            Arc(c, 5, 5, 14, 14, -66, 312);
            Line(c, 12, 3.6f, 12, 11.6f);
            break;
        case UiIcon::Arrow: {
            Line(c, 5, 19, 19, 5);
            Polyline(c, Grid(c, {11.6f, 5, 19, 5, 19, 12.4f}), false);
            break;
        }
        case UiIcon::Rectangle:
            RoundRect(c, 4, 5, 16, 14, 2.2f);
            break;
        case UiIcon::Pen:
            Polyline(c, Grid(c, {5, 19, 5, 15.8f, 14.6f, 6.2f, 17.8f, 9.4f, 8.2f, 19, 5, 19}), false);
            Line(c, 12.6f, 8.2f, 15.8f, 11.4f);
            break;
        case UiIcon::Highlighter:
            Polyline(c, Grid(c, {4.5f, 19.5f, 4.5f, 15.5f, 13, 7, 17, 11, 8.5f, 19.5f, 4.5f, 19.5f}), false);
            Line(c, 11.4f, 8.6f, 15.4f, 12.6f);
            Line(c, 4, 21.6f, 11, 21.6f);
            break;
        case UiIcon::Eraser:
            Polyline(c, Grid(c, {14.6f, 5.6f, 20.2f, 11.2f, 11.6f, 19.8f, 6, 14.2f, 14.6f, 5.6f}), true);
            Line(c, 9.4f, 10.4f, 15.4f, 16.4f);
            Line(c, 5.6f, 20.4f, 20.4f, 20.4f);
            break;
        case UiIcon::Undo: {
            GraphicsPath path;
            path.AddBezier(c.P(20, 19), c.P(22, 4), c.P(12, 4), c.P(6, 12));
            Stroke(c, path);
            Polyline(c, Grid(c, {11, 7.5f, 6, 12, 11, 16.5f}), false);
            break;
        }
        case UiIcon::Redo: {
            GraphicsPath path;
            path.AddBezier(c.P(4, 19), c.P(2, 4), c.P(12, 4), c.P(18, 12));
            Stroke(c, path);
            Polyline(c, Grid(c, {13, 7.5f, 18, 12, 13, 16.5f}), false);
            break;
        }
        case UiIcon::Copy:
            RoundRect(c, 8, 4, 12, 12, 2.2f);
            RoundRect(c, 4, 8, 12, 12, 2.2f);
            break;
        case UiIcon::Save:
            Polyline(c, Grid(c, {4, 6, 4, 20, 20, 20, 20, 8, 18, 6, 4, 6}), true);
            Polyline(c, Grid(c, {8, 6, 8, 10.5f, 15, 10.5f, 15, 6}), false);
            RoundRect(c, 8, 13.5f, 8, 6.5f, 1);
            break;
        case UiIcon::Close:
            Line(c, 6.5f, 6.5f, 17.5f, 17.5f);
            Line(c, 17.5f, 6.5f, 6.5f, 17.5f);
            break;
        case UiIcon::Minimize:
            Line(c, 6, 16.5f, 18, 16.5f);
            break;
        case UiIcon::Check:
            Polyline(c, Grid(c, {5, 12.8f, 10, 17.8f, 19, 6.5f}), false);
            break;
        case UiIcon::Warning:
            // Rounded exclamation triangle (Feather "alert-triangle").
            Polyline(c, Grid(c, {12, 3.6f, 21.2f, 19.4f, 2.8f, 19.4f}), true);
            Line(c, 12, 9.4f, 12, 13.8f);
            Dot(c, 12, 16.6f, 1.05f);
            break;
        case UiIcon::Record:
            RoundRect(c, 3, 4.5f, 18, 15, 2.4f);
            Dot(c, 12, 12, 4.1f);
            break;
        case UiIcon::Replay: {
            GraphicsPath path;
            path.AddArc(c.P(4.2f, 4.2f).X, c.P(4.2f, 4.2f).Y, c.S(15.6f), c.S(15.6f), -42, 292);
            Stroke(c, path);
            Polyline(c, Grid(c, {4.2f, 5, 4.2f, 11, 10.2f, 11}), false);
            Line(c, 12, 7.5f, 12, 12);
            Line(c, 12, 12, 15.2f, 14);
            break;
        }
        case UiIcon::Plugin:
            // Puzzle-piece placeholder for the future extension system.
            Polyline(c, Grid(c, {4, 8, 9, 8, 9, 5.8f, 10.4f, 4.2f, 12.2f, 4.2f,
                                 14, 4.2f, 15, 5.8f, 15, 8, 20, 8, 20, 13,
                                 17.8f, 13, 16.2f, 14.2f, 16.2f, 16, 16.2f, 17.8f,
                                 17.8f, 19, 20, 19, 20, 21, 19, 21, 4, 21, 4, 8}), false);
            break;
        case UiIcon::Microphone:
            RoundRect(c, 8.5f, 3, 7, 12, 3.5f);
            Arc(c, 5.5f, 8, 13, 11, 0, 180);
            Line(c, 12, 19, 12, 22);
            Line(c, 8.5f, 22, 15.5f, 22);
            break;
        case UiIcon::Cursor:
            Polyline(c, Grid(c, {5, 3.5f, 5, 19, 9.2f, 14.8f, 12, 21, 15, 19.6f, 12.1f, 13.8f, 18, 13.8f, 5, 3.5f}), true);
            break;
        case UiIcon::Photo:
            RoundRect(c, 3, 4, 18, 16, 2.4f);
            Dot(c, 8, 9, 1.6f);
            Polyline(c, Grid(c, {4.5f, 17.5f, 9.5f, 12.5f, 13, 16, 15.5f, 13.5f, 20, 18}), false);
            break;
        case UiIcon::Video:
            RoundRect(c, 3, 5, 18, 14, 2.4f);
            FilledTriangle(c, c.P(10, 9), c.P(10, 15), c.P(16, 12));
            break;
        case UiIcon::Play:
            FilledTriangle(c, c.P(7.5f, 5), c.P(7.5f, 19), c.P(19, 12));
            break;
        case UiIcon::Pause:
            Line(c, 8.5f, 5, 8.5f, 19);
            Line(c, 15.5f, 5, 15.5f, 19);
            break;
        case UiIcon::Move:
            // Four-way move cross.
            Line(c, 12, 3, 12, 21);
            Line(c, 3, 12, 21, 12);
            Polyline(c, Grid(c, {9, 6, 12, 3, 15, 6}), false);
            Polyline(c, Grid(c, {9, 18, 12, 21, 15, 18}), false);
            Polyline(c, Grid(c, {6, 9, 3, 12, 6, 15}), false);
            Polyline(c, Grid(c, {18, 9, 21, 12, 18, 15}), false);
            break;
        case UiIcon::SkipBack:
            FilledTriangle(c, c.P(19, 5.5f), c.P(19, 18.5f), c.P(10, 12));
            Line(c, 6.5f, 5.5f, 6.5f, 18.5f);
            break;
        case UiIcon::SkipForward:
            FilledTriangle(c, c.P(5, 5.5f), c.P(5, 18.5f), c.P(14, 12));
            Line(c, 17.5f, 5.5f, 17.5f, 18.5f);
            break;
        case UiIcon::Expand:
            // Arrows pushing out to opposite corners.
            Polyline(c, Grid(c, {4, 9.5f, 4, 4, 9.5f, 4}), false);
            Polyline(c, Grid(c, {14.5f, 20, 20, 20, 20, 14.5f}), false);
            Line(c, 4, 4, 10.5f, 10.5f);
            Line(c, 20, 20, 13.5f, 13.5f);
            break;
        case UiIcon::Collapse:
            Polyline(c, Grid(c, {10, 4.5f, 10, 10, 4.5f, 10}), false);
            Polyline(c, Grid(c, {14, 19.5f, 14, 14, 19.5f, 14}), false);
            Line(c, 10, 10, 3.5f, 3.5f);
            Line(c, 14, 14, 20.5f, 20.5f);
            break;
        case UiIcon::Mute:
            Polyline(c, Grid(c, {3.5f, 9.5f, 7, 9.5f, 11, 5.5f, 11, 18.5f, 7, 14.5f, 3.5f, 14.5f, 3.5f, 9.5f}), false);
            Line(c, 14.5f, 9.5f, 20, 15);
            Line(c, 20, 9.5f, 14.5f, 15);
            break;
    }
}
