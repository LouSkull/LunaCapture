#include "WinUtil.hpp"
#include "CoreUtils.hpp"

#include <ShlObj.h>
#include <ShObjIdl.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iterator>
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

void OpenFolderInExplorer(const std::wstring& path) {
    EnsureDirectoryExists(path);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring FormatHotkey(UINT modifiers, UINT vk) {
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
