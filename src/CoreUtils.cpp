#include "CoreUtils.hpp"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <cwctype>

bool BgraPreviewScaler::Scale(
    const std::uint8_t* source,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::uint32_t sourceStride,
    std::uint32_t maximumWidth,
    std::uint32_t maximumHeight,
    std::vector<std::uint8_t>& destination,
    std::uint32_t& destinationWidth,
    std::uint32_t& destinationHeight,
    std::uint32_t& destinationStride) {
    if (!source || !sourceWidth || !sourceHeight || sourceStride < sourceWidth * 4 ||
        !maximumWidth || !maximumHeight) return false;

    const double scale = (std::min)(static_cast<double>(maximumWidth) / sourceWidth,
                                    static_cast<double>(maximumHeight) / sourceHeight);
    destinationWidth = (std::max)(1U, static_cast<std::uint32_t>(sourceWidth * scale));
    destinationHeight = (std::max)(1U, static_cast<std::uint32_t>(sourceHeight * scale));
    destinationStride = destinationWidth * 4;
    if (sourceWidth_ != sourceWidth || sourceHeight_ != sourceHeight ||
        destinationWidth_ != destinationWidth || destinationHeight_ != destinationHeight) {
        sourceWidth_ = sourceWidth;
        sourceHeight_ = sourceHeight;
        destinationWidth_ = destinationWidth;
        destinationHeight_ = destinationHeight;
        xMap_.resize(destinationWidth);
        yMap_.resize(destinationHeight);
        for (std::uint32_t x = 0; x < destinationWidth; ++x) {
            xMap_[x] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * sourceWidth / destinationWidth);
        }
        for (std::uint32_t y = 0; y < destinationHeight; ++y) {
            yMap_[y] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * sourceHeight / destinationHeight);
        }
    }

    destination.resize(static_cast<std::size_t>(destinationStride) * destinationHeight);
    for (std::uint32_t y = 0; y < destinationHeight; ++y) {
        const auto* sourceRow = source + static_cast<std::size_t>(yMap_[y]) * sourceStride;
        auto* destinationRow = destination.data() + static_cast<std::size_t>(y) * destinationStride;
        for (std::uint32_t x = 0; x < destinationWidth; ++x) {
            const auto* sourcePixel = sourceRow + static_cast<std::size_t>(xMap_[x]) * 4;
            auto* destinationPixel = destinationRow + static_cast<std::size_t>(x) * 4;
            destinationPixel[0] = sourcePixel[0];
            destinationPixel[1] = sourcePixel[1];
            destinationPixel[2] = sourcePixel[2];
            destinationPixel[3] = 255;
        }
    }
    return true;
}

void BgraPreviewScaler::Reset() {
    sourceWidth_ = sourceHeight_ = destinationWidth_ = destinationHeight_ = 0;
    xMap_.clear();
    yMap_.clear();
}

std::wstring SanitizeFilenamePrefix(const std::wstring& value, const std::wstring& fallback) {
    std::wstring result;
    result.reserve((std::min)(value.size(), static_cast<std::size_t>(80)));
    bool pendingSpace = false;
    for (wchar_t ch : value) {
        if (result.size() >= 80) break;
        if (ch < 32 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' || ch == L'/' ||
            ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
            ch = L'_';
        }
        if (std::iswspace(ch)) {
            pendingSpace = !result.empty();
            continue;
        }
        if (pendingSpace) {
            result.push_back(L' ');
            pendingSpace = false;
        }
        result.push_back(ch);
    }
    while (!result.empty() && (result.back() == L' ' || result.back() == L'.')) result.pop_back();
    std::size_t first = 0;
    while (first < result.size() && (result[first] == L' ' || result[first] == L'.')) ++first;
    if (first > 0) result.erase(0, first);
    if (result.empty()) result = fallback.empty() ? L"Desktop" : fallback;
    return result;
}

std::wstring MakeScreenshotFilename(
    const std::wstring& prefix,
    int year,
    int month,
    int day,
    int hour,
    int minute,
    int second,
    int millis,
    bool jpeg) {
    std::wostringstream out;
    out << SanitizeFilenamePrefix(prefix) << L"_" << std::setfill(L'0') << std::setw(4) << year << L'-' << std::setw(2) << month << L'-'
        << std::setw(2) << day << L'_' << std::setw(2) << hour << L'-' << std::setw(2) << minute << L'-'
        << std::setw(2) << second << L'_' << std::setw(3) << millis << (jpeg ? L".jpg" : L".png");
    return out.str();
}

std::wstring FileNameOnly(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring EllipsizeMiddle(const std::wstring& value, std::size_t maxChars) {
    if (value.size() <= maxChars || maxChars < 5) {
        return value;
    }
    const auto left = (maxChars - 1) / 2;
    const auto right = maxChars - left - 1;
    return value.substr(0, left) + L"…" + value.substr(value.size() - right);
}

double QpcMilliseconds(std::int64_t start, std::int64_t end, std::int64_t frequency) {
    if (frequency <= 0 || end < start) {
        return 0.0;
    }
    return static_cast<double>(end - start) * 1000.0 / static_cast<double>(frequency);
}

int NormalizeAacBitrateKbps(int requestedKbps) {
    // Microsoft's inbox AAC encoder accepts only these stereo rates. Keeping this
    // normalization in one place prevents an unsupported custom value from silently
    // disabling every audio source in the recording.
    constexpr int supported[] = {96, 128, 160, 192};
    int closest = supported[0];
    for (const int candidate : supported) {
        if (std::abs(requestedKbps - candidate) < std::abs(requestedKbps - closest)) closest = candidate;
    }
    return closest;
}
