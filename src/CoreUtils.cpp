#include "CoreUtils.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cwctype>

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
