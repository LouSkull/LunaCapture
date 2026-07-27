#include "AudioMixer.hpp"

#include "CoreUtils.hpp"

#include <Windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <functiondiscoverykeys_devpkey.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT32 kMixRate = 48'000;
constexpr UINT32 kMixChannels = 2;
constexpr UINT32 kMixBits = 16;
constexpr UINT32 kChunkFrames = 480;

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

WAVEFORMATEX MixFormat() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kMixChannels;
    format.nSamplesPerSec = kMixRate;
    format.wBitsPerSample = kMixBits;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

class ProcessActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ProcessActivationHandler() { completed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~ProcessActivationHandler() { if (completed_) CloseHandle(completed_); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (iid == __uuidof(IAgileObject)) {
            *object = static_cast<IAgileObject*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activationResult = E_UNEXPECTED;
        ComPtr<IUnknown> unknown;
        result_ = operation ? operation->GetActivateResult(&activationResult, &unknown) : E_POINTER;
        if (SUCCEEDED(result_)) result_ = activationResult;
        if (SUCCEEDED(result_) && unknown) result_ = unknown.As(&client_);
        SetEvent(completed_);
        return S_OK;
    }

    HRESULT Wait(ComPtr<IAudioClient>& client) {
        if (!completed_) return E_OUTOFMEMORY;
        if (WaitForSingleObject(completed_, 5000) != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        if (SUCCEEDED(result_)) client = client_;
        return result_;
    }

private:
    std::atomic<ULONG> references_{1};
    HANDLE completed_ = nullptr;
    HRESULT result_ = E_PENDING;
    ComPtr<IAudioClient> client_;
};

class AudioSource {
public:
    bool InitializeSystem(float gain, std::wstring& error) {
        gain_ = gain;
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator));
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                 reinterpret_cast<void**>(client_.GetAddressOf()));
        if (FAILED(hr)) { error = L"System audio: " + HResultText(hr); return false; }
        return FinishInitialize(true, error);
    }

    bool InitializeMicrophone(const std::wstring& id, float gain, std::wstring& error) {
        gain_ = gain;
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator));
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(hr)) hr = enumerator->GetDevice(id.c_str(), &device);
        if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                 reinterpret_cast<void**>(client_.GetAddressOf()));
        if (FAILED(hr)) { error = L"Microphone: " + HResultText(hr); return false; }
        return FinishInitialize(false, error);
    }

    bool InitializeProcess(DWORD processId, float gain, std::wstring& error) {
        if (!processId) { error = L"Application audio target is not running"; return false; }
        gain_ = gain;
        AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
        parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        parameters.ProcessLoopbackParams.TargetProcessId = processId;
        parameters.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT value{};
        value.vt = VT_BLOB;
        value.blob.cbSize = sizeof(parameters);
        value.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);

        ComPtr<ProcessActivationHandler> handler;
        handler.Attach(new (std::nothrow) ProcessActivationHandler());
        if (!handler) { error = L"Application audio activation allocation failed"; return false; }
        ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
        HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                                  &value, handler.Get(), &operation);
        if (SUCCEEDED(hr)) hr = handler->Wait(client_);
        if (FAILED(hr)) { error = L"Application audio requires Windows 11 / build 20348+: " + HResultText(hr); return false; }
        return FinishInitialize(true, error);
    }

    bool Start(std::wstring& error) {
        if (!client_) return false;
        const HRESULT hr = client_->Start();
        if (FAILED(hr)) { error = HResultText(hr); return false; }
        active_ = true;
        return true;
    }

    void Stop() {
        if (client_ && active_) client_->Stop();
        active_ = false;
        samples_.clear();
        sampleOffset_ = 0;
    }

    bool Pull(std::wstring& error) {
        if (!active_ || !capture_) return true;
        for (;;) {
            UINT32 frames = 0;
            HRESULT hr = capture_->GetNextPacketSize(&frames);
            if (FAILED(hr)) { active_ = false; error = HResultText(hr); return false; }
            if (!frames) break;
            BYTE* data = nullptr;
            DWORD flags = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) { active_ = false; error = HResultText(hr); return false; }
            const std::size_t sampleCount = static_cast<std::size_t>(frames) * kMixChannels;
            const std::size_t oldSize = samples_.size();
            samples_.resize(oldSize + sampleCount);
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data) {
                std::fill(samples_.begin() + static_cast<std::ptrdiff_t>(oldSize), samples_.end(),
                          static_cast<std::int16_t>(0));
            } else {
                std::memcpy(samples_.data() + oldSize, data, sampleCount * sizeof(std::int16_t));
            }
            capture_->ReleaseBuffer(frames);
        }
        Compact();
        return true;
    }

    void MixInto(float* output, UINT32 frames) {
        if (!output || !active_) return;
        const std::size_t requested = static_cast<std::size_t>(frames) * kMixChannels;
        const std::size_t available = samples_.size() > sampleOffset_ ? samples_.size() - sampleOffset_ : 0;
        const std::size_t count = (std::min)(requested, available);
        for (std::size_t i = 0; i < count; ++i) {
            output[i] += static_cast<float>(samples_[sampleOffset_ + i]) * (gain_ / 32768.0f);
        }
        sampleOffset_ += count;
    }

    bool Active() const noexcept { return active_; }

private:
    bool FinishInitialize(bool loopback, std::wstring& error) {
        format_ = MixFormat();
        DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                      AUDCLNT_STREAMFLAGS_NOPERSIST;
        if (loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &format_, nullptr);
        if (FAILED(hr)) {
            // A few older drivers reject SRC_DEFAULT_QUALITY even though AUTOCONVERTPCM works.
            flags &= ~AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &format_, nullptr);
        }
        if (SUCCEEDED(hr)) hr = client_->GetService(IID_PPV_ARGS(&capture_));
        if (FAILED(hr)) { error = L"WASAPI 48 kHz mixer input: " + HResultText(hr); return false; }
        return true;
    }

    void Compact() {
        if (sampleOffset_ > static_cast<std::size_t>(kMixRate) * kMixChannels) {
            samples_.erase(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(sampleOffset_));
            sampleOffset_ = 0;
        }
        const std::size_t maxSamples = static_cast<std::size_t>(kMixRate) * kMixChannels * 2;
        const std::size_t available = samples_.size() - sampleOffset_;
        if (available > maxSamples) sampleOffset_ += available - maxSamples;
    }

    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    WAVEFORMATEX format_{};
    std::vector<std::int16_t> samples_;
    std::size_t sampleOffset_ = 0;
    float gain_ = 1.0f;
    bool active_ = false;
};
}  // namespace

class AudioMixer::Impl {
public:
    bool Initialize(const AppSettings& settings, std::wstring& warning) {
        Stop();
        bitrateKbps_ = NormalizeAacBitrateKbps(settings.audioBitrateKbps);
        std::vector<std::wstring> warnings;

        if (settings.recordApplicationAudio) {
            auto source = std::make_unique<AudioSource>();
            std::wstring error;
            if (source->InitializeProcess(settings.recordingProcessId,
                                          settings.applicationAudioVolumePercent / 100.0f, error)) {
                sources_.push_back(std::move(source));
            } else {
                warnings.push_back(error);
            }
        } else if (settings.recordSystemAudio) {
            auto source = std::make_unique<AudioSource>();
            std::wstring error;
            if (source->InitializeSystem(settings.systemAudioVolumePercent / 100.0f, error)) {
                sources_.push_back(std::move(source));
            } else {
                warnings.push_back(error);
            }
        }

        for (const auto& input : settings.microphoneInputs) {
            if (!input.enabled || input.deviceId.empty()) continue;
            auto source = std::make_unique<AudioSource>();
            std::wstring error;
            if (source->InitializeMicrophone(input.deviceId, input.volumePercent / 100.0f, error)) {
                sources_.push_back(std::move(source));
            } else {
                warnings.push_back(input.name + L": " + error);
            }
        }
        if (!warnings.empty()) {
            warning = warnings.front();
            if (warnings.size() > 1) warning += L" (and " + std::to_wstring(warnings.size() - 1) + L" more)";
        }
        return !sources_.empty();
    }

    bool ConfigureWriter(IMFSinkWriter* writer, DWORD& streamIndex, std::wstring& error) {
        if (!writer || sources_.empty()) return false;
        ComPtr<IMFMediaType> output;
        HRESULT hr = MFCreateMediaType(&output);
        if (SUCCEEDED(hr)) hr = output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kMixChannels);
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kMixRate);
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                                  static_cast<UINT32>(bitrateKbps_ * 1000 / 8));
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kMixBits);
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AVG_BITRATE,
                                                  static_cast<UINT32>(bitrateKbps_ * 1000));
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, FALSE);
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        if (SUCCEEDED(hr)) hr = writer->AddStream(output.Get(), &streamIndex);
        if (FAILED(hr)) { error = L"Configure mixed AAC output: " + HResultText(hr); return false; }

        ComPtr<IMFMediaType> input;
        hr = MFCreateMediaType(&input);
        if (SUCCEEDED(hr)) hr = input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kMixChannels);
        if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kMixRate);
        if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kMixChannels * kMixBits / 8);
        if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                                 kMixRate * kMixChannels * kMixBits / 8);
        if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kMixBits);
        if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_AVG_BITRATE,
                                                 kMixRate * kMixChannels * kMixBits);
        if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        if (SUCCEEDED(hr)) hr = input->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(streamIndex, input.Get(), nullptr);
        if (FAILED(hr)) { error = L"Configure 48 kHz audio mixer input: " + HResultText(hr); return false; }
        return true;
    }

    bool Start(std::wstring& error) {
        audioFrames_ = 0;
        startedAt_ = GetTickCount64();
        bool any = false;
        for (auto& source : sources_) {
            std::wstring sourceError;
            if (source->Start(sourceError)) any = true;
            else if (error.empty()) error = sourceError;
        }
        started_ = any;
        return any;
    }

    void Stop() {
        for (auto& source : sources_) source->Stop();
        sources_.clear();
        started_ = false;
        startedAt_ = 0;
        audioFrames_ = 0;
    }

    bool Drain(IMFSinkWriter* writer, DWORD streamIndex, std::wstring& error) {
        if (!started_ || !writer) return true;
        for (auto& source : sources_) {
            std::wstring sourceError;
            if (!source->Pull(sourceError) && error.empty()) error = L"Audio input disconnected: " + sourceError;
        }

        const UINT64 expectedFrames = static_cast<UINT64>(GetTickCount64() - startedAt_) * kMixRate / 1000;
        int chunks = 0;
        while (audioFrames_ + kChunkFrames <= expectedFrames && chunks++ < 12) {
            const std::size_t sampleCount = static_cast<std::size_t>(kChunkFrames) * kMixChannels;
            mixed_.assign(sampleCount, 0.0f);
            pcm_.resize(sampleCount);
            for (auto& source : sources_) source->MixInto(mixed_.data(), kChunkFrames);
            for (std::size_t i = 0; i < mixed_.size(); ++i) {
                const float limited = std::tanh(mixed_[i]);
                pcm_[i] = static_cast<std::int16_t>((std::max)(-1.0f, (std::min)(1.0f, limited)) * 32767.0f);
            }

            const DWORD bytes = static_cast<DWORD>(pcm_.size() * sizeof(std::int16_t));
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMFSample> sample;
            BYTE* destination = nullptr;
            HRESULT hr = MFCreateMemoryBuffer(bytes, &buffer);
            if (SUCCEEDED(hr)) hr = buffer->Lock(&destination, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                std::memcpy(destination, pcm_.data(), bytes);
                buffer->Unlock();
                hr = buffer->SetCurrentLength(bytes);
            }
            if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
            if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
            const LONGLONG time = static_cast<LONGLONG>(audioFrames_ * 10'000'000 / kMixRate);
            const LONGLONG duration = static_cast<LONGLONG>(static_cast<UINT64>(kChunkFrames) * 10'000'000 / kMixRate);
            if (SUCCEEDED(hr)) hr = sample->SetSampleTime(time);
            if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
            if (SUCCEEDED(hr)) hr = writer->WriteSample(streamIndex, sample.Get());
            if (FAILED(hr)) { error = L"Encode mixed audio: " + HResultText(hr); return false; }
            audioFrames_ += kChunkFrames;
        }
        return true;
    }

    bool HasSources() const noexcept { return !sources_.empty(); }
    std::size_t SourceCount() const noexcept { return sources_.size(); }

private:
    std::vector<std::unique_ptr<AudioSource>> sources_;
    std::vector<float> mixed_;
    std::vector<std::int16_t> pcm_;
    UINT64 audioFrames_ = 0;
    ULONGLONG startedAt_ = 0;
    int bitrateKbps_ = 192;
    bool started_ = false;
};

AudioMixer::AudioMixer() : impl_(std::make_unique<Impl>()) {}
AudioMixer::~AudioMixer() = default;

std::vector<AudioDeviceInfo> AudioMixer::EnumerateMicrophones() {
    std::vector<AudioDeviceInfo> result;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) return result;
    std::wstring defaultId;
    ComPtr<IMMDevice> defaultDevice;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &defaultDevice))) {
        wchar_t* id = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&id)) && id) { defaultId = id; CoTaskMemFree(id); }
    }
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices))) return result;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(i, &device))) continue;
        wchar_t* id = nullptr;
        if (FAILED(device->GetId(&id)) || !id) continue;
        AudioDeviceInfo info;
        info.id = id;
        info.isDefault = info.id == defaultId;
        CoTaskMemFree(id);
        ComPtr<IPropertyStore> properties;
        PROPVARIANT name{};
        PropVariantInit(&name);
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) &&
            SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR && name.pwszVal) {
            info.name = name.pwszVal;
        }
        PropVariantClear(&name);
        if (info.name.empty()) info.name = L"Microphone " + std::to_wstring(i + 1);
        result.push_back(std::move(info));
    }
    std::stable_sort(result.begin(), result.end(), [](const AudioDeviceInfo& a, const AudioDeviceInfo& b) {
        return a.isDefault && !b.isDefault;
    });
    return result;
}

float AudioMixer::QueryMicrophonePeak(const std::wstring& deviceId) {
    return QueryMicrophoneMeter(deviceId).peak;
}

AudioMeterState AudioMixer::QueryMicrophoneMeter(const std::wstring& deviceId) {
    AudioMeterState state;
    if (deviceId.empty()) return state;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioMeterInformation> meter;
    ComPtr<IAudioEndpointVolume> volume;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDevice(deviceId.c_str(), &device);
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr,
                                             reinterpret_cast<void**>(meter.GetAddressOf()));
    if (SUCCEEDED(hr) && SUCCEEDED(meter->GetPeakValue(&state.peak))) state.available = true;
    if (device && SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                             reinterpret_cast<void**>(volume.GetAddressOf())))) {
        BOOL muted = FALSE;
        if (SUCCEEDED(volume->GetMute(&muted))) state.muted = muted != FALSE;
    }
    state.peak = (std::max)(0.0f, (std::min)(1.0f, state.peak));
    return state;
}

bool AudioMixer::Initialize(const AppSettings& settings, std::wstring& warning) {
    return impl_->Initialize(settings, warning);
}
bool AudioMixer::ConfigureWriter(IMFSinkWriter* writer, DWORD& streamIndex, std::wstring& error) {
    return impl_->ConfigureWriter(writer, streamIndex, error);
}
bool AudioMixer::Start(std::wstring& error) { return impl_->Start(error); }
void AudioMixer::Stop() { impl_->Stop(); }
bool AudioMixer::Drain(IMFSinkWriter* writer, DWORD streamIndex, std::wstring& error) {
    return impl_->Drain(writer, streamIndex, error);
}
bool AudioMixer::HasSources() const noexcept { return impl_->HasSources(); }
std::size_t AudioMixer::SourceCount() const noexcept { return impl_->SourceCount(); }
