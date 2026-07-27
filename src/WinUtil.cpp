#include "WinUtil.hpp"
#include "CoreUtils.hpp"
#include "UiGfx.hpp"
#include "UiIcons.hpp"

#include <ShlObj.h>
#include <ShObjIdl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>

namespace {
std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) || raw == nullptr) return {};
    std::wstring result(raw);
    CoTaskMemFree(raw);
    return result;
}

std::wstring VirtualKeyName(UINT vk) {
    switch (vk) {
        case VK_SNAPSHOT: return L"Print Screen";
        case VK_SPACE: return L"Space";
        case VK_RETURN: return L"Enter";
        case VK_ESCAPE: return L"Esc";
        case VK_TAB: return L"Tab";
        case VK_BACK: return L"Backspace";
        case VK_DELETE: return L"Delete";
        case VK_INSERT: return L"Insert";
        case VK_HOME: return L"Home";
        case VK_END: return L"End";
        case VK_PRIOR: return L"Page Up";
        case VK_NEXT: return L"Page Down";
        case VK_LEFT: return L"Left";
        case VK_RIGHT: return L"Right";
        case VK_UP: return L"Up";
        case VK_DOWN: return L"Down";
        case VK_VOLUME_MUTE: return L"Volume Mute";
        case VK_VOLUME_DOWN: return L"Volume Down";
        case VK_VOLUME_UP: return L"Volume Up";
        case VK_MEDIA_NEXT_TRACK: return L"Media Next";
        case VK_MEDIA_PREV_TRACK: return L"Media Previous";
        case VK_MEDIA_STOP: return L"Media Stop";
        case VK_MEDIA_PLAY_PAUSE: return L"Play / Pause";
        case VK_BROWSER_BACK: return L"Browser Back";
        case VK_BROWSER_FORWARD: return L"Browser Forward";
        case VK_BROWSER_REFRESH: return L"Browser Refresh";
        case VK_BROWSER_STOP: return L"Browser Stop";
        case VK_BROWSER_SEARCH: return L"Browser Search";
        case VK_BROWSER_FAVORITES: return L"Browser Favorites";
        case VK_BROWSER_HOME: return L"Browser Home";
        case VK_LAUNCH_MAIL: return L"Launch Mail";
        case VK_LAUNCH_MEDIA_SELECT: return L"Launch Media";
        case VK_LAUNCH_APP1: return L"Launch App 1";
        case VK_LAUNCH_APP2: return L"Launch App 2";
        default: break;
    }
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t buffer[96]{};
    if (GetKeyNameTextW(static_cast<LONG>(scan << 16), buffer, static_cast<int>(std::size(buffer))) > 0) {
        return buffer;
    }
    return L"VK " + std::to_wstring(vk);
}

bool IsUsableForegroundWindow(HWND window, HWND applicationWindow) {
    if (!window || window == applicationWindow || !IsWindowVisible(window) || IsIconic(window)) return false;
    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0 ||
        wcscmp(className, L"Shell_TrayWnd") == 0) return false;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    return true;
}

constexpr wchar_t kNumericDialogClass[] = L"LunaCaptureNumericDialog";
constexpr wchar_t kChoiceDialogClass[] = L"LunaCaptureChoiceDialog";
constexpr int kNumericEditId = 4101;
constexpr int kChoiceListId = 4201;

struct NumericDialogState {
    std::wstring title;
    std::wstring detail;
    std::wstring error;
    int minimum = 0;
    int maximum = 0;
    int result = 0;
    bool textMode = false;
    std::wstring textResult;
    std::size_t maximumLength = 64;
    bool done = false;
    bool accepted = false;
    HWND edit = nullptr;
    HFONT titleFont = nullptr;
    HFONT normalFont = nullptr;
    HBRUSH editBrush = nullptr;
};

void ApplyDialogChrome(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = (std::max)(1L, client.right - client.left);
    const int height = (std::max)(1L, client.bottom - client.top);
    const int radius = ui::ResolveThemeRadius(12);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2, radius * 2);
    if (region && !SetWindowRgn(window, region, TRUE)) DeleteObject(region);

    const BOOL dark = ui::CurrentThemePalette().appearance == ui::ThemeAppearance::Light ? FALSE : TRUE;
    const DWORD round = 2;  // DWMWCP_ROUND
    const COLORREF noBorder = 0xFFFFFFFE;  // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(window, 19, &dark, sizeof(dark));
    DwmSetWindowAttribute(window, 33, &round, sizeof(round));
    DwmSetWindowAttribute(window, 34, &noBorder, sizeof(noBorder));
}

void DrawDialogButton(const DRAWITEMSTRUCT& item, const wchar_t* text, bool primary, HFONT font,
                      bool destructive = false) {
    const std::wstring translated = ui::TranslateText(text ? text : L"");
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const COLORREF fill = destructive ? (pressed ? RGB(155, 52, 66) : RGB(186, 62, 78))
                        : primary     ? (pressed ? RGB(62, 105, 181) : RGB(73, 124, 211))
                                      : (pressed ? RGB(44, 49, 60) : RGB(34, 38, 47));
    HBRUSH outside = CreateSolidBrush(ui::ResolveThemeColor(RGB(15, 17, 22)));
    FillRect(item.hDC, &item.rcItem, outside);
    DeleteObject(outside);
    HBRUSH brush = CreateSolidBrush(ui::ResolveThemeColor(fill));
    HPEN pen = CreatePen(PS_SOLID, 1, ui::ResolveThemeColor(destructive ? RGB(216, 92, 108)
                                                          : primary     ? RGB(91, 145, 232)
                                                                        : RGB(60, 66, 78)));
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
    HGDIOBJ oldPen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom, 14, 14);
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, ui::ResolveThemeColor(primary || destructive ? RGB(250, 252, 255) : RGB(202, 208, 219)));
    HGDIOBJ oldFont = SelectObject(item.hDC, font);
    RECT textRect = item.rcItem;
    DrawTextW(item.hDC, translated.c_str(), -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item.hDC, oldFont);
}

constexpr wchar_t kConfirmDialogClass[] = L"LunaCaptureConfirmDialog";

struct ConfirmDialogState {
    std::wstring title;
    std::wstring detail;
    std::wstring confirmLabel;
    bool destructive = true;
    bool noticeOnly = false;
    bool errorTone = false;
    bool done = false;
    bool accepted = false;
    HFONT titleFont = nullptr;
    HFONT normalFont = nullptr;
};

LRESULT CALLBACK ConfirmDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ConfirmDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<ConfirmDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
        case WM_CREATE: {
            state->titleFont = ui::CreateThemeFont(20, FW_SEMIBOLD);
            state->normalFont = ui::CreateThemeFont(15, FW_NORMAL);
            if (!state->noticeOnly) {
                HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                              288, 196, 104, 38, window,
                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
                                              GetModuleHandleW(nullptr), nullptr);
                SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(state->normalFont), TRUE);
                SetFocus(cancel);  // destructive default: never the confirm button
            }
            HWND accept = CreateWindowExW(0, L"BUTTON", L"OK",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                          state->noticeOnly ? 404 : 404, 196, 112, 38, window,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
                                          GetModuleHandleW(nullptr), nullptr);
            SendMessageW(accept, WM_SETFONT, reinterpret_cast<WPARAM>(state->normalFont), TRUE);
            if (state->noticeOnly) SetFocus(accept);
            ApplyDialogChrome(window);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL) { state->done = true; return 0; }
            if (LOWORD(wParam) == IDOK) { state->accepted = true; state->done = true; return 0; }
            break;
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (!item) return FALSE;
            if (item->CtlID == IDOK) {
                const std::wstring label = state->noticeOnly ? L"Got it" : state->confirmLabel;
                DrawDialogButton(*item, label.c_str(), !state->destructive, state->normalFont,
                                 state->destructive && !state->noticeOnly);
            } else {
                DrawDialogButton(*item, L"Cancel", false, state->normalFont);
            }
            return TRUE;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            HBRUSH background = CreateSolidBrush(ui::ResolveThemeColor(RGB(15, 17, 22)));
            FillRect(dc, &client, background);
            DeleteObject(background);
            ui::PaintThemeBackdrop(dc, client, ui::ThemeSurface::Dialog, GetTickCount64());

            const COLORREF accent = state->errorTone || state->destructive ? RGB(243, 112, 126)
                                  : RGB(110, 168, 255);
            const UiIcon glyph = state->errorTone ? UiIcon::Close
                               : state->destructive ? UiIcon::Warning
                               : UiIcon::Bell;
            const RECT badge{28, 30, 84, 86};
            ui::FillEllipse(dc, badge, ui::MixColor(RGB(15, 17, 22), accent, 42.0f / 255.0f));
            DrawUiIcon(dc, glyph, RECT{badge.left + 15, badge.top + 15, badge.right - 15, badge.bottom - 15},
                       accent, 2);

            SetBkMode(dc, TRANSPARENT);
            HGDIOBJ oldFont = SelectObject(dc, state->titleFont);
            SetTextColor(dc, ui::ResolveThemeColor(RGB(242, 245, 251)));
            RECT title{104, 30, 512, 64};
            const std::wstring translatedTitle = ui::TranslateText(state->title);
            DrawTextW(dc, translatedTitle.c_str(), -1, &title,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, state->normalFont);
            SetTextColor(dc, ui::ResolveThemeColor(RGB(150, 158, 173)));
            RECT detail{104, 68, 512, 180};
            const std::wstring translatedDetail = ui::TranslateText(state->detail);
            DrawTextW(dc, translatedDetail.c_str(), -1, &detail, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, oldFont);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_CLOSE: state->done = true; return 0;
        case WM_DESTROY:
            if (state->titleFont) DeleteObject(state->titleFont);
            if (state->normalFont) DeleteObject(state->normalFont);
            state->titleFont = nullptr;
            state->normalFont = nullptr;
            return 0;
        default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RunConfirmDialog(HWND owner, ConfirmDialogState& state) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ConfirmDialogProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kConfirmDialogClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    RECT work{};
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitorInfo)) work = monitorInfo.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    constexpr int kWidth = 540;
    constexpr int kHeight = 260;
    const int x = work.left + ((work.right - work.left) - kWidth) / 2;
    const int y = work.top + ((work.bottom - work.top) - kHeight) / 2;
    HWND dialog = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kConfirmDialogClass, state.title.c_str(),
                                  WS_POPUP, x, y, kWidth, kHeight, owner, nullptr,
                                  GetModuleHandleW(nullptr), &state);
    if (!dialog) return false;
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    SetForegroundWindow(dialog);
    while (!state.done && IsWindow(dialog)) {
        MSG message{};
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && message.wParam == VK_ESCAPE) {
            state.done = true;
            continue;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (IsWindow(dialog)) DestroyWindow(dialog);
    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    return state.accepted;
}

LRESULT CALLBACK NumericDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<NumericDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<NumericDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
        case WM_CREATE: {
            state->titleFont = ui::CreateThemeFont(20, FW_SEMIBOLD);
            state->normalFont = ui::CreateThemeFont(15, FW_NORMAL);
            state->editBrush = CreateSolidBrush(ui::ResolveThemeColor(RGB(25, 29, 36)));
            const std::wstring initial = state->textMode ? state->textResult : std::to_wstring(state->result);
            const DWORD editStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                                    (state->textMode ? 0 : ES_NUMBER);
            state->edit = CreateWindowExW(0, L"EDIT", initial.c_str(), editStyle,
                                           24, 108, 492, 42, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNumericEditId)),
                                           GetModuleHandleW(nullptr), nullptr);
            HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                          320, 204, 90, 38, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), GetModuleHandleW(nullptr), nullptr);
            HWND accept = CreateWindowExW(0, L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                          422, 204, 94, 38, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(state->normalFont), TRUE);
            SendMessageW(state->edit, EM_SETLIMITTEXT,
                         static_cast<WPARAM>(state->textMode ? state->maximumLength : 63), 0);
            SetWindowTheme(state->edit,
                           ui::CurrentThemePalette().appearance == ui::ThemeAppearance::Light ? L"Explorer" : L"DarkMode_Explorer",
                           nullptr);
            SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(state->normalFont), TRUE);
            SendMessageW(accept, WM_SETFONT, reinterpret_cast<WPARAM>(state->normalFont), TRUE);
            SendMessageW(state->edit, EM_SETSEL, 0, -1);
            SetFocus(state->edit);
            ApplyDialogChrome(window);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL) { state->done = true; return 0; }
            if (LOWORD(wParam) == IDOK) {
                if (state->textMode) {
                    std::wstring value(state->maximumLength + 1, L'\0');
                    const int length = GetWindowTextW(state->edit, value.data(), static_cast<int>(value.size()));
                    value.resize((std::max)(0, length));
                    while (!value.empty() && std::iswspace(value.front())) value.erase(value.begin());
                    while (!value.empty() && std::iswspace(value.back())) value.pop_back();
                    if (value.empty()) {
                        state->error = L"Enter a non-empty profile name";
                        MessageBeep(MB_ICONWARNING);
                        InvalidateRect(window, nullptr, FALSE);
                        return 0;
                    }
                    state->textResult = std::move(value);
                    state->accepted = true;
                    state->done = true;
                    return 0;
                }
                wchar_t value[64]{};
                GetWindowTextW(state->edit, value, static_cast<int>(std::size(value)));
                wchar_t* end = nullptr;
                const long long parsed = wcstoll(value, &end, 10);
                while (end && *end == L' ') ++end;
                if (end == value || (end && *end != L'\0') || parsed < state->minimum || parsed > state->maximum) {
                    state->error = L"Enter a value from " + std::to_wstring(state->minimum) + L" to " + std::to_wstring(state->maximum);
                    MessageBeep(MB_ICONWARNING);
                    SendMessageW(state->edit, EM_SETSEL, 0, -1);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
                state->result = static_cast<int>(parsed);
                state->accepted = true;
                state->done = true;
                return 0;
            }
            break;
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (!item) return FALSE;
            DrawDialogButton(*item, item->CtlID == IDOK ? (state->textMode ? L"Save" : L"Apply") : L"Cancel",
                             item->CtlID == IDOK, state->normalFont);
            return TRUE;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ui::ResolveThemeColor(RGB(238, 242, 249)));
            SetBkColor(dc, ui::ResolveThemeColor(RGB(25, 29, 36)));
            return reinterpret_cast<LRESULT>(state->editBrush);
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            HBRUSH background = CreateSolidBrush(ui::ResolveThemeColor(RGB(15, 17, 22)));
            FillRect(dc, &client, background);
            DeleteObject(background);
            ui::PaintThemeBackdrop(dc, client, ui::ThemeSurface::Dialog, GetTickCount64());
            SetBkMode(dc, TRANSPARENT);
            HGDIOBJ oldFont = SelectObject(dc, state->titleFont);
            SetTextColor(dc, ui::ResolveThemeColor(RGB(242, 245, 251)));
            RECT title{24, 17, 516, 49};
            const std::wstring translatedTitle = ui::TranslateText(state->title);
            DrawTextW(dc, translatedTitle.c_str(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(dc, state->normalFont);
            SetTextColor(dc, ui::ResolveThemeColor(RGB(130, 139, 154)));
            RECT detail{24, 49, 516, 99};
            const std::wstring translatedDetail = ui::TranslateText(state->detail);
            DrawTextW(dc, translatedDetail.c_str(), -1, &detail, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            if (!state->error.empty()) {
                SetTextColor(dc, ui::ResolveThemeColor(RGB(243, 112, 126)));
                RECT error{24, 158, 304, 242};
                const std::wstring translatedError = ui::TranslateText(state->error);
                DrawTextW(dc, translatedError.c_str(), -1, &error, DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
            } else {
                SetTextColor(dc, ui::ResolveThemeColor(RGB(89, 99, 114)));
                const std::wstring range = state->textMode ?
                    (L"Maximum: " + std::to_wstring(state->maximumLength) + L" characters") :
                    (L"Allowed: " + std::to_wstring(state->minimum) + L"–" + std::to_wstring(state->maximum));
                RECT rangeRect{24, 158, 304, 242};
                const std::wstring translatedRange = ui::TranslateText(range);
                DrawTextW(dc, translatedRange.c_str(), -1, &rangeRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            SelectObject(dc, oldFont);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_CLOSE: state->done = true; return 0;
        case WM_DESTROY:
            if (state->titleFont) DeleteObject(state->titleFont);
            if (state->normalFont) DeleteObject(state->normalFont);
            if (state->editBrush) DeleteObject(state->editBrush);
            state->titleFont = nullptr; state->normalFont = nullptr; state->editBrush = nullptr;
            return 0;
        default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

struct ChoiceDialogState {
    std::wstring title;
    std::wstring detail;
    std::vector<std::wstring> items;
    int result = -1;
    std::uint32_t resultMask = 0;
    int height = 320;
    bool done = false;
    bool accepted = false;
    bool multiple = false;
    HWND list = nullptr;
    HFONT titleFont = nullptr;
    HFONT normalFont = nullptr;
    HBRUSH listBrush = nullptr;
};

LRESULT CALLBACK ChoiceDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ChoiceDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<ChoiceDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
        case WM_CREATE: {
            state->titleFont = ui::CreateThemeFont(20, FW_SEMIBOLD);
            state->normalFont = ui::CreateThemeFont(15, FW_NORMAL);
            state->listBrush = CreateSolidBrush(ui::ResolveThemeColor(RGB(24, 28, 35)));
            const int listTop = state->detail.empty() ? 58 : 82;
            // Every click toggles one monitor. Requiring Ctrl here is easy to miss in a compact popup.
            const DWORD selectionStyle = state->multiple ? LBS_MULTIPLESEL : 0;
            state->list = CreateWindowExW(0, L"LISTBOX", nullptr,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | selectionStyle,
                20, listTop, 500, state->height - listTop - 70, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChoiceListId)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(state->list, WM_SETFONT, reinterpret_cast<WPARAM>(state->normalFont), TRUE);
            SetWindowTheme(state->list,
                           ui::CurrentThemePalette().appearance == ui::ThemeAppearance::Light ? L"Explorer" : L"DarkMode_Explorer",
                           nullptr);
            SendMessageW(state->list, LB_SETITEMHEIGHT, 0, 28);
            for (const auto& item : state->items) {
                const std::wstring translated = ui::TranslateText(item);
                SendMessageW(state->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(translated.c_str()));
            }
            if (state->multiple) {
                for (std::size_t index = 0; index < state->items.size() && index < 32; ++index) {
                    if ((state->resultMask & (1u << index)) != 0) {
                        SendMessageW(state->list, LB_SETSEL, TRUE, static_cast<LPARAM>(index));
                    }
                }
            } else if (state->result >= 0) {
                SendMessageW(state->list, LB_SETCURSEL, static_cast<WPARAM>(state->result), 0);
            }
            HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                          320, state->height - 54, 90, 36, window,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), GetModuleHandleW(nullptr), nullptr);
            HWND accept = CreateWindowExW(0, L"BUTTON", L"Select", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                          422, state->height - 54, 98, 36, window,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(state->normalFont), TRUE);
            SendMessageW(accept, WM_SETFONT, reinterpret_cast<WPARAM>(state->normalFont), TRUE);
            SetFocus(state->list);
            ApplyDialogChrome(window);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL) { state->done = true; return 0; }
            if (LOWORD(wParam) == IDOK ||
                (LOWORD(wParam) == kChoiceListId && HIWORD(wParam) == LBN_DBLCLK)) {
                if (state->multiple) {
                    std::uint32_t mask = 0;
                    for (std::size_t index = 0; index < state->items.size() && index < 32; ++index) {
                        if (SendMessageW(state->list, LB_GETSEL, static_cast<WPARAM>(index), 0) > 0) mask |= 1u << index;
                    }
                    state->resultMask = mask;
                    state->accepted = true;
                    state->done = true;
                    return 0;
                }
                const LRESULT selected = SendMessageW(state->list, LB_GETCURSEL, 0, 0);
                if (selected != LB_ERR) {
                    state->result = static_cast<int>(selected);
                    state->accepted = true;
                    state->done = true;
                } else {
                    MessageBeep(MB_ICONWARNING);
                }
                return 0;
            }
            break;
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (!item) return FALSE;
            DrawDialogButton(*item, item->CtlID == IDOK ? L"Select" : L"Cancel", item->CtlID == IDOK, state->normalFont);
            return TRUE;
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ui::ResolveThemeColor(RGB(224, 229, 238)));
            SetBkColor(dc, ui::ResolveThemeColor(RGB(24, 28, 35)));
            return reinterpret_cast<LRESULT>(state->listBrush);
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            HBRUSH brush = CreateSolidBrush(ui::ResolveThemeColor(RGB(15, 17, 22)));
            FillRect(dc, &client, brush);
            DeleteObject(brush);
            ui::PaintThemeBackdrop(dc, client, ui::ThemeSurface::Dialog, GetTickCount64());
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ui::ResolveThemeColor(RGB(242, 245, 251)));
            HGDIOBJ oldFont = SelectObject(dc, state->titleFont);
            RECT title{20, 14, 520, 49};
            const std::wstring translatedTitle = ui::TranslateText(state->title);
            DrawTextW(dc, translatedTitle.c_str(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (!state->detail.empty()) {
                SelectObject(dc, state->normalFont);
                SetTextColor(dc, ui::ResolveThemeColor(RGB(128, 138, 154)));
                RECT detail{20, 46, 520, 78};
                const std::wstring translatedDetail = ui::TranslateText(state->detail);
                DrawTextW(dc, translatedDetail.c_str(), -1, &detail, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            SelectObject(dc, oldFont);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_CLOSE: state->done = true; return 0;
        case WM_DESTROY:
            if (state->titleFont) DeleteObject(state->titleFont);
            if (state->normalFont) DeleteObject(state->normalFont);
            if (state->listBrush) DeleteObject(state->listBrush);
            state->titleFont = nullptr; state->normalFont = nullptr; state->listBrush = nullptr;
            return 0;
        default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

std::wstring GetLocalAppDataDirectory() {
    const auto base = KnownFolder(FOLDERID_LocalAppData);
    return base.empty() ? L"." : (std::filesystem::path(base) / L"Luna Capture").wstring();
}

std::wstring GetDefaultScreenshotDirectory() {
    const auto pictures = KnownFolder(FOLDERID_Pictures);
    if (!pictures.empty()) return (std::filesystem::path(pictures) / L"Luna Capture").wstring();
    return (std::filesystem::path(GetLocalAppDataDirectory()) / L"Screenshots").wstring();
}

std::wstring GetDefaultRecordingDirectory() {
    const auto videos = KnownFolder(FOLDERID_Videos);
    if (!videos.empty()) return (std::filesystem::path(videos) / L"Luna Capture").wstring();
    return (std::filesystem::path(GetLocalAppDataDirectory()) / L"Recordings").wstring();
}

std::wstring GetSoundLibraryDirectory() {
    return (std::filesystem::path(GetLocalAppDataDirectory()) / L"Sounds").wstring();
}

std::wstring GetExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(len);
    return path;
}

bool EnsureDirectoryExists(const std::wstring& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec && std::filesystem::is_directory(path, ec);
}

void ApplyChronologicalFolderView(const std::wstring& directory) {
    // A folder under Pictures or Videos inherits that library's Explorer template,
    // and those templates sort by "Date taken" / "Media created" — metadata that
    // screen captures do not carry. Explorer then falls back to an order that
    // looks random. Pinning the folder to the generic template makes it sort by
    // name instead, and our filenames are chronological by construction
    // (YYYY-MM-DD_HH-MM-SS_mmm_...), so name order *is* date order.
    if (directory.empty() || !EnsureDirectoryExists(directory)) return;

    const std::filesystem::path desktopIni = std::filesystem::path(directory) / L"desktop.ini";
    std::error_code ec;

    // Never clobber a customization the user (or another tool) already made.
    if (std::filesystem::exists(desktopIni, ec)) {
        wchar_t existing[64]{};
        GetPrivateProfileStringW(L"ViewState", L"FolderType", L"", existing,
                                 static_cast<DWORD>(std::size(existing)), desktopIni.c_str());
        if (existing[0] != L'\0') return;
    }

    // The file must be writable before we can update a hidden/system copy.
    SetFileAttributesW(desktopIni.c_str(), FILE_ATTRIBUTE_NORMAL);
    const bool wrote =
        WritePrivateProfileStringW(L"ViewState", L"Mode", L"", desktopIni.c_str()) &&
        WritePrivateProfileStringW(L"ViewState", L"Vid", L"", desktopIni.c_str()) &&
        WritePrivateProfileStringW(L"ViewState", L"FolderType", L"Generic", desktopIni.c_str());
    if (!wrote) {
        SetFileAttributesW(desktopIni.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        return;
    }
    SetFileAttributesW(desktopIni.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);

    // Windows only reads desktop.ini from a folder flagged read-only or system.
    // Read-only is the safe one: the bit is meaningless for folders, while a
    // system folder would vanish for anyone hiding protected operating files.
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) == 0) {
        SetFileAttributesW(directory.c_str(), attributes | FILE_ATTRIBUTE_READONLY);
    }
}

void OpenFolderInExplorer(const std::wstring& path) {
    EnsureDirectoryExists(path);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring FormatHotkey(UINT modifiers, UINT vk) {
    if (vk == 0) return L"Disabled";
    std::wstring result;
    auto add = [&result](const wchar_t* part) {
        if (!result.empty()) result += L" + ";
        result += part;
    };
    if (modifiers & MOD_CONTROL) add(L"Ctrl");
    if (modifiers & MOD_ALT) add(L"Alt");
    if (modifiers & MOD_SHIFT) add(L"Shift");
    if (modifiers & MOD_WIN) add(L"Win");
    const auto key = VirtualKeyName(vk);
    if (!result.empty()) result += L" + ";
    result += key;
    return result;
}

bool ConfirmAction(HWND owner, const std::wstring& title, const std::wstring& detail,
                   const std::wstring& confirmLabel, bool destructive) {
    ConfirmDialogState state;
    state.title = title;
    state.detail = detail;
    state.confirmLabel = confirmLabel.empty() ? std::wstring(L"Confirm") : confirmLabel;
    state.destructive = destructive;
    return RunConfirmDialog(owner, state);
}

void ShowNoticeDialog(HWND owner, const std::wstring& title, const std::wstring& detail, bool error) {
    ConfirmDialogState state;
    state.title = title;
    state.detail = detail;
    state.noticeOnly = true;
    state.destructive = false;
    state.errorTone = error;
    RunConfirmDialog(owner, state);
}

bool ChooseFolder(HWND owner, const std::wstring& initialPath, std::wstring& selectedPath) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return false;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose screenshot folder");
    if (!initialPath.empty()) {
        IShellItem* initial = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initialPath.c_str(), nullptr, IID_PPV_ARGS(&initial)))) {
            dialog->SetFolder(initial);
            initial->Release();
        }
    }
    bool accepted = false;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR raw = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
                selectedPath = raw;
                CoTaskMemFree(raw);
                accepted = true;
            }
            item->Release();
        }
    }
    dialog->Release();
    return accepted;
}

bool ChooseWaveFile(HWND owner, std::wstring& selectedPath) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return false;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Import notification sound");
    const COMDLG_FILTERSPEC filters[] = {{L"Wave audio (*.wav)", L"*.wav"}};
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetDefaultExtension(L"wav");

    bool accepted = false;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR raw = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
                selectedPath = raw;
                CoTaskMemFree(raw);
                accepted = true;
            }
            item->Release();
        }
    }
    dialog->Release();
    return accepted;
}

bool PromptForInteger(HWND owner, const std::wstring& title, const std::wstring& detail,
                      int initialValue, int minimum, int maximum, int& selectedValue) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = NumericDialogProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kNumericDialogClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    NumericDialogState state;
    state.title = title;
    state.detail = detail;
    state.minimum = minimum;
    state.maximum = maximum;
    state.result = (std::max)(minimum, (std::min)(maximum, initialValue));
    RECT work{};
    HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (GetMonitorInfoW(monitor, &monitorInfo)) work = monitorInfo.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - 540) / 2;
    const int y = work.top + ((work.bottom - work.top) - 260) / 2;
    HWND dialog = CreateWindowExW(WS_EX_TOOLWINDOW, kNumericDialogClass, title.c_str(), WS_POPUP,
                                  x, y, 540, 260, owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!dialog) return false;
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    while (!state.done && IsWindow(dialog)) {
        MSG message{};
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && message.wParam == VK_ESCAPE) {
            state.done = true;
            continue;
        }
        if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && message.wParam == VK_RETURN) {
            SendMessageW(dialog, WM_COMMAND, IDOK, 0);
            continue;
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (IsWindow(dialog)) DestroyWindow(dialog);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (state.accepted) selectedValue = state.result;
    return state.accepted;
}

bool PromptForText(HWND owner, const std::wstring& title, const std::wstring& detail,
                   const std::wstring& initialValue, std::size_t maximumLength,
                   std::wstring& selectedValue) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = NumericDialogProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kNumericDialogClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    NumericDialogState state;
    state.title = title;
    state.detail = detail;
    state.textMode = true;
    state.maximumLength = (std::max<std::size_t>)(1, (std::min<std::size_t>)(128, maximumLength));
    state.textResult = initialValue.substr(0, state.maximumLength);
    RECT work{};
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitorInfo)) work = monitorInfo.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - 540) / 2;
    const int y = work.top + ((work.bottom - work.top) - 260) / 2;
    HWND dialog = CreateWindowExW(WS_EX_TOOLWINDOW, kNumericDialogClass, title.c_str(), WS_POPUP,
                                  x, y, 540, 260, owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!dialog) return false;
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    while (!state.done && IsWindow(dialog)) {
        MSG message{};
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && message.wParam == VK_ESCAPE) {
            state.done = true; continue;
        }
        if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && message.wParam == VK_RETURN) {
            SendMessageW(dialog, WM_COMMAND, IDOK, 0); continue;
        }
        if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (IsWindow(dialog)) DestroyWindow(dialog);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (state.accepted) selectedValue = state.textResult;
    return state.accepted;
}

bool ChooseIntegerPreset(HWND owner, const std::wstring& title, const std::wstring& detail,
                         const std::vector<IntegerPreset>& presets, int initialValue,
                         int minimum, int maximum, int& selectedValue, bool allowCustom) {
    if (presets.empty()) return false;
    std::vector<std::wstring> labels;
    labels.reserve(presets.size() + (allowCustom ? 1u : 0u));
    int current = -1;
    for (std::size_t index = 0; index < presets.size(); ++index) {
        labels.push_back(presets[index].label);
        if (presets[index].value == initialValue) current = static_cast<int>(index);
    }
    if (allowCustom) {
        labels.push_back(L"Custom value…  ·  type an exact number");
        if (current < 0) current = static_cast<int>(presets.size());
    } else if (current < 0) {
        current = 0;
    }
    const int choice = ChooseNamedItem(owner, labels, current, title, detail);
    if (choice < 0) return false;
    if (choice < static_cast<int>(presets.size())) {
        selectedValue = presets[static_cast<std::size_t>(choice)].value;
        return true;
    }
    int custom = (std::max)(minimum, (std::min)(maximum, initialValue));
    if (!PromptForInteger(owner, title + L" · custom", detail, custom, minimum, maximum, custom)) return false;
    selectedValue = custom;
    return true;
}

int ChooseNamedItem(HWND owner, const std::vector<std::wstring>& items, int selectedIndex,
                    const std::wstring& title, const std::wstring& detail) {
    if (items.empty()) return -1;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ChoiceDialogProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kChoiceDialogClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return -1;
    ChoiceDialogState state;
    state.title = title;
    state.detail = detail;
    state.items = items;
    state.result = selectedIndex >= 0 && selectedIndex < static_cast<int>(items.size()) ? selectedIndex : 0;
    state.height = (std::max)(230, (std::min)(540, 128 + static_cast<int>(items.size()) * 30 + (detail.empty() ? 0 : 24)));
    RECT work{};
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitorInfo)) work = monitorInfo.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - 540) / 2;
    const int y = work.top + ((work.bottom - work.top) - state.height) / 2;
    HWND dialog = CreateWindowExW(WS_EX_TOOLWINDOW, kChoiceDialogClass, title.c_str(), WS_POPUP,
                                  x, y, 540, state.height, owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!dialog) return -1;
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    while (!state.done && IsWindow(dialog)) {
        MSG message{};
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && message.wParam == VK_ESCAPE) {
            state.done = true; continue;
        }
        if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && message.wParam == VK_RETURN) {
            SendMessageW(dialog, WM_COMMAND, IDOK, 0); continue;
        }
        if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (IsWindow(dialog)) DestroyWindow(dialog);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return state.accepted ? state.result : -1;
}

bool ChooseMultipleNamedItems(HWND owner, const std::vector<std::wstring>& items, std::uint32_t initialMask,
                              const std::wstring& title, const std::wstring& detail,
                              std::uint32_t& selectedMask) {
    if (items.empty() || items.size() > 32) return false;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ChoiceDialogProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kChoiceDialogClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    ChoiceDialogState state;
    state.title = title;
    state.detail = detail;
    state.items = items;
    state.multiple = true;
    state.resultMask = initialMask;
    state.height = (std::max)(250, (std::min)(560, 152 + static_cast<int>(items.size()) * 30));
    RECT work{};
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitorInfo)) work = monitorInfo.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - 540) / 2;
    const int y = work.top + ((work.bottom - work.top) - state.height) / 2;
    HWND dialog = CreateWindowExW(WS_EX_TOOLWINDOW, kChoiceDialogClass, title.c_str(), WS_POPUP,
                                  x, y, 540, state.height, owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!dialog) return false;
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    while (!state.done && IsWindow(dialog)) {
        MSG message{};
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && message.wParam == VK_ESCAPE) {
            state.done = true; continue;
        }
        if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && message.wParam == VK_RETURN) {
            SendMessageW(dialog, WM_COMMAND, IDOK, 0); continue;
        }
        if (!IsDialogMessageW(dialog, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (IsWindow(dialog)) DestroyWindow(dialog);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (state.accepted) selectedMask = state.resultMask;
    return state.accepted;
}

bool ImportSoundToLibrary(const std::wstring& sourcePath, std::wstring& importedPath) {
    std::error_code ec;
    const std::filesystem::path source(sourcePath);
    std::wstring extension = source.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    if (extension != L".wav" || !std::filesystem::is_regular_file(source, ec)) return false;

    const std::filesystem::path library(GetSoundLibraryDirectory());
    if (!EnsureDirectoryExists(library.wstring())) return false;
    std::wstring stem = SanitizeFilenamePrefix(source.stem().wstring());
    if (stem.empty()) stem = L"Notification";
    std::filesystem::path destination = library / (stem + L".wav");
    for (int suffix = 2; std::filesystem::exists(destination, ec) && suffix < 1000; ++suffix) {
        if (std::filesystem::equivalent(source, destination, ec) && !ec) {
            importedPath = destination.wstring();
            return true;
        }
        ec.clear();
        destination = library / (stem + L" " + std::to_wstring(suffix) + L".wav");
    }
    ec.clear();
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return false;
    importedPath = destination.wstring();
    return true;
}


bool ChooseScreenshotSavePath(HWND owner, const AppSettings& settings, const std::wstring& filenamePrefix, std::wstring& selectedPath, ImageFormat& selectedFormat) {
    IFileSaveDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return false;

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN);
    dialog->SetTitle(L"Save region screenshot");

    const COMDLG_FILTERSPEC filters[] = {
        {L"PNG image (*.png)", L"*.png"},
        {L"JPEG image (*.jpg;*.jpeg)", L"*.jpg;*.jpeg"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    const UINT initialType = settings.imageFormat == ImageFormat::Jpeg ? 2U : 1U;
    dialog->SetFileTypeIndex(initialType);
    dialog->SetDefaultExtension(settings.imageFormat == ImageFormat::Jpeg ? L"jpg" : L"png");

    SYSTEMTIME time{};
    GetLocalTime(&time);
    const std::wstring suggested = MakeScreenshotFilename(filenamePrefix, time.wYear, time.wMonth, time.wDay,
                                                          time.wHour, time.wMinute, time.wSecond, 0,
                                                          settings.imageFormat == ImageFormat::Jpeg);
    dialog->SetFileName(suggested.c_str());

    const std::wstring initialDirectory = settings.saveDirectory.empty() ? GetDefaultScreenshotDirectory() : settings.saveDirectory;
    EnsureDirectoryExists(initialDirectory);
    IShellItem* initialFolder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(initialDirectory.c_str(), nullptr, IID_PPV_ARGS(&initialFolder)))) {
        dialog->SetFolder(initialFolder);
        initialFolder->Release();
    }

    bool accepted = false;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR raw = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
                selectedPath = raw;
                CoTaskMemFree(raw);

                UINT selectedType = initialType;
                dialog->GetFileTypeIndex(&selectedType);
                std::filesystem::path path(selectedPath);
                std::wstring extension = path.extension().wstring();
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });

                if (extension == L".jpg" || extension == L".jpeg") {
                    selectedFormat = ImageFormat::Jpeg;
                } else if (extension == L".png") {
                    selectedFormat = ImageFormat::Png;
                } else {
                    selectedFormat = selectedType == 2 ? ImageFormat::Jpeg : ImageFormat::Png;
                    path += selectedFormat == ImageFormat::Jpeg ? L".jpg" : L".png";
                    selectedPath = path.wstring();
                }
                accepted = true;
            }
            item->Release();
        }
    }

    dialog->Release();
    return accepted;
}

std::wstring FormatResolution(int width, int height) {
    if (width <= 0 || height <= 0) return L"Original";
    return std::to_wstring(width) + L" × " + std::to_wstring(height);
}

std::wstring FormatMilliseconds(double value) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(value < 10.0 ? 2 : 1) << value << L" ms";
    return out.str();
}

HMONITOR GetPriorityMonitor(HWND applicationWindow) {
    const HWND foreground = GetForegroundWindow();
    if (IsUsableForegroundWindow(foreground, applicationWindow)) {
        if (HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL)) return monitor;
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    return MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
}

HWND GetCapturableWindowAtPoint(POINT screenPoint, HWND applicationWindow) {
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        HWND root = GetAncestor(window, GA_ROOT);
        if (root != window) continue;
        if (!IsUsableForegroundWindow(window, applicationWindow)) continue;
        RECT bounds = GetWindowCaptureBounds(window);
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) continue;
        if (screenPoint.x >= bounds.left && screenPoint.x < bounds.right &&
            screenPoint.y >= bounds.top && screenPoint.y < bounds.bottom) {
            return window;
        }
    }
    return nullptr;
}

HWND GetPriorityCaptureWindow(HWND applicationWindow) {
    HWND window = GetForegroundWindow();
    if (!IsUsableForegroundWindow(window, applicationWindow)) {
        POINT cursor{};
        GetCursorPos(&cursor);
        window = GetCapturableWindowAtPoint(cursor, applicationWindow);
    }
    if (!IsUsableForegroundWindow(window, applicationWindow)) return nullptr;
    return window;
}

RECT GetWindowCaptureBounds(HWND window) {
    RECT bounds{};
    if (!window || !IsWindow(window)) return bounds;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) {
        GetWindowRect(window, &bounds);
    }
    const RECT desktop = GetVirtualDesktopRect();
    bounds.left = (std::max)(bounds.left, desktop.left);
    bounds.top = (std::max)(bounds.top, desktop.top);
    bounds.right = (std::min)(bounds.right, desktop.right);
    bounds.bottom = (std::min)(bounds.bottom, desktop.bottom);
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return RECT{};
    return bounds;
}

std::wstring GetWindowCaptureName(HWND window) {
    if (!window || !IsWindow(window)) return L"Desktop";
    wchar_t title[512]{};
    const int titleLength = GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    if (titleLength > 0) return SanitizeFilenamePrefix(std::wstring(title, static_cast<std::size_t>(titleLength)));

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != 0) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (process) {
            wchar_t path[32768]{};
            DWORD length = static_cast<DWORD>(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &length) && length > 0) {
                CloseHandle(process);
                std::filesystem::path executable(std::wstring(path, length));
                return SanitizeFilenamePrefix(executable.stem().wstring());
            }
            CloseHandle(process);
        }
    }
    return L"Desktop";
}

RECT GetVirtualDesktopRect() {
    RECT rect{};
    rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    rect.right = rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    rect.bottom = rect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return rect;
}

std::vector<AudioWindowInfo> EnumerateAudioWindows(HWND applicationWindow) {
    struct Context {
        HWND excluded = nullptr;
        std::vector<AudioWindowInfo> windows;
    } context{applicationWindow, {}};
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        auto& state = *reinterpret_cast<Context*>(value);
        if (!window || window == state.excluded || !IsWindowVisible(window) || GetAncestor(window, GA_ROOT) != window) return TRUE;
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if ((style & WS_EX_TOOLWINDOW) != 0) return TRUE;
        wchar_t title[512]{};
        const int titleLength = GetWindowTextW(window, title, static_cast<int>(std::size(title)));
        if (titleLength <= 0) return TRUE;
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (!processId || processId == GetCurrentProcessId()) return TRUE;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!process) return TRUE;
        wchar_t path[32768]{};
        DWORD pathLength = static_cast<DWORD>(std::size(path));
        const bool havePath = QueryFullProcessImageNameW(process, 0, path, &pathLength) != FALSE;
        CloseHandle(process);
        if (!havePath || !pathLength) return TRUE;
        AudioWindowInfo info;
        info.processId = processId;
        info.window = window;
        info.title.assign(title, static_cast<std::size_t>(titleLength));
        info.executableName = std::filesystem::path(std::wstring(path, pathLength)).filename().wstring();
        for (const auto& existing : state.windows) if (existing.processId == processId) return TRUE;
        state.windows.push_back(std::move(info));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.windows;
}

void AppendDiagnosticLog(const std::wstring& message) {
    static std::mutex logMutex;
    std::lock_guard lock(logMutex);
    const std::wstring directory = GetLocalAppDataDirectory();
    if (!EnsureDirectoryExists(directory)) return;
    const std::wstring path = (std::filesystem::path(directory) / L"capture.log").wstring();
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t prefix[64]{};
    swprintf_s(prefix, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", time.wYear, time.wMonth, time.wDay,
               time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    const std::wstring line = std::wstring(prefix) + message + L"\r\n";
    const int bytesNeeded = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                                nullptr, 0, nullptr, nullptr);
    if (bytesNeeded > 0) {
        std::string utf8(static_cast<std::size_t>(bytesNeeded), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                            utf8.data(), bytesNeeded, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(file);
}

bool CopyFrameToClipboard(HWND owner, const FrameBuffer& frame) {
    if (frame.bgra.empty() || !frame.width || !frame.height || frame.stride < frame.width * 4) return false;

    const SIZE_T rowBytes = static_cast<SIZE_T>(frame.width) * 4;
    const SIZE_T pixels = rowBytes * frame.height;

    auto fillBottomUp = [&](BYTE* destination) {
        for (UINT y = 0; y < frame.height; ++y) {
            const BYTE* source = frame.bgra.data() + static_cast<SIZE_T>(frame.height - 1 - y) * frame.stride;
            BYTE* row = destination + static_cast<SIZE_T>(y) * rowBytes;
            std::memcpy(row, source, rowBytes);
            for (SIZE_T x = 3; x < rowBytes; x += 4) row[x] = 255;
        }
    };

    HGLOBAL dibV5 = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(BITMAPV5HEADER) + pixels);
    HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(BITMAPINFOHEADER) + pixels);
    if (!dibV5 || !dib) {
        if (dibV5) GlobalFree(dibV5);
        if (dib) GlobalFree(dib);
        return false;
    }

    auto* v5Base = static_cast<BYTE*>(GlobalLock(dibV5));
    auto* dibBase = static_cast<BYTE*>(GlobalLock(dib));
    if (!v5Base || !dibBase) {
        if (v5Base) GlobalUnlock(dibV5);
        if (dibBase) GlobalUnlock(dib);
        GlobalFree(dibV5);
        GlobalFree(dib);
        return false;
    }

    auto* v5 = reinterpret_cast<BITMAPV5HEADER*>(v5Base);
    *v5 = {};
    v5->bV5Size = sizeof(BITMAPV5HEADER);
    v5->bV5Width = static_cast<LONG>(frame.width);
    v5->bV5Height = static_cast<LONG>(frame.height); // positive = bottom-up, most compatible on clipboard
    v5->bV5Planes = 1;
    v5->bV5BitCount = 32;
    v5->bV5Compression = BI_BITFIELDS;
    v5->bV5SizeImage = static_cast<DWORD>(pixels);
    v5->bV5RedMask = 0x00FF0000;
    v5->bV5GreenMask = 0x0000FF00;
    v5->bV5BlueMask = 0x000000FF;
    v5->bV5AlphaMask = 0xFF000000;
    v5->bV5CSType = LCS_sRGB;
    v5->bV5Intent = LCS_GM_IMAGES;
    fillBottomUp(v5Base + sizeof(BITMAPV5HEADER));

    auto* header = reinterpret_cast<BITMAPINFOHEADER*>(dibBase);
    *header = {};
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = static_cast<LONG>(frame.width);
    header->biHeight = static_cast<LONG>(frame.height);
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(pixels);
    fillBottomUp(dibBase + sizeof(BITMAPINFOHEADER));
    const BITMAPINFOHEADER bitmapHeader = *header;

    GlobalUnlock(dibV5);
    GlobalUnlock(dib);

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader = bitmapHeader;
    void* bitmapBits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(screen, &bitmapInfo, DIB_RGB_COLORS, &bitmapBits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (bitmap && bitmapBits) {
        auto* locked = static_cast<BYTE*>(GlobalLock(dib));
        if (locked) {
            std::memcpy(bitmapBits, locked + sizeof(BITMAPINFOHEADER), pixels);
            GlobalUnlock(dib);
        }
    }

    bool opened = false;
    for (int attempt = 0; attempt < 8 && !opened; ++attempt) {
        opened = OpenClipboard(owner) != FALSE;
        if (!opened) Sleep(4);
    }
    if (!opened) {
        GlobalFree(dibV5);
        GlobalFree(dib);
        if (bitmap) DeleteObject(bitmap);
        return false;
    }

    EmptyClipboard();
    const bool v5Ok = SetClipboardData(CF_DIBV5, dibV5) != nullptr;
    const bool dibOk = SetClipboardData(CF_DIB, dib) != nullptr;
    const bool bitmapOk = bitmap && SetClipboardData(CF_BITMAP, bitmap) != nullptr;
    CloseClipboard();

    if (!v5Ok) GlobalFree(dibV5);
    if (!dibOk) GlobalFree(dib);
    if (bitmap && !bitmapOk) DeleteObject(bitmap);
    return v5Ok || dibOk || bitmapOk;
}

bool IsExclusiveFullscreenAppActive() {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (FAILED(SHQueryUserNotificationState(&state))) return false;
    return state == QUNS_RUNNING_D3D_FULL_SCREEN;
}
