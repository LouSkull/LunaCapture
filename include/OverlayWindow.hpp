#pragma once

#include <Windows.h>
#include <string>

class OverlayWindow {
public:
    OverlayWindow() = default;
    ~OverlayWindow();

    bool Create(HINSTANCE instance);
    void Destroy();
    void ShowSuccess(const std::wstring& filename, double responseMs, int durationMs, HMONITOR monitor);
    void ShowError(const std::wstring& message, int durationMs, HMONITOR monitor);
    void ShowMessage(const std::wstring& title, const std::wstring& detail, int durationMs, HMONITOR monitor);
    HWND Handle() const noexcept { return hwnd_; }
    bool IsFullscreenPresentation() const noexcept { return fullscreenPresentation_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void BeginShow(bool success, std::wstring title, std::wstring detail, double responseMs, int durationMs,
                   HMONITOR monitor);
    void Paint();
    bool CreateGraphics();
    void DestroyGraphics();
    void Tick();
    void Hide();

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    std::wstring title_;
    std::wstring detail_;
    double responseMs_ = 0.0;
    bool success_ = true;
    ULONGLONG startedAt_ = 0;
    int holdDurationMs_ = 1200;
    int width_ = 336;
    int height_ = 68;
    POINT basePosition_{};
    HDC backBufferDc_ = nullptr;
    HBITMAP backBufferBitmap_ = nullptr;
    HGDIOBJ backBufferOldBitmap_ = nullptr;
    void* backBufferPixels_ = nullptr;
    BYTE opacity_ = 255;
    bool fullscreenPresentation_ = false;
    HFONT titleFont_ = nullptr;
    HFONT detailFont_ = nullptr;
};
