// Exercise the actual ALSA scan, cache merge and saved cache without hardware.
#undef NDEBUG
#include "app/DeviceSelection.h"

#include <alsa/asoundlib.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {
bool cardPresent = true;
int exclusiveResult = -EBUSY;
int sharedResult = -EBUSY;
int defaultResult = -EBUSY;
}

extern "C" int __wrap_snd_card_next(int* card)
{
    *card = cardPresent && *card == -1 ? 42 : -1;
    return 0;
}
extern "C" int __wrap_snd_ctl_open(snd_ctl_t** control, const char* name, int)
{
    assert(std::strcmp(name, "hw:42") == 0);
    *control = reinterpret_cast<snd_ctl_t*>(1);
    return 0;
}
extern "C" int __wrap_snd_ctl_close(snd_ctl_t*) { return 0; }
extern "C" int __wrap_snd_ctl_card_info(snd_ctl_t*, snd_ctl_card_info_t*) { return 0; }
extern "C" const char* __wrap_snd_ctl_card_info_get_id(const snd_ctl_card_info_t*) { return "BusyTest"; }
extern "C" const char* __wrap_snd_ctl_card_info_get_name(const snd_ctl_card_info_t*) { return "Test card"; }
extern "C" const char* __wrap_snd_ctl_card_info_get_longname(const snd_ctl_card_info_t*) { return "Test card description"; }
extern "C" int __wrap_snd_ctl_pcm_next_device(snd_ctl_t*, int* device)
{
    *device = *device == -1 ? 0 : -1;
    return 0;
}
extern "C" int __wrap_snd_ctl_pcm_info(snd_ctl_t*, snd_pcm_info_t*) { return 0; }
extern "C" const char* __wrap_snd_pcm_info_get_name(const snd_pcm_info_t*) { return "Test PCM"; }
extern "C" int __wrap_snd_mixer_attach(snd_mixer_t*, const char*) { return -ENODEV; }
extern "C" int __real_snd_pcm_open(snd_pcm_t**, const char*, snd_pcm_stream_t, int);
extern "C" int __wrap_snd_pcm_open(snd_pcm_t** pcm, const char* name, snd_pcm_stream_t stream, int mode)
{
    assert(stream == SND_PCM_STREAM_PLAYBACK);
    assert(mode == SND_PCM_NONBLOCK);
    const int result = std::strcmp(name, "default") == 0 ? defaultResult
        : std::strcmp(name, "hw:CARD=BusyTest,DEV=0") == 0 ? exclusiveResult : sharedResult;
    if (result != 0)
        return result;
    // Real null PCM parameter probing, with no physical output or audio thread.
    return __real_snd_pcm_open(pcm, "null", stream, mode);
}

int main()
{
    using namespace multiroom;
    AudioDeviceInfo hardware;
    hardware.id = "hw:CARD=BusyTest,DEV=0";
    hardware.name = "Cached card name";
    AudioFormat exclusive;
    exclusive.sampleRate = 44100;
    exclusive.channels = 2;
    exclusive.sampleType = AudioFormat::SampleType::Int24In32;
    AudioFormat shared = exclusive;
    shared.sampleRate = 48000;
    hardware.exclusiveFormats = {exclusive};
    hardware.sharedFormats = {shared};
    hardware.buffer.minBufferFrames = 123;
    hardware.buffer.maxBufferFrames = 456;
    AudioDeviceInfo defaultDevice;
    defaultDevice.id = "default";
    defaultDevice.name = "Cached default";
    defaultDevice.isDefault = true;
    defaultDevice.sharedFormats = {shared};
    AudioDeviceCacheData original;
    original.devices = {defaultDevice, hardware};

    char path[] = "/tmp/galdrun-alsa-rescan-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    std::string error;
    auto scan = [&] {
        AudioDeviceCacheData cache = original;
        assert(rescanAudioDevices(path, I2sSettings(), cache, error));
        AudioDeviceCacheData saved;
        assert(device_cache::load(path, saved, error) == device_cache::LoadResult::Ok);
        return saved;
    };

    auto cache = scan();
    auto device = cache.findById(hardware.id);
    assert(device && device->exclusiveFormats == hardware.exclusiveFormats);
    assert(device->sharedFormats == hardware.sharedFormats);
    assert(device->buffer.minBufferFrames == 123 && device->buffer.maxBufferFrames == 456);
    assert(device->name == "Test card: Test PCM");
    assert(cache.findById("default") && cache.findById("default")->sharedFormats == defaultDevice.sharedFormats);

    sharedResult = defaultResult = 0;
    cache = scan();
    device = cache.findById(hardware.id);
    assert(device && device->exclusiveFormats == hardware.exclusiveFormats);
    assert(!device->sharedFormats.empty() && device->sharedFormats != hardware.sharedFormats);

    exclusiveResult = 0;
    sharedResult = -EBUSY;
    cache = scan();
    device = cache.findById(hardware.id);
    assert(device && !device->exclusiveFormats.empty() && device->exclusiveFormats != hardware.exclusiveFormats);
    assert(device->sharedFormats == hardware.sharedFormats);
    assert(device->buffer.minBufferFrames != 123);

    exclusiveResult = sharedResult = -ENODEV;
    cache = scan();
    device = cache.findById(hardware.id);
    assert(device && device->exclusiveFormats.empty() && device->sharedFormats.empty());

    cardPresent = false;
    cache = scan();
    assert(!cache.findById(hardware.id));

    cardPresent = true;
    exclusiveResult = sharedResult = defaultResult = -EBUSY;
    original.devices.clear();
    cache = scan();
    device = cache.findById(hardware.id);
    assert(device && device->exclusiveFormats.empty() && device->sharedFormats.empty());
    unlink(path);
    std::puts("PASS: busy-mode formats/buffer persist, fresh probes replace them, errors/removal clear them and unknown devices gain no formats");
}
