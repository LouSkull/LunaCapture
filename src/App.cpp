#include "App.hpp"
#include "CapturePrivacy.hpp"

#include "CoreUtils.hpp"
#include "UiGfx.hpp"
#include "UiIcons.hpp"
#include "WinUtil.hpp"
#include "resource.h"

#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>

namespace {
constexpr int kWidth = 584;
constexpr int kHeight = 398;
constexpr int kTitlebarHeight = 42;
constexpr int kSidebarWidth = 126;
constexpr UINT kHotkeyModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void Stroke(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &rect, brush);
    DeleteObject(brush);
}

void Text(HDC dc, const std::wstring& value, RECT rect, HFONT font, COLORREF color,
          UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    HGDIOBJ old = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, value.c_str(), -1, &rect, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

UINT CurrentModifiers() {
    UINT result = MOD_NOREPEAT;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) result |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) result |= MOD_SHIFT;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) result |= MOD_ALT;
    if (GetAsyncKeyState(VK_LWIN) & 0x8000 || GetAsyncKeyState(VK_RWIN) & 0x8000) result |= MOD_WIN;
    return result;
}

bool IsModifierKey(UINT key) {
    return key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
           key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
           key == VK_MENU || key == VK_LMENU || key == VK_RMENU ||
           key == VK_LWIN || key == VK_RWIN;
}

UINT NormalizeRawVirtualKey(const RAWKEYBOARD& key) {
    if (key.VKey == VK_SHIFT) return MapVirtualKeyW(key.MakeCode, MAPVK_VSC_TO_VK_EX);
    if (key.VKey == VK_CONTROL) return key.Flags & RI_KEY_E0 ? VK_RCONTROL : VK_LCONTROL;
    if (key.VKey == VK_MENU) return key.Flags & RI_KEY_E0 ? VK_RMENU : VK_LMENU;
    return key.VKey;
}

UINT VirtualKeyForAppCommand(UINT command) {
    switch (command) {
        case APPCOMMAND_BROWSER_BACKWARD: return VK_BROWSER_BACK;
        case APPCOMMAND_BROWSER_FORWARD: return VK_BROWSER_FORWARD;
        case APPCOMMAND_BROWSER_REFRESH: return VK_BROWSER_REFRESH;
        case APPCOMMAND_BROWSER_STOP: return VK_BROWSER_STOP;
        case APPCOMMAND_BROWSER_SEARCH: return VK_BROWSER_SEARCH;
        case APPCOMMAND_BROWSER_FAVORITES: return VK_BROWSER_FAVORITES;
        case APPCOMMAND_BROWSER_HOME: return VK_BROWSER_HOME;
        case APPCOMMAND_VOLUME_MUTE: return VK_VOLUME_MUTE;
        case APPCOMMAND_VOLUME_DOWN: return VK_VOLUME_DOWN;
        case APPCOMMAND_VOLUME_UP: return VK_VOLUME_UP;
        case APPCOMMAND_MEDIA_NEXTTRACK: return VK_MEDIA_NEXT_TRACK;
        case APPCOMMAND_MEDIA_PREVIOUSTRACK: return VK_MEDIA_PREV_TRACK;
        case APPCOMMAND_MEDIA_STOP: return VK_MEDIA_STOP;
        case APPCOMMAND_MEDIA_PLAY_PAUSE: return VK_MEDIA_PLAY_PAUSE;
        case APPCOMMAND_LAUNCH_MAIL: return VK_LAUNCH_MAIL;
        case APPCOMMAND_LAUNCH_MEDIA_SELECT: return VK_LAUNCH_MEDIA_SELECT;
        case APPCOMMAND_LAUNCH_APP1: return VK_LAUNCH_APP1;
        case APPCOMMAND_LAUNCH_APP2: return VK_LAUNCH_APP2;
        default: return 0;
    }
}
}

App::App(HINSTANCE instance) : instance_(instance) {}

App::~App() {
    if (privacyHook_) UnhookWindowsHookEx(privacyHook_);
    privacyApp_ = nullptr;
    if (bindingHook_) UnhookWindowsHookEx(bindingHook_);
    bindingApp_ = nullptr;
    if (rawKeyboardReady_) {
        RAWINPUTDEVICE keyboard{1, 6, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard));
    }
    UnregisterHotkeys();
    if (engine_) engine_->Stop();
    trayMenu_.Destroy();
    regionEditor_.Destroy();
    overlay_.Destroy();
    RemoveTrayIcon();
    for (HFONT font : {fontTitle_, fontHeading_, fontBody_, fontSmall_}) if (font) DeleteObject(font);
}

bool App::Initialize(bool startHidden) {
    settings_ = settingsStore_.Load();
    if (settings_.saveDirectory.empty()) settings_.saveDirectory = GetDefaultScreenshotDirectory();
    EnsureDirectoryExists(settings_.saveDirectory);
    ApplyChronologicalFolderView(settings_.saveDirectory);
    // --background is used only by the Startup-folder shortcut. A manual
    // launch must always open the window, even when the preferred autostart
    // behaviour is "start in tray".
    if (!CreateMainWindow()) return false;
    if (!overlay_.Create(instance_) || !regionEditor_.Create(instance_, hwnd_) || !trayMenu_.Create(instance_, hwnd_)) return false;
    if (!ApplyCapturePrivacy()) { settings_.privateCapture = false; ApplyCapturePrivacy(); }
    engine_ = std::make_unique<CaptureEngine>(hwnd_);
    if (!engine_->Start(settings_)) return false;
    RAWINPUTDEVICE keyboard{1, 6, RIDEV_INPUTSINK, hwnd_};
    rawKeyboardReady_ = RegisterRawInputDevices(&keyboard, 1, sizeof(keyboard)) != FALSE;
    for (UINT key : {VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN}) {
        if (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) rawKeys_.Feed(key, false);
    }
    bindingApp_ = this;
    bindingHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, BindingKeyboardProc, instance_, 0);
    const bool hotkeysReady = RegisterHotkeys();
    AddTrayIcon();
    status_ = hotkeysReady ? L"Ready" : L"Some shortcuts are unavailable";
    ShowWindow(hwnd_, startHidden && settings_.startHiddenToTray ? SW_HIDE : SW_SHOWNORMAL);
    return true;
}

int App::Run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK App::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->hwnd_ = hwnd;
    }
    return app ? app->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

bool App::CreateMainWindow() {
    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass);
    klass.hInstance = instance_;
    klass.lpszClassName = kMainWindowClassName;
    klass.lpfnWndProc = WindowProc;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    klass.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kMainWindowClassName, L"Luna Capture",
                            WS_POPUP | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                            kWidth, kHeight, nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;
    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions, sizeof(disableTransitions));
    fontTitle_ = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fontHeading_ = CreateFontW(-25, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fontBody_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fontSmall_ = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    if (!fontTitle_ || !fontHeading_ || !fontBody_ || !fontSmall_) return false;
    return true;
}

void App::AddHit(RECT rect, Action action) { hits_.push_back({rect, action}); }

void App::DrawNav(HDC dc, const RECT& rect, const std::wstring& text, Page page, UiIcon icon) {
    const auto colors = ui::CurrentThemePalette();
    const bool selected = page_ == page;
    if (selected) Fill(dc, rect, colors.surfaceSelected);
    DrawUiIcon(dc, icon, RECT{rect.left + 9, rect.top + 9, rect.left + 25, rect.bottom - 9},
               selected ? colors.accent : colors.textSecondary, 2);
    Text(dc, text, RECT{rect.left + 33, rect.top, rect.right - 8, rect.bottom}, fontBody_,
         selected ? colors.text : colors.textSecondary);
    AddHit(rect, static_cast<Action>(static_cast<int>(Action::Capture) + static_cast<int>(page)));
}

void App::DrawButton(HDC dc, const RECT& rect, const std::wstring& title, const std::wstring& detail,
                     Action action, UiIcon icon) {
    const auto colors = ui::CurrentThemePalette();
    Fill(dc, rect, colors.surfaceRaised);
    Stroke(dc, rect, colors.border);
    const int iconY = (rect.top + rect.bottom) / 2 - 9;
    DrawUiIcon(dc, icon, RECT{rect.left + 10, iconY, rect.left + 28, iconY + 18}, colors.accent, 2);
    const UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    const int titleBottom = detail.empty() ? rect.bottom : rect.top + 24;
    Text(dc, title, RECT{rect.left + 37, rect.top, rect.right - 7, titleBottom}, fontBody_, colors.text, flags);
    if (!detail.empty())
        Text(dc, detail, RECT{rect.left + 37, titleBottom, rect.right - 7, rect.bottom - 3}, fontSmall_, colors.muted, flags);
    AddHit(rect, action);
}

void App::Paint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);
    RECT client{};
    GetClientRect(hwnd_, &client);
    const auto colors = ui::CurrentThemePalette();
    Fill(dc, client, colors.background);
    Stroke(dc, client, colors.border);
    hits_.clear();

    Fill(dc, RECT{1, 1, client.right - 1, kTitlebarHeight}, colors.topbar);
    Text(dc, L"Luna Capture", RECT{14, 1, 220, kTitlebarHeight}, fontTitle_, colors.text);
    const RECT min{client.right - 75, 5, client.right - 43, 37};
    const RECT close{client.right - 39, 5, client.right - 7, 37};
    DrawUiIcon(dc, UiIcon::Minimize, RECT{min.left + 8, 13, min.right - 8, 29}, colors.textSecondary, 2);
    DrawUiIcon(dc, UiIcon::Close, RECT{close.left + 8, 13, close.right - 8, 29}, colors.textSecondary, 2);
    AddHit(min, Action::Minimize);
    AddHit(close, Action::Close);

    Fill(dc, RECT{1, kTitlebarHeight, kSidebarWidth, client.bottom - 1}, colors.sidebar);
    DrawNav(dc, RECT{8, 54, kSidebarWidth - 8, 88}, L"Capture", Page::Capture, UiIcon::Capture);
    DrawNav(dc, RECT{8, 92, kSidebarWidth - 8, 126}, L"Shortcuts", Page::Binds, UiIcon::Region);
    DrawNav(dc, RECT{8, 130, kSidebarWidth - 8, 164}, L"Photos", Page::Photos, UiIcon::Photo);
    DrawNav(dc, RECT{8, 168, kSidebarWidth - 8, 202}, L"Settings", Page::Settings, UiIcon::Settings);

    const RECT body{kSidebarWidth + 16, 53, client.right - 16, client.bottom - 14};
    const wchar_t* headings[] = {L"Capture", L"Shortcuts", L"Photos", L"Settings"};
    Text(dc, headings[static_cast<int>(page_)], RECT{body.left, body.top, body.right, body.top + 35}, fontHeading_, colors.text);
    const int split = (body.left + body.right) / 2;
    const auto cell = [&](int col, int row, int height) -> RECT {
        const int y = body.top + 49 + row * (height + 7);
        return {col == 0 ? body.left : split + 4, y, col == 0 ? split - 4 : body.right, y + height};
    };
    const UINT textFlags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    const struct { BindingSlot slot; Action capture; Action bind; UiIcon icon; } rows[] = {
        {BindingSlot::ActiveDisplay, Action::CaptureActive, Action::BindActive, UiIcon::Monitor},
        {BindingSlot::AllDisplays, Action::CaptureAll, Action::BindAll, UiIcon::AllDisplays},
        {BindingSlot::Region, Action::CaptureRegion, Action::BindRegion, UiIcon::Region},
        {BindingSlot::ActiveWindow, Action::CaptureWindow, Action::BindWindow, UiIcon::Window},
    };

    if (page_ == Page::Capture) {
        for (int i = 0; i < static_cast<int>(std::size(rows)); ++i) {
            const auto& row = rows[i];
            const auto* shortcut = Binding(row.slot);
            DrawButton(dc, cell(i % 2, i / 2, 53), BindingName(row.slot),
                       FormatHotkeyCompact(shortcut->modifiers, shortcut->vk), row.capture, row.icon);
        }
        Text(dc, status_, RECT{body.left, 234, body.right, 257}, fontBody_, colors.text, textFlags);
        Text(dc, detail_, RECT{body.left, 257, body.right, 282}, fontSmall_, colors.muted, textFlags);
    } else if (page_ == Page::Binds) {
        const RECT reset{body.right - 112, body.top + 4, body.right, body.top + 31};
        Fill(dc, reset, colors.surfaceRaised);
        Stroke(dc, reset, colors.border);
        Text(dc, L"Reset defaults", reset, fontSmall_, colors.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddHit(reset, Action::ResetBinds);
        int y = body.top + 49;
        for (const auto& row : rows) {
            const bool listening = bindingSlot_ == row.slot;
            const auto* shortcut = Binding(row.slot);
            const RECT rect{body.left, y, body.right, y + 41};
            Fill(dc, rect, listening ? colors.surfaceSelected : colors.surfaceRaised);
            Stroke(dc, rect, listening ? colors.accent : colors.border);
            DrawUiIcon(dc, row.icon, RECT{rect.left + 11, y + 12, rect.left + 28, y + 29}, colors.accent, 2);
            Text(dc, BindingName(row.slot), RECT{rect.left + 37, y, rect.left + 183, rect.bottom}, fontBody_, colors.text, textFlags);
            Text(dc, listening ? L"Press shortcut…" : FormatHotkeyCompact(shortcut->modifiers, shortcut->vk),
                 RECT{rect.left + 185, y, rect.right - 12, rect.bottom}, fontBody_, listening ? colors.accent : colors.textSecondary,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            AddHit(rect, listening ? Action::CancelBind : row.bind);
            y += 47;
        }
        Text(dc, bindingSlot_ == BindingSlot::None ? status_ : L"Esc clears the shortcut. Click the row again to cancel.",
             RECT{body.left, y + 2, body.right, y + 27}, fontSmall_, colors.muted, textFlags);
        if (bindingSlot_ == BindingSlot::None)
            Text(dc, detail_, RECT{body.left, y + 27, body.right, y + 50}, fontSmall_, colors.muted, textFlags);
    } else if (page_ == Page::Photos) {
        DrawButton(dc, RECT{body.left, 102, body.right, 150}, L"Open screenshot folder", L"", Action::OpenFolder, UiIcon::Folder);
        Text(dc, settings_.saveDirectory, RECT{body.left, 163, body.right, 189}, fontSmall_, colors.muted, textFlags);
        if (!lastCapture_.empty())
            Text(dc, L"Last saved  ·  " + FileNameOnly(lastCapture_), RECT{body.left, 205, body.right, 231}, fontSmall_, colors.text, textFlags);
    } else {
        DrawButton(dc, cell(0, 0, 43), L"Output folder", L"", Action::ChooseFolder, UiIcon::Folder);
        DrawButton(dc, cell(1, 0, 43), settings_.imageFormat == ImageFormat::Png ? L"Format: PNG" : L"Format: JPEG", L"", Action::ToggleFormat, UiIcon::Save);
        DrawButton(dc, cell(0, 1, 43), settings_.notifications ? L"Notifications on" : L"Notifications off", L"", Action::ToggleNotifications, UiIcon::Bell);
        DrawButton(dc, cell(1, 1, 43), settings_.sound ? L"Capture sound on" : L"Capture sound off", L"", Action::ToggleSound, UiIcon::Sound);
        DrawButton(dc, cell(0, 2, 43), settings_.startWithWindows ? L"Autostart on" : L"Autostart off", L"", Action::ToggleAutostart, UiIcon::Power);
        DrawButton(dc, cell(1, 2, 43), settings_.startHiddenToTray ? L"Start in tray" : L"Show app at startup", L"", Action::ToggleStartHidden, UiIcon::Home);
        DrawButton(dc, cell(0, 3, 43), settings_.showRegionGuide ? L"Region guide on" : L"Region guide off", L"", Action::ToggleRegionGuide, UiIcon::Region);
        DrawButton(dc, cell(1, 3, 43), settings_.privateCapture ? L"Hidden capture on" : L"Hidden capture off", L"", Action::TogglePrivateCapture, UiIcon::Check);
        DrawButton(dc, cell(0, 4, 43), L"Check for updates", updateStatus_, Action::CheckUpdates, UiIcon::Settings);
        Text(dc, std::wstring(L"Version ") + kAppVersion, cell(1, 4, 43), fontSmall_, colors.muted, textFlags);
        Text(dc, status_, RECT{body.left, 353, body.right, body.bottom}, fontSmall_, colors.muted, textFlags);
    }
    EndPaint(hwnd_, &paint);
}

void App::Execute(Action action) {
    switch (action) {
        case Action::Capture: ShowMain(Page::Capture); break;
        case Action::Binds: ShowMain(Page::Binds); break;
        case Action::Photos: ShowMain(Page::Photos); break;
        case Action::Settings: ShowMain(Page::Settings); break;
        case Action::CaptureActive: TriggerCapture(CaptureMode::ActiveDisplay); break;
        case Action::CaptureAll: TriggerCapture(CaptureMode::AllDisplays); break;
        case Action::CaptureRegion: TriggerCapture(CaptureMode::Region); break;
        case Action::CaptureWindow: TriggerCapture(CaptureMode::ActiveWindow); break;
        case Action::OpenFolder: OpenFolderInExplorer(settings_.saveDirectory); break;
        case Action::ChooseFolder: { std::wstring picked; if (ChooseFolder(hwnd_, settings_.saveDirectory, picked)) { settings_.saveDirectory = picked; SaveSettings(); } break; }
        case Action::ToggleFormat: settings_.imageFormat = settings_.imageFormat == ImageFormat::Png ? ImageFormat::Jpeg : ImageFormat::Png; SaveSettings(); break;
        case Action::ToggleSound: ToggleCaptureSound(); break;
        case Action::ToggleNotifications: settings_.notifications = !settings_.notifications; SaveSettings(); break;
        case Action::ToggleAutostart: {
            const bool enable = !settings_.startWithWindows;
            if (settingsStore_.ApplyAutostart(enable)) {
                settings_.startWithWindows = enable;
                if (enable) settings_.startHiddenToTray = true;
                SaveSettings();
                status_ = enable ? L"Autostart enabled" : L"Autostart disabled";
                detail_ = enable ? L"Luna Capture will start quietly in the tray after sign-in." : L"The Windows Startup shortcut has been removed.";
            } else {
                status_ = L"Autostart could not be changed";
                detail_ = L"Windows did not allow the Startup shortcut to be updated.";
                overlay_.ShowError(detail_, settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
            }
            break;
        }
        case Action::ToggleStartHidden:
            if (!settings_.startWithWindows) {
                status_ = L"Enable Autostart first";
                detail_ = L"This option controls only an automatic Windows launch.";
            } else {
                settings_.startHiddenToTray = !settings_.startHiddenToTray;
                SaveSettings();
            }
            break;
        case Action::ToggleRegionGuide: settings_.showRegionGuide = !settings_.showRegionGuide; SaveSettings(); break;
        case Action::TogglePrivateCapture:
            settings_.privateCapture = !settings_.privateCapture;
            if (!ApplyCapturePrivacy()) {
                const std::wstring error = privacyError_;
                settings_.privateCapture = false;
                ApplyCapturePrivacy();
                status_ = L"Hidden capture could not be enabled";
                detail_ = error;
                overlay_.ShowWarning(L"Hidden capture unavailable", error, 3000, GetPriorityMonitor(hwnd_));
            }
            SaveSettings();
            break;
        case Action::CheckUpdates: if (!updateChecking_) { updateChecking_ = true; updateStatus_ = L"Checking…"; StartUpdateCheck(hwnd_, WM_APP_UPDATE_CHECK, true); } break;
        case Action::BindActive: BeginBinding(BindingSlot::ActiveDisplay); break;
        case Action::BindAll: BeginBinding(BindingSlot::AllDisplays); break;
        case Action::BindRegion: BeginBinding(BindingSlot::Region); break;
        case Action::BindWindow: BeginBinding(BindingSlot::ActiveWindow); break;
        case Action::CancelBind: CancelBinding(); break;
        case Action::ResetBinds: ResetBindings(); break;
        case Action::Minimize: ShowWindow(hwnd_, SW_MINIMIZE); break;
        case Action::Close: CancelBinding(); ShowWindow(hwnd_, SW_HIDE); break;
        default: break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::TriggerCapture(CaptureMode mode) {
    if (!engine_ || capturePending_ || regionEditor_.IsOpen() || bindingSlot_ != BindingSlot::None) return;
    if (IsWindowVisible(overlay_.Handle())) {
        overlay_.Dismiss();
        DwmFlush();
    }
    const HMONITOR monitor = mode == CaptureMode::ActiveDisplay ? GetPriorityMonitor(hwnd_) : nullptr;
    const HWND target = mode == CaptureMode::ActiveWindow ? GetPriorityCaptureWindow(hwnd_) : nullptr;
    if (!engine_->RequestCapture(mode, monitor, target, L"Screenshot", settings_)) {
        status_ = L"Capture unavailable";
        detail_ = L"Wait for the current request to finish.";
        overlay_.ShowError(detail_, settings_.overlayDurationMs, monitor ? monitor : GetPriorityMonitor(hwnd_));
    } else {
        capturePending_ = true;
        status_ = L"Capturing…";
        detail_ = L"Preparing a still screenshot.";
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::HandleEngineEvent(std::unique_ptr<EngineEvent> event) {
    if (!event) return;
    if (event->type == EngineEventType::Error) {
        capturePending_ = false;
        status_ = L"Capture failed";
        detail_ = event->detail;
        overlay_.ShowError(event->detail, settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
    } else if (event->type == EngineEventType::ClipboardImageReady) {
        const bool copied = PublishClipboardImage(hwnd_, event->clipboardImage);
        status_ = copied ? L"Screenshot copied" : L"Clipboard unavailable";
        detail_.clear();
        if (!copied) overlay_.ShowWarning(status_, L"Try copying the screenshot again.", settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
        if (event->path.empty()) {
            capturePending_ = false;
            if (copied && settings_.notifications)
                overlay_.ShowMessage(status_, L"Ready to paste", settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
            if (copied) PlayCaptureSound();
        }
    } else if (event->type == EngineEventType::Ready || event->type == EngineEventType::Reconfiguring) {
        status_ = event->type == EngineEventType::Ready ? L"Ready when you are." : L"Reconnecting";
        detail_ = event->detail;
    } else if (event->type == EngineEventType::RegionFrame && event->frame) {
        capturePending_ = false;
        if (!regionEditor_.Show(event->frame, event->mode == CaptureMode::ActiveWindow ? RegionEditor::SessionMode::WindowPicker : RegionEditor::SessionMode::Region, settings_, event->filenamePrefix)) {
            status_ = L"Could not open region editor";
            overlay_.ShowError(status_, settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
        }
    } else if (event->type == EngineEventType::Saved) {
        capturePending_ = false;
        lastCapture_ = event->path;
        status_ = L"Screenshot saved";
        detail_ = FileNameOnly(event->path);
        PlayCaptureSound();
        if (settings_.notifications) overlay_.ShowSuccess(FileNameOnly(event->path), settings_.overlayDurationMs, event->monitor);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::HandleRegionResult(std::unique_ptr<RegionResult> result) {
    if (!result || result->action == RegionResultAction::Cancel || result->frame.bgra.empty() || !engine_) return;
    auto outputSettings = settings_;
    outputSettings.imageFormat = result->saveFormat;
    capturePending_ = true;
    engine_->FinishEditedFrame(std::move(result->frame), outputSettings, result->action,
                              result->savePath, result->filenamePrefix);
}

void App::HandleUpdateResult(std::unique_ptr<UpdateCheckResult> result) {
    updateChecking_ = false;
    if (!result || result->outcome == UpdateCheckOutcome::Failed) updateStatus_ = L"Check failed";
    else if (result->outcome == UpdateCheckOutcome::UpdateAvailable) updateStatus_ = L"New version: " + result->latestVersion;
    else updateStatus_ = L"You are up to date";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

HotkeyBinding* App::Binding(BindingSlot slot) {
    switch (slot) {
        case BindingSlot::ActiveDisplay: return &settings_.activeDisplayHotkey;
        case BindingSlot::AllDisplays: return &settings_.allDisplaysHotkey;
        case BindingSlot::Region: return &settings_.regionHotkey;
        case BindingSlot::ActiveWindow: return &settings_.activeWindowHotkey;
        default: return nullptr;
    }
}

const HotkeyBinding* App::Binding(BindingSlot slot) const { return const_cast<App*>(this)->Binding(slot); }

const wchar_t* App::BindingName(BindingSlot slot) {
    switch (slot) {
        case BindingSlot::ActiveDisplay: return L"Active display";
        case BindingSlot::AllDisplays: return L"All displays";
        case BindingSlot::Region: return L"Region capture";
        case BindingSlot::ActiveWindow: return L"Active window";
        default: return L"Bind";
    }
}

void App::BeginBinding(BindingSlot slot) {
    CancelBinding();
    UnregisterHotkeys();
    bindingSlot_ = slot;
    ++bindingGeneration_;
    SetFocus(hwnd_);
}

void App::CancelBinding() {
    if (bindingSlot_ == BindingSlot::None) return;
    bindingSlot_ = BindingSlot::None;
    bindingPosted_ = false;
    RegisterHotkeys();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::QueueRecordedBinding(UINT key, UINT modifiers) {
    if (bindingSlot_ == BindingSlot::None || bindingPosted_ || !key || key >= 255 ||
        key == VK_PACKET || IsModifierKey(key)) return;
    const LPARAM shortcut = MAKELPARAM(key, modifiers & kHotkeyModifiers);
    bindingPosted_ = PostMessageW(hwnd_, WM_APP_HOTKEY_CAPTURED, bindingGeneration_, shortcut) != FALSE;
}

App::BindingSlot App::MatchBinding(UINT key, UINT modifiers) const {
    if (!key) return BindingSlot::None;
    for (BindingSlot slot : {BindingSlot::ActiveDisplay, BindingSlot::AllDisplays, BindingSlot::Region, BindingSlot::ActiveWindow}) {
        const auto* binding = Binding(slot);
        if (binding->vk == key && ((binding->modifiers ^ modifiers) & kHotkeyModifiers) == 0) return slot;
    }
    return BindingSlot::None;
}

LRESULT CALLBACK App::BindingKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    auto* app = bindingApp_;
    if (code != HC_ACTION || !app) return CallNextHookEx(nullptr, code, wParam, lParam);
    const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    const UINT key = event.vkCode;
    const bool up = (event.flags & LLKHF_UP) != 0;
    if (key == VK_LWIN || key == VK_RWIN) {
        const UINT bit = key == VK_LWIN ? 1u : 2u;
        if (app->suppressedWinKeys_ & bit) {
            if (up) app->suppressedWinKeys_ &= ~bit;
            return 1;
        }
        if (!up && app->bindingSlot_ != BindingSlot::None && GetForegroundWindow() == app->hwnd_ &&
            !(GetAsyncKeyState(static_cast<int>(key)) & 0x8000)) {
            app->suppressedWinKeys_ |= bit;
            return 1;
        }
    }
    if (IsModifierKey(key) || !app->hookKeys_.Feed(key, up))
        return CallNextHookEx(nullptr, code, wParam, lParam);

    if (app->bindingSlot_ != BindingSlot::None) {
        if (GetForegroundWindow() == app->hwnd_) {
            const UINT modifiers = CurrentModifiers() | ((event.flags & LLKHF_ALTDOWN) ? MOD_ALT : 0) |
                                   (app->suppressedWinKeys_ ? MOD_WIN : 0);
            app->QueueRecordedBinding(key, modifiers);
            if (app->bindingPosted_) return 1;
        }
    } else {
        for (BindingSlot slot : {BindingSlot::ActiveDisplay, BindingSlot::AllDisplays, BindingSlot::Region, BindingSlot::ActiveWindow}) {
            const auto* binding = app->Binding(slot);
            if (binding->vk != key) continue;
            const UINT modifiers = CurrentModifiers() | ((event.flags & LLKHF_ALTDOWN) ? MOD_ALT : 0);
            if (((binding->modifiers ^ modifiers) & kHotkeyModifiers) == 0) {
                PostMessageW(app->hwnd_, WM_APP_GLOBAL_HOTKEY_INPUT, key, modifiers);
                break;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void App::HandleRawKeyboard(HRAWINPUT handle) {
    RAWINPUT input{};
    UINT size = sizeof(input);
    if (GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) ||
        input.header.dwType != RIM_TYPEKEYBOARD) return;
    const auto& keyboard = input.data.keyboard;
    const UINT key = NormalizeRawVirtualKey(keyboard);
    const bool up = (keyboard.Flags & RI_KEY_BREAK) != 0;
    if (!rawKeys_.Feed(key, up) || IsModifierKey(key)) return;

    UINT modifiers = 0;
    if (rawKeys_.Down(VK_LCONTROL) || rawKeys_.Down(VK_RCONTROL)) modifiers |= MOD_CONTROL;
    if (rawKeys_.Down(VK_LMENU) || rawKeys_.Down(VK_RMENU)) modifiers |= MOD_ALT;
    if (rawKeys_.Down(VK_LSHIFT) || rawKeys_.Down(VK_RSHIFT)) modifiers |= MOD_SHIFT;
    if (rawKeys_.Down(VK_LWIN) || rawKeys_.Down(VK_RWIN)) modifiers |= MOD_WIN;
    if (bindingSlot_ != BindingSlot::None) {
        if (GetForegroundWindow() == hwnd_) QueueRecordedBinding(key, modifiers);
    } else {
        ExecuteBinding(MatchBinding(key, modifiers), HotkeySource::RawInput);
    }
}

void App::ExecuteBinding(BindingSlot slot, HotkeySource source) {
    if (slot == BindingSlot::None || bindingSlot_ != BindingSlot::None) return;
    const size_t index = static_cast<size_t>(slot) - 1;
    const ULONGLONG now = GetTickCount64();
    if (Binding(slot)->vk == recordedKey_ && now - recordedAt_ < 150) return;
    // A physical press can arrive via all three routes. Keep rapid presses on the same route.
    if (lastHotkeyTime_[index] && lastHotkeySource_[index] != source && now - lastHotkeyTime_[index] < 150) return;
    lastHotkeyTime_[index] = now;
    lastHotkeySource_[index] = source;
    const CaptureMode modes[] = {CaptureMode::ActiveDisplay, CaptureMode::AllDisplays, CaptureMode::Region, CaptureMode::ActiveWindow};
    TriggerCapture(modes[index]);
}

bool App::ApplyCapturePrivacy() {
    privacyError_.clear();
    if (settings_.privateCapture && !capturePrivacy::Supported()) {
        privacyError_ = L"Windows 10 2004 or newer with desktop composition is required.";
        AppendDiagnosticLog(privacyError_);
        return false;
    }
    const bool enabled = settings_.privateCapture;
    if (enabled && !privacyHook_) {
        privacyApp_ = this;
        privacyHook_ = SetWindowsHookExW(WH_CALLWNDPROC, PrivacyWindowProc, nullptr, GetCurrentThreadId());
    } else if (!enabled && privacyHook_) {
        UnhookWindowsHookEx(privacyHook_);
        privacyHook_ = nullptr;
        privacyApp_ = nullptr;
    }
    bool ok = true;
    std::wstring error;
    const auto record = [&](bool applied) {
        if (!applied) { ok = false; privacyError_ = error; AppendDiagnosticLog(error); }
    };
    record(capturePrivacy::Apply(hwnd_, enabled, L"Main window", error));
    record(overlay_.SetCapturePrivacy(enabled, error));
    record(trayMenu_.SetCapturePrivacy(enabled, error));
    record(capturePrivacy::Apply(regionEditor_.WindowHandle(), enabled, L"Region editor", error));
    DwmFlush();
    return ok;
}

LRESULT CALLBACK App::PrivacyWindowProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && privacyApp_ && privacyApp_->settings_.privateCapture) {
        const auto& message = *reinterpret_cast<CWPSTRUCT*>(lParam);
        const auto* app = privacyApp_;
        // These renderers explicitly set affinity after their style changes.
        // The hook is only for additional native dialogs owned by this thread.
        if (message.hwnd == app->hwnd_ || message.hwnd == app->overlay_.Handle() ||
            message.hwnd == app->trayMenu_.Handle() || message.hwnd == app->regionEditor_.WindowHandle())
            return CallNextHookEx(nullptr, code, wParam, lParam);
        if (message.message == WM_WINDOWPOSCHANGING && GetAncestor(message.hwnd, GA_ROOT) == message.hwnd) {
            const auto* position = reinterpret_cast<WINDOWPOS*>(message.lParam);
            DWORD affinity = WDA_NONE;
            if ((position->flags & SWP_SHOWWINDOW) && GetWindowDisplayAffinity(message.hwnd, &affinity) && affinity != WDA_EXCLUDEFROMCAPTURE) {
                std::wstring error;
                wchar_t name[128]{};
                GetClassNameW(message.hwnd, name, 128);
                if (!capturePrivacy::Apply(message.hwnd, true, name, error)) AppendDiagnosticLog(error);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void App::CompleteBinding(UINT key, UINT modifiers) {
    const BindingSlot slot = bindingSlot_;
    auto* binding = Binding(slot);
    if (!binding) return;
    bindingPosted_ = false;
    recordedKey_ = key;
    recordedAt_ = GetTickCount64();
    const bool clear = key == VK_ESCAPE;
    const BindingSlot duplicate = clear ? BindingSlot::None : MatchBinding(key, modifiers);
    if (duplicate != BindingSlot::None && duplicate != slot) {
        CancelBinding();
        status_ = L"Shortcut already assigned";
        detail_ = BindingName(duplicate);
        return;
    }

    const HotkeyBinding previous = *binding;
    bindingSlot_ = BindingSlot::None;
    *binding = clear ? HotkeyBinding{0, MOD_NOREPEAT} : HotkeyBinding{key, (modifiers & kHotkeyModifiers) | MOD_NOREPEAT};
    if (RegisterHotkeys(slot)) {
        const bool saved = SaveSettings();
        status_ = !saved ? L"Could not save shortcuts" : clear ? L"Shortcut cleared" : L"Shortcut updated";
        detail_ = std::wstring(BindingName(slot)) + L"  ·  " + FormatHotkey(binding->modifiers, binding->vk);
    } else {
        UnregisterHotkeys();
        *binding = previous;
        RegisterHotkeys();
        status_ = L"Shortcut unavailable";
        detail_ = L"Choose another key combination.";
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ResetBindings() {
    bindingSlot_ = BindingSlot::None;
    bindingPosted_ = false;
    UnregisterHotkeys();
    const AppSettings defaults;
    settings_.activeDisplayHotkey = defaults.activeDisplayHotkey;
    settings_.allDisplaysHotkey = defaults.allDisplaysHotkey;
    settings_.regionHotkey = defaults.regionHotkey;
    settings_.activeWindowHotkey = defaults.activeWindowHotkey;
    const bool registered = RegisterHotkeys();
    const bool saved = SaveSettings();
    status_ = !saved ? L"Could not save shortcuts" : registered ? L"Default shortcuts restored" : L"Some shortcuts are unavailable";
    detail_.clear();
}

void App::ToggleCaptureSound() {
    settings_.sound = !settings_.sound;
    if (!settings_.sound) soundPlayer_.Shutdown();
    SaveSettings();
}

void App::PlayCaptureSound() {
    if (!settings_.sound || settings_.privateCapture) return;
    if (soundPlayer_.IsReady() || soundPlayer_.Initialize(instance_, IDR_CAPTURE_SOUND)) soundPlayer_.Play();
}

bool App::SaveSettings() {
    const bool saved = settingsStore_.Save(settings_);
    if (engine_) engine_->UpdateSettings(settings_);
    return saved;
}

void App::ShowMain(Page page) {
    CancelBinding();
    page_ = page;
    if (!IsWindowVisible(hwnd_) || IsIconic(hwnd_)) ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool App::RegisterHotkeys(BindingSlot required) {
    const struct { UINT id; BindingSlot slot; } slots[] = {
        {LUNASHOT_HOTKEY_ACTIVE, BindingSlot::ActiveDisplay},
        {LUNASHOT_HOTKEY_ALL, BindingSlot::AllDisplays},
        {LUNASHOT_HOTKEY_REGION, BindingSlot::Region},
        {LUNASHOT_HOTKEY_WINDOW, BindingSlot::ActiveWindow},
    };
    bool ok = true;
    for (const auto& item : slots) {
        const auto& binding = *Binding(item.slot);
        if (!binding.vk || RegisterHotKey(hwnd_, item.id, (binding.modifiers & kHotkeyModifiers) | MOD_NOREPEAT, binding.vk))
            continue;
        const bool keyboardFallback = binding.vk > VK_XBUTTON2 && (rawKeyboardReady_ || bindingHook_);
        if (!keyboardFallback && (required == BindingSlot::None || required == item.slot)) ok = false;
    }
    return ok;
}

void App::UnregisterHotkeys() {
    for (UINT id : {LUNASHOT_HOTKEY_ACTIVE, LUNASHOT_HOTKEY_ALL, LUNASHOT_HOTKEY_REGION, LUNASHOT_HOTKEY_WINDOW}) UnregisterHotKey(hwnd_, id);
}

void App::AddTrayIcon() {
    trayIcon_.cbSize = sizeof(trayIcon_);
    trayIcon_.hWnd = hwnd_;
    trayIcon_.uID = 1;
    trayIcon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    trayIcon_.uCallbackMessage = WM_APP_TRAY;
    trayIcon_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    lstrcpynW(trayIcon_.szTip, L"Luna Capture", ARRAYSIZE(trayIcon_.szTip));
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &trayIcon_) != FALSE;
}

void App::RemoveTrayIcon() { if (trayAdded_) { Shell_NotifyIconW(NIM_DELETE, &trayIcon_); trayAdded_ = false; } }

void App::ShowTrayMenu() { POINT point{}; GetCursorPos(&point); trayMenu_.Show(point, settings_); }

LRESULT App::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: Paint(); return 0;
        case WM_NCHITTEST: return HTCLIENT;
        case WM_LBUTTONDOWN: {
            if (bindingSlot_ != BindingSlot::None) return 0;
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            for (const auto& hit : hits_) if (PtInRect(&hit.rect, point)) return 0;
            POINT cursor{};
            GetCursorPos(&cursor);
            windowDrag_.Begin(hwnd_, cursor);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!windowDrag_.Active()) break;
            if (!(wParam & MK_LBUTTON)) { windowDrag_.End(); return 0; }
            POINT cursor{};
            GetCursorPos(&cursor);
            // Move the actual window on each event; never enter Windows' system
            // move loop, which obeys SPI_GETDRAGFULLWINDOWS and may draw a frame.
            windowDrag_.Move(cursor);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (windowDrag_.Active()) { windowDrag_.End(); return 0; }
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) if (PtInRect(&it->rect, point)) { Execute(it->action); break; }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (windowDrag_.Active() && wParam == VK_ESCAPE) { windowDrag_.End(true); return 0; }
            if (bindingSlot_ != BindingSlot::None) {
                if ((lParam & (1LL << 30)) == 0) QueueRecordedBinding(static_cast<UINT>(wParam), CurrentModifiers());
                return 0;
            }
            break;
        case WM_XBUTTONDOWN:
            if (bindingSlot_ != BindingSlot::None) {
                QueueRecordedBinding(GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2, CurrentModifiers());
                return TRUE;
            }
            break;
        case WM_APPCOMMAND: {
            const UINT key = VirtualKeyForAppCommand(GET_APPCOMMAND_LPARAM(lParam));
            if (!key) break;
            if (bindingSlot_ != BindingSlot::None) {
                QueueRecordedBinding(key, CurrentModifiers());
                return TRUE;
            }
            const BindingSlot slot = MatchBinding(key, CurrentModifiers());
            if (slot == BindingSlot::None) break;
            ExecuteBinding(slot, HotkeySource::AppCommand);
            return TRUE;
        }
        case WM_INPUT:
            HandleRawKeyboard(reinterpret_cast<HRAWINPUT>(lParam));
            if (GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT) return DefWindowProcW(hwnd_, message, wParam, lParam);
            return 0;
        case WM_HOTKEY:
            ExecuteBinding(MatchBinding(HIWORD(lParam), LOWORD(lParam)), HotkeySource::Registered);
            return 0;
        case WM_APP_GLOBAL_HOTKEY_INPUT:
            ExecuteBinding(MatchBinding(static_cast<UINT>(wParam), static_cast<UINT>(lParam)), HotkeySource::Hook);
            return 0;
        case WM_APP_HOTKEY_CAPTURED:
            if (bindingSlot_ != BindingSlot::None && wParam == bindingGeneration_)
                CompleteBinding(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) { windowDrag_.End(); CancelBinding(); }
            break;
        case WM_CANCELMODE: windowDrag_.End(); return 0;
        case WM_CAPTURECHANGED: windowDrag_.CaptureLost(); return 0;
        case WM_APP_ENGINE_EVENT: HandleEngineEvent(std::unique_ptr<EngineEvent>(reinterpret_cast<EngineEvent*>(lParam))); return 0;
        case WM_APP_REGION_COMPLETE: HandleRegionResult(std::unique_ptr<RegionResult>(reinterpret_cast<RegionResult*>(lParam))); return 0;
        case WM_APP_UPDATE_CHECK: HandleUpdateResult(std::unique_ptr<UpdateCheckResult>(reinterpret_cast<UpdateCheckResult*>(lParam))); return 0;
        case WM_APP_TRAY_COMMAND:
            if (wParam == static_cast<WPARAM>(TrayCommand::Open)) ShowMain();
            else if (wParam == static_cast<WPARAM>(TrayCommand::CaptureActive)) TriggerCapture(CaptureMode::ActiveDisplay);
            else if (wParam == static_cast<WPARAM>(TrayCommand::CaptureAll)) TriggerCapture(CaptureMode::AllDisplays);
            else if (wParam == static_cast<WPARAM>(TrayCommand::CaptureRegion)) TriggerCapture(CaptureMode::Region);
            else if (wParam == static_cast<WPARAM>(TrayCommand::CaptureWindow)) TriggerCapture(CaptureMode::ActiveWindow);
            else if (wParam == static_cast<WPARAM>(TrayCommand::OpenFolder)) OpenFolderInExplorer(settings_.saveDirectory);
            else if (wParam == static_cast<WPARAM>(TrayCommand::ToggleSound)) ToggleCaptureSound();
            else if (wParam == static_cast<WPARAM>(TrayCommand::ToggleNotifications)) { settings_.notifications = !settings_.notifications; SaveSettings(); }
            else if (wParam == static_cast<WPARAM>(TrayCommand::Exit)) DestroyWindow(hwnd_);
            return 0;
        case WM_APP_TRAY:
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) ShowTrayMenu();
            else if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) ShowMain();
            return 0;
        case WM_CLOSE: CancelBinding(); ShowWindow(hwnd_, SW_HIDE); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}
