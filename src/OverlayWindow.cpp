#include "OverlayWindow.hpp"

#include "CoreUtils.hpp"
#include "UiGfx.hpp"
#include "UiIcons.hpp"
#include "WinUtil.hpp"

#include <dwmapi.h>

#include <algorithm>

namespace {
constexpr wchar_t kOverlayClassName[] = L"LunaCaptureOverlay";
constexpr UINT_PTR kTimer = 1;
constexpr int kSlideInMs = 110;
constexpr int kFadeOutMs = 150;
constexpr int kCornerRadius = 16;

bool IsForegroundFullscreenOnMonitor(const MONITORINFO& monitorInfo, HWND overlay) {
    const HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == overlay || !IsWindowVisible(foreground) || IsIconic(foreground)) return false;
    RECT bounds{};
    if (FAILED(DwmGetWindowAttribute(foreground, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) &&
        !GetWindowRect(foreground, &bounds)) return false;
    constexpr int kTolerance = 8;
    return bounds.left <= monitorInfo.rcMonitor.left + kTolerance &&
           bounds.top <= monitorInfo.rcMonitor.top + kTolerance &&
           bounds.right >= monitorInfo.rcMonitor.right - kTolerance &&
           bounds.bottom >= monitorInfo.rcMonitor.bottom - kTolerance;
}

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(ui::ResolveThemeColor(color));
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void DrawLabel(HDC dc, const std::wstring& value, RECT rect, COLORREF color, HFONT font, UINT flags) {
    const std::wstring translated = ui::TranslateText(value);
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ui::ResolveThemeColor(color));
    DrawTextW(dc, translated.c_str(), -1, &rect, flags | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}
}

OverlayWindow::~OverlayWindow() { Destroy(); }

bool OverlayWindow::Create(HINSTANCE instance) {
    instance_ = instance;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &OverlayWindow::WndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClassName;
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                            kOverlayClassName, L"Luna Capture", WS_POPUP, 0, 0, width_, height_,
                            nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;
    if (!CreateGraphics()) {
        Destroy();
        return false;
    }
    // Let the toast be picked up by screen capture — it must be visible in
    // recordings, Discord streams and OBS. (WDA_EXCLUDEFROMCAPTURE hid it and the
    // WDA_MONITOR fallback painted it solid black in captured video.)
    SetWindowDisplayAffinity(hwnd_, WDA_NONE);
    return true;
}

bool OverlayWindow::CreateGraphics() {
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    backBufferDc_ = CreateCompatibleDC(screen);
    backBufferBitmap_ = ui::CreateLayeredDib(width_, height_, &backBufferPixels_);
    ReleaseDC(nullptr, screen);
    if (!backBufferDc_ || !backBufferBitmap_) {
        DestroyGraphics();
        return false;
    }
    backBufferOldBitmap_ = SelectObject(backBufferDc_, backBufferBitmap_);
    if (!RecreateFonts()) {
        DestroyGraphics();
        return false;
    }
    return true;
}

bool OverlayWindow::RecreateFonts() {
    HFONT title = ui::CreateThemeFont(15, FW_SEMIBOLD);
    HFONT detail = ui::CreateThemeFont(13, FW_NORMAL);
    if (!title || !detail) {
        if (title) DeleteObject(title);
        if (detail) DeleteObject(detail);
        return false;
    }

    HFONT oldTitle = titleFont_;
    HFONT oldDetail = detailFont_;
    titleFont_ = title;
    detailFont_ = detail;
    if (oldTitle) DeleteObject(oldTitle);
    if (oldDetail) DeleteObject(oldDetail);
    return true;
}

void OverlayWindow::DestroyGraphics() {
    if (backBufferDc_ && backBufferOldBitmap_) {
        SelectObject(backBufferDc_, backBufferOldBitmap_);
        backBufferOldBitmap_ = nullptr;
    }
    if (backBufferBitmap_) { DeleteObject(backBufferBitmap_); backBufferBitmap_ = nullptr; }
    backBufferPixels_ = nullptr;
    if (backBufferDc_) { DeleteDC(backBufferDc_); backBufferDc_ = nullptr; }
    if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
    if (detailFont_) { DeleteObject(detailFont_); detailFont_ = nullptr; }
}

void OverlayWindow::Destroy() {
    if (hwnd_ && IsWindow(hwnd_)) KillTimer(hwnd_, kTimer);
    DestroyGraphics();
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void OverlayWindow::RefreshTheme() {
    RecreateFonts();
    ApplyOpaqueWindowRegion();
    if (hwnd_ && IsWindowVisible(hwnd_)) {
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
}

void OverlayWindow::ApplyOpaqueWindowRegion() {
    if (!hwnd_ || !opaqueFullscreenFallback_) return;
    const int radius = (std::min)(ui::ResolveThemeRadius(kCornerRadius),
                                  (std::min)(width_, height_) / 2);
    HRGN region = radius > 0
        ? CreateRoundRectRgn(0, 0, width_ + 1, height_ + 1, radius * 2, radius * 2)
        : CreateRectRgn(0, 0, width_, height_);
    if (region && !SetWindowRgn(hwnd_, region, FALSE)) DeleteObject(region);
}

void OverlayWindow::ConfigurePresentationMode(bool fullscreen) {
    if (!hwnd_) return;
    const bool useOpaqueFallback = fullscreen;
    if (opaqueFullscreenFallback_ == useOpaqueFallback) return;

    ShowWindow(hwnd_, SW_HIDE);
    LONG_PTR extendedStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    extendedStyle |= WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    if (useOpaqueFallback) extendedStyle &= ~static_cast<LONG_PTR>(WS_EX_LAYERED);
    else extendedStyle |= WS_EX_LAYERED;
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, extendedStyle);

    if (useOpaqueFallback) {
        opaqueFullscreenFallback_ = true;
        ApplyOpaqueWindowRegion();
    } else {
        SetWindowRgn(hwnd_, nullptr, FALSE);
        opaqueFullscreenFallback_ = false;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, width_, height_,
                 SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
}

void OverlayWindow::ShowSuccess(const std::wstring& filename, double responseMs, int durationMs, HMONITOR monitor) {
    BeginShow(OverlayStatus::Success, L"Screenshot saved", EllipsizeMiddle(filename, 36), responseMs, durationMs, monitor);
}

void OverlayWindow::ShowError(const std::wstring& message, int durationMs, HMONITOR monitor) {
    BeginShow(OverlayStatus::Error, L"Capture failed", EllipsizeMiddle(message, 42), 0.0, durationMs, monitor);
}

void OverlayWindow::ShowMessage(const std::wstring& title, const std::wstring& detail, int durationMs, HMONITOR monitor) {
    BeginShow(OverlayStatus::Success, title, EllipsizeMiddle(detail, 42), 0.0, durationMs, monitor);
}

void OverlayWindow::ShowWarning(const std::wstring& title, const std::wstring& detail, int durationMs, HMONITOR monitor) {
    BeginShow(OverlayStatus::Warning, title, EllipsizeMiddle(detail, 42), 0.0, durationMs, monitor);
}

void OverlayWindow::BeginShow(OverlayStatus status, std::wstring title, std::wstring detail, double responseMs,
                              int durationMs, HMONITOR monitor) {
    if (!hwnd_) return;
    // A normal window is never composited over true exclusive fullscreen (DWM stops compositing
    // that surface entirely) — showing it anyway just fights Windows for a result nobody will
    // see. Detect that state and skip the visual toast outright instead of trying.
    if (IsExclusiveFullscreenAppActive()) return;
    status_ = status;
    title_ = std::move(title);
    detail_ = std::move(detail);
    responseMs_ = responseMs;
    MONITORINFO info{sizeof(info)};
    if (!monitor) monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(monitor, &info)) return;
    fullscreenPresentation_ = IsForegroundFullscreenOnMonitor(info, hwnd_);
    ConfigurePresentationMode(fullscreenPresentation_);
    holdDurationMs_ = fullscreenPresentation_
        ? (std::max)(1800, (std::min)(4200, durationMs))
        : (std::max)(650, (std::min)(3000, durationMs));
    const RECT placement = fullscreenPresentation_ ? info.rcMonitor : info.rcWork;
    basePosition_.x = placement.right - width_ - (fullscreenPresentation_ ? 26 : 18);
    basePosition_.y = placement.top + (fullscreenPresentation_ ? 26 : 18);
    startedAt_ = GetTickCount64();
    opacity_ = 255;

    // Rebind the z-order instead of trusting a stale TOPMOST band after an exclusive/independent-flip
    // transition. SW_SHOWNOACTIVATE and WS_EX_NOACTIVATE preserve the game's foreground ownership.
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    // Always drop out of and back into the top-most band so the toast is
    // re-inserted above other top-most windows (borderless-fullscreen games,
    // other overlays) instead of trusting a stale band position.
    SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SetWindowPos(hwnd_, HWND_TOPMOST, basePosition_.x + 20, basePosition_.y, width_, height_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    if (fullscreenPresentation_) DwmFlush();
    SetTimer(hwnd_, kTimer, 16, nullptr);
}

void OverlayWindow::Hide() {
    if (!hwnd_) return;
    KillTimer(hwnd_, kTimer);
    ShowWindow(hwnd_, SW_HIDE);
}

void OverlayWindow::Tick() {
    const int elapsed = static_cast<int>(GetTickCount64() - startedAt_);
    const int fadeStart = kSlideInMs + holdDurationMs_;
    if (elapsed >= fadeStart + kFadeOutMs) { Hide(); return; }
    int x = basePosition_.x;
    BYTE alpha = 255;
    if (elapsed < kSlideInMs) {
        const double t = static_cast<double>(elapsed) / kSlideInMs;
        const double eased = 1.0 - (1.0 - t) * (1.0 - t);
        x = basePosition_.x + static_cast<int>((1.0 - eased) * 20.0);
    } else if (elapsed > fadeStart) {
        const double t = static_cast<double>(elapsed - fadeStart) / kFadeOutMs;
        alpha = static_cast<BYTE>((std::max)(0.0, 1.0 - t) * 255.0);
        x = basePosition_.x + static_cast<int>(t * 8.0);
    }
    opacity_ = opaqueFullscreenFallback_ ? 255 : alpha;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, basePosition_.y, 0, 0,
                  SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
    if (fullscreenPresentation_) DwmFlush();
}

void OverlayWindow::Paint() {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(hwnd_, &paint);
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (!backBufferDc_) {
        Fill(target, client, RGB(20, 23, 29));
        EndPaint(hwnd_, &paint);
        return;
    }

    Fill(backBufferDc_, client, RGB(20, 23, 29));
    ui::MakeLayeredSurfaceOpaque(backBufferPixels_, client.right, client.bottom);
    ui::PaintThemeBackdrop(backBufferDc_, client, ui::ThemeSurface::Overlay, GetTickCount64());
    const COLORREF accentColor = status_ == OverlayStatus::Success ? RGB(76, 201, 142) :
                                 status_ == OverlayStatus::Warning ? RGB(246, 180, 72) : RGB(244, 101, 112);
    const UiIcon statusIcon = status_ == OverlayStatus::Success ? UiIcon::Check :
                              status_ == OverlayStatus::Warning ? UiIcon::Warning : UiIcon::Close;
    ui::FillRoundRect(backBufferDc_, RECT{7, 14, 11, client.bottom - 14}, 2, accentColor);

    // Soft circular badge behind the status glyph.
    ui::FillEllipse(backBufferDc_, RECT{22, 17, 56, 51},
                    ui::MixColor(RGB(20, 23, 29), accentColor, 40.0f / 255.0f));
    DrawUiIcon(backBufferDc_, statusIcon, RECT{30, 25, 48, 43}, accentColor, 2);

    RECT titleRect{66, 9, width_ - 102, 34};
    DrawLabel(backBufferDc_, title_, titleRect, RGB(244, 246, 250), titleFont_, DT_LEFT);
    RECT detailRect{66, 32, width_ - 16, 59};
    DrawLabel(backBufferDc_, detail_, detailRect, RGB(150, 157, 170), detailFont_, DT_LEFT);
    if (status_ == OverlayStatus::Success && responseMs_ > 0.0) {
        RECT latency{width_ - 90, 9, width_ - 14, 34};
        DrawLabel(backBufferDc_, FormatMilliseconds(responseMs_), latency, RGB(113, 170, 255), detailFont_, DT_RIGHT);
    }
    if (opaqueFullscreenFallback_) {
        BitBlt(target, 0, 0, client.right, client.bottom, backBufferDc_, 0, 0, SRCCOPY);
    } else {
        ui::PresentRoundedLayeredWindow(hwnd_, backBufferDc_, backBufferPixels_,
                                        client.right, client.bottom, kCornerRadius, opacity_);
    }
    EndPaint(hwnd_, &paint);
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    OverlayWindow* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<OverlayWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT OverlayWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: Paint(); return 0;
        case WM_TIMER: if (wParam == kTimer) { Tick(); return 0; } break;
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        default: break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}
