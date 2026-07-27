#pragma once

#include <Windows.h>
#include <mmsystem.h>

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

class SoundPlayer {
public:
    SoundPlayer() = default;
    ~SoundPlayer();

    SoundPlayer(const SoundPlayer&) = delete;
    SoundPlayer& operator=(const SoundPlayer&) = delete;

    bool Initialize(HINSTANCE instance, int resourceId);
    bool LoadFile(const std::wstring& path);
    void Play();
    void Shutdown();
    bool IsReady() const noexcept { return waveOut_ != nullptr && prepared_; }

private:
    bool LoadWaveBytes(const std::uint8_t* bytes, std::size_t size);
    HWAVEOUT waveOut_ = nullptr;
    WAVEFORMATEX format_{};
    WAVEHDR header_{};
    std::vector<std::uint8_t> audioData_;
    bool prepared_ = false;
};
