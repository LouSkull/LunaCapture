#pragma once

#include <string>
#include <string_view>

// Locale entries normally use exact-string matching. A source containing
// positional placeholders ("{0}" through "{9}") is also treated as a bounded
// template so dynamic host text can be localized without translating user data.
// Example: "Monitor {0} replay saved" -> "Replay monitoru {0} ulozen".
bool HasLocalizationPlaceholders(std::wstring_view pattern) noexcept;
bool ExpandLocalizationTemplate(std::wstring_view pattern, std::wstring_view replacement,
                                std::wstring_view source, std::wstring& output);
