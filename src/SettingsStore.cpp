#include "SettingsStore.hpp"

#include "WinUtil.hpp"

#include <Windows.h>

#include <algorithm>
#include <filesystem>

namespace {
std::wstring ReadString(const std::wstring& path, const wchar_t* key, const std::wstring& fallback) {
    std::wstring buffer(32768, L'\0');
    const DWORD count = GetPrivateProfileStringW(L"LunaCapture", key, fallback.c_str(), buffer.data(),
                                                  static_cast<DWORD>(buffer.size()), path.c_str());
    buffer.resize(count);
    return buffer;
}

int ReadInt(const std::wstring& path, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(L"LunaCapture", key, fallback, path.c_str()));
}

bool WriteString(const std::wstring& path, const wchar_t* key, const std::wstring& value) {
    return WritePrivateProfileStringW(L"LunaCapture", key, value.c_str(), path.c_str()) != FALSE;
}

bool WriteInt(const std::wstring& path, const wchar_t* key, int value) {
    return WriteString(path, key, std::to_wstring(value));
}

HotkeyBinding ReadHotkey(const std::wstring& path, const wchar_t* vkKey, const wchar_t* modKey,
                         UINT defaultVk, UINT defaultModifiers) {
    HotkeyBinding binding;
    binding.vk = static_cast<UINT>(ReadInt(path, vkKey, static_cast<int>(defaultVk)));
    binding.modifiers = static_cast<UINT>(ReadInt(path, modKey, static_cast<int>(defaultModifiers))) | MOD_NOREPEAT;
    return binding;
}
}

SettingsStore::SettingsStore() {
    const auto directory = GetLocalAppDataDirectory();
    EnsureDirectoryExists(directory);
    path_ = (std::filesystem::path(directory) / L"settings.ini").wstring();
}

AppSettings SettingsStore::Load() const {
    AppSettings settings;
    settings.saveDirectory = ReadString(path_, L"SaveDirectory", GetDefaultScreenshotDirectory());
    settings.imageFormat = ReadInt(path_, L"ImageFormat", 0) == 1 ? ImageFormat::Jpeg : ImageFormat::Png;
    settings.outputWidth = ReadInt(path_, L"OutputWidth", 0);
    settings.outputHeight = ReadInt(path_, L"OutputHeight", 0);
    settings.jpegQuality = (std::max)(50, (std::min)(100, ReadInt(path_, L"JpegQuality", 95)));
    settings.notifications = ReadInt(path_, L"Notifications", 1) != 0;
    settings.sound = ReadInt(path_, L"Sound", 1) != 0;
    settings.startWithWindows = ReadInt(path_, L"StartWithWindows", 0) != 0;
    settings.copyRegionToClipboard = ReadInt(path_, L"CopyRegionToClipboard", 0) != 0;
    settings.overlayDurationMs = (std::max)(650, (std::min)(3000, ReadInt(path_, L"OverlayDurationMs", 1200)));
    settings.activeDisplayHotkey = ReadHotkey(path_, L"ActiveVk", L"ActiveModifiers", VK_SNAPSHOT, MOD_NOREPEAT);
    settings.allDisplaysHotkey = ReadHotkey(path_, L"AllVk", L"AllModifiers", VK_SNAPSHOT,
                                            MOD_CONTROL | MOD_NOREPEAT);
    settings.regionHotkey = ReadHotkey(path_, L"RegionVk", L"RegionModifiers", VK_SNAPSHOT,
                                       MOD_SHIFT | MOD_NOREPEAT);
    settings.activeWindowHotkey = ReadHotkey(path_, L"WindowVk", L"WindowModifiers", VK_SNAPSHOT,
                                             MOD_ALT | MOD_NOREPEAT);
    EnsureDirectoryExists(settings.saveDirectory);
    return settings;
}

bool SettingsStore::Save(const AppSettings& settings) const {
    EnsureDirectoryExists(std::filesystem::path(path_).parent_path().wstring());
    bool ok = true;
    ok &= WriteString(path_, L"SaveDirectory", settings.saveDirectory);
    ok &= WriteInt(path_, L"ImageFormat", settings.imageFormat == ImageFormat::Jpeg ? 1 : 0);
    ok &= WriteInt(path_, L"OutputWidth", settings.outputWidth);
    ok &= WriteInt(path_, L"OutputHeight", settings.outputHeight);
    ok &= WriteInt(path_, L"JpegQuality", settings.jpegQuality);
    ok &= WriteInt(path_, L"Notifications", settings.notifications ? 1 : 0);
    ok &= WriteInt(path_, L"Sound", settings.sound ? 1 : 0);
    ok &= WriteInt(path_, L"StartWithWindows", settings.startWithWindows ? 1 : 0);
    ok &= WriteInt(path_, L"CopyRegionToClipboard", settings.copyRegionToClipboard ? 1 : 0);
    ok &= WriteInt(path_, L"OverlayDurationMs", settings.overlayDurationMs);
    ok &= WriteInt(path_, L"ActiveVk", static_cast<int>(settings.activeDisplayHotkey.vk));
    ok &= WriteInt(path_, L"ActiveModifiers", static_cast<int>(settings.activeDisplayHotkey.modifiers));
    ok &= WriteInt(path_, L"AllVk", static_cast<int>(settings.allDisplaysHotkey.vk));
    ok &= WriteInt(path_, L"AllModifiers", static_cast<int>(settings.allDisplaysHotkey.modifiers));
    ok &= WriteInt(path_, L"RegionVk", static_cast<int>(settings.regionHotkey.vk));
    ok &= WriteInt(path_, L"RegionModifiers", static_cast<int>(settings.regionHotkey.modifiers));
    ok &= WriteInt(path_, L"WindowVk", static_cast<int>(settings.activeWindowHotkey.vk));
    ok &= WriteInt(path_, L"WindowModifiers", static_cast<int>(settings.activeWindowHotkey.modifiers));
    return ok;
}

bool SettingsStore::ApplyAutostart(bool enabled) const {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const wchar_t* valueName = L"LunaCapture";
    // Remove any autostart entry left by the previous "LunaShot" name so the app
    // never launches twice on sign-in.
    RegDeleteValueW(key, L"LunaShot");
    LSTATUS status = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring command = L"\"" + GetExecutablePath() + L"\" --background";
        status = RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, valueName);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}
