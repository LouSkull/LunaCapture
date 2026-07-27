#include <Windows.h>
#include <mfapi.h>
#include <mfplay.h>

#include <atomic>
#include <iostream>

class PlayerCallback final : public IMFPMediaPlayerCallback {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFPMediaPlayerCallback)) {
            *object = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* event) override {
        if (!event) return;
        lastResult.store(event->hrEvent);
        if (FAILED(event->hrEvent) || event->eEventType == MFP_EVENT_TYPE_ERROR) failed.store(true);
        if (event->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET || event->eEventType == MFP_EVENT_TYPE_PLAY) ready.store(true);
    }

    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    std::atomic<HRESULT> lastResult{S_OK};

private:
    std::atomic<ULONG> references_{1};
};

int wmain(int argumentCount, wchar_t** arguments) {
    if (argumentCount < 2) {
        std::wcerr << L"usage: LunaShotMediaPlayerSmoke <video-path>\n";
        return 2;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HRESULT mf = MFStartup(MF_VERSION);
    if (FAILED(mf)) return 3;

    HWND videoHost = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 640, 360,
                                     nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    auto* callback = new PlayerCallback();
    IMFPMediaPlayer* player = nullptr;
    const HRESULT created = MFPCreateMediaPlayer(arguments[1], FALSE, MFP_OPTION_NONE, callback, videoHost, &player);
    const ULONGLONG deadline = GetTickCount64() + 7000;
    while (SUCCEEDED(created) && !callback->ready.load() && !callback->failed.load() && GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(10);
    }
    LONGLONG duration = 0;
    HRESULT durationResult = E_FAIL;
    VARTYPE durationType = VT_EMPTY;
    const ULONGLONG metadataDeadline = GetTickCount64() + 3000;
    while (player && callback->ready.load() && duration == 0 && GetTickCount64() < metadataDeadline) {
        PROPVARIANT value{};
        durationResult = player->GetDuration(MFP_POSITIONTYPE_100NS, &value);
        durationType = value.vt;
        if (SUCCEEDED(durationResult)) {
            if (value.vt == VT_I8) duration = value.hVal.QuadPart;
            else if (value.vt == VT_UI8) duration = static_cast<LONGLONG>(value.uhVal.QuadPart);
        }
        PropVariantClear(&value);
        if (duration == 0) Sleep(20);
    }
    const bool success = SUCCEEDED(created) && callback->ready.load() && !callback->failed.load() && duration > 0;
    std::wcout << L"created=0x" << std::hex << created << L" event=0x" << callback->lastResult.load()
               << L" durationHr=0x" << durationResult << std::dec << L" vt=" << durationType
               << L" ready=" << callback->ready.load() << L" duration100ns=" << duration << L"\n";
    if (player) { player->Shutdown(); player->Release(); }
    callback->Release();
    if (videoHost) DestroyWindow(videoHost);
    MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    return success ? 0 : 1;
}
