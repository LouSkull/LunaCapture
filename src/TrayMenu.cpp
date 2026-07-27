#include "TrayMenu.hpp"

#include "UiGfx.hpp"
#include "WinUtil.hpp"

#include <windowsx.h>
#include <algorithm>

namespace {
constexpr wchar_t kTrayMenuClass[] = L"LunaCaptureDarkTrayMenu";
constexpr int kTopPadding = 5;
constexpr int kHeaderHeight = 38;
constexpr int kBottomPadding = 5;
constexpr int kSeparatorHeight = 5;
constexpr int kCornerRadius = 16;

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void FillRound(HDC dc, const RECT& rect, int radius, COLORREF color) {
    ui::FillRoundRect(dc, rect, radius, color);
}

void StrokeRound(HDC dc, const RECT& rect, int radius, COLORREF color) {
    ui::StrokeRoundRect(dc, rect, radius, color, 1);
}

void Text(HDC dc, const std::wstring& value, RECT rect, COLORREF color, HFONT font, UINT flags) {
    const HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, value.c_str(), -1, &rect, flags | DT_NOPREFIX | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, oldFont);
}
}

TrayMenu::~TrayMenu() { Destroy(); }

bool TrayMenu::Create(HINSTANCE instance, HWND notifyWindow) {
    instance_ = instance;
    notifyWindow_ = notifyWindow;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &TrayMenu::WndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kTrayMenuClass;
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, kTrayMenuClass, L"Luna Capture",
                            WS_POPUP, 0, 0, width_, 100, nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;
    titleFont_ = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    normalFont_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    smallFont_ = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!titleFont_ || !normalFont_ || !smallFont_) {
        Destroy();
        return false;
    }
    return true;
}

void TrayMenu::ReleaseGraphics() {
    if (backBufferDc_ && backBufferOldBitmap_) {
        SelectObject(backBufferDc_, backBufferOldBitmap_);
        backBufferOldBitmap_ = nullptr;
    }
    if (backBufferBitmap_) { DeleteObject(backBufferBitmap_); backBufferBitmap_ = nullptr; }
    backBufferPixels_ = nullptr;
    if (backBufferDc_) { DeleteDC(backBufferDc_); backBufferDc_ = nullptr; }
    backBufferWidth_ = 0;
    backBufferHeight_ = 0;
}

bool TrayMenu::EnsureBackBuffer(int width, int height) {
    if (backBufferDc_ && backBufferBitmap_ && width == backBufferWidth_ && height == backBufferHeight_) return true;
    ReleaseGraphics();
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    backBufferDc_ = CreateCompatibleDC(screen);
    backBufferBitmap_ = ui::CreateLayeredDib(width, height, &backBufferPixels_);
    ReleaseDC(nullptr, screen);
    if (!backBufferDc_ || !backBufferBitmap_) {
        ReleaseGraphics();
        return false;
    }
    backBufferOldBitmap_ = SelectObject(backBufferDc_, backBufferBitmap_);
    backBufferWidth_ = width;
    backBufferHeight_ = height;
    return true;
}

void TrayMenu::Destroy() {
    ReleaseGraphics();
    if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
    if (normalFont_) { DeleteObject(normalFont_); normalFont_ = nullptr; }
    if (smallFont_) { DeleteObject(smallFont_); smallFont_ = nullptr; }
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

RECT TrayMenu::ItemRect(int index) const {
    int y = kTopPadding + kHeaderHeight;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (items_[i].separatorBefore) y += kSeparatorHeight;
        RECT rect{8, y, width_ - 8, y + rowHeight_};
        if (i == index) return rect;
        y += rowHeight_;
    }
    return {};
}

int TrayMenu::HitTest(POINT point) const {
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const RECT rect = ItemRect(i);
        if (PtInRect(&rect, point)) return i;
    }
    return -1;
}

void TrayMenu::Show(POINT screenPoint, const AppSettings& settings) {
    items_ = {
        {TrayCommand::Open, UiIcon::Home, L"Open Luna Capture", L"", false, false},
        {TrayCommand::CaptureActive, UiIcon::Monitor, L"Active display", FormatHotkey(settings.activeDisplayHotkey.modifiers, settings.activeDisplayHotkey.vk), true, false},
        {TrayCommand::CaptureAll, UiIcon::AllDisplays, L"All displays", FormatHotkey(settings.allDisplaysHotkey.modifiers, settings.allDisplaysHotkey.vk), false, false},
        {TrayCommand::CaptureRegion, UiIcon::Region, L"Region editor", FormatHotkey(settings.regionHotkey.modifiers, settings.regionHotkey.vk), false, false},
        {TrayCommand::CaptureWindow, UiIcon::Window, L"Active window", FormatHotkey(settings.activeWindowHotkey.modifiers, settings.activeWindowHotkey.vk), false, false},
        {TrayCommand::OpenFolder, UiIcon::Folder, L"Screenshot folder", L"", true, false},
        {TrayCommand::ToggleSound, UiIcon::Sound, L"Capture sound", L"", true, settings.sound},
        {TrayCommand::ToggleNotifications, UiIcon::Bell, L"Notifications", L"", false, settings.notifications},
        {TrayCommand::Exit, UiIcon::Power, L"Exit", L"", true, false},
    };
    int height = kTopPadding + kHeaderHeight + kBottomPadding + static_cast<int>(items_.size()) * rowHeight_;
    for (const auto& item : items_) if (item.separatorBefore) height += kSeparatorHeight;
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST), &monitor);
    int x = screenPoint.x;
    int y = screenPoint.y - height;
    if (x + width_ > monitor.rcWork.right) x = monitor.rcWork.right - width_ - 6;
    if (x < monitor.rcWork.left) x = monitor.rcWork.left + 6;
    if (y < monitor.rcWork.top) y = screenPoint.y + 4;
    hovered_ = -1;
    pressed_ = -1;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height, SWP_SHOWWINDOW);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void TrayMenu::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    hovered_ = pressed_ = -1;
}

void TrayMenu::Paint() {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(hwnd_, &paint);
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (!EnsureBackBuffer(width, height)) {
        Fill(target, client, RGB(15, 17, 22));
        EndPaint(hwnd_, &paint);
        return;
    }

    // Paint the whole opaque source with the shell color. The final layered
    // alpha mask removes the outside corners, so no dark rectangular wedges
    // can leak between the shell and the Windows 10 window shape.
    Fill(backBufferDc_, client, RGB(18, 21, 27));
    ui::MakeLayeredSurfaceOpaque(backBufferPixels_, width, height);
    RECT shell{0, 0, width, height};
    FillRound(backBufferDc_, shell, kCornerRadius, RGB(18, 21, 27));
    StrokeRound(backBufferDc_, RECT{0, 0, width - 1, height - 1}, kCornerRadius, RGB(55, 61, 73));

    RECT header{6, 6, width - 6, kTopPadding + kHeaderHeight - 2};
    FillRound(backBufferDc_, header, 12, RGB(24, 28, 36));
    RECT logoBox{13, 11, 35, 33};
    ui::FillRoundRectVGradient(backBufferDc_, logoBox, 7, RGB(122, 162, 255), RGB(78, 116, 232));
    DrawUiIcon(backBufferDc_, UiIcon::Capture, RECT{17, 15, 31, 29}, RGB(255, 255, 255), 2);
    RECT titleRect{43, 6, width - 68, 36};
    Text(backBufferDc_, L"Luna Capture", titleRect, RGB(244, 246, 251), titleFont_, DT_LEFT);
    RECT versionRect{width - 68, 6, width - 12, 36};
    Text(backBufferDc_, L"1.0.0.0", versionRect, RGB(105, 112, 126), smallFont_, DT_RIGHT);

    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const RECT rect = ItemRect(i);
        if (items_[i].separatorBefore) {
            RECT line{13, rect.top - 4, width - 13, rect.top - 3};
            Fill(backBufferDc_, line, RGB(43, 47, 57));
        }
        if (i == hovered_) FillRound(backBufferDc_, rect, 9, RGB(32, 37, 47));
        if (i == pressed_) FillRound(backBufferDc_, rect, 9, RGB(40, 46, 58));

        RECT iconRect{rect.left + 10, rect.top + 7, rect.left + 26, rect.bottom - 7};
        const COLORREF iconColor = i == hovered_ ? RGB(123, 176, 255) : RGB(177, 185, 199);
        DrawUiIcon(backBufferDc_, items_[i].icon, iconRect, iconColor, 2);

        const bool hasShortcut = !items_[i].shortcut.empty();
        RECT label{rect.left + 34, rect.top, hasShortcut ? rect.left + 168 : rect.right - 48, rect.bottom};
        Text(backBufferDc_, items_[i].label, label, RGB(231, 234, 241), normalFont_, DT_LEFT | DT_END_ELLIPSIS);

        if (hasShortcut) {
            RECT shortcut{rect.left + 170, rect.top, rect.right - 10, rect.bottom};
            Text(backBufferDc_, items_[i].shortcut, shortcut, RGB(122, 130, 145), smallFont_, DT_RIGHT | DT_END_ELLIPSIS);
        } else if (items_[i].command == TrayCommand::ToggleSound || items_[i].command == TrayCommand::ToggleNotifications) {
            RECT toggle{rect.right - 40, rect.top + 7, rect.right - 10, rect.bottom - 7};
            ui::FillRoundRect(backBufferDc_, toggle, (toggle.bottom - toggle.top) / 2,
                              items_[i].checked ? RGB(70, 122, 226) : RGB(52, 57, 69));
            const int knobD = toggle.bottom - toggle.top - 6;
            const int knobX = items_[i].checked ? toggle.right - 3 - knobD : toggle.left + 3;
            ui::FillEllipse(backBufferDc_, RECT{knobX, toggle.top + 3, knobX + knobD, toggle.top + 3 + knobD}, RGB(247, 249, 252));
        }
    }

    ui::PresentRoundedLayeredWindow(hwnd_, backBufferDc_, backBufferPixels_,
                                    width, height, kCornerRadius);
    EndPaint(hwnd_, &paint);
}

LRESULT CALLBACK TrayMenu::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    TrayMenu* self = reinterpret_cast<TrayMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<TrayMenu*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT TrayMenu::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: Paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_NCHITTEST: {
            RECT client{};
            GetClientRect(hwnd_, &client);
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &point);
            if (!ui::PointInRoundedRect(point, client.right, client.bottom, kCornerRadius)) {
                return HTTRANSPARENT;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int next = HitTest(point);
            if (next != hovered_) { hovered_ = next; InvalidateRect(hwnd_, nullptr, FALSE); }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hovered_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            pressed_ = HitTest(point);
            SetCapture(hwnd_);
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int released = HitTest(point);
            ReleaseCapture();
            if (pressed_ >= 0 && pressed_ == released) {
                const int activatedIndex = pressed_;
                const TrayCommand command = items_[static_cast<std::size_t>(activatedIndex)].command;
                const bool persistentToggle = command == TrayCommand::ToggleSound ||
                                              command == TrayCommand::ToggleNotifications;
                if (persistentToggle) {
                    items_[static_cast<std::size_t>(activatedIndex)].checked =
                        !items_[static_cast<std::size_t>(activatedIndex)].checked;
                    pressed_ = -1;
                    hovered_ = activatedIndex;
                    RECT toggleRect = ItemRect(activatedIndex);
                    InvalidateRect(hwnd_, &toggleRect, FALSE);
                    PostMessageW(notifyWindow_, WM_APP_TRAY_COMMAND, static_cast<WPARAM>(command), 0);
                    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    return 0;
                }
                Hide();
                PostMessageW(notifyWindow_, WM_APP_TRAY_COMMAND, static_cast<WPARAM>(command), 0);
            }
            pressed_ = -1;
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) Hide();
            return 0;
        case WM_KILLFOCUS:
            Hide();
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) Hide();
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}
