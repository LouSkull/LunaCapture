#pragma once
#include <Windows.h>

// Direct positioning keeps the window moving even with DRAGFULLWINDOWS off.
// Screen coordinates avoid feedback as the client origin moves under the mouse.
class WindowDrag {
public:
    bool Active() const { return active_; }
    void Begin(HWND window, POINT cursor) {
        window_ = window;
        start_ = cursor;
        GetWindowRect(window_, &original_);
        SetCapture(window_);
        active_ = GetCapture() == window_;
    }
    void Move(POINT cursor) {
        if (active_) SetWindowPos(window_, nullptr, original_.left + cursor.x - start_.x,
                                 original_.top + cursor.y - start_.y, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void End(bool restore = false) {
        if (!active_) return;
        active_ = false;
        if (restore) SetWindowPos(window_, nullptr, original_.left, original_.top, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (GetCapture() == window_) ReleaseCapture();
    }
    void CaptureLost() { active_ = false; }
private:
    HWND window_ = nullptr;
    bool active_ = false;
    POINT start_{};
    RECT original_{};
};
