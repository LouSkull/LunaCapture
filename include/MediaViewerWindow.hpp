#pragma once

#include <Windows.h>

#include <string>

// A Media Foundation video surface embedded as a child window inside the
// main app window (the Photos/Videos detail view owns layout/chrome; this
// class only owns playback). Not used for photos — those are decoded
// straight into the app's own backbuffer, no window of their own needed.
class MediaViewerWindow {
public:
    MediaViewerWindow();
    ~MediaViewerWindow();

    MediaViewerWindow(const MediaViewerWindow&) = delete;
    MediaViewerWindow& operator=(const MediaViewerWindow&) = delete;

    // parentEventWindow receives WM_APP_MEDIA_STATE_CHANGED whenever playback
    // state changes, so the host can repaint its own play/pause chrome.
    bool CreateHost(HINSTANCE instance, HWND parentEventWindow);
    void DestroyHost();

    bool LoadVideo(const std::wstring& path, std::wstring& error);
    void UnloadVideo();
    bool HasVideo() const noexcept;

    void SetBounds(const RECT& rect);
    void SetVisible(bool visible);
    HWND Handle() const noexcept;

    // True fullscreen: detaches the video surface from the app window and blows it
    // up to cover the whole monitor that `ownerWindow` sits on, with a floating
    // "Exit fullscreen" button on top. Leaving fullscreen re-parents it back; the
    // caller then restores the normal bounds with SetBounds().
    void SetFullscreen(bool enable, HWND ownerWindow);
    bool IsFullscreen() const noexcept;

    void TogglePlayback();
    void Play();
    void Pause();
    void SkipBy(int deltaSeconds);
    void SetPositionRatio(double ratio);
    void SetVolume(float value);
    void ToggleMute();

    bool IsPlaying() const noexcept;
    bool IsMuted() const noexcept;
    float Volume() const noexcept;
    LONGLONG PositionValue() const;
    LONGLONG DurationValue() const;
    const std::wstring& StatusText() const noexcept;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
