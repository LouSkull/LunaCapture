#include "SoundPlayer.hpp"

#include <algorithm>
#include <cstring>

namespace {
std::uint32_t ReadU32(const std::uint8_t* data) {
    std::uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::uint16_t ReadU16(const std::uint8_t* data) {
    std::uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

bool MatchFour(const std::uint8_t* data, const char text[5]) {
    return std::memcmp(data, text, 4) == 0;
}
}  // namespace

SoundPlayer::~SoundPlayer() {
    Shutdown();
}

bool SoundPlayer::Initialize(HINSTANCE instance, int resourceId) {
    Shutdown();

    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(resourceId), L"WAVE");
    if (!resource) return false;
    HGLOBAL loaded = LoadResource(instance, resource);
    if (!loaded) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(LockResource(loaded));
    const DWORD size = SizeofResource(instance, resource);
    if (!bytes || size < 44 || !MatchFour(bytes, "RIFF") || !MatchFour(bytes + 8, "WAVE")) return false;

    const std::uint8_t* fmtData = nullptr;
    std::uint32_t fmtSize = 0;
    const std::uint8_t* waveData = nullptr;
    std::uint32_t waveSize = 0;

    std::size_t offset = 12;
    while (offset + 8 <= size) {
        const auto* chunk = bytes + offset;
        const std::uint32_t chunkSize = ReadU32(chunk + 4);
        const std::size_t payload = offset + 8;
        if (payload + chunkSize > size) break;
        if (MatchFour(chunk, "fmt ")) {
            fmtData = bytes + payload;
            fmtSize = chunkSize;
        } else if (MatchFour(chunk, "data")) {
            waveData = bytes + payload;
            waveSize = chunkSize;
        }
        offset = payload + chunkSize + (chunkSize & 1u);
    }

    if (!fmtData || fmtSize < 16 || !waveData || waveSize == 0) return false;
    format_.wFormatTag = ReadU16(fmtData + 0);
    format_.nChannels = ReadU16(fmtData + 2);
    format_.nSamplesPerSec = ReadU32(fmtData + 4);
    format_.nAvgBytesPerSec = ReadU32(fmtData + 8);
    format_.nBlockAlign = ReadU16(fmtData + 12);
    format_.wBitsPerSample = ReadU16(fmtData + 14);
    format_.cbSize = 0;
    if (format_.wFormatTag != WAVE_FORMAT_PCM) return false;

    audioData_.assign(waveData, waveData + waveSize);
    MMRESULT result = waveOutOpen(&waveOut_, WAVE_MAPPER, &format_, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        waveOut_ = nullptr;
        audioData_.clear();
        return false;
    }

    header_ = {};
    header_.lpData = reinterpret_cast<LPSTR>(audioData_.data());
    header_.dwBufferLength = static_cast<DWORD>(audioData_.size());
    result = waveOutPrepareHeader(waveOut_, &header_, sizeof(header_));
    if (result != MMSYSERR_NOERROR) {
        waveOutClose(waveOut_);
        waveOut_ = nullptr;
        audioData_.clear();
        return false;
    }
    prepared_ = true;
    return true;
}

void SoundPlayer::Play() {
    if (!waveOut_ || !prepared_) return;
    if ((header_.dwFlags & WHDR_INQUEUE) != 0) {
        waveOutReset(waveOut_);
    }
    waveOutWrite(waveOut_, &header_, sizeof(header_));
}

void SoundPlayer::Shutdown() {
    if (waveOut_) {
        waveOutReset(waveOut_);
        if (prepared_) {
            waveOutUnprepareHeader(waveOut_, &header_, sizeof(header_));
        }
        waveOutClose(waveOut_);
    }
    waveOut_ = nullptr;
    prepared_ = false;
    header_ = {};
    audioData_.clear();
    format_ = {};
}
