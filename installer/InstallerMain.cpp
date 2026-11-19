#include "Installer.hpp"

#include "UiGfx.hpp"
#include "UiIcons.hpp"

#include <ShObjIdl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dwmapi.lib")

using Microsoft::WRL::ComPtr;
using namespace installer;

namespace {

constexpr wchar_t kWindowClass[] = L"LunaCaptureInstallerWindow";
constexpr int kWindowWidth = 700;
constexpr int kWindowHeight = 480;
constexpr int kBannerWidth = 248;
constexpr int kContentLeft = kBannerWidth + 34;
constexpr int kContentRight = kWindowWidth - 34;

enum class Page { Welcome, Options, Progress, Done, UninstallConfirm };

enum class Control {
    None,
    Primary,      // Install / Next / Finish / Remove
    Secondary,    // Back / Cancel
    BrowsePath,
    ToggleDesktop,
    ToggleStartMenu,
    ToggleAutostart,
    ToggleRunAsAdmin,
    ToggleKeepData,
    OpenRepository,
    UseNewerVersion,
};

struct HitRegion {
    RECT rect{};
    Control control = Control::None;
};

struct AppState {
    HWND window = nullptr;
    Page page = Page::Welcome;
    bool uninstallMode = false;
    bool keepUserData = true;
    bool busy = false;
    bool finishedOk = false;

    InstallOptions options;
    DependencyReport dependencies;
    ReleaseInfo release;
    std::wstring pinnedVersion;
    std::wstring newerVersion;
    std::wstring resolveError;
    std::wstring statusMessage;
    std::wstring finishMessage;
    int progressPercent = 0;

    std::vector<HitRegion> regions;
    Control hovered = Control::None;
    Control pressed = Control::None;

    HFONT fontTitle = nullptr;
    HFONT fontHeading = nullptr;
    HFONT fontBody = nullptr;
    HFONT fontSmall = nullptr;
    HFONT fontBanner = nullptr;

    // A real edit control so the install path can be typed, not only browsed.
    HWND pathEdit = nullptr;
    HBRUSH editBrush = nullptr;
    std::wstring installWarning;
};

constexpr int kPathEditId = 1001;

AppState g;

// --- painting helpers -------------------------------------------------------

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void DrawText(HDC dc, const std::wstring& text, RECT rect, COLORREF color, HFONT font,
              UINT flags = DT_LEFT | DT_TOP | DT_WORDBREAK) {
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rect, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

void AddRegion(const RECT& rect, Control control) { g.regions.push_back({rect, control}); }

// Height a wrapped paragraph will actually occupy, so stacked blocks can follow
// each other instead of relying on hard-coded offsets that clip long messages.
int MeasureTextHeight(HDC dc, const std::wstring& text, int width, HFONT font) {
    HGDIOBJ old = SelectObject(dc, font);
    RECT measure{0, 0, width, 0};
    DrawTextW(dc, text.c_str(), -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return measure.bottom - measure.top;
}

bool IsHot(Control control) { return g.hovered == control && g.pressed == Control::None; }
bool IsPressed(Control control) { return g.pressed == control; }

void DrawButton(HDC dc, const RECT& rect, const std::wstring& label, bool primary, bool enabled,
                Control control, bool destructive = false) {
    COLORREF fill;
    COLORREF border;
    COLORREF text;
    if (!enabled) {
        fill = RGB(28, 31, 39);
        border = RGB(46, 51, 62);
        text = RGB(96, 104, 119);
    } else if (destructive) {
        fill = IsPressed(control) ? RGB(155, 52, 66) : (IsHot(control) ? RGB(204, 74, 92) : RGB(186, 62, 78));
        border = RGB(216, 92, 108);
        text = RGB(252, 253, 255);
    } else if (primary) {
        fill = IsPressed(control) ? RGB(62, 105, 181) : (IsHot(control) ? RGB(86, 138, 228) : RGB(73, 124, 211));
        border = RGB(104, 156, 240);
        text = RGB(252, 253, 255);
    } else {
        fill = IsPressed(control) ? RGB(44, 49, 60) : (IsHot(control) ? RGB(42, 47, 58) : RGB(31, 35, 44));
        border = RGB(62, 69, 82);
        text = RGB(206, 213, 224);
    }
    ui::FillRoundRect(dc, rect, 10, fill);
    ui::StrokeRoundRect(dc, rect, 10, border);
    DrawText(dc, label, rect, text, g.fontBody, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (enabled) AddRegion(rect, control);
}

void DrawCheckbox(HDC dc, int x, int y, int width, bool checked, const std::wstring& label,
                  const std::wstring& hint, Control control) {
    const RECT row{x, y, x + width, y + (hint.empty() ? 30 : 46)};
    if (IsHot(control)) ui::FillRoundRect(dc, row, 9, RGB(26, 30, 38));
    const RECT box{x + 6, y + 5, x + 26, y + 25};
    ui::FillRoundRect(dc, box, 6, checked ? RGB(73, 124, 211) : RGB(26, 30, 38));
    ui::StrokeRoundRect(dc, box, 6, checked ? RGB(114, 162, 240) : RGB(64, 71, 85));
    if (checked) {
        DrawUiIcon(dc, UiIcon::Check, RECT{box.left + 3, box.top + 3, box.right - 3, box.bottom - 3},
                   RGB(255, 255, 255), 2);
    }
    DrawText(dc, label, RECT{x + 38, y + 3, row.right - 8, y + 26}, RGB(226, 232, 243), g.fontBody,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!hint.empty()) {
        DrawText(dc, hint, RECT{x + 38, y + 24, row.right - 8, row.bottom}, RGB(122, 131, 147), g.fontSmall,
                 DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    AddRegion(row, control);
}

// Classic left banner: a painted gradient panel with soft concentric rings and
// the product mark. No external image file, so the installer stays a single
// self-contained executable.
void PaintBanner(HDC dc) {
    const RECT banner{0, 0, kBannerWidth, kWindowHeight};
    ui::FillRoundRectVGradient(dc, banner, 0, RGB(24, 38, 72), RGB(11, 15, 26));
    {
        ui::Canvas canvas(dc);
        if (canvas.ok()) {
            Gdiplus::Graphics& gfx = canvas.g();
            // Concentric rings anchored below the mark.
            for (int index = 7; index >= 1; --index) {
                const float radius = 46.0f * static_cast<float>(index);
                const BYTE alpha = static_cast<BYTE>(11 + index * 3);
                Gdiplus::Pen pen(ui::FromColorRefRaw(RGB(126, 176, 255), alpha), 1.4f);
                gfx.DrawEllipse(&pen, 124.0f - radius, 214.0f - radius, radius * 2.0f, radius * 2.0f);
            }
            // A soft diagonal sheen across the panel.
            Gdiplus::GraphicsPath sheen;
            sheen.AddLine(Gdiplus::PointF(-40.0f, 330.0f), Gdiplus::PointF(300.0f, 40.0f));
            sheen.AddLine(Gdiplus::PointF(300.0f, 132.0f), Gdiplus::PointF(-40.0f, 422.0f));
            sheen.CloseFigure();
            Gdiplus::SolidBrush brush(ui::FromColorRefRaw(RGB(146, 190, 255), 14));
            gfx.FillPath(&brush, &sheen);
        }
    }

    const RECT mark{96, 186, 152, 242};
    ui::FillRoundRect(dc, mark, 16, RGB(96, 142, 238));
    ui::StrokeRoundRect(dc, mark, 16, RGB(146, 186, 255));
    DrawUiIcon(dc, UiIcon::Capture, RECT{mark.left + 14, mark.top + 14, mark.right - 14, mark.bottom - 14},
               RGB(255, 255, 255), 2);

    DrawText(dc, kProductName, RECT{20, 256, kBannerWidth - 20, 292}, RGB(240, 245, 252), g.fontBanner,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    DrawText(dc, g.uninstallMode ? L"Uninstaller" : L"Setup",
             RECT{20, 290, kBannerWidth - 20, 314}, RGB(140, 172, 224), g.fontSmall,
             DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    DrawText(dc, L"Native Windows capture\nNo Electron · no telemetry",
             RECT{20, kWindowHeight - 74, kBannerWidth - 20, kWindowHeight - 24}, RGB(104, 128, 170),
             g.fontSmall, DT_CENTER);
}

// Returns the y position just past this line, so callers can stack them without
// guessing how tall a wrapped hint turned out to be.
int PaintDependencyLine(HDC dc, int y, bool ok, const std::wstring& label, const std::wstring& hint) {
    const COLORREF accent = ok ? RGB(78, 205, 139) : RGB(244, 182, 76);
    DrawUiIcon(dc, ok ? UiIcon::Check : UiIcon::Warning, RECT{kContentLeft, y + 2, kContentLeft + 17, y + 19},
               accent, 2);
    DrawText(dc, label, RECT{kContentLeft + 26, y, kContentRight, y + 21}, RGB(214, 222, 234), g.fontBody,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    int next = y + 26;
    if (!ok && !hint.empty()) {
        const int width = kContentRight - (kContentLeft + 26);
        const int height = MeasureTextHeight(dc, hint, width, g.fontSmall);
        DrawText(dc, hint, RECT{kContentLeft + 26, y + 21, kContentRight, y + 21 + height},
                 RGB(176, 158, 118), g.fontSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);
        next = y + 21 + height + 8;
    }
    return next;
}

void PaintWelcome(HDC dc) {
    DrawText(dc, L"Install Luna Capture", RECT{kContentLeft, 38, kContentRight, 76}, RGB(243, 246, 252),
             g.fontTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const bool failed = !g.resolveError.empty();
    std::wstring subtitle;
    if (failed) {
        subtitle = g.resolveError;
    } else if (!g.release.version.empty()) {
        subtitle = L"Version " + g.release.version + L" will be downloaded from GitHub.";
        if (!g.pinnedVersion.empty()) subtitle += L"  ·  pinned in installer.ini";
    } else {
        subtitle = L"Looking up the latest release…";
    }

    const int width = kContentRight - kContentLeft;
    int y = 80;
    if (failed) {
        // Errors get a bordered panel sized to the message, so a long explanation
        // can never run under the section below it.
        const int inner = width - 68;
        const int textHeight = MeasureTextHeight(dc, subtitle, inner, g.fontSmall);
        const RECT panel{kContentLeft, y, kContentRight, y + textHeight + 26};
        ui::FillRoundRect(dc, panel, 11, RGB(38, 24, 27));
        ui::StrokeRoundRect(dc, panel, 11, RGB(112, 58, 66));
        DrawUiIcon(dc, UiIcon::Warning, RECT{panel.left + 16, panel.top + 14, panel.left + 36, panel.top + 34},
                   RGB(243, 130, 142), 2);
        DrawText(dc, subtitle, RECT{panel.left + 48, panel.top + 13, panel.right - 16, panel.bottom - 8},
                 RGB(232, 186, 192), g.fontSmall);
        y = panel.bottom + 16;
    } else {
        const int textHeight = MeasureTextHeight(dc, subtitle, width, g.fontBody);
        DrawText(dc, subtitle, RECT{kContentLeft, y, kContentRight, y + textHeight}, RGB(150, 159, 175),
                 g.fontBody);
        y += textHeight + 18;
    }

    if (!g.newerVersion.empty()) {
        const RECT notice{kContentLeft, y, kContentRight, y + 58};
        ui::FillRoundRect(dc, notice, 11, RGB(38, 32, 18));
        ui::StrokeRoundRect(dc, notice, 11, RGB(122, 96, 42));
        DrawUiIcon(dc, UiIcon::Warning, RECT{notice.left + 14, notice.top + 19, notice.left + 34, notice.top + 39},
                   RGB(246, 180, 72), 2);
        DrawText(dc, L"Version " + g.newerVersion + L" has been published since this installer was pinned to " +
                     g.release.version + L".",
                 RECT{notice.left + 44, notice.top + 10, notice.right - 126, notice.bottom - 8},
                 RGB(228, 198, 138), g.fontSmall);
        DrawButton(dc, RECT{notice.right - 116, notice.top + 12, notice.right - 12, notice.bottom - 12},
                   L"Use " + g.newerVersion, false, true, Control::UseNewerVersion);
        y = notice.bottom + 18;
    }

    DrawText(dc, L"SYSTEM CHECK", RECT{kContentLeft, y, kContentRight, y + 20}, RGB(108, 118, 136),
             g.fontSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y += 26;
    y = PaintDependencyLine(dc, y, g.dependencies.windowsVersionOk, L"Windows 10 1903 or newer",
                            g.dependencies.windowsHint);
    PaintDependencyLine(dc, y, g.dependencies.direct3DOk, L"Direct3D 11 graphics device",
                        g.dependencies.direct3DHint);
}

void PaintOptions(HDC dc) {
    DrawText(dc, L"Choose options", RECT{kContentLeft, 40, kContentRight, 78}, RGB(243, 246, 252),
             g.fontTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawText(dc, L"INSTALL FOLDER", RECT{kContentLeft, 84, kContentRight, 104}, RGB(108, 118, 136),
             g.fontSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    // The rounded frame is painted here; the text itself lives in a real edit
    // control positioned on top, so the path can be typed as well as browsed.
    const RECT pathBox{kContentLeft, 106, kContentRight - 104, 144};
    ui::FillRoundRect(dc, pathBox, 9, RGB(20, 23, 30));
    ui::StrokeRoundRect(dc, pathBox, 9, GetFocus() == g.pathEdit ? RGB(96, 148, 236) : RGB(56, 63, 76));
    DrawButton(dc, RECT{kContentRight - 94, 106, kContentRight, 144}, L"Browse…", false, true, Control::BrowsePath);

    int y = 158;
    DrawCheckbox(dc, kContentLeft - 6, y, kContentRight - kContentLeft + 12, g.options.desktopShortcut,
                 L"Desktop shortcut", {}, Control::ToggleDesktop);
    y += 32;
    DrawCheckbox(dc, kContentLeft - 6, y, kContentRight - kContentLeft + 12, g.options.startMenuShortcut,
                 L"Start menu shortcut", {}, Control::ToggleStartMenu);
    y += 32;
    DrawCheckbox(dc, kContentLeft - 6, y, kContentRight - kContentLeft + 12, g.options.startWithWindows,
                 L"Start with Windows", L"Launches hidden in the tray at sign-in.", Control::ToggleAutostart);
    y += 50;
    DrawCheckbox(dc, kContentLeft - 6, y, kContentRight - kContentLeft + 12, g.options.runAsAdmin,
                 L"Always run as administrator",
                 g.options.startWithWindows
                     ? std::wstring(L"With autostart this uses a sign-in task — installer must run as admin.")
                     : std::wstring(L"Needed for hotkeys to work inside elevated apps."),
                 Control::ToggleRunAsAdmin);
}

void PaintProgress(HDC dc) {
    DrawText(dc, g.uninstallMode ? L"Removing Luna Capture" : L"Installing Luna Capture",
             RECT{kContentLeft, 150, kContentRight, 190}, RGB(243, 246, 252), g.fontTitle,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    // A long asset name must clip rather than run into the progress bar below.
    DrawText(dc, g.statusMessage, RECT{kContentLeft, 194, kContentRight, 242}, RGB(150, 159, 175), g.fontBody,
             DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);

    const RECT track{kContentLeft, 254, kContentRight, 268};
    ui::FillRoundRect(dc, track, 7, RGB(24, 28, 36));
    ui::StrokeRoundRect(dc, track, 7, RGB(46, 52, 64));
    const int span = track.right - track.left - 4;
    const int filled = (span * (std::max)(0, (std::min)(100, g.progressPercent))) / 100;
    if (filled > 6) {
        const RECT bar{track.left + 2, track.top + 2, track.left + 2 + filled, track.bottom - 2};
        ui::FillRoundRectVGradient(dc, bar, 5, RGB(112, 164, 250), RGB(74, 122, 212));
    }
    DrawText(dc, std::to_wstring(g.progressPercent) + L"%", RECT{kContentLeft, 274, kContentRight, 296},
             RGB(118, 128, 145), g.fontSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void PaintDone(HDC dc) {
    const COLORREF accent = g.finishedOk ? RGB(78, 205, 139) : RGB(243, 112, 126);
    const RECT badge{kContentLeft, 132, kContentLeft + 56, 188};
    ui::FillEllipse(dc, badge, ui::MixColor(RGB(14, 16, 21), accent, 42.0f / 255.0f));
    DrawUiIcon(dc, g.finishedOk ? UiIcon::Check : UiIcon::Close,
               RECT{badge.left + 15, badge.top + 15, badge.right - 15, badge.bottom - 15}, accent, 2);

    const std::wstring title = g.finishedOk ? (g.uninstallMode ? L"Luna Capture was removed" : L"Installation complete")
                                            : (g.uninstallMode ? L"Uninstall failed" : L"Installation failed");
    DrawText(dc, title, RECT{kContentLeft, 200, kContentRight, 238}, RGB(243, 246, 252), g.fontTitle,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    // The install path can be long; clip instead of overrunning the link/footer.
    DrawText(dc, g.finishMessage, RECT{kContentLeft, 242, kContentRight, 332}, RGB(150, 159, 175), g.fontBody,
             DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);

    if (!g.installWarning.empty()) {
        // Something optional did not apply (usually the elevated sign-in task).
        // The install still succeeded, so this is a note, not a failure.
        const int inner = kContentRight - kContentLeft - 68;
        const int height = MeasureTextHeight(dc, g.installWarning, inner, g.fontSmall);
        const RECT panel{kContentLeft, 330, kContentRight, (std::min)(398, 330 + height + 24)};
        ui::FillRoundRect(dc, panel, 10, RGB(38, 32, 18));
        ui::StrokeRoundRect(dc, panel, 10, RGB(122, 96, 42));
        DrawUiIcon(dc, UiIcon::Warning, RECT{panel.left + 14, panel.top + 12, panel.left + 32, panel.top + 30},
                   RGB(246, 180, 72), 2);
        DrawText(dc, g.installWarning, RECT{panel.left + 44, panel.top + 10, panel.right - 12, panel.bottom - 6},
                 RGB(228, 198, 138), g.fontSmall, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    } else if (g.finishedOk && !g.uninstallMode) {
        DrawText(dc, L"github.com/LouSkull/LunaCapture",
                 RECT{kContentLeft, 336, kContentRight, 358}, RGB(110, 168, 255), g.fontSmall,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        AddRegion(RECT{kContentLeft, 336, kContentLeft + 230, 358}, Control::OpenRepository);
    }
}

void PaintUninstallConfirm(HDC dc) {
    DrawText(dc, L"Remove Luna Capture", RECT{kContentLeft, 100, kContentRight, 138}, RGB(243, 246, 252),
             g.fontTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const std::wstring installed = DetectInstalledVersion();
    DrawText(dc, installed.empty() ? std::wstring(L"This removes the program and its shortcuts.")
                                   : L"Version " + installed + L" will be removed, along with its shortcuts.",
             RECT{kContentLeft, 142, kContentRight, 200}, RGB(150, 159, 175), g.fontBody);

    DrawCheckbox(dc, kContentLeft - 6, 214, kContentRight - kContentLeft + 12, g.keepUserData,
                 L"Keep my settings and captures",
                 L"Screenshots are never deleted either way.", Control::ToggleKeepData);
}

void PaintWindow(HWND window) {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);

    HDC memory = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);

    g.regions.clear();
    Fill(memory, client, RGB(14, 16, 21));
    PaintBanner(memory);

    switch (g.page) {
        case Page::Welcome: PaintWelcome(memory); break;
        case Page::Options: PaintOptions(memory); break;
        case Page::Progress: PaintProgress(memory); break;
        case Page::Done: PaintDone(memory); break;
        case Page::UninstallConfirm: PaintUninstallConfirm(memory); break;
    }

    // Footer separator + buttons.
    if (g.page != Page::Progress) {
        Fill(memory, RECT{kBannerWidth, kWindowHeight - 74, kWindowWidth, kWindowHeight - 73}, RGB(32, 36, 45));
        const RECT primary{kContentRight - 132, kWindowHeight - 56, kContentRight, kWindowHeight - 18};
        const RECT secondary{kContentRight - 244, kWindowHeight - 56, kContentRight - 144, kWindowHeight - 18};
        switch (g.page) {
            case Page::Welcome:
                DrawButton(memory, secondary, L"Cancel", false, true, Control::Secondary);
                if (!g.resolveError.empty()) {
                    // A lookup failure is usually transient, so leave a way forward
                    // instead of a dead-end window.
                    DrawButton(memory, primary, L"Retry", true, true, Control::Primary);
                } else {
                    DrawButton(memory, primary, L"Continue", true,
                               !g.dependencies.BlocksInstall() && !g.release.version.empty(), Control::Primary);
                }
                break;
            case Page::Options:
                DrawButton(memory, secondary, L"Back", false, true, Control::Secondary);
                DrawButton(memory, primary, L"Install", true, true, Control::Primary);
                break;
            case Page::UninstallConfirm:
                DrawButton(memory, secondary, L"Cancel", false, true, Control::Secondary);
                DrawButton(memory, primary, L"Remove", true, true, Control::Primary, true);
                break;
            case Page::Done:
                if (g.finishedOk && !g.uninstallMode) {
                    DrawButton(memory, secondary, L"Close", false, true, Control::Secondary);
                    DrawButton(memory, primary, L"Launch", true, true, Control::Primary);
                } else {
                    DrawButton(memory, primary, L"Close", true, true, Control::Primary);
                }
                break;
            default: break;
        }
    }

    BitBlt(target, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(window, &paint);
}

// --- interaction ------------------------------------------------------------

Control HitTest(POINT point) {
    for (auto it = g.regions.rbegin(); it != g.regions.rend(); ++it) {
        if (PtInRect(&it->rect, point)) return it->control;
    }
    return Control::None;
}

bool BrowseForFolder(HWND owner, std::wstring& selected) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose the install folder");
    if (FAILED(dialog->Show(owner))) return false;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)) || !item) return false;
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return false;
    selected.assign(raw);
    CoTaskMemFree(raw);
    return true;
}

// The edit control only exists on the Options page; keep it in sync with the
// page and with whatever the Browse dialog last produced.
void SyncPathEdit() {
    if (!g.pathEdit) return;
    const bool visible = g.page == Page::Options;
    ShowWindow(g.pathEdit, visible ? SW_SHOW : SW_HIDE);
    if (visible) SetWindowTextW(g.pathEdit, g.options.installDirectory.c_str());
}

// Pulls whatever the user typed back into the options before it is used.
void CommitPathEdit() {
    if (!g.pathEdit) return;
    std::wstring value(1024, L'\0');
    const int length = GetWindowTextW(g.pathEdit, value.data(), static_cast<int>(value.size()));
    value.resize((std::max)(0, length));
    while (!value.empty() && (value.back() == L' ' || value.back() == L'\\')) value.pop_back();
    while (!value.empty() && value.front() == L' ') value.erase(value.begin());
    if (!value.empty()) g.options.installDirectory = value;
}

void PostProgress(int percent, const std::wstring& message) {
    PostMessageW(g.window, WM_APP_INSTALL_PROGRESS, static_cast<WPARAM>(percent),
                 reinterpret_cast<LPARAM>(new std::wstring(message)));
}

void StartWork() {
    g.busy = true;
    g.page = Page::Progress;
    g.progressPercent = 0;
    g.installWarning.clear();
    g.statusMessage = g.uninstallMode ? L"Starting removal…" : L"Starting…";
    SyncPathEdit();
    InvalidateRect(g.window, nullptr, FALSE);

    const bool uninstall = g.uninstallMode;
    const bool keepData = g.keepUserData;
    const InstallOptions options = g.options;
    const ReleaseInfo release = g.release;

    std::thread([uninstall, keepData, options, release]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::wstring error;
        std::wstring warning;
        const ProgressCallback callback = [](int percent, const std::wstring& message) {
            PostProgress(percent, message);
        };
        const bool ok = uninstall ? RunUninstall(keepData, callback, error)
                                  : RunInstall(options, release, callback, error, warning);
        CoUninitialize();
        // On success the payload carries the non-fatal warning (if any); on
        // failure it carries the reason.
        PostMessageW(g.window, WM_APP_INSTALL_FINISHED, ok ? 1u : 0u,
                     reinterpret_cast<LPARAM>(new std::wstring(ok ? warning : error)));
    }).detach();
}

// Switches the pending download to the newest published release. Kept as an
// inline control rather than a message box so the installer never shows stock
// Windows chrome.
void SwitchToNewerVersion() {
    if (g.newerVersion.empty()) return;
    ReleaseInfo newer;
    std::wstring ignored;
    std::wstring error;
    if (ResolveRelease({}, newer, ignored, error)) {
        g.release = newer;
        g.newerVersion.clear();
        g.pinnedVersion.clear();
    } else {
        g.resolveError = error;
    }
}

void HandleControl(Control control) {
    switch (control) {
        case Control::UseNewerVersion:
            SwitchToNewerVersion();
            break;
        case Control::Primary:
            if (g.page == Page::Welcome) {
                if (!g.resolveError.empty()) {
                    g.resolveError.clear();
                    g.release = {};
                    g.newerVersion.clear();
                    ResolveRelease(g.pinnedVersion, g.release, g.newerVersion, g.resolveError);
                } else {
                    g.page = Page::Options;
                    SyncPathEdit();
                }
            } else if (g.page == Page::Options) {
                CommitPathEdit();
                StartWork();
            } else if (g.page == Page::UninstallConfirm) {
                StartWork();
            } else if (g.page == Page::Done) {
                if (g.finishedOk && !g.uninstallMode) {
                    const std::wstring exe = InstalledExecutablePath(g.options.installDirectory);
                    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr,
                                  std::filesystem::path(exe).parent_path().c_str(), SW_SHOWNORMAL);
                }
                DestroyWindow(g.window);
            }
            break;
        case Control::Secondary:
            if (g.page == Page::Options) {
                CommitPathEdit();
                g.page = Page::Welcome;
                SyncPathEdit();
            } else {
                DestroyWindow(g.window);
            }
            break;
        case Control::BrowsePath: {
            std::wstring picked;
            if (BrowseForFolder(g.window, picked)) {
                // Keep the product folder name so users do not install loose files
                // into the root of Documents or a drive.
                std::filesystem::path chosen(picked);
                if (chosen.filename() != L"Luna Capture") chosen /= L"Luna Capture";
                g.options.installDirectory = chosen.wstring();
                SyncPathEdit();
            }
            break;
        }
        case Control::ToggleDesktop: g.options.desktopShortcut = !g.options.desktopShortcut; break;
        case Control::ToggleStartMenu: g.options.startMenuShortcut = !g.options.startMenuShortcut; break;
        // Both options can be on together: that combination is served by a
        // logon-triggered scheduled task instead of a Startup shortcut, because a
        // shortcut plus a RUNASADMIN shim never launches at sign-in.
        case Control::ToggleAutostart:
            g.options.startWithWindows = !g.options.startWithWindows;
            break;
        case Control::ToggleRunAsAdmin:
            g.options.runAsAdmin = !g.options.runAsAdmin;
            break;
        case Control::ToggleKeepData: g.keepUserData = !g.keepUserData; break;
        case Control::OpenRepository:
            ShellExecuteW(nullptr, L"open", L"https://github.com/LouSkull/LunaCapture", nullptr, nullptr,
                          SW_SHOWNORMAL);
            break;
        default: break;
    }
    InvalidateRect(g.window, nullptr, FALSE);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            g.window = window;
            g.fontTitle = ui::CreateThemeFont(25, FW_SEMIBOLD);
            g.fontHeading = ui::CreateThemeFont(18, FW_SEMIBOLD);
            g.fontBody = ui::CreateThemeFont(15, FW_NORMAL);
            g.fontSmall = ui::CreateThemeFont(13, FW_NORMAL);
            g.fontBanner = ui::CreateThemeFont(24, FW_SEMIBOLD);
            g.editBrush = CreateSolidBrush(RGB(20, 23, 30));
            g.pathEdit = CreateWindowExW(0, L"EDIT", L"",
                                         WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, kContentLeft + 10, 116,
                                         kContentRight - 104 - kContentLeft - 20, 20, window,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPathEditId)),
                                         GetModuleHandleW(nullptr), nullptr);
            if (g.pathEdit) {
                SendMessageW(g.pathEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g.fontSmall), TRUE);
                SendMessageW(g.pathEdit, EM_SETLIMITTEXT, 512, 0);
                // Seed it now: the page transition is not the only way to arrive
                // here, and an empty box would read as "no folder chosen".
                SyncPathEdit();
            }
            const BOOL dark = TRUE;
            const DWORD round = 2;
            DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
            DwmSetWindowAttribute(window, 33, &round, sizeof(round));
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, RGB(214, 222, 236));
            SetBkColor(dc, RGB(20, 23, 30));
            return reinterpret_cast<LRESULT>(g.editBrush);
        }
        case WM_PAINT: PaintWindow(window); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const Control hit = g.busy ? Control::None : HitTest(point);
            if (hit != g.hovered) {
                g.hovered = hit;
                InvalidateRect(window, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            g.hovered = Control::None;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            if (g.busy) return 0;
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            g.pressed = HitTest(point);
            if (g.pressed == Control::None) {
                // Classic installers are draggable by their chrome.
                ReleaseCapture();
                SendMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g.busy) return 0;
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const Control released = g.pressed;
            g.pressed = Control::None;
            ReleaseCapture();
            if (released != Control::None && HitTest(point) == released) HandleControl(released);
            else InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_APP_INSTALL_PROGRESS: {
            std::unique_ptr<std::wstring> payload(reinterpret_cast<std::wstring*>(lParam));
            g.progressPercent = static_cast<int>(wParam);
            if (payload && !payload->empty()) g.statusMessage = *payload;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_APP_INSTALL_FINISHED: {
            std::unique_ptr<std::wstring> payload(reinterpret_cast<std::wstring*>(lParam));
            g.busy = false;
            g.finishedOk = wParam != 0;
            g.page = Page::Done;
            g.installWarning = g.finishedOk && payload ? *payload : std::wstring();
            if (g.finishedOk) {
                g.finishMessage = g.uninstallMode
                    ? L"The program and its shortcuts are gone. This window closes the uninstaller when you press Close."
                    : L"Luna Capture " + g.release.version + L" is installed in " + g.options.installDirectory +
                          L".\n\nPress Launch to start it now — it lives in the system tray.";
            } else {
                g.finishMessage = payload && !payload->empty() ? *payload : std::wstring(L"An unknown error occurred.");
            }
            SyncPathEdit();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE && !g.busy) DestroyWindow(window);
            return 0;
        case WM_CLOSE:
            if (g.busy) return 0;  // never leave a half-written install behind
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            for (HFONT* font : {&g.fontTitle, &g.fontHeading, &g.fontBody, &g.fontSmall, &g.fontBanner}) {
                if (*font) { DeleteObject(*font); *font = nullptr; }
            }
            if (g.editBrush) { DeleteObject(g.editBrush); g.editBrush = nullptr; }
            g.pathEdit = nullptr;
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ui::InitGraphics();

    const std::wstring arguments = commandLine ? commandLine : L"";

    // Uninstall mode is decided by the argument *or* by this binary's own name.
    //
    // Install copies setup to "<install dir>\LunaUninstall.exe" and registers it
    // with "--uninstall" in Add/Remove Programs. Nothing passes that argument
    // when the user double-clicks that file in the install folder, though — and
    // a file called LunaUninstall.exe is the obvious thing to double-click — so
    // it used to come up as the installer and offer to install over itself.
    bool namedUninstaller = false;
    {
        std::wstring modulePath(MAX_PATH * 4, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        modulePath.resize(length);
        std::wstring stem = std::filesystem::path(modulePath).stem().wstring();
        for (wchar_t& ch : stem) ch = static_cast<wchar_t>(towlower(ch));
        namedUninstaller = stem.find(L"uninstall") != std::wstring::npos;
    }
    // Explicit --install still wins, so a copy that happens to be named badly can
    // always be forced back into installing.
    const bool forceInstall = arguments.find(L"--install") != std::wstring::npos;
    g.uninstallMode = !forceInstall &&
                      (namedUninstaller || arguments.find(L"--uninstall") != std::wstring::npos);

    if (g.uninstallMode) {
        g.page = Page::UninstallConfirm;
    } else {
        g.options.installDirectory = DefaultInstallDirectory();
        const std::wstring existing = DetectExistingInstall();
        if (!existing.empty()) g.options.installDirectory = existing;
        g.dependencies = CheckDependencies();
        g.pinnedVersion = ReadPinnedVersion();
        if (!ResolveRelease(g.pinnedVersion, g.release, g.newerVersion, g.resolveError)) {
            // Keep the window up so the reason is readable instead of flashing away.
        }
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - kWindowWidth) / 2;
    const int y = work.top + ((work.bottom - work.top) - kWindowHeight) / 2;
    HWND window = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass,
                                  g.uninstallMode ? L"Luna Capture Uninstaller" : L"Luna Capture Setup",
                                  WS_POPUP, x, y, kWindowWidth, kWindowHeight, nullptr, nullptr, instance, nullptr);
    if (!window) {
        ui::ShutdownGraphics();
        CoUninitialize();
        return 1;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    ui::ShutdownGraphics();
    CoUninitialize();
    return 0;
}
