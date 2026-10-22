#include "audio/windows/WasapiBackend.h"

#include "audio/FormatProbe.h"
#include "util/FileUtil.h"
#include "util/Logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <windows.h>

// initguid.h makes the DEFINE_GUID / DEFINE_PROPERTYKEY macros in the headers
// below emit definitions instead of declarations, so no extra GUID import
// library is needed. It must appear exactly once in the program, which is why
// this is the only file that includes it.
#include <initguid.h>

// Include order matters here and is not alphabetical:
// mmdeviceapi.h brings in DEFINE_PROPERTYKEY, which
// functiondiscoverykeys_devpkey.h uses without declaring it.
#include <mmdeviceapi.h>

#include <audioclient.h>
#include <avrt.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>

#include <cstdio>
#include <cstring>

namespace multiroom {
namespace {

// Subformat GUIDs, spelled out so <ksmedia.h> (which drags in the whole
// kernel streaming header chain) is not needed.
const GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// Minimal COM pointer. Release on scope exit is the only behaviour needed
// here, and it keeps the many early returns below from leaking interfaces.
template <typename T>
class ComPtr
{
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** put()
    {
        reset();
        return &pointer_;
    }
    void** putVoid() { return reinterpret_cast<void**>(put()); }

    T* get() const { return pointer_; }
    T* operator->() const { return pointer_; }
    explicit operator bool() const { return pointer_ != nullptr; }

    void reset()
    {
        if (pointer_ != nullptr)
        {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

private:
    T* pointer_ = nullptr;
};

// Frees a CoTaskMemAlloc'd block on scope exit.
template <typename T>
class CoMem
{
public:
    CoMem() = default;
    ~CoMem()
    {
        if (pointer_ != nullptr)
            CoTaskMemFree(pointer_);
    }

    CoMem(const CoMem&) = delete;
    CoMem& operator=(const CoMem&) = delete;

    T** put() { return &pointer_; }
    T* get() const { return pointer_; }

private:
    T* pointer_ = nullptr;
};

// COM apartment for the calling thread. RPC_E_CHANGED_MODE means the thread is
// already initialized in another apartment, which is fine -- we just must not
// uninitialize it on the way out.
class ComApartment
{
public:
    ComApartment()
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = SUCCEEDED(hr);
        ok_ = initialized_ || hr == RPC_E_CHANGED_MODE;
    }

    ~ComApartment()
    {
        if (initialized_)
            CoUninitialize();
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    bool ok() const { return ok_; }

private:
    bool initialized_ = false;
    bool ok_ = false;
};

std::string hresultText(HRESULT hr)
{
    // The handful of codes that mean something specific to a user get a name;
    // "open failed, hr=0x88890004" helps nobody.
    switch (hr)
    {
    case AUDCLNT_E_DEVICE_IN_USE:
        return "device is already in use by another application in exclusive mode";
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
        return "exclusive mode is disabled for this endpoint in the Windows sound settings";
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        return "the device does not support this format";
    case AUDCLNT_E_DEVICE_INVALIDATED:
        return "the device was removed or reconfigured";
    // HRESULT_FROM_WIN32(ERROR_NOT_FOUND), spelled numerically because the
    // macro is an inline function in C++ and cannot label a case. This is what
    // a device id from the cache returns once the device is gone.
    case 0x80070490L:
        return "the device is no longer present";
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
        return "the audio endpoint could not be created";
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return "the Windows audio service is not running";
    default:
        break;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "hr=0x%08lx", static_cast<unsigned long>(hr));
    return std::string(buffer);
}

// Channel masks for the probed layouts. Spelled numerically for the same
// reason as the subformat GUIDs above.
DWORD channelMask(uint16_t channels)
{
    switch (channels)
    {
    case 1:
        return 0x4; // FRONT_CENTER
    case 2:
        return 0x3; // FRONT_LEFT | FRONT_RIGHT
    case 6:
        return 0x3F; // 5.1
    case 8:
        return 0x63F; // 7.1 surround
    default:
        return 0;
    }
}

void fillWaveFormat(const AudioFormat& format, WAVEFORMATEXTENSIBLE& out)
{
    const uint32_t containerBits = AudioFormat::bytesPerSample(format.sampleType) * 8;

    ZeroMemory(&out, sizeof(out));
    out.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    out.Format.nChannels = format.channels;
    out.Format.nSamplesPerSec = format.sampleRate;
    out.Format.wBitsPerSample = static_cast<WORD>(containerBits);
    out.Format.nBlockAlign = static_cast<WORD>(format.channels * (containerBits / 8));
    out.Format.nAvgBytesPerSec = format.sampleRate * out.Format.nBlockAlign;
    out.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    out.Samples.wValidBitsPerSample =
        static_cast<WORD>(AudioFormat::validBits(format.sampleType));
    out.dwChannelMask = channelMask(format.channels);
    out.SubFormat =
        format.sampleType == AudioFormat::SampleType::Float32 ? kSubtypeIeeeFloat : kSubtypePcm;
}

bool audioFormatFromWaveFormat(const WAVEFORMATEX* wfx, AudioFormat& out)
{
    if (wfx == nullptr)
        return false;

    bool isFloat = false;
    uint32_t validBits = wfx->wBitsPerSample;

    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
    {
        const WAVEFORMATEXTENSIBLE* extensible =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        if (IsEqualGUID(extensible->SubFormat, kSubtypeIeeeFloat))
            isFloat = true;
        else if (!IsEqualGUID(extensible->SubFormat, kSubtypePcm))
            return false;
        if (extensible->Samples.wValidBitsPerSample != 0)
            validBits = extensible->Samples.wValidBitsPerSample;
    }
    else if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
    {
        isFloat = true;
    }
    else if (wfx->wFormatTag != WAVE_FORMAT_PCM)
    {
        return false;
    }

    AudioFormat format;
    format.sampleRate = wfx->nSamplesPerSec;
    format.channels = wfx->nChannels;

    if (isFloat)
    {
        if (wfx->wBitsPerSample != 32)
            return false;
        format.sampleType = AudioFormat::SampleType::Float32;
    }
    else if (wfx->wBitsPerSample == 16)
    {
        format.sampleType = AudioFormat::SampleType::Int16;
    }
    else if (wfx->wBitsPerSample == 24)
    {
        format.sampleType = AudioFormat::SampleType::Int24;
    }
    else if (wfx->wBitsPerSample == 32)
    {
        format.sampleType = validBits == 24 ? AudioFormat::SampleType::Int24In32
                                            : AudioFormat::SampleType::Int32;
    }
    else
    {
        return false;
    }

    out = format;
    return true;
}

std::string readStringProperty(IPropertyStore* store, const PROPERTYKEY& key)
{
    if (store == nullptr)
        return std::string();

    PROPVARIANT value;
    PropVariantInit(&value);
    std::string text;
    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR)
        text = fileutil::fromWide(value.pwszVal);
    PropVariantClear(&value);
    return text;
}

// IsFormatSupported never opens the device, so the full probe is cheap on
// Windows -- unlike on ALSA, where each combination needs an open PCM handle.
void probeFormats(IAudioClient* client, AudioDeviceInfo& device)
{
    for (size_t t = 0; t < probe::kSampleTypeCount; ++t)
    {
        for (size_t c = 0; c < probe::kChannelCountCount; ++c)
        {
            for (size_t r = 0; r < probe::kSampleRateCount; ++r)
            {
                AudioFormat format;
                format.sampleRate = probe::kSampleRates[r];
                format.channels = probe::kChannelCounts[c];
                format.sampleType = probe::kSampleTypes[t];

                WAVEFORMATEXTENSIBLE wfx;
                fillWaveFormat(format, wfx);

                // Shared mode: the mixer only accepts its own rate and channel
                // count, so S_FALSE (a "closest match" was offered) counts as
                // unsupported here. We want the exact format or nothing.
                CoMem<WAVEFORMATEX> closest;
                if (client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &wfx.Format,
                                              closest.put()) == S_OK)
                {
                    device.sharedFormats.push_back(format);
                }

                // Exclusive mode takes no closest-match argument.
                if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wfx.Format,
                                              nullptr) == S_OK)
                {
                    device.exclusiveFormats.push_back(format);
                }
            }
        }
    }
}

// The endpoint's volume, when the device applies it. Without
// ENDPOINT_HARDWARE_SUPPORT_VOLUME the endpoint volume is the audio engine
// scaling the samples in software -- exactly what hardware volume must not
// be -- so such a device is left to VolumeControl.
void describeHardwareVolume(IMMDevice* endpoint, HardwareVolumeInfo& out)
{
    ComPtr<IAudioEndpointVolume> volume;
    if (FAILED(endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                  volume.putVoid())))
        return;

    DWORD support = 0;
    if (FAILED(volume->QueryHardwareSupport(&support)) ||
        (support & ENDPOINT_HARDWARE_SUPPORT_VOLUME) == 0)
        return;

    float minDb = 0.0f;
    float maxDb = 0.0f;
    float incrementDb = 0.0f;
    UINT step = 0;
    UINT stepCount = 0;
    if (FAILED(volume->GetVolumeRange(&minDb, &maxDb, &incrementDb)) || !(maxDb > minDb) ||
        FAILED(volume->GetVolumeStepInfo(&step, &stepCount)) || stepCount < 2)
        return;

    HardwareVolumeInfo info;
    info.available = true;
    info.control = "Endpoint volume";
    info.minStep = 0;
    info.maxStep = static_cast<int64_t>(stepCount) - 1;
    info.hasDecibels = true;
    info.minDb = minDb;
    info.maxDb = maxDb;
    info.hasMute = (support & ENDPOINT_HARDWARE_SUPPORT_MUTE) != 0;
    out = info;
}

// The same control, opened for the room's volume. Lives on the control
// thread, which it joins to the MTA for as long as it exists.
class WasapiHardwareVolume final : public IHardwareVolume
{
public:
    explicit WasapiHardwareVolume(const HardwareVolumeInfo& info) : info_(info) {}

    bool open(const std::string& deviceId, std::string& error)
    {
        if (!apartment_.ok())
        {
            error = "CoInitializeEx failed";
            return false;
        }
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), enumerator.putVoid());
        ComPtr<IMMDevice> endpoint;
        if (SUCCEEDED(hr))
            hr = enumerator->GetDevice(fileutil::toWide(deviceId).c_str(), endpoint.put());
        if (SUCCEEDED(hr))
            hr = endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                    volume_.putVoid());
        if (FAILED(hr))
        {
            error = "cannot open the endpoint volume: " + hresultText(hr);
            return false;
        }
        return true;
    }

    bool set(double gain, bool muted, std::string& error) override
    {
        const double db = muted && !info_.hasMute ? info_.minDb
                                                  : hardware_volume::decibelsForGain(info_, gain);
        HRESULT hr = volume_->SetMasterVolumeLevel(static_cast<float>(db), nullptr);
        if (SUCCEEDED(hr) && info_.hasMute)
            hr = volume_->SetMute(muted ? TRUE : FALSE, nullptr);
        if (FAILED(hr))
        {
            error = "cannot set the endpoint volume: " + hresultText(hr);
            return false;
        }
        return true;
    }

    bool setMute(bool muted, std::string& error) override
    {
        const HRESULT hr =
            info_.hasMute ? volume_->SetMute(muted ? TRUE : FALSE, nullptr) : E_NOTIMPL;
        if (FAILED(hr))
        {
            error = "cannot mute the endpoint: " + hresultText(hr);
            return false;
        }
        return true;
    }

    bool read(HardwareVolumeState& out, std::string& error) override
    {
        float db = 0.0f;
        BOOL muted = FALSE;
        HRESULT hr = volume_->GetMasterVolumeLevel(&db);
        if (SUCCEEDED(hr) && info_.hasMute)
            hr = volume_->GetMute(&muted);
        if (FAILED(hr))
        {
            error = "cannot read the endpoint volume: " + hresultText(hr);
            return false;
        }
        out = HardwareVolumeState();
        out.hasDecibels = true;
        out.db = db;
        out.gain = hardware_volume::gainForDecibels(info_, db);
        out.muted = muted != FALSE;
        return true;
    }

private:
    ComApartment apartment_; // first in, last out: the pointer is released inside it
    HardwareVolumeInfo info_;
    ComPtr<IAudioEndpointVolume> volume_;
};

bool describeDevice(IMMDevice* endpoint, const std::string& defaultId, CapabilityScan scan,
                    AudioDeviceInfo& out)
{
    CoMem<WCHAR> id;
    HRESULT hr = endpoint->GetId(id.put());
    if (FAILED(hr))
    {
        MULTIROOM_WARN("wasapi: GetId failed (%s)", hresultText(hr).c_str());
        return false;
    }

    AudioDeviceInfo device;
    device.id = fileutil::fromWide(id.get());
    device.isDefault = !device.id.empty() && device.id == defaultId;

    ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ, properties.put())))
    {
        device.name = readStringProperty(properties.get(), PKEY_Device_FriendlyName);
        device.description = readStringProperty(properties.get(), PKEY_Device_DeviceDesc);
    }
    if (device.name.empty())
        device.name = device.id;

    describeHardwareVolume(endpoint, device.hardwareVolume);

    ComPtr<IAudioClient> client;
    hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid());
    if (FAILED(hr))
    {
        // A device that cannot be activated is still worth listing; it just has
        // no capabilities we can report.
        MULTIROOM_WARN("wasapi: cannot activate %s (%s)", device.name.c_str(),
                 hresultText(hr).c_str());
        out = device;
        return true;
    }

    CoMem<WAVEFORMATEX> mixFormat;
    if (SUCCEEDED(client->GetMixFormat(mixFormat.put())))
        device.hasMixFormat = audioFormatFromWaveFormat(mixFormat.get(), device.mixFormat);

    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minimumPeriod = 0;
    if (SUCCEEDED(client->GetDevicePeriod(&defaultPeriod, &minimumPeriod)))
    {
        // REFERENCE_TIME counts 100 ns units.
        device.buffer.defaultPeriodNs = static_cast<uint64_t>(defaultPeriod) * 100;
        device.buffer.minPeriodNs = static_cast<uint64_t>(minimumPeriod) * 100;
        if (device.hasMixFormat && device.mixFormat.sampleRate > 0)
        {
            const uint64_t rate = device.mixFormat.sampleRate;
            device.buffer.minPeriodFrames =
                static_cast<uint32_t>(device.buffer.minPeriodNs * rate / 1000000000ULL);
            device.buffer.maxPeriodFrames = 0; // WASAPI reports no upper bound
        }
    }

    if (scan == CapabilityScan::Full)
    {
        probeFormats(client.get(), device);
    }
    else if (device.hasMixFormat)
    {
        device.sharedFormats.push_back(device.mixFormat);
    }

    out = device;
    return true;
}

REFERENCE_TIME durationForFrames(uint32_t frames, uint32_t sampleRate)
{
    if (sampleRate == 0)
        return 0;
    // REFERENCE_TIME is in 100 ns units; round up so the buffer is never short.
    const double units = 10000000.0 * static_cast<double>(frames) /
                         static_cast<double>(sampleRate);
    return static_cast<REFERENCE_TIME>(units + 0.5);
}

// Why the render thread ended the stream. Stored as a number so the thread
// never has to build a string.
enum class RenderFailure : int
{
    None = 0,
    StoppedSignalling,
    GetCurrentPadding,
    GetBuffer,
    ReleaseBuffer,
};

} // namespace

// Holds everything that would otherwise force windows.h into the header.
struct WasapiBackend::Impl
{
    // Created in open() and destroyed in close(), both on the control thread.
    // MTA, so the render thread may use these interfaces too.
    std::unique_ptr<ComApartment> apartment;

    ComPtr<IMMDevice> endpoint;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> renderClient;
    ComPtr<IAudioClock> clock;

    // Units per second that IAudioClock::GetPosition counts in. Often the
    // sample rate, often the byte rate -- it must be queried, never assumed.
    UINT64 clockFrequency = 0;

    HANDLE renderEvent = nullptr;
    AUDCLNT_SHAREMODE shareMode = AUDCLNT_SHAREMODE_EXCLUSIVE;
    uint32_t bytesPerFrame = 0;

    ~Impl()
    {
        if (renderEvent != nullptr)
            CloseHandle(renderEvent);
    }
};

WasapiBackend::WasapiBackend()
    : impl_(new Impl()),
      state_(static_cast<int>(BackendState::Closed)),
      stopRequested_(false),
      underruns_(0),
      renderFailure_(static_cast<int>(RenderFailure::None)),
      renderFailureCode_(0)
{
}

WasapiBackend::~WasapiBackend()
{
    close();
}

BackendState WasapiBackend::state() const
{
    return static_cast<BackendState>(state_.load(std::memory_order_acquire));
}

uint64_t WasapiBackend::underrunCount() const
{
    return underruns_.load(std::memory_order_relaxed);
}

std::unique_ptr<IHardwareVolume> WasapiBackend::openHardwareVolume(
    const AudioDeviceInfo& device) const
{
    if (!device.hardwareVolume.available || device.id.empty())
        return std::unique_ptr<IHardwareVolume>();

    std::unique_ptr<WasapiHardwareVolume> volume(new WasapiHardwareVolume(device.hardwareVolume));
    std::string error;
    if (!volume->open(device.id, error))
    {
        MULTIROOM_WARN("wasapi: %s; the room's volume stays in software", error.c_str());
        return std::unique_ptr<IHardwareVolume>();
    }
    return volume;
}

std::string WasapiBackend::lastError() const
{
    // The render thread stores these before it publishes Failed; the text is
    // built here, on the caller's thread.
    const auto failure = static_cast<RenderFailure>(renderFailure_.load(std::memory_order_acquire));
    const HRESULT hr = static_cast<HRESULT>(renderFailureCode_.load(std::memory_order_relaxed));
    switch (failure)
    {
    case RenderFailure::None:
        break;
    case RenderFailure::StoppedSignalling:
        return "the audio device stopped signalling";
    case RenderFailure::GetCurrentPadding:
        return "GetCurrentPadding failed: " + hresultText(hr);
    case RenderFailure::GetBuffer:
        return "GetBuffer failed: " + hresultText(hr);
    case RenderFailure::ReleaseBuffer:
        return "ReleaseBuffer failed: " + hresultText(hr);
    }
    return lastError_;
}

std::vector<AudioDeviceInfo> WasapiBackend::enumeratePlaybackDevices(CapabilityScan scan)
{
    std::vector<AudioDeviceInfo> devices;

    ComApartment apartment;
    if (!apartment.ok())
    {
        lastError_ = "CoInitializeEx failed";
        MULTIROOM_ERROR("wasapi: %s", lastError_.c_str());
        return devices;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), enumerator.putVoid());
    if (FAILED(hr))
    {
        lastError_ = "cannot create MMDeviceEnumerator: " + hresultText(hr);
        MULTIROOM_ERROR("wasapi: %s", lastError_.c_str());
        return devices;
    }

    // Resolved first so every endpoint can be compared against it. A machine
    // with no audio hardware has no default endpoint, which is not an error.
    std::string defaultId;
    {
        ComPtr<IMMDevice> defaultEndpoint;
        if (SUCCEEDED(
                enumerator->GetDefaultAudioEndpoint(eRender, eConsole, defaultEndpoint.put())))
        {
            CoMem<WCHAR> id;
            if (SUCCEEDED(defaultEndpoint->GetId(id.put())))
                defaultId = fileutil::fromWide(id.get());
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put());
    if (FAILED(hr))
    {
        lastError_ = "EnumAudioEndpoints failed: " + hresultText(hr);
        MULTIROOM_ERROR("wasapi: %s", lastError_.c_str());
        return devices;
    }

    UINT count = 0;
    if (FAILED(collection->GetCount(&count)))
    {
        lastError_ = "IMMDeviceCollection::GetCount failed";
        MULTIROOM_ERROR("wasapi: %s", lastError_.c_str());
        return devices;
    }

    devices.reserve(count);
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> endpoint;
        if (FAILED(collection->Item(i, endpoint.put())))
            continue;

        AudioDeviceInfo device;
        if (describeDevice(endpoint.get(), defaultId, scan, device))
            devices.push_back(device);
    }

    return devices;
}

bool WasapiBackend::open(const AudioBackendConfig& config)
{
    stop();

    // Held for the whole function: clockSnapshot() may run on a telemetry
    // thread, and it must never see half-built COM interfaces. Every early
    // return below releases it.
    std::lock_guard<std::mutex> lock(clockMutex_);
    closeLocked();
    lastError_.clear();
    renderFailure_.store(static_cast<int>(RenderFailure::None), std::memory_order_relaxed);

    // Every failure releases whatever was built up to it. An exclusive-mode
    // client that got as far as Initialize holds the device, and the state is
    // Closed on a failed open, so nothing else would release it before the
    // next attempt -- other applications would find the card taken through
    // the whole backoff.
    if (openLocked(config))
        return true;
    closeLocked();
    return false;
}

bool WasapiBackend::openLocked(const AudioBackendConfig& config)
{
    if (config.renderCallback == nullptr)
    {
        lastError_ = "no render callback";
        return false;
    }
    if (!config.format.isValid())
    {
        lastError_ = "invalid audio format";
        return false;
    }

    impl_->apartment.reset(new ComApartment());
    if (!impl_->apartment->ok())
    {
        lastError_ = "CoInitializeEx failed";
        return false;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), enumerator.putVoid());
    if (FAILED(hr))
    {
        lastError_ = "cannot create MMDeviceEnumerator: " + hresultText(hr);
        return false;
    }

    if (config.deviceId.empty())
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, impl_->endpoint.put());
    else
        hr = enumerator->GetDevice(fileutil::toWide(config.deviceId).c_str(), impl_->endpoint.put());
    if (FAILED(hr))
    {
        lastError_ = "cannot open device: " + hresultText(hr);
        return false;
    }

    hr = impl_->endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   impl_->client.putVoid());
    if (FAILED(hr))
    {
        lastError_ = "cannot activate device: " + hresultText(hr);
        return false;
    }

    impl_->shareMode = config.accessMode == AudioAccessMode::Exclusive
                           ? AUDCLNT_SHAREMODE_EXCLUSIVE
                           : AUDCLNT_SHAREMODE_SHARED;

    WAVEFORMATEXTENSIBLE wfx;
    fillWaveFormat(config.format, wfx);

    // Checked up front so the failure names the format rather than surfacing as
    // a generic Initialize error. Never substitutes the other share mode: the
    // caller decided that, and falling back costs clock accuracy.
    CoMem<WAVEFORMATEX> closest;
    hr = impl_->client->IsFormatSupported(
        impl_->shareMode, &wfx.Format,
        impl_->shareMode == AUDCLNT_SHAREMODE_SHARED ? closest.put() : nullptr);
    if (hr != S_OK)
    {
        lastError_ = std::string("device rejected ") + describeFormat(config.format) + " in " +
                     audioAccessModeName(config.accessMode) + " mode: " + hresultText(hr);
        return false;
    }

    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minimumPeriod = 0;
    impl_->client->GetDevicePeriod(&defaultPeriod, &minimumPeriod);

    REFERENCE_TIME duration = defaultPeriod;
    if (config.requestedBufferFrames > 0)
    {
        duration = durationForFrames(config.requestedBufferFrames, config.format.sampleRate);
        if (duration < minimumPeriod)
            duration = minimumPeriod;
    }

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    // Event-driven exclusive mode requires periodicity == buffer duration;
    // shared mode requires periodicity == 0.
    const REFERENCE_TIME periodicity =
        impl_->shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE ? duration : 0;

    hr = impl_->client->Initialize(impl_->shareMode, flags, duration, periodicity, &wfx.Format,
                                   nullptr);

    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
    {
        // Exclusive mode insists on a buffer size that is a whole number of
        // hardware periods. WASAPI tells us the aligned size, but the client
        // object is now unusable and has to be activated again.
        UINT32 alignedFrames = 0;
        if (SUCCEEDED(impl_->client->GetBufferSize(&alignedFrames)) && alignedFrames > 0)
        {
            duration = durationForFrames(alignedFrames, config.format.sampleRate);
            impl_->client.reset();
            hr = impl_->endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           impl_->client.putVoid());
            if (SUCCEEDED(hr))
            {
                MULTIROOM_DEBUG("wasapi: retrying with the aligned buffer size of %u frames",
                          alignedFrames);
                hr = impl_->client->Initialize(impl_->shareMode, flags, duration, duration,
                                               &wfx.Format, nullptr);
            }
        }
    }

    if (FAILED(hr))
    {
        lastError_ = "IAudioClient::Initialize failed: " + hresultText(hr);
        impl_->client.reset();
        impl_->endpoint.reset();
        return false;
    }

    UINT32 frames = 0;
    hr = impl_->client->GetBufferSize(&frames);
    if (FAILED(hr) || frames == 0)
    {
        lastError_ = "GetBufferSize failed: " + hresultText(hr);
        return false;
    }

    impl_->renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (impl_->renderEvent == nullptr)
    {
        lastError_ = "could not create the render event";
        return false;
    }
    hr = impl_->client->SetEventHandle(impl_->renderEvent);
    if (FAILED(hr))
    {
        lastError_ = "SetEventHandle failed: " + hresultText(hr);
        return false;
    }

    hr = impl_->client->GetService(__uuidof(IAudioRenderClient), impl_->renderClient.putVoid());
    if (FAILED(hr))
    {
        lastError_ = "cannot obtain IAudioRenderClient: " + hresultText(hr);
        return false;
    }

    // The hardware clock. Playback works without it, so a failure here is a
    // warning rather than an error -- but drift measurement is then blind, and
    // the snapshot quality says so.
    hr = impl_->client->GetService(__uuidof(IAudioClock), impl_->clock.putVoid());
    if (SUCCEEDED(hr))
    {
        if (FAILED(impl_->clock->GetFrequency(&impl_->clockFrequency)) ||
            impl_->clockFrequency == 0)
        {
            MULTIROOM_WARN("wasapi: IAudioClock reported no frequency; drift measurement disabled");
            impl_->clock.reset();
        }
    }
    else
    {
        MULTIROOM_WARN("wasapi: no IAudioClock on this device (%s); drift measurement disabled",
                 hresultText(hr).c_str());
    }

    format_ = config.format;
    bufferFrames_ = frames;
    impl_->bytesPerFrame = config.format.bytesPerFrame();
    renderCallback_ = config.renderCallback;
    userData_ = config.userData;
    underruns_.store(0, std::memory_order_relaxed);
    state_.store(static_cast<int>(BackendState::Open), std::memory_order_release);
    return true;
}

bool WasapiBackend::start()
{
    if (state() == BackendState::Running)
        return true;
    if (state() != BackendState::Open)
    {
        lastError_ = "device is not open";
        return false;
    }

    // Prefill one buffer before starting the clock, otherwise the device runs
    // dry between Start() and the first event.
    BYTE* data = nullptr;
    HRESULT hr = impl_->renderClient->GetBuffer(bufferFrames_, &data);
    if (FAILED(hr))
    {
        lastError_ = "cannot prefill the device buffer: " + hresultText(hr);
        return false;
    }
    const uint32_t filled = renderCallback_(userData_, data, bufferFrames_);
    if (filled < bufferFrames_)
    {
        std::memset(data + static_cast<size_t>(filled) * impl_->bytesPerFrame, 0,
                    static_cast<size_t>(bufferFrames_ - filled) * impl_->bytesPerFrame);
    }
    impl_->renderClient->ReleaseBuffer(bufferFrames_, 0);

    shutdown_.prepare(format_);
    stopRequested_.store(false, std::memory_order_release);
    // An earlier stop() can leave the auto-reset event signalled: its own
    // wake-up, or a period the thread exited without waiting for. The new
    // thread would then ask for a buffer the prefill still occupies, which
    // exclusive mode refuses with AUDCLNT_E_BUFFER_TOO_LARGE.
    ResetEvent(impl_->renderEvent);

    hr = impl_->client->Start();
    if (FAILED(hr))
    {
        lastError_ = "IAudioClient::Start failed: " + hresultText(hr);
        return false;
    }

    state_.store(static_cast<int>(BackendState::Running), std::memory_order_release);
    renderThread_ = std::thread(&WasapiBackend::renderLoop, this);
    return true;
}

void WasapiBackend::renderLoop()
{
    // The render thread calls COM interfaces, so it needs its own apartment.
    ComApartment apartment;

    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (task == nullptr)
    {
        // Not fatal: MMCSS is unavailable on some systems (Server SKUs), and a
        // raised ordinary priority is still far better than nothing.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }

    auto lastSignal = std::chrono::steady_clock::now();
    unsigned drainEvents = 0;
    while (true)
    {
        shutdown_.begin(stopRequested_.load(std::memory_order_acquire));
        if (shutdown_.active() && shutdown_.remainingMs() == 0)
            break;
        // Two seconds is far beyond any sane period; reaching it means the
        // driver stopped feeding us and the stream is dead.
        const DWORD wait = WaitForSingleObject(impl_->renderEvent,
            static_cast<DWORD>(std::min(100, shutdown_.remainingMs())));
        shutdown_.begin(stopRequested_.load(std::memory_order_acquire));

        if (wait == WAIT_TIMEOUT && (shutdown_.active() ||
            std::chrono::steady_clock::now() - lastSignal < std::chrono::seconds(2)))
            continue;

        if (wait != WAIT_OBJECT_0)
        {
            renderFailure_.store(static_cast<int>(RenderFailure::StoppedSignalling),
                                 std::memory_order_release);
            state_.store(static_cast<int>(BackendState::Failed), std::memory_order_release);
            break;
        }
        lastSignal = std::chrono::steady_clock::now();

        if (shutdown_.finished())
        {
            if (impl_->shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE)
            {
                // Exclusive event mode has two ping-pong buffers. Let both
                // return after the tail; the first is refilled with zeros.
                if (++drainEvents == 2)
                    break;
            }
            else
            {
                // Stop submitting data so shared-mode padding can reach zero.
                UINT32 padding = 0;
                while (shutdown_.remainingMs() != 0 &&
                       SUCCEEDED(impl_->client->GetCurrentPadding(&padding)) && padding != 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                break;
            }
        }

        UINT32 framesToWrite = bufferFrames_;
        if (impl_->shareMode == AUDCLNT_SHAREMODE_SHARED)
        {
            UINT32 padding = 0;
            const HRESULT hr = impl_->client->GetCurrentPadding(&padding);
            if (FAILED(hr))
            {
                renderFailureCode_.store(static_cast<int32_t>(hr), std::memory_order_relaxed);
                renderFailure_.store(static_cast<int>(RenderFailure::GetCurrentPadding),
                                     std::memory_order_release);
                state_.store(static_cast<int>(BackendState::Failed), std::memory_order_release);
                break;
            }
            framesToWrite = bufferFrames_ > padding ? bufferFrames_ - padding : 0;
        }

        if (framesToWrite == 0)
            continue;

        BYTE* data = nullptr;
        const HRESULT hr = impl_->renderClient->GetBuffer(framesToWrite, &data);
        if (FAILED(hr))
        {
            renderFailureCode_.store(static_cast<int32_t>(hr), std::memory_order_relaxed);
            renderFailure_.store(static_cast<int>(RenderFailure::GetBuffer),
                                 std::memory_order_release);
            state_.store(static_cast<int>(BackendState::Failed), std::memory_order_release);
            break;
        }

        const uint32_t filled = shutdown_.render(renderCallback_, userData_, data, framesToWrite);

        DWORD bufferFlags = 0;
        if (filled < framesToWrite)
        {
            // The source could not keep up. Silence the rest rather than
            // replaying whatever the buffer happened to contain.
            std::memset(data + static_cast<size_t>(filled) * impl_->bytesPerFrame, 0,
                        static_cast<size_t>(framesToWrite - filled) * impl_->bytesPerFrame);
            underruns_.fetch_add(1, std::memory_order_relaxed);
            if (filled == 0)
                bufferFlags = AUDCLNT_BUFFERFLAGS_SILENT;
        }

        const HRESULT released = impl_->renderClient->ReleaseBuffer(framesToWrite, bufferFlags);
        if (FAILED(released))
        {
            renderFailureCode_.store(static_cast<int32_t>(released), std::memory_order_relaxed);
            renderFailure_.store(static_cast<int>(RenderFailure::ReleaseBuffer),
                                 std::memory_order_release);
            state_.store(static_cast<int>(BackendState::Failed), std::memory_order_release);
            break;
        }
    }

    if (task != nullptr)
        AvRevertMmThreadCharacteristics(task);
}

void WasapiBackend::stop()
{
    if (state() != BackendState::Running && !renderThread_.joinable())
        return;

    stopRequested_.store(true, std::memory_order_release);
    if (renderThread_.joinable())
        renderThread_.join();

    if (impl_->client)
    {
        impl_->client->Stop();
        // Reset so a later start() begins from an empty buffer rather than
        // whatever was left behind.
        impl_->client->Reset();
    }

    // A failed stream stays failed; only a healthy one returns to Open.
    if (state() == BackendState::Running)
        state_.store(static_cast<int>(BackendState::Open), std::memory_order_release);
}

void WasapiBackend::close()
{
    // Outside the lock: stop() joins the render thread, and holding a lock a
    // telemetry thread may be waiting on across a join invites trouble.
    stop();

    std::lock_guard<std::mutex> lock(clockMutex_);
    closeLocked();
}

void WasapiBackend::closeLocked()
{
    impl_->clock.reset();
    impl_->clockFrequency = 0;
    impl_->renderClient.reset();
    impl_->client.reset();
    impl_->endpoint.reset();

    if (impl_->renderEvent != nullptr)
    {
        CloseHandle(impl_->renderEvent);
        impl_->renderEvent = nullptr;
    }
    impl_->apartment.reset();

    renderCallback_ = nullptr;
    userData_ = nullptr;
    bufferFrames_ = 0;
    format_ = AudioFormat();
    state_.store(static_cast<int>(BackendState::Closed), std::memory_order_release);
}

AudioClockSnapshot WasapiBackend::clockSnapshot()
{
    AudioClockSnapshot snapshot;

    std::lock_guard<std::mutex> lock(clockMutex_);
    if (!impl_->clock || impl_->clockFrequency == 0 || !format_.isValid())
        return snapshot;

    UINT64 position = 0;
    UINT64 qpcPosition = 0;
    const HRESULT hr = impl_->clock->GetPosition(&position, &qpcPosition);
    if (FAILED(hr))
        return snapshot;

    // pu64QPCPosition is the performance counter at the moment the device
    // position was read, already converted to 100 ns units -- the same time
    // base monotonicNowNs() produces, just at a coarser scale.
    snapshot.monotonicTimeNs = qpcPosition * 100;

    // GetPosition counts in clockFrequency units per second, which is the
    // sample rate for some drivers and the byte rate for others.
    if (impl_->clockFrequency == format_.sampleRate)
    {
        snapshot.deviceFramePosition = position;
    }
    else
    {
        snapshot.deviceFramePosition =
            position * static_cast<UINT64>(format_.sampleRate) / impl_->clockFrequency;
    }

    snapshot.nominalSampleRate = format_.sampleRate;

    if (hr == S_FALSE)
    {
        // The driver told us the position may be stale.
        snapshot.quality = AudioClockQuality::Estimated;
    }
    else if (impl_->shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE)
    {
        // Nothing between us and the endpoint: the driver's position of the
        // card, read together with the QPC. WASAPI offers no link timestamp,
        // so this is Driver on the same scale as ALSA's driver_htstamp.
        snapshot.quality = AudioClockQuality::Driver;
    }
    else
    {
        // In shared mode the position comes from the system mixer's engine,
        // which resamples. It is not the hardware's rate -- which is exactly
        // why exclusive mode is preferred.
        snapshot.quality = AudioClockQuality::Estimated;
    }

    return snapshot;
}

} // namespace multiroom
