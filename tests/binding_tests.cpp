#include "BindingRecorder.hpp"
#include "Types.hpp"
#include <iostream>

int main() {
    int failures = 0;
    const auto expect = [&](bool condition, const char* description) {
        if (!condition) { std::cerr << description << '\n'; ++failures; }
    };
    KeyboardPressState keys;
    expect(keys.Feed(VK_SNAPSHOT, true), "Release-only Print Screen is recognized");
    expect(keys.Feed('K', false), "A shortcut is recognized directly on key-down");
    expect(!keys.Feed('K', false), "Auto-repeat is ignored");
    expect(!keys.Feed('K', true), "Release does not trigger a second shortcut");
    expect(keys.Feed('K', false), "A new press of the same shortcut is recognized");
    expect(keys.Feed(VK_SNAPSHOT, false), "Print Screen with a down event is recognized");
    expect(!keys.Feed(VK_SNAPSHOT, true), "Paired Print Screen release is ignored");
    expect(keys.Feed(VK_RCONTROL, false) && keys.Down(VK_RCONTROL), "Raw Input modifier stays down");
    expect(!keys.Feed(VK_RCONTROL, true) && !keys.Down(VK_RCONTROL), "Raw Input modifier resets on release");
    expect(!keys.Feed(VK_PACKET, false) && !keys.Feed(255, false), "Pseudo keys are ignored");
    expect(!AppSettings{}.privateCapture, "Normal capture is visible by default");
    if (failures) return 1;
    std::cout << "Keyboard press tests passed\n";
    return 0;
}
