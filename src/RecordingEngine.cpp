#include "RecordingEngine.hpp"

#include "AudioMixer.hpp"
#include "CoreUtils.hpp"
#include "WicEncoder.hpp"
#include "WinUtil.hpp"

#include <Windows.h>
#include <avrt.h>
#include <audioclient.h>
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

std::wstring TimestampedPath(const std::wstring& directory, const wchar_t* prefix) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t filename[160]{};
    swprintf_s(filename, L"%s_%04u-%02u-%02u_%02u-%02u-%02u-%03u.mp4", prefix,
               time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
               time.wSecond, time.wMilliseconds);
    return (std::filesystem::path(directory) / filename).wstring();
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

    bool Initialize(int requestedIndex, std::wstring& error) {
        Reset();
        requestedIndex_ = (std::max)(0, requestedIndex);
        const ULONGLONG deadline = GetTickCount64() + 5000;
        std::wstring lastError;
        while (GetTickCount64() < deadline) {
            if (!duplication_ && !Connect(lastError)) {
                Sleep(75);
                continue;
            }
            std::wstring frameError;
            if (AcquireLatest(250, frameError) && ReadFrame(false, frameError)) return true;
            if (!frameError.empty()) lastError = frameError;
        }
        error = lastError.empty() ? L"Fullscreen display did not deliver a frame" : lastError;
        return false;
    }

    bool AcquireLatest(DWORD timeoutMs, std::wstring& error) {
        error.clear();
        if (!duplication_) {
            const ULONGLONG now = GetTickCount64();
            if (now < nextReconnectTick_) return hasFrame_;
            std::wstring reconnectError;
            if (!Connect(reconnectError)) {
                nextReconnectTick_ = now + 250;
                if (!hasFrame_) error = reconnectError;
                return hasFrame_;
            }
            recovering_ = false;
        }

        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        const HRESULT hr = duplication_->AcquireNextFrame(timeoutMs, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return hasFrame_;
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED ||
            hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_SESSION_DISCONNECTED) {
            BeginReconnect();
            return hasFrame_;
        }
        if (FAILED(hr)) {
            if (hasFrame_) {
                AppendDiagnosticLog(L"AcquireNextFrame failed while a cached frame was available: " + HResultText(hr));
                BeginReconnect();
                return true;
            }
            error = L"Recording AcquireNextFrame: " + HResultText(hr);
            AppendDiagnosticLog(error);
            return false;
        }
        struct ReleaseGuard {
            IDXGIOutputDuplication* duplication = nullptr;
            ~ReleaseGuard() { if (duplication) duplication->ReleaseFrame(); }
        } guard{duplication_.Get()};

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
            if (hasFrame_) { BeginReconnect(); return true; }
            error = L"Desktop recording frame is not a texture";
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        frame->GetDesc(&desc);
        if (!EnsureSurfaces(desc, error)) {
            if (hasFrame_) { error.clear(); BeginReconnect(); return true; }
            return false;
        }
        context_->CopyResource(staging_.Get(), frame.Get());
        nativeFrameReady_ = true;
        return true;
    }

    const BYTE* ReadFrame(bool drawCursor, std::wstring& error) {
        if (!dibBits_) { error = L"No desktop frame surface is ready"; return nullptr; }
        if (!nativeFrameReady_) {
            if (!hasFrame_) return nullptr;
            RestoreCursorBackground();
            if (drawCursor) DrawCursor();
            return static_cast<const BYTE*>(dibBits_);
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            BeginReconnect();
            if (hasFrame_) {
                RestoreCursorBackground();
                if (drawCursor) DrawCursor();
                return static_cast<const BYTE*>(dibBits_);
            }
            error = L"Map recording frame: " + HResultText(hr);
            return nullptr;
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
        context_->Unmap(staging_.Get(), 0);
        if (nativeWidth_ != width_ || nativeHeight_ != height_) ScaleNativeFrame();
        cursorBackupValid_ = false;
        hasFrame_ = true;
        nativeFrameReady_ = false;
        if (IsNearlyBlack()) {
            ++blackFrameCount_;
            if (blackFrameCount_ >= 120 && blackFallbacks_ < 2) {
                ++blackFallbacks_;
                blackFrameCount_ = 0;
                AppendDiagnosticLog(L"Sustained black fullscreen frames detected; forcing backend fallback");
                BeginReconnect();
            }
        } else {
            blackFrameCount_ = 0;
        }
        if (drawCursor) DrawCursor();
        return static_cast<const BYTE*>(dibBits_);
    }

    UINT Width() const noexcept { return width_; }
    UINT Height() const noexcept { return height_; }
    bool IsRecovering() const noexcept { return recovering_; }
    void Release() { Reset(); }
    void NotifyDisplayChange() { if (hasFrame_) BeginReconnect(); }
    HMONITOR Monitor() const noexcept { return target_.monitor; }
    RECT DesktopRect() const noexcept { return target_.desktopRect; }
    std::wstring Description() const {
        return target_.deviceName + L" · " + (usingModernDuplication_ ? L"DXGI 1.5" : L"DXGI legacy") +
               L" · " + std::to_wstring(width_) + L"x" + std::to_wstring(height_);
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
            std::memcpy(destination, source, static_cast<std::size_t>(width) * 4);
            for (UINT x = 0; x < width; ++x) destination[static_cast<std::size_t>(x) * 4 + 3] = 255;
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

    bool Connect(std::wstring& error) {
        ResetCaptureObjects();
        auto targets = EnumerateDisplayTargets();
        if (targets.empty()) { error = L"No desktop display is available for recording"; return false; }
        const std::size_t index = static_cast<std::size_t>(requestedIndex_) % targets.size();
        target_ = std::move(targets[index]);

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
        recovering_ = false;
        nextReconnectTick_ = 0;
        return true;
    }

    void BeginReconnect() {
        preferLegacyDuplication_ = !preferLegacyDuplication_;
        AppendDiagnosticLog(L"Fullscreen surface changed; switching duplication backend");
        ResetCaptureObjects();
        recovering_ = true;
        nextReconnectTick_ = 0;
    }

    bool EnsureSurfaces(const D3D11_TEXTURE2D_DESC& source, std::wstring& error) {
        if (staging_ && nativeWidth_ == source.Width && nativeHeight_ == source.Height && format_ == source.Format) {
            return true;
        }
        staging_.Reset();

        D3D11_TEXTURE2D_DESC staging = source;
        staging.MipLevels = 1;
        staging.ArraySize = 1;
        staging.SampleDesc.Count = 1;
        staging.SampleDesc.Quality = 0;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        HRESULT hr = device_->CreateTexture2D(&staging, nullptr, &staging_);
        if (FAILED(hr)) { error = L"Create recording staging texture: " + HResultText(hr); return false; }

        nativeWidth_ = source.Width;
        nativeHeight_ = source.Height;
        format_ = source.Format;
        nativeFrameReady_ = false;
        if (width_ != 0 && height_ != 0 && dibBits_) return true;

        width_ = source.Width;
        height_ = source.Height;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = static_cast<LONG>(source.Width);
        info.bmiHeader.biHeight = -static_cast<LONG>(source.Height);
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
        ICONINFO icon{};
        if (!GetIconInfo(cursor.hCursor, &icon)) return false;
        const int desktopWidth = (std::max)(1L, target_.desktopRect.right - target_.desktopRect.left);
        const int desktopHeight = (std::max)(1L, target_.desktopRect.bottom - target_.desktopRect.top);
        const double scaleX = static_cast<double>(contentRect_.right - contentRect_.left) / desktopWidth;
        const double scaleY = static_cast<double>(contentRect_.bottom - contentRect_.top) / desktopHeight;
        BITMAP bitmap{};
        int nativeCursorWidth = GetSystemMetrics(SM_CXCURSOR);
        int nativeCursorHeight = GetSystemMetrics(SM_CYCURSOR);
        if (icon.hbmColor && GetObjectW(icon.hbmColor, sizeof(bitmap), &bitmap) == sizeof(bitmap)) {
            nativeCursorWidth = bitmap.bmWidth;
            nativeCursorHeight = bitmap.bmHeight;
        } else if (icon.hbmMask && GetObjectW(icon.hbmMask, sizeof(bitmap), &bitmap) == sizeof(bitmap)) {
            nativeCursorWidth = bitmap.bmWidth;
            nativeCursorHeight = bitmap.bmHeight / 2;
        }
        const int cursorWidth = (std::max)(1, static_cast<int>(nativeCursorWidth * scaleX));
        const int cursorHeight = (std::max)(1, static_cast<int>(nativeCursorHeight * scaleY));
        const int x = contentRect_.left + static_cast<int>((cursor.ptScreenPos.x - target_.desktopRect.left) * scaleX) -
                      static_cast<int>(icon.xHotspot * scaleX);
        const int y = contentRect_.top + static_cast<int>((cursor.ptScreenPos.y - target_.desktopRect.top) * scaleY) -
                      static_cast<int>(icon.yHotspot * scaleY);
        BackupCursorBackground(x, y, x + cursorWidth, y + cursorHeight);
        const bool drawn = DrawIconEx(dibDc_, x, y, cursor.hCursor, cursorWidth, cursorHeight,
                                      0, nullptr, DI_NORMAL) != FALSE;
        GdiFlush();
        if (icon.hbmColor) DeleteObject(icon.hbmColor);
        if (icon.hbmMask) DeleteObject(icon.hbmMask);
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
        staging_.Reset();
        duplication_.Reset();
        context_.Reset();
        device_.Reset();
        target_ = {};
        nativeWidth_ = nativeHeight_ = 0;
        format_ = DXGI_FORMAT_UNKNOWN;
        nativeFrameReady_ = false;
        pointerShape_.clear();
        pointerShapeInfo_ = {};
        pointerVisible_ = false;
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
        hasFrame_ = false;
        blackFrameCount_ = 0;
        blackFallbacks_ = 0;
        recovering_ = false;
        nextReconnectTick_ = 0;
        preferLegacyDuplication_ = false;
        usingModernDuplication_ = false;
        cursorRenderLogged_ = false;
    }

    DisplayTarget target_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> staging_;
    HDC dibDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ dibOld_ = nullptr;
    void* dibBits_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT nativeWidth_ = 0;
    UINT nativeHeight_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    std::vector<BYTE> sourceFrame_;
    std::vector<BYTE> cursorBackup_;
    std::vector<BYTE> pointerShape_;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO pointerShapeInfo_{};
    POINT pointerPosition_{};
    RECT contentRect_{};
    int requestedIndex_ = 0;
    ULONGLONG nextReconnectTick_ = 0;
    bool hasFrame_ = false;
    bool nativeFrameReady_ = false;
    bool recovering_ = false;
    UINT blackFrameCount_ = 0;
    UINT blackFallbacks_ = 0;
    bool preferLegacyDuplication_ = false;
    bool usingModernDuplication_ = false;
    bool pointerVisible_ = false;
    bool cursorRenderLogged_ = false;
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
              VideoCodec codec, AudioMixer* audio, std::wstring& error) {
        Close(nullptr);
        sourceWidth_ = sourceWidth;
        sourceHeight_ = sourceHeight;
        fps_ = fps;
        frameDuration_ = kHundredNsPerSecond / fps_;
        audio_ = audio;
        audioWarning_.clear();

        ComPtr<IMFAttributes> attributes;
        HRESULT hr = MFCreateAttributes(&attributes, 5);
        if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        // Preserve sink-writer backpressure so raw BGRA frames cannot build an
        // unbounded queue when the encoder or disk is briefly slower than capture.
        if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, FALSE);
        if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attributes.Get(), &writer_);
        if (FAILED(hr)) { error = L"Create MP4 writer: " + HResultText(hr); return false; }

        ComPtr<IMFMediaType> output;
        const GUID& videoSubtype = codec == VideoCodec::Hevc ? MFVideoFormat_HEVC :
                                   codec == VideoCodec::Av1 ? MFVideoFormat_AV1 : MFVideoFormat_H264;
        const wchar_t* codecName = codec == VideoCodec::Hevc ? L"HEVC / H.265" :
                                   codec == VideoCodec::Av1 ? L"AV1" : L"H.264";
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

bool MergeMp4Segments(const std::vector<std::wstring>& paths, const std::wstring& outputPath, std::wstring& error) {
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
    attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    ComPtr<IMFSinkWriter> writer;
    hr = MFCreateSinkWriterFromURL(outputPath.c_str(), nullptr, attributes.Get(), &writer);
    DWORD videoOutput = 0;
    DWORD audioOutput = 0;
    if (SUCCEEDED(hr)) hr = writer->AddStream(videoType.Get(), &videoOutput);
    if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(videoOutput, videoType.Get(), nullptr);
    if (SUCCEEDED(hr) && hasAudio) hr = writer->AddStream(audioType.Get(), &audioOutput);
    if (SUCCEEDED(hr) && hasAudio) hr = writer->SetInputMediaType(audioOutput, audioType.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = writer->BeginWriting();
    if (FAILED(hr)) { error = L"Create replay MP4: " + HResultText(hr); return false; }

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
        error = L"Assemble replay MP4: " + HResultText(hr);
        DeleteFileW(outputPath.c_str());
        return false;
    }
    return true;
}

struct ActiveSession {
    enum class Mode { Idle, Recording, Replay } mode = Mode::Idle;
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

bool RecordingEngine::StartRecording(const AppSettings& settings) {
    if (busy_.exchange(true, std::memory_order_acq_rel)) return false;
    previewFps_.store((std::max)(1, (std::min)(60, settings.previewFps)), std::memory_order_release);
    if (!Queue(Command{CommandType::StartRecording, settings})) { busy_.store(false); return false; }
    return true;
}

bool RecordingEngine::StartReplayBuffer(const AppSettings& settings) {
    if (busy_.exchange(true, std::memory_order_acq_rel)) return false;
    previewFps_.store((std::max)(1, (std::min)(60, settings.previewFps)), std::memory_order_release);
    if (!Queue(Command{CommandType::StartReplay, settings})) { busy_.store(false); return false; }
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
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    DWORD mmcssTaskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Capture", &mmcssTaskIndex);
    if (mmcssHandle) AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_HIGH);
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

    auto resetSessionMetadata = [&]() {
        session.source.Release();
        session.mode = ActiveSession::Mode::Idle;
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
                                                session.settings.videoCodec, session.audio.get(), error);
        const std::wstring audioWarning = session.writer.TakeAudioWarning();
        if (!audioWarning.empty()) {
            AppendDiagnosticLog(audioWarning);
            PostEvent({RecordingEventType::AudioWarning, audioWarning});
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
            PostEvent({RecordingEventType::AudioWarning, audioWarning});
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
                (segmentPrefix + std::to_wstring(++session.segmentSerial) + L".mp4")).wstring();
    };

    auto startSession = [&](ActiveSession::Mode mode, const AppSettings& settings) {
        resetSessionMetadata();
        session.mode = mode;
        session.settings = settings;
        session.settings.recordingFps = (std::max)(1, (std::min)(240, session.settings.recordingFps));
        session.settings.recordingBitrateMbps = (std::max)(1, (std::min)(200, session.settings.recordingBitrateMbps));
        session.settings.replaySeconds = (std::max)(5, (std::min)(3600, session.settings.replaySeconds));
        session.settings.previewFps = (std::max)(1, (std::min)(60, session.settings.previewFps));
        EnsureDirectoryExists(session.settings.recordingDirectory.empty() ? GetDefaultRecordingDirectory() : session.settings.recordingDirectory);
        PostEvent({RecordingEventType::Starting, mode == ActiveSession::Mode::Replay ? L"Starting replay buffer…" : L"Starting recorder…"});

        std::wstring error;
        if (!session.source.Initialize(session.settings.recordingMonitorIndex, error)) {
            resetSessionMetadata();
            busy_.store(false, std::memory_order_release);
            PostEvent({RecordingEventType::Error, error});
            return;
        }
        AppendDiagnosticLog(L"Recorder source ready: " + session.source.Description());
        prepareAudio();
        const std::wstring directory = session.settings.recordingDirectory.empty() ? GetDefaultRecordingDirectory() : session.settings.recordingDirectory;
        session.currentPath = mode == ActiveSession::Mode::Replay ? nextReplayPath() : TimestampedPath(directory, L"Recording");
        if (!openWriter(session.currentPath, error)) {
            session.writer.Close(nullptr);
            RemoveReplaySegments(session);
            resetSessionMetadata();
            busy_.store(false, std::memory_order_release);
            PostEvent({RecordingEventType::Error, error});
            return;
        }
        session.sessionStarted = session.segmentStarted = session.lastStats = GetTickCount64();
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        if (mode == ActiveSession::Mode::Recording) {
            recording_.store(true, std::memory_order_release);
            PostEvent({RecordingEventType::RecordingStarted, session.source.Description(), session.currentPath});
        } else {
            replay_.store(true, std::memory_order_release);
            PostEvent({RecordingEventType::ReplayStarted, session.source.Description()});
        }
    };

    auto closeSession = [&](bool postStatus) {
        if (session.mode == ActiveSession::Mode::Idle) return;
        const auto previousMode = session.mode;
        const std::wstring finishedPath = session.currentPath;
        const std::uint64_t elapsed = GetTickCount64() - session.sessionStarted;
        const std::uint64_t frames = session.totalFrames;
        const std::uint64_t dropped = session.droppedFrames;
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
        if (!error.empty()) {
            PostEvent({RecordingEventType::Error, error});
        } else if (previousMode == ActiveSession::Mode::Recording) {
            PostEvent({RecordingEventType::RecordingSaved, L"Recording saved", finishedPath,
                       elapsed, frames, dropped});
        } else {
            PostEvent({RecordingEventType::ReplayStopped, L"Replay buffer stopped"});
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
            closeSession(false);
            PostEvent({RecordingEventType::Error, error});
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
        const std::wstring output = TimestampedPath(directory, prefix.c_str());
        bool saved = MergeMp4Segments(selected, output, error);
        if (!saved && !selected.empty()) {
            saved = CopyFileW(selected.back().c_str(), output.c_str(), FALSE) != FALSE;
            if (saved) error.clear();
        }
        if (saved) PostEvent({RecordingEventType::ReplaySaved, L"Replay saved", output, total});
        else PostEvent({RecordingEventType::Error, error.empty() ? L"Replay could not be saved" : error});
    };

    auto saveScreenshot = [&](const AppSettings& settings) {
        if (session.mode == ActiveSession::Mode::Idle) return;
        std::wstring error;
        session.source.AcquireLatest(0, error);
        const BYTE* pixels = session.source.ReadFrame(false, error);
        if (!pixels) {
            PostCaptureEvent({EngineEventType::Error, CaptureMode::ActiveDisplay,
                              error.empty() ? L"The recorder has no fullscreen frame yet" : error});
            return;
        }

        SYSTEMTIME time{};
        GetLocalTime(&time);
        const std::wstring directory = settings.saveDirectory.empty()
            ? GetDefaultScreenshotDirectory() : settings.saveDirectory;
        EnsureDirectoryExists(directory);
        const std::wstring filename = MakeScreenshotFilename(L"Screenshot", time.wYear, time.wMonth, time.wDay,
                                                              time.wHour, time.wMinute, time.wSecond,
                                                              time.wMilliseconds,
                                                              settings.imageFormat == ImageFormat::Jpeg);
        SaveJob job;
        job.path = (std::filesystem::path(directory) / filename).wstring();
        job.width = session.source.Width();
        job.height = session.source.Height();
        job.stride = job.width * 4;
        job.format = settings.imageFormat;
        job.outputWidth = settings.outputWidth;
        job.outputHeight = settings.outputHeight;
        job.jpegQuality = settings.jpegQuality;
        job.bgra.resize(static_cast<std::size_t>(job.stride) * job.height);
        std::memcpy(job.bgra.data(), pixels, job.bgra.size());

        PostCaptureEvent({EngineEventType::Feedback, CaptureMode::ActiveDisplay, L"Fullscreen frame captured",
                          job.path, {}, 0.0, 0.0, session.source.Monitor()});
        const auto started = GetTickCount64();
        const bool saved = EncodeScreenshotWic(job, error);
        EngineEvent event;
        event.type = saved ? EngineEventType::Saved : EngineEventType::Error;
        event.mode = CaptureMode::ActiveDisplay;
        event.path = job.path;
        event.detail = saved ? L"Saved from active recorder" : error;
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
        const BYTE* pixels = session.source.ReadFrame(command.drawCursor, error);
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
                    if (session.mode == ActiveSession::Mode::Idle) startSession(ActiveSession::Mode::Recording, command.settings);
                    break;
                case CommandType::StartReplay:
                    if (session.mode == ActiveSession::Mode::Idle) startSession(ActiveSession::Mode::Replay, command.settings);
                    break;
                case CommandType::Stop: closeSession(true); break;
                case CommandType::SaveReplay: saveReplay(); break;
                case CommandType::DisplayChanged:
                    if (session.mode != ActiveSession::Mode::Idle) {
                        session.source.NotifyDisplayChange();
                        if (!session.sourceRecovering) {
                            session.sourceRecovering = true;
                            PostEvent({RecordingEventType::Recovering,
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
            if (!session.source.AcquireLatest(0, error) && !error.empty()) {
                closeSession(false);
                PostEvent({RecordingEventType::Error, error});
                continue;
            }
            const bool sourceRecovering = session.source.IsRecovering();
            if (sourceRecovering != session.sourceRecovering) {
                session.sourceRecovering = sourceRecovering;
                PostEvent({sourceRecovering ? RecordingEventType::Recovering : RecordingEventType::Recovered,
                           sourceRecovering ? L"Display mode changed; reconnecting fullscreen capture…"
                                            : L"Fullscreen capture reconnected"});
            }
            const BYTE* frame = session.source.ReadFrame(session.settings.recordCursor, error);
            if (!frame) {
                closeSession(false);
                PostEvent({RecordingEventType::Error, error});
                continue;
            }
            const int effectivePreviewFps = (std::min)(session.settings.recordingFps,
                                                       previewFps_.load(std::memory_order_acquire));
            const ULONGLONG previewInterval = 1000 / static_cast<ULONGLONG>((std::max)(1, effectivePreviewFps));
            if (now - session.lastPreview >= previewInterval) {
                postPreview(frame, session.source.Width(), session.source.Height());
                session.lastPreview = now;
            }
            if (!session.writer.WriteVideo(frame, error)) {
                closeSession(false);
                PostEvent({RecordingEventType::Error, error});
                continue;
            }
            ++session.totalFrames;
        }
        std::wstring audioError;
        session.writer.DrainAudio(audioError);
        if (!audioError.empty()) PostEvent({RecordingEventType::AudioWarning, audioError});

        if (session.mode == ActiveSession::Mode::Replay) {
            const int segmentSeconds = ReplaySegmentSeconds(session.settings.replaySeconds);
            if (now - session.segmentStarted >= static_cast<ULONGLONG>(segmentSeconds) * 1000) {
                std::wstring rotateError;
                if (!rotateReplay(rotateError)) {
                    closeSession(false);
                    PostEvent({RecordingEventType::Error, rotateError});
                    continue;
                }
            }
        }
        if (now - session.lastStats >= 500) {
            const std::uint64_t elapsed = now - session.sessionStarted;
            const double fps = elapsed ? static_cast<double>(session.totalFrames) * 1000.0 / elapsed : 0.0;
            PostEvent({RecordingEventType::Stats, session.mode == ActiveSession::Mode::Replay ? L"Replay buffer" : L"Recording",
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
