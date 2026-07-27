#include "PluginInstallPrompt.hpp"

#include "LunaScript.hpp"
#include "UiGfx.hpp"
#include "UiIcons.hpp"
#include "WinUtil.hpp"

#include <Windows.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

// A self-contained "mini installer" for a .luna file. It deliberately does not
// talk to a running Luna Capture: the plugin folder is an ordinary folder and
// the app hot-reloads it, so this works identically whether Luna is closed,
// sitting in the tray, or running elevated — the cases where handing the file
// to the running instance used to silently do nothing.

namespace {

constexpr wchar_t kPromptClass[] = L"LunaCapturePluginInstallPrompt";
constexpr int kWidth = 560;
constexpr int kHeight = 430;
constexpr int kMargin = 30;

enum class Hit { None, Install, Cancel };

struct PromptState {
    const LunaPluginDefinition* plugin = nullptr;
    std::wstring fileName;
    std::wstring headline;
    std::wstring subline;
    std::vector<std::wstring> permissions;
    bool replacing = false;
    bool invalid = false;
    std::wstring problem;

    bool done = false;
    bool accepted = false;
    Hit hovered = Hit::None;
    Hit pressed = Hit::None;
    RECT installRect{};
    RECT cancelRect{};

    HFONT fontTitle = nullptr;
    HFONT fontBody = nullptr;
    HFONT fontSmall = nullptr;
};

void DrawLabel(HDC dc, const std::wstring& text, RECT rect, COLORREF color, HFONT font,
               UINT flags = DT_LEFT | DT_TOP | DT_WORDBREAK) {
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ui::ResolveThemeColor(color));
    DrawTextW(dc, text.c_str(), -1, &rect, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

int MeasureHeight(HDC dc, const std::wstring& text, int width, HFONT font) {
    HGDIOBJ old = SelectObject(dc, font);
    RECT measure{0, 0, width, 0};
    DrawTextW(dc, text.c_str(), -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return measure.bottom - measure.top;
}

void DrawPromptButton(HDC dc, const RECT& rect, const std::wstring& label, bool primary,
                      bool hot, bool pressed, HFONT font) {
    const COLORREF fill = primary ? (pressed ? RGB(62, 105, 181) : (hot ? RGB(86, 138, 228) : RGB(73, 124, 211)))
                                  : (pressed ? RGB(44, 49, 60) : (hot ? RGB(42, 47, 58) : RGB(32, 36, 45)));
    ui::FillRoundRect(dc, rect, 11, ui::ResolveThemeColor(fill));
    ui::StrokeRoundRect(dc, rect, 11, ui::ResolveThemeColor(primary ? RGB(104, 156, 240) : RGB(62, 69, 82)));
    DrawLabel(dc, label, rect, primary ? RGB(252, 253, 255) : RGB(206, 213, 224), font,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void PaintPrompt(HWND window, PromptState& state) {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);

    HDC memory = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);

    HBRUSH background = CreateSolidBrush(ui::ResolveThemeColor(RGB(15, 17, 22)));
    FillRect(memory, &client, background);
    DeleteObject(background);
    ui::PaintThemeBackdrop(memory, client, ui::ThemeSurface::Dialog, GetTickCount64());

    // Header badge + title.
    const COLORREF accent = state.invalid ? RGB(243, 112, 126) : RGB(110, 168, 255);
    const RECT badge{kMargin, 28, kMargin + 54, 82};
    ui::FillRoundRect(memory, badge, 15, ui::MixColor(RGB(15, 17, 22), accent, 46.0f / 255.0f));
    DrawUiIcon(memory, state.invalid ? UiIcon::Warning : UiIcon::Plugin,
               RECT{badge.left + 15, badge.top + 15, badge.right - 15, badge.bottom - 15}, accent, 2);

    DrawLabel(memory, state.headline, RECT{kMargin + 70, 28, kWidth - kMargin, 60}, RGB(243, 246, 252),
              state.fontTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawLabel(memory, state.subline, RECT{kMargin + 70, 58, kWidth - kMargin, 84}, RGB(140, 149, 166),
              state.fontSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    int y = 100;
    const int width = kWidth - kMargin * 2;

    if (state.invalid) {
        const int height = MeasureHeight(memory, state.problem, width - 32, state.fontSmall);
        const RECT panel{kMargin, y, kWidth - kMargin, y + height + 26};
        ui::FillRoundRect(memory, panel, 11, RGB(38, 24, 27));
        ui::StrokeRoundRect(memory, panel, 11, RGB(112, 58, 66));
        DrawLabel(memory, state.problem, RECT{panel.left + 16, panel.top + 12, panel.right - 16, panel.bottom - 8},
                  RGB(232, 186, 192), state.fontSmall);
    } else {
        if (!state.plugin->description.empty()) {
            const int height = (std::min)(56, MeasureHeight(memory, state.plugin->description, width, state.fontBody));
            DrawLabel(memory, state.plugin->description, RECT{kMargin, y, kWidth - kMargin, y + height},
                      RGB(168, 177, 192), state.fontBody, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
            y += height + 16;
        }

        DrawLabel(memory, state.permissions.empty() ? L"REQUESTS NO PERMISSIONS" : L"REQUESTED PERMISSIONS",
                  RECT{kMargin, y, kWidth - kMargin, y + 20}, RGB(108, 118, 136), state.fontSmall,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 26;

        const RECT box{kMargin, y, kWidth - kMargin, kHeight - 108};
        ui::FillRoundRect(memory, box, 11, RGB(19, 22, 29));
        ui::StrokeRoundRect(memory, box, 11, RGB(44, 50, 62));
        int row = box.top + 12;
        if (state.permissions.empty()) {
            DrawLabel(memory, L"This plugin only defines content — it cannot control capture, settings or audio.",
                      RECT{box.left + 16, row, box.right - 16, row + 40}, RGB(150, 158, 173), state.fontSmall);
        } else {
            for (const auto& permission : state.permissions) {
                if (row + 22 > box.bottom - 8) break;
                DrawUiIcon(memory, UiIcon::Check, RECT{box.left + 16, row + 2, box.left + 32, row + 18},
                           RGB(78, 205, 139), 2);
                DrawLabel(memory, permission, RECT{box.left + 40, row, box.right - 16, row + 22},
                          RGB(206, 214, 228), state.fontSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                row += 23;
            }
        }
    }

    // Footer.
    const RECT separator{0, kHeight - 72, kWidth, kHeight - 71};
    HBRUSH line = CreateSolidBrush(ui::ResolveThemeColor(RGB(32, 36, 45)));
    FillRect(memory, &separator, line);
    DeleteObject(line);

    state.cancelRect = RECT{kWidth - kMargin - 244, kHeight - 56, kWidth - kMargin - 140, kHeight - 18};
    state.installRect = RECT{kWidth - kMargin - 128, kHeight - 56, kWidth - kMargin, kHeight - 18};
    if (!state.invalid) {
        DrawPromptButton(memory, state.cancelRect, L"Cancel", false, state.hovered == Hit::Cancel,
                         state.pressed == Hit::Cancel, state.fontBody);
        DrawPromptButton(memory, state.installRect, state.replacing ? L"Replace" : L"Install", true,
                         state.hovered == Hit::Install, state.pressed == Hit::Install, state.fontBody);
    } else {
        DrawPromptButton(memory, state.installRect, L"Close", true, state.hovered == Hit::Install,
                         state.pressed == Hit::Install, state.fontBody);
    }

    BitBlt(target, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(window, &paint);
}

Hit HitTest(PromptState& state, POINT point) {
    if (!state.invalid && PtInRect(&state.cancelRect, point)) return Hit::Cancel;
    if (PtInRect(&state.installRect, point)) return Hit::Install;
    return Hit::None;
}

LRESULT CALLBACK PromptProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<PromptState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<PromptState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
        case WM_CREATE: {
            state->fontTitle = ui::CreateThemeFont(21, FW_SEMIBOLD);
            state->fontBody = ui::CreateThemeFont(15, FW_NORMAL);
            state->fontSmall = ui::CreateThemeFont(13, FW_NORMAL);
            const BOOL dark = TRUE;
            const DWORD round = 2;
            DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
            DwmSetWindowAttribute(window, 33, &round, sizeof(round));
            return 0;
        }
        case WM_PAINT: PaintPrompt(window, *state); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const Hit hit = HitTest(*state, point);
            if (hit != state->hovered) {
                state->hovered = hit;
                InvalidateRect(window, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            state->hovered = Hit::None;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            state->pressed = HitTest(*state, point);
            if (state->pressed == Hit::None) {
                ReleaseCapture();
                SendMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const Hit released = state->pressed;
            state->pressed = Hit::None;
            ReleaseCapture();
            if (released != Hit::None && HitTest(*state, point) == released) {
                state->accepted = released == Hit::Install && !state->invalid;
                state->done = true;
            } else {
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { state->done = true; return 0; }
            if (wParam == VK_RETURN && !state->invalid) {
                state->accepted = true;
                state->done = true;
                return 0;
            }
            return 0;
        case WM_CLOSE: state->done = true; return 0;
        case WM_DESTROY:
            for (HFONT* font : {&state->fontTitle, &state->fontBody, &state->fontSmall}) {
                if (*font) { DeleteObject(*font); *font = nullptr; }
            }
            return 0;
        default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool ReadUtf8(const std::filesystem::path& path, std::wstring& text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (needed < 0) return false;
    text.assign(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), needed);
    return true;
}

bool RunPrompt(PromptState& state) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = PromptProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPromptClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    RECT work{};
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &info)) work = info.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - kWidth) / 2;
    const int y = work.top + ((work.bottom - work.top) - kHeight) / 2;

    HWND window = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_TOPMOST, kPromptClass, L"Luna Capture plugin",
                                  WS_POPUP, x, y, kWidth, kHeight, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), &state);
    if (!window) return false;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetForegroundWindow(window);

    while (!state.done && IsWindow(window)) {
        MSG message{};
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) break;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (IsWindow(window)) DestroyWindow(window);
    return state.accepted;
}

}  // namespace

PluginInstallOutcome ShowPluginInstallPrompt(const std::wstring& sourcePath) {
    PluginInstallOutcome outcome;
    const std::filesystem::path source(sourcePath);
    outcome.fileName = source.filename().wstring();

    PromptState state;
    state.fileName = outcome.fileName;

    std::error_code ec;
    std::wstring text;
    LunaPluginDefinition plugin;
    std::vector<LunaDiagnostic> diagnostics;

    if (!std::filesystem::is_regular_file(source, ec) || !ReadUtf8(source, text)) {
        state.invalid = true;
        state.headline = L"Plugin file unavailable";
        state.subline = outcome.fileName;
        state.problem = L"The file could not be read. It may have been moved or deleted.";
    } else if (!LunaScript::Parse(text, plugin, diagnostics) || !diagnostics.empty()) {
        state.invalid = true;
        state.headline = L"This plugin has errors";
        state.subline = outcome.fileName;
        state.problem = diagnostics.empty()
            ? std::wstring(L"The file is not valid LunaScript.")
            : L"Line " + std::to_wstring(diagnostics.front().line) + L": " + diagnostics.front().message +
                  L"\n\nFix the file and open it again.";
    } else {
        state.plugin = &plugin;
        state.headline = plugin.name.empty() ? outcome.fileName : plugin.name;
        state.subline = (plugin.version.empty() ? std::wstring(L"") : L"Version " + plugin.version);
        if (!plugin.author.empty()) {
            if (!state.subline.empty()) state.subline += L"  ·  ";
            state.subline += plugin.author;
        }
        if (state.subline.empty()) state.subline = outcome.fileName;
        for (const auto permission : plugin.permissions) {
            state.permissions.push_back(LunaScript::PermissionName(permission));
        }
    }

    const std::filesystem::path root = std::filesystem::path(GetLocalAppDataDirectory()) / L"Plugins";
    const std::filesystem::path destination = root / source.filename();
    state.replacing = !state.invalid && std::filesystem::exists(destination, ec);
    ec.clear();

    if (!RunPrompt(state)) {
        outcome.cancelled = !state.invalid;
        outcome.failureReason = state.invalid ? state.problem : std::wstring();
        return outcome;
    }

    std::filesystem::create_directories(root, ec);
    ec.clear();
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        outcome.failureReason = L"The plugin could not be copied into the plugin folder.";
        return outcome;
    }
    outcome.installed = true;
    outcome.pluginName = state.headline;
    return outcome;
}
