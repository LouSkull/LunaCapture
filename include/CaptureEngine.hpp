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
    struct WorkCounters {
        std::uint64_t acquireAttempts = 0;
        std::uint64_t frameCopies = 0;
    };

    explicit CaptureEngine(HWND notifyWindow);
    ~CaptureEngine();

    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    static std::vector<OutputInfo> EnumerateOutputs();

    bool Start(const AppSettings& settings);
    void Stop();
    void UpdateSettings(const AppSettings& settings);
    void NotifyDisplayChange();
    // Drop any full-desktop overlay image cached by DXGI before another region
    // or window-picker request can reuse it.
    void InvalidateOverlayFrames();
    bool RequestCapture(CaptureMode mode, HMONITOR preferredMonitor, HWND targetWindow,
                        const std::wstring& filenamePrefix, const AppSettings& settings);
    void FinishEditedFrame(FrameBuffer frame, const AppSettings& settings, RegionResultAction action,
                           const std::wstring& outputPath, const std::wstring& filenamePrefix);
    bool IsReady() const noexcept { return ready_.load(std::memory_order_acquire); }
    WorkCounters GetWorkCounters() const noexcept {
        return {acquireAttempts_.load(std::memory_order_relaxed),
                frameCopies_.load(std::memory_order_relaxed)};
    }

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
    void ProcessPendingRequests(std::size_t maximumRequests);
    void ProcessCaptureRequest(CaptureRequest request);
    bool ReadStateToCpu(DuplicatorState& state, FrameBuffer& frame, std::wstring& error, bool queueCopy = true);
    bool BuildCombinedFrame(FrameBuffer& frame, std::wstring& error);
    DuplicatorState* FindState(HMONITOR monitor);
    std::wstring BuildOutputPath(const AppSettings& settings, const std::wstring& filenamePrefix) const;
    void PostEngineEvent(EngineEvent event) const;
    void QueueSave(SaveJob job);
    void SetReady(bool ready, const std::wstring& detail = L"");
    bool HasPendingWork() const;
    void FailPendingWork(const std::wstring& error);

    HWND notifyWindow_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    HANDLE wakeEvent_ = nullptr;
    std::thread captureThread_;
    std::thread saveThread_;

    mutable std::mutex stateMutex_;
    AppSettings activeSettings_;
    std::deque<CaptureRequest> requests_;
    std::uint64_t nextRequestId_ = 1;

    std::mutex saveMutex_;
    std::condition_variable saveCv_;
    std::deque<SaveJob> saveQueue_;
    bool saveStopping_ = false;

    std::vector<std::unique_ptr<DuplicatorState>> duplicators_;
    std::size_t expectedOutputCount_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> displayChangePending_{false};
    std::atomic<bool> overlayInvalidationPending_{false};
    std::atomic<std::uint64_t> acquireAttempts_{0};
    std::atomic<std::uint64_t> frameCopies_{0};
    LARGE_INTEGER qpcFrequency_{};
};
