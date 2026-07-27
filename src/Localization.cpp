#include "Localization.hpp"

#include <array>
#include <cstddef>

namespace {
bool PlaceholderAt(std::wstring_view text, std::size_t position, std::size_t& index) noexcept {
    if (position + 2 >= text.size() || text[position] != L'{' || text[position + 2] != L'}' ||
        text[position + 1] < L'0' || text[position + 1] > L'9') return false;
    index = static_cast<std::size_t>(text[position + 1] - L'0');
    return true;
}
}

bool HasLocalizationPlaceholders(std::wstring_view pattern) noexcept {
    for (std::size_t position = 0; position < pattern.size(); ++position) {
        std::size_t index = 0;
        if (PlaceholderAt(pattern, position, index)) return true;
    }
    return false;
}

bool ExpandLocalizationTemplate(std::wstring_view pattern, std::wstring_view replacement,
                                std::wstring_view source, std::wstring& output) {
    output.clear();
    std::array<std::wstring, 10> captures;
    std::array<bool, 10> captured{};
    std::size_t patternPosition = 0;
    std::size_t sourcePosition = 0;
    bool foundPlaceholder = false;

    while (patternPosition < pattern.size()) {
        std::size_t placeholderIndex = 0;
        if (!PlaceholderAt(pattern, patternPosition, placeholderIndex)) {
            if (sourcePosition >= source.size() || source[sourcePosition] != pattern[patternPosition]) return false;
            ++patternPosition;
            ++sourcePosition;
            continue;
        }

        foundPlaceholder = true;
        patternPosition += 3;
        const std::size_t nextPlaceholder = pattern.find(L'{', patternPosition);
        std::size_t literalEnd = nextPlaceholder;
        while (literalEnd != std::wstring_view::npos) {
            std::size_t ignored = 0;
            if (PlaceholderAt(pattern, literalEnd, ignored)) break;
            literalEnd = pattern.find(L'{', literalEnd + 1);
        }
        if (literalEnd == std::wstring_view::npos) literalEnd = pattern.size();
        const std::wstring_view followingLiteral = pattern.substr(patternPosition, literalEnd - patternPosition);

        std::size_t captureEnd = source.size();
        if (!followingLiteral.empty()) {
            captureEnd = source.find(followingLiteral, sourcePosition);
            if (captureEnd == std::wstring_view::npos) return false;
        } else if (literalEnd < pattern.size()) {
            // Adjacent placeholders are ambiguous and intentionally unsupported.
            return false;
        }

        const std::wstring capture(source.substr(sourcePosition, captureEnd - sourcePosition));
        if (captured[placeholderIndex] && captures[placeholderIndex] != capture) return false;
        captures[placeholderIndex] = capture;
        captured[placeholderIndex] = true;
        sourcePosition = captureEnd;
    }

    if (!foundPlaceholder || sourcePosition != source.size()) return false;

    output.reserve(replacement.size() + source.size());
    for (std::size_t position = 0; position < replacement.size();) {
        std::size_t placeholderIndex = 0;
        if (PlaceholderAt(replacement, position, placeholderIndex) && captured[placeholderIndex]) {
            output += captures[placeholderIndex];
            position += 3;
        } else {
            output.push_back(replacement[position]);
            ++position;
        }
    }
    return true;
}
