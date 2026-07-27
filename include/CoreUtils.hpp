#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

class InteractiveCaptureGate {
public:
    bool TryBegin() noexcept {
        if (active_) return false;
        active_ = true;
        return true;
    }
    void End() noexcept { active_ = false; }
    bool IsActive() const noexcept { return active_; }

private:
    bool active_ = false;
};

// Latches after too many asynchronous media actions are requested from plugin
// completion handlers in a short window. Timers and user-invoked commands use a
// different path, so legitimate scheduled capture remains available.
class PluginFeedbackCircuitBreaker {
public:
    static constexpr std::size_t kBurstLimit = 8;
    static constexpr std::uint64_t kWindowMilliseconds = 30'000;

    bool TryAcquire(std::uint64_t nowMilliseconds) noexcept;
    bool IsTripped() const noexcept { return tripped_; }
    void Reset() noexcept;

private:
    std::array<std::uint64_t, kBurstLimit> timestamps_{};
    std::size_t count_ = 0;
    bool tripped_ = false;
};

class BgraPreviewScaler {
public:
    bool Scale(const std::uint8_t* source, std::uint32_t sourceWidth, std::uint32_t sourceHeight,
               std::uint32_t sourceStride, std::uint32_t maximumWidth, std::uint32_t maximumHeight,
               std::vector<std::uint8_t>& destination, std::uint32_t& destinationWidth,
               std::uint32_t& destinationHeight, std::uint32_t& destinationStride);
    void Reset();

private:
    std::uint32_t sourceWidth_ = 0;
    std::uint32_t sourceHeight_ = 0;
    std::uint32_t destinationWidth_ = 0;
    std::uint32_t destinationHeight_ = 0;
    std::vector<std::uint32_t> xMap_;
    std::vector<std::uint32_t> yMap_;
};

bool BlitBgraScaledNearest(const std::uint8_t* source, std::uint32_t sourceWidth,
                           std::uint32_t sourceHeight, std::uint32_t sourceStride,
                           std::uint8_t* destination, std::uint32_t destinationWidth,
                           std::uint32_t destinationHeight, std::uint32_t destinationStride);
// Same contract as BlitBgraScaledNearest but samples with bilinear interpolation instead of
// nearest-neighbor. Use this whenever the result is the actual saved/exported image (screenshots,
// stitched multi-monitor frames) rather than a live low-res preview, where nearest-neighbor's
// blocky resampling reads as a pixelated capture instead of just a soft one.
bool BlitBgraScaledBilinear(const std::uint8_t* source, std::uint32_t sourceWidth,
                            std::uint32_t sourceHeight, std::uint32_t sourceStride,
                            std::uint8_t* destination, std::uint32_t destinationWidth,
                            std::uint32_t destinationHeight, std::uint32_t destinationStride);
bool FillCaptureRecoveryFrame(std::uint8_t* destination, std::uint32_t width,
                              std::uint32_t height, std::uint32_t stride);
std::wstring SanitizeFilenamePrefix(const std::wstring& value, const std::wstring& fallback = L"Desktop");
std::wstring MakeTimestampedMediaFilename(const std::wstring& prefix, int year, int month, int day,
                                          int hour, int minute, int second, int millis,
                                          const std::wstring& extension);
std::wstring MakeScreenshotFilename(const std::wstring& prefix, int year, int month, int day, int hour, int minute, int second, int millis, bool jpeg);
// Returns a process-wide unique path whose timestamp is strictly increasing. Keeping
// the timestamp at the front makes mixed screenshots, recordings, and replays sort in
// capture order, even when several names are requested in the same millisecond.
std::wstring MakeChronologicalMediaPath(const std::wstring& directory, const std::wstring& prefix,
                                        const std::wstring& extension);
bool HasChronologicalMediaFilename(const std::wstring& filename) noexcept;
std::wstring FileNameOnly(const std::wstring& path);
std::wstring EllipsizeMiddle(const std::wstring& value, std::size_t maxChars);
double QpcMilliseconds(std::int64_t start, std::int64_t end, std::int64_t frequency);
int NormalizeAacBitrateKbps(int requestedKbps);
