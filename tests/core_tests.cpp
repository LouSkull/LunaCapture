#include "CoreUtils.hpp"

#include <chrono>
#include <cmath>
#include <iostream>

int main() {
    if (MakeScreenshotFilename(L"Discord", 2026, 7, 21, 9, 5, 4, 7, false) != L"Discord_2026-07-21_09-05-04_007.png") return 10;
    if (MakeScreenshotFilename(L"Grand Theft Auto V", 2026, 7, 21, 9, 5, 4, 7, true) != L"Grand Theft Auto V_2026-07-21_09-05-04_007.jpg") return 11;
    if (SanitizeFilenamePrefix(L"  Discord: Friends / Chat  ") != L"Discord_ Friends _ Chat") return 12;
    if (SanitizeFilenamePrefix(L"...   ") != L"Desktop") return 13;
    if (FileNameOnly(L"C:\\Shots\\a.png") != L"a.png") return 14;
    if (EllipsizeMiddle(L"1234567890", 7) != L"123…890") return 15;
    if (std::abs(QpcMilliseconds(100, 350, 1000) - 250.0) >= 0.0001) return 16;
    if (NormalizeAacBitrateKbps(64) != 96 || NormalizeAacBitrateKbps(127) != 128 ||
        NormalizeAacBitrateKbps(176) != 160 || NormalizeAacBitrateKbps(256) != 192) return 17;
    const std::uint8_t sourcePixels[] = {
        1, 2, 3, 0,  4, 5, 6, 0,
        7, 8, 9, 0,  10, 11, 12, 0,
    };
    std::vector<std::uint8_t> scaled;
    std::uint32_t scaledWidth = 0;
    std::uint32_t scaledHeight = 0;
    std::uint32_t scaledStride = 0;
    BgraPreviewScaler scaler;
    if (!scaler.Scale(sourcePixels, 2, 2, 8, 1, 1, scaled,
                      scaledWidth, scaledHeight, scaledStride) ||
        scaledWidth != 1 || scaledHeight != 1 || scaledStride != 4 ||
        scaled != std::vector<std::uint8_t>{1, 2, 3, 255}) return 2;

    std::vector<std::uint8_t> fullHd(static_cast<std::size_t>(1920) * 1080 * 4, 127);
    const auto benchmarkStart = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 120; ++frame) {
        if (!scaler.Scale(fullHd.data(), 1920, 1080, 1920 * 4, 640, 360, scaled,
                          scaledWidth, scaledHeight, scaledStride)) return 3;
    }
    const auto benchmarkMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - benchmarkStart).count();
    std::cout << "Preview scaler: 120 frames in " << benchmarkMs << " ms\n";
    std::cout << "Core tests passed\n";
    return 0;
}
