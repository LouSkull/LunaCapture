#include "OverlayWindow.hpp"

#include "CoreUtils.hpp"
#include "UiGfx.hpp"
#include "UiIcons.hpp"
#include "WinUtil.hpp"

#include <algorithm>

namespace {
constexpr wchar_t kOverlayClassName[] = L"LunaCaptureOverlay";
constexpr UINT_PTR kTimer = 1;
constexpr int kSlideInMs = 110;
constexpr int kFadeOutMs = 150;
constexpr int kCornerRadius = 16;

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void DrawLabel(HDC dc, const std::wstring& value, RECT rect, COLORREF color, HFONT font, UINT flags) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, value.c_str(), -1, &rect, flags | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
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
    titleFont_ = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    detailFont_ = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!titleFont_ || !detailFont_) {
        DestroyGraphics();
        return false;
    }
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
    DestroyGraphics();
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void OverlayWindow::ShowSuccess(const std::wstring& filename, double responseMs, int durationMs, HMONITOR monitor) {
    BeginShow(true, L"Screenshot saved", EllipsizeMiddle(filename, 36), responseMs, durationMs, monitor);
}

void OverlayWindow::ShowError(const std::wstring& message, int durationMs, HMONITOR monitor) {
    BeginShow(false, L"Capture failed", EllipsizeMiddle(message, 42), 0.0, durationMs, monitor);
}

void OverlayWindow::ShowMessage(const std::wstring& title, const std::wstring& detail, int durationMs, HMONITOR monitor) {
    BeginShow(true, title, EllipsizeMiddle(detail, 42), 0.0, durationMs, monitor);
}

void OverlayWindow::BeginShow(bool success, std::wstring title, std::wstring detail, double responseMs,
                              int durationMs, HMONITOR monitor) {
    if (!hwnd_) return;
    success_ = success;
    title_ = std::move(title);
    detail_ = std::move(detail);
    responseMs_ = responseMs;
    holdDurationMs_ = (std::max)(650, (std::min)(3000, durationMs));
    MONITORINFO info{sizeof(info)};
    if (!monitor) monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(monitor, &info);
    basePosition_.x = info.rcWork.right - width_ - 18;
    basePosition_.y = info.rcWork.top + 18;
    startedAt_ = GetTickCount64();
    opacity_ = 255;
    SetWindowPos(hwnd_, HWND_TOPMOST, basePosition_.x + 20, basePosition_.y, width_, height_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
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
    opacity_ = alpha;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, basePosition_.y, 0, 0,
                  SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
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
    const COLORREF accentColor = success_ ? RGB(76, 201, 142) : RGB(244, 101, 112);
    ui::FillRoundRect(backBufferDc_, RECT{7, 14, 11, client.bottom - 14}, 2, accentColor);

    // Soft circular badge behind the status glyph.
    ui::FillEllipse(backBufferDc_, RECT{22, 17, 56, 51},
                    ui::MixColor(RGB(20, 23, 29), accentColor, 40.0f / 255.0f));
    DrawUiIcon(backBufferDc_, success_ ? UiIcon::Check : UiIcon::Close,
               RECT{30, 25, 48, 43}, accentColor, 2);

    RECT titleRect{66, 9, width_ - 102, 34};
    DrawLabel(backBufferDc_, title_, titleRect, RGB(244, 246, 250), titleFont_, DT_LEFT);
    RECT detailRect{66, 32, width_ - 16, 59};
    DrawLabel(backBufferDc_, detail_, detailRect, RGB(150, 157, 170), detailFont_, DT_LEFT);
    if (success_ && responseMs_ > 0.0) {
        RECT latency{width_ - 90, 9, width_ - 14, 34};
        DrawLabel(backBufferDc_, FormatMilliseconds(responseMs_), latency, RGB(113, 170, 255), detailFont_, DT_RIGHT);
    }
    ui::PresentRoundedLayeredWindow(hwnd_, backBufferDc_, backBufferPixels_,
                                    client.right, client.bottom, kCornerRadius, opacity_);
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
