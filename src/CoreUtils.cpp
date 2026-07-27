#include "CoreUtils.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <mutex>
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

bool BlitBgraScaledNearest(
    const std::uint8_t* source,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::uint32_t sourceStride,
    std::uint8_t* destination,
    std::uint32_t destinationWidth,
    std::uint32_t destinationHeight,
    std::uint32_t destinationStride) {
    if (!source || !destination || !sourceWidth || !sourceHeight ||
        !destinationWidth || !destinationHeight ||
        static_cast<std::uint64_t>(sourceWidth) * 4 > sourceStride ||
        static_cast<std::uint64_t>(destinationWidth) * 4 > destinationStride) {
        return false;
    }

    if (sourceWidth == destinationWidth && sourceHeight == destinationHeight) {
        for (std::uint32_t y = 0; y < destinationHeight; ++y) {
            std::memcpy(destination + static_cast<std::size_t>(y) * destinationStride,
                        source + static_cast<std::size_t>(y) * sourceStride,
                        static_cast<std::size_t>(destinationWidth) * 4);
        }
        return true;
    }

    for (std::uint32_t y = 0; y < destinationHeight; ++y) {
        const std::uint32_t sourceY = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(y) * sourceHeight / destinationHeight);
        const auto* sourceRow = source + static_cast<std::size_t>(sourceY) * sourceStride;
        auto* destinationRow = destination + static_cast<std::size_t>(y) * destinationStride;
        for (std::uint32_t x = 0; x < destinationWidth; ++x) {
            const std::uint32_t sourceX = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(x) * sourceWidth / destinationWidth);
            const auto* sourcePixel = sourceRow + static_cast<std::size_t>(sourceX) * 4;
            auto* destinationPixel = destinationRow + static_cast<std::size_t>(x) * 4;
            destinationPixel[0] = sourcePixel[0];
            destinationPixel[1] = sourcePixel[1];
            destinationPixel[2] = sourcePixel[2];
            destinationPixel[3] = 255;
        }
    }
    return true;
}

bool BlitBgraScaledBilinear(
    const std::uint8_t* source,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::uint32_t sourceStride,
    std::uint8_t* destination,
    std::uint32_t destinationWidth,
    std::uint32_t destinationHeight,
    std::uint32_t destinationStride) {
    if (!source || !destination || !sourceWidth || !sourceHeight ||
        !destinationWidth || !destinationHeight ||
        static_cast<std::uint64_t>(sourceWidth) * 4 > sourceStride ||
        static_cast<std::uint64_t>(destinationWidth) * 4 > destinationStride) {
        return false;
    }

    if (sourceWidth == destinationWidth && sourceHeight == destinationHeight) {
        for (std::uint32_t y = 0; y < destinationHeight; ++y) {
            std::memcpy(destination + static_cast<std::size_t>(y) * destinationStride,
                        source + static_cast<std::size_t>(y) * sourceStride,
                        static_cast<std::size_t>(destinationWidth) * 4);
        }
        return true;
    }

    const double scaleX = static_cast<double>(sourceWidth) / destinationWidth;
    const double scaleY = static_cast<double>(sourceHeight) / destinationHeight;
    const auto clampIndex = [](double value, std::uint32_t limit) {
        const double clamped = (std::max)(0.0, (std::min)(value, static_cast<double>(limit - 1)));
        return static_cast<std::uint32_t>(clamped);
    };
    for (std::uint32_t y = 0; y < destinationHeight; ++y) {
        // Sample at the destination pixel's center, mapped back into source space, so a whole-
        // number scale factor lands exactly on source pixel centers instead of a half-pixel off.
        const double sourceYf = (static_cast<double>(y) + 0.5) * scaleY - 0.5;
        const double y0f = std::floor(sourceYf);
        const double fy = sourceYf - y0f;
        const std::uint32_t y0 = clampIndex(y0f, sourceHeight);
        const std::uint32_t y1 = clampIndex(y0f + 1.0, sourceHeight);
        const auto* row0 = source + static_cast<std::size_t>(y0) * sourceStride;
        const auto* row1 = source + static_cast<std::size_t>(y1) * sourceStride;
        auto* destinationRow = destination + static_cast<std::size_t>(y) * destinationStride;
        for (std::uint32_t x = 0; x < destinationWidth; ++x) {
            const double sourceXf = (static_cast<double>(x) + 0.5) * scaleX - 0.5;
            const double x0f = std::floor(sourceXf);
            const double fx = sourceXf - x0f;
            const std::uint32_t x0 = clampIndex(x0f, sourceWidth);
            const std::uint32_t x1 = clampIndex(x0f + 1.0, sourceWidth);
            const auto* p00 = row0 + static_cast<std::size_t>(x0) * 4;
            const auto* p10 = row0 + static_cast<std::size_t>(x1) * 4;
            const auto* p01 = row1 + static_cast<std::size_t>(x0) * 4;
            const auto* p11 = row1 + static_cast<std::size_t>(x1) * 4;
            auto* destinationPixel = destinationRow + static_cast<std::size_t>(x) * 4;
            for (int channel = 0; channel < 3; ++channel) {
                const double top = p00[channel] + (p10[channel] - p00[channel]) * fx;
                const double bottom = p01[channel] + (p11[channel] - p01[channel]) * fx;
                const double value = top + (bottom - top) * fy;
                destinationPixel[channel] = static_cast<std::uint8_t>((std::clamp)(value + 0.5, 0.0, 255.0));
            }
            destinationPixel[3] = 255;
        }
    }
    return true;
}

bool FillCaptureRecoveryFrame(std::uint8_t* destination, std::uint32_t width,
                              std::uint32_t height, std::uint32_t stride) {
    if (!destination || width == 0 || height == 0 || stride < width * 4) return false;

    // A dark, deterministic recovery card. It is cheap enough to regenerate only when
    // capture is lost and is visually distinct from both a frozen frame and plain black.
    const std::uint32_t panelLeft = width / 6;
    const std::uint32_t panelRight = width - panelLeft;
    const std::uint32_t panelTop = (height * 2) / 5;
    const std::uint32_t panelBottom = (height * 3) / 5;
    const std::uint32_t accentHeight = (std::max)(1U, height / 180);
    for (std::uint32_t y = 0; y < height; ++y) {
        auto* row = destination + static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint8_t gradient = static_cast<std::uint8_t>(10 + (y * 8) / height);
            const std::uint8_t stripe = ((x + y) / (std::max)(12U, width / 80)) % 2 == 0 ? 2 : 0;
            std::uint8_t blue = static_cast<std::uint8_t>(gradient + 9 + stripe);
            std::uint8_t green = static_cast<std::uint8_t>(gradient + 4 + stripe);
            std::uint8_t red = static_cast<std::uint8_t>(gradient + stripe);
            if (x >= panelLeft && x < panelRight && y >= panelTop && y < panelBottom) {
                blue = 31;
                green = 25;
                red = 22;
                if (y < panelTop + accentHeight) {
                    blue = 238;
                    green = 112;
                    red = 124;
                }
            }
            const std::size_t offset = static_cast<std::size_t>(x) * 4;
            row[offset + 0] = blue;
            row[offset + 1] = green;
            row[offset + 2] = red;
            row[offset + 3] = 255;
        }
    }
    return true;
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

std::wstring MakeTimestampedMediaFilename(
    const std::wstring& prefix,
    int year,
    int month,
    int day,
    int hour,
    int minute,
    int second,
    int millis,
    const std::wstring& extension) {
    // Timestamp leads and the source name trails so files still sort into chronological
    // order by filename even when captures come from different apps/monitors/prefixes.
    std::wostringstream out;
    out << std::setfill(L'0') << std::setw(4) << year << L'-' << std::setw(2) << month << L'-'
        << std::setw(2) << day << L'_' << std::setw(2) << hour << L'-' << std::setw(2) << minute << L'-'
        << std::setw(2) << second << L'_' << std::setw(3) << millis << L'_' << SanitizeFilenamePrefix(prefix)
        << (extension.empty() || extension.front() == L'.' ? extension : L"." + extension);
    return out.str();
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
    return MakeTimestampedMediaFilename(prefix, year, month, day, hour, minute, second, millis,
                                        jpeg ? L".jpg" : L".png");
}

std::wstring MakeChronologicalMediaPath(const std::wstring& directory, const std::wstring& prefix,
                                        const std::wstring& extension) {
    // Local FILETIME is intentionally treated as a sortable wall-clock counter. When the
    // clock stalls or moves backwards, advance it by one millisecond so names remain unique
    // and lexicographically chronological for the lifetime of the process.
    static std::mutex mutex;
    static ULARGE_INTEGER lastLocalTime{};

    FILETIME utcFileTime{};
    FILETIME localFileTime{};
    GetSystemTimeAsFileTime(&utcFileTime);
    if (!FileTimeToLocalFileTime(&utcFileTime, &localFileTime)) localFileTime = utcFileTime;

    std::lock_guard lock(mutex);
    ULARGE_INTEGER candidateTime{};
    candidateTime.LowPart = localFileTime.dwLowDateTime;
    candidateTime.HighPart = localFileTime.dwHighDateTime;
    constexpr ULONGLONG kOneMillisecond = 10'000;
    if (candidateTime.QuadPart <= lastLocalTime.QuadPart) {
        candidateTime.QuadPart = lastLocalTime.QuadPart + kOneMillisecond;
    }

    std::wstring candidate;
    std::error_code ec;
    for (;;) {
        FILETIME sortableTime{candidateTime.LowPart, candidateTime.HighPart};
        SYSTEMTIME local{};
        if (!FileTimeToSystemTime(&sortableTime, &local)) GetLocalTime(&local);
        const auto filename = MakeTimestampedMediaFilename(
            prefix, local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
            local.wSecond, local.wMilliseconds, extension);
        candidate = (std::filesystem::path(directory) / filename).wstring();
        ec.clear();
        if (!std::filesystem::exists(candidate, ec)) break;
        candidateTime.QuadPart += kOneMillisecond;
    }
    lastLocalTime = candidateTime;
    return candidate;
}

bool HasChronologicalMediaFilename(const std::wstring& filename) noexcept {
    // YYYY-MM-DD_HH-MM-SS_mmm_<label>.<ext>
    if (filename.size() < 26) return false;
    constexpr std::size_t separators[] = {4, 7, 10, 13, 16, 19, 23};
    constexpr wchar_t expected[] = {L'-', L'-', L'_', L'-', L'-', L'_', L'_'};
    for (std::size_t index = 0; index < std::size(separators); ++index) {
        if (filename[separators[index]] != expected[index]) return false;
    }
    for (std::size_t index = 0; index < 23; ++index) {
        bool separator = false;
        for (const auto position : separators) {
            if (index == position) { separator = true; break; }
        }
        if (!separator && (filename[index] < L'0' || filename[index] > L'9')) return false;
    }
    return true;
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
bool PluginFeedbackCircuitBreaker::TryAcquire(std::uint64_t nowMilliseconds) noexcept {
    if (tripped_) return false;
    std::size_t retained = 0;
    for (std::size_t index = 0; index < count_; ++index) {
        const std::uint64_t timestamp = timestamps_[index];
        if (nowMilliseconds < timestamp || nowMilliseconds - timestamp < kWindowMilliseconds) {
            timestamps_[retained++] = timestamp;
        }
    }
    count_ = retained;
    if (count_ >= kBurstLimit) {
        tripped_ = true;
        return false;
    }
    timestamps_[count_++] = nowMilliseconds;
    return true;
}

void PluginFeedbackCircuitBreaker::Reset() noexcept {
    timestamps_.fill(0);
    count_ = 0;
    tripped_ = false;
}
