#include "SettingsStore.hpp"

#include "CoreUtils.hpp"
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
    settings.overlayDurationMs = (std::max)(250, (std::min)(10000, ReadInt(path_, L"OverlayDurationMs", 1200)));
    settings.notificationSoundPath = ReadString(path_, L"NotificationSoundPath", L"");
    settings.recordingDirectory = ReadString(path_, L"RecordingDirectory", GetDefaultRecordingDirectory());
    settings.recordingFps = (std::max)(1, (std::min)(240, ReadInt(path_, L"RecordingFps", 60)));
    settings.recordingBitrateMbps = (std::max)(1, (std::min)(200, ReadInt(path_, L"RecordingBitrateMbps", 16)));
    settings.videoCodec = static_cast<VideoCodec>((std::max)(0, (std::min)(2, ReadInt(path_, L"VideoCodec", 0))));
    const int height = ReadInt(path_, L"RecordingHeight", 0);
    settings.recordingHeight = (std::max)(0, (std::min)(4320, height));
    settings.recordingMonitorIndex = (std::max)(0, ReadInt(path_, L"RecordingMonitorIndex", 0));
    settings.recordingAutoMonitor = ReadInt(path_, L"RecordingAutoMonitor", 1) != 0;
    settings.replaySeconds = (std::max)(5, (std::min)(3600, ReadInt(path_, L"ReplaySeconds", 30)));
    settings.replayMonitorMask = static_cast<std::uint32_t>(ReadInt(path_, L"ReplayMonitorMask", 0)) & 0xFFu;
    settings.recordSystemAudio = ReadInt(path_, L"RecordSystemAudio", 1) != 0;
    settings.systemAudioVolumePercent = (std::max)(0, (std::min)(200, ReadInt(path_, L"SystemAudioVolume", 100)));
    settings.recordApplicationAudio = ReadInt(path_, L"RecordApplicationAudio", 0) != 0;
    settings.applicationAudioVolumePercent = (std::max)(0, (std::min)(200, ReadInt(path_, L"ApplicationAudioVolume", 100)));
    settings.recordingApplicationName = ReadString(path_, L"RecordingApplicationName", L"");
    settings.audioBitrateKbps = NormalizeAacBitrateKbps(ReadInt(path_, L"AudioBitrateKbps", 192));
    for (int i = 0; i < 4; ++i) {
        const std::wstring prefix = L"Microphone" + std::to_wstring(i);
        const std::wstring id = ReadString(path_, (prefix + L"Id").c_str(), L"");
        if (id.empty()) continue;
        AudioInputSetting input;
        input.deviceId = id;
        input.name = ReadString(path_, (prefix + L"Name").c_str(), L"Microphone");
        input.volumePercent = (std::max)(0, (std::min)(200, ReadInt(path_, (prefix + L"Volume").c_str(), 100)));
        input.enabled = ReadInt(path_, (prefix + L"Enabled").c_str(), 1) != 0;
        settings.microphoneInputs.push_back(std::move(input));
    }
    settings.recordCursor = ReadInt(path_, L"RecordCursor", 1) != 0;
    settings.previewEnabled = ReadInt(path_, L"PreviewEnabled", 1) != 0;
    settings.previewFps = (std::max)(1, (std::min)(60, ReadInt(path_, L"PreviewFps", 30)));
    if (ReadInt(path_, L"PreviewPipelineVersion", 0) < 2 && settings.previewFps == 8) {
        settings.previewFps = 30;
    }
    settings.activeDisplayHotkey = ReadHotkey(path_, L"ActiveVk", L"ActiveModifiers", VK_SNAPSHOT, MOD_NOREPEAT);
    settings.allDisplaysHotkey = ReadHotkey(path_, L"AllVk", L"AllModifiers", VK_SNAPSHOT,
                                            MOD_CONTROL | MOD_NOREPEAT);
    settings.regionHotkey = ReadHotkey(path_, L"RegionVk", L"RegionModifiers", VK_SNAPSHOT,
                                       MOD_SHIFT | MOD_NOREPEAT);
    settings.activeWindowHotkey = ReadHotkey(path_, L"WindowVk", L"WindowModifiers", VK_SNAPSHOT,
                                              MOD_ALT | MOD_NOREPEAT);
    settings.recordingHotkey = ReadHotkey(path_, L"RecordingVk", L"RecordingModifiers", VK_F9, MOD_NOREPEAT);
    settings.replaySaveHotkey = ReadHotkey(path_, L"ReplaySaveVk", L"ReplaySaveModifiers", VK_F10, MOD_NOREPEAT);
    for (std::size_t index = 0; index < settings.replayMonitorHotkeys.size(); ++index) {
        const std::wstring prefix = L"ReplayMonitor" + std::to_wstring(index);
        settings.replayMonitorHotkeys[index] = ReadHotkey(path_, (prefix + L"Vk").c_str(),
            (prefix + L"Modifiers").c_str(), 0, MOD_NOREPEAT);
    }
    EnsureDirectoryExists(settings.saveDirectory);
    EnsureDirectoryExists(settings.recordingDirectory);
    EnsureDirectoryExists(GetSoundLibraryDirectory());
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
    ok &= WriteString(path_, L"NotificationSoundPath", settings.notificationSoundPath);
    ok &= WriteString(path_, L"RecordingDirectory", settings.recordingDirectory);
    ok &= WriteInt(path_, L"RecordingFps", settings.recordingFps);
    ok &= WriteInt(path_, L"RecordingBitrateMbps", settings.recordingBitrateMbps);
    ok &= WriteInt(path_, L"VideoCodec", static_cast<int>(settings.videoCodec));
    ok &= WriteInt(path_, L"RecordingHeight", settings.recordingHeight);
    ok &= WriteInt(path_, L"RecordingMonitorIndex", settings.recordingMonitorIndex);
    ok &= WriteInt(path_, L"RecordingAutoMonitor", settings.recordingAutoMonitor ? 1 : 0);
    ok &= WriteInt(path_, L"ReplaySeconds", settings.replaySeconds);
    ok &= WriteInt(path_, L"ReplayMonitorMask", static_cast<int>(settings.replayMonitorMask));
    ok &= WriteInt(path_, L"RecordSystemAudio", settings.recordSystemAudio ? 1 : 0);
    ok &= WriteInt(path_, L"SystemAudioVolume", settings.systemAudioVolumePercent);
    ok &= WriteInt(path_, L"RecordApplicationAudio", settings.recordApplicationAudio ? 1 : 0);
    ok &= WriteInt(path_, L"ApplicationAudioVolume", settings.applicationAudioVolumePercent);
    ok &= WriteString(path_, L"RecordingApplicationName", settings.recordingApplicationName);
    ok &= WriteInt(path_, L"AudioBitrateKbps", settings.audioBitrateKbps);
    for (int i = 0; i < 4; ++i) {
        const std::wstring prefix = L"Microphone" + std::to_wstring(i);
        const AudioInputSetting* input = i < static_cast<int>(settings.microphoneInputs.size())
            ? &settings.microphoneInputs[static_cast<std::size_t>(i)] : nullptr;
        ok &= WriteString(path_, (prefix + L"Id").c_str(), input ? input->deviceId : L"");
        ok &= WriteString(path_, (prefix + L"Name").c_str(), input ? input->name : L"");
        ok &= WriteInt(path_, (prefix + L"Volume").c_str(), input ? input->volumePercent : 100);
        ok &= WriteInt(path_, (prefix + L"Enabled").c_str(), input && input->enabled ? 1 : 0);
    }
    ok &= WriteInt(path_, L"RecordCursor", settings.recordCursor ? 1 : 0);
    ok &= WriteInt(path_, L"PreviewEnabled", settings.previewEnabled ? 1 : 0);
    ok &= WriteInt(path_, L"PreviewFps", settings.previewFps);
    ok &= WriteInt(path_, L"PreviewPipelineVersion", 2);
    ok &= WriteInt(path_, L"ActiveVk", static_cast<int>(settings.activeDisplayHotkey.vk));
    ok &= WriteInt(path_, L"ActiveModifiers", static_cast<int>(settings.activeDisplayHotkey.modifiers));
    ok &= WriteInt(path_, L"AllVk", static_cast<int>(settings.allDisplaysHotkey.vk));
    ok &= WriteInt(path_, L"AllModifiers", static_cast<int>(settings.allDisplaysHotkey.modifiers));
    ok &= WriteInt(path_, L"RegionVk", static_cast<int>(settings.regionHotkey.vk));
    ok &= WriteInt(path_, L"RegionModifiers", static_cast<int>(settings.regionHotkey.modifiers));
    ok &= WriteInt(path_, L"WindowVk", static_cast<int>(settings.activeWindowHotkey.vk));
    ok &= WriteInt(path_, L"WindowModifiers", static_cast<int>(settings.activeWindowHotkey.modifiers));
    ok &= WriteInt(path_, L"RecordingVk", static_cast<int>(settings.recordingHotkey.vk));
    ok &= WriteInt(path_, L"RecordingModifiers", static_cast<int>(settings.recordingHotkey.modifiers));
    ok &= WriteInt(path_, L"ReplaySaveVk", static_cast<int>(settings.replaySaveHotkey.vk));
    ok &= WriteInt(path_, L"ReplaySaveModifiers", static_cast<int>(settings.replaySaveHotkey.modifiers));
    for (std::size_t index = 0; index < settings.replayMonitorHotkeys.size(); ++index) {
        const std::wstring prefix = L"ReplayMonitor" + std::to_wstring(index);
        ok &= WriteInt(path_, (prefix + L"Vk").c_str(), static_cast<int>(settings.replayMonitorHotkeys[index].vk));
        ok &= WriteInt(path_, (prefix + L"Modifiers").c_str(), static_cast<int>(settings.replayMonitorHotkeys[index].modifiers));
    }
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
