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
    ResetEvent(stopEvent_);
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

bool CaptureEngine::RequestCapture(CaptureMode mode, HMONITOR preferredMonitor, HWND targetWindow,
                                   const std::wstring& filenamePrefix, const AppSettings& settings) {
    if (!running_.load() || !ready_.load(std::memory_order_acquire)) return false;
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

void CaptureEngine::SaveEditedFrame(FrameBuffer frame, const AppSettings& settings, bool, const std::wstring& outputPath, const std::wstring& filenamePrefix) {
    if (frame.bgra.empty()) return;
    SaveJob job;
    {
        std::lock_guard lock(stateMutex_);
        job.id = nextRequestId_++;
    }
    job.path = outputPath.empty() ? BuildOutputPath(settings, filenamePrefix) : outputPath;
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

void CaptureEngine::CaptureThreadMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetReady(false, L"Starting DXGI capture…");

    while (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
        if (duplicators_.empty()) {
            std::wstring error;
            SetReady(false, L"Connecting displays…");
            if (!InitializeAllDuplicators(error)) {
                EngineEvent event;
                event.type = EngineEventType::Error;
                event.detail = error;
                PostEngineEvent(std::move(event));
                WaitForSingleObject(wakeEvent_, 500);
                continue;
            }
            SetReady(true, L"All displays are warm and ready");
        }

        ProcessPendingRequests();

        bool lost = false;
        for (auto& state : duplicators_) {
            std::wstring error;
            if (!PumpOneFrame(*state, 0, error) && !error.empty()) {
                EngineEvent event;
                event.type = EngineEventType::Reconfiguring;
                event.detail = error;
                PostEngineEvent(std::move(event));
                lost = true;
                break;
            }
        }
        if (lost) {
            ready_.store(false, std::memory_order_release);
            ResetDuplicators();
            continue;
        }

        ProcessPendingRequests();
        WaitForSingleObject(wakeEvent_, 1);
    }

    ResetDuplicators();
    CoUninitialize();
}

void CaptureEngine::SaveThreadMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    PrewarmWic();
    for (;;) {
        SaveJob job;
        {
            std::unique_lock lock(saveMutex_);
            saveCv_.wait(lock, [&] { return saveStopping_ || !saveQueue_.empty(); });
            if (saveStopping_ && saveQueue_.empty()) break;
            job = std::move(saveQueue_.front());
            saveQueue_.pop_front();
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

    const ULONGLONG deadline = GetTickCount64() + 2500;
    while (GetTickCount64() < deadline && WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
        bool allWarm = true;
        for (auto& state : duplicators_) {
            if (state->hasFrame) continue;
            allWarm = false;
            std::wstring warmError;
            PumpOneFrame(*state, 40, warmError);
        }
        if (allWarm) return true;
    }
    for (const auto& state : duplicators_) {
        if (state->hasFrame) return true;
    }
    error = L"DXGI did not deliver a warm-up frame";
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
    hr = state.output->DuplicateOutput(state.device.Get(), &state.duplication);
    if (FAILED(hr)) { error = L"DuplicateOutput: " + HResultText(hr); return false; }
    return true;
}

void CaptureEngine::ResetDuplicators() { duplicators_.clear(); }

bool CaptureEngine::EnsureFrameTextures(DuplicatorState& state, const D3D11_TEXTURE2D_DESC& sourceDesc,
                                        std::wstring& error) {
    if (state.latestTexture && state.stagingTexture && state.frameDesc.Width == sourceDesc.Width &&
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

    D3D11_TEXTURE2D_DESC staging = latest;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stagingTexture;
    hr = state.device->CreateTexture2D(&staging, nullptr, &stagingTexture);
    if (FAILED(hr)) { error = L"Create staging texture: " + HResultText(hr); return false; }
    state.latestTexture = std::move(latestTexture);
    state.stagingTexture = std::move(stagingTexture);
    state.frameDesc = latest;
    state.hasFrame = false;
    return true;
}

bool CaptureEngine::PumpOneFrame(DuplicatorState& state, DWORD timeoutMs, std::wstring& error) {
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    ComPtr<IDXGIResource> resource;
    const HRESULT hr = state.duplication->AcquireNextFrame(timeoutMs, &frameInfo, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (hr == DXGI_ERROR_ACCESS_LOST) { error = L"Display access changed; reconnecting DXGI"; return false; }
    if (FAILED(hr)) { error = L"AcquireNextFrame: " + HResultText(hr); return false; }
    struct Guard { IDXGIOutputDuplication* duplication; ~Guard() { if (duplication) duplication->ReleaseFrame(); } }
        guard{state.duplication.Get()};
    ComPtr<ID3D11Texture2D> frameTexture;
    if (FAILED(resource.As(&frameTexture))) { error = L"Desktop frame is not a texture"; return false; }
    D3D11_TEXTURE2D_DESC desc{};
    frameTexture->GetDesc(&desc);
    if (!EnsureFrameTextures(state, desc, error)) return false;
    state.context->CopyResource(state.latestTexture.Get(), frameTexture.Get());
    state.hasFrame = true;
    return true;
}

CaptureEngine::DuplicatorState* CaptureEngine::FindState(HMONITOR monitor) {
    for (auto& state : duplicators_) if (state->info.monitor == monitor && state->hasFrame) return state.get();
    for (auto& state : duplicators_) if (state->hasFrame) return state.get();
    return nullptr;
}

bool CaptureEngine::ReadStateToCpu(DuplicatorState& state, FrameBuffer& frame, std::wstring& error, bool queueCopy) {
    if (!state.hasFrame || !state.latestTexture || !state.stagingTexture) { error = L"Display has no cached frame"; return false; }
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
    std::vector<std::pair<OutputInfo, FrameBuffer>> pieces;
    RECT bounds{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
    for (auto& state : duplicators_) {
        if (!state->hasFrame) continue;
        FrameBuffer piece;
        if (!ReadStateToCpu(*state, piece, error)) continue;
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

void CaptureEngine::ProcessPendingRequests() {
    for (;;) {
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

    if (request.mode == CaptureMode::ActiveDisplay) {
        auto* state = FindState(request.preferredMonitor);
        if (!state || !state->hasFrame || !state->stagingTexture || !state->latestTexture) {
            EngineEvent event;
            event.type = EngineEventType::Error;
            event.detail = L"No active display frame is available";
            PostEngineEvent(std::move(event));
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
            EngineEvent event;
            event.type = EngineEventType::Error;
            event.detail = error;
            PostEngineEvent(std::move(event));
            return;
        }
    } else if (request.mode == CaptureMode::ActiveWindow) {
        if (!BuildCombinedFrame(frame, error)) {
            EngineEvent event;
            event.type = EngineEventType::Error;
            event.detail = error.empty() ? L"The window picker could not prepare the desktop frame" : error;
            PostEngineEvent(std::move(event));
            return;
        }
        feedbackMonitor = request.preferredMonitor;
        const auto capturedAt = QpcNow();
        response = QpcMilliseconds(request.requestedQpc, capturedAt, qpcFrequency_.QuadPart);
    } else {
        if (!BuildCombinedFrame(frame, error)) {
            EngineEvent event;
            event.type = EngineEventType::Error;
            event.detail = error;
            PostEngineEvent(std::move(event));
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
    SYSTEMTIME time{};
    GetLocalTime(&time);
    const auto filename = MakeScreenshotFilename(filenamePrefix, time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                                                 time.wSecond, time.wMilliseconds,
                                                 settings.imageFormat == ImageFormat::Jpeg);
    const auto directory = settings.saveDirectory.empty() ? GetDefaultScreenshotDirectory() : settings.saveDirectory;
    return (std::filesystem::path(directory) / filename).wstring();
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
