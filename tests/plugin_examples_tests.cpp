// Every plugin bundled in plugins/ must parse cleanly with the shipping
// LunaScript parser. Without this, a language or validation change silently
// rots the starter plugins users are told to copy.
#include "LunaScript.hpp"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef LUNA_PLUGIN_EXAMPLES_DIR
#error "LUNA_PLUGIN_EXAMPLES_DIR must point at the bundled plugins directory"
#endif

namespace {
bool ReadUtf8File(const std::filesystem::path& path, std::wstring& text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    if (needed <= 0) return false;
    text.assign(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()),
                        text.data(), needed);
    return true;
}
}  // namespace

int main() {
    const std::filesystem::path directory(LUNA_PLUGIN_EXAMPLES_DIR);
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        std::wcerr << L"Bundled plugin directory is missing: " << directory.wstring() << L"\n";
        return 2;
    }

    int examined = 0;
    int failures = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != L".luna") continue;
        ++examined;

        std::wstring source;
        if (!ReadUtf8File(entry.path(), source)) {
            std::wcerr << entry.path().filename().wstring() << L": not readable as UTF-8\n";
            ++failures;
            continue;
        }

        LunaPluginDefinition plugin;
        std::vector<LunaDiagnostic> diagnostics;
        const bool parsed = LunaScript::Parse(source, plugin, diagnostics);
        if (!parsed || !diagnostics.empty()) {
            std::wcerr << entry.path().filename().wstring() << L": rejected by the parser\n";
            for (const auto& diagnostic : diagnostics) {
                std::wcerr << L"    line " << diagnostic.line << L" column " << diagnostic.column
                           << L": " << diagnostic.message << L"\n";
            }
            ++failures;
            continue;
        }
        if (plugin.id.empty()) {
            std::wcerr << entry.path().filename().wstring() << L": parsed without a plugin id\n";
            ++failures;
        }
    }

    if (examined == 0) {
        std::wcerr << L"No .luna files were found in " << directory.wstring() << L"\n";
        return 3;
    }
    if (failures != 0) {
        std::wcerr << failures << L" bundled plugin(s) failed to parse\n";
        return 1;
    }
    std::wcout << L"Bundled plugin examples passed (" << examined << L" file(s))\n";
    return 0;
}
