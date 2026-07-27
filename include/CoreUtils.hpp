#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

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

std::wstring SanitizeFilenamePrefix(const std::wstring& value, const std::wstring& fallback = L"Desktop");
std::wstring MakeScreenshotFilename(const std::wstring& prefix, int year, int month, int day, int hour, int minute, int second, int millis, bool jpeg);
std::wstring FileNameOnly(const std::wstring& path);
std::wstring EllipsizeMiddle(const std::wstring& value, std::size_t maxChars);
double QpcMilliseconds(std::int64_t start, std::int64_t end, std::int64_t frequency);
int NormalizeAacBitrateKbps(int requestedKbps);
