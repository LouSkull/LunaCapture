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
#include <bit>
#include <cmath>
#include <cstring>
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
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
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

constexpr UINT kHotkeyModifierMask = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;

std::wstring HotkeySlotName(int slot) {
    switch (slot) {
        case 0: return L"active display screenshot";
        case 1: return L"all displays screenshot";
        case 2: return L"region screenshot";
        case 3: return L"active window screenshot";
        case 4: return L"record toggle";
        case 5: return L"replay save";
        default:
            if (slot >= 6 && slot < 6 + static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) {
                return L"save replay for monitor " + std::to_wstring(slot - 5);
            }
            return L"unknown";
    }
}

const wchar_t* VideoCodecLabel(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::Hevc: return L"HEVC";
        case VideoCodec::Av1: return L"AV1";
        default: return L"H.264";
    }
}

std::shared_ptr<FrameBuffer> CaptureDesktopGdi(const RECT& desktopRect) {
    const int width = desktopRect.right - desktopRect.left;
    const int height = desktopRect.bottom - desktopRect.top;
    if (width <= 0 || height <= 0) return {};
    HDC screen = GetDC(nullptr);
    if (!screen) return {};
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = memory ? CreateCompatibleBitmap(screen, width, height) : nullptr;
    HGDIOBJ oldBitmap = bitmap ? SelectObject(memory, bitmap) : nullptr;
    const bool copied = bitmap && BitBlt(memory, 0, 0, width, height, screen,
                                         desktopRect.left, desktopRect.top, SRCCOPY | CAPTUREBLT) != FALSE;
    auto frame = copied ? std::make_shared<FrameBuffer>() : std::shared_ptr<FrameBuffer>{};
    if (frame) {
        frame->width = static_cast<UINT>(width);
        frame->height = static_cast<UINT>(height);
        frame->stride = frame->width * 4;
        frame->virtualRect = desktopRect;
        frame->bgra.resize(static_cast<std::size_t>(frame->stride) * frame->height);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        if (GetDIBits(memory, bitmap, 0, frame->height, frame->bgra.data(), &info, DIB_RGB_COLORS) == 0) {
            frame.reset();
        } else {
            for (std::size_t index = 3; index < frame->bgra.size(); index += 4) frame->bgra[index] = 255;
        }
    }
    if (oldBitmap) SelectObject(memory, oldBitmap);
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return frame;
}

std::shared_ptr<FrameBuffer> CombineDesktopFrames(const std::vector<std::shared_ptr<FrameBuffer>>& frames) {
    RECT bounds{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
    bool any = false;
    for (const auto& frame : frames) {
        if (!frame || frame->bgra.empty() || !frame->width || !frame->height) continue;
        bounds.left = (std::min)(bounds.left, frame->virtualRect.left);
        bounds.top = (std::min)(bounds.top, frame->virtualRect.top);
        bounds.right = (std::max)(bounds.right, frame->virtualRect.right);
        bounds.bottom = (std::max)(bounds.bottom, frame->virtualRect.bottom);
        any = true;
    }
    if (!any || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return {};
    auto combined = std::make_shared<FrameBuffer>();
    combined->width = static_cast<UINT>(bounds.right - bounds.left);
    combined->height = static_cast<UINT>(bounds.bottom - bounds.top);
    combined->stride = combined->width * 4;
    combined->virtualRect = bounds;
    combined->bgra.assign(static_cast<std::size_t>(combined->stride) * combined->height, 0);
    for (std::size_t index = 3; index < combined->bgra.size(); index += 4) combined->bgra[index] = 255;
    for (const auto& frame : frames) {
        if (!frame || frame->bgra.empty()) continue;
        const LONG destinationX = frame->virtualRect.left - bounds.left;
        const LONG destinationY = frame->virtualRect.top - bounds.top;
        if (destinationX < 0 || destinationY < 0) continue;
        const UINT width = (std::min)(frame->width, static_cast<UINT>((std::max)(0L, frame->virtualRect.right - frame->virtualRect.left)));
        const UINT height = (std::min)(frame->height, static_cast<UINT>((std::max)(0L, frame->virtualRect.bottom - frame->virtualRect.top)));
        if (static_cast<UINT>(destinationX) >= combined->width || static_cast<UINT>(destinationY) >= combined->height) continue;
        const UINT copyWidth = (std::min)(width, combined->width - static_cast<UINT>(destinationX));
        const UINT copyHeight = (std::min)(height, combined->height - static_cast<UINT>(destinationY));
        for (UINT y = 0; y < copyHeight; ++y) {
            std::memcpy(combined->bgra.data() + static_cast<std::size_t>(static_cast<UINT>(destinationY) + y) * combined->stride +
                            static_cast<std::size_t>(destinationX) * 4,
                        frame->bgra.data() + static_cast<std::size_t>(y) * frame->stride,
                        static_cast<std::size_t>(copyWidth) * 4);
        }
    }
    return combined;
}
}

App* App::globalHotkeyInstance_ = nullptr;
App* App::hotkeyRecordingInstance_ = nullptr;

App::App(HINSTANCE instance) : instance_(instance), settings_(settingsStore_.Load()) {}
App::~App() { ExitApplication(); }

bool App::Initialize(bool startHidden) {
    if (!CreateMainWindow(startHidden)) return false;
    StartGlobalKeyboardHook();
    if (!overlay_.Create(instance_)) return false;
    if (!regionEditor_.Create(instance_, hwnd_)) return false;
    if (!trayMenu_.Create(instance_, hwnd_)) return false;
    soundPlayer_.Initialize(instance_, IDR_CAPTURE_SOUND);
    LoadNotificationSound();
    AddTrayIcon();
    engine_ = std::make_unique<CaptureEngine>(hwnd_);
    engine_->Start(settings_);
    recordingEngine_ = std::make_unique<RecordingEngine>(hwnd_);
    if (!recordingEngine_->Start()) recordingEngine_.reset();
    microphones_ = RecordingEngine::EnumerateMicrophones();
    audioWindows_ = EnumerateAudioWindows(hwnd_);
    recordingOutputs_ = CaptureEngine::EnumerateOutputs();
    RegisterAllHotkeys();
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
    keyboardDevice.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    keyboardDevice.hwndTarget = hwnd_;
    rawInputReady_ = RegisterRawInputDevices(&keyboardDevice, 1, sizeof(keyboardDevice)) != FALSE;
    if (!rawInputReady_) {
        AppendDiagnosticLog(L"Fullscreen hotkey Raw Input registration failed: " + std::to_wstring(GetLastError()));
    }

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
    fontSmall_ = MakeFont(13, FW_NORMAL);
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

void App::ShowSystemNotification(const std::wstring& title, const std::wstring& detail, bool error) {
    if (!trayAdded_) return;
    NOTIFYICONDATAW notification = trayIcon_;
    notification.uFlags = NIF_INFO;
    wcsncpy_s(notification.szInfoTitle, _countof(notification.szInfoTitle), title.c_str(), _TRUNCATE);
    wcsncpy_s(notification.szInfo, _countof(notification.szInfo), detail.c_str(), _TRUNCATE);
    notification.dwInfoFlags = (error ? NIIF_ERROR : NIIF_INFO) | NIIF_NOSOUND | NIIF_LARGE_ICON;
    notification.uTimeout = static_cast<UINT>((std::max)(1000, settings_.overlayDurationMs));
    Shell_NotifyIconW(NIM_MODIFY, &notification);
}

void App::ShowTrayMenu() {
    POINT cursor{};
    GetCursorPos(&cursor);
    std::uint32_t activeReplayMask = 0;
    if (recordingEngine_ && recordingEngine_->IsReplayBuffering() && primaryReplayMonitorIndex_ >= 0 &&
        primaryReplayMonitorIndex_ < static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) {
        activeReplayMask |= 1u << primaryReplayMonitorIndex_;
    }
    for (const auto& channel : auxiliaryReplayEngines_) {
        if (channel.engine && channel.engine->IsReplayBuffering() && channel.monitorIndex >= 0 &&
            channel.monitorIndex < static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) {
            activeReplayMask |= 1u << channel.monitorIndex;
        }
    }
    trayMenu_.Show(cursor, settings_, recordingEngine_ && recordingEngine_->IsRecording(),
                   IsReplayBuffering(), activeReplayMask);
}

void App::ShowMainWindow(Page page) {
    page_ = page;
    if (page != Page::Record) ReleasePreviewFrame();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    InvalidateUi();
}

void App::HideMainWindow() {
    if (engine_) engine_->SetPreviewConsumerActive(false);
    if (recordingEngine_) recordingEngine_->SetPreviewConsumerActive(false);
    ReleasePreviewFrame();
    ShowWindow(hwnd_, SW_HIDE);
}

void App::ExitApplication() {
    if (exiting_) return;
    exiting_ = true;
    StopHotkeyRecorder();
    StopGlobalKeyboardHook();
    UnregisterAllHotkeys();
    StopAuxiliaryReplayEngines();
    if (recordingEngine_) { recordingEngine_->Stop(); recordingEngine_.reset(); }
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
    if (recordingHotkeySlot_ >= 0) return false;
    const auto reg = [this](UINT id, const HotkeyBinding& binding) {
        return RegisterHotKey(hwnd_, id, binding.modifiers | MOD_NOREPEAT, binding.vk) != FALSE;
    };
    // Recording shortcuts are registered first and independently so a PrintScreen conflict
    // cannot disable fullscreen recording/replay controls inside a game.
    const bool record = reg(LUNASHOT_HOTKEY_RECORD, settings_.recordingHotkey);
    const bool replaySave = reg(LUNASHOT_HOTKEY_REPLAY_SAVE, settings_.replaySaveHotkey);
    const bool active = reg(LUNASHOT_HOTKEY_ACTIVE, settings_.activeDisplayHotkey);
    const bool all = reg(LUNASHOT_HOTKEY_ALL, settings_.allDisplaysHotkey);
    const bool region = reg(LUNASHOT_HOTKEY_REGION, settings_.regionHotkey);
    const bool window = reg(LUNASHOT_HOTKEY_WINDOW, settings_.activeWindowHotkey);
    bool monitorHotkeysRegistered = true;
    for (std::size_t index = 0; index < settings_.replayMonitorHotkeys.size(); ++index) {
        const auto& binding = settings_.replayMonitorHotkeys[index];
        if ((settings_.replayMonitorMask & (1u << index)) != 0 && binding.vk != 0) {
            monitorHotkeysRegistered &= reg(LUNASHOT_HOTKEY_REPLAY_MONITOR_BASE + static_cast<UINT>(index), binding);
        }
    }
    const bool allRegistered = active && all && region && window && record && replaySave && monitorHotkeysRegistered;
    const bool fullscreenFallback = rawInputReady_ || globalKeyboardHook_ != nullptr;
    if (!allRegistered) {
        hotkeysRegistered_ = fullscreenFallback || active || all || region || window || record || replaySave;
        engineStatus_ = fullscreenFallback ? L"Fullscreen hotkeys protected" : L"Some hotkeys conflict";
        engineDetail_ = fullscreenFallback
            ? L"Raw Input / keyboard hook will handle shortcuts owned by the game"
            : L"Change unavailable shortcuts; the fullscreen fallback could not start";
        AppendDiagnosticLog(fullscreenFallback
            ? L"One or more RegisterHotKey routes conflicted; fullscreen Raw Input/hook fallback remains active"
            : L"Hotkey registration conflict and no fullscreen fallback is available");
        InvalidateUi();
        return fullscreenFallback;
    }
    hotkeysRegistered_ = true;
    return true;
}

void App::UnregisterAllHotkeys() {
    UnregisterHotKey(hwnd_, LUNASHOT_HOTKEY_ACTIVE);
    UnregisterHotKey(hwnd_, LUNASHOT_HOTKEY_ALL);
    UnregisterHotKey(hwnd_, LUNASHOT_HOTKEY_REGION);
    UnregisterHotKey(hwnd_, LUNASHOT_HOTKEY_WINDOW);
    UnregisterHotKey(hwnd_, LUNASHOT_HOTKEY_RECORD);
    UnregisterHotKey(hwnd_, LUNASHOT_HOTKEY_REPLAY_SAVE);
    for (std::size_t index = 0; index < LUNASHOT_MAX_REPLAY_MONITORS; ++index) {
        UnregisterHotKey(hwnd_, LUNASHOT_HOTKEY_REPLAY_MONITOR_BASE + static_cast<UINT>(index));
    }
    hotkeysRegistered_ = false;
}

bool App::StartGlobalKeyboardHook() {
    if (globalKeyboardHook_) return true;
    globalHotkeyInstance_ = this;
    globalKeyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &App::GlobalLowLevelKeyboardProc, instance_, 0);
    if (!globalKeyboardHook_) {
        globalHotkeyInstance_ = nullptr;
        AppendDiagnosticLog(L"Fullscreen low-level hotkey hook failed: " + std::to_wstring(GetLastError()));
        return false;
    }
    AppendDiagnosticLog(rawInputReady_
        ? L"Fullscreen hotkeys armed: Raw Input background + low-level hook + RegisterHotKey"
        : L"Fullscreen hotkeys armed: low-level hook + RegisterHotKey (Raw Input unavailable)");
    return true;
}

void App::StopGlobalKeyboardHook() {
    if (globalKeyboardHook_) {
        UnhookWindowsHookEx(globalKeyboardHook_);
        globalKeyboardHook_ = nullptr;
    }
    if (globalHotkeyInstance_ == this) globalHotkeyInstance_ = nullptr;
    lowLevelKeysDown_.fill(false);
}

bool App::DispatchGlobalHotkey(UINT vk, UINT modifiers, const wchar_t* source) {
    if (recordingHotkeySlot_ >= 0 || exiting_ || vk == 0 || IsModifierKey(vk)) return false;
    const UINT normalizedModifiers = modifiers & kHotkeyModifierMask;
    const HotkeyBinding bindings[] = {settings_.activeDisplayHotkey, settings_.allDisplaysHotkey,
                                      settings_.regionHotkey, settings_.activeWindowHotkey,
                                      settings_.recordingHotkey, settings_.replaySaveHotkey};
    for (int slot = 0; slot < static_cast<int>(std::size(bindings)); ++slot) {
        if (bindings[slot].vk == vk && (bindings[slot].modifiers & kHotkeyModifierMask) == normalizedModifiers) {
            ExecuteGlobalHotkey(slot, source);
            return true;
        }
    }
    for (std::size_t index = 0; index < settings_.replayMonitorHotkeys.size(); ++index) {
        const auto& binding = settings_.replayMonitorHotkeys[index];
        if ((settings_.replayMonitorMask & (1u << index)) != 0 && binding.vk != 0 && binding.vk == vk &&
            (binding.modifiers & kHotkeyModifierMask) == normalizedModifiers) {
            ExecuteGlobalHotkey(6 + static_cast<int>(index), source);
            return true;
        }
    }
    return false;
}

void App::ExecuteGlobalHotkey(int slot, const wchar_t* source) {
    if (slot < 0 || slot >= static_cast<int>(lastHotkeyTrigger_.size()) || recordingHotkeySlot_ >= 0 || exiting_) return;
    const ULONGLONG now = GetTickCount64();
    if (lastHotkeyTrigger_[static_cast<std::size_t>(slot)] != 0 &&
        now - lastHotkeyTrigger_[static_cast<std::size_t>(slot)] < 250) return;
    lastHotkeyTrigger_[static_cast<std::size_t>(slot)] = now;
    AppendDiagnosticLog(std::wstring(L"Hotkey accepted via ") + (source ? source : L"unknown") +
                        L": " + HotkeySlotName(slot));
    switch (slot) {
        case 0: TriggerCapture(CaptureMode::ActiveDisplay); break;
        case 1: TriggerCapture(CaptureMode::AllDisplays); break;
        case 2: TriggerCapture(CaptureMode::Region); break;
        case 3: TriggerCapture(CaptureMode::ActiveWindow); break;
        case 4: ToggleRecording(); break;
        case 5: SaveAllReplays(); break;
        default:
            if (slot >= 6 && slot < 6 + static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) {
                SaveReplayForMonitor(slot - 6);
            }
            break;
    }
}

bool App::StartHotkeyRecorder() {
    StopHotkeyRecorder();
    hotkeyCapturePosted_ = false;
    if (globalKeyboardHook_) return true;
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

LRESULT CALLBACK App::GlobalLowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    App* app = globalHotkeyInstance_;
    if (code == HC_ACTION && app) {
        const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (key && key->vkCode != VK_PACKET && key->vkCode > 0 && key->vkCode < app->lowLevelKeysDown_.size()) {
            const UINT vk = static_cast<UINT>(key->vkCode);
            const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
            const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
            if (up) {
                app->lowLevelKeysDown_[vk] = false;
            } else if (down && !app->lowLevelKeysDown_[vk]) {
                app->lowLevelKeysDown_[vk] = true;
                UINT modifiers = CurrentModifiers();
                if (key->flags & LLKHF_ALTDOWN) modifiers |= MOD_ALT;
                if (app->recordingHotkeySlot_ >= 0) {
                    app->QueueRecordedHotkey(vk, modifiers);
                } else {
                    PostMessageW(app->hwnd_, WM_APP_GLOBAL_HOTKEY_INPUT,
                                 static_cast<WPARAM>(vk), static_cast<LPARAM>(modifiers));
                }
            }
        }
    }
    return CallNextHookEx(app ? app->globalKeyboardHook_ : nullptr, code, wParam, lParam);
}

void App::HandleRawKeyboardInput(HRAWINPUT rawInput) {
    RAWINPUT input{};
    UINT size = sizeof(input);
    if (GetRawInputData(rawInput, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) ||
        input.header.dwType != RIM_TYPEKEYBOARD) return;
    const RAWKEYBOARD& keyboard = input.data.keyboard;
    const UINT vk = NormalizeRawVirtualKey(keyboard);
    if (vk == 0 || vk >= rawKeysDown_.size()) return;
    const bool up = (keyboard.Flags & RI_KEY_BREAK) != 0 ||
                    keyboard.Message == WM_KEYUP || keyboard.Message == WM_SYSKEYUP;
    if (up) {
        rawKeysDown_[vk] = false;
        return;
    }
    if (keyboard.Message != WM_KEYDOWN && keyboard.Message != WM_SYSKEYDOWN) return;
    if (rawKeysDown_[vk]) return;
    rawKeysDown_[vk] = true;
    if (recordingHotkeySlot_ >= 0) QueueRecordedHotkey(vk, CurrentModifiers());
    else DispatchGlobalHotkey(vk, CurrentModifiers(), L"Raw Input background");
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
    engineDetail_ = engineReady_ ? L"Six fullscreen-safe capture and recording shortcuts are active" : L"Preparing DXGI";
    InvalidateUi();
}

void App::FinishHotkeyRecording(UINT vk, UINT modifiers) {
    if (recordingHotkeySlot_ < 0 || IsModifierKey(vk)) return;
    StopHotkeyRecorder();
    HotkeyBinding binding{vk, modifiers | MOD_NOREPEAT};
    HotkeyBinding* target = nullptr;
    if (recordingHotkeySlot_ == 0) target = &settings_.activeDisplayHotkey;
    else if (recordingHotkeySlot_ == 1) target = &settings_.allDisplaysHotkey;
    else if (recordingHotkeySlot_ == 2) target = &settings_.regionHotkey;
    else if (recordingHotkeySlot_ == 3) target = &settings_.activeWindowHotkey;
    else if (recordingHotkeySlot_ == 4) target = &settings_.recordingHotkey;
    else if (recordingHotkeySlot_ == 5) target = &settings_.replaySaveHotkey;
    else if (recordingHotkeySlot_ >= 6 &&
             recordingHotkeySlot_ < 6 + static_cast<int>(settings_.replayMonitorHotkeys.size())) {
        target = &settings_.replayMonitorHotkeys[static_cast<std::size_t>(recordingHotkeySlot_ - 6)];
    }
    if (!target) {
        recordingHotkeySlot_ = -1;
        RegisterAllHotkeys();
        return;
    }
    const HotkeyBinding previous = *target;
    *target = binding;
    const auto same = [](const HotkeyBinding& a, const HotkeyBinding& b) {
        return a.vk == b.vk && (a.modifiers | MOD_NOREPEAT) == (b.modifiers | MOD_NOREPEAT);
    };
    std::vector<HotkeyBinding> bindings{settings_.activeDisplayHotkey, settings_.allDisplaysHotkey,
                                        settings_.regionHotkey, settings_.activeWindowHotkey,
                                        settings_.recordingHotkey, settings_.replaySaveHotkey};
    for (std::size_t index = 0; index < settings_.replayMonitorHotkeys.size(); ++index) {
        if ((settings_.replayMonitorMask & (1u << index)) != 0 && settings_.replayMonitorHotkeys[index].vk != 0) {
            bindings.push_back(settings_.replayMonitorHotkeys[index]);
        }
    }
    bool duplicate = false;
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        for (std::size_t j = i + 1; j < bindings.size(); ++j) duplicate |= same(bindings[i], bindings[j]);
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

void App::LoadNotificationSound() {
    if (!settings_.notificationSoundPath.empty() && soundPlayer_.LoadFile(settings_.notificationSoundPath)) return;
    settings_.notificationSoundPath.clear();
    soundPlayer_.Initialize(instance_, IDR_CAPTURE_SOUND);
}

AppSettings App::BuildSessionSettings() {
    AppSettings session = settings_;
    const HWND foreground = GetForegroundWindow();
    if (session.recordingAutoMonitor) {
        HMONITOR monitor = nullptr;
        if (foreground && foreground != hwnd_) monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL);
        if (!monitor) monitor = GetPriorityMonitor(hwnd_);
        if (recordingOutputs_.empty()) recordingOutputs_ = CaptureEngine::EnumerateOutputs();
        for (const auto& output : recordingOutputs_) {
            if (output.monitor == monitor) { session.recordingMonitorIndex = output.linearIndex; break; }
        }
    }
    if (session.recordApplicationAudio) {
        session.recordingProcessId = 0;
        audioWindows_ = EnumerateAudioWindows(hwnd_);
        if (!session.recordingApplicationName.empty()) {
            for (const auto& window : audioWindows_) {
                if (_wcsicmp(window.executableName.c_str(), session.recordingApplicationName.c_str()) == 0) {
                    session.recordingProcessId = window.processId;
                    break;
                }
            }
        }
        if (!session.recordingProcessId && foreground && foreground != hwnd_) {
            GetWindowThreadProcessId(foreground, &session.recordingProcessId);
        }
    }
    return session;
}

void App::RefreshAudioSources() {
    microphones_ = RecordingEngine::EnumerateMicrophones();
    audioWindows_ = EnumerateAudioWindows(hwnd_);
    recordingStatus_ = L"Audio sources refreshed";
    recordingDetail_ = std::to_wstring(microphones_.size()) + L" microphones · " +
                       std::to_wstring(audioWindows_.size()) + L" application windows";
}

void App::CycleApplicationTarget() {
    audioWindows_ = EnumerateAudioWindows(hwnd_);
    if (audioWindows_.empty()) {
        settings_.recordingApplicationName.clear();
        recordingDetail_ = L"Open the game/application, then refresh audio sources";
        return;
    }
    std::vector<std::wstring> labels;
    int selected = -1;
    for (std::size_t i = 0; i < audioWindows_.size(); ++i) {
        labels.push_back(audioWindows_[i].title + L"  ·  " + audioWindows_[i].executableName);
        if (_wcsicmp(audioWindows_[i].executableName.c_str(), settings_.recordingApplicationName.c_str()) == 0) {
            selected = static_cast<int>(i);
        }
    }
    const int choice = ChooseNamedItem(hwnd_, labels, selected, L"Choose application audio");
    if (choice >= 0) settings_.recordingApplicationName = audioWindows_[static_cast<std::size_t>(choice)].executableName;
}

void App::AddMicrophoneInput() {
    if (settings_.microphoneInputs.size() >= 4) return;
    if (microphones_.empty()) microphones_ = RecordingEngine::EnumerateMicrophones();
    if (microphones_.empty()) {
        recordingDetail_ = L"Windows did not return an active microphone";
        return;
    }
    std::vector<std::wstring> labels;
    for (const auto& microphone : microphones_) {
        labels.push_back((microphone.isDefault ? L"Default · " : L"") + microphone.name);
    }
    const int choice = ChooseNamedItem(hwnd_, labels, -1, L"Add microphone input");
    if (choice >= 0) {
        const auto& selected = microphones_[static_cast<std::size_t>(choice)];
        settings_.microphoneInputs.push_back({selected.id, selected.name, 100, true});
    }
}

void App::ExecuteMicrophoneAction(Action action) {
    constexpr Action toggles[] = {Action::Mic0Toggle, Action::Mic1Toggle, Action::Mic2Toggle, Action::Mic3Toggle};
    constexpr Action devices[] = {Action::Mic0Device, Action::Mic1Device, Action::Mic2Device, Action::Mic3Device};
    constexpr Action volumes[] = {Action::Mic0Volume, Action::Mic1Volume, Action::Mic2Volume, Action::Mic3Volume};
    constexpr Action removes[] = {Action::Mic0Remove, Action::Mic1Remove, Action::Mic2Remove, Action::Mic3Remove};
    for (std::size_t slot = 0; slot < 4; ++slot) {
        if (action == removes[slot]) {
            if (slot < settings_.microphoneInputs.size()) settings_.microphoneInputs.erase(settings_.microphoneInputs.begin() + slot);
            return;
        }
        if (slot >= settings_.microphoneInputs.size()) continue;
        auto& input = settings_.microphoneInputs[slot];
        if (action == toggles[slot]) { input.enabled = !input.enabled; return; }
        if (action == volumes[slot]) {
            int value = input.volumePercent;
            const std::vector<IntegerPreset> presets{{0, L"Muted · 0%"}, {50, L"Quiet · 50%"},
                {75, L"Reduced · 75%"}, {100, L"Original level · 100%"},
                {125, L"Boost · 125%"}, {150, L"Strong boost · 150%"}, {200, L"Maximum · 200%"}};
            if (ChooseIntegerPreset(hwnd_, L"Microphone gain", input.name, presets, value, 0, 200, value)) {
                input.volumePercent = value;
            }
            return;
        }
        if (action == devices[slot]) {
            if (microphones_.empty()) microphones_ = RecordingEngine::EnumerateMicrophones();
            if (microphones_.empty()) return;
            std::vector<std::wstring> labels;
            int selected = -1;
            for (std::size_t i = 0; i < microphones_.size(); ++i) {
                labels.push_back((microphones_[i].isDefault ? L"Default · " : L"") + microphones_[i].name);
                if (microphones_[i].id == input.deviceId) selected = static_cast<int>(i);
            }
            const int choice = ChooseNamedItem(hwnd_, labels, selected, L"Choose microphone device");
            if (choice >= 0) {
                input.deviceId = microphones_[static_cast<std::size_t>(choice)].id;
                input.name = microphones_[static_cast<std::size_t>(choice)].name;
            }
            return;
        }
    }
}

void App::ToggleRecording() {
    if (!recordingEngine_) {
        recordingStatus_ = L"Recorder unavailable";
        recordingDetail_ = L"The recording worker could not start";
        return;
    }
    if (recordingEngine_->IsRecording()) {
        recordingStatus_ = L"Finalizing recording…";
        recordingEngine_->StopActiveSession();
    } else if (IsReplayBuffering()) {
        recordingStatus_ = L"Replay buffer is already active";
        recordingDetail_ = L"Stop replay before starting a normal recording";
    } else if (recordingEngine_->IsBusy()) {
        recordingStatus_ = L"Recorder is changing state";
        recordingDetail_ = L"Wait for the current start or finalize operation";
    } else {
        audioWarning_.clear();
        SuspendCaptureForRecording();
        AppSettings session = BuildSessionSettings();
        activeRecordingMonitorIndex_ = session.recordingMonitorIndex;
        if (!recordingEngine_->StartRecording(session)) {
            recordingStatus_ = L"Recorder is busy";
            activeRecordingMonitorIndex_ = -1;
            ResumeCaptureAfterRecording();
        }
    }
    InvalidateUi();
}

void App::ToggleReplayBuffer() {
    if (!recordingEngine_) return;
    if (IsReplayBuffering()) {
        recordingStatus_ = L"Stopping replay buffers…";
        StopAuxiliaryReplayEngines();
        recordingEngine_->StopActiveSession();
    } else if (recordingEngine_->IsRecording()) {
        recordingStatus_ = L"A recording is already active";
        recordingDetail_ = L"Stop the recording before enabling replay";
    } else if (recordingEngine_->IsBusy()) {
        recordingStatus_ = L"Recorder is changing state";
        recordingDetail_ = L"Wait for the current start or finalize operation";
    } else {
        audioWarning_.clear();
        SuspendCaptureForRecording();
        AppSettings primarySettings = BuildSessionSettings();
        std::vector<int> monitors;
        if (settings_.replayMonitorMask != 0) {
            recordingOutputs_ = CaptureEngine::EnumerateOutputs();
            for (const auto& output : recordingOutputs_) {
                if (output.linearIndex >= 0 && output.linearIndex < static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS) &&
                    (settings_.replayMonitorMask & (1u << output.linearIndex)) != 0) {
                    monitors.push_back(output.linearIndex);
                }
            }
        }
        if (monitors.empty()) monitors.push_back(primarySettings.recordingMonitorIndex);
        primaryReplayMonitorIndex_ = monitors.front();
        primarySettings.recordingAutoMonitor = false;
        primarySettings.recordingMonitorIndex = primaryReplayMonitorIndex_;
        if (!recordingEngine_->StartReplayBuffer(primarySettings)) {
            recordingStatus_ = L"Recorder is busy";
            primaryReplayMonitorIndex_ = -1;
            ResumeCaptureAfterRecording();
        } else {
            for (std::size_t index = 1; index < monitors.size(); ++index) {
                AppSettings channelSettings = primarySettings;
                channelSettings.recordingMonitorIndex = monitors[index];
                channelSettings.previewEnabled = false;
                auto engine = std::make_unique<RecordingEngine>(hwnd_, monitors[index] + 1);
                if (!engine->Start() || !engine->StartReplayBuffer(channelSettings)) {
                    engine->Stop();
                    recordingDetail_ = L"One monitor channel could not start; the remaining buffers stay active";
                    continue;
                }
                auxiliaryReplayEngines_.push_back({monitors[index], std::move(engine)});
            }
            recordingStatus_ = monitors.size() > 1 ? L"Starting multi-monitor replay…" : L"Starting replay buffer…";
            recordingDetail_ = std::to_wstring(1 + auxiliaryReplayEngines_.size()) + L" encoded monitor channel(s)";
        }
    }
    InvalidateUi();
}

bool App::IsReplayBuffering() const {
    if (recordingEngine_ && recordingEngine_->IsReplayBuffering()) return true;
    for (const auto& channel : auxiliaryReplayEngines_) {
        if (channel.engine && channel.engine->IsReplayBuffering()) return true;
    }
    return false;
}

void App::StopAuxiliaryReplayEngines() {
    for (auto& channel : auxiliaryReplayEngines_) {
        if (channel.engine) channel.engine->Stop();
    }
    auxiliaryReplayEngines_.clear();
}

void App::SaveAllReplays() {
    if (recordingEngine_ && recordingEngine_->IsReplayBuffering()) recordingEngine_->SaveReplay();
    for (auto& channel : auxiliaryReplayEngines_) {
        if (channel.engine && channel.engine->IsReplayBuffering()) channel.engine->SaveReplay();
    }
}

void App::SaveReplayForMonitor(int monitorIndex) {
    if (monitorIndex == primaryReplayMonitorIndex_ && recordingEngine_ && recordingEngine_->IsReplayBuffering()) {
        recordingEngine_->SaveReplay();
        return;
    }
    for (auto& channel : auxiliaryReplayEngines_) {
        if (channel.monitorIndex == monitorIndex && channel.engine && channel.engine->IsReplayBuffering()) {
            channel.engine->SaveReplay();
            return;
        }
    }
    recordingStatus_ = L"Monitor replay is not active";
    recordingDetail_ = L"Enable that monitor in REPLAY SOURCES and restart the buffer";
    InvalidateUi();
}

void App::OpenReplayControlCenter() {
    recordingOutputs_ = CaptureEngine::EnumerateOutputs();
    int configured = 0;
    int active = 0;
    for (std::size_t index = 0; index < LUNASHOT_MAX_REPLAY_MONITORS; ++index) {
        if ((settings_.replayMonitorMask & (1u << index)) != 0) ++configured;
    }
    if (recordingEngine_ && recordingEngine_->IsReplayBuffering()) ++active;
    for (const auto& channel : auxiliaryReplayEngines_) {
        if (channel.engine && channel.engine->IsReplayBuffering()) ++active;
    }
    const bool live = active > 0;
    const std::wstring sourceSummary = settings_.replayMonitorMask == 0
        ? L"Auto foreground monitor" : std::to_wstring(configured) + L" selected monitor(s)";
    const std::vector<std::wstring> actions{
        live ? L"Stop Replay Buffer  ·  " + std::to_wstring(active) + L" channel(s) live"
             : L"Start Replay Buffer  ·  " + sourceSummary,
        live ? L"Save ALL active monitors now  ·  " + FormatHotkey(settings_.replaySaveHotkey.modifiers, settings_.replaySaveHotkey.vk)
             : L"Save ALL monitors  ·  start Replay Buffer first",
        live ? L"Save one specific monitor…  ·  choose a live channel"
             : L"Save one specific monitor…  ·  no live channels",
        L"Replay sources…  ·  " + sourceSummary,
        L"Shortcut matrix…  ·  Save All + independent monitor binds",
        L"Open replay recordings folder",
    };
    const int choice = ChooseNamedItem(hwnd_, actions, 0, L"Replay control center",
                                       L"One place for monitor groups, one-monitor saves and fullscreen shortcuts");
    switch (choice) {
        case 0: ToggleReplayBuffer(); break;
        case 1:
            if (live) SaveAllReplays();
            else { recordingStatus_ = L"Replay buffer is off"; recordingDetail_ = L"Start it before saving past moments"; }
            break;
        case 2: SaveReplayFromChooser(); break;
        case 3: ConfigureReplaySources(); break;
        case 4: ConfigureReplayHotkey(); break;
        case 5: OpenFolderInExplorer(settings_.recordingDirectory); break;
        default: break;
    }
    InvalidateUi();
}

void App::SaveReplayFromChooser() {
    struct LiveChannel { int monitorIndex = -1; };
    std::vector<LiveChannel> channels;
    if (recordingEngine_ && recordingEngine_->IsReplayBuffering() && primaryReplayMonitorIndex_ >= 0 &&
        primaryReplayMonitorIndex_ < static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) {
        channels.push_back({primaryReplayMonitorIndex_});
    }
    for (const auto& channel : auxiliaryReplayEngines_) {
        if (channel.engine && channel.engine->IsReplayBuffering() && channel.monitorIndex >= 0 &&
            channel.monitorIndex < static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) channels.push_back({channel.monitorIndex});
    }
    if (channels.empty()) {
        recordingStatus_ = L"No live replay channels";
        recordingDetail_ = L"Start Replay Buffer, then choose one monitor or Save All";
        return;
    }
    recordingOutputs_ = CaptureEngine::EnumerateOutputs();
    std::vector<std::wstring> labels;
    for (const auto& channel : channels) {
        std::wstring display = L"Monitor " + std::to_wstring(channel.monitorIndex + 1);
        for (const auto& output : recordingOutputs_) {
            if (output.linearIndex == channel.monitorIndex) {
                display += L"  ·  " + output.name + L"  ·  " + std::to_wstring(output.width) + L"×" + std::to_wstring(output.height);
                break;
            }
        }
        const auto& binding = settings_.replayMonitorHotkeys[static_cast<std::size_t>(channel.monitorIndex)];
        display += binding.vk ? L"  ·  " + FormatHotkey(binding.modifiers, binding.vk) : L"  ·  no individual bind";
        labels.push_back(std::move(display));
    }
    const int choice = ChooseNamedItem(hwnd_, labels, 0, L"Save one replay monitor",
                                       L"Only the selected monitor is exported; every rolling buffer keeps running");
    if (choice >= 0) SaveReplayForMonitor(channels[static_cast<std::size_t>(choice)].monitorIndex);
}

void App::ConfigureReplaySources() {
    if (!GuardSessionSettingChange(L"Replay monitor sources")) return;
    recordingOutputs_ = CaptureEngine::EnumerateOutputs();
    if (recordingOutputs_.empty()) {
        recordingStatus_ = L"No monitors detected";
        recordingDetail_ = L"Windows returned no attached desktop outputs";
        return;
    }
    std::uint32_t allMask = 0;
    std::uint32_t primaryMask = 0;
    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    for (const auto& output : recordingOutputs_) {
        if (output.linearIndex < 0 || output.linearIndex >= static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) continue;
        allMask |= 1u << output.linearIndex;
        if (output.monitor == primary) primaryMask = 1u << output.linearIndex;
    }
    if (primaryMask == 0 && !recordingOutputs_.empty()) primaryMask = 1u << recordingOutputs_.front().linearIndex;
    int current = settings_.replayMonitorMask == 0 ? 0 : settings_.replayMonitorMask == primaryMask ? 1 :
                  settings_.replayMonitorMask == allMask ? 2 : 3;
    const std::vector<std::wstring> profiles{
        L"Auto foreground  ·  buffer the monitor containing the active game",
        L"Primary only  ·  minimum GPU/disk load",
        L"All connected monitors  ·  every display gets an independent rolling file",
        L"Custom monitor group…  ·  toggle exactly the displays you want",
    };
    const int choice = ChooseNamedItem(hwnd_, profiles, current, L"Replay source profile",
                                       L"Save All exports the whole group; monitor commands export only one channel");
    if (choice < 0) return;
    if (choice == 3) {
        ConfigureReplayMonitors();
        return;
    }
    settings_.replayMonitorMask = choice == 0 ? 0 : choice == 1 ? primaryMask : allMask;
    SaveSettings();
    RegisterAllHotkeys();
    recordingStatus_ = L"Replay source profile updated";
    recordingDetail_ = choice == 0 ? L"The foreground game monitor will be selected when replay starts" :
                       choice == 1 ? L"Only the primary monitor will be buffered" :
                                     L"Every connected monitor will use an independent rolling cache";
}

void App::SetReplaySourceProfile(int profile) {
    if (!GuardSessionSettingChange(L"Replay monitor sources")) return;
    recordingOutputs_ = CaptureEngine::EnumerateOutputs();
    if (recordingOutputs_.empty()) return;
    std::uint32_t mask = 0;
    if (profile == 1) {
        const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        for (const auto& output : recordingOutputs_) {
            if (output.monitor == primary && output.linearIndex >= 0 &&
                output.linearIndex < static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) {
                mask = 1u << output.linearIndex;
                break;
            }
        }
        if (mask == 0 && recordingOutputs_.front().linearIndex < static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) {
            mask = 1u << recordingOutputs_.front().linearIndex;
        }
    } else if (profile == 2) {
        for (const auto& output : recordingOutputs_) {
            if (output.linearIndex >= 0 && output.linearIndex < static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) {
                mask |= 1u << output.linearIndex;
            }
        }
    }
    settings_.replayMonitorMask = mask;
    SaveSettings();
    RegisterAllHotkeys();
    recordingStatus_ = L"Replay source profile updated";
    recordingDetail_ = profile == 0 ? L"AUTO will follow the foreground game when buffering starts" :
                       profile == 1 ? L"Only the Windows primary monitor is armed" :
                                      L"Every connected monitor is armed as an independent channel";
}

void App::ToggleReplayMonitorSource(int monitorIndex) {
    if (monitorIndex < 0 || monitorIndex >= static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS) ||
        !GuardSessionSettingChange(L"Replay monitor sources")) return;
    const std::uint32_t bit = 1u << monitorIndex;
    settings_.replayMonitorMask = settings_.replayMonitorMask == 0 ? bit : settings_.replayMonitorMask ^ bit;
    SaveSettings();
    RegisterAllHotkeys();
    recordingStatus_ = (settings_.replayMonitorMask & bit) != 0 ? L"Monitor armed for replay" : L"Monitor removed from replay";
    recordingDetail_ = L"Monitor " + std::to_wstring(monitorIndex + 1) +
                       (settings_.replayMonitorMask == 0 ? L" removed · profile returned to AUTO" : L" source group updated");
}

void App::ConfigureReplayMonitors() {
    if (!GuardSessionSettingChange(L"Replay monitor sources")) return;
    recordingOutputs_ = CaptureEngine::EnumerateOutputs();
    std::vector<std::wstring> labels;
    std::uint32_t selectionMask = 0;
    for (std::size_t index = 0; index < recordingOutputs_.size() && index < LUNASHOT_MAX_REPLAY_MONITORS; ++index) {
        const auto& output = recordingOutputs_[index];
        labels.push_back(L"Monitor " + std::to_wstring(output.linearIndex + 1) + L" · " + output.name + L" · " +
                         std::to_wstring(output.width) + L" × " + std::to_wstring(output.height));
        if ((settings_.replayMonitorMask & (1u << output.linearIndex)) != 0) selectionMask |= 1u << index;
    }
    std::uint32_t selected = selectionMask;
    if (!ChooseMultipleNamedItems(hwnd_, labels, selectionMask, L"Replay monitor sources",
                                  L"Select several monitors; no selection means automatic foreground monitor", selected)) return;
    std::uint32_t monitorMask = 0;
    for (std::size_t index = 0; index < recordingOutputs_.size() && index < LUNASHOT_MAX_REPLAY_MONITORS; ++index) {
        if ((selected & (1u << index)) != 0 && recordingOutputs_[index].linearIndex < 32) {
            monitorMask |= 1u << recordingOutputs_[index].linearIndex;
        }
    }
    settings_.replayMonitorMask = monitorMask & 0xFFu;
    SaveSettings();
    RegisterAllHotkeys();
}

void App::ConfigureReplayHotkey() {
    if (settings_.replayMonitorMask == 0) {
        BeginHotkeyRecording(5);
        return;
    }
    recordingOutputs_ = CaptureEngine::EnumerateOutputs();
    std::vector<std::wstring> labels{L"All active replay monitors · " +
                                     FormatHotkey(settings_.replaySaveHotkey.modifiers, settings_.replaySaveHotkey.vk)};
    std::vector<int> monitorIndices{-1};
    for (const auto& output : recordingOutputs_) {
        if (output.linearIndex >= 0 && output.linearIndex < static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS) &&
            (settings_.replayMonitorMask & (1u << output.linearIndex)) != 0) {
            const auto& binding = settings_.replayMonitorHotkeys[static_cast<std::size_t>(output.linearIndex)];
            const std::wstring key = binding.vk ? FormatHotkey(binding.modifiers, binding.vk) : L"Not assigned";
            labels.push_back(L"Monitor " + std::to_wstring(output.linearIndex + 1) + L" · " + key);
            monitorIndices.push_back(output.linearIndex);
        }
    }
    labels.push_back(L"Quick layout  ·  assign Ctrl + Alt + 1…8 to selected monitors");
    labels.push_back(L"Clear individual monitor shortcuts  ·  keep Save All only");
    const std::size_t quickLayoutIndex = labels.size() - 2;
    const std::size_t clearIndex = labels.size() - 1;
    const int choice = ChooseNamedItem(hwnd_, labels, 0, L"Replay shortcut matrix",
                                       L"Choose All or one monitor, then press the new shortcut");
    if (choice < 0) return;
    if (static_cast<std::size_t>(choice) == quickLayoutIndex) {
        const HotkeyBinding protectedBindings[] = {settings_.activeDisplayHotkey, settings_.allDisplaysHotkey,
            settings_.regionHotkey, settings_.activeWindowHotkey, settings_.recordingHotkey, settings_.replaySaveHotkey};
        bool conflict = false;
        for (const int monitorIndex : monitorIndices) {
            if (monitorIndex < 0) continue;
            const HotkeyBinding candidate{static_cast<UINT>(L'1' + monitorIndex), MOD_CONTROL | MOD_ALT | MOD_NOREPEAT};
            for (const auto& binding : protectedBindings) {
                if (binding.vk == candidate.vk && (binding.modifiers & kHotkeyModifierMask) ==
                    (candidate.modifiers & kHotkeyModifierMask)) conflict = true;
            }
        }
        if (conflict) {
            engineStatus_ = L"Quick replay layout conflicts";
            engineDetail_ = L"Ctrl + Alt + a monitor number is already used; assign those monitor binds manually";
            return;
        }
        for (const int monitorIndex : monitorIndices) {
            if (monitorIndex >= 0) settings_.replayMonitorHotkeys[static_cast<std::size_t>(monitorIndex)] =
                HotkeyBinding{static_cast<UINT>(L'1' + monitorIndex), MOD_CONTROL | MOD_ALT | MOD_NOREPEAT};
        }
        SaveSettings();
        RegisterAllHotkeys();
        engineStatus_ = L"Replay shortcut matrix ready";
        engineDetail_ = L"Ctrl + Alt + monitor number saves only that monitor; Save All keeps its existing bind";
        return;
    }
    if (static_cast<std::size_t>(choice) == clearIndex) {
        for (const int monitorIndex : monitorIndices) {
            if (monitorIndex >= 0) settings_.replayMonitorHotkeys[static_cast<std::size_t>(monitorIndex)] = HotkeyBinding{};
        }
        SaveSettings();
        RegisterAllHotkeys();
        engineStatus_ = L"Individual replay shortcuts cleared";
        engineDetail_ = L"The Save All replay shortcut remains active";
        return;
    }
    if (monitorIndices[static_cast<std::size_t>(choice)] < 0) BeginHotkeyRecording(5);
    else BeginHotkeyRecording(6 + monitorIndices[static_cast<std::size_t>(choice)]);
}

void App::TriggerCapture(CaptureMode mode) {
    if (captureSuspendedForRecording_) {
        BeginRecorderFrameCapture(mode);
        InvalidateUi();
        return;
    }
    if (!engine_) return;
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

RecordingEngine* App::RecorderForMonitor(HMONITOR monitor) {
    int monitorIndex = -1;
    if (recordingOutputs_.empty()) recordingOutputs_ = CaptureEngine::EnumerateOutputs();
    for (const auto& output : recordingOutputs_) {
        if (output.monitor == monitor) { monitorIndex = output.linearIndex; break; }
    }
    const int primaryIndex = recordingEngine_ && recordingEngine_->IsRecording()
        ? activeRecordingMonitorIndex_ : primaryReplayMonitorIndex_;
    if (monitorIndex == primaryIndex && recordingEngine_ &&
        (recordingEngine_->IsRecording() || recordingEngine_->IsReplayBuffering())) return recordingEngine_.get();
    for (auto& channel : auxiliaryReplayEngines_) {
        if (channel.monitorIndex == monitorIndex && channel.engine && channel.engine->IsReplayBuffering()) {
            return channel.engine.get();
        }
    }
    return nullptr;
}

void App::BeginRecorderFrameCapture(CaptureMode mode) {
    if (!engine_ || recorderCaptureBatch_) {
        engineStatus_ = L"Capture is already preparing";
        engineDetail_ = L"Wait for the current live-frame snapshot";
        return;
    }
    recordingOutputs_ = CaptureEngine::EnumerateOutputs();
    if (mode == CaptureMode::ActiveDisplay) {
        const HMONITOR monitor = GetPriorityMonitor(hwnd_);
        if (auto* recorder = RecorderForMonitor(monitor); recorder && recorder->CaptureScreenshot(settings_)) {
            engineStatus_ = L"Fullscreen screenshot queued";
            engineDetail_ = L"Copied from the live recorder frame; replay continues";
            return;
        }
        for (const auto& output : recordingOutputs_) {
            if (output.monitor != monitor) continue;
            auto frame = CaptureDesktopGdi(output.desktopRect);
            if (frame) {
                if (settings_.sound) soundPlayer_.Play();
                if (settings_.notifications) overlay_.ShowMessage(L"Screenshot captured", L"Saving without interrupting replay",
                                                                   settings_.overlayDurationMs, monitor);
                engine_->SaveEditedFrame(std::move(*frame), settings_, false, L"", L"Screenshot");
                engineStatus_ = L"Screenshot captured";
                engineDetail_ = L"Fallback desktop frame queued for saving";
                return;
            }
        }
        engineStatus_ = L"Screenshot unavailable";
        engineDetail_ = L"The requested monitor has no replay or desktop frame";
        return;
    }

    auto batch = std::make_unique<RecorderCaptureBatch>();
    batch->requestId = nextRecorderCaptureRequestId_++;
    if (batch->requestId == 0) batch->requestId = nextRecorderCaptureRequestId_++;
    batch->mode = mode;
    for (const auto& output : recordingOutputs_) {
        if (auto frame = CaptureDesktopGdi(output.desktopRect)) batch->frames.push_back(std::move(frame));
    }
    if (recordingEngine_ && (recordingEngine_->IsRecording() || recordingEngine_->IsReplayBuffering()) &&
        recordingEngine_->RequestFrame(mode, batch->requestId, false)) {
        ++batch->pendingFrames;
    }
    for (auto& channel : auxiliaryReplayEngines_) {
        if (channel.engine && channel.engine->IsReplayBuffering() &&
            channel.engine->RequestFrame(mode, batch->requestId, false)) {
            ++batch->pendingFrames;
        }
    }
    recorderCaptureBatch_ = std::move(batch);
    engineStatus_ = mode == CaptureMode::AllDisplays ? L"Capturing all displays" :
                    mode == CaptureMode::ActiveWindow ? L"Preparing window picker" : L"Preparing region editor";
    engineDetail_ = L"Replay remains active while live frames are frozen";
    if (recorderCaptureBatch_->pendingFrames == 0) CompleteRecorderFrameCapture();
}

void App::HandleRecorderFrame(std::unique_ptr<EngineEvent> event) {
    if (!event || !recorderCaptureBatch_ || event->requestId != recorderCaptureBatch_->requestId) return;
    if (event->frame && !event->frame->bgra.empty()) recorderCaptureBatch_->frames.push_back(std::move(event->frame));
    if (recorderCaptureBatch_->pendingFrames > 0) --recorderCaptureBatch_->pendingFrames;
    if (recorderCaptureBatch_->pendingFrames == 0) CompleteRecorderFrameCapture();
}

void App::CompleteRecorderFrameCapture() {
    if (!recorderCaptureBatch_) return;
    auto batch = std::move(recorderCaptureBatch_);
    auto combined = CombineDesktopFrames(batch->frames);
    if (!combined) {
        engineStatus_ = L"Capture unavailable";
        engineDetail_ = L"No replay or fallback desktop frame was ready";
        InvalidateUi();
        return;
    }
    if (batch->mode == CaptureMode::AllDisplays) {
        if (settings_.sound) soundPlayer_.Play();
        if (settings_.notifications) overlay_.ShowMessage(L"All displays captured", L"Saving while replay continues",
                                                           settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
        engine_->SaveEditedFrame(std::move(*combined), settings_, false, L"", L"Screenshot");
        engineStatus_ = L"All displays captured";
        engineDetail_ = L"Live replay frames stitched and queued for saving";
    } else {
        auto event = std::make_unique<EngineEvent>();
        event->type = EngineEventType::RegionFrame;
        event->mode = batch->mode;
        event->filenamePrefix = L"Screenshot";
        event->frame = std::move(combined);
        HandleEngineEvent(std::move(event));
        return;
    }
    InvalidateUi();
}

void App::HandleEngineEvent(std::unique_ptr<EngineEvent> event) {
    if (!event) return;
    if (event->type == EngineEventType::RecorderFrame) {
        HandleRecorderFrame(std::move(event));
        return;
    }
    switch (event->type) {
        case EngineEventType::Ready:
            engineReady_ = true;
            engineStatus_ = L"Ready";
            engineDetail_ = L"Capture and recording shortcuts are ready";
            RegisterAllHotkeys();
            break;
        case EngineEventType::Reconfiguring:
            engineReady_ = false;
            engineStatus_ = L"Reconnecting";
            engineDetail_ = event->detail;
            break;
        case EngineEventType::Feedback: {
            lastCapturePath_ = event->path;
            lastResponseMs_ = event->responseMs;
            if (settings_.sound) soundPlayer_.Play();
            if (settings_.notifications) {
                const std::wstring filename = std::filesystem::path(event->path).filename().wstring();
                overlay_.ShowSuccess(filename, event->responseMs, settings_.overlayDurationMs, event->monitor);
                if (overlay_.IsFullscreenPresentation()) {
                    ShowSystemNotification(L"Screenshot saved", filename);
                    AppendDiagnosticLog(L"Fullscreen screenshot notification raised through overlay + Windows tray fallback");
                }
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
        case EngineEventType::RecorderFrame: break;
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

void App::HandleRecordingEvent(std::unique_ptr<RecordingEvent> event) {
    if (!event) return;
    if (event->channelId > 0) {
        const std::wstring monitor = L"Monitor " + std::to_wstring(event->channelId);
        if (event->type == RecordingEventType::ReplaySaved) {
            lastRecordingPath_ = event->path;
            recordingStatus_ = monitor + L" replay saved";
            recordingDetail_ = PathFileName(event->path);
            if (settings_.notifications) overlay_.ShowMessage(monitor + L" replay saved", PathFileName(event->path),
                                                               settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
        } else if (event->type == RecordingEventType::Error) {
            recordingStatus_ = monitor + L" replay error";
            recordingDetail_ = event->detail;
            AppendDiagnosticLog(monitor + L" replay channel: " + event->detail);
        } else if (event->type == RecordingEventType::AudioWarning) {
            AppendDiagnosticLog(monitor + L" audio: " + event->detail);
        }
        InvalidateUi();
        return;
    }
    recordingElapsedMs_ = event->elapsedMs;
    recordingFrames_ = event->framesWritten;
    recordingDroppedFrames_ = event->droppedFrames;
    recordingEncodedFps_ = event->encodedFps;
    switch (event->type) {
        case RecordingEventType::Starting:
            recordingStatus_ = event->detail;
            recordingDetail_ = std::wstring(L"Opening DXGI, ") + VideoCodecLabel(settings_.videoCodec) + L" encoder and audio loopback";
            break;
        case RecordingEventType::RecordingStarted:
            recordingStatus_ = L"Recording live";
            recordingDetail_ = event->detail + L" · " + VideoCodecLabel(settings_.videoCodec) + L" " +
                               std::to_wstring(settings_.recordingFps) + L" FPS";
            lastRecordingPath_ = event->path;
            if (settings_.sound) soundPlayer_.Play();
            break;
        case RecordingEventType::ReplayStarted:
            recordingStatus_ = auxiliaryReplayEngines_.empty() ? L"Replay buffer live" : L"Multi-monitor replay live";
            recordingDetail_ = std::to_wstring(1 + auxiliaryReplayEngines_.size()) + L" monitor channel(s) · last " +
                               std::to_wstring(settings_.replaySeconds) + L" seconds";
            if (settings_.sound) soundPlayer_.Play();
            break;
        case RecordingEventType::Recovering:
            recordingStatus_ = L"Holding recording";
            recordingDetail_ = L"Fullscreen mode changed · reconnecting GPU capture";
            break;
        case RecordingEventType::Recovered:
            recordingStatus_ = recordingEngine_ && recordingEngine_->IsReplayBuffering()
                ? L"Replay buffer live" : L"Recording live";
            recordingDetail_ = L"GPU capture restored without stopping the MP4";
            break;
        case RecordingEventType::AudioWarning:
            audioWarning_ = event->detail;
            recordingStatus_ = L"Recording with partial audio";
            recordingDetail_ = event->detail;
            break;
        case RecordingEventType::Stats:
            if (recordingEngine_ && recordingEngine_->IsReplayBuffering()) {
                recordingStatus_ = L"Replay rolling buffer";
                recordingDetail_ = L"Only the newest " + std::to_wstring(settings_.replaySeconds) +
                                   L" seconds are retained · encoded disk cache";
            } else {
                recordingStatus_ = event->detail;
                recordingDetail_ = std::to_wstring(event->framesWritten) + L" frames · " +
                                   std::to_wstring(event->droppedFrames) + L" dropped";
            }
            break;
        case RecordingEventType::RecordingSaved:
            lastRecordingPath_ = event->path;
            recordingStatus_ = L"Recording saved";
            recordingDetail_ = PathFileName(event->path);
            if (settings_.sound) soundPlayer_.Play();
            if (settings_.notifications) overlay_.ShowMessage(L"Recording saved", PathFileName(event->path),
                                                               settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
            activeRecordingMonitorIndex_ = -1;
            ResumeCaptureAfterRecording();
            break;
        case RecordingEventType::ReplaySaved:
            lastRecordingPath_ = event->path;
            recordingStatus_ = L"Replay saved";
            recordingDetail_ = PathFileName(event->path);
            if (settings_.sound) soundPlayer_.Play();
            if (settings_.notifications) overlay_.ShowMessage(L"Replay saved", PathFileName(event->path),
                                                               settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
            break;
        case RecordingEventType::ReplayStopped:
        case RecordingEventType::RecordingStopped:
            recordingStatus_ = L"Ready to record";
            recordingDetail_ = event->detail;
            activeRecordingMonitorIndex_ = -1;
            primaryReplayMonitorIndex_ = -1;
            ResumeCaptureAfterRecording();
            break;
        case RecordingEventType::Error:
            recordingStatus_ = L"Recording error";
            recordingDetail_ = event->detail;
            if (settings_.notifications) overlay_.ShowError(event->detail, settings_.overlayDurationMs,
                                                             GetPriorityMonitor(hwnd_));
            if (!recordingEngine_ || (!recordingEngine_->IsRecording() && !IsReplayBuffering())) {
                activeRecordingMonitorIndex_ = -1;
                primaryReplayMonitorIndex_ = -1;
                ResumeCaptureAfterRecording();
            }
            break;
    }
    InvalidateUi();
}

void App::HandlePreviewFrame(std::unique_ptr<PreviewFrame> frame) {
    if (!frame) return;
    const bool liveRecording = frame->liveRecording;
    ReleasePreviewFrame();
    if (!frame->bgra.empty() && frame->width && frame->height &&
        page_ == Page::Record && settings_.previewEnabled && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
        previewFrame_ = std::move(frame);
        InvalidateUi();
    } else if (liveRecording) {
        if (recordingEngine_) recordingEngine_->RecyclePreviewFrame(std::move(frame));
    } else if (engine_) {
        engine_->RecyclePreviewFrame(std::move(frame));
    }
    if (liveRecording) {
        if (recordingEngine_) recordingEngine_->AcknowledgePreview();
    } else if (engine_) {
        engine_->AcknowledgePreview();
    }
}

void App::ReleasePreviewFrame() {
    if (!previewFrame_) return;
    if (previewFrame_->liveRecording) {
        if (recordingEngine_) recordingEngine_->RecyclePreviewFrame(std::move(previewFrame_));
    } else if (engine_) {
        engine_->RecyclePreviewFrame(std::move(previewFrame_));
    }
    previewFrame_.reset();
}

void App::SuspendCaptureForRecording() {
    if (captureSuspendedForRecording_ || !engine_) return;
    ReleasePreviewFrame();
    if (engine_->PauseForRecording()) {
        captureSuspendedForRecording_ = true;
        engineReady_ = false;
        engineStatus_ = L"Fullscreen recorder active";
        engineDetail_ = L"Screenshot saving stays online; DXGI frames come from the recorder";
    } else {
        engineStatus_ = L"Capture handoff timed out";
        engineDetail_ = L"The recorder will retry the fullscreen GPU channel";
    }
}

void App::ResumeCaptureAfterRecording() {
    if (!captureSuspendedForRecording_ || !engine_ || exiting_) return;
    captureSuspendedForRecording_ = false;
    engineReady_ = false;
    engineStatus_ = L"Reconnecting screenshots";
    engineDetail_ = L"Restoring all display capture channels";
    engine_->ResumeAfterRecording(settings_);
}

void App::HandleTrayCommand(TrayCommand command) {
    switch (command) {
        case TrayCommand::Open: ShowMainWindow(); break;
        case TrayCommand::CaptureActive: TriggerCapture(CaptureMode::ActiveDisplay); break;
        case TrayCommand::CaptureAll: TriggerCapture(CaptureMode::AllDisplays); break;
        case TrayCommand::CaptureRegion: TriggerCapture(CaptureMode::Region); break;
        case TrayCommand::CaptureWindow: TriggerCapture(CaptureMode::ActiveWindow); break;
        case TrayCommand::OpenFolder: OpenFolderInExplorer(settings_.saveDirectory); break;
        case TrayCommand::ToggleRecording: ToggleRecording(); break;
        case TrayCommand::ToggleReplay: ToggleReplayBuffer(); break;
        case TrayCommand::SaveReplay: SaveAllReplays(); break;
        case TrayCommand::OpenReplayCenter: ShowMainWindow(Page::Replay); break;
        case TrayCommand::OpenRecordings: OpenFolderInExplorer(settings_.recordingDirectory); break;
        case TrayCommand::ToggleSound: settings_.sound = !settings_.sound; SaveSettings(); InvalidateUi(); break;
        case TrayCommand::ToggleNotifications: settings_.notifications = !settings_.notifications; SaveSettings(); InvalidateUi(); break;
        case TrayCommand::Exit: ExitApplication(); PostQuitMessage(0); break;
        default: {
            const UINT commandValue = static_cast<UINT>(command);
            const UINT firstMonitor = static_cast<UINT>(TrayCommand::SaveReplayMonitor0);
            const UINT lastMonitor = static_cast<UINT>(TrayCommand::SaveReplayMonitor7);
            if (commandValue >= firstMonitor && commandValue <= lastMonitor) {
                SaveReplayForMonitor(static_cast<int>(commandValue - firstMonitor));
            }
            break;
        }
    }
}

void App::NavigateTo(Page page) {
    if (page_ == page) return;
    page_ = page;
    if (page != Page::Record) {
        if (engine_) engine_->SetPreviewConsumerActive(false);
        if (recordingEngine_) recordingEngine_->SetPreviewConsumerActive(false);
        ReleasePreviewFrame();
    }
    if (page == Page::Audio) RefreshAudioSources();
    if (page == Page::Replay) recordingOutputs_ = CaptureEngine::EnumerateOutputs();
    InvalidateUi();
}

float App::HoverAmount(Action action) const { return hoverAnimation_[static_cast<std::size_t>(action)]; }
float App::PressAmount(Action action) const { return pressAnimation_[static_cast<std::size_t>(action)]; }

void App::TickAnimations() {
    const bool previewVisible = page_ == Page::Record && settings_.previewEnabled &&
                                IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
    if (engine_) engine_->SetPreviewConsumerActive(previewVisible);
    if (recordingEngine_) recordingEngine_->SetPreviewConsumerActive(previewVisible);
    if (!IsWindowVisible(hwnd_) || IsIconic(hwnd_)) return;
    const ULONGLONG now = GetTickCount64();
    if (page_ == Page::Audio && now - lastMicrophoneMeterTick_ >= 200) {
        for (std::size_t i = 0; i < microphonePeaks_.size(); ++i) {
            if (i < settings_.microphoneInputs.size() && settings_.microphoneInputs[i].enabled) {
                const auto meter = RecordingEngine::QueryMicrophoneMeter(settings_.microphoneInputs[i].deviceId);
                microphonePeaks_[i] = meter.peak;
                microphoneMuted_[i] = meter.muted;
            } else {
                microphonePeaks_[i] = 0.0f;
                microphoneMuted_[i] = false;
            }
        }
        lastMicrophoneMeterTick_ = now;
        InvalidateUi();
    }
    if (page_ == Page::Record && settings_.previewEnabled && engine_ && !captureSuspendedForRecording_ &&
        now - lastPreviewRequest_ >= 1000ULL / static_cast<ULONGLONG>((std::max)(1, settings_.previewFps))) {
        HMONITOR monitor = GetPriorityMonitor(hwnd_);
        if (!settings_.recordingAutoMonitor) {
            for (const auto& output : recordingOutputs_) {
                if (output.linearIndex == settings_.recordingMonitorIndex) { monitor = output.monitor; break; }
            }
        }
        if (engine_->RequestPreview(monitor)) lastPreviewRequest_ = now;
    }
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
    const float targetY = page_ == Page::Home ? 102.0f : page_ == Page::Capture ? 150.0f :
                          page_ == Page::Record ? 198.0f : page_ == Page::Replay ? 246.0f :
                          page_ == Page::Audio ? 294.0f : 342.0f;
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

bool App::GuardSessionSettingChange(const wchar_t* settingName) {
    if ((!recordingEngine_ || !recordingEngine_->IsRecording()) && !IsReplayBuffering()) return true;
    recordingStatus_ = IsReplayBuffering() ? L"Setting locked while replay is live" : L"Setting locked while recording";
    recordingDetail_ = std::wstring(settingName ? settingName : L"This setting") +
                       L" changes the encoder pipeline; stop the active session first. Hotkeys, screenshots, preview and notifications stay editable.";
    InvalidateUi();
    return false;
}

void App::ExecuteAction(Action action) {
    switch (action) {
        case Action::ChooseRecordingFolder:
        case Action::CycleRecordingFps:
        case Action::CycleVideoCodec:
        case Action::CycleRecordingBitrate:
        case Action::CycleRecordingResolution:
        case Action::CycleRecordingMonitor:
        case Action::CycleReplaySeconds:
        case Action::ToggleRecordAudio:
        case Action::ToggleRecordCursor:
        case Action::ToggleSystemAudio:
        case Action::CycleSystemVolume:
        case Action::ToggleApplicationAudio:
        case Action::CycleApplicationVolume:
        case Action::CycleApplicationTarget:
        case Action::AddMicrophone:
        case Action::CycleAudioBitrate:
        case Action::Mic0Toggle: case Action::Mic0Device: case Action::Mic0Volume: case Action::Mic0Remove:
        case Action::Mic1Toggle: case Action::Mic1Device: case Action::Mic1Volume: case Action::Mic1Remove:
        case Action::Mic2Toggle: case Action::Mic2Device: case Action::Mic2Volume: case Action::Mic2Remove:
        case Action::Mic3Toggle: case Action::Mic3Device: case Action::Mic3Volume: case Action::Mic3Remove:
        case Action::ResetDefaults:
            if (!GuardSessionSettingChange(L"Recording, replay or audio configuration")) return;
            break;
        default: break;
    }
    switch (action) {
        case Action::Minimize: ShowWindow(hwnd_, SW_MINIMIZE); break;
        case Action::Hide: HideMainWindow(); break;
        case Action::Exit: ExitApplication(); PostQuitMessage(0); break;
        case Action::NavHome: NavigateTo(Page::Home); break;
        case Action::NavCapture: NavigateTo(Page::Capture); break;
        case Action::NavRecord: NavigateTo(Page::Record); break;
        case Action::NavReplay: NavigateTo(Page::Replay); break;
        case Action::NavAudio: NavigateTo(Page::Audio); break;
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
            const std::pair<int, int> sizes[] = {{0, 0}, {1280, 720}, {1920, 1080}, {2560, 1440},
                                                 {3440, 1440}, {3840, 2160}, {5120, 2880}};
            std::vector<std::wstring> labels{L"Native · no resize", L"HD · 1280 × 720", L"Full HD · 1920 × 1080",
                L"QHD · 2560 × 1440", L"Ultrawide · 3440 × 1440", L"4K UHD · 3840 × 2160",
                L"5K · 5120 × 2880", L"Custom resolution…"};
            int current = static_cast<int>(std::size(sizes));
            for (std::size_t index = 0; index < std::size(sizes); ++index) {
                if (sizes[index].first == settings_.outputWidth && sizes[index].second == settings_.outputHeight) {
                    current = static_cast<int>(index); break;
                }
            }
            const int choice = ChooseNamedItem(hwnd_, labels, current, L"Screenshot resolution",
                                               L"Choose a preset; use Custom only for an exact size");
            if (choice < 0) break;
            if (choice < static_cast<int>(std::size(sizes))) {
                settings_.outputWidth = sizes[choice].first;
                settings_.outputHeight = sizes[choice].second;
            } else {
                int width = settings_.outputWidth > 0 ? settings_.outputWidth : 1920;
                if (!PromptForInteger(hwnd_, L"Screenshot width", L"Exact output width in pixels", width, 1, 16384, width)) break;
                int height = settings_.outputHeight > 0 ? settings_.outputHeight : 1080;
                if (!PromptForInteger(hwnd_, L"Screenshot height", L"Exact output height in pixels", height, 1, 16384, height)) break;
                settings_.outputWidth = width;
                settings_.outputHeight = height;
            }
            SaveSettings(); break;
        }
        case Action::ToggleNotifications: settings_.notifications = !settings_.notifications; SaveSettings(); break;
        case Action::ToggleSound: settings_.sound = !settings_.sound; SaveSettings(); break;
        case Action::ToggleCopyRegion: settings_.copyRegionToClipboard = !settings_.copyRegionToClipboard; SaveSettings(); break;
        case Action::TestOverlay: overlay_.ShowMessage(L"Luna Capture", L"Notifications are ready", settings_.overlayDurationMs, GetPriorityMonitor(hwnd_)); break;
        case Action::ImportNotificationSound: {
            std::wstring selected;
            std::wstring imported;
            if (ChooseWaveFile(hwnd_, selected) && ImportSoundToLibrary(selected, imported)) {
                if (soundPlayer_.LoadFile(imported)) {
                    settings_.notificationSoundPath = imported;
                    SaveSettings();
                    if (settings_.sound) soundPlayer_.Play();
                } else {
                    settings_.notificationSoundPath.clear();
                    soundPlayer_.Initialize(instance_, IDR_CAPTURE_SOUND);
                    overlay_.ShowError(L"The WAV must use PCM or IEEE-float audio", settings_.overlayDurationMs,
                                       GetPriorityMonitor(hwnd_));
                }
            }
            break;
        }
        case Action::ResetNotificationSound:
            settings_.notificationSoundPath.clear(); LoadNotificationSound(); SaveSettings(); break;
        case Action::ToggleRecording: ToggleRecording(); break;
        case Action::ToggleReplay: ToggleReplayBuffer(); break;
        case Action::SaveReplay:
            if (IsReplayBuffering()) SaveAllReplays();
            else { recordingStatus_ = L"Replay buffer is off"; recordingDetail_ = L"Start replay first, then save the last moments"; }
            break;
        case Action::OpenRecordingFolder: OpenFolderInExplorer(settings_.recordingDirectory); break;
        case Action::ChooseRecordingFolder: {
            std::wstring selected;
            if (ChooseFolder(hwnd_, settings_.recordingDirectory, selected)) { settings_.recordingDirectory = selected; SaveSettings(); }
            break;
        }
        case Action::CycleRecordingFps: {
            int value = settings_.recordingFps;
            const std::vector<IntegerPreset> presets{{24, L"Cinema · 24 FPS"}, {30, L"Efficient · 30 FPS"},
                {60, L"Smooth · 60 FPS"}, {90, L"High motion · 90 FPS"}, {120, L"Competitive · 120 FPS"},
                {144, L"144 Hz · 144 FPS"}, {165, L"165 Hz · 165 FPS"}, {240, L"Extreme · 240 FPS"}};
            if (ChooseIntegerPreset(hwnd_, L"Recording frame rate", L"60 FPS is the recommended quality/performance balance",
                                    presets, value, 1, 240, value)) {
                settings_.recordingFps = value; SaveSettings();
            }
            break;
        }
        case Action::CycleVideoCodec: {
            const std::vector<std::wstring> codecs{L"H.264 / AVC · maximum compatibility",
                                                    L"HEVC / H.265 · smaller files, modern players",
                                                    L"AV1 · best compression, requires a supported GPU/Windows encoder"};
            const int choice = ChooseNamedItem(hwnd_, codecs, static_cast<int>(settings_.videoCodec),
                                               L"Video codec", L"All modes use MP4 and prefer the hardware encoder");
            if (choice >= 0) {
                settings_.videoCodec = static_cast<VideoCodec>(choice);
                SaveSettings();
            }
            break;
        }
        case Action::CycleRecordingBitrate: {
            int value = settings_.recordingBitrateMbps;
            const std::vector<IntegerPreset> presets{{6, L"Compact · 6 Mbps"}, {10, L"Efficient · 10 Mbps"},
                {16, L"Balanced · 16 Mbps"}, {24, L"High quality · 24 Mbps"}, {35, L"QHD quality · 35 Mbps"},
                {50, L"4K quality · 50 Mbps"}, {80, L"Near-lossless · 80 Mbps"}, {120, L"Archive · 120 Mbps"}};
            if (ChooseIntegerPreset(hwnd_, L"Video bitrate", L"Higher values preserve more motion detail and use more disk space",
                                    presets, value, 1, 200, value)) {
                settings_.recordingBitrateMbps = value; SaveSettings();
            }
            break;
        }
        case Action::CycleRecordingResolution: {
            int value = settings_.recordingHeight;
            const std::vector<IntegerPreset> presets{{0, L"Native · no scaling"}, {720, L"HD · 720p"},
                {1080, L"Full HD · 1080p"}, {1440, L"QHD · 1440p"}, {2160, L"4K UHD · 2160p"},
                {4320, L"8K · 4320p"}};
            if (ChooseIntegerPreset(hwnd_, L"Recording output size", L"Width follows the source aspect ratio automatically",
                                    presets, value, 0, 4320, value)) {
                settings_.recordingHeight = value; SaveSettings();
            }
            break;
        }
        case Action::CycleRecordingMonitor: {
            recordingOutputs_ = CaptureEngine::EnumerateOutputs();
            std::vector<std::wstring> labels{L"Auto · foreground fullscreen window"};
            for (const auto& output : recordingOutputs_) {
                labels.push_back(L"Monitor " + std::to_wstring(output.linearIndex + 1) + L" · " + output.name +
                                 L" · " + std::to_wstring(output.width) + L"x" + std::to_wstring(output.height));
            }
            const int current = settings_.recordingAutoMonitor ? 0 : settings_.recordingMonitorIndex + 1;
            const int choice = ChooseNamedItem(hwnd_, labels, current, L"Choose recording display");
            if (choice >= 0) {
                settings_.recordingAutoMonitor = choice == 0;
                if (choice > 0) settings_.recordingMonitorIndex = recordingOutputs_[static_cast<std::size_t>(choice - 1)].linearIndex;
                SaveSettings();
            }
            break;
        }
        case Action::CycleReplaySeconds: {
            int value = settings_.replaySeconds;
            const std::vector<IntegerPreset> presets{{15, L"Quick moment · 15 seconds"}, {30, L"Default · 30 seconds"},
                {45, L"Extended moment · 45 seconds"}, {60, L"One minute · 60 seconds"},
                {90, L"One and a half minutes · 90 seconds"}, {120, L"Two minutes · 120 seconds"},
                {180, L"Three minutes · 180 seconds"}, {300, L"Five minutes · 300 seconds"},
                {600, L"Ten minutes · 600 seconds"}};
            if (ChooseIntegerPreset(hwnd_, L"Instant replay length", L"Encoded segments stay on disk instead of filling RAM",
                                    presets, value, 5, 3600, value)) {
                settings_.replaySeconds = value; SaveSettings();
            }
            break;
        }
        case Action::ConfigureReplayMonitors: NavigateTo(Page::Replay); break;
        case Action::ReplayProfileAuto: SetReplaySourceProfile(0); break;
        case Action::ReplayProfilePrimary: SetReplaySourceProfile(1); break;
        case Action::ReplayProfileAll: SetReplaySourceProfile(2); break;
        case Action::ReplayMonitor0Toggle: case Action::ReplayMonitor1Toggle: case Action::ReplayMonitor2Toggle: case Action::ReplayMonitor3Toggle:
        case Action::ReplayMonitor4Toggle: case Action::ReplayMonitor5Toggle: case Action::ReplayMonitor6Toggle: case Action::ReplayMonitor7Toggle:
            ToggleReplayMonitorSource(static_cast<int>(action) - static_cast<int>(Action::ReplayMonitor0Toggle)); break;
        case Action::ReplayMonitor0Save: case Action::ReplayMonitor1Save: case Action::ReplayMonitor2Save: case Action::ReplayMonitor3Save:
        case Action::ReplayMonitor4Save: case Action::ReplayMonitor5Save: case Action::ReplayMonitor6Save: case Action::ReplayMonitor7Save:
            SaveReplayForMonitor(static_cast<int>(action) - static_cast<int>(Action::ReplayMonitor0Save)); break;
        case Action::ReplayMonitor0Bind: case Action::ReplayMonitor1Bind: case Action::ReplayMonitor2Bind: case Action::ReplayMonitor3Bind:
        case Action::ReplayMonitor4Bind: case Action::ReplayMonitor5Bind: case Action::ReplayMonitor6Bind: case Action::ReplayMonitor7Bind:
            BeginHotkeyRecording(6 + static_cast<int>(action) - static_cast<int>(Action::ReplayMonitor0Bind)); break;
        case Action::RecordReplayAllHotkey: BeginHotkeyRecording(5); break;
        case Action::ToggleRecordAudio: settings_.recordSystemAudio = !settings_.recordSystemAudio; SaveSettings(); break;
        case Action::ToggleRecordCursor: settings_.recordCursor = !settings_.recordCursor; SaveSettings(); break;
        case Action::TogglePreview:
            settings_.previewEnabled = !settings_.previewEnabled;
            if (!settings_.previewEnabled) {
                ReleasePreviewFrame();
                if (engine_) engine_->SetPreviewConsumerActive(false);
                if (recordingEngine_) recordingEngine_->SetPreviewConsumerActive(false);
            }
            SaveSettings();
            break;
        case Action::CyclePreviewFps: {
            int value = settings_.previewEnabled ? settings_.previewFps : 0;
            const std::vector<IntegerPreset> presets{{0, L"Disabled · release preview GPU/RAM resources"},
                {5, L"Ultra efficient · 5 FPS"}, {10, L"Efficient · 10 FPS"}, {15, L"Balanced · 15 FPS"},
                {24, L"Cinema · 24 FPS"}, {30, L"Smooth · 30 FPS"}, {60, L"Fluid · 60 FPS"}};
            if (ChooseIntegerPreset(hwnd_, L"Live preview", L"This does not change the FPS of the saved recording",
                                    presets, value, 0, 60, value)) {
                settings_.previewEnabled = value > 0;
                if (value > 0) settings_.previewFps = value;
                else {
                    ReleasePreviewFrame();
                    if (engine_) engine_->SetPreviewConsumerActive(false);
                    if (recordingEngine_) recordingEngine_->SetPreviewConsumerActive(false);
                }
                if (recordingEngine_) recordingEngine_->SetPreviewFrameRate(settings_.previewFps);
                SaveSettings();
            }
            break;
        }
        case Action::ToggleSystemAudio:
            settings_.recordSystemAudio = !settings_.recordSystemAudio;
            if (settings_.recordSystemAudio) settings_.recordApplicationAudio = false;
            SaveSettings(); break;
        case Action::CycleSystemVolume: {
            int value = settings_.systemAudioVolumePercent;
            const std::vector<IntegerPreset> presets{{0, L"Muted · 0%"}, {50, L"Quiet · 50%"}, {75, L"Reduced · 75%"},
                {100, L"Original level · 100%"}, {125, L"Boost · 125%"}, {150, L"Strong boost · 150%"},
                {200, L"Maximum · 200%"}};
            if (ChooseIntegerPreset(hwnd_, L"System audio gain", L"100% preserves the original level",
                                    presets, value, 0, 200, value)) {
                settings_.systemAudioVolumePercent = value; SaveSettings();
            }
            break;
        }
        case Action::ToggleApplicationAudio:
            settings_.recordApplicationAudio = !settings_.recordApplicationAudio;
            if (settings_.recordApplicationAudio) settings_.recordSystemAudio = false;
            SaveSettings(); break;
        case Action::CycleApplicationVolume: {
            int value = settings_.applicationAudioVolumePercent;
            const std::vector<IntegerPreset> presets{{0, L"Muted · 0%"}, {50, L"Quiet · 50%"}, {75, L"Reduced · 75%"},
                {100, L"Original level · 100%"}, {125, L"Boost · 125%"}, {150, L"Strong boost · 150%"},
                {200, L"Maximum · 200%"}};
            if (ChooseIntegerPreset(hwnd_, L"Application audio gain", settings_.recordingApplicationName,
                                    presets, value, 0, 200, value)) {
                settings_.applicationAudioVolumePercent = value; SaveSettings();
            }
            break;
        }
        case Action::CycleApplicationTarget: CycleApplicationTarget(); SaveSettings(); break;
        case Action::RefreshAudioSources: RefreshAudioSources(); break;
        case Action::AddMicrophone: AddMicrophoneInput(); SaveSettings(); break;
        case Action::CycleAudioBitrate: {
            int value = settings_.audioBitrateKbps;
            const std::vector<IntegerPreset> presets{{96, L"Compact · 96 kbps"}, {128, L"Standard · 128 kbps"},
                {160, L"High · 160 kbps"}, {192, L"Maximum Windows AAC profile · 192 kbps"}};
            if (ChooseIntegerPreset(hwnd_, L"AAC audio bitrate", L"Only profiles supported by the Windows encoder are shown",
                                    presets, value, 96, 192, value, false)) {
                settings_.audioBitrateKbps = NormalizeAacBitrateKbps(value); SaveSettings();
            }
            break;
        }
        case Action::RecordRecordingHotkey: BeginHotkeyRecording(4); break;
        case Action::RecordReplayHotkey: ConfigureReplayHotkey(); break;
        case Action::RecordActiveHotkey: BeginHotkeyRecording(0); break;
        case Action::RecordAllHotkey: BeginHotkeyRecording(1); break;
        case Action::RecordRegionHotkey: BeginHotkeyRecording(2); break;
        case Action::RecordWindowHotkey: BeginHotkeyRecording(3); break;
        case Action::ToggleAutostart: settings_.startWithWindows = !settings_.startWithWindows; SaveSettings(true); break;
        case Action::CycleOverlayDuration: {
            int value = settings_.overlayDurationMs;
            const std::vector<IntegerPreset> presets{{650, L"Fast · 0.65 seconds"}, {1000, L"Compact · 1 second"},
                {1200, L"Default · 1.2 seconds"}, {1800, L"Comfortable · 1.8 seconds"},
                {2500, L"Long · 2.5 seconds"}, {4000, L"Accessibility · 4 seconds"}};
            if (ChooseIntegerPreset(hwnd_, L"Notification duration", L"How long the visual confirmation stays on screen",
                                    presets, value, 250, 10000, value)) {
                settings_.overlayDurationMs = value; SaveSettings();
            }
            break;
        }
        case Action::ResetDefaults:
            settings_ = AppSettings{};
            settings_.saveDirectory = GetDefaultScreenshotDirectory();
            settings_.recordingDirectory = GetDefaultRecordingDirectory();
            LoadNotificationSound();
            SaveSettings(true);
            RegisterAllHotkeys();
            break;
        default:
            ExecuteMicrophoneAction(action);
            SaveSettings();
            break;
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

    FillRound(dc, RECT{16, 78, 180, 380}, 16, RGB(21, 24, 30));
    FillRound(dc, RECT{18, static_cast<int>(sidebarIndicatorY_), 22, static_cast<int>(sidebarIndicatorY_) + 30}, 3,
              RGB(96, 149, 244));

    const struct { const wchar_t* label; int y; Action action; Page page; UiIcon icon; } items[] = {
        {L"Home", 88, Action::NavHome, Page::Home, UiIcon::Home},
        {L"Capture", 136, Action::NavCapture, Page::Capture, UiIcon::Capture},
        {L"Record", 184, Action::NavRecord, Page::Record, UiIcon::Record},
        {L"Replay", 232, Action::NavReplay, Page::Replay, UiIcon::Replay},
        {L"Audio", 280, Action::NavAudio, Page::Audio, UiIcon::Microphone},
        {L"Settings", 328, Action::NavSettings, Page::Settings, UiIcon::Settings},
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
    const wchar_t* name = page_ == Page::Home ? L"Home" : page_ == Page::Capture ? L"Capture" :
                          page_ == Page::Record ? L"Record" : page_ == Page::Replay ? L"Instant Replay" :
                          page_ == Page::Audio ? L"Audio mixer" : L"Settings";
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

    RECT soundCard{x, 426, 582, 558};
    FillRound(dc, soundCard, 14, RGB(20, 23, 29));
    StrokeRound(dc, soundCard, 14, RGB(39, 44, 54));
    DrawUiIcon(dc, UiIcon::Sound, RECT{x + 18, 444, x + 40, 466}, RGB(121, 168, 242), 2);
    Text(dc, L"Notification sound", RECT{x + 50, 436, 520, 466}, RGB(235, 238, 244), fontTitle_);
    const std::wstring soundName = settings_.notificationSoundPath.empty() ? L"Built-in Luna sound" : PathFileName(settings_.notificationSoundPath);
    Text(dc, soundName, RECT{x + 20, 470, 558, 496}, RGB(124, 132, 146), fontSmall_, DT_LEFT | DT_END_ELLIPSIS | DT_SINGLELINE | DT_VCENTER);
    RECT import{x + 20, 505, x + 214, 544};
    FillRound(dc, import, 10, Mix(RGB(31, 37, 47), RGB(48, 60, 78), HoverAmount(Action::ImportNotificationSound)));
    Text(dc, L"Import WAV", import, RGB(226, 232, 242), fontNormal_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddRegion(import.left, import.top, import.right - import.left, import.bottom - import.top, Action::ImportNotificationSound);
    RECT defaultSound{x + 226, 505, x + 334, 544};
    FillRound(dc, defaultSound, 10, Mix(RGB(29, 32, 40), RGB(43, 47, 58), HoverAmount(Action::ResetNotificationSound)));
    Text(dc, L"Default", defaultSound, RGB(170, 178, 192), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddRegion(defaultSound.left, defaultSound.top, defaultSound.right - defaultSound.left, defaultSound.bottom - defaultSound.top, Action::ResetNotificationSound);

}

void App::PaintRecord(HDC dc) {
    const int x = 228;
    const bool recording = recordingEngine_ && recordingEngine_->IsRecording();
    const bool replay = IsReplayBuffering();
    int configuredReplayMonitors = 0;
    for (std::size_t index = 0; index < LUNASHOT_MAX_REPLAY_MONITORS; ++index) {
        if ((settings_.replayMonitorMask & (1u << index)) != 0) ++configuredReplayMonitors;
    }
    int activeReplayChannels = recordingEngine_ && recordingEngine_->IsReplayBuffering() ? 1 : 0;
    for (const auto& channel : auxiliaryReplayEngines_) {
        if (channel.engine && channel.engine->IsReplayBuffering()) ++activeReplayChannels;
    }
    Text(dc, L"Studio recorder", RECT{x, 76, 940, 112}, RGB(244, 246, 251), fontHeadline_);
    Text(dc, L"Fullscreen DXGI capture, live preview, hardware codecs and a multi-source audio mixer.",
         RECT{x, 111, 956, 139}, RGB(137, 144, 158), fontNormal_);

    RECT preview{x, 146, 956, 540};
    FillRound(dc, preview, 16, RGB(18, 21, 27));
    StrokeRound(dc, preview, 16, RGB(39, 44, 54));
    #if 0
    Text(dc, previewFrame_ && previewFrame_->liveRecording ? L"LIVE OUTPUT" : L"DISPLAY PREVIEW",
         RECT{x + 16, 151, x + 180, 178}, RGB(126, 169, 237), fontSmall_);
    RECT previewFps{x + 242, 152, x + 296, 176};
    FillRound(dc, previewFps, 8, Mix(RGB(30, 35, 44), RGB(47, 57, 72), HoverAmount(Action::CyclePreviewFps)));
    Text(dc, std::to_wstring(settings_.previewFps) + L" FPS", previewFps, RGB(174, 193, 222), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddRegion(previewFps.left, previewFps.top, previewFps.right - previewFps.left, previewFps.bottom - previewFps.top, Action::CyclePreviewFps);
    DrawToggle(dc, x + 305, 152, settings_.previewEnabled, HoverAmount(Action::TogglePreview));
    AddRegion(x + 300, 148, 54, 32, Action::TogglePreview);
    #endif

    RECT viewport{x + 12, 190, 944, 528};
    FillRound(dc, viewport, 10, RGB(8, 10, 14));
    if (settings_.previewEnabled && previewFrame_ && !previewFrame_->bgra.empty()) {
        const double scale = (std::min)(static_cast<double>(viewport.right - viewport.left) / previewFrame_->width,
                                        static_cast<double>(viewport.bottom - viewport.top) / previewFrame_->height);
        const int width = (std::max)(1, static_cast<int>(previewFrame_->width * scale));
        const int height = (std::max)(1, static_cast<int>(previewFrame_->height * scale));
        const int drawX = viewport.left + ((viewport.right - viewport.left) - width) / 2;
        const int drawY = viewport.top + ((viewport.bottom - viewport.top) - height) / 2;
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = static_cast<LONG>(previewFrame_->width);
        bitmap.bmiHeader.biHeight = -static_cast<LONG>(previewFrame_->height);
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        StretchDIBits(dc, drawX, drawY, width, height, 0, 0, previewFrame_->width, previewFrame_->height,
                      previewFrame_->bgra.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
    } else {
        DrawUiIcon(dc, UiIcon::Monitor, RECT{x + 349, 312, x + 379, 342}, RGB(68, 78, 95), 2);
        Text(dc, settings_.previewEnabled ? L"Waiting for the first GPU frame" : L"Preview disabled",
             RECT{x + 30, 347, 930, 379}, RGB(88, 98, 115), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    RECT status{248, 460, 936, 518};
    FillRound(dc, status, 16, RGB(20, 23, 29));
    StrokeRound(dc, status, 16, recording ? RGB(119, 50, 58) : replay ? RGB(52, 85, 145) : RGB(39, 44, 54));
    const COLORREF liveColor = recording ? RGB(245, 83, 96) : replay ? RGB(94, 153, 255) : RGB(92, 202, 145);
    ui::FillEllipse(dc, RECT{264, 481, 280, 497}, ui::MixColor(liveColor, RGB(20, 23, 29), 0.65f));
    ui::FillEllipse(dc, RECT{268, 485, 276, 493}, liveColor);
    Text(dc, recordingStatus_, RECT{290, 464, 610, 490}, RGB(240, 243, 248), fontTitle_);
    Text(dc, recordingDetail_, RECT{290, 487, 690, 514}, RGB(128, 136, 151), fontSmall_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const std::uint64_t replayLimitMs = static_cast<std::uint64_t>(settings_.replaySeconds) * 1000;
    const std::uint64_t shownElapsedMs = replay ? (std::min)(recordingElapsedMs_, replayLimitMs) : recordingElapsedMs_;
    const std::uint64_t seconds = shownElapsedMs / 1000;
    wchar_t metrics[128]{};
    if (replay) {
        swprintf_s(metrics, L"%llus / %ds rolling · %.1f fps",
                   static_cast<unsigned long long>(seconds), settings_.replaySeconds, recordingEncodedFps_);
    } else {
        swprintf_s(metrics, L"%02llu:%02llu · %.1f fps · %llu drop",
                   static_cast<unsigned long long>(seconds / 60), static_cast<unsigned long long>(seconds % 60),
                   recordingEncodedFps_, static_cast<unsigned long long>(recordingDroppedFrames_));
    }
    Text(dc, metrics, RECT{690, 472, 916, 506}, liveColor, fontSmall_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    const struct PrimaryButton { RECT rect; const wchar_t* label; std::wstring detail; Action action; UiIcon icon; COLORREF base; } buttons[] = {
        {RECT{228, 552, 456, 610}, recording ? L"Stop recording" : L"Start recording",
         FormatHotkey(settings_.recordingHotkey.modifiers, settings_.recordingHotkey.vk), Action::ToggleRecording, UiIcon::Record, RGB(150, 49, 62)},
        {RECT{470, 552, 698, 610}, replay ? L"Stop replay" : L"Start replay",
         (configuredReplayMonitors > 0 ? std::to_wstring(configuredReplayMonitors) + L" monitors · " : L"Auto monitor · ") +
             std::to_wstring(settings_.replaySeconds) + L" sec", Action::ToggleReplay, UiIcon::Replay, RGB(52, 91, 167)},
        {RECT{712, 552, 956, 610}, L"Save instant replay",
         FormatHotkey(settings_.replaySaveHotkey.modifiers, settings_.replaySaveHotkey.vk) +
             (configuredReplayMonitors > 0 ? L" · saves all" : L" · current monitor"), Action::SaveReplay, UiIcon::Save, RGB(79, 66, 151)},
    };
    for (const auto& button : buttons) {
        const float hover = HoverAmount(button.action);
        FillRound(dc, button.rect, 14, Mix(button.base, ui::MixColor(button.base, RGB(255, 255, 255), 0.18f), hover));
        DrawUiIcon(dc, button.icon, RECT{button.rect.left + 14, button.rect.top + 16, button.rect.left + 36, button.rect.top + 38}, RGB(255, 255, 255), 2);
        Text(dc, button.label, RECT{button.rect.left + 44, button.rect.top + 3, button.rect.right - 10, button.rect.top + 30}, RGB(255, 255, 255), fontTitle_);
        Text(dc, button.detail, RECT{button.rect.left + 44, button.rect.top + 27, button.rect.right - 10, button.rect.bottom - 3}, RGB(214, 222, 238), fontSmall_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        AddRegion(button.rect.left, button.rect.top, button.rect.right - button.rect.left, button.rect.bottom - button.rect.top, button.action);
    }
    // The shortcut labels remain directly editable without shrinking the large preview.
    AddRegion(360, 579, 88, 25, Action::RecordRecordingHotkey);
    AddRegion(856, 579, 92, 25, Action::RecordReplayHotkey);

    #if 0
    const std::wstring resolution = settings_.recordingHeight == 0 ? L"Native" : std::to_wstring(settings_.recordingHeight) + L"p";
    const struct OptionRow { int y; const wchar_t* title; std::wstring value; Action action; UiIcon icon; } leftRows[] = {
        {368, L"Display source", settings_.recordingAutoMonitor ? L"Auto · fullscreen window" : L"Monitor " + std::to_wstring(settings_.recordingMonitorIndex + 1), Action::CycleRecordingMonitor, UiIcon::Monitor},
        {422, L"Frame rate", std::to_wstring(settings_.recordingFps) + L" FPS", Action::CycleRecordingFps, UiIcon::Record},
        {476, L"H.264 bitrate", std::to_wstring(settings_.recordingBitrateMbps) + L" Mbps", Action::CycleRecordingBitrate, UiIcon::AllDisplays},
        {530, L"Output size", resolution, Action::CycleRecordingResolution, UiIcon::Window},
    };
    for (const auto& row : leftRows) {
        RECT rect{x, row.y, 582, row.y + 48};
        FillRound(dc, rect, 12, Mix(RGB(20, 23, 29), RGB(29, 35, 44), HoverAmount(row.action)));
        StrokeRound(dc, rect, 12, RGB(39, 44, 54));
        DrawUiIcon(dc, row.icon, RECT{x + 15, row.y + 14, x + 35, row.y + 34}, RGB(118, 166, 242), 2);
        Text(dc, row.title, RECT{x + 46, row.y, 420, row.y + 48}, RGB(226, 230, 238), fontNormal_);
        Text(dc, row.value, RECT{430, row.y, 560, row.y + 48}, RGB(111, 167, 255), fontNormal_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        AddRegion(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, row.action);
    }

    RECT replayRow{604, 368, 956, 416};
    FillRound(dc, replayRow, 12, Mix(RGB(20, 23, 29), RGB(29, 35, 44), HoverAmount(Action::CycleReplaySeconds)));
    StrokeRound(dc, replayRow, 12, RGB(39, 44, 54));
    DrawUiIcon(dc, UiIcon::Replay, RECT{619, 382, 639, 402}, RGB(118, 166, 242), 2);
    Text(dc, L"Replay window", RECT{650, 368, 800, 416}, RGB(226, 230, 238), fontNormal_);
    Text(dc, std::to_wstring(settings_.replaySeconds) + L" sec", RECT{808, 368, 934, 416}, RGB(111, 167, 255), fontNormal_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    AddRegion(replayRow.left, replayRow.top, replayRow.right - replayRow.left, replayRow.bottom - replayRow.top, Action::CycleReplaySeconds);

    const struct ToggleRow { int y; const wchar_t* title; bool on; Action action; UiIcon icon; } toggleRows[] = {
        {422, L"Record cursor", settings_.recordCursor, Action::ToggleRecordCursor, UiIcon::Cursor},
    };
    for (const auto& row : toggleRows) {
        RECT rect{604, row.y, 956, row.y + 48};
        FillRound(dc, rect, 12, Mix(RGB(20, 23, 29), RGB(29, 35, 44), HoverAmount(row.action)));
        StrokeRound(dc, rect, 12, RGB(39, 44, 54));
        DrawUiIcon(dc, row.icon, RECT{621, row.y + 14, 641, row.y + 34}, RGB(118, 166, 242), 2);
        Text(dc, row.title, RECT{652, row.y, 840, row.y + 48}, RGB(226, 230, 238), fontNormal_);
        DrawToggle(dc, 888, row.y + 12, row.on, HoverAmount(row.action));
        AddRegion(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, row.action);
    }

    RECT hotkeys{604, 476, 956, 524};
    FillRound(dc, hotkeys, 12, RGB(20, 23, 29));
    StrokeRound(dc, hotkeys, 12, RGB(39, 44, 54));
    Text(dc, L"Hotkeys", RECT{619, 476, 688, 524}, RGB(226, 230, 238), fontSmall_);
    RECT recordKey{692, 483, 808, 517};
    RECT replayKey{816, 483, 944, 517};
    FillRound(dc, recordKey, 9, Mix(RGB(31, 36, 45), RGB(43, 51, 64), HoverAmount(Action::RecordRecordingHotkey)));
    FillRound(dc, replayKey, 9, Mix(RGB(31, 36, 45), RGB(43, 51, 64), HoverAmount(Action::RecordReplayHotkey)));
    Text(dc, recordingHotkeySlot_ == 4 ? L"Press keys…" : FormatHotkey(settings_.recordingHotkey.modifiers, settings_.recordingHotkey.vk), recordKey, RGB(112, 169, 255), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    Text(dc, recordingHotkeySlot_ == 5 ? L"Press keys…" : FormatHotkey(settings_.replaySaveHotkey.modifiers, settings_.replaySaveHotkey.vk), replayKey, RGB(150, 139, 245), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    AddRegion(recordKey.left, recordKey.top, recordKey.right - recordKey.left, recordKey.bottom - recordKey.top, Action::RecordRecordingHotkey);
    AddRegion(replayKey.left, replayKey.top, replayKey.right - replayKey.left, replayKey.bottom - replayKey.top, Action::RecordReplayHotkey);

    RECT folder{604, 530, 956, 578};
    FillRound(dc, folder, 12, Mix(RGB(20, 23, 29), RGB(29, 35, 44), HoverAmount(Action::OpenRecordingFolder)));
    StrokeRound(dc, folder, 12, RGB(39, 44, 54));
    DrawUiIcon(dc, UiIcon::Folder, RECT{619, 544, 639, 564}, RGB(118, 166, 242), 2);
    Text(dc, settings_.recordingDirectory, RECT{650, 530, 910, 578}, RGB(171, 180, 194), fontSmall_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    AddRegion(folder.left, folder.top, folder.right - folder.left, folder.bottom - folder.top, Action::OpenRecordingFolder);
    #endif

    const auto chip = [&](RECT rect, const std::wstring& title, const std::wstring& value, Action action) {
        const bool locked = (recording || replay) && action != Action::CyclePreviewFps &&
                            action != Action::ConfigureReplayMonitors;
        FillRound(dc, rect, 10, locked ? RGB(18, 20, 25) : Mix(RGB(20, 23, 29), RGB(34, 41, 52), HoverAmount(action)));
        StrokeRound(dc, rect, 10, RGB(39, 45, 56));
        Text(dc, title, RECT{rect.left + 8, rect.top + 1, rect.right - 8, rect.top + 20}, locked ? RGB(75, 81, 93) : RGB(102, 112, 128), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, value, RECT{rect.left + 7, rect.top + 18, rect.right - 7, rect.bottom - 1}, locked ? RGB(102, 109, 122) : RGB(151, 191, 250), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        AddRegion(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, action);
    };
    chip(RECT{228, 150, 337, 184}, L"DISPLAY", settings_.recordingAutoMonitor ? L"Auto" : L"Monitor " + std::to_wstring(settings_.recordingMonitorIndex + 1), Action::CycleRecordingMonitor);
    chip(RECT{343, 150, 403, 184}, L"CODEC", VideoCodecLabel(settings_.videoCodec), Action::CycleVideoCodec);
    chip(RECT{409, 150, 462, 184}, L"FPS", std::to_wstring(settings_.recordingFps), Action::CycleRecordingFps);
    chip(RECT{468, 150, 542, 184}, L"RATE", std::to_wstring(settings_.recordingBitrateMbps) + L"M", Action::CycleRecordingBitrate);
    chip(RECT{548, 150, 620, 184}, L"OUTPUT", settings_.recordingHeight == 0 ? L"Native" : std::to_wstring(settings_.recordingHeight) + L"p", Action::CycleRecordingResolution);
    chip(RECT{626, 150, 694, 184}, L"REPLAY", std::to_wstring(settings_.replaySeconds) + L"s", Action::CycleReplaySeconds);
    chip(RECT{700, 150, 765, 184}, L"CURSOR", settings_.recordCursor ? L"On" : L"Off", Action::ToggleRecordCursor);
    chip(RECT{771, 150, 847, 184}, L"PREVIEW", settings_.previewEnabled ? std::to_wstring(settings_.previewFps) + L" FPS" : L"Off", Action::CyclePreviewFps);
    chip(RECT{853, 150, 904, 184}, L"REPLAY", activeReplayChannels > 0 ? std::to_wstring(activeReplayChannels) + L" live" : L"Open", Action::ConfigureReplayMonitors);
    RECT folderChip{910, 150, 956, 184};
    FillRound(dc, folderChip, 10, (recording || replay) ? RGB(18, 20, 25) : Mix(RGB(20, 23, 29), RGB(34, 41, 52), HoverAmount(Action::ChooseRecordingFolder)));
    StrokeRound(dc, folderChip, 10, RGB(39, 45, 56));
    DrawUiIcon(dc, UiIcon::Folder, RECT{923, 157, 943, 177}, RGB(143, 183, 244), 2);
    AddRegion(folderChip.left, folderChip.top, folderChip.right - folderChip.left, folderChip.bottom - folderChip.top, Action::ChooseRecordingFolder);
}

void App::PaintReplay(HDC dc) {
    const int x = 228;
    const bool live = IsReplayBuffering();
    const bool recording = recordingEngine_ && recordingEngine_->IsRecording();
    int activeChannels = recordingEngine_ && recordingEngine_->IsReplayBuffering() ? 1 : 0;
    for (const auto& channel : auxiliaryReplayEngines_) {
        if (channel.engine && channel.engine->IsReplayBuffering()) ++activeChannels;
    }
    if (recordingOutputs_.empty()) recordingOutputs_ = CaptureEngine::EnumerateOutputs();

    std::uint32_t allMask = 0;
    std::uint32_t primaryMask = 0;
    const HMONITOR primaryMonitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    for (const auto& output : recordingOutputs_) {
        if (output.linearIndex < 0 || output.linearIndex >= static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) continue;
        allMask |= 1u << output.linearIndex;
        if (output.monitor == primaryMonitor) primaryMask = 1u << output.linearIndex;
    }
    const int profile = settings_.replayMonitorMask == 0 ? 0 : settings_.replayMonitorMask == primaryMask ? 1 :
                        settings_.replayMonitorMask == allMask ? 2 : 3;

    Text(dc, L"Instant Replay", RECT{x, 76, 940, 111}, RGB(244, 246, 251), fontHeadline_);
    Text(dc, L"Choose monitor cards once. Save every channel or one exact monitor without stopping the rolling buffer.",
         RECT{x, 110, 956, 137}, RGB(137, 144, 158), fontNormal_);

    RECT hero{x, 142, 956, 225};
    FillRound(dc, hero, 16, live ? RGB(20, 28, 42) : RGB(20, 23, 29));
    StrokeRound(dc, hero, 16, live ? RGB(58, 101, 166) : RGB(39, 44, 54));
    const COLORREF statusColor = live ? RGB(93, 157, 255) : recording ? RGB(243, 112, 126) : RGB(105, 204, 151);
    ui::FillEllipse(dc, RECT{x + 20, 166, x + 34, 180}, ui::MixColor(statusColor, RGB(20, 23, 29), 0.65f));
    ui::FillEllipse(dc, RECT{x + 24, 170, x + 30, 176}, statusColor);
    const std::wstring heroTitle = live ? L"ROLLING BUFFER LIVE  ·  " + std::to_wstring(activeChannels) + L" MONITOR(S)" :
                                   recording ? L"NORMAL RECORDING IS ACTIVE" : L"READY TO BUFFER";
    Text(dc, heroTitle, RECT{x + 44, 149, 558, 178}, RGB(239, 243, 249), fontTitle_);
    const std::uint64_t buffered = live ? (std::min)(recordingElapsedMs_ / 1000,
        static_cast<std::uint64_t>(settings_.replaySeconds)) : 0;
    const std::wstring heroDetail = live ? L"Keeping newest " + std::to_wstring(buffered) + L" / " +
        std::to_wstring(settings_.replaySeconds) + L" sec · old segments delete automatically" :
        profile == 0 ? L"AUTO will follow the foreground fullscreen game" :
        std::to_wstring(std::popcount(settings_.replayMonitorMask)) + L" monitor source(s) armed";
    Text(dc, heroDetail, RECT{x + 44, 177, 558, 212}, RGB(132, 145, 164), fontSmall_);

    RECT startButton{570, 154, 748, 213};
    const COLORREF startBase = live ? RGB(129, 48, 61) : recording ? RGB(39, 43, 53) : RGB(48, 91, 163);
    const COLORREF startHover = live ? RGB(166, 61, 76) : recording ? RGB(47, 52, 64) : RGB(65, 116, 201);
    FillRound(dc, startButton, 13, Mix(startBase, startHover, HoverAmount(Action::ToggleReplay)));
    DrawUiIcon(dc, live ? UiIcon::Close : recording ? UiIcon::Record : UiIcon::Replay,
               RECT{586, 172, 608, 194}, recording && !live ? RGB(116, 123, 137) : RGB(255, 255, 255), 2);
    Text(dc, live ? L"Stop replay" : recording ? L"Recording active" : L"Start replay", RECT{616, 154, 738, 213},
         recording && !live ? RGB(139, 146, 160) : RGB(255, 255, 255), fontTitle_);
    AddRegion(startButton.left, startButton.top, startButton.right - startButton.left, startButton.bottom - startButton.top, Action::ToggleReplay);

    RECT saveAll{758, 154, 944, 213};
    FillRound(dc, saveAll, 13, Mix(live ? RGB(73, 67, 157) : RGB(39, 42, 52),
                                  live ? RGB(98, 89, 204) : RGB(48, 52, 64), HoverAmount(Action::SaveReplay)));
    DrawUiIcon(dc, UiIcon::Save, RECT{774, 172, 796, 194}, live ? RGB(255, 255, 255) : RGB(112, 119, 132), 2);
    Text(dc, L"Save ALL", RECT{806, 154, 934, 184}, live ? RGB(255, 255, 255) : RGB(139, 146, 160), fontTitle_);
    Text(dc, FormatHotkey(settings_.replaySaveHotkey.modifiers, settings_.replaySaveHotkey.vk),
         RECT{806, 181, 934, 210}, live ? RGB(216, 218, 246) : RGB(100, 106, 119), fontSmall_);
    AddRegion(saveAll.left, saveAll.top, saveAll.right - saveAll.left, saveAll.bottom - saveAll.top, Action::SaveReplay);

    const struct ProfileCard { RECT rect; const wchar_t* title; const wchar_t* detail; Action action; int id; } profiles[] = {
        {RECT{228, 236, 456, 289}, L"AUTO GAME", L"Follow foreground game", Action::ReplayProfileAuto, 0},
        {RECT{464, 236, 692, 289}, L"PRIMARY ONLY", L"Lowest resource usage", Action::ReplayProfilePrimary, 1},
        {RECT{700, 236, 956, 289}, L"ALL MONITORS", L"Independent file per display", Action::ReplayProfileAll, 2},
    };
    for (const auto& card : profiles) {
        const bool selected = profile == card.id;
        RECT rect = card.rect;
        FillRound(dc, rect, 12, selected ? RGB(30, 45, 69) : Mix(RGB(20, 23, 29), RGB(29, 35, 44), HoverAmount(card.action)));
        StrokeRound(dc, rect, 12, selected ? RGB(77, 128, 207) : RGB(39, 44, 54), selected ? 2 : 1);
        Text(dc, card.title, RECT{rect.left + 14, rect.top + 3, rect.right - 14, rect.top + 28},
             selected ? RGB(139, 188, 255) : RGB(225, 229, 237), fontTitle_);
        Text(dc, card.detail, RECT{rect.left + 14, rect.top + 26, rect.right - 14, rect.bottom - 3},
             RGB(116, 126, 142), fontSmall_);
        AddRegion(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, card.action);
    }

    const struct OptionButton { RECT rect; std::wstring title; std::wstring value; Action action; UiIcon icon; } options[] = {
        {RECT{228, 299, 448, 342}, L"Rolling window", std::to_wstring(settings_.replaySeconds) + L" seconds", Action::CycleReplaySeconds, UiIcon::Replay},
        {RECT{456, 299, 718, 342}, L"Save ALL shortcut", FormatHotkey(settings_.replaySaveHotkey.modifiers, settings_.replaySaveHotkey.vk), Action::RecordReplayAllHotkey, UiIcon::Save},
        {RECT{726, 299, 956, 342}, L"Replay files", L"Open output folder", Action::OpenRecordingFolder, UiIcon::Folder},
    };
    for (const auto& option : options) {
        FillRound(dc, option.rect, 11, Mix(RGB(20, 23, 29), RGB(31, 37, 47), HoverAmount(option.action)));
        StrokeRound(dc, option.rect, 11, RGB(39, 44, 54));
        DrawUiIcon(dc, option.icon, RECT{option.rect.left + 12, option.rect.top + 12, option.rect.left + 30, option.rect.top + 30}, RGB(119, 169, 245), 2);
        Text(dc, option.title, RECT{option.rect.left + 39, option.rect.top, option.rect.right - 10, option.rect.top + 23}, RGB(225, 230, 238), fontSmall_);
        Text(dc, option.value, RECT{option.rect.left + 39, option.rect.top + 20, option.rect.right - 10, option.rect.bottom}, RGB(128, 160, 207), fontSmall_);
        AddRegion(option.rect.left, option.rect.top, option.rect.right - option.rect.left, option.rect.bottom - option.rect.top, option.action);
    }

    Text(dc, L"MONITOR CHANNELS", RECT{x, 350, 470, 379}, RGB(106, 117, 134), fontSmall_);
    Text(dc, live ? L"Save this exports one channel while every buffer keeps rolling" : L"Click a monitor card to build a custom group",
         RECT{470, 350, 956, 379}, RGB(91, 101, 117), fontSmall_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    const std::size_t visibleOutputs = (std::min<std::size_t>)(4, recordingOutputs_.size());
    for (std::size_t row = 0; row < visibleOutputs; ++row) {
        const auto& output = recordingOutputs_[row];
        if (output.linearIndex < 0 || output.linearIndex >= static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) continue;
        const int monitorIndex = output.linearIndex;
        bool channelLive = recordingEngine_ && recordingEngine_->IsReplayBuffering() && primaryReplayMonitorIndex_ == monitorIndex;
        for (const auto& channel : auxiliaryReplayEngines_) {
            channelLive |= channel.monitorIndex == monitorIndex && channel.engine && channel.engine->IsReplayBuffering();
        }
        const bool armed = (settings_.replayMonitorMask & (1u << monitorIndex)) != 0;
        const bool autoCandidate = settings_.replayMonitorMask == 0;
        const int y = 382 + static_cast<int>(row) * 56;
        RECT card{x, y, 956, y + 50};
        FillRound(dc, card, 12, channelLive ? RGB(23, 35, 54) : armed ? RGB(22, 29, 40) : RGB(19, 22, 28));
        StrokeRound(dc, card, 12, channelLive ? RGB(65, 118, 198) : armed ? RGB(48, 70, 103) : RGB(38, 43, 53));

        const Action toggleAction = static_cast<Action>(static_cast<int>(Action::ReplayMonitor0Toggle) + monitorIndex);
        DrawToggle(dc, x + 14, y + 13, armed || channelLive, HoverAmount(toggleAction));
        Text(dc, L"Monitor " + std::to_wstring(monitorIndex + 1) + L"  ·  " + output.name,
             RECT{x + 72, y + 1, 565, y + 27}, RGB(232, 236, 244), fontTitle_);
        Text(dc, std::to_wstring(output.width) + L" × " + std::to_wstring(output.height) + L"  ·  " + output.adapterName,
             RECT{x + 72, y + 24, 565, y + 48}, RGB(108, 118, 134), fontSmall_);
        AddRegion(card.left, card.top, 410, card.bottom - card.top, toggleAction);

        const std::wstring state = channelLive ? L"LIVE" : armed ? L"ARMED" : autoCandidate ? L"AUTO" : L"OFF";
        RECT stateRect{570, y + 11, 638, y + 39};
        FillRound(dc, stateRect, 8, channelLive ? RGB(43, 82, 143) : armed ? RGB(37, 56, 82) : RGB(31, 34, 42));
        Text(dc, state, stateRect, channelLive ? RGB(174, 211, 255) : armed ? RGB(141, 176, 222) : RGB(110, 117, 130),
             fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        const Action bindAction = static_cast<Action>(static_cast<int>(Action::ReplayMonitor0Bind) + monitorIndex);
        const auto& binding = settings_.replayMonitorHotkeys[static_cast<std::size_t>(monitorIndex)];
        RECT bindRect{646, y + 8, 800, y + 42};
        FillRound(dc, bindRect, 9, Mix(RGB(29, 34, 43), RGB(42, 50, 64), HoverAmount(bindAction)));
        const std::wstring bindText = recordingHotkeySlot_ == 6 + monitorIndex ? L"Press shortcut…" :
                                      binding.vk ? FormatHotkey(binding.modifiers, binding.vk) : L"Set bind";
        Text(dc, bindText, bindRect, RGB(136, 180, 244), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        AddRegion(bindRect.left, bindRect.top, bindRect.right - bindRect.left, bindRect.bottom - bindRect.top, bindAction);

        const Action saveAction = static_cast<Action>(static_cast<int>(Action::ReplayMonitor0Save) + monitorIndex);
        RECT saveRect{808, y + 8, 944, y + 42};
        FillRound(dc, saveRect, 9, Mix(channelLive ? RGB(67, 65, 146) : RGB(31, 34, 42),
                                      channelLive ? RGB(92, 86, 192) : RGB(39, 43, 52), HoverAmount(saveAction)));
        Text(dc, L"Save this", saveRect, channelLive ? RGB(247, 246, 255) : RGB(104, 111, 124),
             fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddRegion(saveRect.left, saveRect.top, saveRect.right - saveRect.left, saveRect.bottom - saveRect.top, saveAction);
    }
    if (recordingOutputs_.size() > visibleOutputs) {
        Text(dc, L"+ " + std::to_wstring(recordingOutputs_.size() - visibleOutputs) + L" more monitor(s) available through the All profile",
             RECT{650, 350, 956, 379}, RGB(102, 112, 128), fontSmall_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

void App::PaintAudio(HDC dc) {
    const int x = 228;
    Text(dc, L"Audio mixer", RECT{x, 76, 940, 112}, RGB(244, 246, 251), fontHeadline_);
    Text(dc, L"Mix system or per-application audio with up to four microphones. Every source has its own gain.",
         RECT{x, 111, 956, 139}, RGB(137, 144, 158), fontNormal_);

    RECT audioState{x, 143, 956, 173};
    const bool hasAudioError = !audioWarning_.empty();
    const bool mixerLocked = (recordingEngine_ && recordingEngine_->IsRecording()) || IsReplayBuffering();
    FillRound(dc, audioState, 9, hasAudioError ? RGB(42, 25, 30) : mixerLocked ? RGB(39, 34, 23) : RGB(20, 26, 34));
    StrokeRound(dc, audioState, 9, hasAudioError ? RGB(94, 43, 53) : mixerLocked ? RGB(92, 75, 39) : RGB(38, 50, 66));
    DrawUiIcon(dc, UiIcon::Sound, RECT{x + 12, 149, x + 30, 167},
               hasAudioError ? RGB(240, 111, 128) : RGB(95, 158, 248), 2);
    Text(dc, hasAudioError ? audioWarning_ : mixerLocked ? L"Mixer locked for this active session · stop replay/recording to change sources; meters stay live" :
            L"Live input meters · 48 kHz stereo AAC · click any value for presets and optional Custom",
         RECT{x + 39, 143, 944, 173}, hasAudioError ? RGB(242, 139, 151) : mixerLocked ? RGB(218, 184, 111) : RGB(135, 157, 190), fontSmall_,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    const auto paintBus = [&](RECT rect, UiIcon icon, const wchar_t* title, const std::wstring& detail,
                              bool enabled, int volume, Action toggle, Action volumeAction, int textRight) {
        FillRound(dc, rect, 13, Mix(RGB(20, 23, 29), RGB(28, 34, 43), HoverAmount(toggle)));
        StrokeRound(dc, rect, 13, enabled ? RGB(58, 94, 150) : RGB(39, 44, 54));
        DrawUiIcon(dc, icon, RECT{rect.left + 17, rect.top + 17, rect.left + 39, rect.top + 39},
                   enabled ? RGB(119, 171, 250) : RGB(97, 107, 123), 2);
        Text(dc, title, RECT{rect.left + 50, rect.top + 2, textRight, rect.top + 30}, RGB(234, 238, 245), fontTitle_);
        Text(dc, detail, RECT{rect.left + 50, rect.top + 29, textRight, rect.bottom - 2}, RGB(119, 128, 143), fontSmall_,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT gain{758, rect.top + 11, 866, rect.bottom - 11};
        FillRound(dc, gain, 9, Mix(RGB(30, 35, 44), RGB(45, 55, 69), HoverAmount(volumeAction)));
        Text(dc, std::to_wstring(volume) + L"%", gain, RGB(126, 177, 255), fontNormal_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawToggle(dc, 888, rect.top + 16, enabled, HoverAmount(toggle));
        AddRegion(gain.left, gain.top, gain.right - gain.left, gain.bottom - gain.top, volumeAction);
        AddRegion(876, rect.top + 5, 68, rect.bottom - rect.top - 10, toggle);
    };

    paintBus(RECT{x, 181, 956, 233}, UiIcon::Sound, L"System audio",
             L"Desktop output · games, browser, voice chat", settings_.recordSystemAudio,
             settings_.systemAudioVolumePercent, Action::ToggleSystemAudio, Action::CycleSystemVolume, 730);

    std::wstring application = settings_.recordingApplicationName.empty()
        ? L"Click target to choose an open application" : settings_.recordingApplicationName;
    paintBus(RECT{x, 241, 956, 293}, UiIcon::Window, L"Application audio",
             L"Isolated process loopback", settings_.recordApplicationAudio,
             settings_.applicationAudioVolumePercent, Action::ToggleApplicationAudio, Action::CycleApplicationVolume, 518);
    RECT target{528, 251, 744, 283};
    FillRound(dc, target, 9, Mix(RGB(29, 34, 42), RGB(44, 52, 65), HoverAmount(Action::CycleApplicationTarget)));
    Text(dc, application, RECT{target.left + 10, target.top, target.right - 10, target.bottom}, RGB(174, 184, 201), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    AddRegion(target.left, target.top, target.right - target.left, target.bottom - target.top, Action::CycleApplicationTarget);

    Text(dc, L"Microphone inputs", RECT{x, 301, 500, 331}, RGB(233, 237, 244), fontTitle_);
    RECT refresh{740, 300, 842, 330};
    RECT add{850, 300, 956, 330};
    FillRound(dc, refresh, 9, Mix(RGB(29, 34, 42), RGB(43, 51, 64), HoverAmount(Action::RefreshAudioSources)));
    FillRound(dc, add, 9, Mix(RGB(48, 77, 126), RGB(63, 101, 165), HoverAmount(Action::AddMicrophone)));
    Text(dc, L"Refresh", refresh, RGB(180, 190, 205), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    Text(dc, L"+ Add mic", add, RGB(235, 241, 252), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddRegion(refresh.left, refresh.top, refresh.right - refresh.left, refresh.bottom - refresh.top, Action::RefreshAudioSources);
    AddRegion(add.left, add.top, add.right - add.left, add.bottom - add.top, Action::AddMicrophone);

    constexpr Action toggles[] = {Action::Mic0Toggle, Action::Mic1Toggle, Action::Mic2Toggle, Action::Mic3Toggle};
    constexpr Action devices[] = {Action::Mic0Device, Action::Mic1Device, Action::Mic2Device, Action::Mic3Device};
    constexpr Action volumes[] = {Action::Mic0Volume, Action::Mic1Volume, Action::Mic2Volume, Action::Mic3Volume};
    constexpr Action removes[] = {Action::Mic0Remove, Action::Mic1Remove, Action::Mic2Remove, Action::Mic3Remove};
    for (std::size_t slot = 0; slot < 4; ++slot) {
        const int y = 335 + static_cast<int>(slot) * 50;
        RECT row{x, y, 956, y + 44};
        FillRound(dc, row, 12, RGB(20, 23, 29));
        StrokeRound(dc, row, 12, RGB(39, 44, 54));
        if (slot >= settings_.microphoneInputs.size()) {
            DrawUiIcon(dc, UiIcon::Microphone, RECT{x + 17, y + 13, x + 37, y + 33}, RGB(62, 70, 83), 2);
            Text(dc, slot == 0 ? L"No microphone added" : L"Empty mixer slot", RECT{x + 49, y, 700, y + 44}, RGB(82, 91, 105), fontSmall_);
            continue;
        }
        const auto& input = settings_.microphoneInputs[slot];
        DrawUiIcon(dc, UiIcon::Microphone, RECT{x + 17, y + 13, x + 37, y + 33}, input.enabled ? RGB(120, 171, 249) : RGB(83, 91, 105), 2);
        Text(dc, input.name.empty() ? L"Microphone" : input.name, RECT{x + 49, y + 1, 650, y + 25}, RGB(229, 233, 241), fontNormal_);
        const std::wstring meterState = microphoneMuted_[slot] ? L"Muted in Windows · click name to change" :
            (input.enabled && microphonePeaks_[slot] < 0.002f ? L"Listening · speak to test this exact device" : L"Live signal · click name to change device");
        Text(dc, meterState,
             RECT{x + 49, y + 23, 492, y + 44}, microphoneMuted_[slot] ? RGB(239, 105, 121) : RGB(104, 113, 128), fontSmall_);
        RECT meter{500, y + 32, 660, y + 38};
        FillRound(dc, meter, 3, RGB(37, 42, 51));
        const int peakWidth = static_cast<int>((meter.right - meter.left) * microphonePeaks_[slot]);
        if (peakWidth > 0) FillRound(dc, RECT{meter.left, meter.top, meter.left + peakWidth, meter.bottom}, 3,
                                    microphonePeaks_[slot] > 0.88f ? RGB(241, 95, 108) : RGB(83, 199, 137));
        RECT gain{688, y + 8, 776, y + 38};
        FillRound(dc, gain, 8, Mix(RGB(30, 35, 44), RGB(45, 55, 69), HoverAmount(volumes[slot])));
        Text(dc, std::to_wstring(input.volumePercent) + L"%", gain, RGB(126, 177, 255), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawToggle(dc, 802, y + 11, input.enabled, HoverAmount(toggles[slot]));
        RECT remove{866, y + 8, 936, y + 38};
        FillRound(dc, remove, 8, Mix(RGB(40, 31, 35), RGB(65, 39, 45), HoverAmount(removes[slot])));
        Text(dc, L"Remove", remove, RGB(211, 144, 157), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddRegion(x + 45, y, 425, 44, devices[slot]);
        AddRegion(gain.left, gain.top, gain.right - gain.left, gain.bottom - gain.top, volumes[slot]);
        AddRegion(792, y + 5, 62, 36, toggles[slot]);
        AddRegion(remove.left, remove.top, remove.right - remove.left, remove.bottom - remove.top, removes[slot]);
    }

    RECT encoding{x, 539, 956, 587};
    FillRound(dc, encoding, 12, Mix(RGB(20, 23, 29), RGB(29, 35, 44), HoverAmount(Action::CycleAudioBitrate)));
    StrokeRound(dc, encoding, 12, RGB(39, 44, 54));
    DrawUiIcon(dc, UiIcon::AllDisplays, RECT{x + 17, 553, x + 37, 573}, RGB(118, 166, 242), 2);
    Text(dc, L"AAC output quality", RECT{x + 49, 539, 700, 587}, RGB(226, 230, 238), fontNormal_);
    Text(dc, std::to_wstring(settings_.audioBitrateKbps) + L" kbps · supported Windows profile", RECT{650, 539, 932, 587}, RGB(111, 167, 255), fontNormal_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    AddRegion(encoding.left, encoding.top, encoding.right - encoding.left, encoding.bottom - encoding.top, Action::CycleAudioBitrate);
}

void App::PaintSettings(HDC dc) {
    const int x = 228;
    Text(dc, L"Shortcuts and startup", RECT{x, 76, 940, 112}, RGB(244, 246, 251), fontHeadline_);
    Text(dc, L"Fullscreen-safe: Raw Input + keyboard hook + Windows hotkey. Click a shortcut to replace it.",
         RECT{x, 111, 956, 139}, RGB(137, 144, 158), fontNormal_);

    const struct HotkeyRow { int y; const wchar_t* title; const wchar_t* detail; const HotkeyBinding* binding; Action action; int slot; UiIcon icon; } rows[] = {
        {145, L"Active display", L"Foreground monitor screenshot", &settings_.activeDisplayHotkey, Action::RecordActiveHotkey, 0, UiIcon::Monitor},
        {197, L"All displays", L"Stitched virtual desktop", &settings_.allDisplaysHotkey, Action::RecordAllHotkey, 1, UiIcon::AllDisplays},
        {249, L"Region editor", L"Selection and annotation", &settings_.regionHotkey, Action::RecordRegionHotkey, 2, UiIcon::Region},
        {301, L"Active window", L"Frozen window picker", &settings_.activeWindowHotkey, Action::RecordWindowHotkey, 3, UiIcon::Window},
        {353, L"Start / stop recording", L"Works over exclusive fullscreen", &settings_.recordingHotkey, Action::RecordRecordingHotkey, 4, UiIcon::Record},
        {405, L"Save instant replay", L"All monitors or choose a monitor bind", &settings_.replaySaveHotkey, Action::RecordReplayHotkey, 5, UiIcon::Replay},
    };
    for (const auto& row : rows) {
        RECT rect{x, row.y, 956, row.y + 46};
        FillRound(dc, rect, 11, Mix(RGB(20, 23, 29), RGB(29, 35, 44), HoverAmount(row.action)));
        StrokeRound(dc, rect, 11, recordingHotkeySlot_ == row.slot ? RGB(93, 149, 245) : RGB(39, 44, 54), recordingHotkeySlot_ == row.slot ? 2 : 1);
        DrawUiIcon(dc, row.icon, RECT{x + 17, row.y + 13, x + 37, row.y + 33}, RGB(127, 174, 246), 2);
        Text(dc, row.title, RECT{x + 48, row.y + 1, 590, row.y + 24}, RGB(236, 239, 245), fontTitle_);
        Text(dc, row.detail, RECT{x + 48, row.y + 21, 670, row.y + 44}, RGB(124, 132, 146), fontSmall_);
        const std::wstring hotkey = recordingHotkeySlot_ == row.slot ? L"Press shortcut / Fn combo…" : FormatHotkey(row.binding->modifiers, row.binding->vk);
        RECT keyRect{700, row.y + 6, 932, row.y + 40};
        FillRound(dc, keyRect, 9, RGB(31, 36, 45));
        Text(dc, hotkey, RECT{keyRect.left + 10, keyRect.top, keyRect.right - 10, keyRect.bottom}, RGB(113, 169, 255), fontNormal_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        AddRegion(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, row.action);
    }

    const struct SettingRow { int y; const wchar_t* title; const wchar_t* detail; Action action; bool toggle; bool on; } settingsRows[] = {
        {464, L"Start with Windows", L"Launch directly to the system tray", Action::ToggleAutostart, true, settings_.startWithWindows},
        {520, L"Notification duration", L"Compact overlay display time", Action::CycleOverlayDuration, false, false},
    };
    for (const auto& row : settingsRows) {
        RECT rect{x, row.y, 718, row.y + 50};
        FillRound(dc, rect, 13, Mix(RGB(20, 23, 29), RGB(28, 33, 42), HoverAmount(row.action)));
        StrokeRound(dc, rect, 13, RGB(39, 44, 54));
        const UiIcon settingIcon = row.action == Action::ToggleAutostart ? UiIcon::Power : UiIcon::Bell;
        DrawUiIcon(dc, settingIcon, RECT{x + 17, row.y + 15, x + 37, row.y + 35}, RGB(121, 168, 242), 2);
        Text(dc, row.title, RECT{x + 49, row.y + 2, 548, row.y + 28}, RGB(235, 238, 244), fontTitle_);
        Text(dc, row.detail, RECT{x + 49, row.y + 26, 548, row.y + 50}, RGB(124, 132, 146), fontSmall_);
        if (row.toggle) DrawToggle(dc, 646, row.y + 13, row.on, HoverAmount(row.action));
        else Text(dc, std::to_wstring(settings_.overlayDurationMs) + L" ms", RECT{570, row.y, 694, row.y + 50}, RGB(111, 167, 255), fontNormal_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        AddRegion(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, row.action);
    }

    RECT reset{742, 464, 956, 570};
    FillRound(dc, reset, 13, Mix(RGB(29, 24, 27), RGB(54, 34, 39), HoverAmount(Action::ResetDefaults)));
    StrokeRound(dc, reset, 13, RGB(70, 47, 53));
    DrawUiIcon(dc, UiIcon::Undo, RECT{760, 480, 784, 504}, RGB(238, 174, 187), 2);
    Text(dc, L"Reset defaults", RECT{790, 475, 940, 507}, RGB(244, 214, 219), fontTitle_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    Text(dc, L"Restore all shortcuts and capture / recording options.", RECT{760, 511, 940, 554}, RGB(155, 126, 133), fontSmall_, DT_LEFT | DT_WORDBREAK);
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
    else if (page_ == Page::Record) PaintRecord(backBufferDc_);
    else if (page_ == Page::Replay) PaintReplay(backBufferDc_);
    else if (page_ == Page::Audio) PaintAudio(backBufferDc_);
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
            if (recordingHotkeySlot_ >= 0 && globalKeyboardHook_ == nullptr && hotkeyRecordingHook_ == nullptr) {
                if (wParam == VK_ESCAPE) CancelHotkeyRecording();
                else FinishHotkeyRecording(static_cast<UINT>(wParam), CurrentModifiers());
                return 0;
            }
            break;
        case WM_INPUT:
            HandleRawKeyboardInput(reinterpret_cast<HRAWINPUT>(lParam));
            if (GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT) {
                return DefWindowProcW(hwnd_, message, wParam, lParam);
            }
            return 0;
        case WM_APPCOMMAND:
            if (recordingHotkeySlot_ >= 0) {
                const UINT vk = AppCommandToVirtualKey(GET_APPCOMMAND_LPARAM(lParam));
                if (vk != 0) { QueueRecordedHotkey(vk, CurrentModifiers()); return TRUE; }
            } else {
                const UINT vk = AppCommandToVirtualKey(GET_APPCOMMAND_LPARAM(lParam));
                if (vk != 0 && DispatchGlobalHotkey(vk, CurrentModifiers(), L"application command")) return TRUE;
            }
            break;
        case WM_APP_GLOBAL_HOTKEY_INPUT:
            DispatchGlobalHotkey(static_cast<UINT>(wParam), static_cast<UINT>(lParam), L"low-level hook");
            return 0;
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
            if (wParam == LUNASHOT_HOTKEY_ACTIVE) ExecuteGlobalHotkey(0, L"RegisterHotKey");
            else if (wParam == LUNASHOT_HOTKEY_ALL) ExecuteGlobalHotkey(1, L"RegisterHotKey");
            else if (wParam == LUNASHOT_HOTKEY_REGION) ExecuteGlobalHotkey(2, L"RegisterHotKey");
            else if (wParam == LUNASHOT_HOTKEY_WINDOW) ExecuteGlobalHotkey(3, L"RegisterHotKey");
            else if (wParam == LUNASHOT_HOTKEY_RECORD) ExecuteGlobalHotkey(4, L"RegisterHotKey");
            else if (wParam == LUNASHOT_HOTKEY_REPLAY_SAVE) ExecuteGlobalHotkey(5, L"RegisterHotKey");
            else if (wParam >= LUNASHOT_HOTKEY_REPLAY_MONITOR_BASE &&
                     wParam < LUNASHOT_HOTKEY_REPLAY_MONITOR_BASE + LUNASHOT_MAX_REPLAY_MONITORS) {
                ExecuteGlobalHotkey(6 + static_cast<int>(wParam - LUNASHOT_HOTKEY_REPLAY_MONITOR_BASE), L"RegisterHotKey");
            }
            return 0;
        case WM_DISPLAYCHANGE:
            recordingOutputs_ = CaptureEngine::EnumerateOutputs();
            if (recordingEngine_) recordingEngine_->NotifyDisplayChange();
            for (auto& channel : auxiliaryReplayEngines_) if (channel.engine) channel.engine->NotifyDisplayChange();
            if (engine_) engine_->NotifyDisplayChange();
            return 0;
        case WM_APP_ENGINE_EVENT:
            HandleEngineEvent(std::unique_ptr<EngineEvent>(reinterpret_cast<EngineEvent*>(lParam)));
            return 0;
        case WM_APP_REGION_COMPLETE:
            HandleRegionResult(std::unique_ptr<RegionResult>(reinterpret_cast<RegionResult*>(lParam)));
            return 0;
        case WM_APP_RECORDING_EVENT:
            HandleRecordingEvent(std::unique_ptr<RecordingEvent>(reinterpret_cast<RecordingEvent*>(lParam)));
            return 0;
        case WM_APP_PREVIEW_FRAME:
            HandlePreviewFrame(std::unique_ptr<PreviewFrame>(reinterpret_cast<PreviewFrame*>(lParam)));
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
