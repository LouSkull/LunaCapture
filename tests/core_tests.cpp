#include "CoreUtils.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    assert(MakeScreenshotFilename(L"Discord", 2026, 7, 21, 9, 5, 4, 7, false) == L"Discord_2026-07-21_09-05-04_007.png");
    assert(MakeScreenshotFilename(L"Grand Theft Auto V", 2026, 7, 21, 9, 5, 4, 7, true) == L"Grand Theft Auto V_2026-07-21_09-05-04_007.jpg");
    assert(SanitizeFilenamePrefix(L"  Discord: Friends / Chat  ") == L"Discord_ Friends _ Chat");
    assert(SanitizeFilenamePrefix(L"...   ") == L"Desktop");
    assert(FileNameOnly(L"C:\\Shots\\a.png") == L"a.png");
    assert(EllipsizeMiddle(L"1234567890", 7) == L"123…890");
    assert(std::abs(QpcMilliseconds(100, 350, 1000) - 250.0) < 0.0001);
    std::cout << "Core tests passed\n";
    return 0;
}
