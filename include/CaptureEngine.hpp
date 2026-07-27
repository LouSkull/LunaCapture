#pragma once

#include "CoreUtils.hpp"
#include "Types.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class CaptureEngine {
public:
    explicit CaptureEngine(HWND notifyWindow);
    ~CaptureEngine();

    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    static std::vector<OutputInfo> EnumerateOutputs();

    bool Start(const AppSettings& settings);
    void Stop();
    bool PauseForRecording();
    void ResumeAfterRecording(const AppSettings& settings);
    void UpdateSettings(const AppSettings& settings);
    void NotifyDisplayChange();
    bool RequestCapture(CaptureMode mode, HMONITOR preferredMonitor, HWND targetWindow,
                        const std::wstring& filenamePrefix, const AppSettings& settings);
    bool RequestPreview(HMONITOR preferredMonitor);
    void SetPreviewConsumerActive(bool active);
    void AcknowledgePreview() noexcept { previewInFlight_.store(false, std::memory_order_release); }
    void RecyclePreviewFrame(std::unique_ptr<PreviewFrame> frame);
    void SaveEditedFrame(FrameBuffer frame, const AppSettings& settings, bool copiedToClipboard, const std::wstring& outputPath, const std::wstring& filenamePrefix);
    bool IsReady() const noexcept { return ready_.load(std::memory_order_acquire); }

private:
    struct DuplicatorState {
        OutputInfo info;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        Microsoft::WRL::ComPtr<IDXGIOutput1> output;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> latestTexture;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTexture;
        D3D11_TEXTURE2D_DESC frameDesc{};
        BgraPreviewScaler previewScaler;
        ULONGLONG stagingLastUsed = 0;
        bool hasFrame = false;
    };

    void CaptureThreadMain();
    void SaveThreadMain();
    bool InitializeAllDuplicators(std::wstring& error);
    bool InitializeDuplicator(DuplicatorState& state, std::wstring& error);
    void ResetDuplicators();
    bool PumpOneFrame(DuplicatorState& state, DWORD timeoutMs, std::wstring& error);
    bool EnsureFrameTextures(DuplicatorState& state, const D3D11_TEXTURE2D_DESC& sourceDesc, std::wstring& error);
    bool EnsureStagingTexture(DuplicatorState& state, std::wstring& error);
    void ReleaseIdleStagingTextures();
    void ProcessPendingRequests();
    void ProcessPreviewRequest();
    void ProcessCaptureRequest(CaptureRequest request);
    bool ReadStateToCpu(DuplicatorState& state, FrameBuffer& frame, std::wstring& error, bool queueCopy = true);
    bool BuildCombinedFrame(FrameBuffer& frame, std::wstring& error);
    DuplicatorState* FindState(HMONITOR monitor);
    std::wstring BuildOutputPath(const AppSettings& settings, const std::wstring& filenamePrefix) const;
    void PostEngineEvent(EngineEvent event) const;
    void QueueSave(SaveJob job);
    void SetReady(bool ready, const std::wstring& detail = L"");
    std::unique_ptr<PreviewFrame> TakePreviewFrame();

    HWND notifyWindow_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    HANDLE wakeEvent_ = nullptr;
    HANDLE pausedEvent_ = nullptr;
    std::thread captureThread_;
    std::thread saveThread_;

    mutable std::mutex stateMutex_;
    AppSettings activeSettings_;
    std::deque<CaptureRequest> requests_;
    HMONITOR previewMonitor_ = nullptr;
    bool previewPending_ = false;
    std::atomic<bool> previewInFlight_{false};
    std::atomic<bool> previewConsumerActive_{false};
    std::mutex previewPoolMutex_;
    std::unique_ptr<PreviewFrame> recycledPreview_;
    std::uint64_t nextRequestId_ = 1;

    std::mutex saveMutex_;
    std::condition_variable saveCv_;
    std::deque<SaveJob> saveQueue_;
    bool saveStopping_ = false;

    std::vector<std::unique_ptr<DuplicatorState>> duplicators_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> pausedForRecording_{false};
    std::atomic<bool> displayChangePending_{false};
    LARGE_INTEGER qpcFrequency_{};
};
