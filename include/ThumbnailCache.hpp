#pragma once

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

// Decodes a small preview bitmap for a photo or video file on a background
// thread (WIC for images, an IMFSourceReader frame grab for videos) so the
// media library grid never blocks the UI while it fills in. Results are
// delivered as a heap-allocated ThumbnailResult* posted via readyMessage;
// the receiver takes ownership and must delete it.
struct ThumbnailResult {
    std::filesystem::path path;
    bool isVideo = false;
    std::vector<std::uint8_t> bgra;  // top-down, premultiplied-free straight BGRA, stride = width * 4
    UINT width = 0;
    UINT height = 0;
};

class ThumbnailCache {
public:
    ~ThumbnailCache();

    bool Start(HWND notifyWindow, UINT readyMessage);
    void Stop();

    // Safe to call repeatedly for the same path; duplicate pending requests are skipped.
    void Request(const std::filesystem::path& path, bool isVideo);

private:
    void WorkerMain();
    bool DecodePhoto(const std::filesystem::path& path, ThumbnailResult& result) const;
    bool DecodeVideoFrame(const std::filesystem::path& path, ThumbnailResult& result) const;

    HWND notifyWindow_ = nullptr;
    UINT readyMessage_ = 0;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<std::filesystem::path, bool>> queue_;
    std::atomic<bool> stopping_{false};
};
