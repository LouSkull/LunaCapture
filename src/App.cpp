#include "App.hpp"

#include "CoreUtils.hpp"
#include "UiGfx.hpp"
#include "UiIcons.hpp"
#include "WinUtil.hpp"
#include "resource.h"

#include <windowsx.h>
#include <shellapi.h>
#include <hidusage.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <cwchar>
#include <memory>
#include <new>
#include <sstream>
#include <unordered_map>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {
constexpr const wchar_t* kMainClassName = kMainWindowClassName;
constexpr int kWindowWidth = 1000;
constexpr int kWindowHeight = 650;
constexpr int kWindowCornerRadius = 22;
constexpr int kSidebarWidth = 196;
constexpr int kTopBarHeight = 64;
constexpr UINT_PTR kAnimationTimer = 1;
// The app usually lives in the tray for days; re-check GitHub for releases periodically so an
// update published after launch still surfaces without a restart.
constexpr UINT_PTR kUpdateRecheckTimer = 2;
constexpr UINT kUpdateRecheckIntervalMs = 6 * 60 * 60 * 1000;
constexpr int kReplayPreviewFps = 8;
constexpr int kQuickPanelHotkeySlot = 6 + static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS);
constexpr std::size_t kQuickPanelPluginCommandLimit = 3;
constexpr std::size_t kTrayPluginCommandLimit = 4;
constexpr std::size_t kPluginRootInstructionBudget = 1024;

bool IsPluginMediaLifecycleEvent(std::wstring_view eventName) {
    return eventName.starts_with(L"capture_") || eventName.starts_with(L"recording_") ||
           eventName.starts_with(L"replay_");
}

class ScopedCounter {
public:
    explicit ScopedCounter(int& value) : value_(value) { ++value_; }
    ~ScopedCounter() { --value_; }
    ScopedCounter(const ScopedCounter&) = delete;
    ScopedCounter& operator=(const ScopedCounter&) = delete;
private:
    int& value_;
};

class PluginExecutionBudgetScope {
public:
    PluginExecutionBudgetScope(int& depth, std::size_t& remaining)
        : depth_(depth), remaining_(remaining), root_(depth == 0) {
        if (root_) remaining_ = kPluginRootInstructionBudget;
        ++depth_;
    }
    ~PluginExecutionBudgetScope() {
        --depth_;
        if (root_) remaining_ = 0;
    }
    PluginExecutionBudgetScope(const PluginExecutionBudgetScope&) = delete;
    PluginExecutionBudgetScope& operator=(const PluginExecutionBudgetScope&) = delete;
private:
    int& depth_;
    std::size_t& remaining_;
    bool root_ = false;
};

class ScopedWinHandle {
public:
    explicit ScopedWinHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~ScopedWinHandle() { if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) CloseHandle(value_); }
    ScopedWinHandle(const ScopedWinHandle&) = delete;
    ScopedWinHandle& operator=(const ScopedWinHandle&) = delete;
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

std::wstring FinalPathForHandle(HANDLE handle) {
    std::wstring path(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    while (path.size() > 4 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

bool IsStrictChildPath(const std::wstring& root, const std::wstring& child) {
    if (root.empty() || child.size() <= root.size() || root.size() > static_cast<std::size_t>(INT_MAX)) return false;
    if (CompareStringOrdinal(root.data(), static_cast<int>(root.size()), child.data(),
                             static_cast<int>(root.size()), TRUE) != CSTR_EQUAL) return false;
    return child[root.size()] == L'\\' || child[root.size()] == L'/';
}

bool ReadThemeAssetSnapshot(const std::filesystem::path& pluginDirectory,
                            const std::filesystem::path& relative,
                            std::vector<std::uint8_t>& bytes) {
    bytes.clear();
    ScopedWinHandle root(CreateFileW(pluginDirectory.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!root) return false;

    const std::filesystem::path requested = pluginDirectory / relative;
    ScopedWinHandle asset(CreateFileW(requested.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!asset) return false;

    const std::wstring finalRoot = FinalPathForHandle(root.get());
    const std::wstring finalAsset = FinalPathForHandle(asset.get());
    if (!IsStrictChildPath(finalRoot, finalAsset)) return false;

    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(asset.get(), &information) ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    ULARGE_INTEGER size{};
    size.LowPart = information.nFileSizeLow;
    size.HighPart = information.nFileSizeHigh;
    constexpr ULONGLONG kMaximumThemeAssetBytes = 16ULL * 1024ULL * 1024ULL;
    if (size.QuadPart == 0 || size.QuadPart > kMaximumThemeAssetBytes) return false;

    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t total = 0;
    while (total < bytes.size()) {
        DWORD read = 0;
        const DWORD wanted = static_cast<DWORD>((std::min<std::size_t>)(bytes.size() - total, 1024 * 1024));
        if (!ReadFile(asset.get(), bytes.data() + total, wanted, &read, nullptr) || read == 0) {
            bytes.clear();
            return false;
        }
        total += read;
    }
    return true;
}

std::size_t FindPluginIndexByIdentity(const PluginSystem& system, const std::wstring& pluginId,
                                      const std::filesystem::path& sourcePath = {}) {
    const auto& plugins = system.Plugins();
    for (std::size_t index = 0; index < plugins.size(); ++index) {
        const bool idMatches = pluginId.empty() ||
            _wcsicmp(plugins[index].definition.id.c_str(), pluginId.c_str()) == 0;
        if (!sourcePath.empty()) {
            const bool pathMatches = _wcsicmp(plugins[index].sourcePath.c_str(), sourcePath.c_str()) == 0;
            if (pathMatches && idMatches) return index;
            continue;
        }
        if (!pluginId.empty() && idMatches) return index;
    }
    return static_cast<std::size_t>(-1);
}

std::uint32_t LimitReplayMonitorMask(std::uint32_t mask) {
    std::uint32_t limited = 0;
    std::size_t count = 0;
    for (std::size_t index = 0; index < LUNASHOT_MAX_REPLAY_MONITORS &&
                                count < LUNASHOT_MAX_ACTIVE_REPLAY_MONITORS; ++index) {
        const std::uint32_t bit = 1u << index;
        if ((mask & bit) == 0) continue;
        limited |= bit;
        ++count;
    }
    return limited;
}

COLORREF Mix(COLORREF a, COLORREF b, float t) {
    t = (std::max)(0.0f, (std::min)(1.0f, t));
    const auto mix = [t](BYTE x, BYTE y) { return static_cast<BYTE>(x + (y - x) * t); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

void Fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(ui::ResolveThemeColor(color));
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
    return ui::CreateThemeFont(pixels, weight);
}

void Text(HDC dc, const std::wstring& value, RECT rect, COLORREF color, HFONT font,
          UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    const std::wstring translated = ui::TranslateText(value);
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ui::ResolveThemeColor(color));
    DrawTextW(dc, translated.c_str(), -1, &rect, flags | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

// Height the translated text needs when wrapped into `width`. Layout that sizes
// itself from this survives both a language pack with longer strings and a theme
// that scales the font up — neither can be predicted from a hardcoded rect.
int MeasuredTextHeight(HDC dc, const std::wstring& value, HFONT font, int width) {
    const std::wstring translated = ui::TranslateText(value);
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT measure{0, 0, (std::max)(1, width), 0};
    DrawTextW(dc, translated.c_str(), -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, oldFont);
    return measure.bottom - measure.top;
}

void FillThemeRole(HDC dc, const RECT& rect, ui::ThemeRole role) {
    ui::ScopedThemeBypass bypass;
    Fill(dc, rect, ui::ThemeColor(role));
}

void FillRoundThemeRole(HDC dc, const RECT& rect, int radius, ui::ThemeRole role) {
    ui::ScopedThemeBypass bypass;
    FillRound(dc, rect, radius, ui::ThemeColor(role));
}

void TextThemeRole(HDC dc, const std::wstring& value, RECT rect, ui::ThemeRole role, HFONT font,
                   UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    ui::ScopedThemeBypass bypass;
    Text(dc, value, rect, ui::ThemeColor(role), font, flags);
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

// A left/right double-chevron ("skip") glyph drawn anti-aliased.
void DrawSkipGlyph(HDC dc, const RECT& rect, COLORREF color, bool forward) {
    ui::Canvas canvas(dc);
    if (!canvas.ok()) return;
    Gdiplus::Graphics& g = canvas.g();
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;
    const float w = 5.0f, h = 7.0f, gap = 5.0f;
    Gdiplus::SolidBrush brush(ui::FromColorRef(color));
    const float dir = forward ? 1.0f : -1.0f;
    for (int i = 0; i < 2; ++i) {
        const float ox = cx + dir * (i * gap - gap / 2.0f);
        Gdiplus::PointF tri[3];
        if (forward) {
            tri[0] = Gdiplus::PointF(ox - w, cy - h);
            tri[1] = Gdiplus::PointF(ox + w, cy);
            tri[2] = Gdiplus::PointF(ox - w, cy + h);
        } else {
            tri[0] = Gdiplus::PointF(ox + w, cy - h);
            tri[1] = Gdiplus::PointF(ox - w, cy);
            tri[2] = Gdiplus::PointF(ox + w, cy + h);
        }
        g.FillPolygon(&brush, tri, 3);
    }
}

// Four corner brackets — the standard fullscreen glyph. `exit` draws them
// pointing inward instead of outward.
void DrawFullscreenGlyph(HDC dc, const RECT& rect, COLORREF color, bool exit) {
    ui::Canvas canvas(dc);
    if (!canvas.ok()) return;
    Gdiplus::Graphics& g = canvas.g();
    Gdiplus::Pen pen(ui::FromColorRef(color), 2.0f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    const float l = static_cast<float>(rect.left) + 3.0f;
    const float t = static_cast<float>(rect.top) + 3.0f;
    const float r = static_cast<float>(rect.right) - 3.0f;
    const float b = static_cast<float>(rect.bottom) - 3.0f;
    const float arm = 6.0f;
    if (!exit) {
        g.DrawLine(&pen, l, t + arm, l, t); g.DrawLine(&pen, l, t, l + arm, t);
        g.DrawLine(&pen, r - arm, t, r, t); g.DrawLine(&pen, r, t, r, t + arm);
        g.DrawLine(&pen, l, b - arm, l, b); g.DrawLine(&pen, l, b, l + arm, b);
        g.DrawLine(&pen, r - arm, b, r, b); g.DrawLine(&pen, r, b, r, b - arm);
    } else {
        const float cx0 = l + arm, cy0 = t + arm, cx1 = r - arm, cy1 = b - arm;
        g.DrawLine(&pen, l, cy0, cx0, cy0); g.DrawLine(&pen, cx0, t, cx0, cy0);
        g.DrawLine(&pen, cx1, t, cx1, cy0); g.DrawLine(&pen, cx1, cy0, r, cy0);
        g.DrawLine(&pen, l, cy1, cx0, cy1); g.DrawLine(&pen, cx0, cy1, cx0, b);
        g.DrawLine(&pen, cx1, cy1, cx1, b); g.DrawLine(&pen, cx1, cy1, r, cy1);
    }
}

// Draws a top-down 32bpp BGRA buffer scaled to fit (preserving aspect ratio,
// centered) inside rect, used for both grid thumbnails and the detail image.
void DrawBgraFit(HDC dc, const RECT& rect, const std::vector<std::uint8_t>& bgra, int width, int height) {
    if (bgra.empty() || width <= 0 || height <= 0) return;
    const int rectWidth = rect.right - rect.left;
    const int rectHeight = rect.bottom - rect.top;
    if (rectWidth <= 0 || rectHeight <= 0) return;
    const double scale = (std::min)(static_cast<double>(rectWidth) / width, static_cast<double>(rectHeight) / height);
    const int drawWidth = (std::max)(1, static_cast<int>(width * scale));
    const int drawHeight = (std::max)(1, static_cast<int>(height * scale));
    const int drawX = rect.left + (rectWidth - drawWidth) / 2;
    const int drawY = rect.top + (rectHeight - drawHeight) / 2;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, drawX, drawY, drawWidth, drawHeight, 0, 0, width, height, bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
}

std::wstring PathFileName(const std::wstring& path) {
    if (path.empty()) return L"No screenshots yet";
    return std::filesystem::path(path).filename().wstring();
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
    if (slot == kQuickPanelHotkeySlot) return L"quick panel toggle";
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

std::wstring LowercaseExtension(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return extension;
}

std::wstring FormatMediaSize(std::uintmax_t bytes) {
    wchar_t buffer[64]{};
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        swprintf_s(buffer, L"%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        swprintf_s(buffer, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        swprintf_s(buffer, L"%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        swprintf_s(buffer, L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

std::wstring FormatMediaTime(const std::filesystem::file_time_type& value) {
    const auto systemValue = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        value - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t raw = std::chrono::system_clock::to_time_t(systemValue);
    std::tm local{};
    if (localtime_s(&local, &raw) != 0) return L"Unknown time";
    wchar_t buffer[64]{};
    wcsftime(buffer, std::size(buffer), L"%d %b · %H:%M", &local);
    return buffer;
}

// Short chip label. Kept narrow enough for the encoder card's value chip; the
// picker below carries the full description.
const wchar_t* VideoCodecLabel(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::Hevc: return L"HEVC";
        case VideoCodec::Av1: return L"AV1";
        case VideoCodec::Vp9: return L"VP9";
        case VideoCodec::H264Fragmented: return L"H.264 f";
        case VideoCodec::HevcFragmented: return L"HEVC f";
        case VideoCodec::Mpeg4Part2: return L"MPEG-4";
        case VideoCodec::H263: return L"H.263";
        case VideoCodec::Wmv3: return L"WMV9";
        case VideoCodec::Vc1: return L"VC-1";
        case VideoCodec::MotionJpeg: return L"MJPEG";
        case VideoCodec::H264Avi: return L"H.264 AVI";
        default: return L"H.264";
    }
}

// Container the chosen codec is muxed into — surfaced next to the codec so the
// resulting file extension is never a surprise. Mirrors CodecProfile() in
// RecordingEngine.cpp.
const wchar_t* VideoContainerLabel(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::H263: return L"3GP";
        case VideoCodec::Wmv3:
        case VideoCodec::Vc1: return L"WMV";
        case VideoCodec::MotionJpeg:
        case VideoCodec::H264Avi: return L"AVI";
        case VideoCodec::H264Fragmented:
        case VideoCodec::HevcFragmented: return L"fragmented MP4";
        default: return L"MP4";
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
        const UINT targetWidth = static_cast<UINT>(frame->virtualRect.right - frame->virtualRect.left);
        const UINT targetHeight = static_cast<UINT>(frame->virtualRect.bottom - frame->virtualRect.top);
        if (!width || !height || targetWidth > combined->width - static_cast<UINT>(destinationX) ||
            targetHeight > combined->height - static_cast<UINT>(destinationY) ||
            static_cast<std::uint64_t>(frame->stride) * frame->height > frame->bgra.size()) continue;
        auto* destination = combined->bgra.data() +
            static_cast<std::size_t>(static_cast<UINT>(destinationY)) * combined->stride +
            static_cast<std::size_t>(destinationX) * 4;
        // Bilinear, not nearest-neighbor: on a DPI-scaled monitor the DXGI-captured frame's pixel
        // size and its virtual-desktop rect size are not equal, so this is a real resample, not a
        // 1:1 copy - nearest-neighbor turned that into a visibly blocky/pixelated screenshot.
        BlitBgraScaledBilinear(frame->bgra.data(), frame->width, frame->height, frame->stride,
                               destination, targetWidth, targetHeight, combined->stride);
    }
    return combined;
}
}

App* App::globalHotkeyInstance_ = nullptr;
App* App::hotkeyRecordingInstance_ = nullptr;

App::App(HINSTANCE instance) : instance_(instance), settings_(settingsStore_.Load()) {
    bool settingsChanged = false;
    const std::uint32_t limitedMask = LimitReplayMonitorMask(settings_.replayMonitorMask);
    if (limitedMask != settings_.replayMonitorMask) {
        settings_.replayMonitorMask = limitedMask;
        settingsChanged = true;
    }
    if (settingsChanged) settingsStore_.Save(settings_);
}
App::~App() { ExitApplication(); }

bool App::Initialize(bool startHidden) {
    if (settings_.startWithWindows) {
        // Windows can silently flip a Startup-folder entry to "disabled" (Task Manager's
        // Startup apps tab, Settings > Apps > Startup) without ever touching our setting or
        // the .lnk file. Re-assert it on every launch so a previously-disabled entry from an
        // older build, or from the user experimenting, doesn't keep failing to auto-launch
        // forever just because nobody happened to re-toggle the checkbox in-app.
        settingsStore_.ApplyAutostart(true);
    }
    if (pluginSystem_.Initialize()) {
        pluginStatus_ = pluginSystem_.LastReloadSummary();
        ApplyPluginTheme();
        if (!themeAssetStatus_.empty()) pluginStatus_ += L" · Theme: " + themeAssetStatus_;
    } else {
        pluginStatus_ = L"Plugin directory could not be initialized";
    }
    if (!CreateMainWindow(startHidden || settings_.startHiddenToTray)) return false;
    StartGlobalKeyboardHook();
    if (!overlay_.Create(instance_)) return false;
    if (!quickPanel_.Create(instance_, [this](QuickPanelAction action, std::size_t dataIndex) { HandleQuickPanelAction(action, dataIndex); })) return false;
    if (!regionEditor_.Create(instance_, hwnd_)) return false;
    if (!trayMenu_.Create(instance_, hwnd_)) return false;
    if (!mediaViewer_.CreateHost(instance_, hwnd_)) return false;
    thumbnailCache_.Start(hwnd_, WM_APP_THUMBNAIL_READY);
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
    RefreshMediaLibrary(false);
    RefreshMediaLibrary(true);
    RegisterAllHotkeys();
    if (settings_.resumeReplayOnLaunch && recordingEngine_) {
        // The buffer was left running last time the app closed (or Windows relaunched it via
        // autostart); resume it automatically instead of silently leaving it off until the user
        // notices and re-enables it by hand.
        ToggleReplayBuffer();
        if (IsReplayBuffering() && settings_.notifications) {
            overlay_.ShowMessage(L"Replay buffer resumed", L"It was on last time, so Luna Capture turned it back on",
                                 settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
        }
    }
    // One lightweight GitHub request per launch, plus a periodic re-check because the app can sit
    // in the tray for days. The old 20-hour launch throttle meant a release published right after
    // a check ran was invisible until the next day's launch — exactly the "there IS a newer
    // version but no banner ever appears" report.
    updateUiState_ = UpdateUiState::Checking;
    StartUpdateCheck(hwnd_, WM_APP_UPDATE_CHECK);
    SetTimer(hwnd_, kUpdateRecheckTimer, kUpdateRecheckIntervalMs, nullptr);

    // Keep Explorer sorting the capture folders chronologically instead of by the
    // "Date taken" metadata that screen captures never carry.
    ApplyChronologicalFolderView(settings_.saveDirectory);
    ApplyChronologicalFolderView(settings_.recordingDirectory);

    DispatchPluginEvent(L"app_started");
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

    // When Luna Capture runs elevated, User Interface Privilege Isolation blocks
    // messages from normal-integrity processes — including the copy Explorer
    // starts when you double-click a .luna file. Without this the plugin handoff
    // silently does nothing. WM_COPYDATA is designed for exactly this and is safe
    // to allow: the handler validates the tag, length and contents.
    ChangeWindowMessageFilterEx(hwnd_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);

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
    fontTiny_ = MakeFont(11, FW_NORMAL);
    fontControl_ = MakeFont(18, FW_NORMAL);
    if (!fontLogo_ || !fontHeadline_ || !fontTitle_ || !fontNormal_ || !fontSmall_ || !fontTiny_ || !fontControl_) {
        DestroyUiResources();
        return false;
    }
    return true;
}

void App::RefreshThemeFonts() {
    if (!hwnd_ || !fontLogo_) return;
    const std::array<HFONT, 7> replacements{
        MakeFont(19, FW_SEMIBOLD), MakeFont(25, FW_SEMIBOLD), MakeFont(16, FW_SEMIBOLD),
        MakeFont(14, FW_NORMAL), MakeFont(13, FW_NORMAL), MakeFont(11, FW_NORMAL), MakeFont(18, FW_NORMAL)};
    if (std::any_of(replacements.begin(), replacements.end(), [](HFONT font) { return font == nullptr; })) {
        for (HFONT font : replacements) if (font) DeleteObject(font);
        return;
    }
    const std::array<HFONT, 7> previous{fontLogo_, fontHeadline_, fontTitle_, fontNormal_, fontSmall_, fontTiny_, fontControl_};
    fontLogo_ = replacements[0];
    fontHeadline_ = replacements[1];
    fontTitle_ = replacements[2];
    fontNormal_ = replacements[3];
    fontSmall_ = replacements[4];
    fontTiny_ = replacements[5];
    fontControl_ = replacements[6];
    for (HFONT font : previous) if (font) DeleteObject(font);
}

void App::DestroyUiResources() {
    if (backBufferDc_ && backBufferOldBitmap_) {
        SelectObject(backBufferDc_, backBufferOldBitmap_);
        backBufferOldBitmap_ = nullptr;
    }
    if (backBufferBitmap_) { DeleteObject(backBufferBitmap_); backBufferBitmap_ = nullptr; }
    backBufferPixels_ = nullptr;
    if (backBufferDc_) { DeleteDC(backBufferDc_); backBufferDc_ = nullptr; }

    const HFONT fonts[] = {fontLogo_, fontHeadline_, fontTitle_, fontNormal_, fontSmall_, fontTiny_, fontControl_};
    for (HFONT font : fonts) if (font) DeleteObject(font);
    fontLogo_ = nullptr;
    fontHeadline_ = nullptr;
    fontTitle_ = nullptr;
    fontNormal_ = nullptr;
    fontSmall_ = nullptr;
    fontTiny_ = nullptr;
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
    const std::wstring translatedTitle = ui::TranslateText(title);
    const std::wstring translatedDetail = ui::TranslateText(detail);
    wcsncpy_s(notification.szInfoTitle, _countof(notification.szInfoTitle), translatedTitle.c_str(), _TRUNCATE);
    wcsncpy_s(notification.szInfo, _countof(notification.szInfo), translatedDetail.c_str(), _TRUNCATE);
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
    trayPluginCommands_ = BuildTrayPluginCommands();
    trayMenu_.Show(cursor, settings_, recordingEngine_ && recordingEngine_->IsRecording(),
                   IsReplayBuffering(), activeReplayMask, trayPluginCommands_);
}

void App::ShowMainWindow(Page page) {
    // A normal window can never be composited over true exclusive fullscreen —
    // DWM stops compositing that surface entirely. Showing it anyway means
    // fighting Windows for a result nobody can see, and hammering the z-order
    // against an exclusive-fullscreen surface has previously destabilised the
    // GPU/DWM badly enough to hang the whole machine. So: check first, and
    // refuse rather than half-open behind the game.
    if (IsExclusiveFullscreenAppActive()) {
        if (settings_.notifications) {
            overlay_.ShowWarning(L"Can't open over fullscreen",
                                 L"A game is running in exclusive fullscreen. Alt-Tab out of it, then open "
                                 L"Luna Capture from the tray again.",
                                 settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
        }
        return;
    }

    page_ = page;
    if (page != Page::Record) ReleasePreviewFrame();
    if (page != Page::Replay) ReleaseReplayPreviewFrames();
    if (page == Page::Replay) recordingOutputs_ = CaptureEngine::EnumerateOutputs();
    if (page == Page::Photos) RefreshMediaLibrary(false);
    if (page == Page::Videos) RefreshMediaLibrary(true);

    ShowWindow(hwnd_, SW_SHOW);
    // Re-enter the top-most band rather than trusting a stale z-order: opening
    // from the tray while a borderless-fullscreen game owns the screen would
    // otherwise put the window behind it. This is a one-shot placement, not a
    // repeating re-assert — the repeating kind is what caused the hang above.
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    BringWindowToTop(hwnd_);
    SetForegroundWindow(hwnd_);
    InvalidateUi();
}

void App::HideMainWindow() {
    if (engine_) engine_->SetPreviewConsumerActive(false);
    if (recordingEngine_) recordingEngine_->SetPreviewConsumerActive(false);
    for (auto& channel : auxiliaryReplayEngines_) {
        if (channel.engine) channel.engine->SetPreviewConsumerActive(false);
    }
    ReleasePreviewFrame();
    ReleaseReplayPreviewFrames();
    ShowWindow(hwnd_, SW_HIDE);
}

void App::RefreshMediaLibrary(bool videos) {
    auto& library = videos ? videoLibrary_ : photoLibrary_;
    // Carry over already-decoded thumbnails for files that are still present
    // instead of throwing them away and re-decoding on every refresh.
    std::unordered_map<std::wstring, MediaEntry> previousByPath;
    for (auto& entry : library) previousByPath.emplace(entry.path.wstring(), std::move(entry));
    library.clear();

    const std::filesystem::path root = videos ? settings_.recordingDirectory : settings_.saveDirectory;
    std::error_code error;
    if (root.empty() || !std::filesystem::is_directory(root, error)) return;
    const auto supported = [videos](const std::filesystem::path& path) {
        const std::wstring extension = LowercaseExtension(path);
        if (videos) return extension == L".mp4" || extension == L".m4v" || extension == L".mov" ||
                           extension == L".wmv" || extension == L".avi";
        return extension == L".png" || extension == L".jpg" || extension == L".jpeg" ||
               extension == L".bmp" || extension == L".webp" || extension == L".tif" || extension == L".tiff";
    };
    for (std::filesystem::directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        std::error_code itemError;
        if (!it->is_regular_file(itemError) || itemError || !supported(it->path())) continue;
        MediaEntry entry;
        entry.path = it->path();
        entry.name = entry.path.filename().wstring();
        entry.sizeBytes = it->file_size(itemError);
        if (itemError) entry.sizeBytes = 0;
        entry.modified = it->last_write_time(itemError);
        const auto previous = previousByPath.find(entry.path.wstring());
        if (previous != previousByPath.end()) {
            entry.thumbnailBgra = std::move(previous->second.thumbnailBgra);
            entry.thumbnailWidth = previous->second.thumbnailWidth;
            entry.thumbnailHeight = previous->second.thumbnailHeight;
        }
        library.push_back(std::move(entry));
    }
    std::sort(library.begin(), library.end(), [](const MediaEntry& left, const MediaEntry& right) {
        const bool leftChronological = HasChronologicalMediaFilename(left.name);
        const bool rightChronological = HasChronologicalMediaFilename(right.name);
        if (leftChronological && rightChronological && left.name != right.name) return left.name > right.name;
        if (left.modified != right.modified) return left.modified > right.modified;
        return left.name > right.name;
    });
    if (library.size() > 512) library.resize(512);
    // A failed decode never posts WM_APP_THUMBNAIL_READY, so re-requesting
    // every refresh (rather than latching a one-shot "already tried" flag) is
    // what lets a thumbnail recover after a transient failure. Already-queued
    // paths are deduped inside ThumbnailCache::Request().
    for (auto& entry : library) {
        if (entry.thumbnailBgra.empty()) thumbnailCache_.Request(entry.path, videos);
    }
    pageScrollY_[static_cast<std::size_t>(videos ? Page::Videos : Page::Photos)] = 0;
    InvalidateUi();
}

void App::HandleThumbnailReady(std::unique_ptr<ThumbnailResult> result) {
    if (!result) return;
    auto& library = result->isVideo ? videoLibrary_ : photoLibrary_;
    for (auto& entry : library) {
        if (entry.path != result->path) continue;
        entry.thumbnailBgra = std::move(result->bgra);
        entry.thumbnailWidth = static_cast<int>(result->width);
        entry.thumbnailHeight = static_cast<int>(result->height);
        if ((result->isVideo && page_ == Page::Videos) || (!result->isVideo && page_ == Page::Photos)) InvalidateUi();
        break;
    }
}

void App::OpenMediaItem(bool videos, std::size_t index) { OpenMediaDetail(videos, index); }

void App::OpenMediaDetail(bool videos, std::size_t index) {
    const auto& library = videos ? videoLibrary_ : photoLibrary_;
    if (index >= library.size()) return;
    mediaDetailOpen_ = true;
    mediaDetailIsVideo_ = videos;
    mediaDetailIndex_ = index;
    mediaDetailFullscreen_ = false;
    mediaSeekDragging_ = false;
    mediaVolumeDragging_ = false;
    if (videos) {
        mediaViewer_.UnloadVideo();
        std::wstring error;
        if (!mediaViewer_.LoadVideo(library[index].path.wstring(), error)) mediaDetailStatus_ = error;
        else mediaDetailStatus_.clear();
        UpdateMediaDetailVideoBounds();
        mediaViewer_.SetVisible(true);
    } else {
        mediaViewer_.SetVisible(false);
        if (!LoadMediaDetailImage(library[index].path)) mediaDetailStatus_ = L"Could not decode this image";
        else mediaDetailStatus_.clear();
    }
    InvalidateUi();
}

void App::CloseMediaDetail() {
    if (!mediaDetailOpen_) return;
    // Leave true fullscreen before tearing the detail view down, or the detached
    // video surface would stay on top of the desktop with nothing driving it.
    if (mediaDetailFullscreen_) mediaViewer_.SetFullscreen(false, hwnd_);
    mediaDetailOpen_ = false;
    mediaDetailFullscreen_ = false;
    mediaViewer_.SetVisible(false);
    mediaViewer_.UnloadVideo();
    mediaDetailImageBgra_.clear();
    mediaDetailImageWidth_ = mediaDetailImageHeight_ = 0;
    InvalidateUi();
}

void App::StepMediaDetail(int delta) {
    if (!mediaDetailOpen_) return;
    const auto& library = mediaDetailIsVideo_ ? videoLibrary_ : photoLibrary_;
    if (library.empty()) return;
    const long next = static_cast<long>(mediaDetailIndex_) + delta;
    if (next < 0 || next >= static_cast<long>(library.size())) return;
    OpenMediaDetail(mediaDetailIsVideo_, static_cast<std::size_t>(next));
}

void App::DeleteMediaDetailItem() {
    if (!mediaDetailOpen_) return;
    auto& library = mediaDetailIsVideo_ ? videoLibrary_ : photoLibrary_;
    if (mediaDetailIndex_ >= library.size()) return;
    const auto& entry = library[mediaDetailIndex_];
    const std::filesystem::path path = entry.path;
    // A modal behind a fullscreen video surface is invisible — drop out first.
    if (mediaDetailFullscreen_) SetMediaDetailFullscreen(false);
    const std::wstring detail = EllipsizeMiddle(entry.name, 46) + L"\n" +
                                FormatMediaSize(entry.sizeBytes) + L"  ·  " + FormatMediaTime(entry.modified) +
                                L"\n\nThis permanently removes the file from disk and cannot be undone.";
    if (!ConfirmAction(hwnd_, mediaDetailIsVideo_ ? L"Delete this recording?" : L"Delete this screenshot?",
                       detail, L"Delete", true)) {
        return;
    }
    if (mediaDetailIsVideo_) { mediaViewer_.SetVisible(false); mediaViewer_.UnloadVideo(); }
    std::error_code error;
    std::filesystem::remove(path, error);
    RefreshMediaLibrary(mediaDetailIsVideo_);
    if (library.empty()) {
        CloseMediaDetail();
    } else {
        const std::size_t next = (std::min)(mediaDetailIndex_, library.size() - 1);
        OpenMediaDetail(mediaDetailIsVideo_, next);
    }
}

bool App::LoadMediaDetailImage(const std::filesystem::path& path) {
    mediaDetailImageBgra_.clear();
    mediaDetailImageWidth_ = mediaDetailImageHeight_ = 0;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    UINT sourceWidth = 0, sourceHeight = 0;
    if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) || !sourceWidth || !sourceHeight) return false;
    if (sourceWidth > 32768 || sourceHeight > 32768) return false;

    UINT width = sourceWidth, height = sourceHeight;
    ComPtr<IWICBitmapSource> source = frame;
    constexpr UINT kMaximumDecodeWidth = 2560, kMaximumDecodeHeight = 1440;
    ComPtr<IWICBitmapScaler> scaler;
    if (sourceWidth > kMaximumDecodeWidth || sourceHeight > kMaximumDecodeHeight) {
        const double scale = (std::min)(static_cast<double>(kMaximumDecodeWidth) / sourceWidth,
                                        static_cast<double>(kMaximumDecodeHeight) / sourceHeight);
        width = (std::max)(1u, static_cast<UINT>(sourceWidth * scale));
        height = (std::max)(1u, static_cast<UINT>(sourceHeight * scale));
        if (FAILED(factory->CreateBitmapScaler(&scaler))) return false;
        if (FAILED(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant))) return false;
        source = scaler;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return false;
    if (FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom))) return false;
    const UINT stride = width * 4;
    mediaDetailImageBgra_.resize(static_cast<std::size_t>(stride) * height);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(mediaDetailImageBgra_.size()),
                                     mediaDetailImageBgra_.data()))) {
        mediaDetailImageBgra_.clear();
        return false;
    }
    mediaDetailImageWidth_ = width;
    mediaDetailImageHeight_ = height;
    return true;
}

RECT App::MediaDetailCanvasRect() const {
    const int controlsHeight = mediaDetailIsVideo_ ? 74 : 0;
    if (mediaDetailFullscreen_) return RECT{0, kTopBarHeight, kWindowWidth, kWindowHeight - controlsHeight};
    return RECT{kSidebarWidth + 16, kTopBarHeight + 64, kWindowWidth - 16, kWindowHeight - 24 - controlsHeight};
}

void App::UpdateMediaDetailVideoBounds() {
    if (!mediaDetailOpen_ || !mediaDetailIsVideo_) return;
    mediaViewer_.SetBounds(MediaDetailCanvasRect());
}

void App::SetMediaDetailFullscreen(bool enable) {
    if (!mediaDetailOpen_ || !mediaDetailIsVideo_) return;
    if (mediaDetailFullscreen_ == enable) return;
    mediaDetailFullscreen_ = enable;
    // Real fullscreen: the video surface detaches from this fixed-size window and
    // covers the whole monitor. The app window keeps its own layout underneath.
    mediaViewer_.SetFullscreen(enable, hwnd_);
    if (!enable) UpdateMediaDetailVideoBounds();
    InvalidateUi();
}

void App::SeekMediaDetailFromPoint(int x) {
    if (mediaProgressRect_.right <= mediaProgressRect_.left) return;
    const double ratio = static_cast<double>(x - mediaProgressRect_.left) /
                         (mediaProgressRect_.right - mediaProgressRect_.left);
    mediaViewer_.SetPositionRatio((std::max)(0.0, (std::min)(1.0, ratio)));
}

void App::VolumeMediaDetailFromPoint(int x) {
    if (mediaVolumeRect_.right <= mediaVolumeRect_.left) return;
    const float ratio = static_cast<float>(x - mediaVolumeRect_.left) /
                        static_cast<float>(mediaVolumeRect_.right - mediaVolumeRect_.left);
    mediaViewer_.SetVolume((std::max)(0.0f, (std::min)(1.0f, ratio)));
}

QuickPanelState App::BuildQuickPanelState() {
    QuickPanelState state;
    state.recording = recordingEngine_ && recordingEngine_->IsRecording();
    state.replay = IsReplayBuffering();
    state.replaySeconds = settings_.replaySeconds;
    if (recordingOutputs_.empty()) recordingOutputs_ = CaptureEngine::EnumerateOutputs();
    quickPanelMonitorIndices_.fill(-1);

    const auto channelLive = [this](int monitorIndex) {
        if (recordingEngine_ && recordingEngine_->IsReplayBuffering() && primaryReplayMonitorIndex_ == monitorIndex) return true;
        for (const auto& channel : auxiliaryReplayEngines_) {
            if (channel.monitorIndex == monitorIndex && channel.engine && channel.engine->IsReplayBuffering()) return true;
        }
        return false;
    };
    std::vector<const OutputInfo*> ordered;
    const auto append = [&ordered](const OutputInfo& output) {
        if (ordered.size() >= 3) return;
        if (std::find(ordered.begin(), ordered.end(), &output) == ordered.end()) ordered.push_back(&output);
    };
    for (const auto& output : recordingOutputs_) if (channelLive(output.linearIndex)) append(output);
    for (const auto& output : recordingOutputs_) {
        if (output.linearIndex >= 0 && output.linearIndex < 32 &&
            (settings_.replayMonitorMask & (1u << output.linearIndex)) != 0) append(output);
    }
    for (const auto& output : recordingOutputs_) append(output);
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        quickPanelMonitorIndices_[index] = ordered[index]->linearIndex;
        state.monitorLabels[index] = L"M" + std::to_wstring(ordered[index]->linearIndex + 1);
        state.monitorAvailable[index] = channelLive(ordered[index]->linearIndex);
    }
    state.monitorCount = static_cast<int>(ordered.size());

    // Surface commands from enabled plugins so they're reachable from Quick Capture,
    // not just the main Plugins page.
    struct Candidate {
        int order = 500;
        QuickPanelPluginCommand command;
    };
    std::vector<Candidate> candidates;
    const auto& plugins = pluginSystem_.Plugins();
    for (std::size_t pluginIndex = 0; pluginIndex < plugins.size(); ++pluginIndex) {
        const auto& plugin = plugins[pluginIndex];
        if (!plugin.valid || !plugin.enabled) continue;
        for (std::size_t commandIndex = 0; commandIndex < plugin.definition.commands.size(); ++commandIndex) {
            const auto& command = plugin.definition.commands[commandIndex];
            if (!LunaScript::HasCommandSurface(command, LunaCommandSurface::QuickPanel)) continue;
            QuickPanelPluginCommand entry;
            entry.title = command.title.empty() ? plugin.DisplayName() : command.title;
            entry.description = command.description.empty() ? plugin.DisplayName() : command.description;
            entry.icon = command.icon;
            entry.pluginId = plugin.definition.id;
            entry.commandId = command.id;
            entry.pluginIndex = pluginIndex;
            entry.commandIndex = commandIndex;
            candidates.push_back({command.order, std::move(entry)});
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.order < right.order;
    });
    quickPanelPluginCommands_.clear();
    quickPanelPluginCommands_.reserve((std::min)(candidates.size(), kQuickPanelPluginCommandLimit));
    for (std::size_t index = 0; index < candidates.size() && index < kQuickPanelPluginCommandLimit; ++index) {
        quickPanelPluginCommands_.push_back(std::move(candidates[index].command));
    }
    state.pluginCommands = quickPanelPluginCommands_;
    return state;
}

std::vector<TrayPluginCommand> App::BuildTrayPluginCommands() const {
    struct Candidate {
        int order = 500;
        TrayPluginCommand command;
    };
    std::vector<Candidate> candidates;
    const auto& plugins = pluginSystem_.Plugins();
    for (std::size_t pluginIndex = 0; pluginIndex < plugins.size(); ++pluginIndex) {
        const auto& plugin = plugins[pluginIndex];
        if (!plugin.valid || !plugin.enabled) continue;
        for (std::size_t commandIndex = 0; commandIndex < plugin.definition.commands.size(); ++commandIndex) {
            const auto& command = plugin.definition.commands[commandIndex];
            if (!LunaScript::HasCommandSurface(command, LunaCommandSurface::TrayMenu)) continue;
            TrayPluginCommand entry;
            entry.title = command.title.empty() ? plugin.DisplayName() : command.title;
            entry.icon = command.icon;
            entry.pluginId = plugin.definition.id;
            entry.commandId = command.id;
            entry.pluginIndex = pluginIndex;
            entry.commandIndex = commandIndex;
            candidates.push_back({command.order, std::move(entry)});
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.order < right.order;
    });
    std::vector<TrayPluginCommand> result;
    result.reserve((std::min)(candidates.size(), kTrayPluginCommandLimit));
    for (std::size_t index = 0; index < candidates.size() && index < kTrayPluginCommandLimit; ++index) {
        result.push_back(std::move(candidates[index].command));
    }
    return result;
}

void App::ToggleQuickPanel() {
    const bool wasVisible = quickPanel_.IsVisible();
    quickPanel_.Toggle(BuildQuickPanelState(),
                       FormatHotkey(settings_.quickPanelHotkey.modifiers, settings_.quickPanelHotkey.vk));
    const bool isVisible = quickPanel_.IsVisible();
    if (isVisible != wasVisible) DispatchPluginEvent(isVisible ? L"quick_panel_opened" : L"quick_panel_closed");
}

void App::HandleQuickPanelAction(QuickPanelAction action, std::size_t dataIndex) {
    const bool closedByAction = !quickPanel_.IsVisible();
    std::wstring clickedPluginId;
    std::wstring clickedCommandId;
    if (action == QuickPanelAction::PluginCommand && dataIndex < quickPanelPluginCommands_.size()) {
        clickedPluginId = quickPanelPluginCommands_[dataIndex].pluginId;
        clickedCommandId = quickPanelPluginCommands_[dataIndex].commandId;
    }
    // QuickPanel hides auto-closing tiles before invoking this callback. Publish
    // that real transition before an action is allowed to reopen the panel.
    if (closedByAction) DispatchPluginEvent(L"quick_panel_closed");
    bool pluginCommandExecuted = false;
    switch (action) {
        case QuickPanelAction::CaptureActive: TriggerCapture(CaptureMode::ActiveDisplay); break;
        case QuickPanelAction::ToggleRecording: ToggleRecording(); break;
        case QuickPanelAction::ToggleReplay: ToggleReplayBuffer(); break;
        case QuickPanelAction::SaveAllReplays: SaveAllReplays(); break;
        case QuickPanelAction::SaveMonitor0:
        case QuickPanelAction::SaveMonitor1:
        case QuickPanelAction::SaveMonitor2: {
            const int slot = static_cast<int>(action) - static_cast<int>(QuickPanelAction::SaveMonitor0);
            if (slot >= 0 && slot < static_cast<int>(quickPanelMonitorIndices_.size()) &&
                quickPanelMonitorIndices_[static_cast<std::size_t>(slot)] >= 0) {
                SaveReplayForMonitor(quickPanelMonitorIndices_[static_cast<std::size_t>(slot)]);
            }
            break;
        }
        case QuickPanelAction::OpenPhotos: ShowMainWindow(Page::Photos); break;
        case QuickPanelAction::OpenVideos: ShowMainWindow(Page::Videos); break;
        case QuickPanelAction::PluginCommand:
            if (!clickedPluginId.empty() && !clickedCommandId.empty()) {
                ExecutePluginExecution(pluginSystem_.ExecuteCommand(clickedPluginId, clickedCommandId));
                pluginCommandExecuted = true;
            }
            break;
        default: break;
    }
    if (pluginCommandExecuted) DispatchPluginEvent(L"quick_panel_command");
    quickPanel_.SetState(BuildQuickPanelState());
}

void App::ExitApplication() {
    if (exiting_) return;
    exiting_ = true;
    StopHotkeyRecorder();
    StopGlobalKeyboardHook();
    UnregisterAllHotkeys();
    ReleaseReplayPreviewFrames();
    StopAuxiliaryReplayEngines();
    if (recordingEngine_) { recordingEngine_->Stop(); recordingEngine_.reset(); }
    if (engine_) { engine_->Stop(); engine_.reset(); }
    soundPlayer_.Shutdown();
    thumbnailCache_.Stop();
    mediaViewer_.DestroyHost();
    quickPanel_.Destroy();
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
        if (binding.vk == 0) return true;
        return RegisterHotKey(hwnd_, id, binding.modifiers | MOD_NOREPEAT, binding.vk) != FALSE;
    };
    // Enabled recording shortcuts are registered first and independently so a
    // PrintScreen conflict cannot affect their fullscreen fallback route.
    const bool record = reg(LUNASHOT_HOTKEY_RECORD, settings_.recordingHotkey);
    const bool replaySave = reg(LUNASHOT_HOTKEY_REPLAY_SAVE, settings_.replaySaveHotkey);
    const bool quickPanel = reg(LUNASHOT_HOTKEY_QUICK_PANEL, settings_.quickPanelHotkey);
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
    const bool allRegistered = active && all && region && window && record && replaySave && quickPanel && monitorHotkeysRegistered;
    const bool fullscreenFallback = rawInputReady_ || globalKeyboardHook_ != nullptr;
    if (!allRegistered) {
        hotkeysRegistered_ = fullscreenFallback || active || all || region || window || record || replaySave || quickPanel;
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
    UnregisterHotKey(hwnd_, LUNASHOT_HOTKEY_QUICK_PANEL);
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
    if (settings_.quickPanelHotkey.vk == vk &&
        (settings_.quickPanelHotkey.modifiers & kHotkeyModifierMask) == normalizedModifiers) {
        ExecuteGlobalHotkey(kQuickPanelHotkeySlot, source);
        return true;
    }
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
            if (slot == kQuickPanelHotkeySlot) {
                ToggleQuickPanel();
            } else if (slot >= 6 && slot < 6 + static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) {
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
        ? L"Press a shortcut to assign it, or press Esc to disable this binding completely."
        : L"Press a shortcut, or Esc to disable it. Focused-window input is being used.";
    InvalidateUi();
}

void App::CancelHotkeyRecording() {
    StopHotkeyRecorder();
    recordingHotkeySlot_ = -1;
    RegisterAllHotkeys();
    engineStatus_ = engineReady_ ? L"Ready" : L"Starting";
    engineDetail_ = engineReady_ ? L"Fullscreen-safe capture, recording and Quick Panel shortcuts are active" : L"Preparing DXGI";
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
    else if (recordingHotkeySlot_ == kQuickPanelHotkeySlot) target = &settings_.quickPanelHotkey;
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
    std::vector<HotkeyBinding> bindings;
    const HotkeyBinding primaryBindings[]{settings_.activeDisplayHotkey, settings_.allDisplaysHotkey,
                                          settings_.regionHotkey, settings_.activeWindowHotkey,
                                          settings_.recordingHotkey, settings_.replaySaveHotkey,
                                          settings_.quickPanelHotkey};
    for (const auto& item : primaryBindings) if (item.vk != 0) bindings.push_back(item);
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

void App::DisableHotkeySlot(int slot) {
    HotkeyBinding* target = nullptr;
    if (slot == 0) target = &settings_.activeDisplayHotkey;
    else if (slot == 1) target = &settings_.allDisplaysHotkey;
    else if (slot == 2) target = &settings_.regionHotkey;
    else if (slot == 3) target = &settings_.activeWindowHotkey;
    else if (slot == 4) target = &settings_.recordingHotkey;
    else if (slot == 5) target = &settings_.replaySaveHotkey;
    else if (slot == kQuickPanelHotkeySlot) target = &settings_.quickPanelHotkey;
    else if (slot >= 6 && slot < 6 + static_cast<int>(settings_.replayMonitorHotkeys.size())) {
        target = &settings_.replayMonitorHotkeys[static_cast<std::size_t>(slot - 6)];
    }
    StopHotkeyRecorder();
    recordingHotkeySlot_ = -1;
    if (!target) { RegisterAllHotkeys(); return; }
    target->vk = 0;
    target->modifiers = MOD_NOREPEAT;
    SaveSettings();
    RegisterAllHotkeys();
    engineStatus_ = L"Shortcut disabled";
    engineDetail_ = L"This action remains available from Luna Capture and the tray menu";
    InvalidateUi();
}

void App::ToggleHotkeySlot(int slot) {
    const HotkeyBinding* current = nullptr;
    if (slot == 0) current = &settings_.activeDisplayHotkey;
    else if (slot == 1) current = &settings_.allDisplaysHotkey;
    else if (slot == 2) current = &settings_.regionHotkey;
    else if (slot == 3) current = &settings_.activeWindowHotkey;
    else if (slot == 4) current = &settings_.recordingHotkey;
    else if (slot == 5) current = &settings_.replaySaveHotkey;
    else if (slot == kQuickPanelHotkeySlot) current = &settings_.quickPanelHotkey;
    if (!current) return;
    if (current->vk != 0) DisableHotkeySlot(slot);
    else BeginHotkeyRecording(slot);
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
        settings_.microphoneInputs.push_back({selected.id, selected.name, 100, 0, 50, true});
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
            const std::vector<std::wstring> microphoneActions{
                L"Change microphone device",
                L"Noise suppression · " + std::to_wstring(input.noiseSuppressionPercent) + L"%",
                L"Noise gate · -" + std::to_wstring(input.noiseGateThresholdDb) + L" dB",
                L"Reset microphone filters",
            };
            const int microphoneAction = ChooseNamedItem(hwnd_, microphoneActions, 0, input.name,
                L"Device identity and lightweight native filters are saved with your settings");
            if (microphoneAction < 0) return;
            if (microphoneAction == 1) {
                int value = input.noiseSuppressionPercent;
                const std::vector<IntegerPreset> presets{{0, L"Off · untouched microphone"}, {25, L"Light · keyboard ambience"},
                    {50, L"Balanced · room noise"}, {70, L"Strong · fan and background"}, {90, L"Maximum · aggressive gate"}};
                if (ChooseIntegerPreset(hwnd_, L"Noise suppression", L"High-pass cleanup plus a smooth downward expander",
                                        presets, value, 0, 100, value)) input.noiseSuppressionPercent = value;
                return;
            }
            if (microphoneAction == 2) {
                int value = input.noiseGateThresholdDb;
                const std::vector<IntegerPreset> presets{{35, L"-35 dB · loud room"}, {45, L"-45 dB · strong"},
                    {50, L"-50 dB · balanced"}, {60, L"-60 dB · sensitive"}, {70, L"-70 dB · very sensitive"}};
                if (ChooseIntegerPreset(hwnd_, L"Noise gate threshold", L"Lower dB magnitude opens the microphone less easily",
                                        presets, value, 20, 80, value)) input.noiseGateThresholdDb = value;
                return;
            }
            if (microphoneAction == 3) {
                input.noiseSuppressionPercent = 0;
                input.noiseGateThresholdDb = 50;
                return;
            }
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

void App::ApplyPluginTheme() {
    themeAssetStatus_.clear();
    ui::ResetTranslations();
    if (const LunaLocaleDefinition* locale = pluginSystem_.ActiveLocale()) {
        std::vector<std::pair<std::wstring, std::wstring>> translations;
        translations.reserve(locale->translations.size());
        for (const auto& item : locale->translations) translations.emplace_back(item.source, item.replacement);
        ui::SetTranslations(translations);
    }

    const LunaThemeDefinition* theme = pluginSystem_.ActiveTheme();
    if (!theme) {
        ui::ResetThemePalette();
        RefreshThemeFonts();
        quickPanel_.RefreshTheme();
        trayMenu_.RefreshTheme();
        overlay_.RefreshTheme();
        regionEditor_.RefreshTheme();
        InvalidateUi();
        if (quickPanel_.IsVisible()) quickPanel_.SetState(BuildQuickPanelState());
        return;
    }
    ui::ThemePalette palette;
    palette.active = true;
    switch (theme->appearance) {
        case LunaThemeAppearance::Light: palette.appearance = ui::ThemeAppearance::Light; break;
        case LunaThemeAppearance::Amoled: palette.appearance = ui::ThemeAppearance::Amoled; break;
        default: palette.appearance = ui::ThemeAppearance::Dark; break;
    }
    palette.background = theme->background;
    palette.sidebar = theme->sidebar;
    palette.topbar = theme->topbar;
    palette.surface = theme->surface;
    palette.surfaceRaised = theme->surfaceRaised;
    palette.surfaceHover = theme->surfaceHover;
    palette.surfacePressed = theme->surfacePressed;
    palette.surfaceSelected = theme->surfaceSelected;
    palette.input = theme->input;
    palette.border = theme->border;
    palette.borderHover = theme->borderHover;
    palette.focus = theme->focus;
    palette.text = theme->text;
    palette.textSecondary = theme->textSecondary;
    palette.muted = theme->muted;
    palette.disabled = theme->disabled;
    palette.accent = theme->accent;
    palette.accentHover = theme->accentHover;
    palette.accentPressed = theme->accentPressed;
    palette.accentSecondary = theme->accentSecondary;
    palette.textOnAccent = theme->textOnAccent;
    palette.info = theme->info;
    palette.success = theme->success;
    palette.warning = theme->warning;
    palette.danger = theme->danger;
    palette.shadow = theme->shadow;
    palette.cornerScalePercent = theme->cornerScalePercent;
    palette.borderScalePercent = theme->borderScalePercent;
    palette.fontScalePercent = theme->fontScalePercent;
    palette.fontWeight = theme->fontWeight;
    palette.fontItalic = theme->fontItalic;
    palette.fontFamily = theme->fontFamily;
    palette.saturationPercent = theme->saturationPercent;
    palette.contrastPercent = theme->contrastPercent;
    palette.brightnessPercent = theme->brightnessPercent;
    palette.motionScalePercent = theme->motionScalePercent;
    palette.backgroundSecondary = theme->backgroundSecondary;
    palette.backdropColor = theme->backdropColor;
    palette.backdropColorSecondary = theme->backdropColorSecondary;
    palette.backdropOpacityPercent = theme->backdropOpacityPercent;
    palette.backdropScale = theme->backdropScale;
    palette.backdropThickness = theme->backdropThickness;
    palette.backdropSpeedPercent = theme->backdropSpeedPercent;
    palette.backdropSeed = theme->backdropSeed;
    palette.gradientAngle = theme->gradientAngle;
    palette.gradientCenterXPercent = theme->gradientCenterXPercent;
    palette.gradientCenterYPercent = theme->gradientCenterYPercent;
    palette.gradientSpreadPercent = theme->gradientSpreadPercent;
    palette.vignetteColor = theme->vignetteColor;
    palette.vignetteOpacityPercent = theme->vignetteOpacityPercent;
    palette.noiseOpacityPercent = theme->noiseOpacityPercent;
    palette.backgroundImagePositionXPercent = theme->backgroundImagePositionXPercent;
    palette.backgroundImagePositionYPercent = theme->backgroundImagePositionYPercent;
    palette.backgroundImageScalePercent = theme->backgroundImageScalePercent;
    palette.backgroundImageTint = theme->backgroundImageTint;
    palette.backgroundImageTintOpacityPercent = theme->backgroundImageTintOpacityPercent;
    palette.backgroundImageBrightnessPercent = theme->backgroundImageBrightnessPercent;
    palette.backgroundImageSaturationPercent = theme->backgroundImageSaturationPercent;
    palette.gradientStyle = theme->gradientStyle == LunaGradientStyle::Radial ?
        ui::GradientStyle::Radial : ui::GradientStyle::Linear;
    switch (theme->backdropScope) {
        case LunaBackdropScope::Main: palette.backdropScope = ui::BackdropScope::Main; break;
        case LunaBackdropScope::Panels: palette.backdropScope = ui::BackdropScope::Panels; break;
        default: palette.backdropScope = ui::BackdropScope::All; break;
    }
    switch (theme->backdropPattern) {
        case LunaBackdropPattern::Gradient: palette.backdropPattern = ui::BackdropPattern::Gradient; break;
        case LunaBackdropPattern::Grid: palette.backdropPattern = ui::BackdropPattern::Grid; break;
        case LunaBackdropPattern::Dots: palette.backdropPattern = ui::BackdropPattern::Dots; break;
        case LunaBackdropPattern::Aurora: palette.backdropPattern = ui::BackdropPattern::Aurora; break;
        case LunaBackdropPattern::Diagonal: palette.backdropPattern = ui::BackdropPattern::Diagonal; break;
        case LunaBackdropPattern::Waves: palette.backdropPattern = ui::BackdropPattern::Waves; break;
        case LunaBackdropPattern::Rings: palette.backdropPattern = ui::BackdropPattern::Rings; break;
        case LunaBackdropPattern::Checker: palette.backdropPattern = ui::BackdropPattern::Checker; break;
        case LunaBackdropPattern::Hexagons: palette.backdropPattern = ui::BackdropPattern::Hexagons; break;
        case LunaBackdropPattern::Stars: palette.backdropPattern = ui::BackdropPattern::Stars; break;
        case LunaBackdropPattern::Circuit: palette.backdropPattern = ui::BackdropPattern::Circuit; break;
        case LunaBackdropPattern::Scanlines: palette.backdropPattern = ui::BackdropPattern::Scanlines; break;
        case LunaBackdropPattern::Noise: palette.backdropPattern = ui::BackdropPattern::Noise; break;
        case LunaBackdropPattern::Mesh: palette.backdropPattern = ui::BackdropPattern::Mesh; break;
        default: palette.backdropPattern = ui::BackdropPattern::Solid; break;
    }
    ui::SetThemePalette(palette);
    ui::ResetThemeBackgroundImage();
    const LoadedPlugin* themePlugin = pluginSystem_.ActiveThemePlugin();
    if (themePlugin && !theme->backgroundImage.empty()) {
        const std::filesystem::path relative(theme->backgroundImage);
        bool safe = !relative.empty() && !relative.is_absolute() && !relative.has_root_path();
        for (const auto& component : relative) if (component == L"..") safe = false;
        const std::wstring extension = relative.extension().wstring();
        safe &= (_wcsicmp(extension.c_str(), L".png") == 0 || _wcsicmp(extension.c_str(), L".jpg") == 0 ||
                 _wcsicmp(extension.c_str(), L".jpeg") == 0 || _wcsicmp(extension.c_str(), L".bmp") == 0);
        std::vector<std::uint8_t> encodedImage;
        if (safe) safe = ReadThemeAssetSnapshot(themePlugin->sourcePath.parent_path(), relative, encodedImage);
        if (safe) {
            ui::BackgroundImageMode mode = ui::BackgroundImageMode::Cover;
            if (theme->backgroundImageMode == LunaBackgroundImageMode::Contain) mode = ui::BackgroundImageMode::Contain;
            else if (theme->backgroundImageMode == LunaBackgroundImageMode::Tile) mode = ui::BackgroundImageMode::Tile;
            else if (theme->backgroundImageMode == LunaBackgroundImageMode::Stretch) mode = ui::BackgroundImageMode::Stretch;
            else if (theme->backgroundImageMode == LunaBackgroundImageMode::Center) mode = ui::BackgroundImageMode::Center;
            ui::ThemeBackgroundImageOptions options;
            options.opacityPercent = theme->backgroundImageOpacityPercent;
            options.mode = mode;
            options.positionXPercent = theme->backgroundImagePositionXPercent;
            options.positionYPercent = theme->backgroundImagePositionYPercent;
            options.scalePercent = theme->backgroundImageScalePercent;
            options.tint = theme->backgroundImageTint;
            options.tintOpacityPercent = theme->backgroundImageTintOpacityPercent;
            options.brightnessPercent = theme->backgroundImageBrightnessPercent;
            options.saturationPercent = theme->backgroundImageSaturationPercent;
            if (!ui::SetThemeBackgroundImage(encodedImage, options)) {
                themeAssetStatus_ = L"background image could not be decoded safely";
            }
        } else {
            themeAssetStatus_ = L"background image must be a local PNG/JPEG/BMP under the plugin folder (max 16 MiB)";
        }
    }
    RefreshThemeFonts();
    quickPanel_.RefreshTheme();
    trayMenu_.RefreshTheme();
    overlay_.RefreshTheme();
    regionEditor_.RefreshTheme();
    InvalidateUi();
    if (quickPanel_.IsVisible()) quickPanel_.SetState(BuildQuickPanelState());
}

void App::ReloadPlugins(bool automatic) {
    if (pluginModalDepth_ > 0) {
        pluginStatus_ = L"Plugin reload postponed until the open review dialog closes";
        return;
    }
    pluginSystem_.Reload();
    ApplyPluginTheme();
    pluginStatus_ = (automatic ? L"Hot reload · " : L"Reloaded · ") + pluginSystem_.LastReloadSummary();
    if (!themeAssetStatus_.empty()) pluginStatus_ += L" · Theme: " + themeAssetStatus_;
    DispatchPluginEvent(L"plugins_reloaded");
    if (!automatic && settings_.notifications) {
        overlay_.ShowMessage(L"LunaScript reloaded", pluginSystem_.LastReloadSummary(),
                             settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
    }
    InvalidateUi();
}

void App::TogglePlugin(std::size_t index) {
    const LoadedPlugin* before = pluginSystem_.PluginAt(index);
    if (!before || !before->valid) {
        pluginStatus_ = before && !before->diagnostics.empty() ? before->diagnostics.front().message : L"Plugin is invalid";
        InvalidateUi();
        return;
    }
    const bool enable = !before->enabled;
    const std::wstring name = before->DisplayName();
    const std::wstring pluginId = before->definition.id;
    const std::filesystem::path sourcePath = before->sourcePath;
    const std::wstring permissionSummary = before->PermissionSummary();
    if (enable) {
        const std::vector<std::wstring> approval{L"Grant permissions and enable this plugin"};
        int accepted = -1;
        {
            ScopedCounter modalGuard(pluginModalDepth_);
            ui::ScopedTranslationBypass translationGuard;
            accepted = ChooseNamedItem(hwnd_, approval, 0, L"Enable " + name,
                L"Declared permissions: " + permissionSummary + L" · source remains sandboxed");
        }
        if (accepted < 0) return;
    }
    const std::size_t resolvedIndex = FindPluginIndexByIdentity(pluginSystem_, pluginId, sourcePath);
    const LoadedPlugin* current = pluginSystem_.PluginAt(resolvedIndex);
    if (!current || !current->valid || (enable && current->PermissionSummary() != permissionSummary)) {
        pluginStatus_ = L"Plugin changed while permission review was open · review it again";
        InvalidateUi();
        return;
    }
    if (!pluginSystem_.SetEnabled(resolvedIndex, enable)) return;
    if (enable) {
        const auto feedback = std::find_if(pluginFeedbackGuards_.begin(), pluginFeedbackGuards_.end(),
            [&pluginId](const PluginFeedbackGuardState& item) {
                return CompareStringOrdinal(item.pluginId.c_str(), static_cast<int>(item.pluginId.size()),
                                            pluginId.c_str(), static_cast<int>(pluginId.size()), TRUE) == CSTR_EQUAL;
            });
        if (feedback != pluginFeedbackGuards_.end()) feedback->breaker.Reset();
    }
    ApplyPluginTheme();
    pluginStatus_ = name + (enable ? L" enabled" : L" disabled");
    if (enable) DispatchPluginEvent(L"plugin_enabled");
    InvalidateUi();
}

void App::ManagePlugin(std::size_t index) {
    enum class ManageOperation {
        TogglePlugin,
        ActivateTheme,
        ResetTheme,
        RunCommand,
        OpenFolder,
        OpenDocumentation,
        Reload,
        ActivateLocale,
        ResetLocale,
    };

    const LoadedPlugin* plugin = pluginSystem_.PluginAt(index);
    if (!plugin) return;
    const std::wstring pluginId = plugin->definition.id;
    const std::filesystem::path sourcePath = plugin->sourcePath;
    std::vector<std::wstring> choices;
    std::vector<ManageOperation> operations;
    if (plugin->valid) {
        choices.push_back(plugin->enabled ? L"Disable plugin · stop all commands and events" : L"Enable plugin · grant declared permissions");
        operations.push_back(ManageOperation::TogglePlugin);
        if (plugin->definition.theme.present) {
            const bool active = _wcsicmp(pluginSystem_.ActiveThemeId().c_str(), plugin->definition.id.c_str()) == 0;
            choices.push_back(active ? L"Reset to Luna default theme" :
                (plugin->enabled ? L"Activate theme · " + plugin->definition.theme.name : L"Activate theme · enable the plugin first"));
            operations.push_back(active ? ManageOperation::ResetTheme : ManageOperation::ActivateTheme);
        }
        if (plugin->definition.locale.present) {
            const bool active = _wcsicmp(pluginSystem_.ActiveLocaleId().c_str(), plugin->definition.id.c_str()) == 0;
            choices.push_back(active ? L"Reset interface language to built-in English" :
                (plugin->enabled ? L"Activate language pack · " + plugin->definition.locale.name :
                                   L"Activate language pack · enable the plugin first"));
            operations.push_back(active ? ManageOperation::ResetLocale : ManageOperation::ActivateLocale);
        }
        if (!plugin->definition.commands.empty()) {
            choices.push_back(L"Run command… · " + std::to_wstring(plugin->definition.commands.size()) + L" available");
            operations.push_back(ManageOperation::RunCommand);
        }
    } else {
        choices.push_back(L"Reload after fixing the LunaScript source");
        operations.push_back(ManageOperation::Reload);
    }
    choices.push_back(L"Open plugin folder"); operations.push_back(ManageOperation::OpenFolder);
    choices.push_back(L"Open LunaScript SDK documentation"); operations.push_back(ManageOperation::OpenDocumentation);
    const std::wstring detail = plugin->valid
        ? plugin->definition.version + L" · " + plugin->definition.author + L" · permissions: " + plugin->PermissionSummary()
        : plugin->StatusText();
    int selected = -1;
    {
        ScopedCounter modalGuard(pluginModalDepth_);
        ui::ScopedTranslationBypass translationGuard;
        selected = ChooseNamedItem(hwnd_, choices, 0, plugin->DisplayName(), detail);
    }
    if (selected < 0 || static_cast<std::size_t>(selected) >= operations.size()) return;
    index = FindPluginIndexByIdentity(pluginSystem_, pluginId, sourcePath);
    plugin = pluginSystem_.PluginAt(index);
    if (!plugin) {
        pluginStatus_ = L"Plugin changed while its management dialog was open";
        InvalidateUi();
        return;
    }
    switch (operations[static_cast<std::size_t>(selected)]) {
        case ManageOperation::TogglePlugin:
            TogglePlugin(index);
            break;
        case ManageOperation::ActivateTheme:
            if (_wcsicmp(pluginSystem_.ActiveThemeId().c_str(), plugin->definition.id.c_str()) == 0) {
                pluginStatus_ = L"Theme state changed while the dialog was open · choose the action again";
            } else if (pluginSystem_.ActivateTheme(index)) {
                pluginStatus_ = plugin->definition.theme.name + L" theme activated";
                ApplyPluginTheme();
                if (!themeAssetStatus_.empty()) pluginStatus_ += L" · " + themeAssetStatus_;
            } else {
                pluginStatus_ = L"Enable this plugin before activating its theme";
            }
            break;
        case ManageOperation::ResetTheme:
            if (_wcsicmp(pluginSystem_.ActiveThemeId().c_str(), plugin->definition.id.c_str()) != 0) {
                pluginStatus_ = L"Theme state changed while the dialog was open · choose the action again";
                break;
            }
            pluginSystem_.ResetTheme();
            pluginStatus_ = L"Default Luna theme restored";
            ApplyPluginTheme();
            break;
        case ManageOperation::RunCommand: {
            if (!plugin || !plugin->enabled) { pluginStatus_ = L"Enable the plugin before running commands"; break; }
            std::vector<std::wstring> commands;
            std::vector<std::wstring> commandIds;
            for (const auto& command : plugin->definition.commands) {
                commands.push_back(command.title + (command.description.empty() ? L"" : L" · " + command.description));
                commandIds.push_back(command.id);
            }
            int command = -1;
            {
                ScopedCounter modalGuard(pluginModalDepth_);
                ui::ScopedTranslationBypass translationGuard;
                command = ChooseNamedItem(hwnd_, commands, 0, plugin->DisplayName() + L" commands",
                                          L"Commands execute only the permissions declared in the source");
            }
            if (command >= 0 && static_cast<std::size_t>(command) < commandIds.size()) {
                ExecutePluginExecution(pluginSystem_.ExecuteCommand(pluginId, commandIds[static_cast<std::size_t>(command)]));
            }
            break;
        }
        case ManageOperation::ActivateLocale:
            if (_wcsicmp(pluginSystem_.ActiveLocaleId().c_str(), plugin->definition.id.c_str()) == 0) {
                pluginStatus_ = L"Language-pack state changed while the dialog was open · choose the action again";
            } else if (pluginSystem_.ActivateLocale(index)) {
                pluginStatus_ = plugin->definition.locale.name + L" language pack activated";
                ApplyPluginTheme();
                if (!themeAssetStatus_.empty()) pluginStatus_ += L" · " + themeAssetStatus_;
            } else {
                pluginStatus_ = L"Enable this plugin before activating its language pack";
            }
            break;
        case ManageOperation::ResetLocale:
            if (_wcsicmp(pluginSystem_.ActiveLocaleId().c_str(), plugin->definition.id.c_str()) != 0) {
                pluginStatus_ = L"Language-pack state changed while the dialog was open · choose the action again";
                break;
            }
            pluginSystem_.ResetLocale();
            pluginStatus_ = L"Built-in English interface restored";
            ApplyPluginTheme();
            if (!themeAssetStatus_.empty()) pluginStatus_ += L" · " + themeAssetStatus_;
            break;
        case ManageOperation::OpenFolder:
            OpenFolderInExplorer(pluginSystem_.PluginDirectory(index).wstring());
            break;
        case ManageOperation::OpenDocumentation:
            OpenPluginDocumentation();
            break;
        case ManageOperation::Reload:
            ReloadPlugins();
            break;
    }
    InvalidateUi();
}

void App::ImportLunaFile(const std::wstring& sourcePath) {
    std::error_code ec;
    const std::filesystem::path source(sourcePath);
    if (!std::filesystem::is_regular_file(source, ec)) {
        ShowNoticeDialog(hwnd_, L"That plugin could not be opened",
                         L"The file no longer exists, or it is not readable.", true);
        return;
    }
    std::wstring extension = source.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    if (extension != L".luna") {
        ShowNoticeDialog(hwnd_, L"Not a LunaScript plugin",
                         L"Only .luna files can be installed as plugins.", true);
        return;
    }

    const std::filesystem::path root = pluginSystem_.RootDirectory();
    std::filesystem::create_directories(root, ec);
    const std::filesystem::path destination = root / source.filename();

    // Importing a file that is already installed is a normal double-click, not
    // an error — but overwriting someone's edited copy silently would not be.
    if (std::filesystem::exists(destination, ec) &&
        !std::filesystem::equivalent(source, destination, ec)) {
        if (!ConfirmAction(hwnd_, L"Replace this plugin?",
                           source.filename().wstring() +
                               L"\n\nA plugin with this file name is already installed. Replacing it discards "
                               L"the installed copy, including any edits you made to it.",
                           L"Replace", true)) {
            return;
        }
    }

    if (!std::filesystem::equivalent(source, destination, ec)) {
        ec.clear();
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            ShowNoticeDialog(hwnd_, L"The plugin could not be installed",
                             L"Copying it into the plugin folder failed. Check that the folder is writable.", true);
            return;
        }
    }
    ec.clear();

    ReloadPlugins(false);
    ShowMainWindow(Page::Plugins);

    // Find what we just imported so the confirmation can name it and point at
    // the permission review the user still has to do.
    std::wstring detail = source.filename().wstring();
    for (const auto& plugin : pluginSystem_.Plugins()) {
        if (plugin.sourcePath.filename() == destination.filename()) {
            detail = plugin.DisplayName() + L"\n" + plugin.StatusText();
            break;
        }
    }
    ShowNoticeDialog(hwnd_, L"Plugin installed",
                     detail + L"\n\nEvery new plugin starts disabled. Review its permissions on the Plugins "
                              L"page, then enable it.",
                     false);
    InvalidateUi();
}

void App::BrowsePlugins() {
    const auto& plugins = pluginSystem_.Plugins();
    if (plugins.empty()) {
        pluginStatus_ = L"No .luna files found · open the plugin folder or read the SDK";
        InvalidateUi();
        return;
    }
    std::vector<std::wstring> labels;
    std::vector<std::wstring> pluginIds;
    std::vector<std::filesystem::path> sourcePaths;
    labels.reserve(plugins.size());
    pluginIds.reserve(plugins.size());
    sourcePaths.reserve(plugins.size());
    for (const auto& plugin : plugins) {
        labels.push_back((plugin.valid ? plugin.enabled ? L"ON  · " : L"OFF · " : L"ERR · ") +
                         plugin.DisplayName() + L"  ·  " + plugin.StatusText());
        pluginIds.push_back(plugin.definition.id);
        sourcePaths.push_back(plugin.sourcePath);
    }
    int selected = -1;
    {
        ScopedCounter modalGuard(pluginModalDepth_);
        ui::ScopedTranslationBypass translationGuard;
        selected = ChooseNamedItem(hwnd_, labels, 0, L"Installed LunaScript plugins",
                                   L"Every new plugin starts disabled; inspect permissions before enabling it");
    }
    if (selected >= 0 && static_cast<std::size_t>(selected) < pluginIds.size()) {
        const std::size_t resolved = FindPluginIndexByIdentity(
            pluginSystem_, pluginIds[static_cast<std::size_t>(selected)], sourcePaths[static_cast<std::size_t>(selected)]);
        if (pluginSystem_.PluginAt(resolved)) ManagePlugin(resolved);
    }
}

bool App::ApplyPluginSettingInstruction(const LunaInstruction& instruction) {
    const bool encoderSetting = instruction.type == LunaInstructionType::SettingCursor ||
        instruction.type == LunaInstructionType::SettingReplaySeconds ||
        instruction.type == LunaInstructionType::SettingRecordingFps ||
        instruction.type == LunaInstructionType::SettingVideoBitrate ||
        instruction.type == LunaInstructionType::SettingRecordingHeight ||
        instruction.type == LunaInstructionType::SettingVideoCodec ||
        instruction.type == LunaInstructionType::SettingSystemAudio ||
        instruction.type == LunaInstructionType::SettingSystemAudioVolume ||
        instruction.type == LunaInstructionType::SettingApplicationAudio ||
        instruction.type == LunaInstructionType::SettingApplicationAudioVolume ||
        instruction.type == LunaInstructionType::SettingAudioBitrate;
    if (encoderSetting && !GuardSessionSettingChange(L"Plugin setting")) {
        pluginStatus_ = L"Plugin setting blocked while recording or Replay is live";
        return false;
    }
    switch (instruction.type) {
        case LunaInstructionType::SettingPreview: settings_.previewEnabled = instruction.value != 0; break;
        case LunaInstructionType::SettingPreviewFps:
            settings_.previewFps = (std::max)(1, (std::min)(60, instruction.value)); break;
        case LunaInstructionType::SettingCursor: settings_.recordCursor = instruction.value != 0; break;
        case LunaInstructionType::SettingNotifications: settings_.notifications = instruction.value != 0; break;
        case LunaInstructionType::SettingSound: settings_.sound = instruction.value != 0; break;
        case LunaInstructionType::SettingReplaySeconds:
            settings_.replaySeconds = (std::max)(5, (std::min)(3600, instruction.value)); break;
        case LunaInstructionType::SettingRecordingFps:
            settings_.recordingFps = (std::max)(1, (std::min)(240, instruction.value)); break;
        case LunaInstructionType::SettingVideoBitrate:
            settings_.recordingBitrateMbps = (std::max)(1, (std::min)(200, instruction.value)); break;
        case LunaInstructionType::SettingRecordingHeight:
            settings_.recordingHeight = (std::max)(0, (std::min)(4320, instruction.value)); break;
        case LunaInstructionType::SettingVideoCodec:
            settings_.videoCodec =
                static_cast<VideoCodec>((std::max)(0, (std::min)(kVideoCodecCount - 1, instruction.value))); break;
        case LunaInstructionType::SettingImageFormat:
            settings_.imageFormat = instruction.value == 1 ? ImageFormat::Jpeg : ImageFormat::Png; break;
        case LunaInstructionType::SettingJpegQuality:
            settings_.jpegQuality = (std::clamp)(instruction.value, 1, 100); break;
        case LunaInstructionType::SettingOutputWidth:
            settings_.outputWidth = (std::clamp)(instruction.value, 0, 7680); break;
        case LunaInstructionType::SettingOutputHeight:
            settings_.outputHeight = (std::clamp)(instruction.value, 0, 4320); break;
        case LunaInstructionType::SettingCopyRegion:
            settings_.copyRegionToClipboard = instruction.value != 0; break;
        case LunaInstructionType::SettingOverlayDuration:
            settings_.overlayDurationMs = (std::clamp)(instruction.value, 250, 10000); break;
        case LunaInstructionType::SettingReplayPreview:
            settings_.replayPreviewEnabled = instruction.value != 0; break;
        case LunaInstructionType::SettingSystemAudio:
            settings_.recordSystemAudio = instruction.value != 0;
            if (settings_.recordSystemAudio) settings_.recordApplicationAudio = false;
            break;
        case LunaInstructionType::SettingSystemAudioVolume:
            settings_.systemAudioVolumePercent = (std::clamp)(instruction.value, 0, 200); break;
        case LunaInstructionType::SettingApplicationAudio:
            settings_.recordApplicationAudio = instruction.value != 0;
            if (settings_.recordApplicationAudio) settings_.recordSystemAudio = false;
            break;
        case LunaInstructionType::SettingApplicationAudioVolume:
            settings_.applicationAudioVolumePercent = (std::clamp)(instruction.value, 0, 200); break;
        case LunaInstructionType::SettingAudioBitrate:
            settings_.audioBitrateKbps = NormalizeAacBitrateKbps(instruction.value); break;
        default: return false;
    }
    SaveSettings();
    return true;
}

bool App::ApplyPluginAudioInstruction(const LunaInstruction& instruction) {
    if (!GuardSessionSettingChange(L"Plugin microphone configuration")) {
        pluginStatus_ = L"Microphone changes are locked until recording or Replay stops";
        return false;
    }
    std::wstring target = instruction.first;
    if (_wcsicmp(target.c_str(), L"default") == 0) {
        if (microphones_.empty()) microphones_ = RecordingEngine::EnumerateMicrophones();
        const auto defaultDevice = std::find_if(microphones_.begin(), microphones_.end(),
                                                 [](const AudioDeviceInfo& item) { return item.isDefault; });
        if (defaultDevice != microphones_.end()) target = defaultDevice->id;
    }
    const auto matches = [&target](const AudioInputSetting& input) {
        return _wcsicmp(input.deviceId.c_str(), target.c_str()) == 0 ||
               _wcsicmp(input.name.c_str(), target.c_str()) == 0;
    };
    auto found = std::find_if(settings_.microphoneInputs.begin(), settings_.microphoneInputs.end(), matches);
    if (instruction.type == LunaInstructionType::AudioRemoveMicrophone) {
        if (found == settings_.microphoneInputs.end()) {
            pluginStatus_ = L"Plugin microphone target is not configured: " + instruction.first;
            return false;
        }
        settings_.microphoneInputs.erase(found);
        SaveSettings();
        RefreshAudioSources();
        return true;
    }
    if (found == settings_.microphoneInputs.end()) {
        if (settings_.microphoneInputs.size() >= 4) {
            pluginStatus_ = L"The four microphone mixer slots are already occupied";
            return false;
        }
        if (microphones_.empty()) microphones_ = RecordingEngine::EnumerateMicrophones();
        const auto device = std::find_if(microphones_.begin(), microphones_.end(), [&target](const AudioDeviceInfo& item) {
            return _wcsicmp(item.id.c_str(), target.c_str()) == 0 ||
                   _wcsicmp(item.name.c_str(), target.c_str()) == 0;
        });
        if (device == microphones_.end()) {
            pluginStatus_ = L"Microphone endpoint not found: " + instruction.first;
            return false;
        }
        AudioInputSetting input;
        input.deviceId = device->id;
        input.name = device->name;
        settings_.microphoneInputs.push_back(std::move(input));
        found = std::prev(settings_.microphoneInputs.end());
    }
    switch (instruction.type) {
        case LunaInstructionType::AudioAddMicrophone: found->enabled = true; break;
        case LunaInstructionType::AudioMicrophoneEnable: found->enabled = instruction.value != 0; break;
        case LunaInstructionType::AudioMicrophoneGain:
            found->volumePercent = (std::max)(0, (std::min)(200, instruction.value)); break;
        case LunaInstructionType::AudioNoiseSuppression:
            found->noiseSuppressionPercent = (std::max)(0, (std::min)(100, instruction.value)); break;
        case LunaInstructionType::AudioNoiseGate:
            found->noiseGateThresholdDb = (std::max)(20, (std::min)(80, instruction.value)); break;
        default: return false;
    }
    SaveSettings();
    RefreshAudioSources();
    return true;
}

bool App::AllowPluginMediaEventAction(const PluginExecution& execution) {
    if (!IsPluginMediaLifecycleEvent(currentPluginEventName_)) return true;
    auto guard = std::find_if(pluginFeedbackGuards_.begin(), pluginFeedbackGuards_.end(),
        [&execution](const PluginFeedbackGuardState& item) {
            return CompareStringOrdinal(item.pluginId.c_str(), static_cast<int>(item.pluginId.size()),
                                        execution.pluginId.c_str(), static_cast<int>(execution.pluginId.size()),
                                        TRUE) == CSTR_EQUAL;
        });
    if (guard == pluginFeedbackGuards_.end()) {
        pluginFeedbackGuards_.push_back({execution.pluginId, {}});
        guard = std::prev(pluginFeedbackGuards_.end());
    }
    const bool wasTripped = guard->breaker.IsTripped();
    if (guard->breaker.TryAcquire(static_cast<std::uint64_t>(GetTickCount64()))) return true;
    if (!wasTripped) {
        AppendDiagnosticLog(L"Plugin " + execution.pluginId +
            L" media feedback circuit opened after 8 event-driven actions in 30 seconds");
    }
    pluginStatus_ = execution.pluginName +
        L" media feedback blocked · fix it, then disable/re-enable the plugin (timers remain the sustained path)";
    return false;
}

void App::ExecutePluginExecution(const PluginExecution& execution) {
    if (execution.pluginId.empty()) return;
    PluginExecutionBudgetScope budgetScope(pluginExecutionScopeDepth_, pluginInstructionBudgetRemaining_);
    // The block stays atomic: never execute a prefix and silently drop the tail.
    if (execution.instructions.size() > pluginInstructionBudgetRemaining_) {
        AppendDiagnosticLog(L"Plugin root execution budget exhausted before atomic handler from " + execution.pluginId);
        pluginStatus_ = execution.pluginName + L" handler skipped · root execution budget exhausted";
        InvalidateUi();
        return;
    }
    pluginInstructionBudgetRemaining_ -= execution.instructions.size();
    const std::wstring statusBefore = pluginStatus_;
    AppendDiagnosticLog(L"Plugin " + execution.pluginId + L" executing " +
                        std::to_wstring(execution.instructions.size()) + L" sandboxed instruction(s)");
    bool succeeded = true;
    const auto allowMediaAction = [&]() {
        const bool allowed = AllowPluginMediaEventAction(execution);
        if (!allowed) succeeded = false;
        return allowed;
    };
    for (const auto& instruction : execution.instructions) {
        switch (instruction.type) {
            case LunaInstructionType::Notify:
                overlay_.ShowMessage(instruction.first, instruction.second, settings_.overlayDurationMs,
                                     GetPriorityMonitor(hwnd_));
                if (overlay_.IsFullscreenPresentation()) ShowSystemNotification(instruction.first, instruction.second);
                break;
            case LunaInstructionType::CaptureActive:
                if (allowMediaAction()) TriggerCapture(CaptureMode::ActiveDisplay); break;
            case LunaInstructionType::CaptureAll:
                if (allowMediaAction()) TriggerCapture(CaptureMode::AllDisplays); break;
            case LunaInstructionType::CaptureRegion:
                if (allowMediaAction()) TriggerCapture(CaptureMode::Region); break;
            case LunaInstructionType::CaptureWindow:
                if (allowMediaAction()) TriggerCapture(CaptureMode::ActiveWindow); break;
            case LunaInstructionType::CaptureMonitor:
                if (allowMediaAction()) TriggerCapture(CaptureMode::ActiveDisplay, instruction.value); break;
            case LunaInstructionType::RecordingToggle:
                if (allowMediaAction()) ToggleRecording(); break;
            case LunaInstructionType::RecordingStart:
                if ((!recordingEngine_ || !recordingEngine_->IsRecording()) && allowMediaAction()) ToggleRecording();
                break;
            case LunaInstructionType::RecordingStop:
                if (recordingEngine_ && recordingEngine_->IsRecording() && allowMediaAction()) ToggleRecording();
                break;
            case LunaInstructionType::ReplayToggle:
                if (allowMediaAction()) ToggleReplayBuffer(); break;
            case LunaInstructionType::ReplayStart:
                if (!IsReplayBuffering() && allowMediaAction()) ToggleReplayBuffer();
                break;
            case LunaInstructionType::ReplayStop:
                if (IsReplayBuffering() && allowMediaAction()) ToggleReplayBuffer();
                break;
            case LunaInstructionType::ReplaySaveAll:
                if (!IsReplayBuffering()) {
                    pluginStatus_ = L"Plugin requested Save All while replay is off";
                    succeeded = false;
                } else if (allowMediaAction()) {
                    SaveAllReplays();
                }
                break;
            case LunaInstructionType::ReplaySaveMonitor:
                if (!IsReplayBuffering()) {
                    pluginStatus_ = L"Plugin requested a monitor Replay save while replay is off";
                    succeeded = false;
                } else if (allowMediaAction()) {
                    SaveReplayForMonitor(instruction.value);
                }
                break;
            case LunaInstructionType::SettingPreview:
            case LunaInstructionType::SettingPreviewFps:
            case LunaInstructionType::SettingCursor:
            case LunaInstructionType::SettingNotifications:
            case LunaInstructionType::SettingSound:
            case LunaInstructionType::SettingReplaySeconds:
            case LunaInstructionType::SettingRecordingFps:
            case LunaInstructionType::SettingVideoBitrate:
            case LunaInstructionType::SettingRecordingHeight:
            case LunaInstructionType::SettingVideoCodec:
            case LunaInstructionType::SettingImageFormat:
            case LunaInstructionType::SettingJpegQuality:
            case LunaInstructionType::SettingOutputWidth:
            case LunaInstructionType::SettingOutputHeight:
            case LunaInstructionType::SettingCopyRegion:
            case LunaInstructionType::SettingOverlayDuration:
            case LunaInstructionType::SettingReplayPreview:
            case LunaInstructionType::SettingSystemAudio:
            case LunaInstructionType::SettingSystemAudioVolume:
            case LunaInstructionType::SettingApplicationAudio:
            case LunaInstructionType::SettingApplicationAudioVolume:
            case LunaInstructionType::SettingAudioBitrate:
                succeeded &= ApplyPluginSettingInstruction(instruction); break;
            case LunaInstructionType::AudioAddMicrophone:
            case LunaInstructionType::AudioRemoveMicrophone:
            case LunaInstructionType::AudioMicrophoneEnable:
            case LunaInstructionType::AudioMicrophoneGain:
            case LunaInstructionType::AudioNoiseSuppression:
            case LunaInstructionType::AudioNoiseGate:
                succeeded &= ApplyPluginAudioInstruction(instruction); break;
            case LunaInstructionType::OpenScreenshots: OpenFolderInExplorer(settings_.saveDirectory); break;
            case LunaInstructionType::OpenRecordings: OpenFolderInExplorer(settings_.recordingDirectory); break;
            case LunaInstructionType::OpenPlugins: OpenFolderInExplorer(pluginSystem_.RootDirectory().wstring()); break;
            case LunaInstructionType::OpenReplayCache:
                OpenFolderInExplorer((std::filesystem::path(GetLocalAppDataDirectory()) / L"ReplayCache").wstring()); break;
            case LunaInstructionType::OpenLogs: OpenFolderInExplorer(GetLocalAppDataDirectory()); break;
            case LunaInstructionType::UiShow: ShowMainWindow(page_); break;
            case LunaInstructionType::UiHide: HideMainWindow(); break;
            case LunaInstructionType::UiToggle:
                if (IsWindowVisible(hwnd_)) HideMainWindow(); else ShowMainWindow(page_);
                break;
            case LunaInstructionType::UiNavigate: {
                static const std::unordered_map<std::wstring, Page> pages{
                    {L"home", Page::Home}, {L"capture", Page::Capture}, {L"photos", Page::Photos},
                    {L"videos", Page::Videos}, {L"record", Page::Record}, {L"replay", Page::Replay},
                    {L"audio", Page::Audio}, {L"plugins", Page::Plugins},
                    {L"encoder", Page::Encoder}, {L"settings", Page::Settings}};
                const auto page = pages.find(instruction.first);
                if (page == pages.end()) succeeded = false;
                else ShowMainWindow(page->second);
                break;
            }
            case LunaInstructionType::QuickPanelShow:
                if (!quickPanel_.IsVisible()) ToggleQuickPanel();
                break;
            case LunaInstructionType::QuickPanelHide:
                if (quickPanel_.IsVisible()) ToggleQuickPanel();
                break;
            case LunaInstructionType::QuickPanelToggle: ToggleQuickPanel(); break;
            case LunaInstructionType::ThemeActivate: {
                bool activated = false;
                const auto& plugins = pluginSystem_.Plugins();
                for (std::size_t index = 0; index < plugins.size(); ++index) {
                    if (plugins[index].definition.id == execution.pluginId) {
                        activated = pluginSystem_.ActivateTheme(index);
                        break;
                    }
                }
                succeeded &= activated;
                if (activated) ApplyPluginTheme();
                break;
            }
            case LunaInstructionType::ThemeReset: pluginSystem_.ResetTheme(); ApplyPluginTheme(); break;
            case LunaInstructionType::LocaleActivate: {
                bool activated = false;
                const auto& plugins = pluginSystem_.Plugins();
                for (std::size_t index = 0; index < plugins.size(); ++index) {
                    if (plugins[index].definition.id == execution.pluginId) {
                        activated = pluginSystem_.ActivateLocale(index);
                        break;
                    }
                }
                succeeded &= activated;
                if (activated) ApplyPluginTheme();
                break;
            }
            case LunaInstructionType::LocaleReset: pluginSystem_.ResetLocale(); ApplyPluginTheme(); break;
            case LunaInstructionType::Log: AppendDiagnosticLog(L"Plugin " + execution.pluginId + L": " + instruction.first); break;
        }
    }
    if (succeeded && pluginStatus_ == statusBefore) pluginStatus_ = execution.pluginName + L" command dispatched";
    else if (pluginStatus_ == statusBefore) pluginStatus_ = execution.pluginName + L" command was blocked or unavailable";
    InvalidateUi();
}

void App::DispatchPluginEvent(const std::wstring& eventName) {
    constexpr int kMaximumPluginDispatchDepth = 8;
    if (pluginDispatchDepth_ >= kMaximumPluginDispatchDepth) {
        AppendDiagnosticLog(L"Plugin event recursion limit reached while dispatching " + eventName);
        return;
    }
    ScopedCounter dispatchGuard(pluginDispatchDepth_);
    PluginExecutionBudgetScope budgetScope(pluginExecutionScopeDepth_, pluginInstructionBudgetRemaining_);
    const std::wstring previousEventName = currentPluginEventName_;
    currentPluginEventName_ = eventName;
    for (const auto& execution : pluginSystem_.DispatchEvent(eventName)) ExecutePluginExecution(execution);
    currentPluginEventName_ = previousEventName;
}

void App::OpenPluginDocumentation() {
    const auto& path = pluginSystem_.DocumentationPath();
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        pluginStatus_ = L"SDK file is missing from docs\\plugin-sdk.html";
        InvalidateUi();
        return;
    }
    ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr, path.parent_path().c_str(), SW_SHOWNORMAL);
}


void App::ToggleRecording() {
    if (!recordingEngine_) {
        recordingStatus_ = L"Recorder unavailable";
        recordingDetail_ = L"The recording worker could not start";
        if (settings_.notifications) overlay_.ShowWarning(recordingStatus_, recordingDetail_, settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
        return;
    }
    if (recordingEngine_->IsRecording()) {
        recordingStatus_ = L"Finalizing recording…";
        recordingEngine_->StopActiveSession();
    } else if (IsReplayBuffering()) {
        // Hit whenever the record hotkey fires while the replay buffer is already running
        // (including a buffer the app just silently resumed on launch). Recording and replay
        // share one recorder, so this always failed quietly before — only a status string
        // nobody sees unless the main window happens to be open. Surface it so a hotkey press
        // during a game doesn't look like it was simply ignored.
        recordingStatus_ = L"Replay buffer is already active";
        recordingDetail_ = L"Stop replay before starting a normal recording";
        if (settings_.notifications) overlay_.ShowWarning(recordingStatus_, recordingDetail_, settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
    } else if (recordingEngine_->IsBusy()) {
        recordingStatus_ = L"Recorder is changing state";
        recordingDetail_ = L"Wait for the current start or finalize operation";
        if (settings_.notifications) overlay_.ShowWarning(recordingStatus_, recordingDetail_, settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
    } else {
        audioWarning_.clear();
        SuspendCaptureForRecording();
        AppSettings session = BuildSessionSettings();
        activeRecordingMonitorIndex_ = session.recordingMonitorIndex;
        std::uint64_t sessionGeneration = 0;
        if (!recordingEngine_->StartRecording(session, &sessionGeneration)) {
            recordingStatus_ = L"Recorder is busy";
            recordingDetail_ = L"Recording could not start; try again in a moment";
            activeRecordingMonitorIndex_ = -1;
            activeRecordingSessionGeneration_ = 0;
            ResumeCaptureAfterRecording();
            if (settings_.notifications) overlay_.ShowWarning(recordingStatus_, recordingDetail_, settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
        } else {
            activeRecordingSessionGeneration_ = sessionGeneration;
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
        if (settings_.resumeReplayOnLaunch) {
            settings_.resumeReplayOnLaunch = false;
            SaveSettings();
        }
    } else if (recordingEngine_->IsRecording()) {
        // Mirror image of the ToggleRecording() conflict: replay and a normal recording share
        // one recorder, so enabling replay while already recording used to fail with nothing
        // but an invisible status string. Now it gets the same overlay treatment.
        recordingStatus_ = L"A recording is already active";
        recordingDetail_ = L"Stop the recording before enabling replay";
        if (settings_.notifications) overlay_.ShowWarning(recordingStatus_, recordingDetail_, settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
    } else if (recordingEngine_->IsBusy()) {
        recordingStatus_ = L"Recorder is changing state";
        recordingDetail_ = L"Wait for the current start or finalize operation";
        if (settings_.notifications) overlay_.ShowWarning(recordingStatus_, recordingDetail_, settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
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
                    if (monitors.size() == LUNASHOT_MAX_ACTIVE_REPLAY_MONITORS) break;
                }
            }
        }
        if (monitors.empty()) monitors.push_back(primarySettings.recordingMonitorIndex);
        primaryReplayMonitorIndex_ = monitors.front();
        primarySettings.recordingAutoMonitor = false;
        primarySettings.recordingMonitorIndex = primaryReplayMonitorIndex_;
        if (!recordingEngine_->StartReplayBuffer(primarySettings)) {
            recordingStatus_ = L"Recorder is busy";
            recordingDetail_ = L"Replay buffer could not start; try again in a moment";
            primaryReplayMonitorIndex_ = -1;
            ResumeCaptureAfterRecording();
            if (settings_.notifications) overlay_.ShowWarning(recordingStatus_, recordingDetail_, settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
        } else {
            if (!settings_.resumeReplayOnLaunch) {
                settings_.resumeReplayOnLaunch = true;
                SaveSettings();
            }
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
    ReleaseReplayPreviewFrames();
    for (auto& channel : auxiliaryReplayEngines_) {
        if (channel.engine) channel.engine->Stop();
    }
    auxiliaryReplayEngines_.clear();
}

void App::SaveAllReplays() {
    bool savedAny = false;
    if (recordingEngine_ && recordingEngine_->IsReplayBuffering()) { recordingEngine_->SaveReplay(); savedAny = true; }
    for (auto& channel : auxiliaryReplayEngines_) {
        if (channel.engine && channel.engine->IsReplayBuffering()) { channel.engine->SaveReplay(); savedAny = true; }
    }
    // This is the global "save replay" hotkey path. With no live buffer it used to do
    // nothing at all — a hotkey press with zero feedback looks identical to a hotkey that
    // never registered, which is indistinguishable from LunaShot being broken.
    if (!savedAny) {
        recordingStatus_ = L"Replay buffer is off";
        recordingDetail_ = L"Start Replay Buffer before saving the last moments";
        if (settings_.notifications) overlay_.ShowWarning(recordingStatus_, recordingDetail_, settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
        InvalidateUi();
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
    if (settings_.notifications) overlay_.ShowWarning(recordingStatus_, recordingDetail_, settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
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
    std::size_t allCount = 0;
    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    for (const auto& output : recordingOutputs_) {
        if (output.linearIndex < 0 || output.linearIndex >= static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) continue;
        if (allCount < LUNASHOT_MAX_ACTIVE_REPLAY_MONITORS) {
            allMask |= 1u << output.linearIndex;
            ++allCount;
        }
        if (output.monitor == primary) primaryMask = 1u << output.linearIndex;
    }
    if (primaryMask == 0 && !recordingOutputs_.empty()) primaryMask = 1u << recordingOutputs_.front().linearIndex;
    int current = settings_.replayMonitorMask == 0 ? 0 : settings_.replayMonitorMask == primaryMask ? 1 :
                  settings_.replayMonitorMask == allMask ? 2 : 3;
    const std::vector<std::wstring> profiles{
        L"Auto foreground  ·  buffer the monitor containing the active game",
        L"Primary only  ·  minimum GPU/disk load",
        L"Up to 3 monitors  ·  each display gets an independent rolling file",
        L"Custom monitor group…  ·  choose any three displays",
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
                                     L"Up to three connected monitors will use independent rolling caches";
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
        std::size_t count = 0;
        for (const auto& output : recordingOutputs_) {
            if (output.linearIndex >= 0 && output.linearIndex < static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) {
                mask |= 1u << output.linearIndex;
                if (++count == LUNASHOT_MAX_ACTIVE_REPLAY_MONITORS) break;
            }
        }
    }
    settings_.replayMonitorMask = mask;
    SaveSettings();
    RegisterAllHotkeys();
    recordingStatus_ = L"Replay source profile updated";
    recordingDetail_ = profile == 0 ? L"AUTO will follow the foreground game when buffering starts" :
                       profile == 1 ? L"Only the Windows primary monitor is armed" :
                                      L"Up to three connected monitors are armed as independent channels";
}

void App::ToggleReplayMonitorSource(int monitorIndex) {
    if (monitorIndex < 0 || monitorIndex >= static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS) ||
        !GuardSessionSettingChange(L"Replay monitor sources")) return;
    const std::uint32_t bit = 1u << monitorIndex;
    const bool alreadySelected = (settings_.replayMonitorMask & bit) != 0;
    if (!alreadySelected && settings_.replayMonitorMask != 0 &&
        std::popcount(settings_.replayMonitorMask) >= LUNASHOT_MAX_ACTIVE_REPLAY_MONITORS) {
        recordingStatus_ = L"Three-monitor limit reached";
        recordingDetail_ = L"Disable one replay source before arming another monitor";
        InvalidateUi();
        return;
    }
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
    if (std::popcount(monitorMask) > LUNASHOT_MAX_ACTIVE_REPLAY_MONITORS) {
        recordingStatus_ = L"Choose no more than three monitors";
        recordingDetail_ = L"Replay is capped at three simultaneous encoders for predictable performance";
        InvalidateUi();
        return;
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
            settings_.regionHotkey, settings_.activeWindowHotkey, settings_.recordingHotkey,
            settings_.replaySaveHotkey, settings_.quickPanelHotkey};
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

void App::TriggerCapture(CaptureMode mode, int requestedMonitorIndex) {
    const bool interactive = mode == CaptureMode::Region || mode == CaptureMode::ActiveWindow;
    if (interactive && (interactiveCaptureGate_.IsActive() || regionEditor_.IsOpen())) {
        // Keep one frozen-desktop session alive from request through editor completion. This also
        // covers the recorder/replay path, which returns before the normal engine guard below.
        if (regionEditor_.IsOpen()) SetForegroundWindow(regionEditor_.WindowHandle());
        DispatchPluginEvent(L"capture_failed");
        return;
    }
    if (interactive && !interactiveCaptureGate_.TryBegin()) {
        DispatchPluginEvent(L"capture_failed");
        return;
    }

    HMONITOR requestedMonitor = nullptr;
    if (requestedMonitorIndex >= 0) {
        recordingOutputs_ = CaptureEngine::EnumerateOutputs();
        for (const auto& output : recordingOutputs_) {
            if (output.linearIndex == requestedMonitorIndex) { requestedMonitor = output.monitor; break; }
        }
        if (!requestedMonitor) {
            if (interactive) interactiveCaptureGate_.End();
            engineStatus_ = L"Plugin monitor is unavailable";
            engineDetail_ = L"Connected monitor index " + std::to_wstring(requestedMonitorIndex) + L" was not found";
            DispatchPluginEvent(L"capture_failed");
            InvalidateUi();
            return;
        }
    }

    if (captureSuspendedForRecording_) {
        if (!BeginRecorderFrameCapture(mode, requestedMonitorIndex)) {
            if (interactive) interactiveCaptureGate_.End();
            DispatchPluginEvent(L"capture_failed");
        } else {
            // A recorder-backed region/window request can already have all of its
            // fallback frames when BeginRecorderFrameCapture returns. Publish the
            // acceptance before completing that request, because completion may
            // synchronously fail while opening the editor.
            const bool completeReadyBatch = recorderCaptureBatch_ &&
                recorderCaptureBatch_->pendingFrames == 0;
            DispatchPluginEvent(L"capture_queued");
            if (completeReadyBatch && recorderCaptureBatch_) CompleteRecorderFrameCapture();
        }
        InvalidateUi();
        return;
    }
    if (!engine_) {
        if (interactive) interactiveCaptureGate_.End();
        DispatchPluginEvent(L"capture_failed");
        return;
    }
    HWND targetWindow = nullptr;
    HMONITOR monitor = nullptr;
    if (mode == CaptureMode::ActiveDisplay) monitor = requestedMonitor ? requestedMonitor : GetPriorityMonitor(hwnd_);
    if (mode == CaptureMode::ActiveWindow) {
        targetWindow = GetPriorityCaptureWindow(hwnd_);
        if (!targetWindow) {
            if (interactive) interactiveCaptureGate_.End();
            engineStatus_ = L"No capturable window";
            engineDetail_ = L"Focus a normal application window and try again";
            DispatchPluginEvent(L"capture_failed");
            InvalidateUi();
            return;
        }
        monitor = MonitorFromWindow(targetWindow, MONITOR_DEFAULTTONEAREST);
    }
    const std::wstring filenamePrefix = L"Screenshot";
    if (!engine_->RequestCapture(mode, monitor, targetWindow, filenamePrefix, settings_)) {
        if (interactive) interactiveCaptureGate_.End();
        engineStatus_ = L"Capture queue is busy";
        engineDetail_ = L"Try again in a moment";
        DispatchPluginEvent(L"capture_failed");
        InvalidateUi();
    } else {
        DispatchPluginEvent(L"capture_queued");
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

bool App::BeginRecorderFrameCapture(CaptureMode mode, int requestedMonitorIndex) {
    if (!engine_ || recorderCaptureBatch_) {
        engineStatus_ = L"Capture is already preparing";
        engineDetail_ = L"Wait for the current live-frame snapshot";
        return false;
    }
    recordingOutputs_ = CaptureEngine::EnumerateOutputs();
    if (mode == CaptureMode::ActiveDisplay) {
        HMONITOR monitor = GetPriorityMonitor(hwnd_);
        if (requestedMonitorIndex >= 0) {
            monitor = nullptr;
            for (const auto& output : recordingOutputs_) {
                if (output.linearIndex == requestedMonitorIndex) { monitor = output.monitor; break; }
            }
            if (!monitor) {
                engineStatus_ = L"Plugin monitor is unavailable";
                engineDetail_ = L"The requested monitor disconnected before capture";
                return false;
            }
        }
        if (auto* recorder = RecorderForMonitor(monitor); recorder && recorder->CaptureScreenshot(settings_)) {
            engineStatus_ = L"Fullscreen screenshot queued";
            engineDetail_ = L"Copied from the live recorder frame; replay continues";
            return true;
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
                return true;
            }
        }
        engineStatus_ = L"Screenshot unavailable";
        engineDetail_ = L"The requested monitor has no replay or desktop frame";
        return false;
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
    // Reject a fully synchronous no-source request here. Otherwise the call
    // below emits capture_failed while TriggerCapture subsequently reports the
    // same rejected request as capture_queued.
    if (batch->pendingFrames == 0 && batch->frames.empty()) {
        engineStatus_ = L"Capture unavailable";
        engineDetail_ = L"No replay or fallback desktop frame was ready";
        return false;
    }
    recorderCaptureBatch_ = std::move(batch);
    engineStatus_ = mode == CaptureMode::AllDisplays ? L"Capturing all displays" :
                    mode == CaptureMode::ActiveWindow ? L"Preparing window picker" : L"Preparing region editor";
    engineDetail_ = L"Replay remains active while live frames are frozen";
    return true;
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
        if (batch->mode == CaptureMode::Region || batch->mode == CaptureMode::ActiveWindow) {
            interactiveCaptureGate_.End();
        }
        engineStatus_ = L"Capture unavailable";
        engineDetail_ = L"No replay or fallback desktop frame was ready";
        DispatchPluginEvent(L"capture_failed");
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
            DispatchPluginEvent(L"capture_saved");
            if (page_ == Page::Photos) RefreshMediaLibrary(false);
            break;
        case EngineEventType::RegionFrame:
            if (regionEditor_.IsOpen()) {
                AppendDiagnosticLog(L"Ignored a duplicate interactive capture frame while the region editor was open");
                break;
            }
            if (!interactiveCaptureGate_.IsActive()) interactiveCaptureGate_.TryBegin();
            engineStatus_ = event->mode == CaptureMode::ActiveWindow ? L"Window picker" : L"Region editor";
            engineDetail_ = event->mode == CaptureMode::ActiveWindow ? L"Hover a window and click to capture" : L"Select an area and annotate it";
            UnregisterAllHotkeys();
            if (!regionEditor_.Show(event->frame, event->mode == CaptureMode::ActiveWindow ? RegionEditor::SessionMode::WindowPicker : RegionEditor::SessionMode::Region, settings_, event->filenamePrefix)) {
                interactiveCaptureGate_.End();
                engineStatus_ = event->mode == CaptureMode::ActiveWindow ? L"Window picker failed" : L"Region editor failed";
                engineDetail_ = L"The frozen desktop surface could not be created";
                DispatchPluginEvent(L"capture_failed");
                RegisterAllHotkeys();
            }
            break;
        case EngineEventType::RecorderFrame: break;
        case EngineEventType::Error:
            if ((event->mode == CaptureMode::Region || event->mode == CaptureMode::ActiveWindow) &&
                !regionEditor_.IsOpen()) {
                interactiveCaptureGate_.End();
            }
            engineStatus_ = L"Capture error";
            engineDetail_ = event->detail;
            if (settings_.notifications) overlay_.ShowError(event->detail, settings_.overlayDurationMs,
                                                             GetPriorityMonitor(hwnd_));
            DispatchPluginEvent(L"capture_failed");
            break;
    }
    InvalidateUi();
}

void App::HandleRegionResult(std::unique_ptr<RegionResult> result) {
    interactiveCaptureGate_.End();
    // RegionEditor hides itself before posting this result. Drop any screenshot
    // textures that might still contain that full-desktop overlay before hotkeys are
    // registered again and another request can be queued.
    if (engine_) engine_->InvalidateOverlayFrames();
    if (!result || result->action == RegionResultAction::Cancel) {
        RegisterAllHotkeys();
        engineStatus_ = L"Ready";
        engineDetail_ = L"Capture cancelled";
        DispatchPluginEvent(L"capture_cancelled");
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
    if (copied) DispatchPluginEvent(L"capture_copied");

    const wchar_t* savedTitle = windowMode ? L"Window captured" : L"Region captured";
    const wchar_t* copiedDetail = windowMode ? L"Window copied to clipboard" : L"Region copied to clipboard";

    if (wantsSave && engine_) {
        AppSettings saveSettings = settings_;
        // Region/window crops are already the exact pixels the user chose; forcing the
        // global "Screenshot resolution" preset onto them would stretch a small selection
        // to that fixed size and make it blurry/pixelated. Always save these at native size.
        saveSettings.outputWidth = 0;
        saveSettings.outputHeight = 0;
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
            recordingStatus_ = monitor + L" replay saved · buffer reset";
            recordingDetail_ = PathFileName(event->path) + L" · this monitor restarted at 0:00";
            if (settings_.notifications) overlay_.ShowMessage(monitor + L" replay saved", PathFileName(event->path),
                                                               settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
            DispatchPluginEvent(L"replay_saved");
        } else if (event->type == RecordingEventType::Error) {
            recordingStatus_ = monitor + L" replay error";
            recordingDetail_ = event->detail;
            AppendDiagnosticLog(monitor + L" replay channel: " + event->detail);
            DispatchPluginEvent(L"replay_failed");
        } else if (event->type == RecordingEventType::Recovering) {
            recordingStatus_ = monitor + L" replay reconnecting";
            recordingDetail_ = L"The buffer continues with recovery frames";
            DispatchPluginEvent(L"replay_recovering");
        } else if (event->type == RecordingEventType::Recovered) {
            recordingStatus_ = monitor + L" replay restored";
            recordingDetail_ = L"GPU capture resumed without resetting the buffer";
            DispatchPluginEvent(L"replay_recovered");
        } else if (event->type == RecordingEventType::AudioWarning) {
            AppendDiagnosticLog(monitor + L" audio: " + event->detail);
        }
        if (event->type == RecordingEventType::ReplaySaved && page_ == Page::Videos) RefreshMediaLibrary(true);
        if (quickPanel_.IsVisible()) quickPanel_.SetState(BuildQuickPanelState());
        InvalidateUi();
        return;
    }
    const bool primaryWasReplay = event->sessionGeneration != 0
        ? event->replaySession
        : (primaryReplayMonitorIndex_ >= 0 ||
           (recordingEngine_ && recordingEngine_->IsReplayBuffering()));
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
            DispatchPluginEvent(L"recording_started");
            break;
        case RecordingEventType::ReplayStarted:
            recordingStatus_ = auxiliaryReplayEngines_.empty() ? L"Replay buffer live" : L"Multi-monitor replay live";
            recordingDetail_ = std::to_wstring(1 + auxiliaryReplayEngines_.size()) + L" monitor channel(s) · last " +
                               std::to_wstring(settings_.replaySeconds) + L" seconds";
            if (settings_.sound) soundPlayer_.Play();
            DispatchPluginEvent(L"replay_started");
            break;
        case RecordingEventType::Recovering:
            // Deliberately no overlay toast here: this fires both for a real
            // display/fullscreen change AND every time the black-frame retry
            // loop cycles the capture backend, which can repeat every few
            // seconds on a source that's stuck black — a debounced toast
            // still popped unpredictably on those cycles. The recording never
            // actually stops (see the source-recovery fix), so this status
            // line is informational only; nothing needs to interrupt the user.
            recordingStatus_ = L"Holding recording";
            recordingDetail_ = L"Fullscreen mode changed · reconnecting GPU capture";
            DispatchPluginEvent(primaryWasReplay ? L"replay_recovering" : L"recording_recovering");
            break;
        case RecordingEventType::Recovered:
            recordingStatus_ = recordingEngine_ && recordingEngine_->IsReplayBuffering()
                ? L"Replay buffer live" : L"Recording live";
            recordingDetail_ = L"GPU capture restored without stopping the MP4";
            DispatchPluginEvent(primaryWasReplay ? L"replay_recovered" : L"recording_recovered");
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
            FinishActiveRecordingSession(*event);
            DispatchPluginEvent(L"recording_saved");
            if (page_ == Page::Videos) RefreshMediaLibrary(true);
            break;
        case RecordingEventType::ReplaySaved:
            recordingElapsedMs_ = 0;
            recordingFrames_ = 0;
            recordingDroppedFrames_ = 0;
            recordingEncodedFps_ = 0.0;
            lastRecordingPath_ = event->path;
            recordingStatus_ = L"Replay saved · buffer reset";
            recordingDetail_ = PathFileName(event->path) + L" · new rolling window started at 0:00";
            if (settings_.sound) soundPlayer_.Play();
            if (settings_.notifications) overlay_.ShowMessage(L"Replay saved", PathFileName(event->path),
                                                               settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
            DispatchPluginEvent(L"replay_saved");
            if (page_ == Page::Videos) RefreshMediaLibrary(true);
            break;
        case RecordingEventType::ReplayStopped:
            recordingStatus_ = L"Ready to record";
            recordingDetail_ = event->detail;
            activeRecordingMonitorIndex_ = -1;
            primaryReplayMonitorIndex_ = -1;
            ResumeCaptureAfterRecording();
            DispatchPluginEvent(L"replay_stopped");
            break;
        case RecordingEventType::RecordingStopped:
            recordingStatus_ = L"Ready to record";
            recordingDetail_ = event->detail;
            FinishActiveRecordingSession(*event);
            DispatchPluginEvent(L"recording_stopped");
            break;
        case RecordingEventType::Error:
            recordingStatus_ = L"Recording error";
            recordingDetail_ = event->detail;
            if (settings_.notifications) overlay_.ShowError(event->detail, settings_.overlayDurationMs,
                                                             GetPriorityMonitor(hwnd_));
            if (RecordingEventMatchesSession(*event, activeRecordingSessionGeneration_)) {
                FinishActiveRecordingSession(*event);
            } else if (event->sessionGeneration == 0 && activeRecordingSessionGeneration_ != 0 &&
                       (!recordingEngine_ || (!recordingEngine_->IsBusy() &&
                                              !recordingEngine_->IsRecording() && !IsReplayBuffering()))) {
                // Engine-wide initialization errors predate a concrete worker
                // session, so they deliberately carry generation zero.
                activeRecordingSessionGeneration_ = 0;
                activeRecordingMonitorIndex_ = -1;
                primaryReplayMonitorIndex_ = -1;
                ResumeCaptureAfterRecording();
            } else if (primaryWasReplay &&
                       (!recordingEngine_ || (!recordingEngine_->IsBusy() &&
                                              !recordingEngine_->IsRecording() && !IsReplayBuffering()))) {
                primaryReplayMonitorIndex_ = -1;
                ResumeCaptureAfterRecording();
            }
            DispatchPluginEvent(primaryWasReplay ? L"replay_failed" : L"recording_failed");
            break;
    }
    if (quickPanel_.IsVisible()) quickPanel_.SetState(BuildQuickPanelState());
    InvalidateUi();
}

void App::HandlePreviewFrame(std::unique_ptr<PreviewFrame> frame) {
    if (!frame) return;
    const bool liveRecording = frame->liveRecording;
    if (liveRecording) {
        int monitorIndex = -1;
        RecordingEngine* sourceEngine = nullptr;
        if (frame->channelId == 0) {
            sourceEngine = recordingEngine_.get();
            monitorIndex = recordingEngine_ && recordingEngine_->IsReplayBuffering()
                ? primaryReplayMonitorIndex_ : activeRecordingMonitorIndex_;
        } else {
            monitorIndex = frame->channelId - 1;
            sourceEngine = ReplayPreviewEngine(monitorIndex);
        }
        const bool valid = !frame->bgra.empty() && frame->width && frame->height;
        const bool visible = IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
        if (frame->channelId == 0 && page_ == Page::Record && settings_.previewEnabled && visible && valid) {
            ReleasePreviewFrame();
            previewFrame_ = std::move(frame);
            InvalidateUi();
        } else if (page_ == Page::Replay && settings_.replayPreviewEnabled && visible && valid && monitorIndex >= 0 &&
                   monitorIndex < static_cast<int>(replayPreviewFrames_.size())) {
            auto& target = replayPreviewFrames_[static_cast<std::size_t>(monitorIndex)];
            if (target) {
                RecordingEngine* oldEngine = target->channelId == 0 ? recordingEngine_.get()
                    : ReplayPreviewEngine(target->channelId - 1);
                if (oldEngine) oldEngine->RecyclePreviewFrame(std::move(target));
                else target.reset();
            }
            target = std::move(frame);
            InvalidateUi();
        } else if (sourceEngine) {
            sourceEngine->RecyclePreviewFrame(std::move(frame));
        }
        if (sourceEngine) sourceEngine->AcknowledgePreview();
        return;
    }

    ReleasePreviewFrame();
    if (!frame->bgra.empty() && frame->width && frame->height && page_ == Page::Record &&
        settings_.previewEnabled && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
        previewFrame_ = std::move(frame);
        InvalidateUi();
    } else if (engine_) {
        engine_->RecyclePreviewFrame(std::move(frame));
    }
    if (engine_) engine_->AcknowledgePreview();
}

void App::HandleUpdateCheckResult(std::unique_ptr<UpdateCheckResult> result) {
    if (!result) return;
    switch (result->outcome) {
        case UpdateCheckOutcome::UpdateAvailable: {
            updateUiState_ = UpdateUiState::UpdateAvailable;
            updateLatestVersion_ = result->latestVersion;
            if (result->manual) {
                // Luna Capture has no in-place patcher or downloader by design (see the license's
                // stance on bundling/loaders) - the honest thing to tell the user is to grab a
                // fresh copy themselves, not silently rewrite files out from under them.
                const std::vector<std::wstring> items{L"Open the download page", L"Not now"};
                const int choice = ChooseNamedItem(hwnd_, items, 0, L"A newer version is available",
                    L"Luna Capture " + result->latestVersion + L" is out; you're on " + std::wstring(kAppVersion) +
                    L". Delete this copy and download the latest release from GitHub.");
                if (choice == 0) {
                    ShellExecuteW(hwnd_, L"open", kUpdateReleasesUrl, nullptr, nullptr, SW_SHOWNORMAL);
                }
            } else if (settings_.notifications) {
                // A background hit must not pop a modal out of nowhere (the window is often hidden
                // in the tray); the persistent top-bar banner plus one toast is enough.
                overlay_.ShowWarning(L"Update available",
                                     L"Luna Capture " + result->latestVersion + L" is on GitHub · you're on " + std::wstring(kAppVersion),
                                     settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
            }
            break;
        }
        case UpdateCheckOutcome::UpToDate:
            updateUiState_ = UpdateUiState::UpToDate;
            updateLatestVersion_.clear();
            if (result->manual && settings_.notifications) {
                overlay_.ShowMessage(L"You're up to date",
                                     L"Luna Capture " + std::wstring(kAppVersion) + L" is the latest release",
                                     settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
            }
            break;
        case UpdateCheckOutcome::Failed:
            // Keep a known update visible even if a later re-check hits a network error —
            // updateLatestVersion_ survives the Checking transition exactly for this.
            updateUiState_ = updateLatestVersion_.empty() ? UpdateUiState::Failed
                                                          : UpdateUiState::UpdateAvailable;
            if (result->manual && settings_.notifications) {
                overlay_.ShowWarning(L"Update check failed", L"GitHub could not be reached · try again later",
                                     settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
            }
            break;
    }
    InvalidateUi();
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

RecordingEngine* App::ReplayPreviewEngine(int monitorIndex) {
    if (recordingEngine_ && (monitorIndex == primaryReplayMonitorIndex_ ||
                             monitorIndex == activeRecordingMonitorIndex_)) {
        return recordingEngine_.get();
    }
    for (auto& channel : auxiliaryReplayEngines_) {
        if (channel.monitorIndex == monitorIndex && channel.engine) return channel.engine.get();
    }
    return nullptr;
}

void App::ReleaseReplayPreviewFrames() {
    for (auto& frame : replayPreviewFrames_) {
        if (!frame) continue;
        RecordingEngine* sourceEngine = frame->channelId == 0 ? recordingEngine_.get()
            : ReplayPreviewEngine(frame->channelId - 1);
        if (sourceEngine) sourceEngine->RecyclePreviewFrame(std::move(frame));
        else frame.reset();
    }
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

bool App::FinishActiveRecordingSession(const RecordingEvent& event) {
    if (!RecordingEventMatchesSession(event, activeRecordingSessionGeneration_)) return false;
    activeRecordingSessionGeneration_ = 0;
    activeRecordingMonitorIndex_ = -1;
    primaryReplayMonitorIndex_ = -1;
    ResumeCaptureAfterRecording();
    return true;
}

void App::HandleTrayCommand(TrayCommand command, std::size_t dataIndex) {
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
        case TrayCommand::PluginCommand:
            if (dataIndex < trayPluginCommands_.size()) {
                const auto& pluginCommand = trayPluginCommands_[dataIndex];
                ExecutePluginExecution(pluginSystem_.ExecuteCommand(pluginCommand.pluginId, pluginCommand.commandId));
            }
            break;
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
    if (mediaDetailOpen_ && page != Page::Photos && page != Page::Videos) CloseMediaDetail();
    page_ = page;
    if (page != Page::Record) {
        if (engine_) engine_->SetPreviewConsumerActive(false);
        ReleasePreviewFrame();
    }
    if (page != Page::Replay) {
        for (auto& channel : auxiliaryReplayEngines_) {
            if (channel.engine) channel.engine->SetPreviewConsumerActive(false);
        }
        ReleaseReplayPreviewFrames();
    }
    if (page == Page::Audio) RefreshAudioSources();
    if (page == Page::Replay) recordingOutputs_ = CaptureEngine::EnumerateOutputs();
    if (page == Page::Photos) RefreshMediaLibrary(false);
    if (page == Page::Videos) RefreshMediaLibrary(true);
    InvalidateUi();
}

float App::HoverAmount(Action action) const { return hoverAnimation_[static_cast<std::size_t>(action)]; }
float App::PressAmount(Action action) const { return pressAnimation_[static_cast<std::size_t>(action)]; }

void App::TickAnimations() {
    const bool windowVisible = IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
    const HWND foreground = GetForegroundWindow();
    const bool windowForeground = windowVisible &&
        (foreground == hwnd_ || (foreground && GetAncestor(foreground, GA_ROOTOWNER) == hwnd_));
    // A covered Luna window is technically still "visible". Treating it as a
    // preview consumer kept scaling and painting frames behind fullscreen games.
    //
    // But "not foreground" is not the same as "covered": alt-tabbing into a game
    // on another monitor leaves Luna fully on screen next to it, and freezing the
    // preview there just leaves a stale image on a display the user is watching.
    // A foreground window on a different monitor cannot be covering us, so treat
    // that as still-visible; same-monitor stays gated exactly as before.
    bool windowUncovered = windowForeground;
    if (windowVisible && !windowForeground) {
        windowUncovered = !foreground ||
                          MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST) !=
                              MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    }
    const bool recordPreviewVisible = page_ == Page::Record && settings_.previewEnabled && windowUncovered;
    const bool replayPreviewVisible = page_ == Page::Replay && settings_.replayPreviewEnabled && windowUncovered;
    if (engine_) engine_->SetPreviewConsumerActive(recordPreviewVisible);
    if (recordingEngine_) {
        recordingEngine_->SetPreviewFrameRate(replayPreviewVisible ? kReplayPreviewFps : settings_.previewFps);
        recordingEngine_->SetPreviewConsumerActive(recordPreviewVisible || replayPreviewVisible);
    }
    for (auto& channel : auxiliaryReplayEngines_) {
        if (!channel.engine) continue;
        channel.engine->SetPreviewFrameRate(kReplayPreviewFps);
        channel.engine->SetPreviewConsumerActive(replayPreviewVisible);
    }
    // Plugin timers ("on timer every N { ... }", e.g. auto-screenshot) must keep
    // firing while minimized to tray or behind a fullscreen game, so this runs
    // unconditionally instead of being gated on windowForeground below.
    const ULONGLONG timerNow = GetTickCount64();
    if (timerNow - lastPluginTimerTick_ >= 1000) {
        lastPluginTimerTick_ = timerNow;
        // One timer scan is one root execution chain. This bounds the combined
        // cost of every due handler and any synchronous events they fan out to,
        // instead of granting a fresh recursive budget to every timer.
        PluginExecutionBudgetScope timerBudgetScope(pluginExecutionScopeDepth_, pluginInstructionBudgetRemaining_);
        for (const auto& execution : pluginSystem_.DispatchDueTimers(static_cast<std::uint64_t>(timerNow))) {
            ExecutePluginExecution(execution);
        }
    }
    const ULONGLONG now = timerNow;
    // Plugin source is a background surface too. Reload while minimized so a
    // changed timer, locale, or Quick Panel command takes effect immediately.
    if (pluginModalDepth_ == 0 && now - lastPluginScanTick_ >= 1000) {
        lastPluginScanTick_ = now;
        if (pluginSystem_.ReloadIfChanged()) {
            ApplyPluginTheme();
            pluginStatus_ = L"Hot reload · " + pluginSystem_.LastReloadSummary();
            if (!themeAssetStatus_.empty()) pluginStatus_ += L" · Theme: " + themeAssetStatus_;
            DispatchPluginEvent(L"plugins_reloaded");
        }
    }
    if (!windowUncovered) return;
    if (ui::IsThemeBackdropAnimated()) InvalidateUi();
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
    const float targetY = 73.0f + static_cast<float>(static_cast<int>(page_)) * 40.0f;
    const float nextY = sidebarIndicatorY_ + (targetY - sidebarIndicatorY_) * (std::min)(1.0f, 0.24f * delta);
    changed |= std::fabs(nextY - sidebarIndicatorY_) > 0.05f;
    sidebarIndicatorY_ = nextY;
    if (changed) InvalidateUi();
}

void App::AddRegion(int x, int y, int width, int height, Action action, int data) {
    RECT rect{x, y, x + width, y + height};
    if (paintingPageContent_) {
        OffsetRect(&rect, 0, -activePagePaintScrollY_);
        const RECT viewport{kSidebarWidth, kTopBarHeight, kWindowWidth, kWindowHeight};
        RECT clipped{};
        if (!IntersectRect(&clipped, &rect, &viewport)) return;
        rect = clipped;
    }
    hitRegions_.push_back(HitRegion{rect, action, data});
}

const App::HitRegion* App::HitTestRegion(POINT point) const {
    for (auto it = hitRegions_.rbegin(); it != hitRegions_.rend(); ++it) {
        if (PtInRect(&it->rect, point)) return &*it;
    }
    return nullptr;
}

App::Action App::HitTestAction(POINT point) const {
    const HitRegion* region = HitTestRegion(point);
    return region ? region->action : Action::None;
}

bool App::GuardSessionSettingChange(const wchar_t* settingName) {
    if ((!recordingEngine_ || !recordingEngine_->IsRecording()) && !IsReplayBuffering()) return true;
    recordingStatus_ = IsReplayBuffering() ? L"Setting locked while replay is live" : L"Setting locked while recording";
    recordingDetail_ = std::wstring(settingName ? settingName : L"This setting") +
                       L" changes the encoder pipeline; stop the active session first. Hotkeys, screenshots, preview and notifications stay editable.";
    InvalidateUi();
    return false;
}

void App::ExecuteAction(Action action, int data) {
    switch (action) {
        case Action::ChooseRecordingFolder:
        case Action::CycleRecordingFps:
        case Action::CycleVideoCodec:
        case Action::CycleCapturePriority:
        case Action::CycleRateControl:
        case Action::CycleEncoderEffort:
        case Action::CycleVideoQuality:
        case Action::CycleKeyframeInterval:
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
        case Action::Hide: HideMainWindow(); break;
        case Action::Exit: ExitApplication(); PostQuitMessage(0); break;
        case Action::NavHome: NavigateTo(Page::Home); break;
        case Action::NavCapture: NavigateTo(Page::Capture); break;
        case Action::NavPhotos: NavigateTo(Page::Photos); break;
        case Action::NavVideos: NavigateTo(Page::Videos); break;
        case Action::NavRecord: NavigateTo(Page::Record); break;
        case Action::NavReplay: NavigateTo(Page::Replay); break;
        case Action::NavEncoder: NavigateTo(Page::Encoder); break;
        case Action::NavAudio: NavigateTo(Page::Audio); break;
        case Action::NavPlugins: NavigateTo(Page::Plugins); break;
        case Action::NavSettings: NavigateTo(Page::Settings); break;
        case Action::PluginsReload: ReloadPlugins(); break;
        case Action::PluginsOpenFolder: OpenFolderInExplorer(pluginSystem_.RootDirectory().wstring()); break;
        case Action::PluginsOpenDocs: OpenPluginDocumentation(); break;
        case Action::PluginsBrowse: BrowsePlugins(); break;
        case Action::PluginsResetTheme:
            pluginSystem_.ResetTheme(); ApplyPluginTheme(); pluginStatus_ = L"Default Luna theme restored"; break;
        case Action::Plugin0Toggle: TogglePlugin(0); break;
        case Action::Plugin1Toggle: TogglePlugin(1); break;
        case Action::Plugin2Toggle: TogglePlugin(2); break;
        case Action::Plugin0Manage: ManagePlugin(0); break;
        case Action::Plugin1Manage: ManagePlugin(1); break;
        case Action::Plugin2Manage: ManagePlugin(2); break;
        case Action::ToggleStartHiddenToTray:
            settings_.startHiddenToTray = !settings_.startHiddenToTray;
            settingsStore_.Save(settings_);
            break;
        case Action::PhotosOpenFolder: OpenFolderInExplorer(settings_.saveDirectory); break;
        case Action::VideosOpenFolder: OpenFolderInExplorer(settings_.recordingDirectory); break;
        case Action::RefreshMediaLibrary: RefreshMediaLibrary(page_ == Page::Videos); break;
        case Action::OpenMediaItem:
            if (data >= 0) OpenMediaDetail(page_ == Page::Videos, static_cast<std::size_t>(data));
            break;
        case Action::MediaDetailClose: CloseMediaDetail(); break;
        case Action::MediaDetailPrev: StepMediaDetail(-1); break;
        case Action::MediaDetailNext: StepMediaDetail(1); break;
        case Action::MediaDetailPlayPause: mediaViewer_.TogglePlayback(); break;
        case Action::MediaDetailSkipBack: mediaViewer_.SkipBy(-10); break;
        case Action::MediaDetailSkipForward: mediaViewer_.SkipBy(10); break;
        case Action::MediaDetailMute: mediaViewer_.ToggleMute(); break;
        case Action::MediaDetailFullscreen:
            SetMediaDetailFullscreen(!mediaDetailFullscreen_);
            break;
        case Action::MediaDetailDelete: DeleteMediaDetailItem(); break;
        case Action::MediaDetailFolder: {
            const auto& library = mediaDetailIsVideo_ ? videoLibrary_ : photoLibrary_;
            if (mediaDetailIndex_ < library.size()) OpenFolderInExplorer(library[mediaDetailIndex_].path.parent_path().wstring());
            break;
        }
        case Action::CaptureActive: TriggerCapture(CaptureMode::ActiveDisplay); break;
        case Action::CaptureAll: TriggerCapture(CaptureMode::AllDisplays); break;
        case Action::CaptureRegion: TriggerCapture(CaptureMode::Region); break;
        case Action::CaptureWindow: TriggerCapture(CaptureMode::ActiveWindow); break;
        case Action::OpenFolder: OpenFolderInExplorer(settings_.saveDirectory); break;
        case Action::ChooseFolder: {
            std::wstring selected;
            if (ChooseFolder(hwnd_, settings_.saveDirectory, selected)) {
                settings_.saveDirectory = selected;
                ApplyChronologicalFolderView(settings_.saveDirectory);
                SaveSettings();
            }
            break;
        }
        case Action::ToggleFormat:
            settings_.imageFormat = settings_.imageFormat == ImageFormat::Png ? ImageFormat::Jpeg : ImageFormat::Png;
            SaveSettings(); break;
        case Action::ToggleNotifications: settings_.notifications = !settings_.notifications; SaveSettings(); break;
        case Action::ToggleSound: settings_.sound = !settings_.sound; SaveSettings(); break;
        case Action::ToggleCopyRegion: settings_.copyRegionToClipboard = !settings_.copyRegionToClipboard; SaveSettings(); break;
        case Action::TestOverlay:
            // A test has to exercise both halves of a real capture notification,
            // otherwise there is no way to check the chosen sound from in-app.
            if (settings_.sound) soundPlayer_.Play();
            overlay_.ShowMessage(L"Luna Capture",
                                 settings_.sound ? L"Notification and sound are ready" : L"Notifications are ready",
                                 settings_.overlayDurationMs, GetPriorityMonitor(hwnd_));
            break;
        case Action::CheckForUpdates:
            if (updateUiState_ != UpdateUiState::Checking) {
                updateUiState_ = UpdateUiState::Checking;
                StartUpdateCheck(hwnd_, WM_APP_UPDATE_CHECK, true);
            }
            break;
        case Action::OpenUpdatePage:
            ShellExecuteW(hwnd_, L"open", kUpdateReleasesUrl, nullptr, nullptr, SW_SHOWNORMAL);
            break;
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
            if (ChooseFolder(hwnd_, settings_.recordingDirectory, selected)) {
                settings_.recordingDirectory = selected;
                ApplyChronologicalFolderView(settings_.recordingDirectory);
                SaveSettings();
            }
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
            // Order matches the VideoCodec enum. Anything past AV1 depends on an
            // encoder MFT this machine may not have — the recorder says so clearly
            // when a start fails, so they are all offered rather than hidden.
            const std::vector<std::wstring> codecs{
                L"H.264 / AVC · MP4 · maximum compatibility",
                L"HEVC / H.265 · MP4 · smaller files, modern players",
                L"AV1 · MP4 · best compression, needs a recent GPU encoder",
                L"VP9 · MP4 · royalty-free, needs a vendor encoder",
                L"H.264 fragmented · fMP4 · survives a crash mid-recording",
                L"HEVC fragmented · fMP4 · crash-safe H.265",
                L"MPEG-4 Part 2 · MP4 · plays on very old devices",
                L"H.263 · 3GP · legacy mobile",
                L"Windows Media Video 9 · WMV · native Windows playback",
                L"VC-1 Advanced · WMV · Windows Media successor to WMV9",
                L"Motion JPEG · AVI · every frame a keyframe, huge but easy to cut",
                L"H.264 in AVI · AVI · for editors that refuse MP4"};
            const int choice = ChooseNamedItem(hwnd_, codecs, static_cast<int>(settings_.videoCodec),
                                               L"Video codec",
                                               L"The container and file extension follow the codec · hardware encoder preferred");
            if (choice >= 0) {
                settings_.videoCodec = static_cast<VideoCodec>(choice);
                SaveSettings();
            }
            break;
        }
        case Action::CycleCapturePriority: {
            const std::vector<std::wstring> modes{
                L"Balanced · hardware encoder, GPU resize when downscaling",
                L"Prefer GPU · least CPU cost — for weaker processors",
                L"Prefer CPU · software encoder, leaves the GPU to the game"};
            const int choice = ChooseNamedItem(
                hwnd_, modes, static_cast<int>(settings_.capturePriority), L"Recording load",
                L"Capture always starts on the GPU; this chooses who does the resizing and encoding");
            if (choice >= 0) {
                settings_.capturePriority = static_cast<CapturePriority>(choice);
                SaveSettings();
            }
            break;
        }
        case Action::CycleRateControl: {
            const std::vector<std::wstring> modes{
                L"CBR · constant bitrate — predictable file size",
                L"VBR · variable bitrate — spends more only on busy scenes",
                L"Constant quality · bitrate follows the quality level"};
            const int choice = ChooseNamedItem(hwnd_, modes, static_cast<int>(settings_.videoRateControl),
                                               L"Rate control",
                                               L"Encoders that do not implement a mode keep their own default");
            if (choice >= 0) { settings_.videoRateControl = static_cast<VideoRateControl>(choice); SaveSettings(); }
            break;
        }
        case Action::CycleEncoderEffort: {
            const std::vector<std::wstring> modes{
                L"Speed · lowest encoder cost, largest files",
                L"Balanced · the default trade-off",
                L"Quality · best picture per bit, costs the most"};
            const int choice = ChooseNamedItem(hwnd_, modes, static_cast<int>(settings_.videoEncoderEffort),
                                               L"Encoder effort",
                                               L"Spends more of whichever processor is doing the encoding");
            if (choice >= 0) { settings_.videoEncoderEffort = static_cast<VideoEncoderEffort>(choice); SaveSettings(); }
            break;
        }
        case Action::CycleVideoQuality: {
            int value = settings_.videoQuality;
            const std::vector<IntegerPreset> presets{{40, L"40 · small files"}, {55, L"55 · light"},
                {70, L"70 · balanced"}, {85, L"85 · high"}, {95, L"95 · near-lossless"}};
            if (ChooseIntegerPreset(hwnd_, L"Quality level",
                                    L"Only used when rate control is set to constant quality",
                                    presets, value, 1, 100, value)) {
                settings_.videoQuality = value;
                SaveSettings();
            }
            break;
        }
        case Action::CycleKeyframeInterval: {
            int value = settings_.keyframeSeconds;
            const std::vector<IntegerPreset> presets{{0, L"Auto · leave it to the encoder"},
                {1, L"1 second · easiest to cut"}, {2, L"2 seconds · recommended"},
                {4, L"4 seconds · smaller files"}, {8, L"8 seconds · smallest"}};
            if (ChooseIntegerPreset(hwnd_, L"Keyframe interval",
                                    L"Shorter intervals make seeking and trimming smoother at a small bitrate cost",
                                    presets, value, 0, 10, value)) {
                settings_.keyframeSeconds = value;
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
        case Action::ToggleReplayPreview:
            settings_.replayPreviewEnabled = !settings_.replayPreviewEnabled;
            if (!settings_.replayPreviewEnabled) {
                ReleaseReplayPreviewFrames();
                if (recordingEngine_) recordingEngine_->SetPreviewConsumerActive(false);
                for (auto& channel : auxiliaryReplayEngines_) {
                    if (channel.engine) channel.engine->SetPreviewConsumerActive(false);
                }
            }
            SaveSettings();
            break;
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
        case Action::RecordQuickPanelHotkey: BeginHotkeyRecording(kQuickPanelHotkeySlot); break;
        case Action::ToggleActiveHotkey: ToggleHotkeySlot(0); break;
        case Action::ToggleAllHotkey: ToggleHotkeySlot(1); break;
        case Action::ToggleRegionHotkey: ToggleHotkeySlot(2); break;
        case Action::ToggleWindowHotkey: ToggleHotkeySlot(3); break;
        case Action::ToggleRecordingHotkey: ToggleHotkeySlot(4); break;
        case Action::ToggleReplayHotkey: ToggleHotkeySlot(5); break;
        case Action::ToggleQuickPanelHotkey: ToggleHotkeySlot(kQuickPanelHotkeySlot); break;
        case Action::ToggleAutostart:
            settings_.startWithWindows = !settings_.startWithWindows;
            SaveSettings(true);
            break;
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
    FillThemeRole(dc, sidebar, ui::ThemeRole::Sidebar);
    FillThemeRole(dc, RECT{kSidebarWidth - 1, 0, kSidebarWidth, kWindowHeight}, ui::ThemeRole::Border);

    // Logo mark: a rounded gradient tile with the capture glyph, next to the wordmark.
    RECT logoTile{22, 20, 54, 52};
    ui::FillRoundRectVGradient(dc, logoTile, 9, RGB(122, 162, 255), RGB(78, 116, 232));
    DrawUiIcon(dc, UiIcon::Capture, RECT{logoTile.left + 7, logoTile.top + 7, logoTile.right - 7, logoTile.bottom - 7}, RGB(255, 255, 255), 2);
    TextThemeRole(dc, L"Luna Capture", RECT{62, 20, 194, 52}, ui::ThemeRole::Text, fontLogo_);

    FillRoundThemeRole(dc, RECT{16, 64, 180, 478}, 16, ui::ThemeRole::Surface);
    FillRoundThemeRole(dc, RECT{18, static_cast<int>(sidebarIndicatorY_), 22, static_cast<int>(sidebarIndicatorY_) + 30},
                       3, ui::ThemeRole::Accent);

    const struct { const wchar_t* label; int y; Action action; Page page; UiIcon icon; } items[] = {
        {L"Home", 70, Action::NavHome, Page::Home, UiIcon::Home},
        {L"Capture", 110, Action::NavCapture, Page::Capture, UiIcon::Capture},
        {L"Photos", 150, Action::NavPhotos, Page::Photos, UiIcon::Photo},
        {L"Videos", 190, Action::NavVideos, Page::Videos, UiIcon::Video},
        {L"Record", 230, Action::NavRecord, Page::Record, UiIcon::Record},
        {L"Replay", 270, Action::NavReplay, Page::Replay, UiIcon::Replay},
        {L"Encoder", 310, Action::NavEncoder, Page::Encoder, UiIcon::Video},
        {L"Audio", 350, Action::NavAudio, Page::Audio, UiIcon::Microphone},
        {L"Plugins", 390, Action::NavPlugins, Page::Plugins, UiIcon::Plugin},
        {L"Settings", 430, Action::NavSettings, Page::Settings, UiIcon::Settings},
    };
    for (const auto& item : items) {
        RECT rect{30, item.y, 166, item.y + 36};
        const float hover = HoverAmount(item.action);
        if (hover > 0.01f) FillRound(dc, rect, 10, Mix(RGB(21, 24, 30), RGB(34, 39, 49), hover));
        const COLORREF navColor = ui::ThemeColor(page_ == item.page ? ui::ThemeRole::Text : ui::ThemeRole::Muted);
        ui::ScopedThemeBypass bypass;
        DrawUiIcon(dc, item.icon, RECT{43, item.y + 9, 61, item.y + 27}, navColor, 2);
        Text(dc, item.label, RECT{68, item.y, 160, item.y + 36}, navColor, fontTitle_);
        AddRegion(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, item.action);
    }

    Text(dc, L"Native C++", RECT{24, 566, 170, 590}, RGB(111, 118, 132), fontSmall_);
    Text(dc, L"Version " + std::wstring(kAppVersion), RECT{24, 588, 170, 612}, RGB(84, 90, 102), fontSmall_);
}

void App::PaintTopBar(HDC dc) {
    RECT top{kSidebarWidth, 0, kWindowWidth, kTopBarHeight};
    FillThemeRole(dc, top, ui::ThemeRole::Topbar);
    const wchar_t* name = page_ == Page::Home ? L"Home" : page_ == Page::Capture ? L"Capture" :
                          page_ == Page::Photos ? L"Photos" : page_ == Page::Videos ? L"Videos" :
                          page_ == Page::Record ? L"Record" : page_ == Page::Replay ? L"Instant Replay" :
                          page_ == Page::Encoder ? L"Video & encoder" :
                          page_ == Page::Audio ? L"Audio mixer" : page_ == Page::Plugins ? L"Plugins" : L"Settings";
    TextThemeRole(dc, name, RECT{kSidebarWidth + 30, 14, kWindowWidth - 170, 52}, ui::ThemeRole::Text, fontTitle_);

    // Close only. Minimising made no sense here: the window has no taskbar button
    // to restore from, so it just hid the same way Close already does.
    {
        RECT rect{920, 14, 954, 48};
        const float hover = HoverAmount(Action::Hide);
        if (hover > 0.01f) FillRound(dc, rect, 10, Mix(RGB(14, 16, 20), RGB(39, 43, 52), hover));
        DrawUiIcon(dc, UiIcon::Close, RECT{rect.left + 9, rect.top + 9, rect.right - 9, rect.bottom - 9},
                   RGB(190, 196, 208), 2);
        AddRegion(rect.left, rect.top, 34, 34, Action::Hide);
    }

    // Keyed off the remembered version rather than the state, so pressing "Check again" does not
    // make the banner blink out for the duration of the request.
    if (!updateLatestVersion_.empty()) {
        // Persistent amber pill next to the window buttons; click opens the GitHub release page.
        RECT pill{624, 14, 866, 48};
        const float hover = HoverAmount(Action::OpenUpdatePage);
        FillRound(dc, pill, 10, Mix(RGB(54, 44, 22), RGB(74, 59, 28), hover));
        StrokeRound(dc, pill, 10, Mix(RGB(122, 96, 42), RGB(178, 140, 62), hover));
        DrawUiIcon(dc, UiIcon::Warning, RECT{pill.left + 11, pill.top + 8, pill.left + 29, pill.top + 26},
                   RGB(246, 180, 72), 2);
        Text(dc, L"Update " + updateLatestVersion_ + L" available",
             RECT{pill.left + 37, pill.top, pill.right - 12, pill.bottom}, RGB(248, 205, 116), fontSmall_,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        AddRegion(pill.left, pill.top, pill.right - pill.left, pill.bottom - pill.top, Action::OpenUpdatePage);
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
        Text(dc, card.detail, RECT{rect.left + 16, rect.top + 80, rect.right - 12, rect.top + 108}, RGB(124, 132, 146), fontSmall_, DT_LEFT | DT_WORDBREAK | DT_WORD_ELLIPSIS);
        // A combo like "Shift + Print Screen" is too wide for the card at fontSmall_ and would
        // ellipsize mid-word; fontTiny_ keeps every default binding on one line without shrinking
        // the badge/title above it, with DT_WORD_ELLIPSIS as a safety net for longer custom binds.
        Text(dc, card.hotkey, RECT{rect.left + 16, rect.bottom - 32, rect.right - 14, rect.bottom - 12},
             RGB(113, 169, 255), fontTiny_, DT_LEFT | DT_WORDBREAK | DT_WORD_ELLIPSIS);
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

    // Image format is the only real screenshot output knob. There is deliberately no "output
    // resolution" control any more - screenshots always save at full native capture resolution, so
    // a downscale preset can never silently soften a photo.
    {
        RECT row{x, 274, 582, 336};
        FillRound(dc, row, 14, Mix(RGB(20, 23, 29), RGB(28, 33, 42), HoverAmount(Action::ToggleFormat)));
        StrokeRound(dc, row, 14, RGB(39, 44, 54));
        DrawUiIcon(dc, UiIcon::Capture, RECT{x + 17, 294, x + 37, 314}, RGB(121, 168, 242), 2);
        Text(dc, L"Image format", RECT{x + 49, 280, 450, 308}, RGB(235, 238, 244), fontTitle_);
        Text(dc, settings_.imageFormat == ImageFormat::Png ? L"PNG · lossless" : L"JPEG · smaller files",
             RECT{x + 49, 305, 470, 330}, RGB(124, 132, 146), fontSmall_);
        Text(dc, settings_.imageFormat == ImageFormat::Png ? L"PNG" : L"JPEG",
             RECT{430, 274, 560, 336}, RGB(111, 167, 255), fontNormal_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        AddRegion(row.left, row.top, row.right - row.left, row.bottom - row.top, Action::ToggleFormat);
    }
    {
        // Static, non-interactive reassurance that captures are never downscaled.
        RECT row{x, 350, 582, 412};
        FillRound(dc, row, 14, RGB(18, 24, 20));
        StrokeRound(dc, row, 14, RGB(37, 58, 44));
        DrawUiIcon(dc, UiIcon::Monitor, RECT{x + 17, 370, x + 37, 390}, RGB(126, 211, 165), 2);
        Text(dc, L"Full quality", RECT{x + 49, 356, 450, 384}, RGB(235, 238, 244), fontTitle_);
        Text(dc, L"Screenshots are always saved at native resolution", RECT{x + 49, 381, 470, 406},
             RGB(124, 132, 146), fontSmall_);
        Text(dc, L"NATIVE", RECT{430, 350, 560, 412}, RGB(126, 211, 165), fontNormal_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
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

void App::PaintMediaLibrary(HDC dc, bool videos) {
    const int x = 228;
    const auto& library = videos ? videoLibrary_ : photoLibrary_;
    const wchar_t* heading = videos ? L"Video library" : L"Photo library";
    const wchar_t* detail = videos
        ? L"Recordings and instant replays · click any file to open it in Luna Video Player."
        : L"Screenshots in one place · click any file to open it in Luna Photo Viewer.";
    Text(dc, heading, RECT{x, 76, 700, 112}, RGB(244, 246, 251), fontHeadline_);
    Text(dc, detail, RECT{x, 111, 760, 139}, RGB(137, 144, 158), fontNormal_,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT refresh{774, 91, 858, 127};
    RECT folder{866, 91, 956, 127};
    FillRound(dc, refresh, 10, Mix(RGB(27, 32, 40), RGB(42, 50, 63), HoverAmount(Action::RefreshMediaLibrary)));
    FillRound(dc, folder, 10, Mix(RGB(34, 50, 76), RGB(47, 72, 111),
                                  HoverAmount(videos ? Action::VideosOpenFolder : Action::PhotosOpenFolder)));
    DrawUiIcon(dc, UiIcon::Replay, RECT{786, 100, 804, 118}, RGB(133, 177, 241), 2);
    Text(dc, L"Refresh", RECT{808, 91, 852, 127}, RGB(190, 202, 219), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawUiIcon(dc, UiIcon::Folder, RECT{877, 100, 895, 118}, RGB(151, 190, 246), 2);
    Text(dc, L"Folder", RECT{900, 91, 949, 127}, RGB(211, 224, 242), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddRegion(refresh.left, refresh.top, refresh.right - refresh.left, refresh.bottom - refresh.top, Action::RefreshMediaLibrary);
    AddRegion(folder.left, folder.top, folder.right - folder.left, folder.bottom - folder.top,
              videos ? Action::VideosOpenFolder : Action::PhotosOpenFolder);

    std::uintmax_t totalBytes = 0;
    for (const auto& item : library) totalBytes += item.sizeBytes;
    RECT summary{x, 148, 956, 211};
    FillRound(dc, summary, 14, RGB(20, 23, 29));
    StrokeRound(dc, summary, 14, RGB(39, 45, 56));
    RECT iconTile{x + 14, 159, x + 55, 200};
    FillRound(dc, iconTile, 12, videos ? RGB(57, 43, 91) : RGB(36, 66, 91));
    DrawUiIcon(dc, videos ? UiIcon::Video : UiIcon::Photo, RECT{x + 24, 169, x + 45, 190},
               videos ? RGB(184, 153, 247) : RGB(141, 204, 249), 2);
    Text(dc, std::to_wstring(library.size()) + (videos ? L" videos" : L" photos"),
         RECT{x + 68, 151, 470, 180}, RGB(232, 236, 244), fontTitle_);
    Text(dc, library.empty() ? L"The library updates automatically when new media is saved" :
             L"Newest first · lightweight indexing keeps memory usage low",
         RECT{x + 68, 179, 650, 206}, RGB(104, 116, 134), fontSmall_);
    Text(dc, FormatMediaSize(totalBytes), RECT{680, 148, 932, 211}, RGB(125, 174, 247), fontTitle_,
         DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    Text(dc, L"RECENT MEDIA", RECT{x, 220, 600, 244}, RGB(103, 114, 131), fontSmall_);
    if (library.empty()) {
        RECT empty{x, 250, 956, 570};
        FillRound(dc, empty, 16, RGB(18, 21, 27));
        StrokeRound(dc, empty, 16, RGB(36, 42, 52));
        DrawUiIcon(dc, videos ? UiIcon::Video : UiIcon::Photo, RECT{566, 326, 618, 378}, RGB(62, 72, 88), 2);
        Text(dc, videos ? L"No recordings yet" : L"No screenshots yet", RECT{x, 392, 956, 430},
             RGB(170, 178, 191), fontTitle_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        Text(dc, videos ? L"Start recording or save an instant replay — it will appear here automatically."
                        : L"Take a screenshot — it will appear here automatically.",
             RECT{x + 40, 431, 916, 470}, RGB(92, 102, 118), fontSmall_,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }

    constexpr int kTileCols = 4;
    constexpr int kTileGap = 12;
    constexpr int kThumbHeight = 118;
    constexpr int kLabelHeight = 54;
    constexpr int kTileHeight = kThumbHeight + kLabelHeight;
    const int contentWidth = 956 - x;
    const int tileWidth = (contentWidth - (kTileCols - 1) * kTileGap) / kTileCols;

    for (std::size_t index = 0; index < library.size(); ++index) {
        const auto& item = library[index];
        const int column = static_cast<int>(index % kTileCols);
        const int row = static_cast<int>(index / kTileCols);
        const int left = x + column * (tileWidth + kTileGap);
        const int top = 250 + row * (kTileHeight + kTileGap);
        RECT tile{left, top, left + tileWidth, top + kTileHeight};
        const bool hover = hoveredAction_ == Action::OpenMediaItem && hoveredActionData_ == static_cast<int>(index);
        FillRound(dc, tile, 13, hover ? RGB(27, 34, 45) : RGB(20, 23, 29));
        StrokeRound(dc, tile, 13, hover ? RGB(61, 94, 143) : RGB(38, 44, 54));

        RECT thumb{left + 6, top + 6, left + tileWidth - 6, top + 6 + kThumbHeight - 6};
        FillRound(dc, thumb, 9, videos ? RGB(28, 24, 38) : RGB(17, 27, 36));
        if (!item.thumbnailBgra.empty()) {
            DrawBgraFit(dc, thumb, item.thumbnailBgra, item.thumbnailWidth, item.thumbnailHeight);
        } else {
            const int cx = (thumb.left + thumb.right) / 2;
            const int cy = (thumb.top + thumb.bottom) / 2;
            DrawUiIcon(dc, videos ? UiIcon::Video : UiIcon::Photo, RECT{cx - 16, cy - 16, cx + 16, cy + 16},
                       videos ? RGB(120, 100, 165) : RGB(90, 130, 160), 2);
        }
        if (videos) {
            RECT badge{thumb.left + 8, thumb.bottom - 30, thumb.left + 30, thumb.bottom - 8};
            ui::FillEllipse(dc, badge, RGB(0, 0, 0), 130);
            DrawUiIcon(dc, UiIcon::Play, RECT{badge.left + 5, badge.top + 5, badge.right - 5, badge.bottom - 5},
                       RGB(255, 255, 255), 2);
        }

        RECT nameRect{left + 10, top + kThumbHeight + 4, tile.right - 10, top + kThumbHeight + 26};
        Text(dc, item.name, nameRect, RGB(230, 234, 242), fontSmall_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT metaRect{left + 10, top + kThumbHeight + 26, tile.right - 10, top + kThumbHeight + 48};
        Text(dc, FormatMediaTime(item.modified) + L" · " + FormatMediaSize(item.sizeBytes), metaRect,
             videos ? RGB(157, 133, 214) : RGB(108, 158, 216), fontSmall_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        AddRegion(tile.left, tile.top, tile.right - tile.left, tile.bottom - tile.top, Action::OpenMediaItem,
                  static_cast<int>(index));
    }
}

void App::PaintMediaDetail(HDC dc) {
    auto& library = mediaDetailIsVideo_ ? videoLibrary_ : photoLibrary_;
    if (mediaDetailIndex_ >= library.size()) { CloseMediaDetail(); return; }
    const auto& item = library[mediaDetailIndex_];

    const RECT panel = mediaDetailFullscreen_ ? RECT{0, kTopBarHeight, kWindowWidth, kWindowHeight}
                                               : RECT{kSidebarWidth, kTopBarHeight, kWindowWidth, kWindowHeight};
    Fill(dc, panel, RGB(9, 11, 15));
    const RECT canvas = MediaDetailCanvasRect();

    if (!mediaDetailFullscreen_) {
        RECT closeBtn{kSidebarWidth + 16, kTopBarHeight + 14, kSidebarWidth + 52, kTopBarHeight + 50};
        FillRound(dc, closeBtn, 10, Mix(RGB(29, 34, 43), RGB(44, 53, 68), HoverAmount(Action::MediaDetailClose)));
        DrawUiIcon(dc, UiIcon::Close, RECT{closeBtn.left + 9, closeBtn.top + 9, closeBtn.right - 9, closeBtn.bottom - 9},
                   RGB(214, 219, 229), 2);
        AddRegion(closeBtn.left, closeBtn.top, closeBtn.right - closeBtn.left, closeBtn.bottom - closeBtn.top,
                  Action::MediaDetailClose);

        int right = kWindowWidth - 16;
        RECT deleteBtn{right - 84, kTopBarHeight + 14, right, kTopBarHeight + 50};
        right -= 84 + 10;
        FillRound(dc, deleteBtn, 10, Mix(RGB(43, 26, 30), RGB(74, 33, 40), HoverAmount(Action::MediaDetailDelete)));
        Text(dc, L"Delete", deleteBtn, RGB(233, 158, 168), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddRegion(deleteBtn.left, deleteBtn.top, deleteBtn.right - deleteBtn.left, deleteBtn.bottom - deleteBtn.top,
                  Action::MediaDetailDelete);

        RECT folderBtn{right - 84, kTopBarHeight + 14, right, kTopBarHeight + 50};
        right -= 84 + 10;
        FillRound(dc, folderBtn, 10, Mix(RGB(27, 32, 40), RGB(42, 50, 63), HoverAmount(Action::MediaDetailFolder)));
        DrawUiIcon(dc, UiIcon::Folder, RECT{folderBtn.left + 12, folderBtn.top + 9, folderBtn.left + 30, folderBtn.top + 27},
                   RGB(151, 190, 246), 2);
        Text(dc, L"Folder", RECT{folderBtn.left + 34, folderBtn.top, folderBtn.right - 6, folderBtn.bottom},
             RGB(211, 224, 242), fontSmall_, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        AddRegion(folderBtn.left, folderBtn.top, folderBtn.right - folderBtn.left, folderBtn.bottom - folderBtn.top,
                  Action::MediaDetailFolder);

        if (mediaDetailIsVideo_) {
            RECT fullscreenBtn{right - 36, kTopBarHeight + 14, right, kTopBarHeight + 50};
            right -= 36 + 10;
            FillRound(dc, fullscreenBtn, 10, Mix(RGB(27, 32, 40), RGB(42, 50, 63), HoverAmount(Action::MediaDetailFullscreen)));
            DrawFullscreenGlyph(dc, fullscreenBtn, RGB(196, 207, 223), false);
            AddRegion(fullscreenBtn.left, fullscreenBtn.top, fullscreenBtn.right - fullscreenBtn.left,
                      fullscreenBtn.bottom - fullscreenBtn.top, Action::MediaDetailFullscreen);
        }

        RECT nameRect{closeBtn.right + 14, kTopBarHeight + 14, right - 10, kTopBarHeight + 50};
        Text(dc, item.name, nameRect, RGB(238, 242, 249), fontTitle_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // Prev / next, centered on the canvas edges.
    const int navY = (canvas.top + canvas.bottom) / 2;
    const bool hasPrev = mediaDetailIndex_ > 0;
    const bool hasNext = mediaDetailIndex_ + 1 < library.size();
    RECT prevBtn{canvas.left + 10, navY - 22, canvas.left + 54, navY + 22};
    RECT nextBtn{canvas.right - 54, navY - 22, canvas.right - 10, navY + 22};
    if (hasPrev) {
        ui::FillEllipse(dc, prevBtn, RGB(12, 14, 18), hoveredAction_ == Action::MediaDetailPrev ? 210 : 150);
        Text(dc, L"‹", prevBtn, RGB(238, 242, 249), fontHeadline_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddRegion(prevBtn.left, prevBtn.top, prevBtn.right - prevBtn.left, prevBtn.bottom - prevBtn.top, Action::MediaDetailPrev);
    }
    if (hasNext) {
        ui::FillEllipse(dc, nextBtn, RGB(12, 14, 18), hoveredAction_ == Action::MediaDetailNext ? 210 : 150);
        Text(dc, L"›", nextBtn, RGB(238, 242, 249), fontHeadline_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddRegion(nextBtn.left, nextBtn.top, nextBtn.right - nextBtn.left, nextBtn.bottom - nextBtn.top, Action::MediaDetailNext);
    }

    if (mediaDetailIsVideo_) {
        if (!mediaDetailStatus_.empty()) {
            Text(dc, mediaDetailStatus_, canvas, RGB(200, 130, 138), fontNormal_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // The video itself paints through the embedded child window; only the
        // control bar below the canvas is ours to draw.
        RECT bar{canvas.left, canvas.bottom + 8, canvas.right, canvas.bottom + 74};
        FillRound(dc, bar, 14, RGB(17, 20, 26));

        const bool playing = mediaViewer_.IsPlaying();
        RECT playBtn{bar.left + 12, bar.top + 9, bar.left + 56, bar.bottom - 9};
        FillRound(dc, playBtn, 12, RGB(48, 83, 143));
        DrawUiIcon(dc, playing ? UiIcon::Pause : UiIcon::Play,
                   RECT{(playBtn.left + playBtn.right) / 2 - 11, (playBtn.top + playBtn.bottom) / 2 - 11,
                        (playBtn.left + playBtn.right) / 2 + 11, (playBtn.top + playBtn.bottom) / 2 + 11},
                   RGB(255, 255, 255), 2);
        AddRegion(playBtn.left, playBtn.top, playBtn.right - playBtn.left, playBtn.bottom - playBtn.top, Action::MediaDetailPlayPause);

        RECT backBtn{playBtn.right + 8, bar.top + 9, playBtn.right + 48, bar.bottom - 9};
        RECT fwdBtn{backBtn.right + 4, bar.top + 9, backBtn.right + 44, bar.bottom - 9};
        FillRound(dc, backBtn, 10, RGB(28, 33, 42));
        FillRound(dc, fwdBtn, 10, RGB(28, 33, 42));
        DrawSkipGlyph(dc, backBtn, RGB(206, 214, 228), false);
        DrawSkipGlyph(dc, fwdBtn, RGB(206, 214, 228), true);
        AddRegion(backBtn.left, backBtn.top, backBtn.right - backBtn.left, backBtn.bottom - backBtn.top, Action::MediaDetailSkipBack);
        AddRegion(fwdBtn.left, fwdBtn.top, fwdBtn.right - fwdBtn.left, fwdBtn.bottom - fwdBtn.top, Action::MediaDetailSkipForward);

        RECT muteBtn{bar.right - 44, bar.top + 9, bar.right - 8, bar.bottom - 9};
        RECT volumeRect{muteBtn.left - 96, (bar.top + bar.bottom) / 2 - 5, muteBtn.left - 12, (bar.top + bar.bottom) / 2 + 5};
        RECT timeRect{volumeRect.left - 118, bar.top + 9, volumeRect.left - 12, bar.bottom - 9};
        RECT progressRect{fwdBtn.right + 16, (bar.top + bar.bottom) / 2 - 5, timeRect.left - 16, (bar.top + bar.bottom) / 2 + 5};
        mediaProgressRect_ = progressRect;
        mediaVolumeRect_ = volumeRect;

        const LONGLONG current = mediaViewer_.PositionValue();
        const LONGLONG duration = mediaViewer_.DurationValue();
        FillRound(dc, progressRect, 5, RGB(38, 43, 52));
        if (duration > 0) {
            const int filled = static_cast<int>((progressRect.right - progressRect.left) * current / duration);
            RECT fill{progressRect.left, progressRect.top, progressRect.left + filled, progressRect.bottom};
            FillRound(dc, fill, 5, RGB(91, 147, 236));
            const int knobX = progressRect.left + filled;
            const int knobCy = (progressRect.top + progressRect.bottom) / 2;
            ui::FillEllipse(dc, RECT{knobX - 7, knobCy - 7, knobX + 7, knobCy + 7}, RGB(226, 234, 248));
        }
        const auto timeText = [](LONGLONG value100ns) {
            const LONGLONG seconds = (std::max<LONGLONG>)(0, value100ns / 10000000LL);
            wchar_t buffer[32]{};
            swprintf_s(buffer, L"%02lld:%02lld", seconds / 60, seconds % 60);
            return std::wstring(buffer);
        };
        Text(dc, timeText(current) + L" / " + timeText(duration), timeRect, RGB(150, 162, 182), fontSmall_,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        const bool muted = mediaViewer_.IsMuted();
        const float volume = mediaViewer_.Volume();
        FillRound(dc, muteBtn, 10, RGB(30, 35, 44));
        DrawUiIcon(dc, UiIcon::Sound, RECT{muteBtn.left + 8, (muteBtn.top + muteBtn.bottom) / 2 - 11, muteBtn.left + 30,
                                          (muteBtn.top + muteBtn.bottom) / 2 + 11},
                   (muted || volume <= 0.0001f) ? RGB(103, 110, 123) : RGB(135, 180, 244), 2);
        AddRegion(muteBtn.left, muteBtn.top, muteBtn.right - muteBtn.left, muteBtn.bottom - muteBtn.top, Action::MediaDetailMute);
        FillRound(dc, volumeRect, 5, RGB(38, 43, 52));
        const float vol = muted ? 0.0f : volume;
        const int volFilled = static_cast<int>((volumeRect.right - volumeRect.left) * vol);
        if (volFilled > 0) {
            FillRound(dc, RECT{volumeRect.left, volumeRect.top, volumeRect.left + volFilled, volumeRect.bottom}, 5, RGB(120, 170, 240));
        }
        const int volKnobX = volumeRect.left + volFilled;
        const int volKnobCy = (volumeRect.top + volumeRect.bottom) / 2;
        ui::FillEllipse(dc, RECT{volKnobX - 6, volKnobCy - 6, volKnobX + 6, volKnobCy + 6}, RGB(226, 234, 248));
    } else {
        mediaProgressRect_ = RECT{};
        mediaVolumeRect_ = RECT{};
        if (!mediaDetailImageBgra_.empty()) {
            DrawBgraFit(dc, canvas, mediaDetailImageBgra_, static_cast<int>(mediaDetailImageWidth_),
                       static_cast<int>(mediaDetailImageHeight_));
        } else {
            Text(dc, mediaDetailStatus_.empty() ? L"Loading…" : mediaDetailStatus_, canvas, RGB(151, 160, 174),
                 fontNormal_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
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
        SetBrushOrgEx(dc, 0, 0, nullptr);
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
    Text(dc, recordingHotkeySlot_ == 4 ? L"Press keys…" : FormatHotkey(settings_.recordingHotkey.modifiers, settings_.recordingHotkey.vk), recordKey, RGB(112, 169, 255), fontTiny_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    Text(dc, recordingHotkeySlot_ == 5 ? L"Press keys…" : FormatHotkey(settings_.replaySaveHotkey.modifiers, settings_.replaySaveHotkey.vk), replayKey, RGB(150, 139, 245), fontTiny_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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
        // A live session locks anything that would reconfigure it. Navigation
        // chips stay live: the Encoder page does its own locking, and being able
        // to look at the settings mid-recording is useful.
        const bool locked = (recording || replay) && action != Action::CyclePreviewFps &&
                            action != Action::ConfigureReplayMonitors && action != Action::NavEncoder;
        FillRound(dc, rect, 10, locked ? RGB(18, 20, 25) : Mix(RGB(20, 23, 29), RGB(34, 41, 52), HoverAmount(action)));
        StrokeRound(dc, rect, 10, RGB(39, 45, 56));
        Text(dc, title, RECT{rect.left + 8, rect.top + 1, rect.right - 8, rect.top + 20}, locked ? RGB(75, 81, 93) : RGB(102, 112, 128), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, value, RECT{rect.left + 7, rect.top + 18, rect.right - 7, rect.bottom - 1}, locked ? RGB(102, 109, 122) : RGB(151, 191, 250), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        AddRegion(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, action);
    };
    // Only capture-source and preview controls live here. Everything that shapes
    // the resulting video file - codec, bitrate, FPS, resolution, cursor, replay
    // length - belongs to the Encoder page and is reachable through the ENCODER
    // chip, so there is exactly one place to change any of it.
    chip(RECT{228, 150, 330, 184}, L"DISPLAY", settings_.recordingAutoMonitor ? L"Auto" : L"Mon " + std::to_wstring(settings_.recordingMonitorIndex + 1), Action::CycleRecordingMonitor);
    chip(RECT{336, 150, 500, 184}, L"ENCODER",
         std::wstring(VideoCodecLabel(settings_.videoCodec)) + L" · " + std::to_wstring(settings_.recordingFps) + L" FPS · " +
             std::to_wstring(settings_.recordingBitrateMbps) + L"M", Action::NavEncoder);
    chip(RECT{506, 150, 640, 184}, L"OUTPUT",
         (settings_.recordingHeight == 0 ? std::wstring(L"Native") : std::to_wstring(settings_.recordingHeight) + L"p") +
             L" · " + (settings_.recordCursor ? L"cursor" : L"no cursor"), Action::NavEncoder);
    chip(RECT{646, 150, 772, 184}, L"REPLAY LENGTH", std::to_wstring(settings_.replaySeconds) + L" s", Action::NavEncoder);
    chip(RECT{778, 150, 848, 184}, L"PREVIEW", settings_.previewEnabled ? std::to_wstring(settings_.previewFps) + L" FPS" : L"Off", Action::CyclePreviewFps);
    chip(RECT{854, 150, 904, 184}, L"REPLAY", activeReplayChannels > 0 ? std::to_wstring(activeReplayChannels) + L" live" : L"Open", Action::ConfigureReplayMonitors);
    RECT folderChip{910, 150, 956, 184};
    FillRound(dc, folderChip, 10, (recording || replay) ? RGB(18, 20, 25) : Mix(RGB(20, 23, 29), RGB(34, 41, 52), HoverAmount(Action::ChooseRecordingFolder)));
    StrokeRound(dc, folderChip, 10, RGB(39, 45, 56));
    DrawUiIcon(dc, UiIcon::Folder, RECT{923, 157, 943, 177}, RGB(143, 183, 244), 2);
    AddRegion(folderChip.left, folderChip.top, folderChip.right - folderChip.left, folderChip.bottom - folderChip.top, Action::ChooseRecordingFolder);
}

void App::PaintEncoder(HDC dc) {
    const int x = 228;
    const bool live = (recordingEngine_ && recordingEngine_->IsRecording()) || IsReplayBuffering();

    Text(dc, L"Video and encoder", RECT{x, 76, 940, 112}, RGB(244, 246, 251), fontHeadline_);
    Text(dc, L"One place for every setting that shapes a video file. These apply to normal recordings "
             L"and to Instant Replay clips alike.",
         RECT{x, 111, 956, 152}, RGB(137, 144, 158), fontNormal_, DT_LEFT | DT_TOP | DT_WORDBREAK);

    if (live) {
        const RECT notice{x, 156, 956, 194};
        FillRound(dc, notice, 11, RGB(38, 32, 18));
        StrokeRound(dc, notice, 11, RGB(122, 96, 42));
        DrawUiIcon(dc, UiIcon::Warning, RECT{notice.left + 14, notice.top + 9, notice.left + 34, notice.top + 29},
                   RGB(246, 180, 72), 2);
        Text(dc, L"A recording or Replay buffer is live — encoder settings are locked until it stops.",
             RECT{notice.left + 44, notice.top, notice.right - 12, notice.bottom}, RGB(228, 198, 138), fontSmall_,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // Two columns of setting rows. Each row is a full-width click target with a
    // value chip on the right, matching the Settings page's behaviour cards.
    //
    // The row grows to fit its hint instead of being a fixed 62px: these hints are
    // long enough that a language pack, or a theme that scales the font up, pushed
    // the second wrapped line straight out through the bottom of the card.
    constexpr int kColumn = 354;
    constexpr int kTextLeft = 48;   // clear of the icon
    constexpr int kChipWidth = 84;
    constexpr int kTextRightInset = kChipWidth + 22;
    constexpr int kHintTop = 32;    // below the title band
    const int hintWidth = kColumn - kTextLeft - kTextRightInset;

    const auto rowHeight = [&](const wchar_t* hint) {
        return (std::max)(62, kHintTop + MeasuredTextHeight(dc, hint, fontSmall_, hintWidth) + 11);
    };

    const auto row = [&](int left, int top, int height, const wchar_t* title, const std::wstring& value,
                         const wchar_t* hint, Action action, UiIcon icon) {
        const RECT rect{left, top, left + kColumn, top + height};
        const int centerY = (rect.top + rect.bottom) / 2;
        FillRound(dc, rect, 13, live ? RGB(18, 20, 25)
                                     : Mix(RGB(20, 23, 29), RGB(29, 35, 44), HoverAmount(action)));
        StrokeRound(dc, rect, 13, RGB(39, 44, 54));
        DrawUiIcon(dc, icon, RECT{rect.left + 15, centerY - 11, rect.left + 37, centerY + 11},
                   live ? RGB(78, 88, 106) : RGB(126, 174, 245), 2);
        Text(dc, title, RECT{rect.left + kTextLeft, rect.top + 6, rect.right - kTextRightInset, rect.top + kHintTop},
             live ? RGB(140, 147, 160) : RGB(235, 238, 244), fontTitle_,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, hint, RECT{rect.left + kTextLeft, rect.top + kHintTop, rect.right - kTextRightInset, rect.bottom - 4},
             RGB(112, 122, 138), fontSmall_, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_WORD_ELLIPSIS);
        RECT chip{rect.right - kChipWidth - 14, centerY - 17, rect.right - 14, centerY + 17};
        FillRound(dc, chip, 9, RGB(30, 36, 46));
        Text(dc, value, chip, live ? RGB(104, 112, 128) : RGB(121, 173, 250), fontSmall_,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!live) AddRegion(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, action);
    };

    int y = live ? 206 : 164;
    const int rightX = x + kColumn + 10;

    // Both cards on a line take the taller of the two heights so the grid stays square.
    const auto rowPair = [&](const wchar_t* leftTitle, const std::wstring& leftValue, const wchar_t* leftHint,
                             Action leftAction, UiIcon leftIcon,
                             const wchar_t* rightTitle, const std::wstring& rightValue, const wchar_t* rightHint,
                             Action rightAction, UiIcon rightIcon) {
        const int height = (std::max)(rowHeight(leftHint), rowHeight(rightHint));
        row(x, y, height, leftTitle, leftValue, leftHint, leftAction, leftIcon);
        row(rightX, y, height, rightTitle, rightValue, rightHint, rightAction, rightIcon);
        y += height + 8;
    };

    Text(dc, L"PIPELINE", RECT{x, y, 600, y + 20}, RGB(106, 117, 134), fontSmall_);
    y += 26;

    const wchar_t* priorityValue = settings_.capturePriority == CapturePriority::Gpu ? L"GPU" :
                                   settings_.capturePriority == CapturePriority::Cpu ? L"CPU" : L"Balanced";
    const std::wstring codecHint = std::wstring(L"Container: ") + VideoContainerLabel(settings_.videoCodec);
    rowPair(L"Processing load", priorityValue, L"Who resizes and encodes",
            Action::CycleCapturePriority, UiIcon::Monitor,
            L"Codec", VideoCodecLabel(settings_.videoCodec), codecHint.c_str(),
            Action::CycleVideoCodec, UiIcon::Video);

    const wchar_t* rateValue = settings_.videoRateControl == VideoRateControl::Cbr ? L"CBR" :
                               settings_.videoRateControl == VideoRateControl::Quality ? L"Quality" : L"VBR";
    rowPair(L"Rate control", rateValue, L"How the bitrate is spent",
            Action::CycleRateControl, UiIcon::Record,
            L"Bitrate", std::to_wstring(settings_.recordingBitrateMbps) + L" Mbps",
            settings_.videoRateControl == VideoRateControl::Quality ? L"Ignored in constant quality"
                                                                    : L"Target for this codec",
            Action::CycleRecordingBitrate, UiIcon::Save);

    const wchar_t* effortValue = settings_.videoEncoderEffort == VideoEncoderEffort::Speed ? L"Speed" :
                                 settings_.videoEncoderEffort == VideoEncoderEffort::Quality ? L"Quality" : L"Balanced";
    rowPair(L"Encoder effort", effortValue, L"Better picture costs more CPU/GPU",
            Action::CycleEncoderEffort, UiIcon::Settings,
            L"Quality level", std::to_wstring(settings_.videoQuality),
            settings_.videoRateControl == VideoRateControl::Quality ? L"1-100, higher is better"
                                                                    : L"Constant quality mode only",
            Action::CycleVideoQuality, UiIcon::Check);
    y += 10;

    Text(dc, L"FRAME", RECT{x, y, 600, y + 20}, RGB(106, 117, 134), fontSmall_);
    y += 26;

    rowPair(L"Frame rate", std::to_wstring(settings_.recordingFps) + L" FPS",
            L"Recordings and Replay alike", Action::CycleRecordingFps, UiIcon::Play,
            L"Resolution",
            settings_.recordingHeight == 0 ? L"Native" : std::to_wstring(settings_.recordingHeight) + L"p",
            L"Below native resizes on the GPU", Action::CycleRecordingResolution, UiIcon::AllDisplays);

    rowPair(L"Keyframe interval",
            settings_.keyframeSeconds == 0 ? L"Auto" : std::to_wstring(settings_.keyframeSeconds) + L" s",
            L"Shorter clips cut more easily", Action::CycleKeyframeInterval, UiIcon::Region,
            L"Cursor", settings_.recordCursor ? L"Shown" : L"Hidden",
            L"Draw the pointer into the video", Action::ToggleRecordCursor, UiIcon::Cursor);

    rowPair(L"Audio bitrate", std::to_wstring(settings_.audioBitrateKbps) + L" kbps",
            L"AAC, 48 kHz stereo", Action::CycleAudioBitrate, UiIcon::Sound,
            L"Replay length", std::to_wstring(settings_.replaySeconds) + L" s",
            L"How far the buffer reaches back", Action::CycleReplaySeconds, UiIcon::Replay);

    encoderContentHeight_ = y + 12;
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
    std::size_t allCount = 0;
    const HMONITOR primaryMonitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    for (const auto& output : recordingOutputs_) {
        if (output.linearIndex < 0 || output.linearIndex >= static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) continue;
        if (allCount < LUNASHOT_MAX_ACTIVE_REPLAY_MONITORS) {
            allMask |= 1u << output.linearIndex;
            ++allCount;
        }
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
         RECT{806, 181, 934, 210}, live ? RGB(216, 218, 246) : RGB(100, 106, 119), fontTiny_,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    AddRegion(saveAll.left, saveAll.top, saveAll.right - saveAll.left, saveAll.bottom - saveAll.top, Action::SaveReplay);

    // A single monitor has nothing for "auto vs. primary vs. multi-monitor" to actually
    // choose between - recording always targets that one display either way, so the whole
    // profile picker is misleading clutter there. Reclaim its space instead of showing it.
    const bool hasMultiMonitorProfiles = recordingOutputs_.size() >= 2;
    const int dy = hasMultiMonitorProfiles ? 0 : -63;
    if (hasMultiMonitorProfiles) {
        const struct ProfileCard { RECT rect; std::wstring title; const wchar_t* detail; Action action; int id; } profiles[] = {
            {RECT{228, 236, 456, 289}, L"AUTO GAME", L"Follow foreground game", Action::ReplayProfileAuto, 0},
            {RECT{464, 236, 692, 289}, L"PRIMARY ONLY", L"Lowest resource usage", Action::ReplayProfilePrimary, 1},
            {RECT{700, 236, 956, 289}, recordingOutputs_.size() == 2 ? L"BOTH MONITORS" : L"UP TO 3 MONITORS",
             L"Independent live channel each", Action::ReplayProfileAll, 2},
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
    }

    // The rolling window is a property of the encoded clip, so it is edited on the
    // Encoder page - this tile only reports it and jumps there.
    const struct OptionButton { RECT rect; std::wstring title; std::wstring value; Action action; UiIcon icon; } options[] = {
        {RECT{228, 299 + dy, 448, 342 + dy}, L"Rolling window", std::to_wstring(settings_.replaySeconds) + L" seconds · in Encoder", Action::NavEncoder, UiIcon::Replay},
        {RECT{456, 299 + dy, 718, 342 + dy}, L"Save ALL shortcut", FormatHotkey(settings_.replaySaveHotkey.modifiers, settings_.replaySaveHotkey.vk), Action::RecordReplayAllHotkey, UiIcon::Save},
        {RECT{726, 299 + dy, 956, 342 + dy}, L"Replay files", L"Open output folder", Action::OpenRecordingFolder, UiIcon::Folder},
    };
    for (const auto& option : options) {
        FillRound(dc, option.rect, 11, Mix(RGB(20, 23, 29), RGB(31, 37, 47), HoverAmount(option.action)));
        StrokeRound(dc, option.rect, 11, RGB(39, 44, 54));
        DrawUiIcon(dc, option.icon, RECT{option.rect.left + 12, option.rect.top + 12, option.rect.left + 30, option.rect.top + 30}, RGB(119, 169, 245), 2);
        Text(dc, option.title, RECT{option.rect.left + 39, option.rect.top, option.rect.right - 10, option.rect.top + 23}, RGB(225, 230, 238), fontSmall_);
        Text(dc, option.value, RECT{option.rect.left + 39, option.rect.top + 20, option.rect.right - 10, option.rect.bottom}, RGB(128, 160, 207), fontSmall_);
        AddRegion(option.rect.left, option.rect.top, option.rect.right - option.rect.left, option.rect.bottom - option.rect.top, option.action);
    }

    Text(dc, L"LIVE MONITOR PREVIEWS", RECT{x, 350 + dy, 470, 379 + dy}, RGB(106, 117, 134), fontSmall_);
    Text(dc, L"Maximum 3 channels  ·  efficient 8 FPS preview  ·  recording FPS is unchanged",
         RECT{430, 350 + dy, 850, 379 + dy}, RGB(91, 101, 117), fontSmall_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    Text(dc, L"Preview", RECT{856, 350 + dy, 904, 379 + dy}, RGB(150, 158, 172), fontSmall_, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    DrawToggle(dc, 910, 352 + dy, settings_.replayPreviewEnabled, HoverAmount(Action::ToggleReplayPreview));
    AddRegion(905, 348 + dy, 61, 32, Action::ToggleReplayPreview);

    const auto isChannelLive = [this](int monitorIndex) {
        if (recordingEngine_ && recordingEngine_->IsReplayBuffering() && primaryReplayMonitorIndex_ == monitorIndex) return true;
        for (const auto& channel : auxiliaryReplayEngines_) {
            if (channel.monitorIndex == monitorIndex && channel.engine && channel.engine->IsReplayBuffering()) return true;
        }
        return false;
    };
    std::vector<const OutputInfo*> visibleOutputs;
    const auto appendOutput = [&visibleOutputs](const OutputInfo& output) {
        if (visibleOutputs.size() >= LUNASHOT_MAX_ACTIVE_REPLAY_MONITORS) return;
        if (output.linearIndex < 0 || output.linearIndex >= static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS)) return;
        if (std::find(visibleOutputs.begin(), visibleOutputs.end(), &output) == visibleOutputs.end()) visibleOutputs.push_back(&output);
    };
    for (const auto& output : recordingOutputs_) if (isChannelLive(output.linearIndex)) appendOutput(output);
    for (const auto& output : recordingOutputs_) {
        if (output.linearIndex >= 0 && output.linearIndex < static_cast<int>(LUNASHOT_MAX_REPLAY_MONITORS) &&
            (settings_.replayMonitorMask & (1u << output.linearIndex)) != 0) appendOutput(output);
    }
    for (const auto& output : recordingOutputs_) appendOutput(output);

    constexpr int cardWidth = 236;
    constexpr int cardGap = 10;
    for (std::size_t column = 0; column < visibleOutputs.size(); ++column) {
        const auto& output = *visibleOutputs[column];
        const int monitorIndex = output.linearIndex;
        const bool channelLive = isChannelLive(monitorIndex);
        const bool armed = (settings_.replayMonitorMask & (1u << monitorIndex)) != 0;
        const bool autoCandidate = settings_.replayMonitorMask == 0;
        const int left = x + static_cast<int>(column) * (cardWidth + cardGap);
        RECT card{left, 382 + dy, left + cardWidth, 607 + dy};
        FillRound(dc, card, 14, channelLive ? RGB(22, 32, 48) : armed ? RGB(21, 27, 37) : RGB(18, 21, 27));
        StrokeRound(dc, card, 14, channelLive ? RGB(60, 111, 190) : armed ? RGB(45, 65, 94) : RGB(36, 41, 51));
        Text(dc, L"Monitor " + std::to_wstring(monitorIndex + 1) + L"  ·  " + output.name,
             RECT{left + 12, 385 + dy, left + cardWidth - 12, 407 + dy}, RGB(230, 234, 242), fontTitle_);
        Text(dc, std::to_wstring(output.width) + L" × " + std::to_wstring(output.height),
             RECT{left + 12, 406 + dy, left + cardWidth - 12, 427 + dy}, RGB(101, 112, 129), fontSmall_);

        RECT viewport{left + 10, 430 + dy, left + cardWidth - 10, 551 + dy};
        FillRound(dc, viewport, 10, RGB(10, 12, 16));
        const auto& preview = replayPreviewFrames_[static_cast<std::size_t>(monitorIndex)];
        if (preview && !preview->bgra.empty() && preview->width && preview->height) {
            const double scale = (std::min)(static_cast<double>(viewport.right - viewport.left) / preview->width,
                                            static_cast<double>(viewport.bottom - viewport.top) / preview->height);
            const int drawWidth = (std::max)(1, static_cast<int>(preview->width * scale));
            const int drawHeight = (std::max)(1, static_cast<int>(preview->height * scale));
            const int drawX = viewport.left + ((viewport.right - viewport.left) - drawWidth) / 2;
            const int drawY = viewport.top + ((viewport.bottom - viewport.top) - drawHeight) / 2;
            BITMAPINFO bitmap{};
            bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = static_cast<LONG>(preview->width);
            bitmap.bmiHeader.biHeight = -static_cast<LONG>(preview->height);
            bitmap.bmiHeader.biPlanes = 1;
            bitmap.bmiHeader.biBitCount = 32;
            bitmap.bmiHeader.biCompression = BI_RGB;
            const int savedDc = SaveDC(dc);
            HRGN clip = CreateRoundRectRgn(viewport.left, viewport.top, viewport.right + 1, viewport.bottom + 1, 20, 20);
            SelectClipRgn(dc, clip);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, drawX, drawY, drawWidth, drawHeight, 0, 0, preview->width, preview->height,
                          preview->bgra.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
            SelectClipRgn(dc, nullptr);
            DeleteObject(clip);
            RestoreDC(dc, savedDc);
        } else {
            DrawUiIcon(dc, UiIcon::Monitor, RECT{left + 100, 454 + dy, left + 136, 490 + dy}, RGB(55, 65, 80), 2);
            Text(dc, !settings_.replayPreviewEnabled ? L"Preview disabled" :
                     channelLive ? L"Waiting for frame" : L"Starts with replay",
                 RECT{left + 18, 493 + dy, left + cardWidth - 18, 526 + dy},
                 RGB(75, 85, 101), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        StrokeRound(dc, viewport, 10, channelLive ? RGB(45, 83, 139) : RGB(31, 36, 45));

        const std::wstring state = channelLive ? L"LIVE" : armed ? L"ARMED" : autoCandidate ? L"AUTO" : L"OFF";
        RECT stateRect{viewport.left + 7, viewport.top + 7, viewport.left + 65, viewport.top + 30};
        FillRound(dc, stateRect, 8, channelLive ? RGB(43, 91, 160) : armed ? RGB(38, 57, 83) : RGB(29, 33, 41));
        Text(dc, state, stateRect, channelLive ? RGB(208, 230, 255) : armed ? RGB(147, 181, 226) : RGB(105, 113, 127),
             fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        const Action toggleAction = static_cast<Action>(static_cast<int>(Action::ReplayMonitor0Toggle) + monitorIndex);
        DrawToggle(dc, left + 10, 567 + dy, armed || channelLive, HoverAmount(toggleAction));
        AddRegion(left + 6, 560 + dy, 56, 38, toggleAction);

        const Action bindAction = static_cast<Action>(static_cast<int>(Action::ReplayMonitor0Bind) + monitorIndex);
        const auto& binding = settings_.replayMonitorHotkeys[static_cast<std::size_t>(monitorIndex)];
        RECT bindRect{left + 64, 562 + dy, left + 145, 596 + dy};
        FillRound(dc, bindRect, 9, Mix(RGB(28, 33, 42), RGB(42, 50, 64), HoverAmount(bindAction)));
        const std::wstring bindText = recordingHotkeySlot_ == 6 + monitorIndex ? L"Press…" :
                                      binding.vk ? FormatHotkey(binding.modifiers, binding.vk) : L"Set bind";
        Text(dc, bindText, bindRect, RGB(134, 177, 240), fontTiny_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        AddRegion(bindRect.left, bindRect.top, bindRect.right - bindRect.left, bindRect.bottom - bindRect.top, bindAction);

        const Action saveAction = static_cast<Action>(static_cast<int>(Action::ReplayMonitor0Save) + monitorIndex);
        RECT saveRect{left + 151, 562 + dy, left + cardWidth - 10, 596 + dy};
        FillRound(dc, saveRect, 9, Mix(channelLive ? RGB(67, 65, 146) : RGB(29, 33, 41),
                                      channelLive ? RGB(92, 86, 192) : RGB(38, 42, 51), HoverAmount(saveAction)));
        Text(dc, L"Save", saveRect, channelLive ? RGB(247, 246, 255) : RGB(99, 107, 121),
             fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddRegion(saveRect.left, saveRect.top, saveRect.right - saveRect.left, saveRect.bottom - saveRect.top, saveAction);
    }
    if (visibleOutputs.empty()) {
        RECT empty{x, 382 + dy, 956, 542 + dy};
        FillRound(dc, empty, 14, RGB(18, 21, 27));
        StrokeRound(dc, empty, 14, RGB(36, 41, 51));
        DrawUiIcon(dc, UiIcon::Monitor, RECT{568, 417 + dy, 616, 465 + dy}, RGB(61, 70, 84), 2);
        Text(dc, L"No connected displays detected", RECT{x, 474 + dy, 956, 514 + dy}, RGB(107, 116, 132), fontNormal_,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
            input.noiseSuppressionPercent > 0 ? L"Noise filter " + std::to_wstring(input.noiseSuppressionPercent) +
                L"% · gate -" + std::to_wstring(input.noiseGateThresholdDb) + L" dB" :
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

void App::PaintPlugins(HDC dc) {
    ui::ScopedTranslationBypass translationGuard;
    const int x = 228;
    const std::wstring hostApi = std::to_wstring(LunaScript::kSupportedHostApi);
    Text(dc, L"Plugins", RECT{x, 76, 940, 112}, RGB(244, 246, 251), fontHeadline_);
    Text(dc, L"LunaScript Host API " + hostApi + L" · advanced themes, UI control, capture, audio, settings, commands and events.",
         RECT{x, 111, 956, 139}, RGB(137, 144, 158), fontNormal_);

    RECT host{x, 148, 956, 237};
    FillRound(dc, host, 16, RGB(20, 23, 29));
    StrokeRound(dc, host, 16, pluginSystem_.ErrorCount() ? RGB(91, 49, 58) : RGB(43, 50, 62));
    RECT iconTile{x + 16, 164, x + 72, 220};
    ui::FillRoundRectVGradient(dc, iconTile, 16, RGB(72, 94, 153), RGB(48, 58, 105));
    DrawUiIcon(dc, UiIcon::Plugin, RECT{x + 31, 179, x + 57, 205}, RGB(206, 224, 255), 2);
    Text(dc, L"LunaScript Host · API " + hostApi, RECT{x + 88, 153, 660, 181}, RGB(236, 240, 247), fontTitle_);
    Text(dc, std::to_wstring(pluginSystem_.Plugins().size()) + L" installed  ·  " +
             std::to_wstring(pluginSystem_.EnabledCount()) + L" enabled  ·  " +
             std::to_wstring(pluginSystem_.CommandCount()) + L" commands  ·  " +
             std::to_wstring(pluginSystem_.ReviewCount()) + L" review  ·  " +
             std::to_wstring(pluginSystem_.ErrorCount()) + L" errors",
         RECT{x + 88, 178, 672, 202}, pluginSystem_.ErrorCount() ? RGB(229, 126, 140) : RGB(121, 138, 162), fontSmall_);
    const LoadedPlugin* themePlugin = pluginSystem_.ActiveThemePlugin();
    const LoadedPlugin* localePlugin = pluginSystem_.ActiveLocalePlugin();
    const std::wstring themeText = (themePlugin ? L"THEME · " + themePlugin->definition.theme.name : L"THEME · Luna Default") +
        (localePlugin ? L"   LANGUAGE · " + localePlugin->definition.locale.name : L"   LANGUAGE · English");
    RECT themeChip{x + 88, 202, 576, 228};
    FillRound(dc, themeChip, 9, themePlugin ? RGB(34, 47, 70) : RGB(29, 33, 41));
    Text(dc, themeText, RECT{themeChip.left + 10, themeChip.top, themeChip.right - 10, themeChip.bottom},
         themePlugin ? RGB(143, 187, 250) : RGB(106, 116, 132), fontSmall_);
    if (themePlugin) AddRegion(themeChip.left, themeChip.top, themeChip.right - themeChip.left, themeChip.bottom - themeChip.top,
                               Action::PluginsResetTheme);

    const struct HostButton { RECT rect; const wchar_t* label; Action action; UiIcon icon; } hostButtons[] = {
        {RECT{690, 167, 770, 207}, L"Reload", Action::PluginsReload, UiIcon::Replay},
        {RECT{778, 167, 858, 207}, L"Folder", Action::PluginsOpenFolder, UiIcon::Folder},
        {RECT{866, 167, 944, 207}, L"SDK", Action::PluginsOpenDocs, UiIcon::Arrow},
    };
    for (const auto& button : hostButtons) {
        FillRound(dc, button.rect, 10, Mix(RGB(29, 34, 43), RGB(44, 53, 68), HoverAmount(button.action)));
        StrokeRound(dc, button.rect, 10, RGB(47, 54, 67));
        DrawUiIcon(dc, button.icon, RECT{button.rect.left + 8, button.rect.top + 11, button.rect.left + 26, button.rect.top + 29},
                   RGB(126, 173, 242), 2);
        Text(dc, button.label, RECT{button.rect.left + 31, button.rect.top, button.rect.right - 5, button.rect.bottom},
             RGB(204, 211, 222), fontSmall_, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        AddRegion(button.rect.left, button.rect.top, button.rect.right - button.rect.left, button.rect.bottom - button.rect.top, button.action);
    }

    Text(dc, L"INSTALLED PLUGINS", RECT{x, 246, 560, 273}, RGB(106, 117, 134), fontSmall_);
    RECT browse{820, 243, 956, 271};
    FillRound(dc, browse, 9, Mix(RGB(25, 29, 37), RGB(38, 46, 59), HoverAmount(Action::PluginsBrowse)));
    Text(dc, L"All plugins…", browse, RGB(128, 171, 235), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddRegion(browse.left, browse.top, browse.right - browse.left, browse.bottom - browse.top, Action::PluginsBrowse);

    constexpr Action toggleActions[] = {Action::Plugin0Toggle, Action::Plugin1Toggle, Action::Plugin2Toggle};
    constexpr Action manageActions[] = {Action::Plugin0Manage, Action::Plugin1Manage, Action::Plugin2Manage};
    const auto& plugins = pluginSystem_.Plugins();
    for (std::size_t index = 0; index < plugins.size(); ++index) {
        const auto& plugin = plugins[index];
        const int y = 278 + static_cast<int>(index) * 106;
        RECT row{x, y, 956, y + 96};
        const COLORREF rowColor = !plugin.valid ? RGB(35, 23, 28) : plugin.enabled ? RGB(22, 31, 46) : RGB(18, 21, 27);
        const COLORREF border = !plugin.valid ? RGB(92, 47, 58) : plugin.enabled ? RGB(53, 88, 139) : RGB(36, 41, 51);
        FillRound(dc, row, 13, rowColor);
        StrokeRound(dc, row, 13, border);
        RECT pluginIcon{x + 14, y + 14, x + 66, y + 66};
        FillRound(dc, pluginIcon, 13, plugin.valid ? plugin.enabled ? RGB(45, 72, 119) : RGB(32, 38, 49) : RGB(65, 33, 40));
        DrawUiIcon(dc, UiIcon::Plugin, RECT{x + 29, y + 29, x + 51, y + 51},
                   plugin.valid ? plugin.enabled ? RGB(167, 202, 255) : RGB(91, 103, 122) : RGB(231, 133, 150), 2);
        const std::wstring version = plugin.definition.version.empty() ? L"" : L"  ·  v" + plugin.definition.version;
        Text(dc, plugin.DisplayName() + version, RECT{x + 80, y + 4, 748, y + 31},
             plugin.valid ? RGB(232, 236, 244) : RGB(239, 173, 184), fontTitle_,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, plugin.valid ? plugin.definition.description : plugin.StatusText(), RECT{x + 80, y + 31, 748, y + 64},
             plugin.valid ? RGB(111, 121, 138) : RGB(189, 108, 123), fontSmall_,
             DT_LEFT | DT_WORDBREAK | DT_WORD_ELLIPSIS);
        const std::wstring permissionText = plugin.valid ? L"Permissions · " + plugin.PermissionSummary() : L"Source blocked · fix and Reload";
        Text(dc, permissionText, RECT{x + 80, y + 66, 748, y + 91},
             plugin.valid ? RGB(82, 101, 130) : RGB(155, 82, 96), fontSmall_,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        const bool hasInlineActions = index < std::size(toggleActions);
        const Action manageAction = hasInlineActions ? manageActions[index] : Action::PluginsBrowse;
        if (hasInlineActions) {
            DrawToggle(dc, 782, y + 35, plugin.valid && plugin.enabled, HoverAmount(toggleActions[index]));
            AddRegion(772, y + 23, 66, 48, toggleActions[index]);
        } else {
            RECT state{768, y + 18, 838, y + 45};
            FillRound(dc, state, 8, plugin.enabled ? RGB(38, 68, 55) : RGB(29, 34, 43));
            Text(dc, plugin.enabled ? L"ENABLED" : L"OFF", state,
                 plugin.enabled ? RGB(131, 215, 166) : RGB(105, 115, 131), fontSmall_,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        RECT manage{846, y + 29, 936, y + 67};
        FillRound(dc, manage, 9, Mix(RGB(29, 34, 43), RGB(44, 53, 68), HoverAmount(manageAction)));
        Text(dc, plugin.valid ? (hasInlineActions ? L"Manage" : L"Open") : L"Error", manage,
             plugin.valid ? RGB(135, 178, 241) : RGB(224, 126, 143), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddRegion(manage.left, manage.top, manage.right - manage.left, manage.bottom - manage.top, manageAction);
    }
    if (plugins.empty()) {
        RECT empty{x, 278, 956, 518};
        FillRound(dc, empty, 14, RGB(18, 21, 27));
        StrokeRound(dc, empty, 14, RGB(36, 41, 51));
        DrawUiIcon(dc, UiIcon::Plugin, RECT{564, 322, 620, 378}, RGB(60, 70, 86), 2);
        Text(dc, L"Drop a .luna file into the Plugins folder", RECT{x, 390, 956, 426}, RGB(153, 161, 176), fontTitle_,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        Text(dc, L"Read the offline SDK, create a plugin, then press Reload. New permissions always require review.",
             RECT{x + 40, 429, 916, 466}, RGB(91, 101, 117), fontSmall_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    const int statusTop = plugins.empty() ? 534 : 278 + static_cast<int>(plugins.size()) * 106 + 8;
    RECT status{x, statusTop, 956, statusTop + 44};
    FillRound(dc, status, 11, pluginSystem_.ErrorCount() ? RGB(37, 25, 30) : RGB(19, 23, 30));
    StrokeRound(dc, status, 11, pluginSystem_.ErrorCount() ? RGB(78, 43, 51) : RGB(35, 42, 53));
    ui::FillEllipse(dc, RECT{x + 15, statusTop + 16, x + 27, statusTop + 28},
                    pluginSystem_.ErrorCount() ? RGB(235, 104, 121) : RGB(80, 199, 139));
    Text(dc, pluginStatus_, RECT{x + 38, statusTop, 932, statusTop + 44}, pluginSystem_.ErrorCount() ? RGB(215, 133, 146) : RGB(117, 131, 151),
         fontSmall_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void App::PaintSettings(HDC dc) {
    const int x = 228;
    Text(dc, L"Shortcuts and startup", RECT{x, 76, 940, 112}, RGB(244, 246, 251), fontHeadline_);
    Text(dc, L"Fullscreen-safe: Raw Input + keyboard hook + Windows hotkey. Click a shortcut to replace it.",
         RECT{x, 111, 956, 139}, RGB(137, 144, 158), fontNormal_);

    const struct HotkeyRow { int y; const wchar_t* title; const wchar_t* detail; const HotkeyBinding* binding; Action action; Action toggleAction; int slot; UiIcon icon; } rows[] = {
        {145, L"Active display", L"Foreground monitor screenshot", &settings_.activeDisplayHotkey, Action::RecordActiveHotkey, Action::ToggleActiveHotkey, 0, UiIcon::Monitor},
        {197, L"All displays", L"Stitched virtual desktop", &settings_.allDisplaysHotkey, Action::RecordAllHotkey, Action::ToggleAllHotkey, 1, UiIcon::AllDisplays},
        {249, L"Region editor", L"Selection and annotation", &settings_.regionHotkey, Action::RecordRegionHotkey, Action::ToggleRegionHotkey, 2, UiIcon::Region},
        {301, L"Active window", L"Frozen window picker", &settings_.activeWindowHotkey, Action::RecordWindowHotkey, Action::ToggleWindowHotkey, 3, UiIcon::Window},
        {353, L"Start / stop recording", L"Works over exclusive fullscreen", &settings_.recordingHotkey, Action::RecordRecordingHotkey, Action::ToggleRecordingHotkey, 4, UiIcon::Record},
        {405, L"Save instant replay", L"All monitors or choose a monitor bind", &settings_.replaySaveHotkey, Action::RecordReplayHotkey, Action::ToggleReplayHotkey, 5, UiIcon::Replay},
        {457, L"Quick Panel", L"Fullscreen side controls · default Alt + X", &settings_.quickPanelHotkey,
         Action::RecordQuickPanelHotkey, Action::ToggleQuickPanelHotkey, kQuickPanelHotkeySlot, UiIcon::AllDisplays},
    };
    for (const auto& row : rows) {
        RECT rect{x, row.y, 956, row.y + 46};
        FillRound(dc, rect, 11, Mix(RGB(20, 23, 29), RGB(29, 35, 44), HoverAmount(row.action)));
        StrokeRound(dc, rect, 11, recordingHotkeySlot_ == row.slot ? RGB(93, 149, 245) : RGB(39, 44, 54), recordingHotkeySlot_ == row.slot ? 2 : 1);
        DrawUiIcon(dc, row.icon, RECT{x + 17, row.y + 13, x + 37, row.y + 33}, RGB(127, 174, 246), 2);
        Text(dc, row.title, RECT{x + 48, row.y + 1, 590, row.y + 24}, RGB(236, 239, 245), fontTitle_);
        Text(dc, row.detail, RECT{x + 48, row.y + 21, 670, row.y + 44}, RGB(124, 132, 146), fontSmall_);
        const std::wstring hotkey = recordingHotkeySlot_ == row.slot ? L"Press keys · Esc disables" : FormatHotkey(row.binding->modifiers, row.binding->vk);
        RECT keyRect{674, row.y + 6, 864, row.y + 40};
        FillRound(dc, keyRect, 9, RGB(31, 36, 45));
        Text(dc, hotkey, RECT{keyRect.left + 10, keyRect.top, keyRect.right - 10, keyRect.bottom}, RGB(113, 169, 255), fontNormal_, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        AddRegion(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, row.action);
        DrawToggle(dc, 884, row.y + 11, row.binding->vk != 0, HoverAmount(row.toggleAction));
        AddRegion(876, row.y + 4, 62, 38, row.toggleAction);
    }

    Text(dc, L"APP BEHAVIOR", RECT{x, 510, 600, 532}, RGB(106, 117, 134), fontSmall_);
    const struct BehaviorCard {
        RECT rect;
        const wchar_t* title;
        const wchar_t* detail;
        Action action;
        UiIcon icon;
        bool toggle;
        bool enabled;
    } behaviorCards[] = {
        {RECT{228, 536, 582, 598}, L"Start with Windows", L"Register Luna Capture at user logon",
         Action::ToggleAutostart, UiIcon::Power, true, settings_.startWithWindows},
        {RECT{592, 536, 956, 598}, L"Launch directly to tray", L"Keep the main window hidden on startup",
         Action::ToggleStartHiddenToTray, UiIcon::Minimize, true, settings_.startHiddenToTray},
        {RECT{228, 606, 582, 668}, L"Notification duration", L"Fullscreen and desktop overlay display time",
         Action::CycleOverlayDuration, UiIcon::Bell, false, false},
    };
    for (const auto& card : behaviorCards) {
        FillRound(dc, card.rect, 13, Mix(RGB(20, 23, 29), RGB(29, 35, 44), HoverAmount(card.action)));
        StrokeRound(dc, card.rect, 13, card.toggle && card.enabled ? RGB(48, 75, 116) : RGB(39, 44, 54));
        DrawUiIcon(dc, card.icon,
                   RECT{card.rect.left + 15, card.rect.top + 20, card.rect.left + 37, card.rect.top + 42},
                   card.toggle && card.enabled ? RGB(126, 174, 245) : RGB(101, 120, 148), 2);
        const int textRight = card.toggle ? card.rect.right - 78 : card.rect.right - 112;
        Text(dc, card.title, RECT{card.rect.left + 48, card.rect.top + 3, textRight, card.rect.top + 31},
             RGB(235, 238, 244), fontTitle_, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, card.detail, RECT{card.rect.left + 48, card.rect.top + 29, textRight, card.rect.bottom - 3},
             RGB(112, 122, 138), fontSmall_, DT_LEFT | DT_WORDBREAK | DT_WORD_ELLIPSIS);
        if (card.toggle) {
            DrawToggle(dc, card.rect.right - 62, card.rect.top + 19, card.enabled, HoverAmount(card.action));
        } else {
            RECT value{card.rect.right - 102, card.rect.top + 14, card.rect.right - 14, card.rect.bottom - 14};
            FillRound(dc, value, 9, RGB(30, 36, 46));
            Text(dc, std::to_wstring(settings_.overlayDurationMs) + L" ms", value, RGB(121, 173, 250), fontSmall_,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        AddRegion(card.rect.left, card.rect.top, card.rect.right - card.rect.left, card.rect.bottom - card.rect.top, card.action);
    }

    RECT reset{592, 606, 956, 668};
    FillRound(dc, reset, 13, Mix(RGB(29, 24, 27), RGB(54, 34, 39), HoverAmount(Action::ResetDefaults)));
    StrokeRound(dc, reset, 13, RGB(70, 47, 53));
    DrawUiIcon(dc, UiIcon::Undo, RECT{608, 626, 630, 648}, RGB(238, 174, 187), 2);
    Text(dc, L"Reset defaults", RECT{642, 609, 938, 637}, RGB(244, 214, 219), fontTitle_,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    Text(dc, L"Restore shortcuts and every capture option", RECT{642, 635, 938, 665}, RGB(155, 126, 133), fontSmall_,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    AddRegion(reset.left, reset.top, reset.right - reset.left, reset.bottom - reset.top, Action::ResetDefaults);

    Text(dc, L"UPDATES", RECT{x, 682, 600, 704}, RGB(106, 117, 134), fontSmall_);
    const bool updateReady = updateUiState_ == UpdateUiState::UpdateAvailable;
    const Action updateAction = updateReady ? Action::OpenUpdatePage : Action::CheckForUpdates;
    RECT update{228, 708, 956, 770};
    if (updateReady) {
        FillRound(dc, update, 13, Mix(RGB(35, 30, 18), RGB(52, 43, 23), HoverAmount(updateAction)));
        StrokeRound(dc, update, 13, RGB(122, 96, 42));
    } else {
        FillRound(dc, update, 13, Mix(RGB(20, 23, 29), RGB(29, 35, 44), HoverAmount(updateAction)));
        StrokeRound(dc, update, 13, RGB(39, 44, 54));
    }
    DrawUiIcon(dc, updateReady ? UiIcon::Warning : UiIcon::Redo,
               RECT{update.left + 15, update.top + 20, update.left + 37, update.top + 42},
               updateReady ? RGB(246, 180, 72) : RGB(126, 174, 245), 2);
    const std::wstring updateTitle = updateReady
        ? L"Update " + updateLatestVersion_ + L" available"
        : L"Check for updates";
    const bool updateChecking = updateUiState_ == UpdateUiState::Checking;
    const std::wstring updateDetail =
        updateReady ? L"Click to open the GitHub download page" :
        updateChecking ? L"Contacting GitHub…" :
        updateUiState_ == UpdateUiState::UpToDate ? L"Luna Capture " + std::wstring(kAppVersion) + L" is the latest release" :
        updateUiState_ == UpdateUiState::Failed ? L"Last check failed · click to retry" :
        L"You're on Luna Capture " + std::wstring(kAppVersion);
    const int textRight = updateReady ? update.right - 254 : update.right - 132;
    Text(dc, updateTitle, RECT{update.left + 48, update.top + 3, textRight, update.top + 31},
         updateReady ? RGB(248, 205, 116) : RGB(235, 238, 244), fontTitle_,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    Text(dc, updateDetail, RECT{update.left + 48, update.top + 29, textRight, update.bottom - 3},
         updateReady ? RGB(186, 154, 92) : RGB(112, 122, 138), fontSmall_,
         DT_LEFT | DT_WORDBREAK | DT_WORD_ELLIPSIS);
    // The card itself is one big button; the chips below are drawn (and hit-tested) on top of
    // it, so re-checking stays possible even while the update banner is up.
    AddRegion(update.left, update.top, update.right - update.left, update.bottom - update.top, updateAction);
    RECT updateChip{update.right - 122, update.top + 14, update.right - 14, update.bottom - 14};
    FillRound(dc, updateChip, 9, updateReady ? RGB(64, 52, 26) : RGB(30, 36, 46));
    Text(dc, updateReady ? L"Download" :
             updateChecking ? L"Checking…" :
             updateUiState_ == UpdateUiState::Failed ? L"Retry" : L"Check now",
         updateChip, updateReady ? RGB(246, 199, 107) : RGB(121, 173, 250), fontSmall_,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddRegion(updateChip.left, updateChip.top, updateChip.right - updateChip.left,
              updateChip.bottom - updateChip.top, updateAction);
    if (updateReady) {
        // A found update must not block re-checking: keep a second, always-live "Check again"
        // chip so the user can re-query GitHub whenever they want.
        RECT recheckChip{update.right - 244, update.top + 14, update.right - 132, update.bottom - 14};
        FillRound(dc, recheckChip, 9, Mix(RGB(30, 36, 46), RGB(41, 49, 62), HoverAmount(Action::CheckForUpdates)));
        Text(dc, updateChecking ? L"Checking…" : L"Check again", recheckChip, RGB(121, 173, 250), fontSmall_,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddRegion(recheckChip.left, recheckChip.top, recheckChip.right - recheckChip.left,
                  recheckChip.bottom - recheckChip.top, Action::CheckForUpdates);
    }
}

int App::PageContentHeight(Page page) const {
    switch (page) {
        case Page::Photos:
        case Page::Videos: {
            if (mediaDetailOpen_) return kWindowHeight;
            const auto& library = page == Page::Videos ? videoLibrary_ : photoLibrary_;
            if (library.empty()) return 590;
            constexpr int kTileCols = 4;
            constexpr int kTileHeight = 118 + 54;
            constexpr int kTileGap = 12;
            const int rows = (static_cast<int>(library.size()) + kTileCols - 1) / kTileCols;
            return 250 + rows * (kTileHeight + kTileGap) + 18;
        }
        case Page::Plugins: {
            const std::size_t count = pluginSystem_.Plugins().size();
            if (count == 0) return 594;
            const int statusTop = 278 + static_cast<int>(count) * 106 + 8;
            return statusTop + 58;
        }
        case Page::Encoder:
            // Rows size themselves from measured text; PaintEncoder publishes the
            // real bottom so scrolling matches whatever the last paint produced.
            return encoderContentHeight_;
        case Page::Settings:
            return 800;
        default:
            return kWindowHeight;
    }
}

int App::PageMaximumScroll(Page page) const {
    return (std::max)(0, PageContentHeight(page) - kWindowHeight);
}

void App::ScrollCurrentPage(int delta) {
    const std::size_t index = static_cast<std::size_t>(page_);
    const int maximum = PageMaximumScroll(page_);
    const int next = (std::clamp)(pageScrollY_[index] + delta, 0, maximum);
    if (next == pageScrollY_[index]) return;
    pageScrollY_[index] = next;
    hoveredAction_ = Action::None;
    hoveredActionData_ = -1;
    InvalidateUi();
}

void App::PaintPageScrollbar(HDC dc) {
    pageScrollTrack_ = RECT{};
    pageScrollThumb_ = RECT{};
    const int maximum = PageMaximumScroll(page_);
    if (maximum <= 0) return;

    const std::size_t pageIndex = static_cast<std::size_t>(page_);
    pageScrollY_[pageIndex] = (std::clamp)(pageScrollY_[pageIndex], 0, maximum);
    pageScrollTrack_ = RECT{978, 78, 986, 628};
    const int trackHeight = pageScrollTrack_.bottom - pageScrollTrack_.top;
    const int visibleHeight = kWindowHeight - kTopBarHeight;
    const int logicalHeight = (std::max)(visibleHeight, PageContentHeight(page_) - kTopBarHeight);
    const int thumbHeight = (std::max)(42, trackHeight * visibleHeight / logicalHeight);
    const int travel = trackHeight - thumbHeight;
    const int thumbTop = pageScrollTrack_.top + (travel * pageScrollY_[pageIndex] + maximum / 2) / maximum;
    pageScrollThumb_ = RECT{977, thumbTop, 987, thumbTop + thumbHeight};

    FillRound(dc, pageScrollTrack_, 4, RGB(24, 28, 35));
    FillRound(dc, pageScrollThumb_, 5, pageScrollDragging_ ? RGB(104, 157, 238) : RGB(66, 82, 107));
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

    FillThemeRole(backBufferDc_, client, ui::ThemeRole::Background);
    ui::MakeLayeredSurfaceOpaque(backBufferPixels_, client.right, client.bottom);
    ui::PaintThemeBackdrop(backBufferDc_, client, ui::ThemeSurface::Main, GetTickCount64());
    hitRegions_.clear();
    PaintSidebar(backBufferDc_);
    PaintTopBar(backBufferDc_);
    const std::size_t pageIndex = static_cast<std::size_t>(page_);
    pageScrollY_[pageIndex] = (std::clamp)(pageScrollY_[pageIndex], 0, PageMaximumScroll(page_));
    const int savedDc = SaveDC(backBufferDc_);
    IntersectClipRect(backBufferDc_, kSidebarWidth, kTopBarHeight, kWindowWidth, kWindowHeight);
    SetViewportOrgEx(backBufferDc_, 0, -pageScrollY_[pageIndex], nullptr);
    paintingPageContent_ = true;
    activePagePaintScrollY_ = pageScrollY_[pageIndex];
    if (page_ == Page::Home) PaintHome(backBufferDc_);
    else if (page_ == Page::Capture) PaintCapture(backBufferDc_);
    else if (page_ == Page::Photos) { if (!mediaDetailOpen_) PaintMediaLibrary(backBufferDc_, false); }
    else if (page_ == Page::Videos) { if (!mediaDetailOpen_) PaintMediaLibrary(backBufferDc_, true); }
    else if (page_ == Page::Record) PaintRecord(backBufferDc_);
    else if (page_ == Page::Replay) PaintReplay(backBufferDc_);
    else if (page_ == Page::Encoder) PaintEncoder(backBufferDc_);
    else if (page_ == Page::Audio) PaintAudio(backBufferDc_);
    else if (page_ == Page::Plugins) PaintPlugins(backBufferDc_);
    else PaintSettings(backBufferDc_);
    paintingPageContent_ = false;
    activePagePaintScrollY_ = 0;
    RestoreDC(backBufferDc_, savedDc);
    if (mediaDetailOpen_) PaintMediaDetail(backBufferDc_);
    else PaintPageScrollbar(backBufferDc_);
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
        case WM_TIMER:
            if (wParam == kAnimationTimer) { TickAnimations(); return 0; }
            if (wParam == kUpdateRecheckTimer) {
                // Skip when a manual check is in flight or an update is already known — the
                // banner is up and re-posting the same result adds nothing.
                if (updateUiState_ != UpdateUiState::Checking && updateUiState_ != UpdateUiState::UpdateAvailable) {
                    StartUpdateCheck(hwnd_, WM_APP_UPDATE_CHECK);
                }
                return 0;
            }
            break;
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &point);
            if (point.x >= kSidebarWidth && point.y >= kTopBarHeight && point.x < kWindowWidth && point.y < kWindowHeight) {
                const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
                int pixelDelta = -(wheelDelta * 58) / WHEEL_DELTA;
                if (pixelDelta == 0 && wheelDelta != 0) pixelDelta = wheelDelta > 0 ? -1 : 1;
                if (pixelDelta != 0) ScrollCurrentPage(pixelDelta);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (pageScrollDragging_) {
                const int maximum = PageMaximumScroll(page_);
                const int thumbHeight = pageScrollThumb_.bottom - pageScrollThumb_.top;
                const int travel = (pageScrollTrack_.bottom - pageScrollTrack_.top) - thumbHeight;
                if (maximum > 0 && travel > 0) {
                    const int top = (std::clamp)(point.y - pageScrollDragOffset_, pageScrollTrack_.top,
                                                 pageScrollTrack_.bottom - thumbHeight);
                    const int next = ((top - pageScrollTrack_.top) * maximum + travel / 2) / travel;
                    const std::size_t index = static_cast<std::size_t>(page_);
                    if (pageScrollY_[index] != next) {
                        pageScrollY_[index] = next;
                        InvalidateUi();
                    }
                }
                return 0;
            }
            if (dragging_) {
                POINT cursor{}; GetCursorPos(&cursor);
                SetWindowPos(hwnd_, nullptr, cursor.x - dragOffset_.x, cursor.y - dragOffset_.y, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            if (mediaSeekDragging_) { SeekMediaDetailFromPoint(point.x); InvalidateUi(); return 0; }
            if (mediaVolumeDragging_) { VolumeMediaDetailFromPoint(point.x); InvalidateUi(); return 0; }
            const HitRegion* region = HitTestRegion(point);
            const Action next = region ? region->action : Action::None;
            const int nextData = region ? region->data : -1;
            if (next != hoveredAction_ || nextData != hoveredActionData_) {
                hoveredAction_ = next;
                hoveredActionData_ = nextData;
                InvalidateUi();
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0}; TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hoveredAction_ = Action::None; hoveredActionData_ = -1; InvalidateUi(); return 0;
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (PageMaximumScroll(page_) > 0) {
                RECT thumbHit = pageScrollThumb_;
                InflateRect(&thumbHit, 8, 2);
                RECT trackHit = pageScrollTrack_;
                InflateRect(&trackHit, 8, 0);
                if (PtInRect(&thumbHit, point) || PtInRect(&trackHit, point)) {
                    const int thumbHeight = pageScrollThumb_.bottom - pageScrollThumb_.top;
                    if (PtInRect(&thumbHit, point)) {
                        pageScrollDragOffset_ = point.y - pageScrollThumb_.top;
                    } else {
                        pageScrollDragOffset_ = thumbHeight / 2;
                        const int maximum = PageMaximumScroll(page_);
                        const int travel = (pageScrollTrack_.bottom - pageScrollTrack_.top) - thumbHeight;
                        const int top = (std::clamp)(point.y - pageScrollDragOffset_, pageScrollTrack_.top,
                                                     pageScrollTrack_.bottom - thumbHeight);
                        pageScrollY_[static_cast<std::size_t>(page_)] = travel > 0
                            ? ((top - pageScrollTrack_.top) * maximum + travel / 2) / travel : 0;
                    }
                    pageScrollDragging_ = true;
                    SetCapture(hwnd_);
                    InvalidateUi();
                    return 0;
                }
            }
            if (mediaDetailOpen_ && mediaDetailIsVideo_) {
                RECT seekHit = mediaProgressRect_; InflateRect(&seekHit, 0, 9);
                RECT volumeHit = mediaVolumeRect_; InflateRect(&volumeHit, 4, 10);
                if (mediaProgressRect_.right > mediaProgressRect_.left && PtInRect(&seekHit, point)) {
                    mediaSeekDragging_ = true;
                    SeekMediaDetailFromPoint(point.x);
                    SetCapture(hwnd_);
                    InvalidateUi();
                    return 0;
                }
                if (mediaVolumeRect_.right > mediaVolumeRect_.left && PtInRect(&volumeHit, point)) {
                    mediaVolumeDragging_ = true;
                    VolumeMediaDetailFromPoint(point.x);
                    SetCapture(hwnd_);
                    InvalidateUi();
                    return 0;
                }
            }
            const HitRegion* pressedRegion = HitTestRegion(point);
            pressedAction_ = pressedRegion ? pressedRegion->action : Action::None;
            pressedActionData_ = pressedRegion ? pressedRegion->data : -1;
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
            if (pageScrollDragging_) {
                pageScrollDragging_ = false;
                ReleaseCapture();
                InvalidateUi();
                return 0;
            }
            if (dragging_) { dragging_ = false; ReleaseCapture(); return 0; }
            if (mediaSeekDragging_ || mediaVolumeDragging_) {
                mediaSeekDragging_ = false;
                mediaVolumeDragging_ = false;
                ReleaseCapture();
                InvalidateUi();
                return 0;
            }
            const HitRegion* releasedRegion = HitTestRegion(point);
            const Action released = releasedRegion ? releasedRegion->action : Action::None;
            const int releasedData = releasedRegion ? releasedRegion->data : -1;
            const Action execute = pressedAction_ == released && pressedActionData_ == releasedData ? released : Action::None;
            pressedAction_ = Action::None;
            pressedActionData_ = -1;
            ReleaseCapture();
            if (execute != Action::None) ExecuteAction(execute, releasedData);
            InvalidateUi(); return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (recordingHotkeySlot_ >= 0 && globalKeyboardHook_ == nullptr && hotkeyRecordingHook_ == nullptr) {
                if (wParam == VK_ESCAPE) DisableHotkeySlot(recordingHotkeySlot_);
                else FinishHotkeyRecording(static_cast<UINT>(wParam), CurrentModifiers());
                return 0;
            }
            if (message == WM_KEYDOWN && mediaDetailOpen_) {
                switch (wParam) {
                    case VK_ESCAPE:
                        if (mediaDetailFullscreen_) SetMediaDetailFullscreen(false);
                        else CloseMediaDetail();
                        return 0;
                    case VK_LEFT: StepMediaDetail(-1); return 0;
                    case VK_RIGHT: StepMediaDetail(1); return 0;
                    case VK_DELETE: DeleteMediaDetailItem(); return 0;
                    case VK_SPACE:
                        if (mediaDetailIsVideo_) { mediaViewer_.TogglePlayback(); InvalidateUi(); }
                        return 0;
                    case 'F':
                        SetMediaDetailFullscreen(!mediaDetailFullscreen_);
                        return 0;
                    default:
                        break;
                }
            }
            if (message == WM_KEYDOWN && PageMaximumScroll(page_) > 0) {
                if (wParam == VK_PRIOR) { ScrollCurrentPage(-(kWindowHeight - kTopBarHeight - 60)); return 0; }
                if (wParam == VK_NEXT) { ScrollCurrentPage(kWindowHeight - kTopBarHeight - 60); return 0; }
                if (wParam == VK_HOME) {
                    pageScrollY_[static_cast<std::size_t>(page_)] = 0;
                    InvalidateUi();
                    return 0;
                }
                if (wParam == VK_END) {
                    pageScrollY_[static_cast<std::size_t>(page_)] = PageMaximumScroll(page_);
                    InvalidateUi();
                    return 0;
                }
            }
            break;
        case WM_CAPTURECHANGED:
            if (pageScrollDragging_) { pageScrollDragging_ = false; InvalidateUi(); }
            if (dragging_) dragging_ = false;
            if (mediaSeekDragging_ || mediaVolumeDragging_) { mediaSeekDragging_ = mediaVolumeDragging_ = false; InvalidateUi(); }
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
                if (static_cast<UINT>(wParam) == VK_ESCAPE) DisableHotkeySlot(recordingHotkeySlot_);
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
            else if (wParam == LUNASHOT_HOTKEY_QUICK_PANEL) ExecuteGlobalHotkey(kQuickPanelHotkeySlot, L"RegisterHotKey");
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
        case WM_APP_THUMBNAIL_READY:
            HandleThumbnailReady(std::unique_ptr<ThumbnailResult>(reinterpret_cast<ThumbnailResult*>(lParam)));
            return 0;
        case WM_APP_MEDIA_STATE_CHANGED:
            if (mediaDetailOpen_) InvalidateUi();
            return 0;
        case WM_APP_MEDIA_EXIT_FULLSCREEN:
            SetMediaDetailFullscreen(false);
            return 0;
        case WM_COPYDATA: {
            // A second launch (Explorer opening a .luna) forwards the path here
            // instead of starting a rival instance.
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (!data) return TRUE;
            if (data->dwData == kLunaReloadCopyDataId) {
                // The opening process already installed and reported it; just pick
                // the new file up without stacking a second dialog on top.
                ReloadPlugins(true);
                return TRUE;
            }
            if (data->dwData == kLunaImportCopyDataId && data->lpData && data->cbData >= sizeof(wchar_t)) {
                const auto* raw = static_cast<const wchar_t*>(data->lpData);
                const std::size_t maximum = data->cbData / sizeof(wchar_t);
                std::wstring path(raw, wcsnlen(raw, maximum));
                if (!path.empty()) ImportLunaFile(path);
            }
            return TRUE;
        }
        case WM_APP_UPDATE_CHECK:
            HandleUpdateCheckResult(std::unique_ptr<UpdateCheckResult>(reinterpret_cast<UpdateCheckResult*>(lParam)));
            return 0;
        case WM_APP_TRAY_COMMAND:
            HandleTrayCommand(static_cast<TrayCommand>(wParam), static_cast<std::size_t>(lParam));
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
