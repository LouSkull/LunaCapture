#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

class BgraPreviewScaler {
public:
    bool Scale(const std::uint8_t*, std::uint32_t, std::uint32_t, std::uint32_t,
               std::uint32_t, std::uint32_t, std::vector<std::uint8_t>&,
               std::uint32_t&, std::uint32_t&, std::uint32_t&);
    void Reset();
private:
    std::uint32_t sourceWidth_ = 0, sourceHeight_ = 0, destinationWidth_ = 0, destinationHeight_ = 0;
    std::vector<std::uint32_t> xMap_, yMap_;
};

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

// Resizes saved/exported screenshot pixels with bilinear interpolation.
bool BlitBgraScaledBilinear(const std::uint8_t* source, std::uint32_t sourceWidth,
                            std::uint32_t sourceHeight, std::uint32_t sourceStride,
                            std::uint8_t* destination, std::uint32_t destinationWidth,
                            std::uint32_t destinationHeight, std::uint32_t destinationStride);
std::wstring SanitizeFilenamePrefix(const std::wstring& value, const std::wstring& fallback = L"Desktop");
std::wstring MakeScreenshotFilename(const std::wstring& prefix, int year, int month, int day,
                                    int hour, int minute, int second, int millis, bool jpeg);
// Returns a process-wide unique path whose timestamp is strictly increasing. Keeping
// the timestamp at the front makes screenshots sort in capture order, even when
// several names are requested in the same millisecond.
std::wstring MakeChronologicalScreenshotPath(const std::wstring& directory,
                                             const std::wstring& prefix, bool jpeg);
bool HasChronologicalScreenshotFilename(const std::wstring& filename) noexcept;
std::wstring MakeChronologicalMediaPath(const std::wstring& directory, const std::wstring& prefix,
                                        const std::wstring& extension);
bool HasChronologicalMediaFilename(const std::wstring& filename) noexcept;
std::wstring FileNameOnly(const std::wstring& path);
std::wstring EllipsizeMiddle(const std::wstring& value, std::size_t maxChars);
double QpcMilliseconds(std::int64_t start, std::int64_t end, std::int64_t frequency);
