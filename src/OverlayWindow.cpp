#include "OverlayWindow.hpp"
#include "CapturePrivacy.hpp"

#include "CoreUtils.hpp"
#include "UiGfx.hpp"
#include "UiIcons.hpp"
#include "WinUtil.hpp"

#include <dwmapi.h>

#include <algorithm>

namespace {
constexpr wchar_t kOverlayClassName[] = L"LunaCaptureOverlay";
constexpr UINT_PTR kTimer = 1;
constexpr int kCornerRadius = 10;

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
    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions, sizeof(disableTransitions));
    if (!CreateGraphics()) {
        Destroy();
        return false;
    }
    // Let the toast be picked up by screen capture — it must be visible in
    // screenshots, Discord streams and OBS. (WDA_EXCLUDEFROMCAPTURE hid it and the
    // WDA_MONITOR fallback painted it solid black in captured images.)
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
    HFONT title = ui::CreateThemeFont(16, FW_SEMIBOLD);
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
    const bool useOpaqueFallback = fullscreen || privateCapture_;
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

bool OverlayWindow::SetCapturePrivacy(bool enabled, std::wstring& error) {
    Hide();
    privateCapture_ = enabled;
    // Per-pixel UpdateLayeredWindow surfaces do not reliably support affinity.
    ConfigurePresentationMode(fullscreenPresentation_);
    return capturePrivacy::Apply(hwnd_, enabled, L"Notification", error);
}

void OverlayWindow::ShowSuccess(const std::wstring& filename, int durationMs, HMONITOR monitor) {
    BeginShow(OverlayStatus::Success, L"Screenshot saved", EllipsizeMiddle(filename, 36), durationMs, monitor);
}

void OverlayWindow::ShowError(const std::wstring& message, int durationMs, HMONITOR monitor) {
    BeginShow(OverlayStatus::Error, L"Capture failed", EllipsizeMiddle(message, 42), durationMs, monitor);
}

void OverlayWindow::ShowMessage(const std::wstring& title, const std::wstring& detail, int durationMs, HMONITOR monitor) {
    BeginShow(OverlayStatus::Success, title, EllipsizeMiddle(detail, 42), durationMs, monitor);
}

void OverlayWindow::ShowWarning(const std::wstring& title, const std::wstring& detail, int durationMs, HMONITOR monitor) {
    BeginShow(OverlayStatus::Warning, title, EllipsizeMiddle(detail, 42), durationMs, monitor);
}

void OverlayWindow::BeginShow(OverlayStatus status, std::wstring title, std::wstring detail,
                              int durationMs, HMONITOR monitor) {
    if (!hwnd_) return;
    status_ = status;
    title_ = std::move(title);
    detail_ = std::move(detail);
    MONITORINFO info{sizeof(info)};
    if (!monitor) monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(monitor, &info)) return;
    fullscreenPresentation_ = IsForegroundFullscreenOnMonitor(info, hwnd_);
    ConfigurePresentationMode(fullscreenPresentation_);
    std::wstring privacyError;
    if (!capturePrivacy::Apply(hwnd_, privateCapture_, L"Notification", privacyError)) {
        AppendDiagnosticLog(privacyError);
        Hide();
        return;
    }
    const RECT placement = fullscreenPresentation_ ? info.rcMonitor : info.rcWork;
    const int margin = fullscreenPresentation_ ? 26 : 18;
    const int x = placement.right - width_ - margin;
    const int y = placement.top + margin;
    KillTimer(hwnd_, kTimer);
    const UINT duration = static_cast<UINT>(std::clamp(durationMs, 650, 4200));
    expiresAt_ = GetTickCount64() + duration;

    // Rebind the z-order instead of trusting a stale TOPMOST band after an exclusive/independent-flip
    // transition. SW_SHOWNOACTIVATE and WS_EX_NOACTIVATE preserve the game's foreground ownership.
    // Always drop out of and back into the top-most band so the toast is
    // re-inserted above other top-most windows (borderless-fullscreen games,
    // other overlays) instead of trusting a stale band position.
    SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    if (fullscreenPresentation_) DwmFlush();
    if (!SetTimer(hwnd_, kTimer, duration, nullptr)) Hide();
}

void OverlayWindow::Hide() {
    if (!hwnd_) return;
    KillTimer(hwnd_, kTimer);
    ShowWindow(hwnd_, SW_HIDE);
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
    const COLORREF accentColor = status_ == OverlayStatus::Success ? RGB(76, 201, 142) :
                                 status_ == OverlayStatus::Warning ? RGB(246, 180, 72) : RGB(244, 101, 112);
    const UiIcon statusIcon = status_ == OverlayStatus::Success ? UiIcon::Check :
                              status_ == OverlayStatus::Warning ? UiIcon::Warning : UiIcon::Close;
    ui::FillRoundRect(backBufferDc_, RECT{8, 14, 12, client.bottom - 14}, 2, accentColor);
    DrawUiIcon(backBufferDc_, statusIcon, RECT{22, 23, 42, 43}, accentColor, 2);
    DrawLabel(backBufferDc_, title_, RECT{54, 9, width_ - 12, 33}, RGB(244, 246, 250), titleFont_, DT_LEFT);
    DrawLabel(backBufferDc_, detail_, RECT{54, 34, width_ - 12, 58}, RGB(150, 157, 170), detailFont_, DT_LEFT);
    if (opaqueFullscreenFallback_) {
        BitBlt(target, 0, 0, client.right, client.bottom, backBufferDc_, 0, 0, SRCCOPY);
    } else {
        ui::PresentRoundedLayeredWindow(hwnd_, backBufferDc_, backBufferPixels_,
                                        client.right, client.bottom, kCornerRadius, 255);
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
        case WM_TIMER:
            if (wParam == kTimer) {
                if (GetTickCount64() >= expiresAt_) Hide();
                return 0;
            }
            break;
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        default: break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}
