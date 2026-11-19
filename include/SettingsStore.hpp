#pragma once

#include "Types.hpp"

#include <string>

class SettingsStore {
public:
    enum class Access { ReadWrite, ReadOnly };

    SettingsStore();
    explicit SettingsStore(Access access);
    explicit SettingsStore(std::wstring path);

    AppSettings Load() const;
    bool Save(const AppSettings& settings) const;
    bool ApplyAutostart(bool enabled) const;
    const std::wstring& Path() const noexcept { return path_; }

private:
    std::wstring path_;
    bool readOnly_ = false;
};
