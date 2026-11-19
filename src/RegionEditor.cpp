#include "RegionEditor.hpp"
#include "CapturePrivacy.hpp"
#include "CoreUtils.hpp"
#include "UiGfx.hpp"
#include "UiIcons.hpp"
#include "WinUtil.hpp"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#pragma comment(lib, "msimg32.lib")

namespace {
constexpr wchar_t kRegionClassName[] = L"LunaCaptureRegionEditor";
constexpr int kToolbarHeight = 50;
constexpr int kToolbarPadding = 8;
constexpr int kGroupGap = 10;

// Curated quick-pick swatches shown in the palette popup.
constexpr int kSwatchCount = 8;
constexpr COLORREF kSwatches[kSwatchCount] = {
    RGB(245, 246, 250), RGB(28, 30, 36),  RGB(255, 86, 86),  RGB(255, 149, 64),
    RGB(255, 209, 64),  RGB(74, 214, 121), RGB(96, 176, 255), RGB(178, 120, 255)};

RECT NormalizeRectPoints(POINT a, POINT b) {
    return RECT{(std::min)(a.x, b.x), (std::min)(a.y, b.y), (std::max)(a.x, b.x), (std::max)(a.y, b.y)};
}

bool PointInside(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

// Squared distance from `point` to the segment a-b, in pixels. Squared so the
// hit tests below never need a sqrt.
double DistanceSquaredToSegment(POINT point, POINT a, POINT b) {
    const double dx = static_cast<double>(b.x - a.x);
    const double dy = static_cast<double>(b.y - a.y);
    const double lengthSquared = dx * dx + dy * dy;
    double t = 0.0;
    if (lengthSquared > 0.0) {
        t = ((point.x - a.x) * dx + (point.y - a.y) * dy) / lengthSquared;
        t = (std::clamp)(t, 0.0, 1.0);
    }
    const double px = a.x + t * dx - point.x;
    const double py = a.y + t * dy - point.y;
    return px * px + py * py;
}

void BlendSolid(HDC destination, const RECT& rect, COLORREF color, BYTE alpha) {
    HDC source = CreateCompatibleDC(destination);
    HBITMAP bitmap = CreateCompatibleBitmap(destination, 1, 1);
    HGDIOBJ old = SelectObject(source, bitmap);
    SetPixel(source, 0, 0, color);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, 0};
    AlphaBlend(destination, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
               source, 0, 0, 1, 1, blend);
    SelectObject(source, old);
    DeleteObject(bitmap);
    DeleteDC(source);
}

void DrawTextCentered(HDC dc, const wchar_t* text, const RECT& rect, COLORREF color, HFONT font) {
    const std::wstring translated = ui::TranslateText(text ? text : L"");
    const HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ui::ResolveThemeColor(color));
    DrawTextW(dc, translated.c_str(), -1, const_cast<RECT*>(&rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

void DrawTextLeft(HDC dc, const wchar_t* text, const RECT& rect, COLORREF color, HFONT font) {
    const std::wstring translated = ui::TranslateText(text ? text : L"");
    const HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ui::ResolveThemeColor(color));
    DrawTextW(dc, translated.c_str(), -1, const_cast<RECT*>(&rect), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

void RgbToHsv(COLORREF c, float& h, float& s, float& v) {
    const float r = GetRValue(c) / 255.0f;
    const float g = GetGValue(c) / 255.0f;
    const float b = GetBValue(c) / 255.0f;
    const float mx = (std::max)(r, (std::max)(g, b));
    const float mn = (std::min)(r, (std::min)(g, b));
    const float d = mx - mn;
    v = mx;
    s = mx <= 0.0001f ? 0.0f : d / mx;
    if (d <= 0.0001f) {
        h = 0.0f;
    } else if (mx == r) {
        h = 60.0f * std::fmod((g - b) / d, 6.0f);
    } else if (mx == g) {
        h = 60.0f * ((b - r) / d + 2.0f);
    } else {
        h = 60.0f * ((r - g) / d + 4.0f);
    }
    if (h < 0.0f) h += 360.0f;
}

COLORREF HsvToRgb(float h, float s, float v) {
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float r = 0, g = 0, b = 0;
    const int seg = static_cast<int>(h / 60.0f) % 6;
    switch (seg) {
        case 0: r = c; g = x; break;
        case 1: r = x; g = c; break;
        case 2: g = c; b = x; break;
        case 3: g = x; b = c; break;
        case 4: r = x; b = c; break;
        default: r = c; b = x; break;
    }
    auto to8 = [](float f) { return static_cast<int>((std::max)(0.0f, (std::min)(1.0f, f)) * 255.0f + 0.5f); };
    return RGB(to8(r + m), to8(g + m), to8(b + m));
}
}  // namespace

RegionEditor::~RegionEditor() { Destroy(); }

bool RegionEditor::Create(HINSTANCE instance, HWND notifyWindow) {
    instance_ = instance;
    notifyWindow_ = notifyWindow;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &RegionEditor::WndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = kRegionClassName;
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    if (!RecreateFonts()) {
        Destroy();
        return false;
    }
    return true;
}

bool RegionEditor::RecreateFonts() {
    HFONT toolbar = ui::CreateThemeFont(14, FW_SEMIBOLD);
    HFONT hint = ui::CreateThemeFont(23, FW_SEMIBOLD);
    HFONT size = ui::CreateThemeFont(14, FW_NORMAL);
    if (!toolbar || !hint || !size) {
        if (toolbar) DeleteObject(toolbar);
        if (hint) DeleteObject(hint);
        if (size) DeleteObject(size);
        return false;
    }

    HFONT oldToolbar = toolbarFont_;
    HFONT oldHint = hintFont_;
    HFONT oldSize = sizeFont_;
    toolbarFont_ = toolbar;
    hintFont_ = hint;
    sizeFont_ = size;
    if (oldToolbar) DeleteObject(oldToolbar);
    if (oldHint) DeleteObject(oldHint);
    if (oldSize) DeleteObject(oldSize);
    return true;
}

void RegionEditor::Destroy() {
    ReleaseFrameSurface();
    if (toolbarFont_) { DeleteObject(toolbarFont_); toolbarFont_ = nullptr; }
    if (hintFont_) { DeleteObject(hintFont_); hintFont_ = nullptr; }
    if (sizeFont_) { DeleteObject(sizeFont_); sizeFont_ = nullptr; }
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    frame_.reset();
    visible_ = false;
}

bool RegionEditor::Show(std::shared_ptr<FrameBuffer> frame, SessionMode mode, const AppSettings& settings, const std::wstring& filenamePrefix) {
    if (!frame || frame->bgra.empty() || !frame->width || !frame->height) return false;
    frame_ = std::move(frame);
    mode_ = mode;
    settings_ = settings;
    filenamePrefix_ = SanitizeFilenamePrefix(filenamePrefix);
    pendingSavePath_.clear();
    pendingSaveFormat_ = settings_.imageFormat;
    ReleaseFrameSurface();
    if (!BuildFrameSurface()) { frame_.reset(); return false; }
    selection_ = {};
    toolbarRect_ = {};
    annotations_.clear();
    redoAnnotations_.clear();
    pendingSavePath_.clear();
    tool_ = mode_ == SessionMode::Region ? Tool::Select : Tool::Arrow;
    hoveredToolbar_ = ToolbarAction::None;
    pressedToolbar_ = ToolbarAction::None;
    draggingSelection_ = false;
    drawingAnnotation_ = false;
    activeGrip_ = Grip::None;
    hoveredGrip_ = Grip::None;
    gripStartAnnotations_.clear();
    hoveredAnnotation_ = -1;
    movingAnnotation_ = -1;
    hoveredWindow_ = nullptr;
    hoveredWindowRect_ = {};
    windowChoicePending_ = false;
    paletteOpen_ = false;
    paletteDrag_ = PaletteTarget::None;
    SyncHsvFromColor();

    if (!hwnd_) {
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kRegionClassName, L"Luna Capture Region",
                                WS_POPUP, frame_->virtualRect.left, frame_->virtualRect.top,
                                static_cast<int>(frame_->width), static_cast<int>(frame_->height),
                                nullptr, nullptr, instance_, this);
        if (!hwnd_) return false;
    }
    // Keep the frozen editor visible to recorders unless privacy was requested.
    std::wstring privacyError;
    if ((settings_.privateCapture && !capturePrivacy::Supported()) ||
        !capturePrivacy::Apply(hwnd_, settings_.privateCapture, L"Region editor", privacyError)) {
        AppendDiagnosticLog(privacyError.empty() ? L"Region editor: capture exclusion unsupported." : privacyError);
        frame_.reset();
        ReleaseFrameSurface();
        return false;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, frame_->virtualRect.left, frame_->virtualRect.top,
                 static_cast<int>(frame_->width), static_cast<int>(frame_->height), SWP_NOACTIVATE);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    DwmFlush();
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    visible_ = true;
    if (mode_ == SessionMode::WindowPicker) {
        POINT cursor{};
        GetCursorPos(&cursor);
        UpdateHoveredWindow(POINT{cursor.x - frame_->virtualRect.left, cursor.y - frame_->virtualRect.top});
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void RegionEditor::Cancel() {
    if (!visible_) return;
    Complete(RegionResultAction::Cancel);
}

void RegionEditor::RefreshTheme() {
    RecreateFonts();
    if (hwnd_ && IsWindowVisible(hwnd_)) {
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
}

RECT RegionEditor::NormalizedSelection() const {
    RECT rect = selection_;
    if (draggingSelection_) rect = NormalizeRectPoints(dragStart_, dragCurrent_);
    return rect;
}

bool RegionEditor::HasSelection() const {
    const RECT rect = NormalizedSelection();
    return rect.right - rect.left >= 4 && rect.bottom - rect.top >= 4;
}

std::vector<RegionEditor::ToolbarItem> RegionEditor::ToolbarItems() const {
    const bool drawingTool = tool_ == Tool::Arrow || tool_ == Tool::Rectangle || tool_ == Tool::Pen || tool_ == Tool::Highlighter;
    std::vector<ToolbarItem> items = {
        {ToolbarAction::Select, 38},      {ToolbarAction::Move, 38},  {ToolbarAction::Arrow, 38},
        {ToolbarAction::Rectangle, 38},   {ToolbarAction::Pen, 38},   {ToolbarAction::Highlighter, 38},
        {ToolbarAction::Eraser, 38},
        {ToolbarAction::Undo, 34},        {ToolbarAction::Redo, 34},
    };
    if (drawingTool) items.push_back({ToolbarAction::Color, 46});
    items.push_back({ToolbarAction::Copy, 72});
    items.push_back({ToolbarAction::Save, 96});
    return items;
}

bool RegionEditor::IsGroupStart(ToolbarAction action) {
    return action == ToolbarAction::Undo || action == ToolbarAction::Color || action == ToolbarAction::Copy;
}

void RegionEditor::UpdateToolbarRect() {
    if (!HasSelection()) { toolbarRect_ = {}; return; }
    const RECT selection = NormalizedSelection();
    const auto items = ToolbarItems();
    int content = 0;
    bool first = true;
    for (const auto& item : items) {
        if (!first && IsGroupStart(item.action)) content += kGroupGap;
        content += item.width;
        first = false;
    }
    const int width = content + kToolbarPadding * 2;
    int left = selection.left;
    if (left + width > static_cast<int>(frame_->width) - 8) left = static_cast<int>(frame_->width) - width - 8;
    left = (std::max)(8, left);
    int top = selection.top - kToolbarHeight - 12;
    if (top < 8) top = selection.bottom + 12;
    if (top + kToolbarHeight > static_cast<int>(frame_->height) - 8) top = selection.top + 12;
    toolbarRect_ = RECT{left, top, left + width, top + kToolbarHeight};
    if (paletteOpen_) LayoutPalette();
}

RECT RegionEditor::ToolbarItemRect(ToolbarAction action) const {
    const auto items = ToolbarItems();
    int x = toolbarRect_.left + kToolbarPadding;
    bool first = true;
    for (const auto& item : items) {
        if (!first && IsGroupStart(item.action)) x += kGroupGap;
        const RECT r{x, toolbarRect_.top + 7, x + item.width, toolbarRect_.bottom - 7};
        if (item.action == action) return r;
        x += item.width;
        first = false;
    }
    return {};
}

void RegionEditor::SetTool(Tool tool) {
    tool_ = tool;
    if (tool_ != Tool::Move) {
        activeGrip_ = Grip::None;
        hoveredGrip_ = Grip::None;
        hoveredAnnotation_ = -1;
        movingAnnotation_ = -1;
    }
    const bool drawingTool = tool_ == Tool::Arrow || tool_ == Tool::Rectangle || tool_ == Tool::Pen || tool_ == Tool::Highlighter;
    if (paletteOpen_ && !drawingTool) ClosePalette();
    if (HasSelection()) UpdateToolbarRect();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void RegionEditor::SetColor(COLORREF color) {
    currentColor_ = color;
    SyncHsvFromColor();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void RegionEditor::SyncHsvFromColor() { RgbToHsv(currentColor_, hsvH_, hsvS_, hsvV_); }
void RegionEditor::SyncColorFromHsv() { currentColor_ = HsvToRgb(hsvH_, hsvS_, hsvV_); }

void RegionEditor::UpdateHoveredWindow(POINT clientPoint) {
    hoveredWindow_ = nullptr;
    hoveredWindowRect_ = {};
    if (mode_ != SessionMode::WindowPicker || !frame_) return;
    POINT screenPoint{clientPoint.x + frame_->virtualRect.left, clientPoint.y + frame_->virtualRect.top};
    HWND window = GetCapturableWindowAtPoint(screenPoint, hwnd_);
    if (!window) return;
    RECT bounds = GetWindowCaptureBounds(window);
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    hoveredWindow_ = window;
    hoveredWindowRect_ = RECT{bounds.left - frame_->virtualRect.left, bounds.top - frame_->virtualRect.top,
                              bounds.right - frame_->virtualRect.left, bounds.bottom - frame_->virtualRect.top};
}

RECT RegionEditor::PrimaryMonitorClientRect() const {
    const RECT client{0, 0, frame_ ? static_cast<LONG>(frame_->width) : 0,
                      frame_ ? static_cast<LONG>(frame_->height) : 0};
    if (!frame_) return client;
    MONITORINFO info{sizeof(info)};
    HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!primary || !GetMonitorInfoW(primary, &info)) return client;
    const RECT mapped{info.rcMonitor.left - frame_->virtualRect.left,
                      info.rcMonitor.top - frame_->virtualRect.top,
                      info.rcMonitor.right - frame_->virtualRect.left,
                      info.rcMonitor.bottom - frame_->virtualRect.top};
    RECT clipped{};
    if (!IntersectRect(&clipped, &mapped, &client) || IsRectEmpty(&clipped)) return client;
    return clipped;
}

// Grip bands are measured inward from the edge so a thin selection stays grabbable:
// with an outward band the corners of a 10px-tall region would overlap each other.
RegionEditor::Grip RegionEditor::GripAt(POINT point) const {
    if (tool_ != Tool::Move || !HasSelection()) return Grip::None;
    const RECT selection = NormalizedSelection();
    constexpr int kBand = 10;
    RECT outer = selection;
    InflateRect(&outer, kBand, kBand);
    if (!PointInside(outer, point)) return Grip::None;

    const int width = selection.right - selection.left;
    const int height = selection.bottom - selection.top;
    const int bandX = (std::min)(kBand, (std::max)(2, width / 3));
    const int bandY = (std::min)(kBand, (std::max)(2, height / 3));
    const bool left = point.x <= selection.left + bandX;
    const bool right = point.x >= selection.right - bandX;
    const bool top = point.y <= selection.top + bandY;
    const bool bottom = point.y >= selection.bottom - bandY;

    if (left && top) return Grip::TopLeft;
    if (right && top) return Grip::TopRight;
    if (left && bottom) return Grip::BottomLeft;
    if (right && bottom) return Grip::BottomRight;
    if (left) return Grip::Left;
    if (right) return Grip::Right;
    if (top) return Grip::Top;
    if (bottom) return Grip::Bottom;
    return PointInside(selection, point) ? Grip::Body : Grip::None;
}

int RegionEditor::AnnotationAt(POINT point) const {
    if (tool_ != Tool::Move) return -1;
    // Newest first: the annotation drawn last is the one on top.
    for (int index = static_cast<int>(annotations_.size()) - 1; index >= 0; --index) {
        const Annotation& annotation = annotations_[static_cast<std::size_t>(index)];
        const double tolerance = annotation.tool == Tool::Highlighter ? 13.0 : 9.0;
        const double limit = tolerance * tolerance;
        switch (annotation.tool) {
            case Tool::Arrow:
                if (DistanceSquaredToSegment(point, annotation.start, annotation.end) <= limit) return index;
                break;
            case Tool::Rectangle: {
                // Border only. Grabbing the hollow middle of a large rectangle has
                // to keep moving the selection, or one big box would swallow the
                // whole canvas.
                const RECT bounds = NormalizeRectPoints(annotation.start, annotation.end);
                const POINT corners[4] = {{bounds.left, bounds.top}, {bounds.right, bounds.top},
                                          {bounds.right, bounds.bottom}, {bounds.left, bounds.bottom}};
                for (int edge = 0; edge < 4; ++edge) {
                    if (DistanceSquaredToSegment(point, corners[edge], corners[(edge + 1) % 4]) <= limit) return index;
                }
                break;
            }
            case Tool::Pen:
            case Tool::Highlighter: {
                if (annotation.points.size() < 2) {
                    if (!annotation.points.empty() &&
                        DistanceSquaredToSegment(point, annotation.points.front(), annotation.points.front()) <= limit) {
                        return index;
                    }
                    break;
                }
                for (std::size_t i = 1; i < annotation.points.size(); ++i) {
                    if (DistanceSquaredToSegment(point, annotation.points[i - 1], annotation.points[i]) <= limit) {
                        return index;
                    }
                }
                break;
            }
            default:
                break;
        }
    }
    return -1;
}

void RegionEditor::OffsetAnnotation(Annotation& annotation, int dx, int dy) {
    annotation.start.x += dx;
    annotation.start.y += dy;
    annotation.end.x += dx;
    annotation.end.y += dy;
    for (POINT& point : annotation.points) {
        point.x += dx;
        point.y += dy;
    }
}

void RegionEditor::ApplyAnnotationDrag(POINT point) {
    if (movingAnnotation_ < 0 || movingAnnotation_ >= static_cast<int>(annotations_.size())) return;
    Annotation moved = annotationDragOrigin_;
    OffsetAnnotation(moved, point.x - annotationDragStart_.x, point.y - annotationDragStart_.y);
    annotations_[static_cast<std::size_t>(movingAnnotation_)] = std::move(moved);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void RegionEditor::ApplyGripDrag(POINT point) {
    if (activeGrip_ == Grip::None || !frame_) return;
    const int frameWidth = static_cast<int>(frame_->width);
    const int frameHeight = static_cast<int>(frame_->height);
    const int dx = point.x - gripDragStart_.x;
    const int dy = point.y - gripDragStart_.y;
    RECT next = gripStartSelection_;

    if (activeGrip_ == Grip::Body) {
        // Moving keeps the size and slides the whole region, stopped by the desktop edge.
        const int width = next.right - next.left;
        const int height = next.bottom - next.top;
        next.left = (std::clamp)(next.left + dx, 0L, static_cast<LONG>(frameWidth - width));
        next.top = (std::clamp)(next.top + dy, 0L, static_cast<LONG>(frameHeight - height));
        next.right = next.left + width;
        next.bottom = next.top + height;
        // Annotations belong to the region, so they travel with it. The delta is
        // taken from the clamped result, not the raw cursor movement, so nothing
        // slides out from under the selection once it hits the desktop edge.
        const int appliedDx = static_cast<int>(next.left - gripStartSelection_.left);
        const int appliedDy = static_cast<int>(next.top - gripStartSelection_.top);
        annotations_ = gripStartAnnotations_;
        for (Annotation& annotation : annotations_) OffsetAnnotation(annotation, appliedDx, appliedDy);
    } else {
        constexpr int kMinimum = 8;
        const bool west = activeGrip_ == Grip::Left || activeGrip_ == Grip::TopLeft || activeGrip_ == Grip::BottomLeft;
        const bool east = activeGrip_ == Grip::Right || activeGrip_ == Grip::TopRight || activeGrip_ == Grip::BottomRight;
        const bool north = activeGrip_ == Grip::Top || activeGrip_ == Grip::TopLeft || activeGrip_ == Grip::TopRight;
        const bool south = activeGrip_ == Grip::Bottom || activeGrip_ == Grip::BottomLeft || activeGrip_ == Grip::BottomRight;
        if (west) next.left = (std::clamp)(next.left + dx, 0L, next.right - kMinimum);
        if (east) next.right = (std::clamp)(next.right + dx, next.left + kMinimum, static_cast<LONG>(frameWidth));
        if (north) next.top = (std::clamp)(next.top + dy, 0L, next.bottom - kMinimum);
        if (south) next.bottom = (std::clamp)(next.bottom + dy, next.top + kMinimum, static_cast<LONG>(frameHeight));
    }

    selection_ = next;
    UpdateToolbarRect();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void RegionEditor::DrawSelectionGrips(HDC dc, const RECT& selection) const {
    constexpr int kHandle = 4;
    const int midX = (selection.left + selection.right) / 2;
    const int midY = (selection.top + selection.bottom) / 2;
    const struct { int x; int y; Grip grip; } handles[] = {
        {selection.left, selection.top, Grip::TopLeft},       {midX, selection.top, Grip::Top},
        {selection.right, selection.top, Grip::TopRight},     {selection.right, midY, Grip::Right},
        {selection.right, selection.bottom, Grip::BottomRight}, {midX, selection.bottom, Grip::Bottom},
        {selection.left, selection.bottom, Grip::BottomLeft}, {selection.left, midY, Grip::Left},
    };
    for (const auto& handle : handles) {
        const bool hot = hoveredGrip_ == handle.grip || activeGrip_ == handle.grip;
        const RECT box{handle.x - kHandle, handle.y - kHandle, handle.x + kHandle + 1, handle.y + kHandle + 1};
        ui::FillRoundRect(dc, box, 3, hot ? RGB(150, 205, 255) : RGB(240, 246, 255));
        ui::StrokeRoundRect(dc, box, 3, RGB(30, 78, 148), 1);
    }
}

RECT RegionEditor::WindowChoicePanel() const {
    const int width = 650;
    const int height = 116;
    // Anchored to the primary monitor, not the virtual desktop, so the prompt is
    // never split down the middle by a monitor seam.
    const RECT host = PrimaryMonitorClientRect();
    const int left = host.left + ((host.right - host.left) - width) / 2;
    const int top = (std::max<int>)(host.top + 24, host.bottom - height - 48);
    return RECT{left, top, left + width, top + height};
}

RECT RegionEditor::WindowChoiceButton(int index) const {
    const RECT panel = WindowChoicePanel();
    constexpr int gap = 10;
    constexpr int buttonWidth = 196;
    const int left = panel.left + 21 + index * (buttonWidth + gap);
    return RECT{left, panel.top + 54, left + buttonWidth, panel.bottom - 14};
}

RegionEditor::ToolbarAction RegionEditor::HitTestToolbar(POINT point) const {
    if (!PointInside(toolbarRect_, point)) return ToolbarAction::None;
    for (const auto& item : ToolbarItems()) {
        if (PointInside(ToolbarItemRect(item.action), point)) return item.action;
    }
    return ToolbarAction::None;
}

void RegionEditor::Undo() {
    if (annotations_.empty()) return;
    redoAnnotations_.push_back(std::move(annotations_.back()));
    annotations_.pop_back();
    // The hover index refers to a list that just changed under it.
    hoveredAnnotation_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void RegionEditor::Redo() {
    if (redoAnnotations_.empty()) return;
    annotations_.push_back(std::move(redoAnnotations_.back()));
    redoAnnotations_.pop_back();
    hoveredAnnotation_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void RegionEditor::EraseAt(POINT point) {
    const RECT eraser{point.x - 10, point.y - 10, point.x + 10, point.y + 10};
    bool removed = false;
    auto intersects = [&](const Annotation& a) {
        RECT bounds{};
        if (!a.points.empty()) {
            bounds = RECT{a.points.front().x, a.points.front().y, a.points.front().x, a.points.front().y};
            for (const auto& p : a.points) {
                bounds.left = (std::min)(bounds.left, p.x); bounds.top = (std::min)(bounds.top, p.y);
                bounds.right = (std::max)(bounds.right, p.x); bounds.bottom = (std::max)(bounds.bottom, p.y);
            }
        } else {
            bounds = NormalizeRectPoints(a.start, a.end);
        }
        InflateRect(&bounds, a.tool == Tool::Highlighter ? 12 : 8, a.tool == Tool::Highlighter ? 12 : 8);
        RECT out{};
        return IntersectRect(&out, &bounds, &eraser) != FALSE;
    };
    auto it = annotations_.begin();
    while (it != annotations_.end()) {
        if (intersects(*it)) { it = annotations_.erase(it); removed = true; }
        else ++it;
    }
    if (removed) {
        redoAnnotations_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void RegionEditor::ExecuteToolbar(ToolbarAction action) {
    switch (action) {
        case ToolbarAction::Select: SetTool(Tool::Select); break;
        case ToolbarAction::Move: SetTool(Tool::Move); break;
        case ToolbarAction::Arrow: SetTool(Tool::Arrow); break;
        case ToolbarAction::Rectangle: SetTool(Tool::Rectangle); break;
        case ToolbarAction::Pen: SetTool(Tool::Pen); break;
        case ToolbarAction::Highlighter: SetTool(Tool::Highlighter); break;
        case ToolbarAction::Eraser: SetTool(Tool::Eraser); break;
        case ToolbarAction::Color: paletteOpen_ ? ClosePalette() : OpenPalette(); break;
        case ToolbarAction::Undo: Undo(); break;
        case ToolbarAction::Redo: Redo(); break;
        case ToolbarAction::Copy: Complete(RegionResultAction::Copy); break;
        case ToolbarAction::Save:
            if (mode_ == SessionMode::WindowPicker || settings_.privateCapture) {
                Complete(RegionResultAction::Save);
            } else {
                std::wstring selectedPath;
                ImageFormat selectedFormat = settings_.imageFormat;
                if (ChooseScreenshotSavePath(hwnd_, settings_, filenamePrefix_, selectedPath, selectedFormat)) {
                    pendingSavePath_ = std::move(selectedPath);
                    pendingSaveFormat_ = selectedFormat;
                    Complete(RegionResultAction::Save);
                } else {
                    SetForegroundWindow(hwnd_);
                    SetFocus(hwnd_);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            break;
        default: break;
    }
}

void RegionEditor::Complete(RegionResultAction action) {
    if (!visible_) return;
    ReleaseCapture();
    ShowWindow(hwnd_, SW_HIDE);
    // Make the hide visible to DWM before hotkeys are re-enabled by the completion event.
    DwmFlush();
    visible_ = false;
    paletteOpen_ = false;
    windowChoicePending_ = false;
    auto* result = new (std::nothrow) RegionResult();
    if (!result) return;
    result->action = action;
    result->sourceMode = mode_ == SessionMode::WindowPicker ? CaptureMode::ActiveWindow : CaptureMode::Region;
    result->savePath = pendingSavePath_;
    result->saveFormat = pendingSaveFormat_;
    result->filenamePrefix = filenamePrefix_;
    if (action != RegionResultAction::Cancel) result->frame = RenderCrop();
    if (!PostMessageW(notifyWindow_, WM_APP_REGION_COMPLETE, 0, reinterpret_cast<LPARAM>(result))) delete result;
    frame_.reset();
    ReleaseFrameSurface();
    annotations_.clear();
    redoAnnotations_.clear();
    pendingSavePath_.clear();
    filenamePrefix_.clear();
    hoveredToolbar_ = ToolbarAction::None;
    pressedToolbar_ = ToolbarAction::None;
}

void RegionEditor::DrawAnnotation(HDC dc, const Annotation& annotation, POINT offset) const {
    if (annotation.tool == Tool::Eraser) return;
    ui::Canvas canvas(dc);
    if (!canvas.ok()) return;
    Gdiplus::Graphics& g = canvas.g();

    auto tx = [offset](POINT p) { return Gdiplus::PointF(static_cast<float>(p.x + offset.x), static_cast<float>(p.y + offset.y)); };
    const float width = annotation.tool == Tool::Highlighter ? 15.0f : (annotation.tool == Tool::Pen ? 3.2f : 2.6f);
    const BYTE alpha = annotation.tool == Tool::Highlighter ? annotation.alpha : 255;
    const Gdiplus::Color color = ui::FromColorRefRaw(annotation.color, alpha);

    Gdiplus::Pen pen(color, width);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);

    if (annotation.tool == Tool::Rectangle) {
        const RECT r = NormalizeRectPoints(annotation.start, annotation.end);
        const Gdiplus::PointF tl = tx(POINT{r.left, r.top});
        g.DrawRectangle(&pen, tl.X, tl.Y, static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
    } else if (annotation.tool == Tool::Arrow) {
        const Gdiplus::PointF a = tx(annotation.start);
        const Gdiplus::PointF b = tx(annotation.end);
        const double angle = std::atan2(static_cast<double>(b.Y - a.Y), static_cast<double>(b.X - a.X));
        const double head = 15.0;
        const double spread = 0.44;
        g.DrawLine(&pen, a, b);
        Gdiplus::PointF p2(b.X - static_cast<float>(std::cos(angle - spread) * head),
                          b.Y - static_cast<float>(std::sin(angle - spread) * head));
        Gdiplus::PointF p3(b.X - static_cast<float>(std::cos(angle + spread) * head),
                          b.Y - static_cast<float>(std::sin(angle + spread) * head));
        Gdiplus::PointF tri[3] = {b, p2, p3};
        Gdiplus::SolidBrush brush(color);
        g.FillPolygon(&brush, tri, 3);
    } else if (!annotation.points.empty()) {
        if (annotation.points.size() == 1) {
            const Gdiplus::PointF p = tx(annotation.points.front());
            Gdiplus::SolidBrush brush(color);
            g.FillEllipse(&brush, p.X - width / 2, p.Y - width / 2, width, width);
        } else {
            std::vector<Gdiplus::PointF> pts;
            pts.reserve(annotation.points.size());
            for (const auto& p : annotation.points) pts.push_back(tx(p));
            Gdiplus::GraphicsPath path;
            path.AddLines(pts.data(), static_cast<INT>(pts.size()));
            g.DrawPath(&pen, &path);
        }
    }
}

void RegionEditor::DrawAnnotations(HDC dc, POINT offset) const {
    for (const auto& annotation : annotations_) DrawAnnotation(dc, annotation, offset);
    if (drawingAnnotation_) DrawAnnotation(dc, currentAnnotation_, offset);
}

FrameBuffer RegionEditor::RenderCrop() const {
    FrameBuffer result;
    if (!frame_ || !HasSelection()) return result;
    RECT selection = NormalizedSelection();
    selection.left = (std::max)(0L, selection.left);
    selection.top = (std::max)(0L, selection.top);
    selection.right = (std::min)(static_cast<LONG>(frame_->width), selection.right);
    selection.bottom = (std::min)(static_cast<LONG>(frame_->height), selection.bottom);
    result.width = static_cast<UINT>(selection.right - selection.left);
    result.height = static_cast<UINT>(selection.bottom - selection.top);
    result.stride = result.width * 4;
    result.virtualRect = RECT{0, 0, static_cast<LONG>(result.width), static_cast<LONG>(result.height)};
    result.bgra.resize(static_cast<std::size_t>(result.stride) * result.height);
    for (UINT y = 0; y < result.height; ++y) {
        std::memcpy(result.bgra.data() + static_cast<std::size_t>(y) * result.stride,
                    frame_->bgra.data() + static_cast<std::size_t>(selection.top + y) * frame_->stride + selection.left * 4,
                    result.stride);
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(result.width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(result.height);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = screen ? CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (memory && bitmap && bits) {
        HGDIOBJ old = SelectObject(memory, bitmap);
        std::memcpy(bits, result.bgra.data(), result.bgra.size());
        DrawAnnotations(memory, POINT{-selection.left, -selection.top});
        std::memcpy(result.bgra.data(), bits, result.bgra.size());
        SelectObject(memory, old);
    }
    for (std::size_t i = 3; i < result.bgra.size(); i += 4) result.bgra[i] = 255;
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(nullptr, screen);
    return result;
}

void RegionEditor::DrawToolbar(HDC dc, const RECT&) {
    ui::FillRoundRect(dc, toolbarRect_, 14, RGB(20, 23, 29), 246);
    ui::StrokeRoundRect(dc, toolbarRect_, 14, RGB(66, 72, 86));

    const auto items = ToolbarItems();
    bool first = true;
    for (const auto& item : items) {
        const ToolbarAction action = item.action;
        const RECT rect = ToolbarItemRect(action);
        if (!first && IsGroupStart(action)) {
            const int sx = rect.left - kGroupGap / 2;
            ui::FillRectAlpha(dc, RECT{sx, rect.top + 4, sx + 1, rect.bottom - 4}, RGB(58, 64, 77), 200);
        }
        first = false;

        const bool selected = (action == ToolbarAction::Select && tool_ == Tool::Select) ||
                              (action == ToolbarAction::Move && tool_ == Tool::Move) ||
                              (action == ToolbarAction::Arrow && tool_ == Tool::Arrow) ||
                              (action == ToolbarAction::Rectangle && tool_ == Tool::Rectangle) ||
                              (action == ToolbarAction::Pen && tool_ == Tool::Pen) ||
                              (action == ToolbarAction::Highlighter && tool_ == Tool::Highlighter) ||
                              (action == ToolbarAction::Eraser && tool_ == Tool::Eraser) ||
                              (action == ToolbarAction::Color && paletteOpen_);
        const bool enabled = !((action == ToolbarAction::Undo && annotations_.empty()) ||
                               (action == ToolbarAction::Redo && redoAnnotations_.empty()));
        const bool hovered = enabled && hoveredToolbar_ == action;
        const bool pressed = enabled && pressedToolbar_ == action;

        if (selected && action != ToolbarAction::Color) ui::FillRoundRect(dc, rect, 9, RGB(62, 110, 208));
        else if (pressed) ui::FillRoundRect(dc, rect, 9, RGB(46, 53, 66));
        else if (hovered) ui::FillRoundRect(dc, rect, 9, RGB(34, 39, 50));

        if (action == ToolbarAction::Color) {
            RECT swatch{rect.left + 7, rect.top + 7, rect.right - 7, rect.bottom - 7};
            { ui::ScopedThemeBypass bypass; ui::FillRoundRect(dc, swatch, 6, currentColor_); }
            ui::StrokeRoundRect(dc, swatch, 6, selected ? RGB(255, 255, 255) : RGB(90, 96, 110), selected ? 2 : 1);
            continue;
        }

        UiIcon icon = UiIcon::Region;
        const wchar_t* label = nullptr;
        switch (action) {
            case ToolbarAction::Select: icon = UiIcon::Region; break;
            case ToolbarAction::Move: icon = UiIcon::Move; break;
            case ToolbarAction::Arrow: icon = UiIcon::Arrow; break;
            case ToolbarAction::Rectangle: icon = UiIcon::Rectangle; break;
            case ToolbarAction::Pen: icon = UiIcon::Pen; break;
            case ToolbarAction::Highlighter: icon = UiIcon::Highlighter; break;
            case ToolbarAction::Eraser: icon = UiIcon::Eraser; break;
            case ToolbarAction::Undo: icon = UiIcon::Undo; break;
            case ToolbarAction::Redo: icon = UiIcon::Redo; break;
            case ToolbarAction::Copy: icon = UiIcon::Copy; label = L"Copy"; break;
            case ToolbarAction::Save: icon = UiIcon::Save; label = L"Save as"; break;
            default: break;
        }

        const COLORREF iconColor = !enabled ? RGB(84, 90, 101) : selected ? RGB(255, 255, 255) :
            (action == ToolbarAction::Save ? RGB(120, 190, 255) : RGB(214, 219, 229));
        if (label) {
            RECT iconRect{rect.left + 12, (rect.top + rect.bottom) / 2 - 9, rect.left + 30, (rect.top + rect.bottom) / 2 + 9};
            DrawUiIcon(dc, icon, iconRect, iconColor, 2);
            RECT labelRect{rect.left + 32, rect.top, rect.right - 6, rect.bottom};
            DrawTextCentered(dc, label, labelRect, iconColor, toolbarFont_);
        } else {
            const int cx = (rect.left + rect.right) / 2;
            const int cy = (rect.top + rect.bottom) / 2;
            DrawUiIcon(dc, icon, RECT{cx - 10, cy - 10, cx + 10, cy + 10}, iconColor, 2);
        }
    }
}

void RegionEditor::OpenPalette() {
    SyncHsvFromColor();
    paletteOpen_ = true;
    LayoutPalette();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void RegionEditor::ClosePalette() {
    paletteOpen_ = false;
    paletteDrag_ = PaletteTarget::None;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void RegionEditor::LayoutPalette() {
    const bool highlighter = tool_ == Tool::Highlighter;
    const int pad = 14;
    const int svW = 176, svH = 118, hueW = 14, gap = 10;
    const int contentW = svW + gap + hueW;
    const int panelW = contentW + pad * 2;
    const int swatchH = 22;
    const int titleH = 26;
    int panelH = pad * 2 + titleH + svH + 14 + swatchH;
    if (highlighter) panelH += 14 + 20;

    const RECT cbr = ToolbarItemRect(ToolbarAction::Color);
    const int frameW = frame_ ? static_cast<int>(frame_->width) : panelW + 32;
    const int frameH = frame_ ? static_cast<int>(frame_->height) : panelH + 32;
    int left = cbr.left - 8;
    left = (std::max)(8, (std::min)(left, frameW - panelW - 8));
    int top = toolbarRect_.top - 8 - panelH;
    if (top < 8) top = (std::min)(static_cast<int>(toolbarRect_.bottom) + 8, frameH - panelH - 8);
    top = (std::max)(8, top);

    paletteRect_ = RECT{left, top, left + panelW, top + panelH};
    const int cx = left + pad;
    const int cy = top + pad + titleH;
    paletteSatValRect_ = RECT{cx, cy, cx + svW, cy + svH};
    paletteHueRect_ = RECT{cx + svW + gap, cy, cx + svW + gap + hueW, cy + svH};
    const int sy = cy + svH + 14;
    paletteSwatchRect_ = RECT{cx, sy, cx + contentW, sy + swatchH};
    if (highlighter) paletteOpacityRect_ = RECT{cx, sy + swatchH + 14, cx + contentW, sy + swatchH + 14 + 20};
    else paletteOpacityRect_ = RECT{};
}

void RegionEditor::DrawPalette(HDC dc) {
    LayoutPalette();
    // Panel shell.
    RECT shadow = paletteRect_;
    InflateRect(&shadow, 3, 3);
    ui::FillRoundRect(dc, shadow, 18, RGB(0, 0, 0), 60);
    ui::FillRoundRect(dc, paletteRect_, 16, RGB(24, 27, 34));
    ui::StrokeRoundRect(dc, paletteRect_, 16, RGB(70, 76, 90));

    DrawTextLeft(dc, L"Color", RECT{paletteRect_.left + 14, paletteRect_.top + 10, paletteRect_.left + 120, paletteRect_.top + 32},
                 RGB(232, 236, 244), toolbarFont_);
    RECT preview{paletteRect_.right - 44, paletteRect_.top + 11, paletteRect_.right - 14, paletteRect_.top + 30};
    { ui::ScopedThemeBypass bypass; ui::FillRoundRect(dc, preview, 6, currentColor_); }
    ui::StrokeRoundRect(dc, preview, 6, RGB(90, 96, 110));

    const Gdiplus::Color hueColor = ui::FromColorRefRaw(HsvToRgb(hsvH_, 1.0f, 1.0f));
    {
        ui::Canvas canvas(dc);
        if (canvas.ok()) {
            Gdiplus::Graphics& g = canvas.g();
            // Saturation / value square (rounded).
            const Gdiplus::RectF sv(static_cast<float>(paletteSatValRect_.left), static_cast<float>(paletteSatValRect_.top),
                                    static_cast<float>(paletteSatValRect_.right - paletteSatValRect_.left),
                                    static_cast<float>(paletteSatValRect_.bottom - paletteSatValRect_.top));
            Gdiplus::GraphicsPath svPath;
            const float svR = 8.0f;
            svPath.AddArc(sv.X, sv.Y, svR * 2, svR * 2, 180, 90);
            svPath.AddArc(sv.X + sv.Width - svR * 2, sv.Y, svR * 2, svR * 2, 270, 90);
            svPath.AddArc(sv.X + sv.Width - svR * 2, sv.Y + sv.Height - svR * 2, svR * 2, svR * 2, 0, 90);
            svPath.AddArc(sv.X, sv.Y + sv.Height - svR * 2, svR * 2, svR * 2, 90, 90);
            svPath.CloseFigure();
            g.SetClip(&svPath);
            Gdiplus::LinearGradientBrush satBrush(Gdiplus::RectF(sv.X - 1, sv.Y, sv.Width + 2, sv.Height),
                                                  Gdiplus::Color(255, 255, 255, 255), hueColor,
                                                  Gdiplus::LinearGradientModeHorizontal);
            g.FillRectangle(&satBrush, sv);
            Gdiplus::LinearGradientBrush valBrush(Gdiplus::RectF(sv.X, sv.Y - 1, sv.Width, sv.Height + 2),
                                                  Gdiplus::Color(0, 0, 0, 0), Gdiplus::Color(255, 0, 0, 0),
                                                  Gdiplus::LinearGradientModeVertical);
            g.FillRectangle(&valBrush, sv);
            g.ResetClip();

            // Hue bar (rounded, 7-stop spectrum).
            const Gdiplus::RectF hue(static_cast<float>(paletteHueRect_.left), static_cast<float>(paletteHueRect_.top),
                                     static_cast<float>(paletteHueRect_.right - paletteHueRect_.left),
                                     static_cast<float>(paletteHueRect_.bottom - paletteHueRect_.top));
            Gdiplus::GraphicsPath huePath;
            const float hr = hue.Width / 2.0f;
            huePath.AddArc(hue.X, hue.Y, hr * 2, hr * 2, 180, 180);
            huePath.AddArc(hue.X, hue.Y + hue.Height - hr * 2, hr * 2, hr * 2, 0, 180);
            huePath.CloseFigure();
            g.SetClip(&huePath);
            Gdiplus::LinearGradientBrush hueBrush(Gdiplus::RectF(hue.X, hue.Y - 1, hue.Width, hue.Height + 2),
                                                  Gdiplus::Color(255, 255, 0, 0), Gdiplus::Color(255, 255, 0, 0),
                                                  Gdiplus::LinearGradientModeVertical);
            Gdiplus::Color hueStops[7] = {
                Gdiplus::Color(255, 255, 0, 0),   Gdiplus::Color(255, 255, 255, 0), Gdiplus::Color(255, 0, 255, 0),
                Gdiplus::Color(255, 0, 255, 255), Gdiplus::Color(255, 0, 0, 255),   Gdiplus::Color(255, 255, 0, 255),
                Gdiplus::Color(255, 255, 0, 0)};
            Gdiplus::REAL huePos[7] = {0.0f, 1.0f / 6, 2.0f / 6, 3.0f / 6, 4.0f / 6, 5.0f / 6, 1.0f};
            hueBrush.SetInterpolationColors(hueStops, huePos, 7);
            g.FillRectangle(&hueBrush, hue);
            g.ResetClip();

            // Selectors.
            const float selX = sv.X + hsvS_ * sv.Width;
            const float selY = sv.Y + (1.0f - hsvV_) * sv.Height;
            Gdiplus::Pen ringDark(Gdiplus::Color(180, 0, 0, 0), 3.0f);
            Gdiplus::Pen ringLight(Gdiplus::Color(255, 255, 255, 255), 2.0f);
            g.DrawEllipse(&ringDark, selX - 6.0f, selY - 6.0f, 12.0f, 12.0f);
            g.DrawEllipse(&ringLight, selX - 6.0f, selY - 6.0f, 12.0f, 12.0f);

            const float hueY = hue.Y + (hsvH_ / 360.0f) * hue.Height;
            const float hueCX = hue.X + hue.Width / 2.0f;
            Gdiplus::Pen hueMarkDark(Gdiplus::Color(200, 0, 0, 0), 3.0f);
            Gdiplus::Pen hueMarkLight(Gdiplus::Color(255, 255, 255, 255), 2.0f);
            g.DrawEllipse(&hueMarkDark, hueCX - 6.0f, hueY - 6.0f, 12.0f, 12.0f);
            g.DrawEllipse(&hueMarkLight, hueCX - 6.0f, hueY - 6.0f, 12.0f, 12.0f);
        }
    }

    // Swatch row.
    const int rowW = paletteSwatchRect_.right - paletteSwatchRect_.left;
    const float cellGap = 6.0f;
    const float cellW = (rowW - cellGap * (kSwatchCount - 1)) / kSwatchCount;
    for (int i = 0; i < kSwatchCount; ++i) {
        const int cl = paletteSwatchRect_.left + static_cast<int>(i * (cellW + cellGap));
        RECT cell{cl, paletteSwatchRect_.top, cl + static_cast<int>(cellW), paletteSwatchRect_.bottom};
        { ui::ScopedThemeBypass bypass; ui::FillRoundRect(dc, cell, 5, kSwatches[i]); }
        if (kSwatches[i] == currentColor_) ui::StrokeRoundRect(dc, cell, 5, RGB(255, 255, 255), 2);
        else ui::StrokeRoundRect(dc, cell, 5, RGB(70, 76, 90), 1);
    }

    // Opacity slider (highlighter only).
    if (tool_ == Tool::Highlighter && paletteOpacityRect_.right > paletteOpacityRect_.left) {
        RECT bar{paletteOpacityRect_.left, paletteOpacityRect_.top + 4, paletteOpacityRect_.right, paletteOpacityRect_.bottom - 4};
        ui::FillRoundRect(dc, bar, (bar.bottom - bar.top) / 2, RGB(38, 42, 52));
        {
            ui::Canvas canvas(dc);
            if (canvas.ok()) {
                Gdiplus::Graphics& g = canvas.g();
                Gdiplus::RectF b(static_cast<float>(bar.left), static_cast<float>(bar.top),
                                 static_cast<float>(bar.right - bar.left), static_cast<float>(bar.bottom - bar.top));
                Gdiplus::GraphicsPath bp;
                const float br = b.Height / 2.0f;
                bp.AddArc(b.X, b.Y, br * 2, br * 2, 90, 180);
                bp.AddArc(b.X + b.Width - br * 2, b.Y, br * 2, br * 2, 270, 180);
                bp.CloseFigure();
                g.SetClip(&bp);
                Gdiplus::LinearGradientBrush op(Gdiplus::RectF(b.X - 1, b.Y, b.Width + 2, b.Height),
                                                ui::FromColorRefRaw(currentColor_, 0), ui::FromColorRefRaw(currentColor_, 255),
                                                Gdiplus::LinearGradientModeHorizontal);
                g.FillRectangle(&op, b);
                g.ResetClip();
            }
        }
        const float t = (highlighterAlpha_ - 38.0f) / (255.0f - 38.0f);
        const int knobX = bar.left + static_cast<int>((std::max)(0.0f, (std::min)(1.0f, t)) * (bar.right - bar.left));
        ui::FillEllipse(dc, RECT{knobX - 8, bar.top - 2, knobX + 8, bar.bottom + 2}, RGB(248, 249, 252));
        ui::StrokeEllipse(dc, RECT{knobX - 8, bar.top - 2, knobX + 8, bar.bottom + 2}, RGB(120, 126, 140), 1);
    }
}

RegionEditor::PaletteTarget RegionEditor::PaletteControlAt(POINT point, int* swatchIndex) const {
    if (swatchIndex) *swatchIndex = -1;
    if (PointInside(paletteSatValRect_, point)) return PaletteTarget::SatVal;
    if (PointInside(paletteHueRect_, point)) return PaletteTarget::Hue;
    if (tool_ == Tool::Highlighter && paletteOpacityRect_.right > paletteOpacityRect_.left &&
        PointInside(paletteOpacityRect_, point)) return PaletteTarget::Opacity;
    if (PointInside(paletteSwatchRect_, point)) {
        const int rowW = paletteSwatchRect_.right - paletteSwatchRect_.left;
        const float cellGap = 6.0f;
        const float cellW = (rowW - cellGap * (kSwatchCount - 1)) / kSwatchCount;
        for (int i = 0; i < kSwatchCount; ++i) {
            const int cl = paletteSwatchRect_.left + static_cast<int>(i * (cellW + cellGap));
            if (point.x >= cl && point.x < cl + static_cast<int>(cellW)) {
                if (swatchIndex) *swatchIndex = i;
                return PaletteTarget::Swatch;
            }
        }
    }
    return PaletteTarget::None;
}

void RegionEditor::ApplyPalettePoint(PaletteTarget target, POINT point) {
    auto clamp01 = [](float v) { return (std::max)(0.0f, (std::min)(1.0f, v)); };
    auto span = [](LONG lo, LONG hi) { return static_cast<float>((std::max)(1L, hi - lo)); };
    if (target == PaletteTarget::SatVal) {
        hsvS_ = clamp01(static_cast<float>(point.x - paletteSatValRect_.left) /
                        span(paletteSatValRect_.left, paletteSatValRect_.right));
        hsvV_ = 1.0f - clamp01(static_cast<float>(point.y - paletteSatValRect_.top) /
                               span(paletteSatValRect_.top, paletteSatValRect_.bottom));
        SyncColorFromHsv();
    } else if (target == PaletteTarget::Hue) {
        hsvH_ = clamp01(static_cast<float>(point.y - paletteHueRect_.top) /
                        span(paletteHueRect_.top, paletteHueRect_.bottom)) * 360.0f;
        SyncColorFromHsv();
    } else if (target == PaletteTarget::Opacity) {
        const float t = clamp01(static_cast<float>(point.x - paletteOpacityRect_.left) /
                                span(paletteOpacityRect_.left, paletteOpacityRect_.right));
        highlighterAlpha_ = static_cast<BYTE>(38.0f + t * (255.0f - 38.0f));
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool RegionEditor::BuildFrameSurface() {
    if (!frame_ || frame_->bgra.empty()) return false;
    HDC screen = GetDC(nullptr);
    frameDc_ = CreateCompatibleDC(screen);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(frame_->width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(frame_->height);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    frameBitmap_ = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!frameDc_ || !frameBitmap_ || !bits) {
        ReleaseFrameSurface();
        return false;
    }
    frameOldBitmap_ = SelectObject(frameDc_, frameBitmap_);
    for (UINT y = 0; y < frame_->height; ++y) {
        std::memcpy(static_cast<BYTE*>(bits) + static_cast<std::size_t>(y) * frame_->width * 4,
                    frame_->bgra.data() + static_cast<std::size_t>(y) * frame_->stride,
                    static_cast<std::size_t>(frame_->width) * 4);
    }

    HDC screen2 = GetDC(nullptr);
    dimmedDc_ = CreateCompatibleDC(screen2);
    renderDc_ = CreateCompatibleDC(screen2);
    dimmedBitmap_ = CreateCompatibleBitmap(screen2, static_cast<int>(frame_->width), static_cast<int>(frame_->height));
    renderBitmap_ = CreateCompatibleBitmap(screen2, static_cast<int>(frame_->width), static_cast<int>(frame_->height));
    ReleaseDC(nullptr, screen2);
    if (!dimmedDc_ || !dimmedBitmap_ || !renderDc_ || !renderBitmap_) {
        ReleaseFrameSurface();
        return false;
    }
    dimmedOldBitmap_ = SelectObject(dimmedDc_, dimmedBitmap_);
    renderOldBitmap_ = SelectObject(renderDc_, renderBitmap_);
    BitBlt(dimmedDc_, 0, 0, static_cast<int>(frame_->width), static_cast<int>(frame_->height), frameDc_, 0, 0, SRCCOPY);
    RECT full{0, 0, static_cast<LONG>(frame_->width), static_cast<LONG>(frame_->height)};
    BlendSolid(dimmedDc_, full, RGB(15, 17, 22), 150);
    BitBlt(renderDc_, 0, 0, static_cast<int>(frame_->width), static_cast<int>(frame_->height), dimmedDc_, 0, 0, SRCCOPY);
    return true;
}

void RegionEditor::ReleaseFrameSurface() {
    if (renderDc_ && renderOldBitmap_) SelectObject(renderDc_, renderOldBitmap_);
    renderOldBitmap_ = nullptr;
    if (renderBitmap_) DeleteObject(renderBitmap_);
    renderBitmap_ = nullptr;
    if (renderDc_) DeleteDC(renderDc_);
    renderDc_ = nullptr;

    if (dimmedDc_ && dimmedOldBitmap_) SelectObject(dimmedDc_, dimmedOldBitmap_);
    dimmedOldBitmap_ = nullptr;
    if (dimmedBitmap_) DeleteObject(dimmedBitmap_);
    dimmedBitmap_ = nullptr;
    if (dimmedDc_) DeleteDC(dimmedDc_);
    dimmedDc_ = nullptr;

    if (frameDc_ && frameOldBitmap_) SelectObject(frameDc_, frameOldBitmap_);
    frameOldBitmap_ = nullptr;
    if (frameBitmap_) DeleteObject(frameBitmap_);
    frameBitmap_ = nullptr;
    if (frameDc_) DeleteDC(frameDc_);
    frameDc_ = nullptr;
}

void RegionEditor::Paint() {
    if (!frame_ || !frameDc_ || !dimmedDc_ || !renderDc_) return;
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(hwnd_, &paint);
    RECT client{};
    GetClientRect(hwnd_, &client);

    RECT present = paint.rcPaint;
    if (IsRectEmpty(&present)) {
        EndPaint(hwnd_, &paint);
        return;
    }
    // Toolbar and palette invalidations only touch their part of the desktop-sized buffer.
    const int renderState = SaveDC(renderDc_);
    if (renderState) IntersectClipRect(renderDc_, present.left, present.top, present.right, present.bottom);
    const int width = present.right - present.left;
    const int height = present.bottom - present.top;
    BitBlt(renderDc_, present.left, present.top, width, height, dimmedDc_, present.left, present.top, SRCCOPY);

    if (mode_ == SessionMode::WindowPicker) {
        if (hoveredWindowRect_.right > hoveredWindowRect_.left && hoveredWindowRect_.bottom > hoveredWindowRect_.top) {
            const RECT selection = hoveredWindowRect_;
            BitBlt(renderDc_, selection.left, selection.top, selection.right - selection.left,
                   selection.bottom - selection.top, frameDc_, selection.left, selection.top, SRCCOPY);
            ui::StrokeRoundRect(renderDc_, selection, 3, RGB(112, 181, 255), 2);

            wchar_t sizeText[96]{};
            swprintf_s(sizeText, L"%ld x %ld", selection.right - selection.left, selection.bottom - selection.top);
            RECT sizeRect{selection.left, selection.bottom + 8, selection.left + 132, selection.bottom + 36};
            if (sizeRect.bottom > client.bottom) { sizeRect.top = selection.top - 36; sizeRect.bottom = selection.top - 8; }
            ui::FillRoundRect(renderDc_, sizeRect, 9, RGB(20, 23, 29));
            ui::StrokeRoundRect(renderDc_, sizeRect, 9, RGB(64, 70, 84));
            DrawTextCentered(renderDc_, sizeText, sizeRect, RGB(224, 229, 238), sizeFont_);
        }
        if (windowChoicePending_) {
            const RECT panel = WindowChoicePanel();
            ui::FillRoundRect(renderDc_, panel, 18, RGB(18, 21, 27));
            ui::StrokeRoundRect(renderDc_, panel, 18, RGB(66, 78, 96), 1);
            DrawTextCentered(renderDc_, L"What should Luna Capture do with this window?", RECT{panel.left + 16, panel.top + 10, panel.right - 16, panel.top + 48},
                             RGB(238, 242, 249), hintFont_);
            const wchar_t* labels[] = {L"Save to default folder", L"Copy to clipboard", L"Save + copy"};
            const COLORREF colors[] = {RGB(46, 87, 157), RGB(45, 101, 118), RGB(86, 65, 157)};
            for (int i = 0; i < 3; ++i) {
                const RECT button = WindowChoiceButton(i);
                ui::FillRoundRect(renderDc_, button, 11, colors[i]);
                DrawTextCentered(renderDc_, labels[i], button, RGB(250, 252, 255), sizeFont_);
            }
        } else if (settings_.showRegionGuide) {
            const RECT host = PrimaryMonitorClientRect();
            RECT message{host.left, (host.top + host.bottom) / 2 - 30, host.right, (host.top + host.bottom) / 2 + 38};
            DrawTextCentered(renderDc_, L"Hover a window and click to capture   •   Esc to cancel", message, RGB(245, 247, 252), hintFont_);
        }
    } else if (HasSelection()) {
        const RECT selection = NormalizedSelection();
        BitBlt(renderDc_, selection.left, selection.top, selection.right - selection.left,
               selection.bottom - selection.top, frameDc_, selection.left, selection.top, SRCCOPY);
        const int selectionState = SaveDC(renderDc_);
        if (selectionState) IntersectClipRect(renderDc_, selection.left, selection.top, selection.right, selection.bottom);
        DrawAnnotations(renderDc_, POINT{0, 0});
        // Outline whatever the Move tool is about to pick up, so it is obvious
        // that a drawing can be dragged on its own rather than with the region.
        if (tool_ == Tool::Move && movingAnnotation_ < 0 && hoveredAnnotation_ >= 0 &&
            hoveredAnnotation_ < static_cast<int>(annotations_.size())) {
            const Annotation& hovered = annotations_[static_cast<std::size_t>(hoveredAnnotation_)];
            RECT bounds{};
            if (!hovered.points.empty()) {
                bounds = RECT{hovered.points.front().x, hovered.points.front().y,
                              hovered.points.front().x, hovered.points.front().y};
                for (const POINT& p : hovered.points) {
                    bounds.left = (std::min)(bounds.left, p.x);
                    bounds.top = (std::min)(bounds.top, p.y);
                    bounds.right = (std::max)(bounds.right, p.x);
                    bounds.bottom = (std::max)(bounds.bottom, p.y);
                }
            } else {
                bounds = NormalizeRectPoints(hovered.start, hovered.end);
            }
            InflateRect(&bounds, 7, 7);
            ui::StrokeRoundRect(renderDc_, bounds, 5, RGB(150, 205, 255), 1, 190);
        }
        if (selectionState) RestoreDC(renderDc_, selectionState);

        ui::StrokeRoundRect(renderDc_, selection, 2, RGB(112, 181, 255), 2);
        if (tool_ == Tool::Move) DrawSelectionGrips(renderDc_, selection);

        wchar_t sizeText[64]{};
        swprintf_s(sizeText, L"%ld x %ld", selection.right - selection.left, selection.bottom - selection.top);
        RECT sizeRect{selection.left, selection.bottom + 8, selection.left + 120, selection.bottom + 36};
        if (sizeRect.bottom > client.bottom) { sizeRect.top = selection.top - 36; sizeRect.bottom = selection.top - 8; }
        ui::FillRoundRect(renderDc_, sizeRect, 9, RGB(20, 23, 29));
        ui::StrokeRoundRect(renderDc_, sizeRect, 9, RGB(64, 70, 84));
        DrawTextCentered(renderDc_, sizeText, sizeRect, RGB(224, 229, 238), sizeFont_);
        UpdateToolbarRect();
        DrawToolbar(renderDc_, selection);
        if (paletteOpen_) DrawPalette(renderDc_);
    } else if (settings_.showRegionGuide) {
        // Selection itself still works across every monitor - only the caption is
        // pinned to the primary one, because centring it on the virtual desktop
        // dropped it onto the seam between two screens.
        const RECT host = PrimaryMonitorClientRect();
        RECT message{host.left, (host.top + host.bottom) / 2 - 30, host.right, (host.top + host.bottom) / 2 + 38};
        DrawTextCentered(renderDc_, L"Drag to select   •   Ctrl+A full screen   •   Esc to cancel", message, RGB(245, 247, 252), hintFont_);
    }

    if (renderState) RestoreDC(renderDc_, renderState);
    BitBlt(target, present.left, present.top, width, height, renderDc_, present.left, present.top, SRCCOPY);
    EndPaint(hwnd_, &paint);
}

LRESULT CALLBACK RegionEditor::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    RegionEditor* self = reinterpret_cast<RegionEditor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<RegionEditor*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT RegionEditor::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: Paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_KEYDOWN: {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (wParam == VK_ESCAPE) {
                if (paletteOpen_) { ClosePalette(); return 0; }
                Complete(RegionResultAction::Cancel);
                return 0;
            }
            if (mode_ == SessionMode::WindowPicker && windowChoicePending_) {
                if (wParam == 'S') { Complete(RegionResultAction::Save); return 0; }
                if (wParam == 'C') { Complete(RegionResultAction::Copy); return 0; }
                if (wParam == 'B') { Complete(RegionResultAction::SaveAndCopy); return 0; }
            }
            if (ctrl && shift && wParam == 'Z') { Redo(); return 0; }
            if (ctrl && wParam == 'Y') { Redo(); return 0; }
            if (ctrl && wParam == 'Z') { Undo(); return 0; }
            if (ctrl && wParam == 'C' && HasSelection()) { Complete(RegionResultAction::Copy); return 0; }
            if (((ctrl && wParam == 'S') || wParam == 'S' || wParam == VK_RETURN) && HasSelection()) { ExecuteToolbar(ToolbarAction::Save); return 0; }
            if (ctrl && wParam == 'A') {
                selection_ = RECT{0, 0, static_cast<LONG>(frame_ ? frame_->width : 0), static_cast<LONG>(frame_ ? frame_->height : 0)};
                annotations_.clear();
                redoAnnotations_.clear();
                tool_ = Tool::Select;
                ClosePalette();
                UpdateToolbarRect();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wParam == '1' || wParam == 'V') { SetTool(Tool::Select); return 0; }
            if (wParam == '2' || wParam == 'M') { SetTool(Tool::Move); return 0; }
            if (wParam == '3' || wParam == 'A') { SetTool(Tool::Arrow); return 0; }
            if (wParam == '4' || wParam == 'R') { SetTool(Tool::Rectangle); return 0; }
            if (wParam == '5' || wParam == 'P') { SetTool(Tool::Pen); return 0; }
            if (wParam == '6' || wParam == 'H') { SetTool(Tool::Highlighter); return 0; }
            if (wParam == '7' || wParam == 'E') { SetTool(Tool::Eraser); return 0; }
            if (wParam == 'B') { SetColor(kSwatches[6]); return 0; }
            if (wParam == 'G') { SetColor(kSwatches[5]); return 0; }
            if (wParam == 'T') { SetColor(kSwatches[2]); return 0; }
            if (wParam == 'Y') { SetColor(kSwatches[4]); return 0; }
            if (wParam == 'W') { SetColor(kSwatches[0]); return 0; }
            if (tool_ == Tool::Highlighter && (wParam == VK_OEM_4 || wParam == VK_SUBTRACT)) {
                highlighterAlpha_ = static_cast<BYTE>((std::max)(38, highlighterAlpha_ - 28));
                InvalidateRect(hwnd_, nullptr, FALSE); return 0;
            }
            if (tool_ == Tool::Highlighter && (wParam == VK_OEM_6 || wParam == VK_ADD)) {
                highlighterAlpha_ = static_cast<BYTE>((std::min)(255, highlighterAlpha_ + 28));
                InvalidateRect(hwnd_, nullptr, FALSE); return 0;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (mode_ == SessionMode::WindowPicker) {
                if (windowChoicePending_) {
                    if (PointInside(WindowChoiceButton(0), point)) Complete(RegionResultAction::Save);
                    else if (PointInside(WindowChoiceButton(1), point)) Complete(RegionResultAction::Copy);
                    else if (PointInside(WindowChoiceButton(2), point)) Complete(RegionResultAction::SaveAndCopy);
                    return 0;
                }
                UpdateHoveredWindow(point);
                if (hoveredWindowRect_.right > hoveredWindowRect_.left &&
                    hoveredWindowRect_.bottom > hoveredWindowRect_.top) {
                    selection_ = hoveredWindowRect_;
                    filenamePrefix_ = GetWindowCaptureName(hoveredWindow_);
                    windowChoicePending_ = true;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }

            // Palette interaction takes priority while it is open.
            if (paletteOpen_ && PointInside(paletteRect_, point)) {
                int swatch = -1;
                const PaletteTarget target = PaletteControlAt(point, &swatch);
                if (target == PaletteTarget::Swatch && swatch >= 0) {
                    SetColor(kSwatches[swatch]);
                } else if (target != PaletteTarget::None) {
                    paletteDrag_ = target;
                    ApplyPalettePoint(target, point);
                }
                SetCapture(hwnd_);
                return 0;
            }

            const auto toolbar = HitTestToolbar(point);
            if (toolbar != ToolbarAction::None) {
                pressedToolbar_ = toolbar;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, &toolbarRect_, FALSE);
                return 0;
            }

            if (paletteOpen_) { ClosePalette(); return 0; }

            // Move grabs an existing region; anywhere outside it still starts a new
            // one, so "reframe this" and "select something else" both work without
            // switching tools. Order matters: the edge grips are small and sit on
            // the border, so they win over an annotation drawn along it; inside the
            // region, ink wins over the body so a single drawing can be nudged on
            // its own.
            const Grip grip = GripAt(point);
            if (grip != Grip::None && grip != Grip::Body) {
                activeGrip_ = grip;
                gripDragStart_ = point;
                gripStartSelection_ = NormalizedSelection();
                gripStartAnnotations_ = annotations_;
                drawingAnnotation_ = false;
                draggingSelection_ = false;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (tool_ == Tool::Move) {
                const int annotationIndex = AnnotationAt(point);
                if (annotationIndex >= 0) {
                    movingAnnotation_ = annotationIndex;
                    annotationDragStart_ = point;
                    annotationDragOrigin_ = annotations_[static_cast<std::size_t>(annotationIndex)];
                    drawingAnnotation_ = false;
                    draggingSelection_ = false;
                    SetCapture(hwnd_);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
            }
            if (grip == Grip::Body) {
                activeGrip_ = grip;
                gripDragStart_ = point;
                gripStartSelection_ = NormalizedSelection();
                gripStartAnnotations_ = annotations_;
                drawingAnnotation_ = false;
                draggingSelection_ = false;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            const RECT selection = NormalizedSelection();
            if (!HasSelection() || !PointInside(selection, point) || tool_ == Tool::Select || tool_ == Tool::Move) {
                const bool keepMoveTool = tool_ == Tool::Move;
                draggingSelection_ = true;
                drawingAnnotation_ = false;
                dragStart_ = point;
                dragCurrent_ = point;
                selection_ = {};
                annotations_.clear();
                redoAnnotations_.clear();
                if (!keepMoveTool) tool_ = Tool::Select;
                hoveredToolbar_ = ToolbarAction::None;
            } else if (tool_ == Tool::Eraser) {
                drawingAnnotation_ = true;
                currentAnnotation_ = {};
                currentAnnotation_.tool = tool_;
                currentAnnotation_.start = point;
                currentAnnotation_.end = point;
                EraseAt(point);
            } else {
                drawingAnnotation_ = true;
                currentAnnotation_ = {};
                currentAnnotation_.tool = tool_;
                currentAnnotation_.color = currentColor_;
                currentAnnotation_.alpha = tool_ == Tool::Highlighter ? highlighterAlpha_ : 255;
                currentAnnotation_.start = point;
                currentAnnotation_.end = point;
                currentAnnotation_.points.push_back(point);
            }
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&track);
            if (paletteDrag_ != PaletteTarget::None) {
                ApplyPalettePoint(paletteDrag_, point);
                return 0;
            }
            if (movingAnnotation_ >= 0) {
                ApplyAnnotationDrag(point);
                return 0;
            }
            if (activeGrip_ != Grip::None) {
                ApplyGripDrag(point);
                return 0;
            }
            if (mode_ == SessionMode::WindowPicker) {
                if (windowChoicePending_) return 0;
                RECT previous = hoveredWindowRect_;
                UpdateHoveredWindow(point);
                if (std::memcmp(&previous, &hoveredWindowRect_, sizeof(RECT)) != 0) InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (draggingSelection_) {
                dragCurrent_ = point;
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (drawingAnnotation_) {
                currentAnnotation_.end = point;
                if (currentAnnotation_.tool == Tool::Eraser) {
                    EraseAt(point);
                } else {
                    if (currentAnnotation_.tool == Tool::Pen || currentAnnotation_.tool == Tool::Highlighter) {
                        if (currentAnnotation_.points.empty() ||
                            std::abs(currentAnnotation_.points.back().x - point.x) + std::abs(currentAnnotation_.points.back().y - point.y) >= 2) {
                            currentAnnotation_.points.push_back(point);
                        }
                    }
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            } else {
                const ToolbarAction next = HitTestToolbar(point);
                if (next != hoveredToolbar_) {
                    hoveredToolbar_ = next;
                    InvalidateRect(hwnd_, &toolbarRect_, FALSE);
                }
                const Grip grip = next == ToolbarAction::None ? GripAt(point) : Grip::None;
                const int annotation = next == ToolbarAction::None && grip != Grip::Left && grip != Grip::Right &&
                                               grip != Grip::Top && grip != Grip::Bottom &&
                                               grip != Grip::TopLeft && grip != Grip::TopRight &&
                                               grip != Grip::BottomLeft && grip != Grip::BottomRight
                                           ? AnnotationAt(point)
                                           : -1;
                if (grip != hoveredGrip_ || annotation != hoveredAnnotation_) {
                    hoveredGrip_ = grip;
                    hoveredAnnotation_ = annotation;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            return 0;
        }
        case WM_SETCURSOR: {
            // Only the client area; the toolbar and palette keep the arrow.
            if (LOWORD(lParam) != HTCLIENT) break;
            if (movingAnnotation_ >= 0 || (activeGrip_ == Grip::None && hoveredAnnotation_ >= 0)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            const Grip grip = activeGrip_ != Grip::None ? activeGrip_ : hoveredGrip_;
            const wchar_t* cursor = nullptr;
            switch (grip) {
                case Grip::Body: cursor = IDC_SIZEALL; break;
                case Grip::Left: case Grip::Right: cursor = IDC_SIZEWE; break;
                case Grip::Top: case Grip::Bottom: cursor = IDC_SIZENS; break;
                case Grip::TopLeft: case Grip::BottomRight: cursor = IDC_SIZENWSE; break;
                case Grip::TopRight: case Grip::BottomLeft: cursor = IDC_SIZENESW; break;
                default: break;
            }
            if (!cursor) break;
            SetCursor(LoadCursorW(nullptr, cursor));
            return TRUE;
        }
        case WM_MOUSELEAVE:
            if (mode_ == SessionMode::WindowPicker) {
                hoveredWindow_ = nullptr;
                hoveredWindowRect_ = {};
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            if (hoveredToolbar_ != ToolbarAction::None) {
                hoveredToolbar_ = ToolbarAction::None;
                InvalidateRect(hwnd_, &toolbarRect_, FALSE);
            }
            if (hoveredAnnotation_ >= 0 || hoveredGrip_ != Grip::None) {
                hoveredAnnotation_ = -1;
                hoveredGrip_ = Grip::None;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (paletteDrag_ != PaletteTarget::None) {
                paletteDrag_ = PaletteTarget::None;
                ReleaseCapture();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (movingAnnotation_ >= 0) {
                ApplyAnnotationDrag(point);
                movingAnnotation_ = -1;
                hoveredAnnotation_ = AnnotationAt(point);
                ReleaseCapture();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (activeGrip_ != Grip::None) {
                ApplyGripDrag(point);
                activeGrip_ = Grip::None;
                gripStartAnnotations_.clear();
                hoveredGrip_ = GripAt(point);
                ReleaseCapture();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (pressedToolbar_ != ToolbarAction::None) {
                const ToolbarAction pressed = pressedToolbar_;
                pressedToolbar_ = ToolbarAction::None;
                ReleaseCapture();
                if (HitTestToolbar(point) == pressed) ExecuteToolbar(pressed);
                if (visible_) InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (draggingSelection_) {
                draggingSelection_ = false;
                dragCurrent_ = point;
                selection_ = NormalizeRectPoints(dragStart_, dragCurrent_);
                // Land on Move once a region exists: the common next action is
                // nudging or resizing what was just drawn, and requiring a trip to
                // the toolbar for that made the tool feel one-shot.
                if (tool_ == Tool::Select && HasSelection()) tool_ = Tool::Move;
                hoveredGrip_ = GripAt(point);
                UpdateToolbarRect();
            } else if (drawingAnnotation_) {
                drawingAnnotation_ = false;
                currentAnnotation_.end = point;
                if (currentAnnotation_.tool == Tool::Eraser) {
                    currentAnnotation_ = {};
                } else {
                    if (currentAnnotation_.tool == Tool::Pen || currentAnnotation_.tool == Tool::Highlighter) currentAnnotation_.points.push_back(point);
                    redoAnnotations_.clear();
                    annotations_.push_back(std::move(currentAnnotation_));
                    currentAnnotation_ = {};
                }
            }
            ReleaseCapture();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_RBUTTONDOWN:
            if (paletteOpen_) { ClosePalette(); return 0; }
            return 0;
        case WM_DESTROY:
            hwnd_ = nullptr;
            visible_ = false;
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}
