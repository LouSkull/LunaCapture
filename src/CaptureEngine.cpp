#include "CaptureEngine.hpp"

#include "CoreUtils.hpp"
#include "WicEncoder.hpp"
#include "WinUtil.hpp"

#include <Windows.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <new>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {
std::wstring HResultText(HRESULT hr) {
    wchar_t* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD count = FormatMessageW(flags, nullptr, static_cast<DWORD>(hr), 0,
                                       reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result;
    if (count && message) {
        result.assign(message, count);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) {
            result.pop_back();
        }
    } else {
        wchar_t hex[32]{};
        swprintf_s(hex, L"0x%08X", static_cast<unsigned int>(hr));
        result = hex;
    }
    if (message) LocalFree(message);
    return result;
}

std::int64_t QpcNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

std::vector<OutputInfo> EnumerateOutputsInternal() {
    std::vector<OutputInfo> outputs;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return outputs;

    int linear = 0;
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);
        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) continue;
            OutputInfo info;
            info.linearIndex = linear++;
            info.adapterIndex = static_cast<int>(adapterIndex);
            info.outputIndex = static_cast<int>(outputIndex);
            info.monitor = desc.Monitor;
            info.desktopRect = desc.DesktopCoordinates;
            info.width = static_cast<UINT>(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
            info.height = static_cast<UINT>(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
            info.name = desc.DeviceName;
            info.adapterName = adapterDesc.Description;
            outputs.push_back(std::move(info));
        }
    }
    return outputs;
}

bool CropFrameToDesktopRect(FrameBuffer& frame, const RECT& cropRect, std::wstring& error) {
    RECT clipped{};
    clipped.left = (std::max)(cropRect.left, frame.virtualRect.left);
    clipped.top = (std::max)(cropRect.top, frame.virtualRect.top);
    clipped.right = (std::min)(cropRect.right, frame.virtualRect.right);
    clipped.bottom = (std::min)(cropRect.bottom, frame.virtualRect.bottom);
    if (clipped.right <= clipped.left || clipped.bottom <= clipped.top) {
        error = L"The active window is outside the captured desktop";
        return false;
    }
    const UINT width = static_cast<UINT>(clipped.right - clipped.left);
    const UINT height = static_cast<UINT>(clipped.bottom - clipped.top);
    const UINT stride = width * 4;
    std::vector<std::uint8_t> cropped(static_cast<std::size_t>(stride) * height);
    const UINT sourceX = static_cast<UINT>(clipped.left - frame.virtualRect.left);
    const UINT sourceY = static_cast<UINT>(clipped.top - frame.virtualRect.top);
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(cropped.data() + static_cast<std::size_t>(y) * stride,
                    frame.bgra.data() + static_cast<std::size_t>(sourceY + y) * frame.stride + sourceX * 4,
                    stride);
    }
    frame.bgra = std::move(cropped);
    frame.width = width;
    frame.height = height;
    frame.stride = stride;
    frame.virtualRect = clipped;
    return true;
}
}  // namespace

CaptureEngine::CaptureEngine(HWND notifyWindow) : notifyWindow_(notifyWindow) {
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    QueryPerformanceFrequency(&qpcFrequency_);
}

CaptureEngine::~CaptureEngine() {
    Stop();
    if (wakeEvent_) CloseHandle(wakeEvent_);
    if (stopEvent_) CloseHandle(stopEvent_);
}

std::vector<OutputInfo> CaptureEngine::EnumerateOutputs() { return EnumerateOutputsInternal(); }

bool CaptureEngine::Start(const AppSettings& settings) {
    if (running_.exchange(true)) return true;
    ready_.store(false, std::memory_order_release);
    displayChangePending_.store(false, std::memory_order_release);
    overlayInvalidationPending_.store(false, std::memory_order_release);
    acquireAttempts_.store(0, std::memory_order_relaxed);
    frameCopies_.store(0, std::memory_order_relaxed);
    ResetEvent(stopEvent_);
    ResetEvent(wakeEvent_);
    {
        std::lock_guard lock(stateMutex_);
        activeSettings_ = settings;
        requests_.clear();
    }
    {
        std::lock_guard lock(saveMutex_);
        saveStopping_ = false;
        saveQueue_.clear();
    }
    captureThread_ = std::thread(&CaptureEngine::CaptureThreadMain, this);
    saveThread_ = std::thread(&CaptureEngine::SaveThreadMain, this);
    return true;
}

void CaptureEngine::Stop() {
    if (!running_.exchange(false)) return;
    ready_.store(false, std::memory_order_release);
    SetEvent(stopEvent_);
    SetEvent(wakeEvent_);
    if (captureThread_.joinable()) captureThread_.join();
    {
        std::lock_guard lock(saveMutex_);
        saveStopping_ = true;
    }
    saveCv_.notify_all();
    if (saveThread_.joinable()) saveThread_.join();
    ResetDuplicators();
}

void CaptureEngine::UpdateSettings(const AppSettings& settings) {
    std::lock_guard lock(stateMutex_);
    activeSettings_ = settings;
}

void CaptureEngine::NotifyDisplayChange() {
    if (!running_.load(std::memory_order_acquire)) return;
    displayChangePending_.store(true, std::memory_order_release);
    SetEvent(wakeEvent_);
}

void CaptureEngine::InvalidateOverlayFrames() {
    if (!running_.load(std::memory_order_acquire)) return;
    overlayInvalidationPending_.store(true, std::memory_order_release);
    SetEvent(wakeEvent_);
}

bool CaptureEngine::RequestCapture(CaptureMode mode, HMONITOR preferredMonitor, HWND targetWindow,
                                   const std::wstring& filenamePrefix, const AppSettings& settings) {
    if (!running_.load()) return false;
    CaptureRequest request;
    request.settings = settings;
    request.mode = mode;
    request.preferredMonitor = preferredMonitor;
    request.targetWindow = targetWindow;
    request.filenamePrefix = SanitizeFilenamePrefix(filenamePrefix);
    request.requestedQpc = QpcNow();
    {
        std::lock_guard lock(stateMutex_);
        if (requests_.size() >= 8) return false;
        request.id = nextRequestId_++;
        requests_.push_back(std::move(request));
    }
    SetEvent(wakeEvent_);
    return true;
}

void CaptureEngine::FinishEditedFrame(FrameBuffer frame, const AppSettings& settings, RegionResultAction action,
                                      const std::wstring& outputPath, const std::wstring& filenamePrefix) {
    if (frame.bgra.empty()) return;
    SaveJob job;
    {
        std::lock_guard lock(stateMutex_);
        job.id = nextRequestId_++;
    }
    if (action != RegionResultAction::Copy)
        job.path = outputPath.empty() ? BuildOutputPath(settings, filenamePrefix) : outputPath;
    job.copyToClipboard = action == RegionResultAction::Copy || action == RegionResultAction::SaveAndCopy;
    job.bgra = std::move(frame.bgra);
    job.width = frame.width;
    job.height = frame.height;
    job.stride = frame.stride;
    job.format = settings.imageFormat;
    job.outputWidth = settings.outputWidth;
    job.outputHeight = settings.outputHeight;
    job.jpegQuality = settings.jpegQuality;
    QueueSave(std::move(job));
}

void CaptureEngine::SetReady(bool ready, const std::wstring& detail) {
    ready_.store(ready, std::memory_order_release);
    EngineEvent event;
    event.type = ready ? EngineEventType::Ready : EngineEventType::Reconfiguring;
    event.detail = detail;
    PostEngineEvent(std::move(event));
}

bool CaptureEngine::HasPendingWork() const {
    std::lock_guard lock(stateMutex_);
    return !requests_.empty();
}

void CaptureEngine::FailPendingWork(const std::wstring& error) {
    std::deque<CaptureRequest> failedRequests;
    {
        std::lock_guard lock(stateMutex_);
        failedRequests.swap(requests_);
    }

    if (failedRequests.empty()) {
        return;
    }
    for (const auto& request : failedRequests) {
        EngineEvent event;
        event.type = EngineEventType::Error;
        event.mode = request.mode;
        event.detail = error;
        PostEngineEvent(std::move(event));
    }
}

void CaptureEngine::CaptureThreadMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetReady(false, L"Idle; capture starts on demand");

    const HANDLE waitHandles[] = {stopEvent_, wakeEvent_};
    bool idleResourceReleaseArmed = false;
    for (;;) {
        const DWORD waitResult = WaitForMultipleObjects(
            static_cast<DWORD>(std::size(waitHandles)), waitHandles, FALSE,
            idleResourceReleaseArmed ? 5000 : INFINITE);
        if (waitResult == WAIT_OBJECT_0) break;
        if (waitResult == WAIT_TIMEOUT) {
            // A genuinely idle engine releases its GPU textures and scarce
            // desktop-duplication slots entirely.
            ResetDuplicators();
            SetReady(false, L"Idle; capture hardware released");
            idleResourceReleaseArmed = false;
            continue;
        }
        if (waitResult != WAIT_OBJECT_0 + 1) break;

        const bool displayChanged = displayChangePending_.exchange(false, std::memory_order_acq_rel);
        const bool overlayInvalidated = overlayInvalidationPending_.exchange(false, std::memory_order_acq_rel);
        if (displayChanged || overlayInvalidated) {
            // Never reuse a frame acquired while the full-desktop region/window-picker
            // surface was visible. Recreating duplication also prevents a queued request
            // from racing the first clean post-overlay compositor frame.
            SetReady(false, displayChanged ? L"Display mode changed; capture will reconnect on demand"
                                           : L"Desktop overlay closed; cached frames were discarded");
            ResetDuplicators();
            idleResourceReleaseArmed = false;
        }

        // Startup and settings changes never touch DXGI. Only an explicit screenshot
        // request connects to the desktop and polls a frame.
        if (!HasPendingWork()) continue;

        bool initializedForRequest = false;
        if (duplicators_.empty()) {
            std::wstring error;
            SetReady(false, L"Connecting displays…");
            if (!InitializeAllDuplicators(error)) {
                if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) break;
                FailPendingWork(error);
                continue;
            }
            initializedForRequest = true;
            SetReady(true, L"All displays are warm and ready");
        }

        bool lost = false;
        std::wstring lostError;
        std::size_t requestBatchSize = 0;
        {
            std::lock_guard lock(stateMutex_);
            requestBatchSize = requests_.size();
        }
        if (!initializedForRequest) {
            for (auto& state : duplicators_) {
                std::wstring error;
                if (!PumpOneFrame(*state, 40, error) && !error.empty()) {
                    lostError = std::move(error);
                    lost = true;
                    break;
                }
            }
        }
        if (lost) {
            SetReady(false, lostError);
            ResetDuplicators();
            idleResourceReleaseArmed = false;
            FailPendingWork(lostError);
            continue;
        }

        ProcessPendingRequests(requestBatchSize);
        ReleaseIdleStagingTextures();
        idleResourceReleaseArmed = true;
    }

    ResetDuplicators();
    if (SUCCEEDED(comResult)) CoUninitialize();
}

void CaptureEngine::SaveThreadMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        SaveJob job;
        {
            std::unique_lock lock(saveMutex_);
            saveCv_.wait(lock, [&] { return saveStopping_ || !saveQueue_.empty(); });
            if (saveStopping_ && saveQueue_.empty()) break;
            job = std::move(saveQueue_.front());
            saveQueue_.pop_front();
        }
        if (job.copyToClipboard) {
            EngineEvent event;
            event.type = EngineEventType::ClipboardImageReady;
            event.path = job.path;
            event.clipboardImage = PrepareClipboardImage(job);
            PostEngineEvent(std::move(event));
            if (job.path.empty()) continue;
        }
        const auto started = QpcNow();
        std::wstring error;
        const bool ok = EncodeScreenshotWic(job, error);
        const auto finished = QpcNow();
        EngineEvent event;
        event.type = ok ? EngineEventType::Saved : EngineEventType::Error;
        event.path = job.path;
        event.responseMs = job.responseMs;
        event.saveMs = QpcMilliseconds(started, finished, qpcFrequency_.QuadPart);
        event.detail = ok ? L"Saved" : error;
        PostEngineEvent(std::move(event));
    }
    CoUninitialize();
}

bool CaptureEngine::InitializeAllDuplicators(std::wstring& error) {
    ResetDuplicators();
    const auto outputs = EnumerateOutputsInternal();
    if (outputs.empty()) {
        error = L"Windows returned no desktop displays";
        return false;
    }
    expectedOutputCount_ = outputs.size();

    for (const auto& output : outputs) {
        auto state = std::make_unique<DuplicatorState>();
        state->info = output;
        std::wstring stateError;
        if (InitializeDuplicator(*state, stateError)) duplicators_.push_back(std::move(state));
    }
    if (duplicators_.empty()) {
        error = L"DXGI could not open any display";
        return false;
    }

    // The first duplication event can contain only pointer metadata. Its texture
    // is not an initialized desktop image. Wait for a real present for this
    // request, with one shared deadline across monitors and no idle polling.
    std::wstring warmFailure;
    const ULONGLONG deadline = GetTickCount64() + 750;
    for (auto& state : duplicators_) {
        std::wstring warmError;
        while (!state->hasFrame && running_.load()) {
            const auto now = GetTickCount64();
            if (now >= deadline) break;
            const auto remaining = deadline - now;
            const DWORD waitMs = static_cast<DWORD>((std::min)(remaining, 50ULL));
            PumpOneFrame(*state, waitMs, warmError);
            if (!warmError.empty()) break;
        }
        if (warmFailure.empty() && !warmError.empty()) warmFailure = std::move(warmError);
    }
    for (const auto& state : duplicators_) {
        if (state->hasFrame) return true;
    }
    error = warmFailure.empty() ? L"DXGI did not deliver a frame for the request" : std::move(warmFailure);
    ResetDuplicators();
    return false;
}

bool CaptureEngine::InitializeDuplicator(DuplicatorState& state, std::wstring& error) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { error = L"CreateDXGIFactory1: " + HResultText(hr); return false; }
    hr = factory->EnumAdapters1(static_cast<UINT>(state.info.adapterIndex), &state.adapter);
    if (FAILED(hr)) { error = L"EnumAdapters1: " + HResultText(hr); return false; }
    ComPtr<IDXGIOutput> outputBase;
    hr = state.adapter->EnumOutputs(static_cast<UINT>(state.info.outputIndex), &outputBase);
    if (FAILED(hr)) { error = L"EnumOutputs: " + HResultText(hr); return false; }
    hr = outputBase.As(&state.output);
    if (FAILED(hr)) { error = L"IDXGIOutput1: " + HResultText(hr); return false; }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                       D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL created{};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    hr = D3D11CreateDevice(state.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                           static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                           &state.device, &created, &state.context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(state.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels + 1,
                               static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION,
                               &state.device, &created, &state.context);
    }
    if (FAILED(hr)) { error = L"D3D11CreateDevice: " + HResultText(hr); return false; }
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(state.output.As(&output5))) {
        constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = output5->DuplicateOutput1(state.device.Get(), 0, static_cast<UINT>(std::size(formats)),
                                       formats, &state.duplication);
    } else {
        hr = E_NOINTERFACE;
    }
    if (FAILED(hr)) hr = state.output->DuplicateOutput(state.device.Get(), &state.duplication);
    if (FAILED(hr)) { error = L"DuplicateOutput: " + HResultText(hr); return false; }
    return true;
}

void CaptureEngine::ResetDuplicators() {
    duplicators_.clear();
    expectedOutputCount_ = 0;
}

bool CaptureEngine::EnsureFrameTextures(DuplicatorState& state, const D3D11_TEXTURE2D_DESC& sourceDesc,
                                         std::wstring& error) {
    if (state.latestTexture && state.frameDesc.Width == sourceDesc.Width &&
        state.frameDesc.Height == sourceDesc.Height && state.frameDesc.Format == sourceDesc.Format) return true;

    D3D11_TEXTURE2D_DESC latest = sourceDesc;
    latest.MipLevels = 1;
    latest.ArraySize = 1;
    latest.SampleDesc.Count = 1;
    latest.SampleDesc.Quality = 0;
    latest.Usage = D3D11_USAGE_DEFAULT;
    latest.BindFlags = 0;
    latest.CPUAccessFlags = 0;
    latest.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> latestTexture;
    HRESULT hr = state.device->CreateTexture2D(&latest, nullptr, &latestTexture);
    if (FAILED(hr)) { error = L"Create GPU texture: " + HResultText(hr); return false; }

    state.latestTexture = std::move(latestTexture);
    state.stagingTexture.Reset();
    state.frameDesc = latest;
    state.stagingLastUsed = 0;
    state.hasFrame = false;
    return true;
}

bool CaptureEngine::EnsureStagingTexture(DuplicatorState& state, std::wstring& error) {
    state.stagingLastUsed = GetTickCount64();
    if (state.stagingTexture) return true;
    if (!state.latestTexture || !state.frameDesc.Width || !state.frameDesc.Height) {
        error = L"Display has no cached frame";
        return false;
    }
    D3D11_TEXTURE2D_DESC staging = state.frameDesc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    const HRESULT hr = state.device->CreateTexture2D(&staging, nullptr, &state.stagingTexture);
    if (FAILED(hr)) {
        error = L"Create staging texture: " + HResultText(hr);
        return false;
    }
    return true;
}

void CaptureEngine::ReleaseIdleStagingTextures() {
    const ULONGLONG now = GetTickCount64();
    for (auto& state : duplicators_) {
        if (state->stagingTexture && state->stagingLastUsed && now - state->stagingLastUsed >= 5000) {
            state->stagingTexture.Reset();
            state->stagingLastUsed = 0;
        }
    }
}

bool CaptureEngine::PumpOneFrame(DuplicatorState& state, DWORD timeoutMs, std::wstring& error) {
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    ComPtr<IDXGIResource> resource;
    acquireAttempts_.fetch_add(1, std::memory_order_relaxed);
    const HRESULT hr = state.duplication->AcquireNextFrame(timeoutMs, &frameInfo, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (hr == DXGI_ERROR_ACCESS_LOST) { error = L"Display access changed; reconnecting DXGI"; return false; }
    if (FAILED(hr)) { error = L"AcquireNextFrame: " + HResultText(hr); return false; }
    struct Guard { IDXGIOutputDuplication* duplication; ~Guard() { if (duplication) duplication->ReleaseFrame(); } }
        guard{state.duplication.Get()};
    // Pointer-only notifications do not carry new desktop pixels. In particular,
    // the first such event may expose an all-zero texture before the first present.
    if (frameInfo.LastPresentTime.QuadPart == 0) return false;
    ComPtr<ID3D11Texture2D> frameTexture;
    if (FAILED(resource.As(&frameTexture))) { error = L"Desktop frame is not a texture"; return false; }
    D3D11_TEXTURE2D_DESC desc{};
    frameTexture->GetDesc(&desc);
    if (!EnsureFrameTextures(state, desc, error)) return false;
    state.context->CopyResource(state.latestTexture.Get(), frameTexture.Get());
    frameCopies_.fetch_add(1, std::memory_order_relaxed);
    state.hasFrame = true;
    return true;
}

CaptureEngine::DuplicatorState* CaptureEngine::FindState(HMONITOR monitor) {
    for (auto& state : duplicators_) if (state->info.monitor == monitor && state->hasFrame) return state.get();
    // A concrete monitor request must never fall back to a different display:
    // that can capture private content from the wrong screen while labelling it
    // as the requested one. Null means the caller explicitly accepts a default.
    if (!monitor) {
        for (auto& state : duplicators_) if (state->hasFrame) return state.get();
    }
    return nullptr;
}

bool CaptureEngine::ReadStateToCpu(DuplicatorState& state, FrameBuffer& frame, std::wstring& error, bool queueCopy) {
    if (!state.hasFrame || !state.latestTexture) { error = L"Display has no cached frame"; return false; }
    if (!EnsureStagingTexture(state, error)) return false;
    if (queueCopy) state.context->CopyResource(state.stagingTexture.Get(), state.latestTexture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = state.context->Map(state.stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) { error = L"Map staging texture: " + HResultText(hr); return false; }
    frame.width = state.frameDesc.Width;
    frame.height = state.frameDesc.Height;
    frame.stride = frame.width * 4;
    frame.virtualRect = state.info.desktopRect;
    frame.bgra.resize(static_cast<std::size_t>(frame.stride) * frame.height);
    const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
    for (UINT y = 0; y < frame.height; ++y) {
        auto* row = frame.bgra.data() + static_cast<std::size_t>(y) * frame.stride;
        std::memcpy(row, source + static_cast<std::size_t>(y) * mapped.RowPitch, frame.stride);
        for (UINT x = 0; x < frame.width; ++x) row[static_cast<std::size_t>(x) * 4 + 3] = 255;
    }
    state.context->Unmap(state.stagingTexture.Get(), 0);
    return true;
}

bool CaptureEngine::BuildCombinedFrame(FrameBuffer& frame, std::wstring& error) {
    if (expectedOutputCount_ == 0 || duplicators_.size() != expectedOutputCount_) {
        error = L"Not every desktop display is available; capture was cancelled instead of saving an incomplete desktop";
        return false;
    }
    std::vector<std::pair<OutputInfo, FrameBuffer>> pieces;
    RECT bounds{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
    for (auto& state : duplicators_) {
        if (!state->hasFrame) {
            error = L"A desktop display has no current frame; capture was cancelled instead of saving an incomplete desktop";
            return false;
        }
        FrameBuffer piece;
        if (!ReadStateToCpu(*state, piece, error)) return false;
        bounds.left = (std::min)(bounds.left, state->info.desktopRect.left);
        bounds.top = (std::min)(bounds.top, state->info.desktopRect.top);
        bounds.right = (std::max)(bounds.right, state->info.desktopRect.right);
        bounds.bottom = (std::max)(bounds.bottom, state->info.desktopRect.bottom);
        pieces.emplace_back(state->info, std::move(piece));
    }
    if (pieces.empty() || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        error = L"No display frame is available";
        return false;
    }
    frame.width = static_cast<UINT>(bounds.right - bounds.left);
    frame.height = static_cast<UINT>(bounds.bottom - bounds.top);
    frame.stride = frame.width * 4;
    frame.virtualRect = bounds;
    frame.bgra.assign(static_cast<std::size_t>(frame.stride) * frame.height, 0);
    for (std::size_t i = 3; i < frame.bgra.size(); i += 4) frame.bgra[i] = 255;
    for (const auto& [info, piece] : pieces) {
        const UINT offsetX = static_cast<UINT>(info.desktopRect.left - bounds.left);
        const UINT offsetY = static_cast<UINT>(info.desktopRect.top - bounds.top);
        const UINT copyWidth = (std::min)(piece.width, static_cast<UINT>(info.desktopRect.right - info.desktopRect.left));
        const UINT copyHeight = (std::min)(piece.height, static_cast<UINT>(info.desktopRect.bottom - info.desktopRect.top));
        for (UINT y = 0; y < copyHeight && offsetY + y < frame.height; ++y) {
            std::memcpy(frame.bgra.data() + static_cast<std::size_t>(offsetY + y) * frame.stride + offsetX * 4,
                        piece.bgra.data() + static_cast<std::size_t>(y) * piece.stride,
                        static_cast<std::size_t>(copyWidth) * 4);
        }
    }
    return true;
}

void CaptureEngine::ProcessPendingRequests(std::size_t maximumRequests) {
    for (std::size_t processed = 0; processed < maximumRequests; ++processed) {
        CaptureRequest request;
        {
            std::lock_guard lock(stateMutex_);
            if (requests_.empty()) return;
            request = std::move(requests_.front());
            requests_.pop_front();
        }
        ProcessCaptureRequest(std::move(request));
    }
}

void CaptureEngine::ProcessCaptureRequest(CaptureRequest request) {
    FrameBuffer frame;
    HMONITOR feedbackMonitor = request.preferredMonitor;
    std::wstring error;
    std::wstring outputPath;
    double response = 0.0;
    bool feedbackAlreadyPosted = false;

    const auto postFastFeedback = [&](const wchar_t* detail) {
        EngineEvent feedback;
        feedback.type = EngineEventType::Feedback;
        feedback.mode = request.mode;
        feedback.path = outputPath;
        feedback.responseMs = response;
        feedback.monitor = feedbackMonitor;
        feedback.detail = detail;
        PostEngineEvent(std::move(feedback));
    };
    const auto postCaptureError = [&](std::wstring detail) {
        EngineEvent event;
        event.type = EngineEventType::Error;
        event.mode = request.mode;
        event.detail = std::move(detail);
        PostEngineEvent(std::move(event));
    };

    if (request.mode == CaptureMode::ActiveDisplay) {
        auto* state = FindState(request.preferredMonitor);
        if (!state || !state->hasFrame || !state->latestTexture) {
            postCaptureError(L"No active display frame is available");
            return;
        }
        if (!EnsureStagingTexture(*state, error)) {
            postCaptureError(error);
            return;
        }
        feedbackMonitor = state->info.monitor;
        state->context->CopyResource(state->stagingTexture.Get(), state->latestTexture.Get());
        const auto capturedAt = QpcNow();
        response = QpcMilliseconds(request.requestedQpc, capturedAt, qpcFrequency_.QuadPart);
        outputPath = BuildOutputPath(request.settings, request.filenamePrefix);
        postFastFeedback(L"Display captured");
        feedbackAlreadyPosted = true;

        if (!ReadStateToCpu(*state, frame, error, false)) {
            postCaptureError(error);
            return;
        }
    } else if (request.mode == CaptureMode::ActiveWindow) {
        if (!BuildCombinedFrame(frame, error)) {
            postCaptureError(error.empty() ? L"The window picker could not prepare the desktop frame" : error);
            return;
        }
        feedbackMonitor = request.preferredMonitor;
        const auto capturedAt = QpcNow();
        response = QpcMilliseconds(request.requestedQpc, capturedAt, qpcFrequency_.QuadPart);
    } else {
        // Region spans the whole virtual desktop so a selection can cross monitors.
        if (!BuildCombinedFrame(frame, error)) {
            postCaptureError(error);
            return;
        }
        const auto capturedAt = QpcNow();
        response = QpcMilliseconds(request.requestedQpc, capturedAt, qpcFrequency_.QuadPart);
    }

    if (request.mode == CaptureMode::Region || request.mode == CaptureMode::ActiveWindow) {
        EngineEvent event;
        event.type = EngineEventType::RegionFrame;
        event.mode = request.mode;
        event.filenamePrefix = request.filenamePrefix;
        event.responseMs = response;
        event.frame = std::make_shared<FrameBuffer>(std::move(frame));
        PostEngineEvent(std::move(event));
        return;
    }

    if (outputPath.empty()) outputPath = BuildOutputPath(request.settings, request.filenamePrefix);
    SaveJob job;
    job.id = request.id;
    job.path = outputPath;
    job.bgra = std::move(frame.bgra);
    job.width = frame.width;
    job.height = frame.height;
    job.stride = frame.stride;
    job.format = request.settings.imageFormat;
    job.outputWidth = request.settings.outputWidth;
    job.outputHeight = request.settings.outputHeight;
    job.jpegQuality = request.settings.jpegQuality;
    job.responseMs = response;

    if (!feedbackAlreadyPosted) {
        EngineEvent feedback;
        feedback.type = EngineEventType::Feedback;
        feedback.mode = request.mode;
        feedback.path = job.path;
        feedback.responseMs = response;
        feedback.monitor = feedbackMonitor;
        feedback.detail = request.mode == CaptureMode::ActiveWindow ? L"Active window captured" : L"All displays captured";
        PostEngineEvent(std::move(feedback));
    }
    QueueSave(std::move(job));
}

std::wstring CaptureEngine::BuildOutputPath(const AppSettings& settings, const std::wstring& filenamePrefix) const {
    const auto directory = settings.saveDirectory.empty() ? GetDefaultScreenshotDirectory() : settings.saveDirectory;
    return MakeChronologicalMediaPath(directory, filenamePrefix,
                                      settings.imageFormat == ImageFormat::Jpeg ? L".jpg" : L".png");
}

void CaptureEngine::PostEngineEvent(EngineEvent event) const {
    auto* heapEvent = new (std::nothrow) EngineEvent(std::move(event));
    if (!heapEvent) return;
    if (!PostMessageW(notifyWindow_, WM_APP_ENGINE_EVENT, 0, reinterpret_cast<LPARAM>(heapEvent))) delete heapEvent;
}

void CaptureEngine::QueueSave(SaveJob job) {
    {
        std::lock_guard lock(saveMutex_);
        if (saveQueue_.size() >= 8) saveQueue_.pop_front();
        saveQueue_.push_back(std::move(job));
    }
    saveCv_.notify_one();
}
