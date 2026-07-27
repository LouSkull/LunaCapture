#include "CaptureEngine.hpp"
#include "AudioMixer.hpp"
#include "RecordingEngine.hpp"
#include "WinUtil.hpp"

#include <Windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <array>
#include <string>

namespace {
RecordingEventType g_lastType = RecordingEventType::Stats;
std::wstring g_lastPath;
std::wstring g_error;
bool g_sawRecovering = false;
bool g_sawRecovered = false;
bool g_captureReady = false;
bool g_screenshotSaved = false;
bool g_recorderFrameReady = false;
std::array<int, 8> g_channelReplayStarted{};
std::array<int, 8> g_channelReplaySaved{};
std::array<std::wstring, 8> g_channelReplayPaths{};
std::array<std::wstring, 8> g_channelErrors{};

LRESULT CALLBACK SmokeProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_APP_ENGINE_EVENT) {
        std::unique_ptr<EngineEvent> event(reinterpret_cast<EngineEvent*>(lParam));
        if (event && event->type == EngineEventType::Ready) g_captureReady = true;
        if (event && event->type == EngineEventType::Saved) g_screenshotSaved = true;
        if (event && event->type == EngineEventType::RecorderFrame && event->requestId == 777 &&
            event->frame && !event->frame->bgra.empty() && event->frame->virtualRect.right > event->frame->virtualRect.left) {
            g_recorderFrameReady = true;
        }
        return 0;
    }
    if (message == WM_APP_RECORDING_EVENT) {
        std::unique_ptr<RecordingEvent> event(reinterpret_cast<RecordingEvent*>(lParam));
        if (event) {
            if (event->channelId >= 0 && event->channelId < static_cast<int>(g_channelReplayStarted.size())) {
                const auto channel = static_cast<std::size_t>(event->channelId);
                if (event->type == RecordingEventType::ReplayStarted) ++g_channelReplayStarted[channel];
                if (event->type == RecordingEventType::ReplaySaved) {
                    ++g_channelReplaySaved[channel];
                    g_channelReplayPaths[channel] = event->path;
                }
                if (event->type == RecordingEventType::Error) g_channelErrors[channel] = event->detail;
            }
            g_lastType = event->type;
            if (!event->path.empty()) g_lastPath = event->path;
            if (event->type == RecordingEventType::Error) g_error = event->detail;
            if (event->type == RecordingEventType::Recovering) g_sawRecovering = true;
            if (event->type == RecordingEventType::Recovered) g_sawRecovered = true;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool PumpUntil(RecordingEventType wanted, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!g_error.empty()) return false;
        if (g_lastType == wanted) return true;
        Sleep(5);
    }
    return false;
}

bool PumpUntilCaptureReady(DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        if (g_captureReady) return true;
        Sleep(5);
    }
    return false;
}

bool PumpUntilReplayChannels(int firstChannel, int secondChannel, bool saved, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        const auto first = static_cast<std::size_t>(firstChannel);
        const auto second = static_cast<std::size_t>(secondChannel);
        if (!g_channelErrors[first].empty() || !g_channelErrors[second].empty()) return false;
        const auto& counts = saved ? g_channelReplaySaved : g_channelReplayStarted;
        if (counts[first] > 0 && counts[second] > 0) return true;
        Sleep(5);
    }
    return false;
}

bool FileLooksLikeMp4(const std::wstring& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(path, ec) &&
           std::filesystem::file_size(path, ec) > 4096;
}

bool FileHasAudioStream(const std::wstring& path) {
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    Microsoft::WRL::ComPtr<IMFMediaType> audio;
    return SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader)) &&
           SUCCEEDED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &audio));
}
}  // namespace

int wmain(int argumentCount, wchar_t** arguments) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
    if (argumentCount > 1 && std::wstring(arguments[1]) == L"--audio-encode-probe") {
        const auto devices = AudioMixer::EnumerateMicrophones();
        if (devices.empty()) {
            std::wcerr << L"No microphone is available\n";
            MFShutdown(); CoUninitialize(); return 20;
        }
        const std::wstring output = (std::filesystem::current_path() / L"audio-encode-probe.m4a").wstring();
        std::error_code ignored;
        std::filesystem::remove(output, ignored);
        AppSettings settings;
        settings.recordSystemAudio = false;
        settings.audioBitrateKbps = 256;  // Regression: this used to disable the encoder.
        settings.microphoneInputs.push_back({devices.front().id, devices.front().name, 100, true});
        AudioMixer mixer;
        Microsoft::WRL::ComPtr<IMFSinkWriter> writer;
        std::wstring warning;
        std::wstring error;
        DWORD stream = 0;
        HRESULT hr = MFCreateSinkWriterFromURL(output.c_str(), nullptr, nullptr, &writer);
        const bool initialized = SUCCEEDED(hr) && mixer.Initialize(settings, warning);
        const bool configured = initialized && mixer.ConfigureWriter(writer.Get(), stream, error);
        if (configured) hr = writer->BeginWriting();
        const bool started = configured && SUCCEEDED(hr) && mixer.Start(error);
        const ULONGLONG deadline = GetTickCount64() + 1200;
        while (started && GetTickCount64() < deadline && error.empty()) {
            mixer.Drain(writer.Get(), stream, error);
            Sleep(5);
        }
        mixer.Stop();
        if (started && error.empty()) hr = writer->Finalize();
        const bool valid = started && error.empty() && SUCCEEDED(hr) &&
                           FileLooksLikeMp4(output) && FileHasAudioStream(output);
        std::wcout << L"audio-encode device=" << devices.front().name << L" configured=" << configured
                   << L" started=" << started << L" warning=" << warning << L" error=" << error
                   << L" output=" << output << L"\n";
        writer.Reset();
        MFShutdown(); CoUninitialize();
        return valid ? 0 : 21;
    }
    if (argumentCount > 1 && std::wstring(arguments[1]) == L"--audio-probe") {
        const auto devices = AudioMixer::EnumerateMicrophones();
        for (const auto& device : devices) {
            float peak = 0.0f;
            bool muted = false;
            for (int sample = 0; sample < 10; ++sample) {
                const auto meter = AudioMixer::QueryMicrophoneMeter(device.id);
                peak = (std::max)(peak, meter.peak);
                muted = meter.muted;
                Sleep(50);
            }
            std::wcout << (device.isDefault ? L"default " : L"") << device.name << L" peak=" << peak
                       << L" muted=" << (muted ? L"yes" : L"no") << L" id=" << device.id << L"\n";
        }
        if (!devices.empty()) {
            AppSettings microphoneSettings;
            microphoneSettings.recordSystemAudio = false;
            microphoneSettings.microphoneInputs.push_back({devices.front().id, devices.front().name, 100, true});
            AudioMixer mixer;
            std::wstring warning;
            std::wstring startError;
            const bool initialized = mixer.Initialize(microphoneSettings, warning);
            const bool started = initialized && mixer.Start(startError);
            std::wcout << L"default-microphone-open initialized=" << initialized << L" started=" << started
                       << L" warning=" << warning << L" error=" << startError << L"\n";
            mixer.Stop();
        }
        MFShutdown();
        CoUninitialize();
        return devices.empty() ? 12 : 0;
    }
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = SmokeProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"LunaRecordingSmokeWindow";
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, instance, nullptr);
    if (!window) return 2;

    const std::wstring output = (std::filesystem::current_path() / L"recording-smoke-output").wstring();
    EnsureDirectoryExists(output);
    AppSettings settings;
    settings.recordingDirectory = output;
    settings.recordingFps = 30;
    settings.recordingBitrateMbps = 4;
    settings.recordingHeight = 720;
    settings.recordSystemAudio = true;
    settings.recordCursor = false;
    settings.replaySeconds = 10;

    if (argumentCount > 3 && std::wstring(arguments[1]) == L"--multi-replay-probe") {
        const int firstMonitor = std::stoi(arguments[2]);
        const int secondMonitor = std::stoi(arguments[3]);
        settings.recordingAutoMonitor = false;
        settings.recordSystemAudio = false;
        settings.recordApplicationAudio = false;
        settings.microphoneInputs.clear();
        settings.replaySeconds = 5;

        RecordingEngine first(window, 1);
        RecordingEngine second(window, 2);
        AppSettings firstSettings = settings;
        AppSettings secondSettings = settings;
        firstSettings.recordingMonitorIndex = firstMonitor;
        secondSettings.recordingMonitorIndex = secondMonitor;
        if (!first.Start() || !second.Start() || !first.StartReplayBuffer(firstSettings) ||
            !second.StartReplayBuffer(secondSettings) || !PumpUntilReplayChannels(1, 2, false, 20000)) {
            std::wcerr << L"Multi-monitor replay start failed: ch1=" << g_channelErrors[1]
                       << L" ch2=" << g_channelErrors[2] << L"\n";
            first.Stop();
            second.Stop();
            return 13;
        }
        const ULONGLONG recordUntil = GetTickCount64() + 2200;
        while (GetTickCount64() < recordUntil) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            Sleep(5);
        }
        first.SaveReplay();
        second.SaveReplay();
        const bool saved = PumpUntilReplayChannels(1, 2, true, 25000);
        const bool valid = saved && FileLooksLikeMp4(g_channelReplayPaths[1]) && FileLooksLikeMp4(g_channelReplayPaths[2]) &&
                           g_channelReplayPaths[1] != g_channelReplayPaths[2];
        first.StopActiveSession();
        second.StopActiveSession();
        first.Stop();
        second.Stop();
        std::wcout << L"multi-replay monitor=" << firstMonitor << L" output=" << g_channelReplayPaths[1]
                   << L" monitor=" << secondMonitor << L" output=" << g_channelReplayPaths[2] << L"\n";
        DestroyWindow(window);
        MFShutdown();
        CoUninitialize();
        return valid ? 0 : 14;
    }

    // Exercise the same ownership handoff used by the app: screenshot capture releases
    // the output before fullscreen recording claims it.
    const bool recordingOnly = argumentCount > 1 && std::wstring(arguments[1]) == L"--record-only";
    if (!recordingOnly) {
        CaptureEngine screenshotEngine(window);
        screenshotEngine.Start(settings);
        if (!PumpUntilCaptureReady(15000)) {
            std::wcerr << L"Screenshot capture handoff setup failed\n";
            screenshotEngine.Stop();
            return 9;
        }
        if (!screenshotEngine.PauseForRecording()) {
            std::wcerr << L"Screenshot capture pause handoff failed\n";
            screenshotEngine.Stop();
            return 17;
        }
        screenshotEngine.Stop();
    }

    RecordingEngine engine(window);
    if (!engine.Start() || !engine.StartRecording(settings)) return 3;
    if (!PumpUntil(RecordingEventType::RecordingStarted, 15000)) {
        std::wcerr << L"Recording start failed: " << g_error << L"\n";
        engine.Stop();
        return 4;
    }
    if (!engine.RequestFrame(CaptureMode::Region, 777, false)) {
        std::wcerr << L"Live recorder frame request failed\n";
        engine.Stop();
        return 15;
    }
    const ULONGLONG frameDeadline = GetTickCount64() + 5000;
    while (!g_recorderFrameReady && GetTickCount64() < frameDeadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Sleep(5);
    }
    if (!g_recorderFrameReady) {
        std::wcerr << L"Live recorder frame was not returned\n";
        engine.Stop();
        return 16;
    }
    engine.NotifyDisplayChange();
    const ULONGLONG reconnectDeadline = GetTickCount64() + 10000;
    while (!g_sawRecovered && GetTickCount64() < reconnectDeadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Sleep(5);
    }
    if (!g_sawRecovering || !g_sawRecovered) {
        std::wcerr << L"Fullscreen reconnect failed\n";
        engine.Stop();
        return 8;
    }
    if (!engine.CaptureScreenshot(settings)) {
        std::wcerr << L"Recorder screenshot queue failed\n";
        engine.Stop();
        return 10;
    }
    const ULONGLONG screenshotDeadline = GetTickCount64() + 10000;
    while (!g_screenshotSaved && GetTickCount64() < screenshotDeadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Sleep(5);
    }
    if (!g_screenshotSaved) {
        std::wcerr << L"Recorder screenshot save failed\n";
        engine.Stop();
        return 11;
    }
    const ULONGLONG recordUntil = GetTickCount64() + 2200;
    while (GetTickCount64() < recordUntil) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Sleep(5);
    }
    engine.StopActiveSession();
    if (!PumpUntil(RecordingEventType::RecordingSaved, 20000) || !FileLooksLikeMp4(g_lastPath) || !FileHasAudioStream(g_lastPath)) {
        std::wcerr << L"Recording finalize failed: " << g_error << L" path=" << g_lastPath << L"\n";
        engine.Stop();
        return 5;
    }
    std::wcout << L"recording=" << g_lastPath << L"\n";

    g_lastType = RecordingEventType::Stats;
    g_lastPath.clear();
    g_error.clear();
    if (!engine.StartReplayBuffer(settings) || !PumpUntil(RecordingEventType::ReplayStarted, 15000)) {
        std::wcerr << L"Replay start failed: " << g_error << L"\n";
        engine.Stop();
        return 6;
    }
    const ULONGLONG replayUntil = GetTickCount64() + 2200;
    while (GetTickCount64() < replayUntil) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Sleep(5);
    }
    engine.SaveReplay();
    if (!PumpUntil(RecordingEventType::ReplaySaved, 20000) || !FileLooksLikeMp4(g_lastPath)) {
        std::wcerr << L"Replay save failed: " << g_error << L" path=" << g_lastPath << L"\n";
        engine.Stop();
        return 7;
    }
    std::wcout << L"replay=" << g_lastPath << L"\n";
    engine.StopActiveSession();
    PumpUntil(RecordingEventType::ReplayStopped, 10000);
    engine.Stop();
    DestroyWindow(window);
    MFShutdown();
    CoUninitialize();
    return 0;
}
