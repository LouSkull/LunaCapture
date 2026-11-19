#pragma once

#include <Windows.h>
#include <array>

class KeyboardPressState {
public:
    // Some Print Screen keys emit only a release event.
    bool Feed(UINT key, bool released) noexcept {
        if (!key || key >= 255 || key == VK_PACKET) return false;
        const bool wasDown = down_[key];
        down_[key] = !released;
        return !wasDown && (!released || key == VK_SNAPSHOT);
    }

    bool Down(UINT key) const noexcept { return down_[key]; }

private:
    std::array<bool, 256> down_{};
};
