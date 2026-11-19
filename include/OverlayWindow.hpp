#pragma once

#include <Windows.h>
#include <string>

// Visual status of a toast: green check, amber exclamation triangle, or red cross.
enum class OverlayStatus { Success, Warning, Error };

class OverlayWindow {
public:
    OverlayWindow() = default;
    ~OverlayWindow();

    bool Create(HINSTANCE instance);
    void Destroy();
    void ShowSuccess(const std::wstring& filename, int durationMs, HMONITOR monitor);
    void ShowError(const std::wstring& message, int durationMs, HMONITOR monitor);
    void ShowMessage(const std::wstring& title, const std::wstring& detail, int durationMs, HMONITOR monitor);
    void ShowWarning(const std::wstring& title, const std::wstring& detail, int durationMs, HMONITOR monitor);
    void RefreshTheme();
    bool SetCapturePrivacy(bool enabled, std::wstring& error);
    void Dismiss() { Hide(); }
    HWND Handle() const noexcept { return hwnd_; }
    bool IsFullscreenPresentation() const noexcept { return fullscreenPresentation_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void BeginShow(OverlayStatus status, std::wstring title, std::wstring detail, int durationMs,
                   HMONITOR monitor);
    void Paint();
    bool CreateGraphics();
    void DestroyGraphics();
    bool RecreateFonts();
    void ConfigurePresentationMode(bool fullscreen);
    void ApplyOpaqueWindowRegion();
    void Hide();

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    std::wstring title_;
    std::wstring detail_;
    OverlayStatus status_ = OverlayStatus::Success;
    ULONGLONG expiresAt_ = 0;
    int width_ = 326;
    int height_ = 66;
    HDC backBufferDc_ = nullptr;
    HBITMAP backBufferBitmap_ = nullptr;
    HGDIOBJ backBufferOldBitmap_ = nullptr;
    void* backBufferPixels_ = nullptr;
    bool fullscreenPresentation_ = false;
    bool privateCapture_ = false;
    bool opaqueFullscreenFallback_ = false;
    HFONT titleFont_ = nullptr;
    HFONT detailFont_ = nullptr;
};
