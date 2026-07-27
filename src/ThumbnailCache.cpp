#include "ThumbnailCache.hpp"

#include "CoreUtils.hpp"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <new>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT kThumbnailMaxSide = 240;
}

ThumbnailCache::~ThumbnailCache() { Stop(); }

bool ThumbnailCache::Start(HWND notifyWindow, UINT readyMessage) {
    notifyWindow_ = notifyWindow;
    readyMessage_ = readyMessage;
    stopping_.store(false);
    worker_ = std::thread(&ThumbnailCache::WorkerMain, this);
    return true;
}

void ThumbnailCache::Stop() {
    if (!worker_.joinable()) return;
    stopping_.store(true);
    cv_.notify_all();
    worker_.join();
    std::lock_guard lock(mutex_);
    queue_.clear();
}

void ThumbnailCache::Request(const std::filesystem::path& path, bool isVideo) {
    std::lock_guard lock(mutex_);
    for (const auto& pending : queue_) {
        if (pending.first == path) return;
    }
    if (queue_.size() >= 64) queue_.pop_front();
    queue_.emplace_back(path, isVideo);
    cv_.notify_one();
}

void ThumbnailCache::WorkerMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    for (;;) {
        std::pair<std::filesystem::path, bool> job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stopping_.load() || !queue_.empty(); });
            if (stopping_.load()) break;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        auto result = std::make_unique<ThumbnailResult>();
        result->path = job.first;
        result->isVideo = job.second;
        const bool ok = job.second ? DecodeVideoFrame(job.first, *result) : DecodePhoto(job.first, *result);
        if (!ok || result->bgra.empty()) continue;
        auto* raw = result.release();
        if (!PostMessageW(notifyWindow_, readyMessage_, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
    }

    MFShutdown();
    CoUninitialize();
}

bool ThumbnailCache::DecodePhoto(const std::filesystem::path& path, ThumbnailResult& result) const {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    UINT sourceWidth = 0, sourceHeight = 0;
    if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) || !sourceWidth || !sourceHeight) return false;
    if (sourceWidth > 32768 || sourceHeight > 32768) return false;

    const double scale = (std::min)(1.0, (std::min)(static_cast<double>(kThumbnailMaxSide) / sourceWidth,
                                                     static_cast<double>(kThumbnailMaxSide) / sourceHeight));
    const UINT width = (std::max)(1u, static_cast<UINT>(sourceWidth * scale));
    const UINT height = (std::max)(1u, static_cast<UINT>(sourceHeight * scale));

    ComPtr<IWICBitmapSource> source = frame;
    ComPtr<IWICBitmapScaler> scaler;
    if (width != sourceWidth || height != sourceHeight) {
        if (FAILED(factory->CreateBitmapScaler(&scaler))) return false;
        if (FAILED(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant))) return false;
        source = scaler;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return false;
    if (FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom))) return false;

    const UINT stride = width * 4;
    result.bgra.resize(static_cast<std::size_t>(stride) * height);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(result.bgra.size()), result.bgra.data()))) return false;
    result.width = width;
    result.height = height;
    return true;
}

bool ThumbnailCache::DecodeVideoFrame(const std::filesystem::path& path, ThumbnailResult& result) const {
    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 1))) return false;
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader))) return false;
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

    ComPtr<IMFMediaType> requested;
    if (FAILED(MFCreateMediaType(&requested))) return false;
    requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    requested->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr,
                                           requested.Get()))) return false;

    // Seek a little way in so the thumbnail isn't a black/blank intro frame.
    PROPVARIANT duration{};
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION,
                                                   &duration)) &&
        duration.vt == VT_UI8) {
        const LONGLONG target = (std::min<LONGLONG>)(static_cast<LONGLONG>(duration.uhVal.QuadPart) / 4, 30LL * 10000000LL);
        PROPVARIANT seek{};
        InitPropVariantFromInt64(target, &seek);
        reader->SetCurrentPosition(GUID_NULL, seek);
        PropVariantClear(&seek);
    }
    PropVariantClear(&duration);

    ComPtr<IMFSample> sample;
    for (int attempt = 0; attempt < 3 && !sample; ++attempt) {
        DWORD flags = 0;
        ComPtr<IMFSample> candidate;
        if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags,
                                      nullptr, &candidate))) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (candidate) sample = candidate;
    }
    if (!sample) return false;

    ComPtr<IMFMediaType> actualType;
    if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &actualType))) return false;
    UINT32 sourceWidth = 0, sourceHeight = 0;
    if (FAILED(MFGetAttributeSize(actualType.Get(), MF_MT_FRAME_SIZE, &sourceWidth, &sourceHeight)) ||
        !sourceWidth || !sourceHeight) return false;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;

    // Prefer the buffer's own 2D lock: it reports the REAL pitch (sign and
    // all), rather than the guess MFGetStrideForBitmapInfoHeader would make
    // from just the pixel format — that guess assumes the classic bottom-up
    // BMP convention, which doesn't match what the video processor MFT
    // actually hands back here and was flipping every video thumbnail
    // upside down. With the real pitch, scanline0 + y*pitch already walks
    // rows in top-down logical order regardless of sign, so no manual row
    // reversal is needed.
    ComPtr<IMF2DBuffer> buffer2D;
    BYTE* scanline0 = nullptr;
    LONG pitch = 0;
    const bool locked2D = SUCCEEDED(buffer.As(&buffer2D)) && SUCCEEDED(buffer2D->Lock2D(&scanline0, &pitch));
    DWORD flatLength = 0;
    if (!locked2D) {
        if (FAILED(buffer->Lock(&scanline0, nullptr, &flatLength))) return false;
        pitch = static_cast<LONG>(sourceWidth) * 4;
    }

    const UINT absStride = static_cast<UINT>(pitch < 0 ? -pitch : pitch);
    std::vector<std::uint8_t> topDown(static_cast<std::size_t>(absStride) * sourceHeight);
    for (UINT y = 0; y < sourceHeight; ++y) {
        std::memcpy(topDown.data() + static_cast<std::size_t>(y) * absStride,
                    scanline0 + static_cast<std::ptrdiff_t>(y) * pitch, absStride);
    }
    if (locked2D) buffer2D->Unlock2D();
    else buffer->Unlock();

    const double scale = (std::min)(1.0, (std::min)(static_cast<double>(kThumbnailMaxSide) / sourceWidth,
                                                     static_cast<double>(kThumbnailMaxSide) / sourceHeight));
    BgraPreviewScaler scaler;
    UINT destWidth = 0, destHeight = 0, destStride = 0;
    if (scale >= 0.999) {
        result.bgra = std::move(topDown);
        result.width = sourceWidth;
        result.height = sourceHeight;
        return true;
    }
    const UINT targetWidth = (std::max)(1u, static_cast<UINT>(sourceWidth * scale));
    const UINT targetHeight = (std::max)(1u, static_cast<UINT>(sourceHeight * scale));
    if (!scaler.Scale(topDown.data(), sourceWidth, sourceHeight, absStride, targetWidth, targetHeight, result.bgra,
                      destWidth, destHeight, destStride)) return false;
    result.width = destWidth;
    result.height = destHeight;
    return true;
}
