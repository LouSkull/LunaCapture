#include "QuickPanel.hpp"

#include "UiGfx.hpp"
#include "UiIcons.hpp"
#include "WinUtil.hpp"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace {
constexpr wchar_t kQuickPanelClass[] = L"LunaCaptureQuickPanel";
constexpr int kWidth = 326;
constexpr int kBaseHeight = 516;
constexpr std::size_t kMaxPluginSlots = 3;
// Extra room for the "PLUGIN COMMANDS" header plus up to kMaxPluginSlots rows,
// only reserved when at least one plugin command is available.
constexpr int kExpandedHeight = kBaseHeight + 154;
// Room reclaimed when the "MONITOR REPLAY" pills are hidden entirely (fewer than two
// monitors, see QuickPanelState::monitorCount) - a single-monitor rig has nothing for
// per-monitor save pills to distinguish, and "Save all replay channels" already covers it.
constexpr int kMonitorSectionHeight = 91;
constexpr int kRadius = 22;
constexpr int kMargin = 20;
// How far the panel travels on entry. Small on purpose: the whole slide has to
// fit between the monitor's left edge and the panel's resting position, because
// anything further left is another monitor.
constexpr int kSlideDistance = 20;
constexpr BYTE kSlideStartAlpha = 30;
constexpr int kSlideDurationMs = 145;
constexpr UINT kSlideFrameMs = 16;
constexpr UINT kBackdropFrameMs = 33;
constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT_PTR kTopMostTimer = 2;

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(ui::ResolveThemeColor(color));
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void Text(HDC dc, const std::wstring& text, RECT rect, COLORREF color, HFONT font,
          UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    // Route through the active language pack so localization plugins reach the
    // Quick Panel, exactly like the main window, tray and region editor.
    const std::wstring translated = ui::TranslateText(text);
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ui::ResolveThemeColor(color));
    DrawTextW(dc, translated.c_str(), -1, &rect, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

void PaintRoundedThemeBackdrop(HDC dc, const RECT& rect, int radius) {
    if (!dc || IsRectEmpty(&rect)) return;
    const int saved = SaveDC(dc);
    if (!saved) return;

    const int rectWidth = static_cast<int>(rect.right - rect.left);
    const int rectHeight = static_cast<int>(rect.bottom - rect.top);
    const int resolvedRadius = (std::min)(ui::ResolveThemeRadius(radius),
                                          (std::min)(rectWidth, rectHeight) / 2);
    HRGN clip = resolvedRadius > 0
        ? CreateRoundRectRgn(rect.left, rect.top, rect.right + 1, rect.bottom + 1,
                             resolvedRadius * 2, resolvedRadius * 2)
        : CreateRectRgn(rect.left, rect.top, rect.right, rect.bottom);
    if (clip) {
        const int clipResult = ExtSelectClipRgn(dc, clip, RGN_AND);
        DeleteObject(clip);
        if (clipResult != ERROR) {
            ui::PaintThemeBackdrop(dc, rect, ui::ThemeSurface::Panel, GetTickCount64());
        }
    }
    RestoreDC(dc, saved);
}

UiIcon PluginIcon(const std::wstring& name) {
    if (name == L"monitor") return UiIcon::Monitor;
    if (name == L"displays") return UiIcon::AllDisplays;
    if (name == L"region") return UiIcon::Region;
    if (name == L"replay") return UiIcon::Replay;
    if (name == L"record") return UiIcon::Record;
    if (name == L"folder") return UiIcon::Folder;
    if (name == L"photo") return UiIcon::Photo;
    if (name == L"video") return UiIcon::Video;
    if (name == L"save") return UiIcon::Save;
    if (name == L"settings") return UiIcon::Settings;
    return UiIcon::Plugin;
}
}

QuickPanel::~QuickPanel() { Destroy(); }

bool QuickPanel::Create(HINSTANCE instance, Callback callback) {
    instance_ = instance;
    callback_ = std::move(callback);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kQuickPanelClass;
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    // WS_EX_LAYERED carries the entrance fade. It replaces the old long slide from
    // off-screen, which could only enter from outside the monitor's left edge.
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                            kQuickPanelClass, L"Luna Quick Panel", WS_POPUP,
                            -kWidth, 100, kWidth, kBaseHeight, nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;
    const BOOL dark = TRUE;
    // DWMWCP_DONOTROUND. The panel already clips itself to a rounded region and
    // paints its own shell, and asking DWM to round it as well made the compositor
    // draw a shadow sized to the *unclipped* window rect - the stray dark band that
    // showed up next to the panel, and bled onto the adjacent monitor.
    const DWORD cornerPreference = 1;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd_, 33, &cornerPreference, sizeof(cornerPreference));
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    ApplyWindowShape(kWidth, kBaseHeight);
    // Must be visible to screen capture (recording / Discord / OBS), so keep the
    // panel included in capture instead of excluding it or painting it black.
    SetWindowDisplayAffinity(hwnd_, WDA_NONE);
    if (!RecreateFonts()) {
        Destroy();
        return false;
    }
    return true;
}

bool QuickPanel::RecreateFonts() {
    HFONT newHeadline = ui::CreateThemeFont(20, FW_SEMIBOLD);
    HFONT newTitle = ui::CreateThemeFont(15, FW_SEMIBOLD);
    HFONT newSmall = ui::CreateThemeFont(13, FW_NORMAL);
    if (!newHeadline || !newTitle || !newSmall) {
        if (newHeadline) DeleteObject(newHeadline);
        if (newTitle) DeleteObject(newTitle);
        if (newSmall) DeleteObject(newSmall);
        return false;
    }

    HFONT oldHeadline = headlineFont_;
    HFONT oldTitle = titleFont_;
    HFONT oldSmall = smallFont_;
    headlineFont_ = newHeadline;
    titleFont_ = newTitle;
    smallFont_ = newSmall;
    if (oldHeadline) DeleteObject(oldHeadline);
    if (oldTitle) DeleteObject(oldTitle);
    if (oldSmall) DeleteObject(oldSmall);
    return true;
}

void QuickPanel::ReleaseBackBuffer() {
    if (memDc_ && memOld_) { SelectObject(memDc_, memOld_); memOld_ = nullptr; }
    if (memBitmap_) { DeleteObject(memBitmap_); memBitmap_ = nullptr; }
    if (memDc_) { DeleteDC(memDc_); memDc_ = nullptr; }
    memW_ = memH_ = 0;
}

bool QuickPanel::EnsureBackBuffer(HDC reference, int width, int height) {
    if (memDc_ && memW_ == width && memH_ == height) return true;
    ReleaseBackBuffer();
    memDc_ = CreateCompatibleDC(reference);
    memBitmap_ = CreateCompatibleBitmap(reference, width, height);
    if (!memDc_ || !memBitmap_) { ReleaseBackBuffer(); return false; }
    memOld_ = SelectObject(memDc_, memBitmap_);
    memW_ = width;
    memH_ = height;
    return true;
}

int QuickPanel::PanelHeight() const {
    int height = state_.pluginCommands.empty() ? kBaseHeight : kExpandedHeight;
    if (state_.monitorCount < 2) height -= kMonitorSectionHeight;
    return height;
}

void QuickPanel::ApplyWindowShape(int width, int height) {
    if (!hwnd_) return;
    const int radius = (std::min)(ui::ResolveThemeRadius(kRadius), (std::min)(width, height) / 2);
    HRGN region = radius > 0
        ? CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2, radius * 2)
        : CreateRectRgn(0, 0, width, height);
    if (region && !SetWindowRgn(hwnd_, region, TRUE)) DeleteObject(region);
    currentHeight_ = height;
}

void QuickPanel::UpdateVisualTimer() {
    if (!hwnd_ || !IsWindow(hwnd_)) return;
    if (!IsWindowVisible(hwnd_) || (!slideAnimationActive_ && !ui::IsThemeBackdropAnimated())) {
        KillTimer(hwnd_, kAnimationTimer);
        return;
    }
    SetTimer(hwnd_, kAnimationTimer, slideAnimationActive_ ? kSlideFrameMs : kBackdropFrameMs, nullptr);
}

void QuickPanel::ReassertTopMost() {
    if (!IsVisible()) return;
    // A true exclusive-fullscreen D3D surface (most games) can only be covered by kicking the
    // app out of exclusive mode on every call, which has been observed to hang the whole system,
    // not just this window - so back off entirely rather than fight a surface we can never
    // actually win against. Show() minimizes the game before this can even come up in practice.
    if (IsExclusiveFullscreenAppActive()) return;
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
}

void QuickPanel::Destroy() {
    if (hwnd_ && IsWindow(hwnd_)) {
        KillTimer(hwnd_, kAnimationTimer);
        KillTimer(hwnd_, kTopMostTimer);
    }
    slideAnimationActive_ = false;
    ReleaseBackBuffer();
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    if (headlineFont_) DeleteObject(headlineFont_);
    if (titleFont_) DeleteObject(titleFont_);
    if (smallFont_) DeleteObject(smallFont_);
    headlineFont_ = titleFont_ = smallFont_ = nullptr;
}

bool QuickPanel::IsVisible() const { return hwnd_ && IsWindowVisible(hwnd_); }

void QuickPanel::SetState(const QuickPanelState& state) {
    state_ = state;
    if (!IsVisible()) return;
    const int height = PanelHeight();
    if (height != currentHeight_) {
        // Keep the panel visually centered on its monitor as it grows/shrinks
        // (e.g. a plugin was hot-reloaded with new quick commands while open),
        // without letting the new height push it off that monitor.
        targetY_ -= (height - currentHeight_) / 2;
        if (!IsRectEmpty(&monitorRect_)) {
            const int lowest = (std::max)(static_cast<int>(monitorRect_.top), static_cast<int>(monitorRect_.bottom) - height);
            targetY_ = (std::clamp)(targetY_, static_cast<int>(monitorRect_.top), lowest);
        }
        SetWindowPos(hwnd_, HWND_TOPMOST, targetX_, targetY_, kWidth, height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
        ApplyWindowShape(kWidth, height);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPanel::Refresh() {
    RefreshTheme();
}

void QuickPanel::RefreshTheme() {
    RecreateFonts();
    if (!hwnd_ || !IsWindow(hwnd_)) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (client.right > client.left && client.bottom > client.top) {
        ApplyWindowShape(client.right - client.left, client.bottom - client.top);
    }
    UpdateVisualTimer();
    if (IsVisible()) {
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
}

void QuickPanel::Show(const QuickPanelState& state, const std::wstring& hotkeyLabel) {
    if (!hwnd_) return;
    state_ = state;
    hotkeyLabel_ = hotkeyLabel;
    const HWND foreground = GetForegroundWindow();
    HMONITOR monitorHandle = MonitorFromWindow(foreground, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(monitorHandle, &monitor)) return;
    // True exclusive fullscreen can never actually composite the panel on top of it (DWM stops
    // compositing that surface entirely). Minimize the game instead of fighting for the topmost
    // band, so the panel is actually visible; Hide() restores focus to it automatically.
    minimizedGameWindow_ = nullptr;
    if (foreground && foreground != hwnd_ && IsExclusiveFullscreenAppActive()) {
        ShowWindow(foreground, SW_MINIMIZE);
        minimizedGameWindow_ = foreground;
    }
    const int height = PanelHeight();
    monitorRect_ = monitor.rcMonitor;
    // Clamp the resting position so the panel is fully inside this monitor even on
    // a display shorter than the panel itself.
    const int monitorWidth = monitorRect_.right - monitorRect_.left;
    const int monitorHeight = monitorRect_.bottom - monitorRect_.top;
    targetX_ = monitorRect_.left + (std::min)(kMargin, (std::max)(0, monitorWidth - kWidth));
    targetY_ = monitorRect_.top + (std::max)(0, (monitorHeight - height) / 2);
    // The entrance never crosses the monitor's left edge, so neither the panel nor
    // its shadow can appear on the display next door while it animates in.
    slideStartX_ = (std::max)(static_cast<int>(monitorRect_.left), targetX_ - kSlideDistance);
    animationStarted_ = GetTickCount64();
    animationDurationMs_ = ui::ResolveThemeDuration(kSlideDurationMs);
    slideAnimationActive_ = animationDurationMs_ > 0;
    ApplyWindowShape(kWidth, height);
    SetLayeredWindowAttributes(hwnd_, 0, slideAnimationActive_ ? kSlideStartAlpha : 255, LWA_ALPHA);
    SetWindowPos(hwnd_, HWND_TOPMOST, slideAnimationActive_ ? slideStartX_ : targetX_, targetY_, kWidth, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateVisualTimer();
    // Keep the panel pinned above fullscreen games that re-grab the foreground.
    SetTimer(hwnd_, kTopMostTimer, 400, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPanel::Hide() {
    if (!hwnd_) return;
    slideAnimationActive_ = false;
    KillTimer(hwnd_, kAnimationTimer);
    KillTimer(hwnd_, kTopMostTimer);
    ShowWindow(hwnd_, SW_HIDE);
    // Leave the window fully opaque and parked at its resting spot. A hide that
    // lands mid-fade otherwise reopens translucent on the next Show.
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    hovered_ = Selection{};
    pressed_ = Selection{};
    if (minimizedGameWindow_ && IsWindow(minimizedGameWindow_)) {
        ShowWindow(minimizedGameWindow_, SW_RESTORE);
        SetForegroundWindow(minimizedGameWindow_);
    }
    minimizedGameWindow_ = nullptr;
}

void QuickPanel::Toggle(const QuickPanelState& state, const std::wstring& hotkeyLabel) {
    if (IsVisible()) Hide();
    else Show(state, hotkeyLabel);
}

const QuickPanel::HitItem* QuickPanel::HitTest(POINT point) const {
    for (auto it = hitItems_.rbegin(); it != hitItems_.rend(); ++it) {
        if (it->action != QuickPanelAction::None && PtInRect(&it->rect, point)) return &(*it);
    }
    return nullptr;
}

void QuickPanel::Tick() {
    if (!IsVisible()) {
        slideAnimationActive_ = false;
        UpdateVisualTimer();
        return;
    }

    bool moved = false;
    if (slideAnimationActive_) {
        const double t = (std::min)(1.0, static_cast<double>(GetTickCount64() - animationStarted_) /
                                             static_cast<double>((std::max)(1, animationDurationMs_)));
        const double eased = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
        const int x = slideStartX_ + static_cast<int>((targetX_ - slideStartX_) * eased);
        SetWindowPos(hwnd_, HWND_TOPMOST, x, targetY_, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
        const double alpha = kSlideStartAlpha + (255.0 - kSlideStartAlpha) * eased;
        SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>((std::min)(255.0, alpha)), LWA_ALPHA);
        moved = true;
        if (t >= 1.0) {
            slideAnimationActive_ = false;
            SetWindowPos(hwnd_, HWND_TOPMOST, targetX_, targetY_, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
            SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        }
    }
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    if (moved) DwmFlush();
    UpdateVisualTimer();
}

void QuickPanel::Paint() {
    PAINTSTRUCT paint{};
    HDC windowDc = BeginPaint(hwnd_, &paint);
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (!EnsureBackBuffer(windowDc, client.right, client.bottom)) { EndPaint(hwnd_, &paint); return; }
    HDC dc = memDc_;
    Fill(dc, client, RGB(15, 17, 22));
    const RECT shell{12, 12, client.right - 12, client.bottom - 12};
    ui::FillRoundRect(dc, shell, 18, RGB(20, 23, 29));
    PaintRoundedThemeBackdrop(dc, shell, 18);
    Text(dc, L"QUICK CAPTURE", RECT{24, 20, 232, 51}, RGB(244, 247, 252), headlineFont_);
    RECT key{232, 20, 292, 49};
    ui::FillRoundRect(dc, key, 9, RGB(31, 38, 49));
    Text(dc, hotkeyLabel_, key, RGB(130, 177, 245), smallFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    ui::FillRoundRect(dc, RECT{24, 60, 302, 82}, 8, state_.recording ? RGB(65, 31, 39) :
                     state_.replay ? RGB(30, 45, 68) : RGB(27, 32, 40));
    Text(dc, state_.recording ? L"● RECORDING LIVE" : state_.replay ? L"● REPLAY BUFFER LIVE" : L"● READY",
         RECT{34, 60, 292, 82}, state_.recording ? RGB(245, 106, 120) :
         state_.replay ? RGB(116, 174, 255) : RGB(105, 204, 151), smallFont_);

    const auto tile = [&](RECT rect, QuickPanelAction action, UiIcon icon, const std::wstring& title,
                          const std::wstring& detail, COLORREF accent) {
        const bool hover = hovered_.action == action;
        ui::FillRoundRect(dc, rect, 13, hover ? ui::MixColor(RGB(24, 29, 37), accent, 0.16f) : RGB(24, 28, 35));
        ui::StrokeRoundRect(dc, rect, 13, hover ? ui::MixColor(accent, RGB(255, 255, 255), 0.12f) : RGB(42, 48, 59));
        DrawUiIcon(dc, icon, RECT{rect.left + 14, rect.top + 15, rect.left + 38, rect.top + 39}, accent, 2);
        Text(dc, title, RECT{rect.left + 48, rect.top + 4, rect.right - 10, rect.top + 31}, RGB(234, 238, 245), titleFont_);
        Text(dc, detail, RECT{rect.left + 48, rect.top + 29, rect.right - 10, rect.bottom - 4}, RGB(107, 119, 137), smallFont_);
    };

    hitItems_[0] = {RECT{24, 94, 159, 158}, QuickPanelAction::ToggleRecording, 0};
    hitItems_[1] = {RECT{167, 94, 302, 158}, QuickPanelAction::ToggleReplay, 0};
    tile(hitItems_[0].rect, hitItems_[0].action, UiIcon::Record, state_.recording ? L"Stop" : L"Record",
         state_.recording ? L"Finish video" : L"Start video", RGB(239, 91, 106));
    tile(hitItems_[1].rect, hitItems_[1].action, UiIcon::Replay, state_.replay ? L"Stop replay" : L"Replay",
         std::to_wstring(state_.replaySeconds) + L" second buffer", RGB(103, 154, 246));

    hitItems_[2] = {RECT{24, 168, 302, 220}, QuickPanelAction::CaptureActive, 0};
    hitItems_[3] = {RECT{24, 228, 302, 280}, QuickPanelAction::SaveAllReplays, 0};
    tile(hitItems_[2].rect, hitItems_[2].action, UiIcon::Capture, L"Screenshot", L"Capture the active game display", RGB(111, 171, 249));
    tile(hitItems_[3].rect, hitItems_[3].action, UiIcon::Save, L"Save all replay channels", L"One synchronized save action", RGB(158, 132, 242));

    // A single-monitor rig has nothing for per-monitor save pills to distinguish - "Save all
    // replay channels" above already does the job, so skip the section and reclaim its space
    // instead of showing two permanently-dead "Monitor 2"/"Monitor 3" placeholders.
    const bool hasMonitorSection = state_.monitorCount >= 2;
    const int monitorOffset = hasMonitorSection ? 0 : -kMonitorSectionHeight;
    if (hasMonitorSection) {
        Text(dc, L"MONITOR REPLAY", RECT{24, 289, 302, 314}, RGB(94, 106, 123), smallFont_);
        const int shownMonitors = (std::min)(state_.monitorCount, 3);
        for (int index = 0; index < 3; ++index) {
            if (index >= shownMonitors) { hitItems_[4 + index] = {}; continue; }
            RECT rect{24 + index * 94, 316, 110 + index * 94, 366};
            const QuickPanelAction action = static_cast<QuickPanelAction>(static_cast<int>(QuickPanelAction::SaveMonitor0) + index);
            hitItems_[4 + index] = {rect, action, 0};
            const bool available = state_.monitorAvailable[static_cast<std::size_t>(index)];
            ui::FillRoundRect(dc, rect, 11, hovered_.action == action ? RGB(38, 48, 63) : RGB(26, 31, 39));
            ui::StrokeRoundRect(dc, rect, 11, available ? RGB(53, 83, 126) : RGB(39, 45, 55));
            Text(dc, state_.monitorLabels[static_cast<std::size_t>(index)], RECT{rect.left + 6, rect.top + 2, rect.right - 6, rect.top + 26},
                 available ? RGB(166, 196, 237) : RGB(91, 101, 116), smallFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            Text(dc, L"SAVE", RECT{rect.left + 6, rect.top + 24, rect.right - 6, rect.bottom - 2},
                 available ? RGB(112, 161, 231) : RGB(73, 81, 94), smallFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    } else {
        for (int index = 0; index < 3; ++index) hitItems_[4 + index] = {};
    }

    // Plugin commands (from enabled plugins) get their own quick-access section so
    // they no longer require opening the main Plugins page to run.
    const bool hasPluginCommands = !state_.pluginCommands.empty();
    if (hasPluginCommands) {
        Text(dc, L"PLUGIN COMMANDS", RECT{24, 380 + monitorOffset, 302, 402 + monitorOffset}, RGB(94, 106, 123), smallFont_);
        const std::size_t shown = (std::min)(state_.pluginCommands.size(), kMaxPluginSlots);
        for (std::size_t i = 0; i < kMaxPluginSlots; ++i) {
            if (i >= shown) { hitItems_[10 + i] = {}; continue; }
            const int top = 406 + monitorOffset + static_cast<int>(i) * 40;
            RECT rect{24, top, 302, top + 34};
            hitItems_[10 + i] = {rect, QuickPanelAction::PluginCommand, i};
            const auto& command = state_.pluginCommands[i];
            const bool hover = hovered_.action == QuickPanelAction::PluginCommand && hovered_.dataIndex == i;
            ui::FillRoundRect(dc, rect, 10, hover ? RGB(38, 48, 63) : RGB(24, 28, 35));
            ui::StrokeRoundRect(dc, rect, 10, hover ? RGB(112, 161, 231) : RGB(42, 48, 59));
            DrawUiIcon(dc, PluginIcon(command.icon), RECT{rect.left + 10, rect.top + 7, rect.left + 30, rect.top + 27},
                       RGB(158, 132, 242), 2);
            Text(dc, command.title, RECT{rect.left + 38, rect.top + 2, rect.right - 10, rect.top + 19}, RGB(226, 230, 240), smallFont_);
            Text(dc, command.description, RECT{rect.left + 38, rect.top + 18, rect.right - 10, rect.bottom - 2}, RGB(107, 119, 137), smallFont_);
        }
    } else {
        for (std::size_t i = 0; i < kMaxPluginSlots; ++i) hitItems_[10 + i] = {};
    }

    const int sectionsY = (hasPluginCommands ? 534 : 380) + monitorOffset;
    hitItems_[7] = {RECT{24, sectionsY, 159, sectionsY + 60}, QuickPanelAction::OpenPhotos, 0};
    hitItems_[8] = {RECT{167, sectionsY, 302, sectionsY + 60}, QuickPanelAction::OpenVideos, 0};
    tile(hitItems_[7].rect, hitItems_[7].action, UiIcon::Photo, L"Photos", L"Open library", RGB(109, 185, 236));
    tile(hitItems_[8].rect, hitItems_[8].action, UiIcon::Video, L"Videos", L"Open library", RGB(167, 129, 241));

    const int closeY = sectionsY + 74;
    hitItems_[9] = {RECT{24, closeY, 302, closeY + 38}, QuickPanelAction::Close, 0};
    ui::FillRoundRect(dc, hitItems_[9].rect, 11, hovered_.action == QuickPanelAction::Close ? RGB(45, 38, 43) : RGB(27, 31, 38));
    Text(dc, L"Close quick panel", hitItems_[9].rect, RGB(142, 150, 163), smallFont_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    BitBlt(windowDc, paint.rcPaint.left, paint.rcPaint.top,
           paint.rcPaint.right - paint.rcPaint.left, paint.rcPaint.bottom - paint.rcPaint.top,
           dc, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
    EndPaint(hwnd_, &paint);
}

LRESULT QuickPanel::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: Paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            if (wParam == kAnimationTimer) { Tick(); return 0; }
            if (wParam == kTopMostTimer) { ReassertTopMost(); return 0; }
            break;
        case WM_NCHITTEST: return HTCLIENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const HitItem* hit = HitTest(point);
            const Selection next = hit ? Selection{hit->action, hit->dataIndex} : Selection{};
            if (next.action != hovered_.action || next.dataIndex != hovered_.dataIndex) {
                hovered_ = next;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hovered_ = Selection{};
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const HitItem* hit = HitTest(point);
            pressed_ = hit ? Selection{hit->action, hit->dataIndex} : Selection{};
            if (pressed_.action != QuickPanelAction::None) SetCapture(hwnd_);
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const HitItem* hit = HitTest(point);
            const Selection released = hit ? Selection{hit->action, hit->dataIndex} : Selection{};
            const bool matches = released.action == pressed_.action && released.dataIndex == pressed_.dataIndex;
            const Selection execute = matches ? released : Selection{};
            pressed_ = Selection{};
            ReleaseCapture();
            if (execute.action == QuickPanelAction::Close) {
                Hide();
                if (callback_) callback_(execute.action, execute.dataIndex);
            }
            else if (execute.action != QuickPanelAction::None && callback_) {
                if (execute.action == QuickPanelAction::CaptureActive || execute.action == QuickPanelAction::OpenPhotos ||
                    execute.action == QuickPanelAction::OpenVideos || execute.action == QuickPanelAction::PluginCommand) Hide();
                callback_(execute.action, execute.dataIndex);
            }
            return 0;
        }
        case WM_CLOSE: Hide(); return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kAnimationTimer);
            KillTimer(hwnd_, kTopMostTimer);
            slideAnimationActive_ = false;
            hwnd_ = nullptr;
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

LRESULT CALLBACK QuickPanel::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    QuickPanel* self = reinterpret_cast<QuickPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<QuickPanel*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}
