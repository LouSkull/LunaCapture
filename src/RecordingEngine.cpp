#include "RecordingEngine.hpp"

#include "AudioMixer.hpp"
#include "CoreUtils.hpp"
#include "WicEncoder.hpp"
#include "WinUtil.hpp"

#include <Windows.h>
#include <avrt.h>
#include <audioclient.h>
// icodecapi.h declares the ICodecAPI interface; codecapi.h carries the
// CODECAPI_* property GUIDs and the rate-control enums used to tune the encoder.
#include <icodecapi.h>
#include <codecapi.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include <immintrin.h>

using Microsoft::WRL::ComPtr;

namespace {
constexpr LONGLONG kHundredNsPerSecond = 10'000'000;

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
        wchar_t value[32]{};
        swprintf_s(value, L"0x%08X", static_cast<unsigned>(hr));
        result = value;
    }
    if (message) LocalFree(message);
    return result;
}

// Codec -> (encoder subtype, muxer, file extension). Not every codec fits every
// container, so the three travel together: asking the MP4 muxer for MJPEG or the
// AVI muxer for HEVC fails at AddStream even when the encoder itself exists.
struct VideoCodecProfile {
    const GUID& subtype;
    const GUID& container;
    const wchar_t* extension;
    const wchar_t* name;
};

const VideoCodecProfile& CodecProfile(VideoCodec codec) {
    static const VideoCodecProfile kProfiles[kVideoCodecCount] = {
        {MFVideoFormat_H264, MFTranscodeContainerType_MPEG4,  L".mp4", L"H.264 / AVC"},
        {MFVideoFormat_HEVC, MFTranscodeContainerType_MPEG4,  L".mp4", L"HEVC / H.265"},
        {MFVideoFormat_AV1,  MFTranscodeContainerType_MPEG4,  L".mp4", L"AV1"},
        {MFVideoFormat_VP90, MFTranscodeContainerType_MPEG4,  L".mp4", L"VP9"},
        {MFVideoFormat_H264, MFTranscodeContainerType_FMPEG4, L".mp4", L"H.264 fragmented"},
        {MFVideoFormat_HEVC, MFTranscodeContainerType_FMPEG4, L".mp4", L"HEVC fragmented"},
        {MFVideoFormat_MP4V, MFTranscodeContainerType_MPEG4,  L".mp4", L"MPEG-4 Part 2"},
        {MFVideoFormat_H263, MFTranscodeContainerType_3GP,    L".3gp", L"H.263"},
        {MFVideoFormat_WMV3, MFTranscodeContainerType_ASF,    L".wmv", L"Windows Media Video 9"},
        {MFVideoFormat_WVC1, MFTranscodeContainerType_ASF,    L".wmv", L"VC-1"},
        {MFVideoFormat_MJPG, MFTranscodeContainerType_AVI,    L".avi", L"Motion JPEG"},
        {MFVideoFormat_H264, MFTranscodeContainerType_AVI,    L".avi", L"H.264 in AVI"},
    };
    const int index = static_cast<int>(codec);
    return kProfiles[index >= 0 && index < kVideoCodecCount ? index : 0];
}

std::wstring TimestampedPath(const std::wstring& directory, const wchar_t* prefix, VideoCodec codec) {
    return MakeChronologicalMediaPath(directory, prefix ? prefix : L"Recording", CodecProfile(codec).extension);
}

UINT Even(UINT value) { return value > 1 ? value & ~1u : 2u; }

struct DisplayTarget {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    RECT desktopRect{};
    HMONITOR monitor = nullptr;
    int linearIndex = 0;
    std::wstring deviceName;
    std::wstring adapterName;
};

std::vector<DisplayTarget> EnumerateDisplayTargets() {
    std::vector<DisplayTarget> targets;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return targets;
    int linear = 0;
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);
        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> outputBase;
            if (adapter->EnumOutputs(outputIndex, &outputBase) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(outputBase->GetDesc(&desc)) || !desc.AttachedToDesktop) continue;
            DisplayTarget target;
            target.adapter = adapter;
            if (FAILED(outputBase.As(&target.output))) continue;
            target.desktopRect = desc.DesktopCoordinates;
            target.monitor = desc.Monitor;
            target.linearIndex = linear++;
            target.deviceName = desc.DeviceName;
            target.adapterName = adapterDesc.Description;
            targets.push_back(std::move(target));
        }
    }
    return targets;
}

class DesktopRecorderSource {
public:
    ~DesktopRecorderSource() { Reset(); }

    bool Initialize(int requestedIndex, int requestedHeight, CapturePriority priority, std::wstring& error) {
        Reset();
        requestedIndex_ = (std::max)(0, requestedIndex);
        requestedHeight_ = requestedHeight > 0 ? Even(static_cast<UINT>(requestedHeight)) : 0;
        // "CPU priority" keeps the GPU free for the game: the frame resize falls
        // back to StretchDIBits on the CPU instead of the D3D video processor.
        priority_ = priority;
        gpuScalingAllowed_ = priority != CapturePriority::Cpu;
        if (!EnsureRecoverySurface(error)) return false;
        recovering_ = true;
        // Give the normal path a brief head start, then begin the MP4 with the recovery
        // card. A protected/fullscreen source must not leave Record/Replay stuck in a
        // five-second startup attempt before automatic retries can take over.
        const ULONGLONG deadline = GetTickCount64() + 1000;
        std::wstring lastError;
        while (GetTickCount64() < deadline) {
            if (!duplication_ && !Connect(lastError)) {
                Sleep(75);
                continue;
            }
            std::wstring frameError;
            if (AcquireLatest(250, frameError) && ReadFrame(false, frameError) && hasFrame_) {
                recovering_ = false;
                lastCaptureError_.clear();
                return true;
            }
            if (!frameError.empty()) lastError = frameError;
        }
        lastCaptureError_ = lastError.empty() ? L"Fullscreen display did not deliver a frame" : lastError;
        AppendDiagnosticLog(lastCaptureError_ + L" · recording will use a recovery frame and retry automatically");
        error.clear();
        return true;
    }

    bool AcquireLatest(DWORD timeoutMs, std::wstring& error) {
        error.clear();
        if (!duplication_) {
            const ULONGLONG now = GetTickCount64();
            if (now < nextReconnectTick_) return hasFrame_;
            std::wstring reconnectError;
            if (!Connect(reconnectError)) {
                nextReconnectTick_ = now + 250;
                lastCaptureError_ = reconnectError;
                if (!hasFrame_) error = reconnectError;
                return hasFrame_;
            }
        }

        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        const HRESULT hr = duplication_->AcquireNextFrame(timeoutMs, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return hasFrame_;
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED ||
            hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_SESSION_DISCONNECTED) {
            lastCaptureError_ = L"Fullscreen capture was interrupted: " + HResultText(hr);
            BeginReconnect();
            return hasFrame_;
        }
        if (FAILED(hr)) {
            error = L"Recording AcquireNextFrame: " + HResultText(hr);
            lastCaptureError_ = error;
            AppendDiagnosticLog(error);
            BeginReconnect();
            return false;
        }
        struct ReleaseGuard {
            // Reconnect paths reset DesktopRecorderSource::duplication_ before this
            // scope exits. Keep our own COM reference so ReleaseFrame never targets
            // an object that was destroyed by BeginReconnect().
            ComPtr<IDXGIOutputDuplication> duplication;
            ~ReleaseGuard() { if (duplication) duplication->ReleaseFrame(); }
        } guard{duplication_};

        if (info.LastMouseUpdateTime.QuadPart != 0) {
            pointerVisible_ = info.PointerPosition.Visible != FALSE;
            pointerPosition_ = info.PointerPosition.Position;
        }
        if (info.PointerShapeBufferSize > 0) {
            std::vector<BYTE> shape(info.PointerShapeBufferSize);
            UINT required = 0;
            DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo{};
            const HRESULT pointerResult = duplication_->GetFramePointerShape(
                static_cast<UINT>(shape.size()), shape.data(), &required, &shapeInfo);
            if (SUCCEEDED(pointerResult)) {
                shape.resize(required);
                pointerShape_ = std::move(shape);
                pointerShapeInfo_ = shapeInfo;
            }
        }

        ComPtr<ID3D11Texture2D> frame;
        if (FAILED(resource.As(&frame))) {
            error = L"Desktop recording frame is not a texture";
            lastCaptureError_ = error;
            BeginReconnect();
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        frame->GetDesc(&desc);
        if (!EnsureSurfaces(desc, error)) {
            lastCaptureError_ = error;
            BeginReconnect();
            return false;
        }
        // Screenshots must not inherit the encoder's downscale. Whenever the CPU readback
        // path is lossy (GPU scaler active, or the DIB kept an older session size), keep a
        // native-resolution GPU copy of the newest raw frame for ReadNativeSnapshot.
        if (gpuScaling_ || nativeWidth_ != width_ || nativeHeight_ != height_) {
            if (!nativeCopy_ || nativeCopyDesc_.Width != desc.Width || nativeCopyDesc_.Height != desc.Height ||
                nativeCopyDesc_.Format != desc.Format) {
                nativeCopy_.Reset();
                nativeCopyStaging_.Reset();
                nativeCopyValid_ = false;
                D3D11_TEXTURE2D_DESC copy = desc;
                copy.MipLevels = 1;
                copy.ArraySize = 1;
                copy.SampleDesc.Count = 1;
                copy.SampleDesc.Quality = 0;
                copy.Usage = D3D11_USAGE_DEFAULT;
                copy.BindFlags = 0;
                copy.CPUAccessFlags = 0;
                copy.MiscFlags = 0;
                if (SUCCEEDED(device_->CreateTexture2D(&copy, nullptr, &nativeCopy_))) nativeCopyDesc_ = copy;
            }
            if (nativeCopy_) {
                context_->CopyResource(nativeCopy_.Get(), frame.Get());
                nativeCopyValid_ = true;
            }
        } else if (nativeCopy_) {
            nativeCopy_.Reset();
            nativeCopyStaging_.Reset();
            nativeCopyValid_ = false;
        }
        // Keep GPU readback one or two frames behind the desktop. Mapping the
        // texture copied in this same iteration forces a pipeline flush and can
        // steal several milliseconds from a fullscreen game's render queue.
        ID3D11Texture2D* readbackSource = frame.Get();
        if (gpuScaling_) {
            if (!ScaleFrameOnGpu(frame.Get(), error)) {
                AppendDiagnosticLog(error + L" · switching to native readback fallback");
                gpuScalingAllowed_ = false;
                lastCaptureError_ = error;
                BeginReconnect();
                return false;
            }
            readbackSource = scaledFrame_.Get();
        }
        if (pendingStagingCopies_ < staging_.size()) {
            context_->CopyResource(staging_[stagingWriteIndex_].Get(), readbackSource);
            stagingWriteIndex_ = (stagingWriteIndex_ + 1) % staging_.size();
            ++pendingStagingCopies_;
            nativeFrameReady_ = true;
        }
        return true;
    }

    const BYTE* ReadFrame(bool drawCursor, std::wstring& error) {
        if (!dibBits_) { error = L"No desktop frame surface is ready"; return nullptr; }
        if (!nativeFrameReady_) {
            if (!hasFrame_) return recovering_ ? static_cast<const BYTE*>(dibBits_) : nullptr;
            RestoreCursorBackground();
            if (drawCursor) DrawCursor();
            return static_cast<const BYTE*>(dibBits_);
        }
        if (hasFrame_ && pendingStagingCopies_ < 2) {
            // Deliberately keep one frame of distance between CopyResource and
            // Map. A recording can tolerate ~16 ms latency; the game's render
            // queue should never be flushed just to obtain the newest pixels.
            RestoreCursorBackground();
            if (drawCursor) DrawCursor();
            return static_cast<const BYTE*>(dibBits_);
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const UINT mapFlags = hasFrame_ ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0;
        const HRESULT hr = context_->Map(staging_[stagingReadIndex_].Get(), 0, D3D11_MAP_READ, mapFlags, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING && hasFrame_) {
            // The GPU is busy with the game. Reuse the last completed desktop
            // image rather than blocking both workloads on a synchronous readback.
            RestoreCursorBackground();
            if (drawCursor) DrawCursor();
            return static_cast<const BYTE*>(dibBits_);
        }
        if (FAILED(hr)) {
            error = L"Map recording frame: " + HResultText(hr);
            lastCaptureError_ = error;
            BeginReconnect();
            return static_cast<const BYTE*>(dibBits_);
        }

        const auto* source = static_cast<const BYTE*>(mapped.pData);
        if (nativeWidth_ == width_ && nativeHeight_ == height_) {
            auto* destination = static_cast<BYTE*>(dibBits_);
            for (UINT y = 0; y < height_; ++y) {
                ConvertRow(destination + static_cast<std::size_t>(y) * width_ * 4,
                           source + static_cast<std::size_t>(y) * mapped.RowPitch, width_, format_);
            }
            contentRect_ = RECT{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        } else {
            sourceFrame_.resize(static_cast<std::size_t>(nativeWidth_) * nativeHeight_ * 4);
            for (UINT y = 0; y < nativeHeight_; ++y) {
                ConvertRow(sourceFrame_.data() + static_cast<std::size_t>(y) * nativeWidth_ * 4,
                           source + static_cast<std::size_t>(y) * mapped.RowPitch, nativeWidth_, format_);
            }
        }
        context_->Unmap(staging_[stagingReadIndex_].Get(), 0);
        stagingReadIndex_ = (stagingReadIndex_ + 1) % staging_.size();
        if (pendingStagingCopies_ > 0) --pendingStagingCopies_;
        if (nativeWidth_ != width_ || nativeHeight_ != height_) ScaleNativeFrame();
        cursorBackupValid_ = false;
        hasFrame_ = true;
        recovering_ = false;
        lastCaptureError_.clear();
        nativeFrameReady_ = pendingStagingCopies_ != 0;
        if (IsNearlyBlack()) {
            ++blackFrameCount_;
            // Keep retrying for as long as the source stays black instead of giving up after a
            // handful of attempts — a stuck capture should never be left to record black forever.
            if (blackFrameCount_ >= 120) {
                ++blackFallbacks_;
                blackFrameCount_ = 0;
                AppendDiagnosticLog(L"Sustained black fullscreen frames detected; forcing backend fallback (attempt " +
                                    std::to_wstring(blackFallbacks_) + L")");
                BeginReconnect();
            }
        } else {
            blackFrameCount_ = 0;
            blackFallbacks_ = 0;
        }
        if (drawCursor) DrawCursor();
        return static_cast<const BYTE*>(dibBits_);
    }

    // On-demand native-resolution readback for screenshots. Returns false when the regular
    // ReadFrame path is already lossless (no downscale, DIB matches the readback size) or
    // no raw frame has been copied yet; callers then fall back to ReadFrame's pixels.
    bool ReadNativeSnapshot(FrameBuffer& out) {
        if (!nativeCopyValid_ || !nativeCopy_ || !device_ || !context_) return false;
        if (!nativeCopyStaging_) {
            D3D11_TEXTURE2D_DESC staging = nativeCopyDesc_;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.BindFlags = 0;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.MiscFlags = 0;
            if (FAILED(device_->CreateTexture2D(&staging, nullptr, &nativeCopyStaging_))) return false;
        }
        context_->CopyResource(nativeCopyStaging_.Get(), nativeCopy_.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        // A synchronous map is acceptable here: this runs once per screenshot, not per frame.
        if (FAILED(context_->Map(nativeCopyStaging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        out.width = nativeCopyDesc_.Width;
        out.height = nativeCopyDesc_.Height;
        out.stride = out.width * 4;
        out.virtualRect = captureDesktopRect_;
        out.bgra.resize(static_cast<std::size_t>(out.stride) * out.height);
        const auto* source = static_cast<const BYTE*>(mapped.pData);
        for (UINT y = 0; y < out.height; ++y) {
            ConvertRow(out.bgra.data() + static_cast<std::size_t>(y) * out.stride,
                       source + static_cast<std::size_t>(y) * mapped.RowPitch, out.width, nativeCopyDesc_.Format);
        }
        context_->Unmap(nativeCopyStaging_.Get(), 0);
        return true;
    }

    UINT Width() const noexcept { return width_; }
    UINT Height() const noexcept { return height_; }
    bool IsRecovering() const noexcept { return recovering_; }
    const std::wstring& RecoveryDetail() const noexcept { return lastCaptureError_; }
    const BYTE* EnterRecovery(const std::wstring& detail) {
        if (!detail.empty()) lastCaptureError_ = detail;
        if (!recovering_) BeginReconnect();
        else PaintRecoveryFrame();
        return static_cast<const BYTE*>(dibBits_);
    }
    void Release() { Reset(); }
    void NotifyDisplayChange() { if (width_ != 0 && height_ != 0) BeginReconnect(); }
    HMONITOR Monitor() const noexcept { return captureMonitor_; }
    RECT DesktopRect() const noexcept { return captureDesktopRect_; }
    // True when the most recent real readback was (near) black — e.g. a monitor
    // whose exclusive-fullscreen app the desktop duplicator can't read. Used to
    // hold the last good preview instead of flashing a black tile.
    bool LastFrameNearlyBlack() const noexcept { return blackFrameCount_ > 0; }
    std::wstring Description() const {
        const std::wstring display = captureDeviceName_.empty() ? L"Desktop" : captureDeviceName_;
        return display + L" · " + (recovering_ ? L"capture retrying" :
               (usingModernDuplication_ ? L"DXGI 1.5" : L"DXGI legacy")) + L" · " +
               std::to_wstring(width_) + L"x" + std::to_wstring(height_);
    }

private:
    bool IsNearlyBlack() const {
        if (!dibBits_ || width_ == 0 || height_ == 0) return false;
        const auto* pixels = static_cast<const BYTE*>(dibBits_);
        const UINT stepX = (std::max)(1U, width_ / 32);
        const UINT stepY = (std::max)(1U, height_ / 18);
        for (UINT y = stepY / 2; y < height_; y += stepY) {
            for (UINT x = stepX / 2; x < width_; x += stepX) {
                const BYTE* pixel = pixels + (static_cast<std::size_t>(y) * width_ + x) * 4;
                if (pixel[0] > 5 || pixel[1] > 5 || pixel[2] > 5) return false;
            }
        }
        return true;
    }

    static float HalfToFloat(std::uint16_t half) {
        const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16;
        std::uint32_t exponent = (half >> 10) & 0x1fu;
        std::uint32_t mantissa = half & 0x03ffu;
        std::uint32_t bits = 0;
        if (exponent == 0) {
            if (mantissa == 0) bits = sign;
            else {
                std::uint32_t floatExponent = 113;
                while ((mantissa & 0x0400u) == 0) { mantissa <<= 1; --floatExponent; }
                mantissa &= 0x03ffu;
                bits = sign | (floatExponent << 23) | (mantissa << 13);
            }
        } else if (exponent == 31) {
            bits = sign | 0x7f800000u | (mantissa << 13);
        } else {
            bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
        }
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    static BYTE FloatChannel(float value) {
        return static_cast<BYTE>((std::max)(0.0f, (std::min)(1.0f, value)) * 255.0f + 0.5f);
    }

    static void ConvertRow(BYTE* destination, const BYTE* source, UINT width, DXGI_FORMAT format) {
        if (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            // Desktop Duplication's alpha channel is not guaranteed. Normalize
            // four pixels per instruction without a second full-frame pass.
            const __m128i alpha = _mm_set1_epi32(static_cast<int>(0xff000000u));
            UINT x = 0;
            for (; x + 4 <= width; x += 4) {
                const __m128i pixels = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source + static_cast<std::size_t>(x) * 4));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + static_cast<std::size_t>(x) * 4),
                                 _mm_or_si128(pixels, alpha));
            }
            for (; x < width; ++x) {
                auto pixel = reinterpret_cast<const std::uint32_t*>(source)[x] | 0xff000000u;
                reinterpret_cast<std::uint32_t*>(destination)[x] = pixel;
            }
            return;
        }
        if (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
            for (UINT x = 0; x < width; ++x) {
                const BYTE* pixel = source + static_cast<std::size_t>(x) * 4;
                BYTE* output = destination + static_cast<std::size_t>(x) * 4;
                output[0] = pixel[2]; output[1] = pixel[1]; output[2] = pixel[0]; output[3] = 255;
            }
            return;
        }
        if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
            const auto* pixels = reinterpret_cast<const std::uint32_t*>(source);
            for (UINT x = 0; x < width; ++x) {
                const std::uint32_t pixel = pixels[x];
                BYTE* output = destination + static_cast<std::size_t>(x) * 4;
                output[2] = static_cast<BYTE>(((pixel & 0x3ffu) * 255u + 511u) / 1023u);
                output[1] = static_cast<BYTE>((((pixel >> 10) & 0x3ffu) * 255u + 511u) / 1023u);
                output[0] = static_cast<BYTE>((((pixel >> 20) & 0x3ffu) * 255u + 511u) / 1023u);
                output[3] = 255;
            }
            return;
        }
        if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            const auto* pixels = reinterpret_cast<const std::uint16_t*>(source);
            for (UINT x = 0; x < width; ++x) {
                const auto* pixel = pixels + static_cast<std::size_t>(x) * 4;
                BYTE* output = destination + static_cast<std::size_t>(x) * 4;
                output[2] = FloatChannel(HalfToFloat(pixel[0]));
                output[1] = FloatChannel(HalfToFloat(pixel[1]));
                output[0] = FloatChannel(HalfToFloat(pixel[2]));
                output[3] = 255;
            }
            return;
        }
        std::memset(destination, 0, static_cast<std::size_t>(width) * 4);
    }

    void PaintRecoveryFrame() {
        if (!dibBits_ || !dibDc_ || width_ == 0 || height_ == 0) return;
        FillCaptureRecoveryFrame(static_cast<std::uint8_t*>(dibBits_), width_, height_, width_ * 4);

        const int panelLeft = static_cast<int>(width_ / 6);
        const int panelRight = static_cast<int>(width_ - width_ / 6);
        const int panelTop = static_cast<int>((height_ * 2) / 5);
        const int panelBottom = static_cast<int>((height_ * 3) / 5);
        const int panelHeight = panelBottom - panelTop;
        const int titlePixels = (std::max)(18, (std::min)(44, static_cast<int>(height_ / 24)));
        const int detailPixels = (std::max)(13, (std::min)(24, static_cast<int>(height_ / 42)));
        HFONT titleFont = CreateFontW(-titlePixels, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT detailFont = CreateFontW(-detailPixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        SetBkMode(dibDc_, TRANSPARENT);
        if (titleFont) {
            const HGDIOBJ oldFont = SelectObject(dibDc_, titleFont);
            SetTextColor(dibDc_, RGB(244, 246, 251));
            RECT titleRect{panelLeft + 24, panelTop + panelHeight / 7,
                           panelRight - 24, panelTop + panelHeight / 2};
            DrawTextW(dibDc_, L"Capture temporarily unavailable", -1, &titleRect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dibDc_, oldFont);
            DeleteObject(titleFont);
        }
        if (detailFont) {
            const HGDIOBJ oldFont = SelectObject(dibDc_, detailFont);
            SetTextColor(dibDc_, RGB(166, 178, 198));
            RECT detailRect{panelLeft + 24, panelTop + panelHeight / 2 - 2,
                            panelRight - 24, panelBottom - panelHeight / 8};
            DrawTextW(dibDc_, L"Recording continues  •  reconnecting automatically", -1, &detailRect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dibDc_, oldFont);
            DeleteObject(detailFont);
        }
        const int dotRadius = (std::max)(2, static_cast<int>(height_ / 270));
        const int dotY = panelBottom - (std::max)(10, panelHeight / 10);
        HBRUSH accent = CreateSolidBrush(RGB(124, 112, 238));
        if (accent) {
            const HGDIOBJ oldBrush = SelectObject(dibDc_, accent);
            const HGDIOBJ oldPen = SelectObject(dibDc_, GetStockObject(NULL_PEN));
            for (int dot = -1; dot <= 1; ++dot) {
                const int centerX = static_cast<int>(width_ / 2) + dot * dotRadius * 4;
                Ellipse(dibDc_, centerX - dotRadius, dotY - dotRadius,
                        centerX + dotRadius + 1, dotY + dotRadius + 1);
            }
            SelectObject(dibDc_, oldPen);
            SelectObject(dibDc_, oldBrush);
            DeleteObject(accent);
        }
        GdiFlush();
        contentRect_ = RECT{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        cursorBackupValid_ = false;
    }

    bool EnsureRecoverySurface(std::wstring& error) {
        if (dibBits_ && width_ != 0 && height_ != 0) return true;

        UINT sourceWidth = 0;
        UINT sourceHeight = 0;
        auto targets = EnumerateDisplayTargets();
        if (!targets.empty()) {
            const std::size_t index = static_cast<std::size_t>(requestedIndex_) % targets.size();
            const auto& selected = targets[index];
            captureMonitor_ = selected.monitor;
            captureDesktopRect_ = selected.desktopRect;
            captureDeviceName_ = selected.deviceName;
            captureAdapterName_ = selected.adapterName;
            sourceWidth = static_cast<UINT>((std::max)(1L, selected.desktopRect.right - selected.desktopRect.left));
            sourceHeight = static_cast<UINT>((std::max)(1L, selected.desktopRect.bottom - selected.desktopRect.top));
        } else {
            const LONG left = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const LONG top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            sourceWidth = static_cast<UINT>((std::max)(1, GetSystemMetrics(SM_CXVIRTUALSCREEN)));
            sourceHeight = static_cast<UINT>((std::max)(1, GetSystemMetrics(SM_CYVIRTUALSCREEN)));
            if (sourceWidth <= 1 || sourceHeight <= 1) {
                sourceWidth = 1280;
                sourceHeight = 720;
            }
            captureDesktopRect_ = RECT{left, top, left + static_cast<LONG>(sourceWidth),
                                       top + static_cast<LONG>(sourceHeight)};
            captureMonitor_ = MonitorFromRect(&captureDesktopRect_, MONITOR_DEFAULTTOPRIMARY);
            captureDeviceName_ = L"Desktop";
        }

        height_ = requestedHeight_ > 0 && requestedHeight_ < sourceHeight ? requestedHeight_ : Even(sourceHeight);
        width_ = requestedHeight_ > 0 && requestedHeight_ < sourceHeight
            ? Even(static_cast<UINT>((static_cast<std::uint64_t>(sourceWidth) * height_ + sourceHeight / 2) /
                                     sourceHeight))
            : Even(sourceWidth);

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = static_cast<LONG>(width_);
        info.bmiHeader.biHeight = -static_cast<LONG>(height_);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        dibDc_ = CreateCompatibleDC(nullptr);
        if (dibDc_) dib_ = CreateDIBSection(dibDc_, &info, DIB_RGB_COLORS, &dibBits_, nullptr, 0);
        if (!dibDc_ || !dib_ || !dibBits_) {
            if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
            if (dibDc_) { DeleteDC(dibDc_); dibDc_ = nullptr; }
            dibBits_ = nullptr;
            width_ = height_ = 0;
            error = L"Create recording recovery surface failed";
            return false;
        }
        dibOld_ = SelectObject(dibDc_, dib_);
        PaintRecoveryFrame();
        return true;
    }

    bool Connect(std::wstring& error) {
        ResetCaptureObjects();
        auto targets = EnumerateDisplayTargets();
        if (targets.empty()) { error = L"No desktop display is available for recording"; return false; }
        const std::size_t index = static_cast<std::size_t>(requestedIndex_) % targets.size();
        target_ = std::move(targets[index]);
        captureMonitor_ = target_.monitor;
        captureDesktopRect_ = target_.desktopRect;
        captureDeviceName_ = target_.deviceName;
        captureAdapterName_ = target_.adapterName;

        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL created{};
        const auto createDevice = [&](UINT flags, const D3D_FEATURE_LEVEL* requestedLevels,
                                      UINT levelCount) {
            device_.Reset();
            context_.Reset();
            return D3D11CreateDevice(target_.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                                     requestedLevels, levelCount, D3D11_SDK_VERSION,
                                     &device_, &created, &context_);
        };
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        HRESULT hr = createDevice(flags, levels, static_cast<UINT>(std::size(levels)));
        if (hr == E_INVALIDARG) hr = createDevice(flags, levels + 1, static_cast<UINT>(std::size(levels) - 1));
        if (FAILED(hr)) {
            flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            hr = createDevice(flags, levels, static_cast<UINT>(std::size(levels)));
            if (hr == E_INVALIDARG) hr = createDevice(flags, levels + 1, static_cast<UINT>(std::size(levels) - 1));
        }
        if (FAILED(hr)) { error = L"Recording D3D device: " + HResultText(hr); return false; }

        ComPtr<IDXGIDevice1> dxgiDevice;
        if (SUCCEEDED(device_.As(&dxgiDevice))) {
            // Game-first GPU scheduling: desktop copies run behind foreground
            // rendering and never build a long device queue.
            dxgiDevice->SetGPUThreadPriority(-5);
            dxgiDevice->SetMaximumFrameLatency(1);
        }

        const auto connectModern = [&]() {
            ComPtr<IDXGIOutput5> output5;
            if (FAILED(target_.output.As(&output5))) return E_NOINTERFACE;
            constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                               DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};
            return output5->DuplicateOutput1(device_.Get(), 0, static_cast<UINT>(std::size(formats)),
                                             formats, &duplication_);
        };
        if (preferLegacyDuplication_) {
            hr = target_.output->DuplicateOutput(device_.Get(), &duplication_);
            usingModernDuplication_ = false;
            if (FAILED(hr)) { hr = connectModern(); usingModernDuplication_ = SUCCEEDED(hr); }
        } else {
            hr = connectModern();
            usingModernDuplication_ = SUCCEEDED(hr);
            if (FAILED(hr)) { hr = target_.output->DuplicateOutput(device_.Get(), &duplication_); usingModernDuplication_ = false; }
        }
        if (FAILED(hr)) {
            error = L"Recording desktop duplication: " + HResultText(hr);
            AppendDiagnosticLog(error + L" · " + target_.deviceName + L" · " + target_.adapterName);
            ResetCaptureObjects();
            return false;
        }
        AppendDiagnosticLog(L"Connected " + target_.deviceName + L" on " + target_.adapterName +
                            (usingModernDuplication_ ? L" using DXGI 1.5" : L" using DXGI legacy"));
        nextReconnectTick_ = 0;
        return true;
    }

    void BeginReconnect() {
        if (!recovering_) preferLegacyDuplication_ = !preferLegacyDuplication_;
        AppendDiagnosticLog(L"Fullscreen surface changed; switching duplication backend");
        ResetCaptureObjects();
        hasFrame_ = false;
        blackFrameCount_ = 0;
        recovering_ = true;
        PaintRecoveryFrame();
        nextReconnectTick_ = 0;
    }

    void ResetGpuScaler() {
        scaledOutputView_.Reset();
        scaledFrame_.Reset();
        videoProcessor_.Reset();
        videoEnumerator_.Reset();
        videoContext_.Reset();
        videoDevice_.Reset();
        gpuScaling_ = false;
    }

    bool ConfigureGpuScaler(const D3D11_TEXTURE2D_DESC& source, UINT outputWidth, UINT outputHeight,
                            std::wstring& error) {
        ResetGpuScaler();
        HRESULT hr = device_.As(&videoDevice_);
        if (SUCCEEDED(hr)) hr = context_.As(&videoContext_);

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = DXGI_RATIONAL{60, 1};
        content.InputWidth = source.Width;
        content.InputHeight = source.Height;
        content.OutputFrameRate = DXGI_RATIONAL{60, 1};
        content.OutputWidth = outputWidth;
        content.OutputHeight = outputHeight;
        content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        if (SUCCEEDED(hr)) hr = videoDevice_->CreateVideoProcessorEnumerator(&content, &videoEnumerator_);

        UINT inputSupport = 0;
        UINT outputSupport = 0;
        if (SUCCEEDED(hr)) hr = videoEnumerator_->CheckVideoProcessorFormat(source.Format, &inputSupport);
        if (SUCCEEDED(hr)) hr = videoEnumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &outputSupport);
        if (SUCCEEDED(hr) && ((inputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0 ||
                             (outputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0)) {
            hr = MF_E_INVALIDMEDIATYPE;
        }
        if (SUCCEEDED(hr)) hr = videoDevice_->CreateVideoProcessor(videoEnumerator_.Get(), 0, &videoProcessor_);

        D3D11_TEXTURE2D_DESC scaled{};
        scaled.Width = outputWidth;
        scaled.Height = outputHeight;
        scaled.MipLevels = 1;
        scaled.ArraySize = 1;
        scaled.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scaled.SampleDesc.Count = 1;
        scaled.Usage = D3D11_USAGE_DEFAULT;
        scaled.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (SUCCEEDED(hr)) hr = device_->CreateTexture2D(&scaled, nullptr, &scaledFrame_);

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputView{};
        outputView.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outputView.Texture2D.MipSlice = 0;
        if (SUCCEEDED(hr)) {
            hr = videoDevice_->CreateVideoProcessorOutputView(scaledFrame_.Get(), videoEnumerator_.Get(),
                                                               &outputView, &scaledOutputView_);
        }
        if (FAILED(hr)) {
            error = HResultText(hr);
            ResetGpuScaler();
            return false;
        }
        gpuScaling_ = true;
        return true;
    }

    bool ScaleFrameOnGpu(ID3D11Texture2D* source, std::wstring& error) {
        if (!gpuScaling_ || !source || !videoDevice_ || !videoContext_ || !videoProcessor_ ||
            !videoEnumerator_ || !scaledOutputView_) return false;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputView{};
        inputView.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputView.Texture2D.MipSlice = 0;
        inputView.Texture2D.ArraySlice = 0;
        ComPtr<ID3D11VideoProcessorInputView> input;
        HRESULT hr = videoDevice_->CreateVideoProcessorInputView(source, videoEnumerator_.Get(), &inputView, &input);
        const RECT sourceRect{0, 0, static_cast<LONG>(sourceTextureWidth_), static_cast<LONG>(sourceTextureHeight_)};
        const RECT outputRect{0, 0, static_cast<LONG>(nativeWidth_), static_cast<LONG>(nativeHeight_)};
        if (SUCCEEDED(hr)) {
            videoContext_->VideoProcessorSetStreamFrameFormat(videoProcessor_.Get(), 0,
                                                               D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
            videoContext_->VideoProcessorSetStreamAutoProcessingMode(videoProcessor_.Get(), 0, FALSE);
            videoContext_->VideoProcessorSetStreamSourceRect(videoProcessor_.Get(), 0, TRUE, &sourceRect);
            videoContext_->VideoProcessorSetStreamDestRect(videoProcessor_.Get(), 0, TRUE, &outputRect);
            videoContext_->VideoProcessorSetOutputTargetRect(videoProcessor_.Get(), TRUE, &outputRect);
            D3D11_VIDEO_PROCESSOR_STREAM stream{};
            stream.Enable = TRUE;
            stream.pInputSurface = input.Get();
            hr = videoContext_->VideoProcessorBlt(videoProcessor_.Get(), scaledOutputView_.Get(), 0, 1, &stream);
        }
        if (FAILED(hr)) {
            error = L"GPU recording scaler: " + HResultText(hr);
            return false;
        }
        return true;
    }

    bool EnsureSurfaces(const D3D11_TEXTURE2D_DESC& source, std::wstring& error) {
        if (staging_[0] && sourceTextureWidth_ == source.Width && sourceTextureHeight_ == source.Height &&
            sourceTextureFormat_ == source.Format) {
            return true;
        }
        for (auto& surface : staging_) surface.Reset();
        ResetGpuScaler();
        stagingReadIndex_ = 0;
        stagingWriteIndex_ = 0;
        pendingStagingCopies_ = 0;

        UINT readbackWidth = source.Width;
        UINT readbackHeight = source.Height;
        DXGI_FORMAT readbackFormat = source.Format;
        if (gpuScalingAllowed_ && requestedHeight_ > 0 && requestedHeight_ < source.Height) {
            readbackHeight = requestedHeight_;
            readbackWidth = Even(static_cast<UINT>((static_cast<std::uint64_t>(source.Width) * readbackHeight +
                                                    source.Height / 2) / source.Height));
            std::wstring scaleError;
            if (ConfigureGpuScaler(source, readbackWidth, readbackHeight, scaleError)) {
                readbackFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
                AppendDiagnosticLog(L"GPU downscale active · " + std::to_wstring(source.Width) + L"x" +
                                    std::to_wstring(source.Height) + L" -> " + std::to_wstring(readbackWidth) + L"x" +
                                    std::to_wstring(readbackHeight));
            } else {
                gpuScalingAllowed_ = false;
                readbackWidth = source.Width;
                readbackHeight = source.Height;
                readbackFormat = source.Format;
                AppendDiagnosticLog(L"GPU downscale unavailable; native readback fallback: " + scaleError);
            }
        }

        D3D11_TEXTURE2D_DESC staging = source;
        staging.Width = readbackWidth;
        staging.Height = readbackHeight;
        staging.Format = readbackFormat;
        staging.MipLevels = 1;
        staging.ArraySize = 1;
        staging.SampleDesc.Count = 1;
        staging.SampleDesc.Quality = 0;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        HRESULT hr = S_OK;
        for (auto& surface : staging_) {
            hr = device_->CreateTexture2D(&staging, nullptr, &surface);
            if (FAILED(hr)) break;
        }
        if (FAILED(hr)) { error = L"Create recording staging textures: " + HResultText(hr); return false; }

        sourceTextureWidth_ = source.Width;
        sourceTextureHeight_ = source.Height;
        sourceTextureFormat_ = source.Format;
        nativeWidth_ = readbackWidth;
        nativeHeight_ = readbackHeight;
        format_ = readbackFormat;
        nativeFrameReady_ = false;
        if (width_ != 0 && height_ != 0 && dibBits_) return true;

        width_ = readbackWidth;
        height_ = readbackHeight;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = static_cast<LONG>(width_);
        info.bmiHeader.biHeight = -static_cast<LONG>(height_);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        dibDc_ = CreateCompatibleDC(nullptr);
        dib_ = CreateDIBSection(dibDc_, &info, DIB_RGB_COLORS, &dibBits_, nullptr, 0);
        if (!dibDc_ || !dib_ || !dibBits_) { error = L"Create recording frame surface failed"; return false; }
        dibOld_ = SelectObject(dibDc_, dib_);
        contentRect_ = RECT{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        return true;
    }

    void ScaleNativeFrame() {
        if (!dibDc_ || !dibBits_ || sourceFrame_.empty() || nativeWidth_ == 0 || nativeHeight_ == 0) return;
        PatBlt(dibDc_, 0, 0, static_cast<int>(width_), static_cast<int>(height_), BLACKNESS);
        const double scale = (std::min)(static_cast<double>(width_) / nativeWidth_,
                                        static_cast<double>(height_) / nativeHeight_);
        const int scaledWidth = (std::max)(1, static_cast<int>(nativeWidth_ * scale));
        const int scaledHeight = (std::max)(1, static_cast<int>(nativeHeight_ * scale));
        const int x = (static_cast<int>(width_) - scaledWidth) / 2;
        const int y = (static_cast<int>(height_) - scaledHeight) / 2;
        contentRect_ = RECT{x, y, x + scaledWidth, y + scaledHeight};

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = static_cast<LONG>(nativeWidth_);
        info.bmiHeader.biHeight = -static_cast<LONG>(nativeHeight_);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dibDc_, HALFTONE);
        SetBrushOrgEx(dibDc_, 0, 0, nullptr);
        StretchDIBits(dibDc_, x, y, scaledWidth, scaledHeight, 0, 0,
                      static_cast<int>(nativeWidth_), static_cast<int>(nativeHeight_),
                      sourceFrame_.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    }

    void RestoreCursorBackground() {
        if (!cursorBackupValid_ || !dibBits_) return;
        const int backupWidth = cursorBackupRect_.right - cursorBackupRect_.left;
        const int backupHeight = cursorBackupRect_.bottom - cursorBackupRect_.top;
        if (backupWidth <= 0 || backupHeight <= 0 ||
            cursorBackup_.size() != static_cast<std::size_t>(backupWidth) * backupHeight * 4) {
            cursorBackupValid_ = false;
            return;
        }
        auto* destination = static_cast<BYTE*>(dibBits_);
        for (int row = 0; row < backupHeight; ++row) {
            std::memcpy(destination + (static_cast<std::size_t>(cursorBackupRect_.top + row) * width_ +
                                       static_cast<UINT>(cursorBackupRect_.left)) * 4,
                        cursorBackup_.data() + static_cast<std::size_t>(row) * backupWidth * 4,
                        static_cast<std::size_t>(backupWidth) * 4);
        }
        cursorBackupValid_ = false;
    }

    bool BackupCursorBackground(int left, int top, int right, int bottom) {
        if (!dibBits_) return false;
        cursorBackupRect_ = RECT{(std::max)(0, left), (std::max)(0, top),
                                 (std::min)(static_cast<int>(width_), right),
                                 (std::min)(static_cast<int>(height_), bottom)};
        const int backupWidth = cursorBackupRect_.right - cursorBackupRect_.left;
        const int backupHeight = cursorBackupRect_.bottom - cursorBackupRect_.top;
        if (backupWidth <= 0 || backupHeight <= 0) {
            cursorBackupValid_ = false;
            return false;
        }
        cursorBackup_.resize(static_cast<std::size_t>(backupWidth) * backupHeight * 4);
        const auto* source = static_cast<const BYTE*>(dibBits_);
        for (int row = 0; row < backupHeight; ++row) {
            std::memcpy(cursorBackup_.data() + static_cast<std::size_t>(row) * backupWidth * 4,
                        source + (static_cast<std::size_t>(cursorBackupRect_.top + row) * width_ +
                                  static_cast<UINT>(cursorBackupRect_.left)) * 4,
                        static_cast<std::size_t>(backupWidth) * 4);
        }
        cursorBackupValid_ = true;
        return true;
    }

    void DrawCursor() {
        if (DrawSystemCursor()) {
            if (!cursorRenderLogged_) {
                AppendDiagnosticLog(L"Recording cursor compositor active (Win32 cursor, refreshed every frame)");
                cursorRenderLogged_ = true;
            }
            return;
        }
        if (pointerVisible_ && !pointerShape_.empty() && DrawCapturedPointer()) {
            if (!cursorRenderLogged_) {
                AppendDiagnosticLog(L"Recording cursor compositor active (DXGI hardware pointer)");
                cursorRenderLogged_ = true;
            }
            return;
        }
    }

    bool DrawSystemCursor() {
        CURSORINFO cursor{sizeof(cursor)};
        if (!GetCursorInfo(&cursor) || (cursor.flags & CURSOR_SHOWING) == 0 || !cursor.hCursor) return false;
        if (cursor.ptScreenPos.x < target_.desktopRect.left || cursor.ptScreenPos.x >= target_.desktopRect.right ||
            cursor.ptScreenPos.y < target_.desktopRect.top || cursor.ptScreenPos.y >= target_.desktopRect.bottom) return false;
        if (cursor.hCursor != cachedSystemCursor_) {
            ICONINFO icon{};
            if (!GetIconInfo(cursor.hCursor, &icon)) return false;
            cachedSystemCursor_ = cursor.hCursor;
            cachedCursorHotspotX_ = icon.xHotspot;
            cachedCursorHotspotY_ = icon.yHotspot;
            cachedCursorWidth_ = GetSystemMetrics(SM_CXCURSOR);
            cachedCursorHeight_ = GetSystemMetrics(SM_CYCURSOR);
            BITMAP bitmap{};
            if (icon.hbmColor && GetObjectW(icon.hbmColor, sizeof(bitmap), &bitmap) == sizeof(bitmap)) {
                cachedCursorWidth_ = bitmap.bmWidth;
                cachedCursorHeight_ = bitmap.bmHeight;
            } else if (icon.hbmMask && GetObjectW(icon.hbmMask, sizeof(bitmap), &bitmap) == sizeof(bitmap)) {
                cachedCursorWidth_ = bitmap.bmWidth;
                cachedCursorHeight_ = bitmap.bmHeight / 2;
            }
            if (icon.hbmColor) DeleteObject(icon.hbmColor);
            if (icon.hbmMask) DeleteObject(icon.hbmMask);
        }
        const int desktopWidth = (std::max)(1L, target_.desktopRect.right - target_.desktopRect.left);
        const int desktopHeight = (std::max)(1L, target_.desktopRect.bottom - target_.desktopRect.top);
        const double scaleX = static_cast<double>(contentRect_.right - contentRect_.left) / desktopWidth;
        const double scaleY = static_cast<double>(contentRect_.bottom - contentRect_.top) / desktopHeight;
        const int cursorWidth = (std::max)(1, static_cast<int>(cachedCursorWidth_ * scaleX));
        const int cursorHeight = (std::max)(1, static_cast<int>(cachedCursorHeight_ * scaleY));
        const int x = contentRect_.left + static_cast<int>((cursor.ptScreenPos.x - target_.desktopRect.left) * scaleX) -
                      static_cast<int>(cachedCursorHotspotX_ * scaleX);
        const int y = contentRect_.top + static_cast<int>((cursor.ptScreenPos.y - target_.desktopRect.top) * scaleY) -
                      static_cast<int>(cachedCursorHotspotY_ * scaleY);
        BackupCursorBackground(x, y, x + cursorWidth, y + cursorHeight);
        const bool drawn = DrawIconEx(dibDc_, x, y, cursor.hCursor, cursorWidth, cursorHeight,
                                      0, nullptr, DI_NORMAL) != FALSE;
        GdiFlush();
        return drawn;
    }

    bool DrawCapturedPointer() {
        if (!dibBits_ || pointerShapeInfo_.Width == 0 || pointerShapeInfo_.Height == 0) return false;
        const UINT sourceWidth = pointerShapeInfo_.Width;
        const UINT sourceHeight = pointerShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME
            ? pointerShapeInfo_.Height / 2 : pointerShapeInfo_.Height;
        if (sourceHeight == 0) return false;
        const int desktopWidth = (std::max)(1L, target_.desktopRect.right - target_.desktopRect.left);
        const int desktopHeight = (std::max)(1L, target_.desktopRect.bottom - target_.desktopRect.top);
        const double scaleX = static_cast<double>(contentRect_.right - contentRect_.left) / desktopWidth;
        const double scaleY = static_cast<double>(contentRect_.bottom - contentRect_.top) / desktopHeight;
        const int destinationX = contentRect_.left + static_cast<int>(pointerPosition_.x * scaleX);
        const int destinationY = contentRect_.top + static_cast<int>(pointerPosition_.y * scaleY);
        const int destinationWidth = (std::max)(1, static_cast<int>(sourceWidth * scaleX));
        const int destinationHeight = (std::max)(1, static_cast<int>(sourceHeight * scaleY));
        if (!BackupCursorBackground(destinationX, destinationY,
                                    destinationX + destinationWidth, destinationY + destinationHeight)) return false;
        auto* desktop = static_cast<BYTE*>(dibBits_);
        bool drew = false;
        for (int dy = 0; dy < destinationHeight; ++dy) {
            const int y = destinationY + dy;
            if (y < contentRect_.top || y >= contentRect_.bottom || y < 0 || y >= static_cast<int>(height_)) continue;
            const UINT sy = (std::min)(sourceHeight - 1, static_cast<UINT>(static_cast<std::uint64_t>(dy) * sourceHeight / destinationHeight));
            for (int dx = 0; dx < destinationWidth; ++dx) {
                const int x = destinationX + dx;
                if (x < contentRect_.left || x >= contentRect_.right || x < 0 || x >= static_cast<int>(width_)) continue;
                const UINT sx = (std::min)(sourceWidth - 1, static_cast<UINT>(static_cast<std::uint64_t>(dx) * sourceWidth / destinationWidth));
                BYTE* destination = desktop + (static_cast<std::size_t>(y) * width_ + static_cast<UINT>(x)) * 4;
                if (pointerShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR ||
                    pointerShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                    const std::size_t offset = static_cast<std::size_t>(sy) * pointerShapeInfo_.Pitch + static_cast<std::size_t>(sx) * 4;
                    if (offset + 3 >= pointerShape_.size()) continue;
                    const BYTE* source = pointerShape_.data() + offset;
                    if (pointerShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                        if (source[3] == 0) {
                            destination[0] = source[0]; destination[1] = source[1]; destination[2] = source[2];
                        } else {
                            destination[0] ^= source[0]; destination[1] ^= source[1]; destination[2] ^= source[2];
                        }
                    } else {
                        const UINT alpha = source[3];
                        const UINT inverse = 255 - alpha;
                        destination[0] = static_cast<BYTE>((source[0] * alpha + destination[0] * inverse + 127) / 255);
                        destination[1] = static_cast<BYTE>((source[1] * alpha + destination[1] * inverse + 127) / 255);
                        destination[2] = static_cast<BYTE>((source[2] * alpha + destination[2] * inverse + 127) / 255);
                    }
                    destination[3] = 255;
                    drew = true;
                } else if (pointerShapeInfo_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                    const std::size_t byteIndex = static_cast<std::size_t>(sy) * pointerShapeInfo_.Pitch + sx / 8;
                    const std::size_t xorIndex = static_cast<std::size_t>(sy + sourceHeight) * pointerShapeInfo_.Pitch + sx / 8;
                    if (byteIndex >= pointerShape_.size() || xorIndex >= pointerShape_.size()) continue;
                    const BYTE mask = static_cast<BYTE>(0x80u >> (sx & 7u));
                    const bool andBit = (pointerShape_[byteIndex] & mask) != 0;
                    const bool xorBit = (pointerShape_[xorIndex] & mask) != 0;
                    for (int channel = 0; channel < 3; ++channel) {
                        BYTE value = andBit ? destination[channel] : 0;
                        if (xorBit) value ^= 0xff;
                        destination[channel] = value;
                    }
                    destination[3] = 255;
                    drew = true;
                }
            }
        }
        return drew;
    }

    void ResetCaptureObjects() {
        for (auto& surface : staging_) surface.Reset();
        nativeCopy_.Reset();
        nativeCopyStaging_.Reset();
        nativeCopyDesc_ = {};
        nativeCopyValid_ = false;
        ResetGpuScaler();
        stagingReadIndex_ = 0;
        stagingWriteIndex_ = 0;
        pendingStagingCopies_ = 0;
        duplication_.Reset();
        context_.Reset();
        device_.Reset();
        target_ = {};
        nativeWidth_ = nativeHeight_ = 0;
        sourceTextureWidth_ = sourceTextureHeight_ = 0;
        sourceTextureFormat_ = DXGI_FORMAT_UNKNOWN;
        format_ = DXGI_FORMAT_UNKNOWN;
        nativeFrameReady_ = false;
        pointerShape_.clear();
        pointerShapeInfo_ = {};
        pointerVisible_ = false;
        cachedSystemCursor_ = nullptr;
        cachedCursorHotspotX_ = cachedCursorHotspotY_ = 0;
        cachedCursorWidth_ = cachedCursorHeight_ = 0;
    }

    void Reset() {
        ResetCaptureObjects();
        if (dibDc_ && dibOld_) { SelectObject(dibDc_, dibOld_); dibOld_ = nullptr; }
        if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
        if (dibDc_) { DeleteDC(dibDc_); dibDc_ = nullptr; }
        dibBits_ = nullptr;
        width_ = height_ = 0;
        sourceFrame_.clear();
        cursorBackup_.clear();
        cursorBackupRect_ = {};
        cursorBackupValid_ = false;
        contentRect_ = {};
        captureDesktopRect_ = {};
        captureMonitor_ = nullptr;
        captureDeviceName_.clear();
        captureAdapterName_.clear();
        lastCaptureError_.clear();
        hasFrame_ = false;
        blackFrameCount_ = 0;
        blackFallbacks_ = 0;
        recovering_ = false;
        nextReconnectTick_ = 0;
        preferLegacyDuplication_ = false;
        usingModernDuplication_ = false;
        cursorRenderLogged_ = false;
        gpuScalingAllowed_ = true;
    }

    DisplayTarget target_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    std::array<ComPtr<ID3D11Texture2D>, 3> staging_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11VideoProcessorEnumerator> videoEnumerator_;
    ComPtr<ID3D11VideoProcessor> videoProcessor_;
    ComPtr<ID3D11Texture2D> scaledFrame_;
    ComPtr<ID3D11VideoProcessorOutputView> scaledOutputView_;
    ComPtr<ID3D11Texture2D> nativeCopy_;
    ComPtr<ID3D11Texture2D> nativeCopyStaging_;
    D3D11_TEXTURE2D_DESC nativeCopyDesc_{};
    bool nativeCopyValid_ = false;
    HDC dibDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ dibOld_ = nullptr;
    void* dibBits_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT nativeWidth_ = 0;
    UINT nativeHeight_ = 0;
    UINT sourceTextureWidth_ = 0;
    UINT sourceTextureHeight_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT sourceTextureFormat_ = DXGI_FORMAT_UNKNOWN;
    std::vector<BYTE> sourceFrame_;
    std::vector<BYTE> cursorBackup_;
    std::vector<BYTE> pointerShape_;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointerShapeInfo_{};
    POINT pointerPosition_{};
    RECT contentRect_{};
    RECT captureDesktopRect_{};
    HMONITOR captureMonitor_ = nullptr;
    std::wstring captureDeviceName_;
    std::wstring captureAdapterName_;
    std::wstring lastCaptureError_;
    int requestedIndex_ = 0;
    UINT requestedHeight_ = 0;
    CapturePriority priority_ = CapturePriority::Balanced;
    ULONGLONG nextReconnectTick_ = 0;
    std::size_t stagingReadIndex_ = 0;
    std::size_t stagingWriteIndex_ = 0;
    std::size_t pendingStagingCopies_ = 0;
    bool hasFrame_ = false;
    bool nativeFrameReady_ = false;
    bool recovering_ = false;
    UINT blackFrameCount_ = 0;
    UINT blackFallbacks_ = 0;
    bool preferLegacyDuplication_ = false;
    bool usingModernDuplication_ = false;
    bool pointerVisible_ = false;
    bool gpuScaling_ = false;
    bool gpuScalingAllowed_ = true;
    bool cursorRenderLogged_ = false;
    HCURSOR cachedSystemCursor_ = nullptr;
    DWORD cachedCursorHotspotX_ = 0;
    DWORD cachedCursorHotspotY_ = 0;
    int cachedCursorWidth_ = 0;
    int cachedCursorHeight_ = 0;
    bool cursorBackupValid_ = false;
    RECT cursorBackupRect_{};
};

class LoopbackCapture {
public:
    ~LoopbackCapture() { Stop(); if (format_) CoTaskMemFree(format_); }

    bool Initialize(std::wstring& error) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) { error = L"WASAPI device enumerator: " + HResultText(hr); return false; }
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
        if (FAILED(hr)) { error = L"No default system audio output"; return false; }
        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(audioClient_.GetAddressOf()));
        if (FAILED(hr)) { error = L"WASAPI audio client: " + HResultText(hr); return false; }
        hr = audioClient_->GetMixFormat(&format_);
        if (FAILED(hr) || !format_) { error = L"WASAPI mix format is unavailable"; return false; }
        if (format_->nChannels == 0 || format_->nChannels > 2 || format_->nSamplesPerSec == 0) {
            error = L"System audio format must be mono or stereo";
            return false;
        }
        hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                      0, 0, format_, nullptr);
        if (FAILED(hr)) { error = L"WASAPI loopback initialization: " + HResultText(hr); return false; }
        hr = audioClient_->GetService(IID_PPV_ARGS(&captureClient_));
        if (FAILED(hr)) { error = L"WASAPI capture service: " + HResultText(hr); return false; }
        return true;
    }

    bool ConfigureWriter(IMFSinkWriter* writer, DWORD& streamIndex, std::wstring& error) {
        if (!writer || !format_) return false;
        ComPtr<IMFMediaType> output;
        HRESULT hr = MFCreateMediaType(&output);
        if (SUCCEEDED(hr)) hr = output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, format_->nChannels);
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, format_->nSamplesPerSec);
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24'000);
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        if (SUCCEEDED(hr)) hr = writer->AddStream(output.Get(), &streamIndex);
        if (FAILED(hr)) { error = L"Configure AAC output: " + HResultText(hr); return false; }

        GUID subtype = MFAudioFormat_PCM;
        if (format_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) subtype = MFAudioFormat_Float;
        if (format_->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format_->cbSize >= 22) {
            const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format_);
            subtype = extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT ? MFAudioFormat_Float : MFAudioFormat_PCM;
        }
        ComPtr<IMFMediaType> input;
        hr = MFCreateMediaType(&input);
        if (SUCCEEDED(hr)) hr = input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = input->SetGUID(MF_MT_SUBTYPE, subtype);
        if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, format_->nChannels);
        if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, format_->nSamplesPerSec);
        if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, format_->nBlockAlign);
        if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, format_->nAvgBytesPerSec);
        if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, format_->wBitsPerSample);
        if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(streamIndex, input.Get(), nullptr);
        if (FAILED(hr)) { error = L"Configure system audio input: " + HResultText(hr); return false; }
        return true;
    }

    bool Start(std::wstring& error) {
        audioFrames_ = 0;
        const HRESULT hr = audioClient_->Start();
        if (FAILED(hr)) { error = L"Start system audio capture: " + HResultText(hr); return false; }
        started_ = true;
        return true;
    }

    void Stop() {
        if (audioClient_ && started_) audioClient_->Stop();
        started_ = false;
    }

    bool Drain(IMFSinkWriter* writer, DWORD streamIndex, std::wstring& error) {
        if (!started_ || !captureClient_ || !writer || !format_) return true;
        for (;;) {
            UINT32 packetFrames = 0;
            HRESULT hr = captureClient_->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) { error = L"Read system audio packet: " + HResultText(hr); return false; }
            if (packetFrames == 0) return true;
            BYTE* data = nullptr;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;
            hr = captureClient_->GetBuffer(&data, &packetFrames, &flags, &devicePosition, &qpcPosition);
            if (FAILED(hr)) { error = L"Lock system audio packet: " + HResultText(hr); return false; }
            const DWORD bytes = packetFrames * format_->nBlockAlign;
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMFSample> sample;
            BYTE* destination = nullptr;
            HRESULT writeHr = MFCreateMemoryBuffer(bytes, &buffer);
            if (SUCCEEDED(writeHr)) writeHr = buffer->Lock(&destination, nullptr, nullptr);
            if (SUCCEEDED(writeHr)) {
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data) std::memset(destination, 0, bytes);
                else std::memcpy(destination, data, bytes);
                buffer->Unlock();
                writeHr = buffer->SetCurrentLength(bytes);
            }
            if (SUCCEEDED(writeHr)) writeHr = MFCreateSample(&sample);
            if (SUCCEEDED(writeHr)) writeHr = sample->AddBuffer(buffer.Get());
            const LONGLONG time = static_cast<LONGLONG>(audioFrames_ * kHundredNsPerSecond / format_->nSamplesPerSec);
            const LONGLONG duration = static_cast<LONGLONG>(static_cast<UINT64>(packetFrames) * kHundredNsPerSecond / format_->nSamplesPerSec);
            if (SUCCEEDED(writeHr)) writeHr = sample->SetSampleTime(time);
            if (SUCCEEDED(writeHr)) writeHr = sample->SetSampleDuration(duration);
            if (SUCCEEDED(writeHr)) writeHr = writer->WriteSample(streamIndex, sample.Get());
            captureClient_->ReleaseBuffer(packetFrames);
            if (FAILED(writeHr)) { error = L"Encode system audio: " + HResultText(writeHr); return false; }
            audioFrames_ += packetFrames;
        }
    }

private:
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> audioClient_;
    ComPtr<IAudioCaptureClient> captureClient_;
    WAVEFORMATEX* format_ = nullptr;
    UINT64 audioFrames_ = 0;
    bool started_ = false;
};

class Mp4Writer {
public:
    bool Open(const std::wstring& path, UINT sourceWidth, UINT sourceHeight,
              UINT outputWidth, UINT outputHeight, int fps, int bitrateMbps,
              VideoCodec codec, AudioMixer* audio, const AppSettings& tuning, std::wstring& error) {
        Close(nullptr);
        priority_ = tuning.capturePriority;
        rateControl_ = tuning.videoRateControl;
        effort_ = tuning.videoEncoderEffort;
        quality_ = tuning.videoQuality;
        keyframeSeconds_ = tuning.keyframeSeconds;
        sourceWidth_ = sourceWidth;
        sourceHeight_ = sourceHeight;
        fps_ = fps;
        frameDuration_ = kHundredNsPerSecond / fps_;
        audio_ = audio;
        audioWarning_.clear();

        ComPtr<IMFAttributes> attributes;
        HRESULT hr = MFCreateAttributes(&attributes, 5);
        // "CPU priority" deliberately refuses the hardware encoder so the GPU is
        // left entirely to the game; Media Foundation then falls back to its
        // software encoder. Every other mode keeps the hardware path.
        const BOOL allowHardwareEncoder = priority_ != CapturePriority::Cpu ? TRUE : FALSE;
        if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, allowHardwareEncoder);
        // File recording benefits from encoder pipelining. Low-latency mode is
        // meant for calls/streaming and makes WriteSample block the capture loop
        // more often, which shows up as frametime spikes in games.
        if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_LOW_LATENCY, FALSE);
        // Preserve sink-writer backpressure so raw BGRA frames cannot build an
        // unbounded queue when the encoder or disk is briefly slower than capture.
        if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, FALSE);
        const VideoCodecProfile& profile = CodecProfile(codec);
        if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, profile.container);
        if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attributes.Get(), &writer_);
        if (FAILED(hr)) { error = L"Create video writer: " + HResultText(hr); return false; }

        ComPtr<IMFMediaType> output;
        const GUID& videoSubtype = profile.subtype;
        const wchar_t* codecName = profile.name;
        hr = MFCreateMediaType(&output);
        if (SUCCEEDED(hr)) hr = output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = output->SetGUID(MF_MT_SUBTYPE, videoSubtype);
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrateMbps * 1'000'000));
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(hr)) hr = MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, outputWidth, outputHeight);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps_), 1);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (SUCCEEDED(hr)) hr = writer_->AddStream(output.Get(), &videoStream_);
        if (FAILED(hr)) {
            error = std::wstring(L"Configure ") + codecName + L" output: " + HResultText(hr) +
                    L". Choose H.264 if this Windows/GPU driver has no encoder for the selected codec.";
            return false;
        }

        ComPtr<IMFMediaType> input;
        hr = MFCreateMediaType(&input);
        if (SUCCEEDED(hr)) hr = input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
        if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_DEFAULT_STRIDE, sourceWidth_ * 4);
        if (SUCCEEDED(hr)) hr = MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, sourceWidth_, sourceHeight_);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps_), 1);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (SUCCEEDED(hr)) hr = writer_->SetInputMediaType(videoStream_, input.Get(), nullptr);
        if (FAILED(hr)) { error = L"Configure desktop frame input: " + HResultText(hr); return false; }

        ApplyEncoderTuning();

        hasAudio_ = false;
        if (audio_) {
            std::wstring audioError;
            hasAudio_ = audio_->ConfigureWriter(writer_.Get(), audioStream_, audioError);
            if (!hasAudio_ && !audioError.empty()) audioWarning_ = L"Audio encoder unavailable: " + audioError;
        }
        hr = writer_->BeginWriting();
        if (FAILED(hr)) { error = L"Start MP4 writer: " + HResultText(hr); return false; }
        if (hasAudio_) {
            std::wstring startError;
            if (!audio_->Start(startError)) hasAudio_ = false;
            if (!startError.empty()) audioWarning_ = L"Audio source could not start: " + startError;
        }
        frameIndex_ = 0;
        open_ = true;
        return true;
    }

    // Rate control, quality and GOP length are encoder-side knobs, reachable only
    // once the stream exists. Every one of these is optional: hardware encoders
    // vary wildly in what they implement, so a rejected property is left at the
    // driver's default instead of failing the recording.
    void ApplyEncoderTuning() {
        if (!writer_) return;
        ComPtr<ICodecAPI> codec;
        if (FAILED(writer_->GetServiceForStream(videoStream_, GUID_NULL, IID_PPV_ARGS(&codec))) || !codec) {
            return;
        }
        const auto trySet = [&codec](const GUID& property, ULONG value) {
            VARIANT variant{};
            variant.vt = VT_UI4;
            variant.ulVal = value;
            codec->SetValue(&property, &variant);
        };

        switch (rateControl_) {
            case VideoRateControl::Cbr:
                trySet(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
                break;
            case VideoRateControl::Quality:
                trySet(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality);
                trySet(CODECAPI_AVEncCommonQuality, static_cast<ULONG>((std::max)(1, (std::min)(100, quality_))));
                break;
            case VideoRateControl::Vbr:
            default:
                trySet(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_UnconstrainedVBR);
                break;
        }

        // 0-100, where 100 is "spend the most time for the best picture".
        const ULONG effort = effort_ == VideoEncoderEffort::Speed ? 0u :
                             effort_ == VideoEncoderEffort::Quality ? 100u : 50u;
        trySet(CODECAPI_AVEncCommonQualityVsSpeed, effort);

        // A shorter GOP costs bitrate but makes seeking and clipping far nicer,
        // which matters for Replay clips people cut up afterwards.
        if (keyframeSeconds_ > 0 && fps_ > 0) {
            trySet(CODECAPI_AVEncMPVGOPSize, static_cast<ULONG>(keyframeSeconds_ * fps_));
        }
    }

    bool WriteVideo(const BYTE* bgra, std::wstring& error) {
        if (!open_ || !writer_ || !bgra) return false;
        const DWORD bytes = sourceWidth_ * sourceHeight_ * 4;
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> sample;
        BYTE* destination = nullptr;
        HRESULT hr = MFCreateMemoryBuffer(bytes, &buffer);
        if (SUCCEEDED(hr)) hr = buffer->Lock(&destination, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            std::memcpy(destination, bgra, bytes);
            buffer->Unlock();
            hr = buffer->SetCurrentLength(bytes);
        }
        if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
        if (SUCCEEDED(hr)) hr = sample->SetSampleTime(static_cast<LONGLONG>(frameIndex_) * frameDuration_);
        if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(frameDuration_);
        if (SUCCEEDED(hr)) hr = writer_->WriteSample(videoStream_, sample.Get());
        if (FAILED(hr)) { error = L"Encode desktop frame: " + HResultText(hr); return false; }
        ++frameIndex_;
        return true;
    }

    bool DrainAudio(std::wstring& error) {
        return !hasAudio_ || audio_->Drain(writer_.Get(), audioStream_, error);
    }

    void SkipFrames(std::uint64_t count) noexcept { frameIndex_ += count; }

    bool Close(std::wstring* error) {
        if (!writer_) return true;
        if (hasAudio_ && audio_) {
            std::wstring drainError;
            audio_->Drain(writer_.Get(), audioStream_, drainError);
            audio_->Stop();
        }
        const HRESULT hr = open_ ? writer_->Finalize() : S_OK;
        writer_.Reset();
        open_ = false;
        hasAudio_ = false;
        audio_ = nullptr;
        if (FAILED(hr)) {
            if (error) *error = L"Finalize MP4: " + HResultText(hr);
            return false;
        }
        return true;
    }

    std::uint64_t FrameCount() const noexcept { return frameIndex_; }
    std::wstring TakeAudioWarning() { return std::exchange(audioWarning_, std::wstring{}); }
    std::uint64_t DurationMs() const noexcept {
        return static_cast<std::uint64_t>(frameIndex_) * 1000 / static_cast<std::uint64_t>((std::max)(1, fps_));
    }

private:
    ComPtr<IMFSinkWriter> writer_;
    DWORD videoStream_ = 0;
    DWORD audioStream_ = 0;
    UINT sourceWidth_ = 0;
    UINT sourceHeight_ = 0;
    CapturePriority priority_ = CapturePriority::Balanced;
    VideoRateControl rateControl_ = VideoRateControl::Vbr;
    VideoEncoderEffort effort_ = VideoEncoderEffort::Balanced;
    int quality_ = 70;
    int keyframeSeconds_ = 2;
    int fps_ = 60;
    LONGLONG frameDuration_ = kHundredNsPerSecond / 60;
    std::uint64_t frameIndex_ = 0;
    AudioMixer* audio_ = nullptr;
    bool hasAudio_ = false;
    bool open_ = false;
    std::wstring audioWarning_;
};

struct ReplaySegment {
    std::wstring path;
    std::uint64_t durationMs = 0;
};

bool MergeVideoSegments(const std::vector<std::wstring>& paths, const std::wstring& outputPath,
                        VideoCodec codec, std::wstring& error) {
    if (paths.empty()) return false;
    constexpr DWORD kFirstVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    constexpr DWORD kFirstAudio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    constexpr DWORD kAllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
    constexpr DWORD kAnyStream = static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM);
    ComPtr<IMFSourceReader> firstReader;
    HRESULT hr = MFCreateSourceReaderFromURL(paths.front().c_str(), nullptr, &firstReader);
    if (FAILED(hr)) { error = L"Open replay segment: " + HResultText(hr); return false; }
    ComPtr<IMFMediaType> videoType;
    hr = firstReader->GetNativeMediaType(kFirstVideo, 0, &videoType);
    if (FAILED(hr)) { error = L"Replay segment has no video stream"; return false; }
    ComPtr<IMFMediaType> audioType;
    const bool hasAudio = SUCCEEDED(firstReader->GetNativeMediaType(kFirstAudio, 0, &audioType));

    ComPtr<IMFAttributes> attributes;
    MFCreateAttributes(&attributes, 1);
    attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, CodecProfile(codec).container);
    ComPtr<IMFSinkWriter> writer;
    hr = MFCreateSinkWriterFromURL(outputPath.c_str(), nullptr, attributes.Get(), &writer);
    DWORD videoOutput = 0;
    DWORD audioOutput = 0;
    if (SUCCEEDED(hr)) hr = writer->AddStream(videoType.Get(), &videoOutput);
    if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(videoOutput, videoType.Get(), nullptr);
    if (SUCCEEDED(hr) && hasAudio) hr = writer->AddStream(audioType.Get(), &audioOutput);
    if (SUCCEEDED(hr) && hasAudio) hr = writer->SetInputMediaType(audioOutput, audioType.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = writer->BeginWriting();
    if (FAILED(hr)) { error = L"Create replay clip: " + HResultText(hr); return false; }

    LONGLONG timelineBase = 0;
    for (const auto& path : paths) {
        ComPtr<IMFSourceReader> reader;
        hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
        if (FAILED(hr)) break;
        reader->SetStreamSelection(kAllStreams, FALSE);
        reader->SetStreamSelection(kFirstVideo, TRUE);
        reader->SetCurrentMediaType(kFirstVideo, nullptr, videoType.Get());
        if (hasAudio) {
            reader->SetStreamSelection(kFirstAudio, TRUE);
            reader->SetCurrentMediaType(kFirstAudio, nullptr, audioType.Get());
        }
        LONGLONG segmentEnd = 0;
        for (;;) {
            DWORD actualStream = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            hr = reader->ReadSample(kAnyStream, 0, &actualStream, &flags, &timestamp, &sample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) break;
            if (!sample) continue;
            ComPtr<IMFMediaType> currentType;
            GUID major = GUID_NULL;
            if (SUCCEEDED(reader->GetCurrentMediaType(actualStream, &currentType))) currentType->GetGUID(MF_MT_MAJOR_TYPE, &major);
            LONGLONG duration = 0;
            sample->GetSampleDuration(&duration);
            sample->SetSampleTime(timelineBase + timestamp);
            const DWORD destination = major == MFMediaType_Audio && hasAudio ? audioOutput : videoOutput;
            hr = writer->WriteSample(destination, sample.Get());
            if (FAILED(hr)) break;
            segmentEnd = (std::max)(segmentEnd, timestamp + duration);
        }
        if (FAILED(hr)) break;
        timelineBase += segmentEnd;
    }
    if (SUCCEEDED(hr)) hr = writer->Finalize();
    if (FAILED(hr)) {
        error = L"Assemble replay clip: " + HResultText(hr);
        DeleteFileW(outputPath.c_str());
        return false;
    }
    return true;
}

struct ActiveSession {
    enum class Mode { Idle, Recording, Replay } mode = Mode::Idle;
    std::uint64_t generation = 0;
    AppSettings settings;
    DesktopRecorderSource source;
    std::unique_ptr<AudioMixer> audio;
    Mp4Writer writer;
    std::wstring currentPath;
    std::deque<ReplaySegment> segments;
    std::uint64_t totalFrames = 0;
    std::uint64_t droppedFrames = 0;
    ULONGLONG sessionStarted = 0;
    ULONGLONG segmentStarted = 0;
    ULONGLONG lastStats = 0;
    ULONGLONG lastPreview = 0;
    BgraPreviewScaler previewScaler;
    int segmentSerial = 0;
    bool sourceRecovering = false;
};

void RemoveReplaySegments(ActiveSession& session) {
    for (const auto& segment : session.segments) DeleteFileW(segment.path.c_str());
    session.segments.clear();
    if (!session.currentPath.empty()) DeleteFileW(session.currentPath.c_str());
}

void CalculateOutputSize(UINT sourceWidth, UINT sourceHeight, int requestedHeight, UINT& outputWidth, UINT& outputHeight) {
    if (requestedHeight <= 0 || sourceHeight <= static_cast<UINT>(requestedHeight)) {
        outputWidth = Even(sourceWidth);
        outputHeight = Even(sourceHeight);
        return;
    }
    outputHeight = Even(static_cast<UINT>(requestedHeight));
    outputWidth = Even(static_cast<UINT>((static_cast<std::uint64_t>(sourceWidth) * outputHeight) / sourceHeight));
}

int ReplaySegmentSeconds(int replaySeconds) {
    replaySeconds = (std::max)(5, (std::min)(3600, replaySeconds));
    if (replaySeconds <= 10) return replaySeconds;
    const int largestUseful = (std::max)(2, (std::min)(20, replaySeconds / 3));
    for (int seconds = largestUseful; seconds >= 2; --seconds) {
        if (replaySeconds % seconds == 0) return seconds;
    }
    return 2;
}
}  // namespace

RecordingEngine::RecordingEngine(HWND notifyWindow, int channelId) : notifyWindow_(notifyWindow), channelId_(channelId) {}
RecordingEngine::~RecordingEngine() { Stop(); }

std::vector<AudioDeviceInfo> RecordingEngine::EnumerateMicrophones() {
    return AudioMixer::EnumerateMicrophones();
}
float RecordingEngine::QueryMicrophonePeak(const std::wstring& deviceId) {
    return AudioMixer::QueryMicrophonePeak(deviceId);
}
AudioMeterState RecordingEngine::QueryMicrophoneMeter(const std::wstring& deviceId) {
    return AudioMixer::QueryMicrophoneMeter(deviceId);
}

bool RecordingEngine::Start() {
    std::lock_guard lock(mutex_);
    if (running_) return true;
    previewInFlight_.store(false, std::memory_order_release);
    running_ = true;
    try {
        thread_ = std::thread(&RecordingEngine::ThreadMain, this);
    } catch (...) {
        running_ = false;
        return false;
    }
    return true;
}

void RecordingEngine::Stop() {
    {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        commands_.push_back(Command{CommandType::Shutdown, {}});
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_);
    running_ = false;
    commands_.clear();
    recording_.store(false, std::memory_order_release);
    replay_.store(false, std::memory_order_release);
    busy_.store(false, std::memory_order_release);
    previewConsumerActive_.store(false, std::memory_order_release);
    previewInFlight_.store(false, std::memory_order_release);
    {
        std::lock_guard previewLock(previewPoolMutex_);
        recycledPreview_.reset();
    }
}

bool RecordingEngine::Queue(Command command) {
    {
        std::lock_guard lock(mutex_);
        if (!running_) return false;
        commands_.push_back(std::move(command));
    }
    cv_.notify_one();
    return true;
}

bool RecordingEngine::StartRecording(const AppSettings& settings, std::uint64_t* sessionGeneration) {
    if (sessionGeneration) *sessionGeneration = 0;
    if (busy_.exchange(true, std::memory_order_acq_rel)) return false;
    previewFps_.store((std::max)(1, (std::min)(60, settings.previewFps)), std::memory_order_release);
    std::uint64_t generation = nextSessionGeneration_.fetch_add(1, std::memory_order_relaxed);
    if (generation == 0) generation = nextSessionGeneration_.fetch_add(1, std::memory_order_relaxed);
    Command command{CommandType::StartRecording, settings};
    command.sessionGeneration = generation;
    if (!Queue(std::move(command))) { busy_.store(false); return false; }
    if (sessionGeneration) *sessionGeneration = generation;
    return true;
}

bool RecordingEngine::StartReplayBuffer(const AppSettings& settings, std::uint64_t* sessionGeneration) {
    if (sessionGeneration) *sessionGeneration = 0;
    if (busy_.exchange(true, std::memory_order_acq_rel)) return false;
    previewFps_.store((std::max)(1, (std::min)(60, settings.previewFps)), std::memory_order_release);
    std::uint64_t generation = nextSessionGeneration_.fetch_add(1, std::memory_order_relaxed);
    if (generation == 0) generation = nextSessionGeneration_.fetch_add(1, std::memory_order_relaxed);
    Command command{CommandType::StartReplay, settings};
    command.sessionGeneration = generation;
    if (!Queue(std::move(command))) { busy_.store(false); return false; }
    if (sessionGeneration) *sessionGeneration = generation;
    return true;
}

void RecordingEngine::StopActiveSession() { Queue(Command{CommandType::Stop, {}}); }
void RecordingEngine::SaveReplay() { if (replay_.load(std::memory_order_acquire)) Queue(Command{CommandType::SaveReplay, {}}); }
void RecordingEngine::NotifyDisplayChange() { Queue(Command{CommandType::DisplayChanged, {}}); }
bool RecordingEngine::CaptureScreenshot(const AppSettings& settings) {
    if (!recording_.load(std::memory_order_acquire) && !replay_.load(std::memory_order_acquire)) return false;
    return Queue(Command{CommandType::Screenshot, settings});
}

bool RecordingEngine::RequestFrame(CaptureMode mode, std::uint64_t requestId, bool drawCursor) {
    if ((!recording_.load(std::memory_order_acquire) && !replay_.load(std::memory_order_acquire)) || requestId == 0) return false;
    Command command;
    command.type = CommandType::FrameSnapshot;
    command.captureMode = mode;
    command.requestId = requestId;
    command.drawCursor = drawCursor;
    return Queue(std::move(command));
}

void RecordingEngine::SetPreviewFrameRate(int fps) noexcept {
    previewFps_.store((std::max)(1, (std::min)(60, fps)), std::memory_order_release);
}

std::unique_ptr<PreviewFrame> RecordingEngine::TakePreviewFrame() {
    std::lock_guard lock(previewPoolMutex_);
    if (recycledPreview_) return std::move(recycledPreview_);
    return std::make_unique<PreviewFrame>();
}

void RecordingEngine::RecyclePreviewFrame(std::unique_ptr<PreviewFrame> frame) {
    if (!frame || !frame->liveRecording || !previewConsumerActive_.load(std::memory_order_acquire)) return;
    std::lock_guard lock(previewPoolMutex_);
    if (!recycledPreview_) recycledPreview_ = std::move(frame);
}

void RecordingEngine::SetPreviewConsumerActive(bool active) {
    if (previewConsumerActive_.exchange(active, std::memory_order_acq_rel) == active) return;
    if (!active) {
        std::lock_guard lock(previewPoolMutex_);
        recycledPreview_.reset();
    }
}

void RecordingEngine::PostEvent(RecordingEvent event) const {
    event.channelId = channelId_;
    auto* heapEvent = new (std::nothrow) RecordingEvent(std::move(event));
    if (!heapEvent) return;
    if (!PostMessageW(notifyWindow_, WM_APP_RECORDING_EVENT, 0, reinterpret_cast<LPARAM>(heapEvent))) delete heapEvent;
}

void RecordingEngine::PostCaptureEvent(EngineEvent event) const {
    event.channelId = channelId_;
    auto* heapEvent = new (std::nothrow) EngineEvent(std::move(event));
    if (!heapEvent) return;
    if (!PostMessageW(notifyWindow_, WM_APP_ENGINE_EVENT, 0, reinterpret_cast<LPARAM>(heapEvent))) delete heapEvent;
}

void RecordingEngine::ThreadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Never outrank the game's render threads. If the machine is saturated we
    // prefer a repeated recording frame over stuttering the foreground game.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    DWORD mmcssTaskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Capture", &mmcssTaskIndex);
    if (mmcssHandle) AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_NORMAL);
    const HRESULT mfResult = MFStartup(MF_VERSION);
    if (FAILED(mfResult)) {
        PostEvent({RecordingEventType::Error, L"Media Foundation startup failed: " + HResultText(mfResult)});
        busy_.store(false, std::memory_order_release);
        if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
        CoUninitialize();
        return;
    }

    ActiveSession session;
    bool shutdown = false;

    auto postGenerationEvent = [&](RecordingEvent event, std::uint64_t generation, bool replaySession) {
        event.sessionGeneration = generation;
        event.replaySession = replaySession;
        PostEvent(std::move(event));
    };

    auto postSessionEvent = [&](RecordingEvent event) {
        postGenerationEvent(std::move(event), session.generation,
                            session.mode == ActiveSession::Mode::Replay);
    };

    auto resetSessionMetadata = [&]() {
        session.source.Release();
        session.mode = ActiveSession::Mode::Idle;
        session.generation = 0;
        session.settings = {};
        session.audio.reset();
        session.currentPath.clear();
        session.segments.clear();
        session.totalFrames = 0;
        session.droppedFrames = 0;
        session.sessionStarted = 0;
        session.segmentStarted = 0;
        session.lastStats = 0;
        session.lastPreview = 0;
        session.previewScaler.Reset();
        session.segmentSerial = 0;
        session.sourceRecovering = false;
    };

    auto openWriter = [&](const std::wstring& path, std::wstring& error) -> bool {
        UINT outputWidth = 0;
        UINT outputHeight = 0;
        CalculateOutputSize(session.source.Width(), session.source.Height(), session.settings.recordingHeight,
                            outputWidth, outputHeight);
        const bool opened = session.writer.Open(path, session.source.Width(), session.source.Height(), outputWidth, outputHeight,
                                                session.settings.recordingFps, session.settings.recordingBitrateMbps,
                                                session.settings.videoCodec, session.audio.get(),
                                                session.settings, error);
        const std::wstring audioWarning = session.writer.TakeAudioWarning();
        if (!audioWarning.empty()) {
            AppendDiagnosticLog(audioWarning);
            postSessionEvent({RecordingEventType::AudioWarning, audioWarning});
        }
        return opened;
    };

    auto prepareAudio = [&]() {
        session.audio.reset();
        if (!session.settings.recordSystemAudio && !session.settings.recordApplicationAudio &&
            session.settings.microphoneInputs.empty()) return;
        auto audio = std::make_unique<AudioMixer>();
        std::wstring audioWarning;
        if (audio->Initialize(session.settings, audioWarning)) {
            AppendDiagnosticLog(L"Audio mixer ready with " + std::to_wstring(audio->SourceCount()) + L" active sources");
            session.audio = std::move(audio);
        }
        if (!audioWarning.empty()) {
            AppendDiagnosticLog(L"Audio mixer warning: " + audioWarning);
            postSessionEvent({RecordingEventType::AudioWarning, audioWarning});
        }
    };

    auto nextReplayPath = [&]() {
        const std::wstring cache = (std::filesystem::path(GetLocalAppDataDirectory()) / L"ReplayCache").wstring();
        EnsureDirectoryExists(cache);
        const std::wstring segmentPrefix = L"segment_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                                           std::to_wstring(channelId_) + L"_";
        if (session.segmentSerial == 0) {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(cache, ec)) {
                if (ec) break;
                const std::wstring name = entry.path().filename().wstring();
                // Replay cache is session-local. Segments left after a crash cannot be
                // reused and are removed before a new buffer starts.
                if (name.rfind(segmentPrefix, 0) == 0) {
                    std::filesystem::remove(entry.path(), ec);
                    ec.clear();
                }
            }
        }
        return (std::filesystem::path(cache) /
                (segmentPrefix + std::to_wstring(++session.segmentSerial) +
                 CodecProfile(session.settings.videoCodec).extension)).wstring();
    };

    auto startSession = [&](ActiveSession::Mode mode, const AppSettings& settings, std::uint64_t generation) {
        resetSessionMetadata();
        session.mode = mode;
        session.generation = generation;
        session.settings = settings;
        session.settings.recordingFps = (std::max)(1, (std::min)(240, session.settings.recordingFps));
        session.settings.recordingBitrateMbps = (std::max)(1, (std::min)(200, session.settings.recordingBitrateMbps));
        session.settings.replaySeconds = (std::max)(5, (std::min)(3600, session.settings.replaySeconds));
        session.settings.previewFps = (std::max)(1, (std::min)(60, session.settings.previewFps));
        EnsureDirectoryExists(session.settings.recordingDirectory.empty() ? GetDefaultRecordingDirectory() : session.settings.recordingDirectory);
        postSessionEvent({RecordingEventType::Starting, mode == ActiveSession::Mode::Replay ? L"Starting replay buffer…" : L"Starting recorder…"});

        std::wstring error;
        if (!session.source.Initialize(session.settings.recordingMonitorIndex,
                                       session.settings.recordingHeight,
                                       session.settings.capturePriority, error)) {
            postGenerationEvent({RecordingEventType::Error, error}, generation,
                                mode == ActiveSession::Mode::Replay);
            resetSessionMetadata();
            busy_.store(false, std::memory_order_release);
            return;
        }
        AppendDiagnosticLog(L"Recorder source ready: " + session.source.Description());
        prepareAudio();
        const std::wstring directory = session.settings.recordingDirectory.empty() ? GetDefaultRecordingDirectory() : session.settings.recordingDirectory;
        session.currentPath = mode == ActiveSession::Mode::Replay
            ? nextReplayPath()
            : TimestampedPath(directory, L"Recording", session.settings.videoCodec);
        if (!openWriter(session.currentPath, error)) {
            session.writer.Close(nullptr);
            RemoveReplaySegments(session);
            postGenerationEvent({RecordingEventType::Error, error}, generation,
                                mode == ActiveSession::Mode::Replay);
            resetSessionMetadata();
            busy_.store(false, std::memory_order_release);
            return;
        }
        session.sessionStarted = session.segmentStarted = session.lastStats = GetTickCount64();
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        if (mode == ActiveSession::Mode::Recording) {
            recording_.store(true, std::memory_order_release);
            postSessionEvent({RecordingEventType::RecordingStarted, session.source.Description(), session.currentPath});
        } else {
            replay_.store(true, std::memory_order_release);
            postSessionEvent({RecordingEventType::ReplayStarted, session.source.Description()});
        }
        if (session.source.IsRecovering()) {
            session.sourceRecovering = true;
            const std::wstring detail = session.source.RecoveryDetail().empty()
                ? L"Fullscreen capture is unavailable; recording a recovery frame and retrying automatically…"
                : session.source.RecoveryDetail() + L" · retrying automatically…";
            postSessionEvent({RecordingEventType::Recovering, detail});
        }
    };

    auto closeSession = [&](bool postStatus) {
        if (session.mode == ActiveSession::Mode::Idle) return;
        const auto previousMode = session.mode;
        const std::wstring finishedPath = session.currentPath;
        const std::uint64_t elapsed = GetTickCount64() - session.sessionStarted;
        const std::uint64_t frames = session.totalFrames;
        const std::uint64_t dropped = session.droppedFrames;
        const std::uint64_t generation = session.generation;
        std::wstring error;
        session.writer.Close(&error);
        if (previousMode == ActiveSession::Mode::Recording) {
            recording_.store(false, std::memory_order_release);
        } else {
            replay_.store(false, std::memory_order_release);
            RemoveReplaySegments(session);
        }
        SetThreadExecutionState(ES_CONTINUOUS);
        resetSessionMetadata();
        busy_.store(false, std::memory_order_release);
        if (!postStatus) return;
        if (previousMode == ActiveSession::Mode::Recording) {
            postGenerationEvent({RecordingEventType::RecordingStopped, L"Recording stopped · finalization complete"},
                                generation, false);
        }
        if (!error.empty()) {
            postGenerationEvent({RecordingEventType::Error, error}, generation,
                                previousMode == ActiveSession::Mode::Replay);
        } else if (previousMode == ActiveSession::Mode::Recording) {
            postGenerationEvent({RecordingEventType::RecordingSaved, L"Recording saved", finishedPath,
                                 elapsed, frames, dropped}, generation, false);
        } else {
            postGenerationEvent({RecordingEventType::ReplayStopped, L"Replay buffer stopped"}, generation, true);
        }
    };

    auto rotateReplay = [&](std::wstring& error) -> bool {
        const std::uint64_t duration = session.writer.DurationMs();
        if (!session.writer.Close(&error)) return false;
        session.segments.push_back({session.currentPath, duration});
        std::uint64_t total = 0;
        for (const auto& item : session.segments) total += item.durationMs;
        const std::uint64_t keep = static_cast<std::uint64_t>(session.settings.replaySeconds) * 1000;
        while (session.segments.size() > 1 && total > keep &&
               total - session.segments.front().durationMs >= keep) {
            total -= session.segments.front().durationMs;
            DeleteFileW(session.segments.front().path.c_str());
            session.segments.pop_front();
        }
        session.currentPath = nextReplayPath();
        prepareAudio();
        if (!openWriter(session.currentPath, error)) return false;
        session.segmentStarted = GetTickCount64();
        return true;
    };

    auto saveReplay = [&]() {
        if (session.mode != ActiveSession::Mode::Replay) return;
        std::wstring error;
        if (!rotateReplay(error)) {
            const std::uint64_t generation = session.generation;
            closeSession(false);
            postGenerationEvent({RecordingEventType::Error, error}, generation, true);
            return;
        }
        std::vector<std::wstring> selected;
        std::uint64_t total = 0;
        const std::uint64_t limit = static_cast<std::uint64_t>(session.settings.replaySeconds) * 1000;
        for (auto it = session.segments.rbegin(); it != session.segments.rend(); ++it) {
            if (!selected.empty() && total + it->durationMs > limit) break;
            selected.push_back(it->path);
            total += it->durationMs;
        }
        std::reverse(selected.begin(), selected.end());
        const std::wstring directory = session.settings.recordingDirectory.empty() ? GetDefaultRecordingDirectory() : session.settings.recordingDirectory;
        const std::wstring prefix = channelId_ > 0 ? L"Replay_Monitor_" + std::to_wstring(channelId_) : L"Replay";
        const std::wstring output = TimestampedPath(directory, prefix.c_str(), session.settings.videoCodec);
        bool saved = MergeVideoSegments(selected, output, session.settings.videoCodec, error);
        if (!saved && !selected.empty()) {
            saved = CopyFileW(selected.back().c_str(), output.c_str(), FALSE) != FALSE;
            if (saved) error.clear();
        }
        if (saved) {
            // A successful save is a checkpoint. Discard the interim writer opened
            // before the merge and create a fresh video+audio timeline after it. This
            // prevents merge time from appearing as a jump in the next rolling window.
            // If merging fails we deliberately keep the segments, allowing a retry.
            std::wstring restartError;
            session.writer.Close(&restartError);
            DeleteFileW(session.currentPath.c_str());
            for (const auto& segment : session.segments) DeleteFileW(segment.path.c_str());
            session.segments.clear();
            session.currentPath = nextReplayPath();
            prepareAudio();
            if (!restartError.empty() || !openWriter(session.currentPath, restartError)) {
                const std::uint64_t generation = session.generation;
                postSessionEvent({RecordingEventType::ReplaySaved, L"Replay saved · buffer restart failed", output, 0, 0, 0});
                closeSession(false);
                postGenerationEvent({RecordingEventType::Error,
                                     restartError.empty() ? L"Replay buffer could not restart after Save" : restartError},
                                    generation, true);
                return;
            }
            session.sessionStarted = session.segmentStarted = GetTickCount64();
            session.totalFrames = 0;
            session.droppedFrames = 0;
            session.lastStats = session.segmentStarted;
            postSessionEvent({RecordingEventType::ReplaySaved, L"Replay saved · new buffer started at 0:00", output, 0, 0, 0});
        } else {
            postSessionEvent({RecordingEventType::Error, error.empty() ? L"Replay could not be saved" : error});
        }
    };

    auto saveScreenshot = [&](const AppSettings& settings) {
        if (session.mode == ActiveSession::Mode::Idle) return;
        std::wstring error;
        session.source.AcquireLatest(0, error);
        // Prefer the native-resolution snapshot over the encoder-sized DIB so a
        // downscaled replay/recording session still saves full-quality screenshots.
        FrameBuffer native;
        const bool nativeSnapshot = session.source.ReadNativeSnapshot(native);
        const BYTE* pixels = nullptr;
        if (!nativeSnapshot) {
            pixels = session.source.ReadFrame(false, error);
            if (!pixels) {
                pixels = session.source.EnterRecovery(error.empty()
                    ? L"The recorder has no fullscreen frame yet" : error);
            }
            if (!pixels) {
                PostCaptureEvent({EngineEventType::Error, CaptureMode::ActiveDisplay,
                                  error.empty() ? L"The recorder has no fullscreen frame yet" : error});
                return;
            }
        }

        const std::wstring directory = settings.saveDirectory.empty()
            ? GetDefaultScreenshotDirectory() : settings.saveDirectory;
        EnsureDirectoryExists(directory);
        SaveJob job;
        job.path = MakeChronologicalMediaPath(directory, L"Screenshot",
                                              settings.imageFormat == ImageFormat::Jpeg ? L".jpg" : L".png");
        job.width = nativeSnapshot ? native.width : session.source.Width();
        job.height = nativeSnapshot ? native.height : session.source.Height();
        job.stride = job.width * 4;
        job.format = settings.imageFormat;
        job.outputWidth = settings.outputWidth;
        job.outputHeight = settings.outputHeight;
        job.jpegQuality = settings.jpegQuality;
        if (nativeSnapshot) {
            job.bgra = std::move(native.bgra);
        } else {
            job.bgra.resize(static_cast<std::size_t>(job.stride) * job.height);
            std::memcpy(job.bgra.data(), pixels, job.bgra.size());
        }

        PostCaptureEvent({EngineEventType::Feedback, CaptureMode::ActiveDisplay, L"Fullscreen frame captured",
                          job.path, {}, 0.0, 0.0, session.source.Monitor()});
        const auto started = GetTickCount64();
        const bool saved = EncodeScreenshotWic(job, error);
        EngineEvent event;
        event.type = saved ? EngineEventType::Saved : EngineEventType::Error;
        event.mode = CaptureMode::ActiveDisplay;
        event.path = job.path;
        event.detail = saved
            ? (session.source.IsRecovering() ? L"Saved recovery frame while fullscreen capture reconnects"
                                             : L"Saved from active recorder")
            : error;
        event.saveMs = static_cast<double>(GetTickCount64() - started);
        event.monitor = session.source.Monitor();
        PostCaptureEvent(std::move(event));
    };

    auto postFrameSnapshot = [&](const Command& command) {
        EngineEvent event;
        event.type = EngineEventType::RecorderFrame;
        event.mode = command.captureMode;
        event.requestId = command.requestId;
        if (session.mode == ActiveSession::Mode::Idle) {
            event.detail = L"The recorder is no longer active";
            PostCaptureEvent(std::move(event));
            return;
        }
        std::wstring error;
        session.source.AcquireLatest(0, error);
        if (!command.drawCursor) {
            // Prefer the native-resolution snapshot: the DIB path is downscaled to the
            // encoder size while the replay/recording scaler is active, and upscaling it
            // back for the region editor made both the preview and the crop blurry.
            auto native = std::make_shared<FrameBuffer>();
            if (session.source.ReadNativeSnapshot(*native)) {
                event.frame = std::move(native);
                PostCaptureEvent(std::move(event));
                return;
            }
        }
        const BYTE* pixels = session.source.ReadFrame(command.drawCursor, error);
        if (!pixels) {
            pixels = session.source.EnterRecovery(error.empty()
                ? L"The recorder has no live frame yet" : error);
        }
        if (!pixels) {
            event.detail = error.empty() ? L"The recorder has no live frame yet" : error;
            PostCaptureEvent(std::move(event));
            return;
        }
        auto frame = std::make_shared<FrameBuffer>();
        frame->width = session.source.Width();
        frame->height = session.source.Height();
        frame->stride = frame->width * 4;
        frame->virtualRect = session.source.DesktopRect();
        frame->bgra.resize(static_cast<std::size_t>(frame->stride) * frame->height);
        std::memcpy(frame->bgra.data(), pixels, frame->bgra.size());
        event.frame = std::move(frame);
        PostCaptureEvent(std::move(event));
    };

    auto postPreview = [&](const BYTE* source, UINT sourceWidth, UINT sourceHeight) {
        // The preview is already gated on the Luna window being foreground on the
        // Record page, so it never runs while a game is in front — suppressing it
        // by capture priority bought nothing and only blanked the preview the user
        // was actively looking at.
        if (!source ||
            !previewConsumerActive_.load(std::memory_order_acquire) ||
            !IsWindowVisible(notifyWindow_) || IsIconic(notifyWindow_)) return;
        if (previewInFlight_.exchange(true, std::memory_order_acq_rel)) return;
        constexpr UINT kPreviewWidth = 640;
        constexpr UINT kPreviewHeight = 360;
        auto preview = TakePreviewFrame();
        preview->liveRecording = true;
        preview->channelId = channelId_;
        const UINT sourceStride = sourceWidth * 4;
        if (!session.previewScaler.Scale(source, sourceWidth, sourceHeight, sourceStride,
                                         kPreviewWidth, kPreviewHeight, preview->bgra,
                                         preview->width, preview->height, preview->stride)) {
            RecyclePreviewFrame(std::move(preview));
            previewInFlight_.store(false, std::memory_order_release);
            return;
        }
        auto* raw = preview.release();
        if (!PostMessageW(notifyWindow_, WM_APP_PREVIEW_FRAME, 0, reinterpret_cast<LPARAM>(raw))) {
            delete raw;
            previewInFlight_.store(false, std::memory_order_release);
        }
    };

    while (!shutdown) {
        std::deque<Command> pending;
        {
            std::unique_lock lock(mutex_);
            if (commands_.empty() && session.mode == ActiveSession::Mode::Idle) {
                cv_.wait(lock, [&] { return !commands_.empty(); });
            }
            pending.swap(commands_);
        }
        for (auto& command : pending) {
            switch (command.type) {
                case CommandType::StartRecording:
                    if (session.mode == ActiveSession::Mode::Idle) {
                        startSession(ActiveSession::Mode::Recording, command.settings, command.sessionGeneration);
                    }
                    break;
                case CommandType::StartReplay:
                    if (session.mode == ActiveSession::Mode::Idle) {
                        startSession(ActiveSession::Mode::Replay, command.settings, command.sessionGeneration);
                    }
                    break;
                case CommandType::Stop: closeSession(true); break;
                case CommandType::SaveReplay: saveReplay(); break;
                case CommandType::DisplayChanged:
                    if (session.mode != ActiveSession::Mode::Idle) {
                        session.source.NotifyDisplayChange();
                        if (!session.sourceRecovering) {
                            session.sourceRecovering = true;
                            postSessionEvent({RecordingEventType::Recovering,
                                              L"Display mode changed; reconnecting fullscreen capture…"});
                        }
                    }
                    break;
                case CommandType::Screenshot: saveScreenshot(command.settings); break;
                case CommandType::FrameSnapshot: postFrameSnapshot(command); break;
                case CommandType::Shutdown: closeSession(false); shutdown = true; break;
            }
        }
        if (shutdown || session.mode == ActiveSession::Mode::Idle) continue;

        const ULONGLONG now = GetTickCount64();
        const std::uint64_t expectedFrames = ((now - session.segmentStarted) * session.settings.recordingFps) / 1000;
        if (session.writer.FrameCount() <= expectedFrames) {
            const std::uint64_t timelineBehind = expectedFrames > session.writer.FrameCount()
                ? expectedFrames - session.writer.FrameCount() : 0;
            if (timelineBehind > 2) {
                const std::uint64_t skipped = timelineBehind - 1;
                session.writer.SkipFrames(skipped);
                session.droppedFrames += skipped;
            }
            std::wstring error;
            const bool acquired = session.source.AcquireLatest(0, error);
            bool sourceRecovering = session.source.IsRecovering() || !acquired;
            if (sourceRecovering != session.sourceRecovering) {
                session.sourceRecovering = sourceRecovering;
                postSessionEvent({sourceRecovering ? RecordingEventType::Recovering : RecordingEventType::Recovered,
                                  sourceRecovering ? (error.empty()
                                      ? L"Fullscreen capture is unavailable; recording a recovery frame and retrying automatically…"
                                      : error + L" · recording continues while capture retries…")
                                                   : L"Fullscreen capture reconnected"});
            }
            const BYTE* frame = session.source.ReadFrame(session.settings.recordCursor && !sourceRecovering, error);
            if (!frame) {
                frame = session.source.EnterRecovery(error.empty()
                    ? L"Fullscreen frame readback is temporarily unavailable" : error);
                sourceRecovering = true;
            }
            const bool recoveryAfterRead = session.source.IsRecovering();
            if (recoveryAfterRead != session.sourceRecovering) {
                session.sourceRecovering = recoveryAfterRead;
                postSessionEvent({recoveryAfterRead ? RecordingEventType::Recovering : RecordingEventType::Recovered,
                                  recoveryAfterRead
                                      ? L"Fullscreen frame readback failed; recording continues while capture retries…"
                                      : L"Fullscreen capture reconnected"});
            }
            if (!frame) {
                // An allocation-level failure is exceptionally rare. Preserve the session and
                // timeline; the next pass will retry instead of finalizing the video/replay.
                session.writer.SkipFrames(1);
                ++session.droppedFrames;
                continue;
            }
            const int effectivePreviewFps = (std::min)(session.settings.recordingFps,
                                                       previewFps_.load(std::memory_order_acquire));
            const ULONGLONG previewInterval = 1000 / static_cast<ULONGLONG>((std::max)(1, effectivePreviewFps));
            if (now - session.lastPreview >= previewInterval &&
                (!session.source.LastFrameNearlyBlack() || recoveryAfterRead)) {
                postPreview(frame, session.source.Width(), session.source.Height());
                session.lastPreview = now;
            }
            if (!session.writer.WriteVideo(frame, error)) {
                const std::uint64_t generation = session.generation;
                closeSession(false);
                postGenerationEvent({RecordingEventType::Error, error}, generation, false);
                continue;
            }
            ++session.totalFrames;
        }
        std::wstring audioError;
        session.writer.DrainAudio(audioError);
        if (!audioError.empty()) postSessionEvent({RecordingEventType::AudioWarning, audioError});

        if (session.mode == ActiveSession::Mode::Replay) {
            const int segmentSeconds = ReplaySegmentSeconds(session.settings.replaySeconds);
            if (now - session.segmentStarted >= static_cast<ULONGLONG>(segmentSeconds) * 1000) {
                std::wstring rotateError;
                if (!rotateReplay(rotateError)) {
                    const std::uint64_t generation = session.generation;
                    closeSession(false);
                    postGenerationEvent({RecordingEventType::Error, rotateError}, generation, true);
                    continue;
                }
            }
        }
        if (now - session.lastStats >= 500) {
            const std::uint64_t elapsed = now - session.sessionStarted;
            const double fps = elapsed ? static_cast<double>(session.totalFrames) * 1000.0 / elapsed : 0.0;
            postSessionEvent({RecordingEventType::Stats,
                              session.mode == ActiveSession::Mode::Replay ? L"Replay buffer" : L"Recording",
                              session.currentPath, elapsed, session.totalFrames, session.droppedFrames, fps});
            session.lastStats = now;
        }

        std::unique_lock lock(mutex_);
        if (commands_.empty()) cv_.wait_for(lock, std::chrono::milliseconds(1));
    }

    MFShutdown();
    SetThreadExecutionState(ES_CONTINUOUS);
    if (mmcssHandle) AvRevertMmThreadCharacteristics(mmcssHandle);
    CoUninitialize();
}
