#pragma once

#include "Types.hpp"

#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <memory>
#include <string>
#include <vector>

class AudioMixer {
public:
    AudioMixer();
    ~AudioMixer();

    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    static std::vector<AudioDeviceInfo> EnumerateMicrophones();
    static float QueryMicrophonePeak(const std::wstring& deviceId);
    static AudioMeterState QueryMicrophoneMeter(const std::wstring& deviceId);

    bool Initialize(const AppSettings& settings, std::wstring& warning);
    bool ConfigureWriter(IMFSinkWriter* writer, DWORD& streamIndex, std::wstring& error);
    bool Start(std::wstring& error);
    void Stop();
    bool Drain(IMFSinkWriter* writer, DWORD streamIndex, std::wstring& error);
    bool HasSources() const noexcept;
    std::size_t SourceCount() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
