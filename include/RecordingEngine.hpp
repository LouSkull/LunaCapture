#pragma once

#include "Types.hpp"

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

class RecordingEngine {
public:
    explicit RecordingEngine(HWND notifyWindow, int channelId = 0);
    ~RecordingEngine();

    RecordingEngine(const RecordingEngine&) = delete;
    RecordingEngine& operator=(const RecordingEngine&) = delete;

    static std::vector<AudioDeviceInfo> EnumerateMicrophones();
    static float QueryMicrophonePeak(const std::wstring& deviceId);
    static AudioMeterState QueryMicrophoneMeter(const std::wstring& deviceId);

    bool Start();
    void Stop();
    bool StartRecording(const AppSettings& settings, std::uint64_t* sessionGeneration = nullptr);
    bool StartReplayBuffer(const AppSettings& settings, std::uint64_t* sessionGeneration = nullptr);
    void StopActiveSession();
    void SaveReplay();
    void NotifyDisplayChange();
    bool CaptureScreenshot(const AppSettings& settings);
    bool RequestFrame(CaptureMode mode, std::uint64_t requestId, bool drawCursor);
    void SetPreviewConsumerActive(bool active);
    void SetPreviewFrameRate(int fps) noexcept;
    void AcknowledgePreview() noexcept { previewInFlight_.store(false, std::memory_order_release); }
    void RecyclePreviewFrame(std::unique_ptr<PreviewFrame> frame);

    bool IsRecording() const noexcept { return recording_.load(std::memory_order_acquire); }
    bool IsReplayBuffering() const noexcept { return replay_.load(std::memory_order_acquire); }
    bool IsBusy() const noexcept { return busy_.load(std::memory_order_acquire); }

private:
    enum class CommandType { StartRecording, StartReplay, Stop, SaveReplay, DisplayChanged, Screenshot, FrameSnapshot, Shutdown };
    struct Command {
        CommandType type = CommandType::Stop;
        AppSettings settings;
        CaptureMode captureMode = CaptureMode::ActiveDisplay;
        std::uint64_t requestId = 0;
        bool drawCursor = false;
        std::uint64_t sessionGeneration = 0;
    };

    bool Queue(Command command);
    void ThreadMain();
    void PostEvent(RecordingEvent event) const;
    void PostCaptureEvent(EngineEvent event) const;
    std::unique_ptr<PreviewFrame> TakePreviewFrame();

    HWND notifyWindow_ = nullptr;
    int channelId_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Command> commands_;
    bool running_ = false;
    std::atomic<bool> recording_{false};
    std::atomic<bool> replay_{false};
    std::atomic<bool> busy_{false};
    std::atomic<bool> previewConsumerActive_{false};
    std::atomic<int> previewFps_{30};
    std::atomic<bool> previewInFlight_{false};
    std::atomic<std::uint64_t> nextSessionGeneration_{1};
    std::mutex previewPoolMutex_;
    std::unique_ptr<PreviewFrame> recycledPreview_;
};

[[nodiscard]] constexpr bool RecordingEventMatchesSession(const RecordingEvent& event,
                                                          std::uint64_t sessionGeneration) noexcept {
    return sessionGeneration != 0 && event.sessionGeneration == sessionGeneration;
}
