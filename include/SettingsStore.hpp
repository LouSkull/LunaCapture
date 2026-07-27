#pragma once

#include "Types.hpp"

#include <string>

class SettingsStore {
public:
    SettingsStore();

    AppSettings Load() const;
    bool Save(const AppSettings& settings) const;
    bool ApplyAutostart(bool enabled) const;
    const std::wstring& Path() const noexcept { return path_; }

private:
    std::wstring path_;
};
