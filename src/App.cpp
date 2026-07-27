#include "App.hpp"

#include "CoreUtils.hpp"
#include "UiGfx.hpp"
#include "UiIcons.hpp"
#include "WinUtil.hpp"
#include "resource.h"

#include <windowsx.h>
#include <shellapi.h>
#include <hidusage.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <cwchar>
#include <memory>
#include <new>
#include <sstream>
#include <utility>

namespace {
constexpr wchar_t kMainClassName[] = L"LunaCaptureMainWindow";
constexpr int kWindowWidth = 1000;
constexpr int kWindowHeight = 650;
constexpr int kWindowCornerRadius = 22;
constexpr int kSidebarWidth = 196;
constexpr int kTopBarHeight = 64;
constexpr UINT_PTR kAnimationTimer = 1;

COLORREF Mix(COLORREF a, COLORREF b, float t) {
    t = (std::max)(0.0f, (std::min)(1.0f, t));
    const auto mix = [t](BYTE x, BYTE y) { return static_cast<BYTE>(x + (y - x) * t); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void FillRound(HDC dc, const RECT& rect, int radius, COLORREF color) {
    ui::FillRoundRect(dc, rect, radius, color);
}

void StrokeRound(HDC dc, const RECT& rect, int radius, COLORREF color, int width = 1) {
    ui::StrokeRoundRect(dc, rect, radius, color, width);
}

HFONT MakeFont(int pixels, int weight = FW_NORMAL) {
    return CreateFontW(-pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void Text(HDC dc, const std::wstring& value, RECT rect, COLORREF color, HFONT font,
          UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, value.c_str(), -1, &rect, flags | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

void DrawToggle(HDC dc, int x, int y, bool on, float hover = 0.0f) {
    RECT track{x, y, x + 46, y + 24};
    const COLORREF onColor = Mix(RGB(70, 122, 226), RGB(96, 152, 250), hover);
    const COLORREF offColor = Mix(RGB(48, 53, 64), RGB(60, 66, 80), hover);
    ui::FillRoundRect(dc, track, 12, on ? onColor : offColor);
    const int knobX = on ? x + 24 : x + 2;
    RECT knob{knobX, y + 2, knobX + 20, y + 22};
    ui::FillEllipse(dc, RECT{knob.left, knob.top, knob.left + 20, knob.top + 20}, RGB(248, 249, 252));
}

std::wstring PathFileName(const std::wstring& path) {
    if (path.empty()) return L"No screenshots yet";
    return std::filesystem::path(path).filename().wstring();
}

std::wstring ResolutionLabel(const AppSettings& settings) {
    return FormatResolution(settings.outputWidth, settings.outputHeight);
}

std::pair<int, int> NextResolution(int width, int height) {
    const std::pair<int, int> values[] = {{0, 0}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {1280, 720}};
    for (std::size_t i = 0; i < std::size(values); ++i) {
        if (values[i].first == width && values[i].second == height) return values[(i + 1) % std::size(values)];
    }
    return values[0];
}

UINT CurrentModifiers() {
    UINT modifiers = MOD_NOREPEAT;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) modifiers |= MOD_WIN;
    return modifiers;
}

UINT NormalizeRawVirtualKey(const RAWKEYBOARD& keyboard) {
    UINT vk = keyboard.VKey;
    if (vk == 0 || vk == 255) return 0;
    if (vk == VK_SHIFT) {
        vk = MapVirtualKeyW(keyboard.MakeCode, MAPVK_VSC_TO_VK_EX);
    } else if (vk == VK_CONTROL) {
        vk = (keyboard.Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
    } else if (vk == VK_MENU) {
        vk = (keyboard.Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;
    }
    return vk;
}

UINT AppCommandToVirtualKey(int command) {
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

bool IsModifierKey(UINT vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_SHIFT ||
           vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_MENU || vk == VK_LMENU ||
           vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
}
}

App* App::hotkeyRecordingInstance_ = nullptr;

App::App(HINSTANCE instance) : instance_(instance), settings_(settingsStore_.Load()) {}
App::~App() { ExitApplication(); }

bool App::Initialize(bool startHidden) {
    if (!CreateMainWindow(startHidden)) return false;
    if (!overlay_.Create(instance_)) return false;
    if (!regionEditor_.Create(instance_, hwnd_)) return false;
    if (!trayMenu_.Create(instance_, hwnd_)) return false;
    soundPlayer_.Initialize(instance_, IDR_CAPTURE_SOUND);
    AddTrayIcon();
    engine_ = std::make_unique<CaptureEngine>(hwnd_);
    engine_->Start(settings_);
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

bool App::CreateMainWindow(bool startHidden) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &App::WindowProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = kMainClassName;
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - kWindowWidth) / 2;
    const int y = work.top + ((work.bottom - work.top) - kWindowHeight) / 2;
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_LAYERED, kMainClassName, L"Luna Capture", WS_POPUP | WS_MINIMIZEBOX,
                            x, y, kWindowWidth, kWindowHeight, nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;
    if (!CreateUiResources()) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    SetWindowDisplayAffinity(hwnd_, WDA_NONE);

    RAWINPUTDEVICE keyboardDevice{};
    keyboardDevice.usUsagePage = HID_USAGE_PAGE_GENERIC;
    keyboardDevice.usUsage = HID_USAGE_GENERIC_KEYBOARD;
    keyboardDevice.dwFlags = RIDEV_INPUTSINK;
    keyboardDevice.hwndTarget = hwnd_;
    RegisterRawInputDevices(&keyboardDevice, 1, sizeof(keyboardDevice));

    SetTimer(hwnd_, kAnimationTimer, 16, nullptr);
    if (!startHidden) {
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
    }
    return true;
}

bool App::CreateUiResources() {
    if (!hwnd_) return false;
    HDC windowDc = GetDC(hwnd_);
    if (!windowDc) return false;

    backBufferDc_ = CreateCompatibleDC(windowDc);
    backBufferBitmap_ = ui::CreateLayeredDib(kWindowWidth, kWindowHeight, &backBufferPixels_);
    ReleaseDC(hwnd_, windowDc);
    if (!backBufferDc_ || !backBufferBitmap_) {
        DestroyUiResources();
        return false;
    }
    backBufferOldBitmap_ = SelectObject(backBufferDc_, backBufferBitmap_);

    fontLogo_ = MakeFont(19, FW_SEMIBOLD);
    fontHeadline_ = MakeFont(25, FW_SEMIBOLD);
    fontTitle_ = MakeFont(16, FW_SEMIBOLD);
    fontNormal_ = MakeFont(14, FW_NORMAL);
    fontSmall_ = MakeFont(12, FW_NORMAL);
    fontControl_ = MakeFont(18, FW_NORMAL);
    if (!fontLogo_ || !fontHeadline_ || !fontTitle_ || !fontNormal_ || !fontSmall_ || !fontControl_) {
        DestroyUiResources();
        return false;
    }
    return true;
}

void App::DestroyUiResources() {
    if (backBufferDc_ && backBufferOldBitmap_) {
        SelectObject(backBufferDc_, backBufferOldBitmap_);
        backBufferOldBitmap_ = nullptr;
    }
    if (backBufferBitmap_) { DeleteObject(backBufferBitmap_); backBufferBitmap_ = nullptr; }
    backBufferPixels_ = nullptr;
    if (backBufferDc_) { DeleteDC(backBufferDc_); backBufferDc_ = nullptr; }

    const HFONT fonts[] = {fontLogo_, fontHeadline_, fontTitle_, fontNormal_, fontSmall_, fontControl_};
    for (HFONT font : fonts) if (font) DeleteObject(font);
    fontLogo_ = nullptr;
    fontHeadline_ = nullptr;
    fontTitle_ = nullptr;
    fontNormal_ = nullptr;
    fontSmall_ = nullptr;
    fontControl_ = nullptr;
}

void App::AddTrayIcon() {
    trayIcon_ = {};
    trayIcon_.cbSize = sizeof(trayIcon_);
    trayIcon_.hWnd = hwnd_;
    trayIcon_.uID = 1;
    trayIcon_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    trayIcon_.uCallbackMessage = WM_APP_TRAY;
    trayIcon_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
    wcscpy_s(trayIcon_.szTip, _countof(trayIcon_.szTip), L"Luna Capture 1.0.0.0");
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &trayIcon_) != FALSE;
}

void App::RemoveTrayIcon() {
    if (trayAdded_) {
        Shell_NotifyIconW(NIM_DELETE, &trayIcon_);
        trayAdded_ = false;
    }
}

void App::ShowTrayMenu() {
    POINT cursor{};
    GetCursorPos(&cursor);
    trayMenu_.Show(cursor, settings_);
}

void App::ShowMainWindow(Page page) {
    page_ = page;
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    InvalidateUi();
}

void App::HideMainWindow() { ShowWindow(hwnd_, SW_HIDE); }

void App::ExitApplication() {
    if (exiting_) return;
    exiting_ = true;
    StopHotkeyRecorder();
    UnregisterAllHotkeys();
    if (engine_) { engine_->Stop(); engine_.reset(); }
    soundPlayer_.Shutdown();
    trayMenu_.Destroy();
    regionEditor_.Destroy();
    overlay_.Destroy();
    RemoveTrayIcon();
    KillTimer(hwnd_, kAnimationTimer);
    DestroyUiResources();
    if (hwnd_ && IsWindow(hwnd_)) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

bool App::RegisterAllHotkeys() {
    UnregisterAllHotkeys();
    if (!engineReady_ || recordingHotkeySlot_ >= 0) return false;
    const auto reg = [this](UINT id, const HotkeyBinding& binding) {
        return RegisterHotKey(hwnd_, id, binding.modifiers | MOD_NOREPEAT, binding.vk) != FALSE;
    };
    const bool active = reg(LUNASHOT_HOTKEY_ACTIVE, settings_.activeDisplayHotkey);
    const bool all = active && reg(LUNASHOT_HOTKEY_ALL, settings_.allDisplaysHotkey);
    const bool region = all && reg(LUNASHOT_HOTKEY_REGION, settings_.regionHotkey);
    const bool window = region && reg(LUNASHOT_HOTKEY_WINDOW, settings_.activeWindowHotkey);
    if (!(active && all && region && window)) {
        UnregisterAllHotkeys();
        engineStatus_ = L"Hotkey conflict";
        engineDetail_ = L"One shortcut is already used by Windows or another application";
        InvalidateUi();
        return false;
    }
    hotkeysRegistered_ = true;
    return true;
}

void App::UnregisterAllHotkeys() {
    UnregisterHotKey(hwnd_, LUNASHOT_HOTKEY_ACTIVE);
    UnregisterHotKey(hwnd_, LUNASHOT_HOTKEY_ALL);
    UnregisterHotKey(hwnd_, LUNASHOT_HOTKEY_REGION);
    UnregisterHotKey(hwnd_, LUNASHOT_HOTKEY_WINDOW);
    hotkeysRegistered_ = false;
}

bool App::StartHotkeyRecorder() {
    StopHotkeyRecorder();
    hotkeyCapturePosted_ = false;
    hotkeyRecordingInstance_ = this;
    hotkeyRecordingHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &App::LowLevelKeyboardProc, instance_, 0);
    if (!hotkeyRecordingHook_) {
        hotkeyRecordingInstance_ = nullptr;
        return false;
    }
    return true;
}

void App::StopHotkeyRecorder() {
    if (hotkeyRecordingHook_) {
        UnhookWindowsHookEx(hotkeyRecordingHook_);
        hotkeyRecordingHook_ = nullptr;
    }
    if (hotkeyRecordingInstance_ == this) hotkeyRecordingInstance_ = nullptr;
    hotkeyCapturePosted_ = false;
}

void App::QueueRecordedHotkey(UINT vk, UINT modifiers) {
    if (recordingHotkeySlot_ < 0 || hotkeyCapturePosted_ || vk == 0 || IsModifierKey(vk)) return;
    hotkeyCapturePosted_ = true;
    PostMessageW(hwnd_, WM_APP_HOTKEY_CAPTURED, static_cast<WPARAM>(vk), static_cast<LPARAM>(modifiers | MOD_NOREPEAT));
}

LRESULT CALLBACK App::LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    App* app = hotkeyRecordingInstance_;
    if (code == HC_ACTION && app && app->recordingHotkeySlot_ >= 0 &&
        (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (key && key->vkCode != VK_PACKET && key->vkCode != 0) {
            UINT modifiers = CurrentModifiers();
            if (key->flags & LLKHF_ALTDOWN) modifiers |= MOD_ALT;
            app->QueueRecordedHotkey(static_cast<UINT>(key->vkCode), modifiers);
        }
    }
    return CallNextHookEx(app ? app->hotkeyRecordingHook_ : nullptr, code, wParam, lParam);
}

void App::HandleRawKeyboardInput(HRAWINPUT rawInput) {
    if (recordingHotkeySlot_ < 0 || hotkeyCapturePosted_) return;
    UINT size = 0;
    if (GetRawInputData(rawInput, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0) return;
    std::vector<std::byte> storage(size);
    if (GetRawInputData(rawInput, RID_INPUT, storage.data(), &size, sizeof(RAWINPUTHEADER)) != size) return;
    const auto* input = reinterpret_cast<const RAWINPUT*>(storage.data());
    if (input->header.dwType != RIM_TYPEKEYBOARD) return;
    const RAWKEYBOARD& keyboard = input->data.keyboard;
    if (keyboard.Message != WM_KEYDOWN && keyboard.Message != WM_SYSKEYDOWN) return;
    const UINT vk = NormalizeRawVirtualKey(keyboard);
    QueueRecordedHotkey(vk, CurrentModifiers());
}

void App::BeginHotkeyRecording(int slot) {
    recordingHotkeySlot_ = slot;
    UnregisterAllHotkeys();
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    const bool hookReady = StartHotkeyRecorder();
    engineStatus_ = L"Recording shortcut";
    engineDetail_ = hookReady
        ? L"Press the compact-keyboard shortcut. Fn may stay hidden; the produced Windows key is recorded."
        : L"Press the shortcut. Global detection failed, so focused-window input is being used.";
    InvalidateUi();
}

void App::CancelHotkeyRecording() {
    StopHotkeyRecorder();
    recordingHotkeySlot_ = -1;
    RegisterAllHotkeys();
    engineStatus_ = engineReady_ ? L"Ready" : L"Starting";
    engineDetail_ = engineReady_ ? L"Four native capture shortcuts are active" : L"Preparing DXGI";
    InvalidateUi();
}

void App::FinishHotkeyRecording(UINT vk, UINT modifiers) {
    if (recordingHotkeySlot_ < 0 || IsModifierKey(vk)) return;
    StopHotkeyRecorder();
    HotkeyBinding binding{vk, modifiers | MOD_NOREPEAT};
    HotkeyBinding* target = recordingHotkeySlot_ == 0 ? &settings_.activeDisplayHotkey :
                            recordingHotkeySlot_ == 1 ? &settings_.allDisplaysHotkey :
                            recordingHotkeySlot_ == 2 ? &settings_.regionHotkey : &settings_.activeWindowHotkey;
    const HotkeyBinding previous = *target;
    *target = binding;
    const auto same = [](const HotkeyBinding& a, const HotkeyBinding& b) {
        return a.vk == b.vk && (a.modifiers | MOD_NOREPEAT) == (b.modifiers | MOD_NOREPEAT);
    };
    const HotkeyBinding bindings[] = {settings_.activeDisplayHotkey, settings_.allDisplaysHotkey,
                                      settings_.regionHotkey, settings_.activeWindowHotkey};
    bool duplicate = false;
    for (std::size_t i = 0; i < std::size(bindings); ++i) {
        for (std::size_t j = i + 1; j < std::size(bindings); ++j) duplicate |= same(bindings[i], bindings[j]);
    }
    if (duplicate) {
        *target = previous;
        engineStatus_ = L"Duplicate shortcut";
        engineDetail_ = L"Each capture mode needs a different shortcut";
        recordingHotkeySlot_ = -1;
        RegisterAllHotkeys();
        InvalidateUi();
        return;
    }
    recordingHotkeySlot_ = -1;
    if (!RegisterAllHotkeys()) {
        *target = previous;
        RegisterAllHotkeys();
        return;
    }
    SaveSettings();
    engineStatus_ = L"Ready";
    engineDetail_ = L"Shortcut updated";
    InvalidateUi();
}

void App::SaveSettings(bool applyAutostart) {
    settingsStore_.Save(settings_);
    if (applyAutostart) settingsStore_.ApplyAutostart(settings_.startWithWindows);
    if (engine_) engine_->UpdateSettings(settings_);
}

void App::TriggerCapture(CaptureMode mode) {
    if (!engine_ || !engineReady_) {
        engineStatus_ = L"Capture engine is not ready";
        InvalidateUi();
        return;
    }
    HWND targetWindow = nullptr;
    HMONITOR monitor = nullptr;
    if (mode == CaptureMode::ActiveDisplay) monitor = GetPriorityMonitor(hwnd_);
    if (mode == CaptureMode::ActiveWindow) {
        targetWindow = GetPriorityCaptureWindow(hwnd_);
        if (!targetWindow) {
            engineStatus_ = L"No capturable window";
            engineDetail_ = L"Focus a normal application window and try again";
            InvalidateUi();
            return;
        }
        monitor = MonitorFromWindow(targetWindow, MONITOR_DEFAULTTONEAREST);
    }
    const std::wstring filenamePrefix = L"Screenshot";
    if (!engine_->RequestCapture(mode, monitor, targetWindow, filenamePrefix, settings_)) {
        engineStatus_ = L"Capture queue is busy";
        engineDetail_ = L"Try again in a moment";
        InvalidateUi();
    }
}

void App::HandleEngineEvent(std::unique_ptr<EngineEvent> event) {
    if (!event) return;
    switch (event->type) {
        case EngineEventType::Ready:
            engineReady_ = true;
            engineStatus_ = L"Ready";
            engineDetail_ = L"Four capture shortcuts are ready";
            RegisterAllHotkeys();
            break;
        case EngineEventType::Reconfiguring:
            engineReady_ = false;
            engineStatus_ = L"Reconnecting";
            engineDetail_ = event->detail;
            UnregisterAllHotkeys();
            break;
        case EngineEventType::Feedback: {
            lastCapturePath_ = event->path;
            lastResponseMs_ = event->responseMs;
            if (settings_.sound) soundPlayer_.Play();
            if (settings_.notifications) {
                overlay_.ShowSuccess(std::filesystem::path(event->path).filename().wstring(), event->responseMs,
                                     settings_.overlayDurationMs, event->monitor);
            }
            engineStatus_ = L"Captured";
            engineDetail_ = event->mode == CaptureMode::AllDisplays ? L"All displays queued for saving" :
                            event->mode == CaptureMode::ActiveWindow ? L"Active window queued for saving" :
                                                                      L"Active display queued for saving";
            break;
        }
        case EngineEventType::Saved:
            lastCapturePath_ = event->path;
            lastSaveMs_ = event->saveMs;
            engineStatus_ = L"Saved";
            engineDetail_ = PathFileName(event->path);
            break;
        case EngineEventType::RegionFrame:
            engineStatus_ = event->mode == CaptureMode::ActiveWindow ? L"Window picker" : L"Region editor";
            engineDetail_ = event->mode == CaptureMode::ActiveWindow ? L"Hover a window and click to capture" : L"Select an area and annotate it";
            UnregisterAllHotkeys();
            if (!regionEditor_.Show(event->frame, event->mode == CaptureMode::ActiveWindow ? RegionEditor::SessionMode::WindowPicker : RegionEditor::SessionMode::Region, settings_, event->filenamePrefix)) {
                engineStatus_ = event->mode == CaptureMode::ActiveWindow ? L"Window picker failed" : L"Region editor failed";
                engineDetail_ = L"The frozen desktop surface could not be created";
                RegisterAllHotkeys();
            }
            break;
        case EngineEventType::Error:
            engineStatus_ = L"Capture error";
            engineDetail_ = event->detail;
            if (settings_.notifications) overlay_.ShowError(event->detail, settings_.overlayDurationMs,
                                                             GetPriorityMonitor(hwnd_));
            break;
    }
    InvalidateUi();
}

void App::HandleRegionResult(std::unique_ptr<RegionResult> result) {
    if (!result || result->action == RegionResultAction::Cancel) {
        RegisterAllHotkeys();
        engineStatus_ = L"Ready";
        engineDetail_ = L"Capture cancelled";
        InvalidateUi();
        return;
    }

    RegisterAllHotkeys();

    const bool windowMode = result->sourceMode == CaptureMode::ActiveWindow;
    const bool wantsCopy = result->action == RegionResultAction::Copy || result->action == RegionResultAction::SaveAndCopy ||
                           (settings_.copyRegionToClipboard && result->action == RegionResultAction::Save);
    const bool wantsSave = result->action == RegionResultAction::Save || result->action == RegionResultAction::SaveAndCopy;

    bool copied = false;
    if (wantsCopy) copied = CopyFrameToClipboard(hwnd_, result->frame);

    const wchar_t* savedTitle = windowMode ? L"Window captured" : L"Region captured";
    const wchar_t* copiedDetail = windowMode ? L"Window copied to clipboard" : L"Region copied to clipboard";

    if (wantsSave && engine_) {
        AppSettings saveSettings = settings_;
        if (!windowMode && !result->savePath.empty()) saveSettings.imageFormat = result->saveFormat;
        if (settings_.sound) soundPlayer_.Play();
        engine_->SaveEditedFrame(std::move(result->frame), saveSettings, copied, result->savePath, result->filenamePrefix);
        if (settings_.notifications) {
            const std::wstring detail = result->savePath.empty()
                ? (copied ? L"Saving and copied to clipboard" : L"Saving screenshot")
                : (copied ? L"Saved there · copied to clipboard" : L"Saving to selected location");
            overlay_.ShowMessage(savedTitle, detail, settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
        }
        engineStatus_ = savedTitle;
        engineDetail_ = result->savePath.empty() ?
            (copied ? L"Saving · copied to clipboard" : L"Saving screenshot") :
            (L"Saving to " + PathFileName(result->savePath));
    } else if (copied) {
        if (settings_.sound) soundPlayer_.Play();
        if (settings_.notifications) overlay_.ShowMessage(L"Copied", copiedDetail,
                                                           settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
        engineStatus_ = L"Copied";
        engineDetail_ = copiedDetail;
    }
    InvalidateUi();
}

void App::HandleTrayCommand(TrayCommand command) {
    switch (command) {
        case TrayCommand::Open: ShowMainWindow(); break;
        case TrayCommand::CaptureActive: TriggerCapture(CaptureMode::ActiveDisplay); break;
        case TrayCommand::CaptureAll: TriggerCapture(CaptureMode::AllDisplays); break;
        case TrayCommand::CaptureRegion: TriggerCapture(CaptureMode::Region); break;
        case TrayCommand::CaptureWindow: TriggerCapture(CaptureMode::ActiveWindow); break;
        case TrayCommand::OpenFolder: OpenFolderInExplorer(settings_.saveDirectory); break;
        case TrayCommand::ToggleSound: settings_.sound = !settings_.sound; SaveSettings(); InvalidateUi(); break;
        case TrayCommand::ToggleNotifications: settings_.notifications = !settings_.notifications; SaveSettings(); InvalidateUi(); break;
        case TrayCommand::Exit: ExitApplication(); PostQuitMessage(0); break;
    }
}

void App::NavigateTo(Page page) {
    if (page_ == page) return;
    page_ = page;
    InvalidateUi();
}

float App::HoverAmount(Action action) const { return hoverAnimation_[static_cast<std::size_t>(action)]; }
float App::PressAmount(Action action) const { return pressAnimation_[static_cast<std::size_t>(action)]; }

void App::TickAnimations() {
    if (!IsWindowVisible(hwnd_)) return;
    const ULONGLONG now = GetTickCount64();
    if (!lastAnimationTick_) lastAnimationTick_ = now;
    const float delta = (std::min)(50.0f, static_cast<float>(now - lastAnimationTick_)) / 16.0f;
    lastAnimationTick_ = now;
    bool changed = false;
    for (std::size_t i = 0; i < hoverAnimation_.size(); ++i) {
        const float target = static_cast<Action>(i) == hoveredAction_ ? 1.0f : 0.0f;
        const float next = hoverAnimation_[i] + (target - hoverAnimation_[i]) * (std::min)(1.0f, 0.28f * delta);
        changed |= std::fabs(next - hoverAnimation_[i]) > 0.002f;
        hoverAnimation_[i] = next;
        const float pressTarget = static_cast<Action>(i) == pressedAction_ ? 1.0f : 0.0f;
        const float pressNext = pressAnimation_[i] + (pressTarget - pressAnimation_[i]) * (std::min)(1.0f, 0.38f * delta);
        changed |= std::fabs(pressNext - pressAnimation_[i]) > 0.002f;
        pressAnimation_[i] = pressNext;
    }
    const float targetY = page_ == Page::Home ? 102.0f : page_ == Page::Capture ? 150.0f : 198.0f;
    const float nextY = sidebarIndicatorY_ + (targetY - sidebarIndicatorY_) * (std::min)(1.0f, 0.24f * delta);
    changed |= std::fabs(nextY - sidebarIndicatorY_) > 0.05f;
    sidebarIndicatorY_ = nextY;
    if (changed) InvalidateUi();
}

void App::AddRegion(int x, int y, int width, int height, Action action) {
    hitRegions_.push_back(HitRegion{RECT{x, y, x + width, y + height}, action});
}

App::Action App::HitTestAction(POINT point) const {
    for (auto it = hitRegions_.rbegin(); it != hitRegions_.rend(); ++it) {
        if (PtInRect(&it->rect, point)) return it->action;
    }
    return Action::None;
}

void App::ExecuteAction(Action action) {
    switch (action) {
        case Action::Minimize: ShowWindow(hwnd_, SW_MINIMIZE); break;
        case Action::Hide: HideMainWindow(); break;
        case Action::Exit: ExitApplication(); PostQuitMessage(0); break;
        case Action::NavHome: NavigateTo(Page::Home); break;
        case Action::NavCapture: NavigateTo(Page::Capture); break;
        case Action::NavSettings: NavigateTo(Page::Settings); break;
        case Action::CaptureActive: TriggerCapture(CaptureMode::ActiveDisplay); break;
        case Action::CaptureAll: TriggerCapture(CaptureMode::AllDisplays); break;
        case Action::CaptureRegion: TriggerCapture(CaptureMode::Region); break;
        case Action::CaptureWindow: TriggerCapture(CaptureMode::ActiveWindow); break;
        case Action::OpenFolder: OpenFolderInExplorer(settings_.saveDirectory); break;
        case Action::ChooseFolder: {
            std::wstring selected;
            if (ChooseFolder(hwnd_, settings_.saveDirectory, selected)) { settings_.saveDirectory = selected; SaveSettings(); }
            break;
        }
        case Action::ToggleFormat:
            settings_.imageFormat = settings_.imageFormat == ImageFormat::Png ? ImageFormat::Jpeg : ImageFormat::Png;
            SaveSettings(); break;
        case Action::CycleResolution: {
            const auto next = NextResolution(settings_.outputWidth, settings_.outputHeight);
            settings_.outputWidth = next.first; settings_.outputHeight = next.second; SaveSettings(); break;
        }
        case Action::ToggleNotifications: settings_.notifications = !settings_.notifications; SaveSettings(); break;
        case Action::ToggleSound: settings_.sound = !settings_.sound; SaveSettings(); break;
        case Action::ToggleCopyRegion: settings_.copyRegionToClipboard = !settings_.copyRegionToClipboard; SaveSettings(); break;
        case Action::TestOverlay: overlay_.ShowMessage(L"Luna Capture", L"Notifications are ready", settings_.overlayDurationMs, GetPriorityMonitor(hwnd_)); break;
        case Action::RecordActiveHotkey: BeginHotkeyRecording(0); break;
        case Action::RecordAllHotkey: BeginHotkeyRecording(1); break;
        case Action::RecordRegionHotkey: BeginHotkeyRecording(2); break;
        case Action::RecordWindowHotkey: BeginHotkeyRecording(3); break;
        case Action::ToggleAutostart: settings_.startWithWindows = !settings_.startWithWindows; SaveSettings(true); break;
        case Action::CycleOverlayDuration:
            settings_.overlayDurationMs = settings_.overlayDurationMs >= 2000 ? 800 : settings_.overlayDurationMs + 400;
            SaveSettings(); break;
        case Action::ResetDefaults:
            settings_ = AppSettings{};
            settings_.saveDirectory = GetDefaultScreenshotDirectory();
            SaveSettings(true);
            RegisterAllHotkeys();
            break;
        default: break;
    }
    InvalidateUi();
}

void App::PaintSidebar(HDC dc) {
    RECT sidebar{0, 0, kSidebarWidth, kWindowHeight};
    Fill(dc, sidebar, RGB(17, 19, 24));
    Fill(dc, RECT{kSidebarWidth - 1, 0, kSidebarWidth, kWindowHeight}, RGB(28, 32, 40));

    // Logo mark: a rounded gradient tile with the capture glyph, next to the wordmark.
    RECT logoTile{22, 20, 54, 52};
    ui::FillRoundRectVGradient(dc, logoTile, 9, RGB(122, 162, 255), RGB(78, 116, 232));
    DrawUiIcon(dc, UiIcon::Capture, RECT{logoTile.left + 7, logoTile.top + 7, logoTile.right - 7, logoTile.bottom - 7}, RGB(255, 255, 255), 2);
    Text(dc, L"Luna Capture", RECT{62, 20, 194, 52}, RGB(244, 247, 252), fontLogo_);

    FillRound(dc, RECT{16, 78, 180, 236}, 16, RGB(21, 24, 30));
    FillRound(dc, RECT{18, static_cast<int>(sidebarIndicatorY_), 22, static_cast<int>(sidebarIndicatorY_) + 30}, 3,
              RGB(96, 149, 244));

    const struct { const wchar_t* label; int y; Action action; Page page; UiIcon icon; } items[] = {
        {L"Home", 88, Action::NavHome, Page::Home, UiIcon::Home},
        {L"Capture", 136, Action::NavCapture, Page::Capture, UiIcon::Capture},
        {L"Settings", 184, Action::NavSettings, Page::Settings, UiIcon::Settings},
    };
    for (const auto& item : items) {
        RECT rect{30, item.y, 166, item.y + 40};
        const float hover = HoverAmount(item.action);
        if (hover > 0.01f) FillRound(dc, rect, 10, Mix(RGB(21, 24, 30), RGB(34, 39, 49), hover));
        const COLORREF navColor = page_ == item.page ? RGB(242, 245, 251) : RGB(145, 151, 164);
        DrawUiIcon(dc, item.icon, RECT{43, item.y + 11, 61, item.y + 29}, navColor, 2);
        Text(dc, item.label, RECT{68, item.y, 160, item.y + 40}, navColor, fontTitle_);
        AddRegion(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, item.action);
    }

    Text(dc, L"Native C++", RECT{24, 566, 170, 590}, RGB(111, 118, 132), fontSmall_);
    Text(dc, L"Version 1.0.0.0", RECT{24, 588, 170, 612}, RGB(84, 90, 102), fontSmall_);
}

void App::PaintTopBar(HDC dc) {
    RECT top{kSidebarWidth, 0, kWindowWidth, kTopBarHeight};
    Fill(dc, top, RGB(14, 16, 20));
    const wchar_t* name = page_ == Page::Home ? L"Home" : page_ == Page::Capture ? L"Capture" : L"Settings";
    Text(dc, name, RECT{kSidebarWidth + 30, 14, kWindowWidth - 170, 52}, RGB(236, 239, 246), fontTitle_);

    const struct { int x; UiIcon icon; Action action; } buttons[] = {
        {880, UiIcon::Minimize, Action::Minimize}, {920, UiIcon::Close, Action::Hide},
    };
    for (const auto& button : buttons) {
        RECT rect{button.x, 14, button.x + 34, 48};
        const float hover = HoverAmount(button.action);
        if (hover > 0.01f) FillRound(dc, rect, 10, Mix(RGB(14, 16, 20), RGB(39, 43, 52), hover));
        DrawUiIcon(dc, button.icon, RECT{rect.left + 9, rect.top + 9, rect.right - 9, rect.bottom - 9}, RGB(190, 196, 208), 2);
        AddRegion(rect.left, rect.top, 34, 34, button.action);
    }
}

void App::PaintHome(HDC dc) {
    const int x = 228;

    Text(dc, L"Capture instantly. Keep moving.", RECT{x, 80, 940, 120}, RGB(244, 246, 251), fontHeadline_);
    Text(dc, L"Active display, full desktop, or a precise region.", RECT{x, 118, 940, 146}, RGB(137, 144, 158), fontNormal_);

    RECT status{x, 164, 956, 252};
    FillRound(dc, status, 16, RGB(20, 23, 29));
    StrokeRound(dc, status, 16, RGB(39, 44, 54));
    const COLORREF dot = engineReady_ ? RGB(78, 205, 139) : RGB(246, 180, 72);
    ui::FillEllipse(dc, RECT{x + 24, 190, x + 40, 206}, ui::MixColor(dot, RGB(14, 16, 20), 0.7f), 90);
    ui::FillEllipse(dc, RECT{x + 27, 193, x + 37, 203}, dot);
    Text(dc, engineStatus_, RECT{x + 52, 177, 700, 207}, RGB(238, 241, 247), fontTitle_);
    Text(dc, engineDetail_, RECT{x + 52, 205, 900, 234}, RGB(130, 137, 151), fontNormal_);

    const struct Card { int x; const wchar_t* title; const wchar_t* detail; std::wstring hotkey; Action action; UiIcon icon; } cards[] = {
        {x, L"Active display", L"Foreground monitor", FormatHotkey(settings_.activeDisplayHotkey.modifiers, settings_.activeDisplayHotkey.vk), Action::CaptureActive, UiIcon::Monitor},
        {x + 182, L"All displays", L"Stitched virtual desktop", FormatHotkey(settings_.allDisplaysHotkey.modifiers, settings_.allDisplaysHotkey.vk), Action::CaptureAll, UiIcon::AllDisplays},
        {x + 364, L"Region editor", L"Select, annotate, copy or save", FormatHotkey(settings_.regionHotkey.modifiers, settings_.regionHotkey.vk), Action::CaptureRegion, UiIcon::Region},
        {x + 546, L"Active window", L"Click a window to capture", FormatHotkey(settings_.activeWindowHotkey.modifiers, settings_.activeWindowHotkey.vk), Action::CaptureWindow, UiIcon::Window},
    };
    for (const auto& card : cards) {
        RECT rect{card.x, 276, card.x + 170, 418};
        const float hover = HoverAmount(card.action);
        const float press = PressAmount(card.action);
        FillRound(dc, rect, 16, Mix(RGB(20, 23, 29), RGB(29, 35, 44), hover * 0.8f));
        StrokeRound(dc, rect, 16, Mix(RGB(39, 44, 54), RGB(84, 134, 230), hover));
        RECT badge{rect.left + 16, rect.top + 16, rect.left + 48, rect.top + 48};
        ui::FillRoundRectVGradient(dc, badge, 11, Mix(RGB(108, 150, 246), RGB(128, 168, 255), hover),
                                   Mix(RGB(70, 110, 214), RGB(88, 128, 236), hover));
        DrawUiIcon(dc, card.icon, RECT{badge.left + 7, badge.top + 7, badge.right - 7, badge.bottom - 7}, RGB(255, 255, 255), 2);
        Text(dc, card.title, RECT{rect.left + 16, rect.top + 52, rect.right - 12, rect.top + 79}, RGB(239, 242, 248), fontTitle_);
        Text(dc, card.detail, RECT{rect.left + 16, rect.top + 80, rect.right - 12, rect.top + 116}, RGB(124, 132, 146), fontSmall_, DT_LEFT | DT_WORDBREAK);
        Text(dc, card.hotkey, RECT{rect.left + 16, rect.bottom - 34, rect.right - 14, rect.bottom - 12}, RGB(113, 169, 255), fontSmall_);
        if (press > 0.02f) StrokeRound(dc, rect, 16, RGB(120, 172, 255), 2);
        AddRegion(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, card.action);
    }

    RECT recent{x, 432, 956, 554};
    FillRound(dc, recent, 16, RGB(20, 23, 29));
    StrokeRound(dc, recent, 16, RGB(39, 44, 54));
    Text(dc, L"Latest capture", RECT{x + 20, 444, x + 250, 474}, RGB(238, 241, 247), fontTitle_);
    Text(dc, PathFileName(lastCapturePath_), RECT{x + 20, 477, 760, 507}, RGB(144, 151, 165), fontNormal_);
    std::wstring metrics = lastResponseMs_ > 0.0 ? L"Response " + FormatMilliseconds(lastResponseMs_) : L"Waiting for first capture";
    if (lastSaveMs_ > 0.0) metrics += L"  ·  Saved in " + FormatMilliseconds(lastSaveMs_);
    Text(dc, metrics, RECT{x + 20, 510, 760, 538}, RGB(99, 151, 239), fontSmall_);
    RECT open{780, 462, 930, 520};
    FillRound(dc, open, 12, Mix(RGB(34, 38, 47), RGB(52, 62, 78), HoverAmount(Action::OpenFolder)));
    DrawUiIcon(dc, UiIcon::Folder, RECT{open.left + 20, open.top + 19, open.left + 40, open.top + 39}, RGB(192, 205, 226), 2);
    Text(dc, L"Open folder", RECT{open.left + 42, open.top, open.right - 12, open.bottom}, RGB(232, 235, 242), fontNormal_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddRegion(open.left, open.top, open.right - open.left, open.bottom - open.top, Action::OpenFolder);

}

void App::PaintCapture(HDC dc) {
    const int x = 228;
    Text(dc, L"Capture output", RECT{x, 80, 940, 118}, RGB(244, 246, 251), fontHeadline_);
    Text(dc, L"Only the settings that affect your files and feedback.", RECT{x, 116, 940, 144}, RGB(137, 144, 158), fontNormal_);

    RECT folder{x, 164, 956, 250};
    FillRound(dc, folder, 16, RGB(20, 23, 29)); StrokeRound(dc, folder, 16, RGB(39, 44, 54));
    Text(dc, L"Save location", RECT{x + 20, 174, x + 240, 204}, RGB(238, 241, 247), fontTitle_);
    Text(dc, settings_.saveDirectory, RECT{x + 20, 205, 760, 235}, RGB(126, 134, 149), fontNormal_);
    RECT choose{790, 180, 930, 230};
    FillRound(dc, choose, 12, Mix(RGB(34, 38, 47), RGB(52, 62, 78), HoverAmount(Action::ChooseFolder)));
    DrawUiIcon(dc, UiIcon::Folder, RECT{choose.left + 22, choose.top + 16, choose.left + 42, choose.top + 36}, RGB(192, 205, 226), 2);
    Text(dc, L"Choose", RECT{choose.left + 42, choose.top, choose.right - 12, choose.bottom}, RGB(232, 235, 242), fontNormal_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddRegion(choose.left, choose.top, choose.right - choose.left, choose.bottom - choose.top, Action::ChooseFolder);

    const struct Option { int y; const wchar_t* title; const wchar_t* detail; Action action; } options[] = {
        {274, L"Image format", settings_.imageFormat == ImageFormat::Png ? L"PNG · lossless" : L"JPEG · smaller files", Action::ToggleFormat},
        {350, L"Output resolution", L"Original or a fixed export size", Action::CycleResolution},
    };
    for (const auto& option : options) {
        RECT row{x, option.y, 582, option.y + 62};
        FillRound(dc, row, 14, Mix(RGB(20, 23, 29), RGB(28, 33, 42), HoverAmount(option.action)));
        StrokeRound(dc, row, 14, RGB(39, 44, 54));
        const UiIcon optionIcon = option.action == Action::ToggleFormat ? UiIcon::Capture : UiIcon::Monitor;
        DrawUiIcon(dc, optionIcon, RECT{x + 17, option.y + 20, x + 37, option.y + 40}, RGB(121, 168, 242), 2);
        Text(dc, option.title, RECT{x + 49, option.y + 6, 450, option.y + 34}, RGB(235, 238, 244), fontTitle_);
        Text(dc, option.detail, RECT{x + 49, option.y + 31, 470, option.y + 56}, RGB(124, 132, 146), fontSmall_);
        const std::wstring value = option.action == Action::ToggleFormat ?
            (settings_.imageFormat == ImageFormat::Png ? L"PNG" : L"JPEG") : ResolutionLabel(settings_);
        Text(dc, value, RECT{430, option.y, 560, option.y + 62}, RGB(111, 167, 255), fontNormal_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        AddRegion(row.left, row.top, row.right - row.left, row.bottom - row.top, option.action);
    }

    const struct Toggle { int y; const wchar_t* title; const wchar_t* detail; bool on; Action action; } toggles[] = {
        {274, L"Visual notifications", L"Compact capture confirmation", settings_.notifications, Action::ToggleNotifications},
        {350, L"Capture sound", L"Preloaded low-latency sound", settings_.sound, Action::ToggleSound},
        {426, L"Copy region automatically", L"Region Save also copies to clipboard", settings_.copyRegionToClipboard, Action::ToggleCopyRegion},
    };
    for (const auto& toggle : toggles) {
        RECT row{606, toggle.y, 956, toggle.y + 62};
        FillRound(dc, row, 14, Mix(RGB(20, 23, 29), RGB(28, 33, 42), HoverAmount(toggle.action)));
        StrokeRound(dc, row, 14, RGB(39, 44, 54));
        const UiIcon toggleIcon = toggle.action == Action::ToggleNotifications ? UiIcon::Bell :
                                  toggle.action == Action::ToggleSound ? UiIcon::Sound : UiIcon::Copy;
        DrawUiIcon(dc, toggleIcon, RECT{623, toggle.y + 20, 643, toggle.y + 40}, RGB(121, 168, 242), 2);
        Text(dc, toggle.title, RECT{655, toggle.y + 6, 870, toggle.y + 34}, RGB(235, 238, 244), fontTitle_);
        Text(dc, toggle.detail, RECT{655, toggle.y + 31, 870, toggle.y + 56}, RGB(124, 132, 146), fontSmall_);
        DrawToggle(dc, 888, toggle.y + 19, toggle.on, HoverAmount(toggle.action));
        AddRegion(row.left, row.top, row.right - row.left, row.bottom - row.top, toggle.action);
    }

    RECT test{606, 502, 956, 558};
    FillRound(dc, test, 14, Mix(RGB(31, 37, 47), RGB(48, 60, 78), HoverAmount(Action::TestOverlay)));
    DrawUiIcon(dc, UiIcon::Bell, RECT{test.left + 82, test.top + 18, test.left + 102, test.top + 38}, RGB(185, 204, 235), 2);
    Text(dc, L"Test notification", RECT{test.left + 104, test.top, test.right - 52, test.bottom}, RGB(233, 237, 244), fontNormal_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddRegion(test.left, test.top, test.right - test.left, test.bottom - test.top, Action::TestOverlay);

}

void App::PaintSettings(HDC dc) {
    const int x = 228;
    Text(dc, L"Shortcuts and startup", RECT{x, 76, 940, 112}, RGB(244, 246, 251), fontHeadline_);
    Text(dc, L"Click a shortcut, then press it exactly as used — compact-keyboard Fn combinations are supported.",
         RECT{x, 111, 956, 139}, RGB(137, 144, 158), fontNormal_);

    const struct HotkeyRow { int y; const wchar_t* title; const wchar_t* detail; const HotkeyBinding* binding; Action action; int slot; UiIcon icon; } rows[] = {
        {150, L"Active display", L"Foreground monitor; cursor fallback", &settings_.activeDisplayHotkey, Action::RecordActiveHotkey, 0, UiIcon::Monitor},
        {218, L"All displays", L"Stitched virtual desktop", &settings_.allDisplaysHotkey, Action::RecordAllHotkey, 1, UiIcon::AllDisplays},
        {286, L"Region editor", L"Selection and annotation overlay", &settings_.regionHotkey, Action::RecordRegionHotkey, 2, UiIcon::Region},
        {354, L"Active window", L"Click a window to capture", &settings_.activeWindowHotkey, Action::RecordWindowHotkey, 3, UiIcon::Window},
    };
    for (const auto& row : rows) {
        RECT rect{x, row.y, 956, row.y + 58};
        FillRound(dc, rect, 13, Mix(RGB(20, 23, 29), RGB(29, 35, 44), HoverAmount(row.action)));
        StrokeRound(dc, rect, 13, recordingHotkeySlot_ == row.slot ? RGB(93, 149, 245) : RGB(39, 44, 54), recordingHotkeySlot_ == row.slot ? 2 : 1);
        DrawUiIcon(dc, row.icon, RECT{x + 18, row.y + 18, x + 40, row.y + 40}, RGB(127, 174, 246), 2);
        Text(dc, row.title, RECT{x + 52, row.y + 4, 590, row.y + 30}, RGB(236, 239, 245), fontTitle_);
        Text(dc, row.detail, RECT{x + 52, row.y + 29, 650, row.y + 53}, RGB(124, 132, 146), fontSmall_);
        const std::wstring hotkey = recordingHotkeySlot_ == row.slot ? L"Press shortcut / Fn combo…" : FormatHotkey(row.binding->modifiers, row.binding->vk);
        RECT keyRect{706, row.y + 9, 932, row.y + 49};
        FillRound(dc, keyRect, 10, RGB(31, 36, 45));
        Text(dc, hotkey, RECT{keyRect.left + 10, keyRect.top, keyRect.right - 10, keyRect.bottom}, RGB(113, 169, 255), fontNormal_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        AddRegion(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, row.action);
    }

    const struct SettingRow { int y; const wchar_t* title; const wchar_t* detail; Action action; bool toggle; bool on; } settingsRows[] = {
        {438, L"Start with Windows", L"Launch directly to the system tray", Action::ToggleAutostart, true, settings_.startWithWindows},
        {502, L"Notification duration", L"Compact overlay display time", Action::CycleOverlayDuration, false, false},
    };
    for (const auto& row : settingsRows) {
        RECT rect{x, row.y, 718, row.y + 54};
        FillRound(dc, rect, 13, Mix(RGB(20, 23, 29), RGB(28, 33, 42), HoverAmount(row.action)));
        StrokeRound(dc, rect, 13, RGB(39, 44, 54));
        const UiIcon settingIcon = row.action == Action::ToggleAutostart ? UiIcon::Power : UiIcon::Bell;
        DrawUiIcon(dc, settingIcon, RECT{x + 17, row.y + 17, x + 37, row.y + 37}, RGB(121, 168, 242), 2);
        Text(dc, row.title, RECT{x + 49, row.y + 2, 548, row.y + 28}, RGB(235, 238, 244), fontTitle_);
        Text(dc, row.detail, RECT{x + 49, row.y + 26, 548, row.y + 50}, RGB(124, 132, 146), fontSmall_);
        if (row.toggle) DrawToggle(dc, 646, row.y + 15, row.on, HoverAmount(row.action));
        else Text(dc, std::to_wstring(settings_.overlayDurationMs) + L" ms", RECT{570, row.y, 694, row.y + 54}, RGB(111, 167, 255), fontNormal_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        AddRegion(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, row.action);
    }

    RECT reset{742, 438, 956, 556};
    FillRound(dc, reset, 13, Mix(RGB(29, 24, 27), RGB(54, 34, 39), HoverAmount(Action::ResetDefaults)));
    StrokeRound(dc, reset, 13, RGB(70, 47, 53));
    DrawUiIcon(dc, UiIcon::Undo, RECT{760, 455, 784, 479}, RGB(238, 174, 187), 2);
    Text(dc, L"Reset defaults", RECT{790, 450, 940, 482}, RGB(244, 214, 219), fontTitle_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    Text(dc, L"Restore all four shortcuts and output options.", RECT{760, 486, 940, 536}, RGB(155, 126, 133), fontSmall_, DT_LEFT | DT_WORDBREAK);
    AddRegion(reset.left, reset.top, reset.right - reset.left, reset.bottom - reset.top, Action::ResetDefaults);
}

void App::Paint() {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(hwnd_, &paint);
    RECT client{};
    GetClientRect(hwnd_, &client);
    if (!backBufferDc_) {
        Fill(target, client, RGB(14, 16, 20));
        EndPaint(hwnd_, &paint);
        return;
    }

    Fill(backBufferDc_, client, RGB(14, 16, 20));
    ui::MakeLayeredSurfaceOpaque(backBufferPixels_, client.right, client.bottom);
    hitRegions_.clear();
    PaintSidebar(backBufferDc_);
    PaintTopBar(backBufferDc_);
    if (page_ == Page::Home) PaintHome(backBufferDc_);
    else if (page_ == Page::Capture) PaintCapture(backBufferDc_);
    else PaintSettings(backBufferDc_);
    ui::PresentRoundedLayeredWindow(hwnd_, backBufferDc_, backBufferPixels_,
                                    client.right, client.bottom, kWindowCornerRadius);
    EndPaint(hwnd_, &paint);
}

void App::InvalidateUi() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }

LRESULT CALLBACK App::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT App::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: Paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &point);
            if (!ui::PointInRoundedRect(point, kWindowWidth, kWindowHeight, kWindowCornerRadius)) {
                return HTTRANSPARENT;
            }
            break;
        }
        case WM_TIMER: if (wParam == kAnimationTimer) { TickAnimations(); return 0; } break;
        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (dragging_) {
                POINT cursor{}; GetCursorPos(&cursor);
                SetWindowPos(hwnd_, nullptr, cursor.x - dragOffset_.x, cursor.y - dragOffset_.y, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            const Action next = HitTestAction(point);
            if (next != hoveredAction_) { hoveredAction_ = next; InvalidateUi(); }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0}; TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hoveredAction_ = Action::None; InvalidateUi(); return 0;
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            pressedAction_ = HitTestAction(point);
            if (pressedAction_ != Action::None) {
                SetCapture(hwnd_); InvalidateUi(); return 0;
            }
            if (point.y < kTopBarHeight) {
                RECT window{}; GetWindowRect(hwnd_, &window);
                POINT cursor{}; GetCursorPos(&cursor);
                dragOffset_ = POINT{cursor.x - window.left, cursor.y - window.top};
                dragging_ = true; SetCapture(hwnd_); return 0;
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (dragging_) { dragging_ = false; ReleaseCapture(); return 0; }
            const Action released = HitTestAction(point);
            const Action execute = pressedAction_ == released ? released : Action::None;
            pressedAction_ = Action::None; ReleaseCapture();
            if (execute != Action::None) ExecuteAction(execute);
            InvalidateUi(); return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (recordingHotkeySlot_ >= 0 && hotkeyRecordingHook_ == nullptr) {
                if (wParam == VK_ESCAPE) CancelHotkeyRecording();
                else FinishHotkeyRecording(static_cast<UINT>(wParam), CurrentModifiers());
                return 0;
            }
            break;
        case WM_INPUT:
            if (recordingHotkeySlot_ >= 0) {
                HandleRawKeyboardInput(reinterpret_cast<HRAWINPUT>(lParam));
                return 0;
            }
            break;
        case WM_APPCOMMAND:
            if (recordingHotkeySlot_ >= 0) {
                const UINT vk = AppCommandToVirtualKey(GET_APPCOMMAND_LPARAM(lParam));
                if (vk != 0) { QueueRecordedHotkey(vk, CurrentModifiers()); return TRUE; }
            }
            break;
        case WM_APP_HOTKEY_CAPTURED:
            if (recordingHotkeySlot_ >= 0) {
                hotkeyCapturePosted_ = false;
                if (static_cast<UINT>(wParam) == VK_ESCAPE) CancelHotkeyRecording();
                else FinishHotkeyRecording(static_cast<UINT>(wParam), static_cast<UINT>(lParam));
                return 0;
            }
            hotkeyCapturePosted_ = false;
            return 0;
        case WM_HOTKEY:
            if (wParam == LUNASHOT_HOTKEY_ACTIVE) TriggerCapture(CaptureMode::ActiveDisplay);
            else if (wParam == LUNASHOT_HOTKEY_ALL) TriggerCapture(CaptureMode::AllDisplays);
            else if (wParam == LUNASHOT_HOTKEY_REGION) TriggerCapture(CaptureMode::Region);
            else if (wParam == LUNASHOT_HOTKEY_WINDOW) TriggerCapture(CaptureMode::ActiveWindow);
            return 0;
        case WM_APP_ENGINE_EVENT:
            HandleEngineEvent(std::unique_ptr<EngineEvent>(reinterpret_cast<EngineEvent*>(lParam)));
            return 0;
        case WM_APP_REGION_COMPLETE:
            HandleRegionResult(std::unique_ptr<RegionResult>(reinterpret_cast<RegionResult*>(lParam)));
            return 0;
        case WM_APP_TRAY_COMMAND:
            HandleTrayCommand(static_cast<TrayCommand>(wParam));
            return 0;
        case WM_APP_TRAY:
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) ShowTrayMenu();
            else if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) ShowMainWindow();
            return 0;
        case WM_CLOSE:
            HideMainWindow(); return 0;
        case WM_DESTROY:
            PostQuitMessage(0); return 0;
        default: break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}
