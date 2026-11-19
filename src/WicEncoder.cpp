#include "WicEncoder.hpp"

#include <Windows.h>
#include <objidl.h>
#include <propidl.h>
#include <oleauto.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

using Microsoft::WRL::ComPtr;

namespace {
std::wstring HResultMessage(HRESULT hr) {
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD count = FormatMessageW(flags, nullptr, static_cast<DWORD>(hr), 0,
                                       reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result = count && buffer ? std::wstring(buffer, count) : L"HRESULT 0x" + std::to_wstring(static_cast<unsigned long>(hr));
    if (buffer) LocalFree(buffer);
    return result;
}

bool Failed(HRESULT hr, const wchar_t* stage, std::wstring& error) {
    if (SUCCEEDED(hr)) return false;
    error = std::wstring(stage) + L": " + HResultMessage(hr);
    return true;
}
}  // namespace


bool EncodeScreenshotWic(const SaveJob& job, std::wstring& error) {
    if (job.bgra.empty() || job.width == 0 || job.height == 0 || job.stride == 0) {
        error = L"Empty frame";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(job.path).parent_path(), ec);
    if (ec) {
        const std::string narrow = ec.message();
        error = L"Could not create screenshot folder: " + std::wstring(narrow.begin(), narrow.end());
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        hr = CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    }
    if (Failed(hr, L"WIC factory", error)) return false;

    ComPtr<IWICBitmap> source;
    hr = factory->CreateBitmapFromMemory(job.width, job.height, GUID_WICPixelFormat32bppBGRA, job.stride,
                                         static_cast<UINT>(job.bgra.size()), const_cast<BYTE*>(job.bgra.data()), &source);
    if (Failed(hr, L"CreateBitmapFromMemory", error)) return false;

    ComPtr<IWICBitmapSource> finalSource;
    hr = source.As(&finalSource);
    if (Failed(hr, L"IWICBitmapSource", error)) return false;
    // Screenshots are always saved at the exact captured resolution. Resampling a
    // still image softens real pixels without adding detail, so the legacy output
    // size fields are deliberately ignored by the screenshot-only encoder.
    const UINT outputWidth = job.width;
    const UINT outputHeight = job.height;

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (Failed(hr, L"CreateStream", error)) return false;
    hr = stream->InitializeFromFilename(job.path.c_str(), GENERIC_WRITE);
    if (Failed(hr, L"InitializeFromFilename", error)) return false;

    const GUID container = job.format == ImageFormat::Jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng;
    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(container, nullptr, &encoder);
    if (Failed(hr, L"CreateEncoder", error)) return false;
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (Failed(hr, L"Encoder::Initialize", error)) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    hr = encoder->CreateNewFrame(&frame, &properties);
    if (Failed(hr, L"CreateNewFrame", error)) return false;

    if (job.format == ImageFormat::Jpeg && properties) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = static_cast<float>((std::max)(1, (std::min)(100, job.jpegQuality))) / 100.0f;
        properties->Write(1, &option, &value);
        VariantClear(&value);
    }

    hr = frame->Initialize(properties.Get());
    if (Failed(hr, L"Frame::Initialize", error)) return false;

    hr = frame->SetSize(outputWidth, outputHeight);
    if (Failed(hr, L"Frame::SetSize", error)) return false;

    if (job.format == ImageFormat::Jpeg) {
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat24bppBGR;
        hr = frame->SetPixelFormat(&pixelFormat);
        if (Failed(hr, L"Frame::SetPixelFormat JPEG", error)) return false;

        ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(&converter);
        if (Failed(hr, L"CreateFormatConverter", error)) return false;
        hr = converter->Initialize(finalSource.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (Failed(hr, L"FormatConverter::Initialize", error)) return false;
        hr = frame->WriteSource(converter.Get(), nullptr);
    } else {
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&pixelFormat);
        if (Failed(hr, L"Frame::SetPixelFormat PNG", error)) return false;
        hr = frame->WriteSource(finalSource.Get(), nullptr);
    }
    if (Failed(hr, L"Frame::WriteSource", error)) return false;

    hr = frame->Commit();
    if (Failed(hr, L"Frame::Commit", error)) return false;
    hr = encoder->Commit();
    if (Failed(hr, L"Encoder::Commit", error)) return false;
    return true;
}

ClipboardMemory EncodeClipboardPng(const SaveJob& job) {
    ClipboardMemory png;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, FALSE, &stream))) return {};
    HGLOBAL data = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &data))) return {};
    png.reset(data);

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IWICBitmap> source;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateBitmapFromMemory(job.width, job.height, GUID_WICPixelFormat32bppBGR, job.stride,
                                              static_cast<UINT>(job.bgra.size()), const_cast<BYTE*>(job.bgra.data()), &source)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(job.width, job.height)) ||
        FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WriteSource(source.Get(), nullptr)) ||
        FAILED(frame->Commit()) ||
        FAILED(encoder->Commit())) {
        return {};
    }
    return png;
}
