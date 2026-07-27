#include "SettingsStore.hpp"

#include "CoreUtils.hpp"
#include "WinUtil.hpp"

#include <Windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>

using Microsoft::WRL::ComPtr;

namespace {
// Explorer tracks a per-shortcut enable flag independent of the .lnk file itself: whenever a
// user (or Windows) flips a Startup-folder entry off in Task Manager's "Startup apps" tab or
// Settings > Apps > Startup, it stamps this 12-byte value with a leading 0x03 and never clears
// it again on its own. Recreating "LunaCapture.lnk" does not touch this flag, so once an entry
// has been disabled this way — even by an old build, or by the user experimenting — toggling
// autostart back on in-app silently keeps failing to launch at sign-in. Windows 11 surfaces
// this list far more prominently (Settings > Apps > Startup) than Windows 10 ever did, which is
// why the failure is mostly reported there, but the same flag exists and applies on Windows 10.
void ResetStartupApprovedFlag(const wchar_t* shortcutFileName, bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        constexpr BYTE kEnabled[12] = {0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        RegSetValueExW(key, shortcutFileName, 0, REG_BINARY, kEnabled, sizeof(kEnabled));
    } else {
        RegDeleteValueW(key, shortcutFileName);
    }
    RegCloseKey(key);
}

// A per-exe "RUNASADMIN" compatibility shim (Properties > Compatibility > "Run this program as an
// administrator", or a stray Program Compatibility Assistant prompt someone accepted while testing)
// is a second, completely independent way for autostart to silently do nothing: a Startup-folder
// shortcut cannot elevate without a UAC prompt, and Explorer's logon auto-launch never shows one, so
// it just skips the launch instead of failing loudly. Strip only the RUNASADMIN token (preserving any
// other layer flags on the same entry) whenever autostart is enabled, so a shim set at any point in
// this exe's history never permanently blocks it from launching at sign-in again.
void ClearRunAsAdminCompatFlag(const std::wstring& exePath) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
                      0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    wchar_t buffer[512]{};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    DWORD type = 0;
    if (RegQueryValueExW(key, exePath.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size) == ERROR_SUCCESS &&
        type == REG_SZ) {
        std::wstring value(buffer);
        const std::wstring token = L"RUNASADMIN";
        const auto pos = value.find(token);
        if (pos != std::wstring::npos) {
            value.erase(pos, token.size());
            std::wstring trimmed;
            bool lastWasSpace = false;
            for (const wchar_t ch : value) {
                if (ch == L' ') {
                    if (!lastWasSpace) trimmed.push_back(ch);
                    lastWasSpace = true;
                } else {
                    trimmed.push_back(ch);
                    lastWasSpace = false;
                }
            }
            while (!trimmed.empty() && trimmed.back() == L' ') trimmed.pop_back();
            if (trimmed.empty() || trimmed == L"~") {
                RegDeleteValueW(key, exePath.c_str());
            } else {
                RegSetValueExW(key, exePath.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(trimmed.c_str()),
                               static_cast<DWORD>((trimmed.size() + 1) * sizeof(wchar_t)));
            }
        }
    }
    RegCloseKey(key);
}

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

SettingsStore::SettingsStore(std::wstring path) : path_(std::move(path)) {
    EnsureDirectoryExists(std::filesystem::path(path_).parent_path().wstring());
}

AppSettings SettingsStore::Load() const {
    AppSettings settings;
    settings.saveDirectory = ReadString(path_, L"SaveDirectory", GetDefaultScreenshotDirectory());
    settings.imageFormat = ReadInt(path_, L"ImageFormat", 0) == 1 ? ImageFormat::Jpeg : ImageFormat::Png;
    settings.outputWidth = (std::max)(0, (std::min)(7680, ReadInt(path_, L"OutputWidth", 0)));
    settings.outputHeight = (std::max)(0, (std::min)(4320, ReadInt(path_, L"OutputHeight", 0)));
    settings.jpegQuality = (std::max)(1, (std::min)(100, ReadInt(path_, L"JpegQuality", 95)));
    settings.notifications = ReadInt(path_, L"Notifications", 1) != 0;
    settings.sound = ReadInt(path_, L"Sound", 1) != 0;
    settings.startWithWindows = ReadInt(path_, L"StartWithWindows", 0) != 0;
    settings.startHiddenToTray = ReadInt(path_, L"StartHiddenToTray", 0) != 0;
    settings.lastUpdateCheckEpochSeconds = std::wcstoull(
        ReadString(path_, L"LastUpdateCheckEpochSeconds", L"0").c_str(), nullptr, 10);
    settings.copyRegionToClipboard = ReadInt(path_, L"CopyRegionToClipboard", 0) != 0;
    settings.overlayDurationMs = (std::max)(250, (std::min)(10000, ReadInt(path_, L"OverlayDurationMs", 1200)));
    settings.notificationSoundPath = ReadString(path_, L"NotificationSoundPath", L"");
    settings.recordingDirectory = ReadString(path_, L"RecordingDirectory", GetDefaultRecordingDirectory());
    settings.recordingFps = (std::max)(1, (std::min)(240, ReadInt(path_, L"RecordingFps", 60)));
    settings.recordingBitrateMbps = (std::max)(1, (std::min)(200, ReadInt(path_, L"RecordingBitrateMbps", 16)));
    settings.videoCodec =
        static_cast<VideoCodec>((std::max)(0, (std::min)(kVideoCodecCount - 1, ReadInt(path_, L"VideoCodec", 0))));
    settings.capturePriority =
        static_cast<CapturePriority>((std::max)(0, (std::min)(2, ReadInt(path_, L"CapturePriority", 0))));
    settings.videoRateControl =
        static_cast<VideoRateControl>((std::max)(0, (std::min)(2, ReadInt(path_, L"VideoRateControl", 1))));
    settings.videoEncoderEffort =
        static_cast<VideoEncoderEffort>((std::max)(0, (std::min)(2, ReadInt(path_, L"VideoEncoderEffort", 1))));
    settings.videoQuality = (std::max)(1, (std::min)(100, ReadInt(path_, L"VideoQuality", 70)));
    settings.keyframeSeconds = (std::max)(0, (std::min)(10, ReadInt(path_, L"KeyframeSeconds", 2)));
    const int height = ReadInt(path_, L"RecordingHeight", 0);
    settings.recordingHeight = (std::max)(0, (std::min)(4320, height));
    settings.recordingMonitorIndex = (std::max)(0, ReadInt(path_, L"RecordingMonitorIndex", 0));
    settings.recordingAutoMonitor = ReadInt(path_, L"RecordingAutoMonitor", 1) != 0;
    settings.replaySeconds = (std::max)(5, (std::min)(3600, ReadInt(path_, L"ReplaySeconds", 30)));
    settings.replayMonitorMask = static_cast<std::uint32_t>(ReadInt(path_, L"ReplayMonitorMask", 1)) & 0xFFu;
    settings.resumeReplayOnLaunch = ReadInt(path_, L"ResumeReplayOnLaunch", 0) != 0;
    settings.recordSystemAudio = ReadInt(path_, L"RecordSystemAudio", 1) != 0;
    settings.systemAudioVolumePercent = (std::max)(0, (std::min)(200, ReadInt(path_, L"SystemAudioVolume", 100)));
    settings.recordApplicationAudio = ReadInt(path_, L"RecordApplicationAudio", 0) != 0;
    // Per-application loopback and the full system mix are alternative capture
    // sources. Keep persisted/corrupted INI state under the same invariant as
    // the settings UI and LunaScript runtime; the later, more specific choice wins.
    if (settings.recordApplicationAudio) settings.recordSystemAudio = false;
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
        input.noiseSuppressionPercent = (std::max)(0, (std::min)(100, ReadInt(path_, (prefix + L"NoiseSuppression").c_str(), 0)));
        input.noiseGateThresholdDb = (std::max)(20, (std::min)(80, ReadInt(path_, (prefix + L"NoiseGateDb").c_str(), 50)));
        input.enabled = ReadInt(path_, (prefix + L"Enabled").c_str(), 1) != 0;
        settings.microphoneInputs.push_back(std::move(input));
    }
    settings.recordCursor = ReadInt(path_, L"RecordCursor", 1) != 0;
    settings.previewEnabled = ReadInt(path_, L"PreviewEnabled", 1) != 0;
    settings.previewFps = (std::max)(1, (std::min)(60, ReadInt(path_, L"PreviewFps", 30)));
    settings.replayPreviewEnabled = ReadInt(path_, L"ReplayPreviewEnabled", 1) != 0;
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
    settings.quickPanelHotkey = ReadHotkey(path_, L"QuickPanelVk", L"QuickPanelModifiers", L'X',
                                           MOD_ALT | MOD_NOREPEAT);
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
    ok &= WriteInt(path_, L"StartHiddenToTray", settings.startHiddenToTray ? 1 : 0);
    ok &= WriteString(path_, L"LastUpdateCheckEpochSeconds", std::to_wstring(settings.lastUpdateCheckEpochSeconds));
    ok &= WriteInt(path_, L"CopyRegionToClipboard", settings.copyRegionToClipboard ? 1 : 0);
    ok &= WriteInt(path_, L"OverlayDurationMs", settings.overlayDurationMs);
    ok &= WriteString(path_, L"NotificationSoundPath", settings.notificationSoundPath);
    ok &= WriteString(path_, L"RecordingDirectory", settings.recordingDirectory);
    ok &= WriteInt(path_, L"RecordingFps", settings.recordingFps);
    ok &= WriteInt(path_, L"RecordingBitrateMbps", settings.recordingBitrateMbps);
    ok &= WriteInt(path_, L"VideoCodec", static_cast<int>(settings.videoCodec));
    ok &= WriteInt(path_, L"CapturePriority", static_cast<int>(settings.capturePriority));
    ok &= WriteInt(path_, L"VideoRateControl", static_cast<int>(settings.videoRateControl));
    ok &= WriteInt(path_, L"VideoEncoderEffort", static_cast<int>(settings.videoEncoderEffort));
    ok &= WriteInt(path_, L"VideoQuality", settings.videoQuality);
    ok &= WriteInt(path_, L"KeyframeSeconds", settings.keyframeSeconds);
    ok &= WriteInt(path_, L"RecordingHeight", settings.recordingHeight);
    ok &= WriteInt(path_, L"RecordingMonitorIndex", settings.recordingMonitorIndex);
    ok &= WriteInt(path_, L"RecordingAutoMonitor", settings.recordingAutoMonitor ? 1 : 0);
    ok &= WriteInt(path_, L"ReplaySeconds", settings.replaySeconds);
    ok &= WriteInt(path_, L"ReplayMonitorMask", static_cast<int>(settings.replayMonitorMask));
    ok &= WriteInt(path_, L"ResumeReplayOnLaunch", settings.resumeReplayOnLaunch ? 1 : 0);
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
        ok &= WriteInt(path_, (prefix + L"NoiseSuppression").c_str(), input ? input->noiseSuppressionPercent : 0);
        ok &= WriteInt(path_, (prefix + L"NoiseGateDb").c_str(), input ? input->noiseGateThresholdDb : 50);
        ok &= WriteInt(path_, (prefix + L"Enabled").c_str(), input && input->enabled ? 1 : 0);
    }
    ok &= WriteInt(path_, L"RecordCursor", settings.recordCursor ? 1 : 0);
    ok &= WriteInt(path_, L"PreviewEnabled", settings.previewEnabled ? 1 : 0);
    ok &= WriteInt(path_, L"PreviewFps", settings.previewFps);
    ok &= WriteInt(path_, L"ReplayPreviewEnabled", settings.replayPreviewEnabled ? 1 : 0);
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
    ok &= WriteInt(path_, L"QuickPanelVk", static_cast<int>(settings.quickPanelHotkey.vk));
    ok &= WriteInt(path_, L"QuickPanelModifiers", static_cast<int>(settings.quickPanelHotkey.modifiers));
    for (std::size_t index = 0; index < settings.replayMonitorHotkeys.size(); ++index) {
        const std::wstring prefix = L"ReplayMonitor" + std::to_wstring(index);
        ok &= WriteInt(path_, (prefix + L"Vk").c_str(), static_cast<int>(settings.replayMonitorHotkeys[index].vk));
        ok &= WriteInt(path_, (prefix + L"Modifiers").c_str(), static_cast<int>(settings.replayMonitorHotkeys[index].modifiers));
    }
    return ok;
}

bool SettingsStore::ApplyAutostart(bool enabled) const {
    // A Startup-folder shortcut launches the app at sign-in exactly like a
    // HKCU\...\CurrentVersion\Run entry did, but as a plain, visible .lnk
    // file instead of a silent registry write — the Run key is the single
    // most heavily-watched "malware persistence" signal for AV behavioral
    // heuristics, so this is strictly less suspicious for identical behavior.
    PWSTR startupPathRaw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &startupPathRaw)) || !startupPathRaw) {
        if (startupPathRaw) CoTaskMemFree(startupPathRaw);
        return false;
    }
    const std::filesystem::path shortcutPath = std::filesystem::path(startupPathRaw) / L"LunaCapture.lnk";
    CoTaskMemFree(startupPathRaw);

    // Clean up any legacy Run-key entry (from this app or the previous
    // "LunaShot" name) so upgrading from an older build never launches twice.
    HKEY runKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE,
                      &runKey) == ERROR_SUCCESS) {
        RegDeleteValueW(runKey, L"LunaCapture");
        RegDeleteValueW(runKey, L"LunaShot");
        RegCloseKey(runKey);
    }

    if (!enabled) {
        std::error_code error;
        std::filesystem::remove(shortcutPath, error);
        ResetStartupApprovedFlag(L"LunaCapture.lnk", false);
        if (error) {
            const std::string narrow = error.message();
            AppendDiagnosticLog(L"Autostart disable: could not remove shortcut: " + std::wstring(narrow.begin(), narrow.end()));
        }
        return !error;
    }

    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comOwned = SUCCEEDED(comInit);
    bool ok = false;
    ComPtr<IShellLinkW> shellLink;
    const HRESULT createHr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink));
    if (SUCCEEDED(createHr)) {
        const std::wstring exePath = GetExecutablePath();
        ClearRunAsAdminCompatFlag(exePath);
        shellLink->SetPath(exePath.c_str());
        shellLink->SetArguments(L"--background");
        shellLink->SetDescription(L"Luna Capture");
        shellLink->SetWorkingDirectory(std::filesystem::path(exePath).parent_path().c_str());
        ComPtr<IPersistFile> persistFile;
        if (SUCCEEDED(shellLink.As(&persistFile))) {
            ok = SUCCEEDED(persistFile->Save(shortcutPath.c_str(), TRUE));
        }
    }
    if (comOwned) CoUninitialize();
    if (ok) {
        // Clear any prior "disabled by user/system" flag so a shortcut that was switched off in
        // Task Manager or Settings > Apps > Startup actually launches again, not just re-exists.
        ResetStartupApprovedFlag(L"LunaCapture.lnk", true);
    } else {
        AppendDiagnosticLog(L"Autostart enable failed: shell link creation returned 0x" +
                           std::to_wstring(static_cast<unsigned long>(createHr)));
    }
    return ok;
}
