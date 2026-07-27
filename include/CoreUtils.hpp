#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

std::wstring SanitizeFilenamePrefix(const std::wstring& value, const std::wstring& fallback = L"Desktop");
std::wstring MakeScreenshotFilename(const std::wstring& prefix, int year, int month, int day, int hour, int minute, int second, int millis, bool jpeg);
std::wstring FileNameOnly(const std::wstring& path);
std::wstring EllipsizeMiddle(const std::wstring& value, std::size_t maxChars);
double QpcMilliseconds(std::int64_t start, std::int64_t end, std::int64_t frequency);
