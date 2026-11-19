#include "CoreUtils.hpp"
#include "Localization.hpp"
#include <filesystem>
int main() {
    if (MakeScreenshotFilename(L"Desk",2026,9,5,10,20,30,4,false) != L"2026-09-05_10-20-30_004_Desk.png") return 1;
    if (SanitizeFilenamePrefix(L"a/b:c") != L"a_b_c") return 2;
    const auto one=std::filesystem::path(MakeChronologicalMediaPath(L"test",L"One",L".png")).filename().wstring();
    const auto two=std::filesystem::path(MakeChronologicalMediaPath(L"test",L"Two",L".png")).filename().wstring();
    if (one >= two || !HasChronologicalMediaFilename(one)) return 3;
    std::wstring localized;
    return ExpandLocalizationTemplate(L"Saved {0}",L"{0} saved",L"Saved image",localized) && localized==L"image saved" ? 0 : 4;
}
