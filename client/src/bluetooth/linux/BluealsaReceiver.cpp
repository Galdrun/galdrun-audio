#include "bluetooth/linux/BluealsaReceiver.h"

#include "util/Logger.h"

#include <alsa/asoundlib.h>
#include <dirent.h>
#include <spawn.h>
#include <sys/wait.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

extern char** environ;

namespace multiroom {
namespace {

// The most recently connected device's A2DP stream; for a capture PCM that is
// the sink side, i.e. a phone sending to us.
const char* const kPcmName = "bluealsa:DEV=00:00:00:00:00:00,PROFILE=a2dp";
const char* const kHelper = "/usr/sbin/galdrun-bluetooth";
const unsigned kPeriodUs = 20000;
const unsigned kBufferUs = 200000;

// The ACL links, each "hci0:<handle>", joined; empty with none. Opening the
// plugin with no phone stream fails and makes alsa-lib print to stderr, once a
// try: the loop below only tries with a link, and seldom on links that failed
// (headphones this device plays to are a link without a phone stream).
std::string links()
{
    DIR* directory = ::opendir("/sys/class/bluetooth");
    if (directory == nullptr)
        return std::string();
    std::vector<std::string> names;
    while (const dirent* entry = ::readdir(directory))
    {
        if (std::strchr(entry->d_name, ':') != nullptr)
            names.push_back(entry->d_name);
    }
    ::closedir(directory);
    std::sort(names.begin(), names.end());
    std::string joined;
    for (const std::string& name : names)
        joined += name + " ";
    return joined;
}

// "bluealsa" is wrapped in plug. With resampling off it offers exactly the
// transport's rate, so the first one is it.
bool configure(snd_pcm_t* pcm, unsigned& rate, snd_pcm_uframes_t& period)
{
    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);
    unsigned periodUs = kPeriodUs;
    unsigned bufferUs = kBufferUs;
    int dir = 0;
    return snd_pcm_hw_params_any(pcm, params) >= 0 &&
           snd_pcm_hw_params_set_rate_resample(pcm, params, 0) >= 0 &&
           snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED) >= 0 &&
           snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE) >= 0 &&
           snd_pcm_hw_params_set_channels(pcm, params, 2) >= 0 &&
           snd_pcm_hw_params_set_rate_first(pcm, params, &rate, &dir) >= 0 &&
           snd_pcm_hw_params_set_buffer_time_near(pcm, params, &bufferUs, &dir) >= 0 &&
           snd_pcm_hw_params_set_period_time_near(pcm, params, &periodUs, &dir) >= 0 &&
           snd_pcm_hw_params(pcm, params) >= 0 &&
           snd_pcm_hw_params_get_period_size(params, &period, &dir) >= 0 && period > 0;
}

} // namespace

BluealsaReceiver::~BluealsaReceiver()
{
    stop();
}

bool BluealsaReceiver::start(std::string& error)
{
    (void)error;
    if (thread_.joinable())
        return true;
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
    return true;
}

void BluealsaReceiver::stop()
{
    stop_.store(true);
    if (thread_.joinable())
        thread_.join();
}

uint32_t BluealsaReceiver::receivedFrames() const
{
    return received_.load(std::memory_order_relaxed);
}

AudioFormat BluealsaReceiver::format() const
{
    AudioFormat format;
    format.sampleRate = sampleRate_.load(std::memory_order_relaxed);
    format.channels = format.sampleRate != 0 ? 2 : 0;
    format.sampleType = AudioFormat::SampleType::Int16;
    return format;
}

void BluealsaReceiver::setSink(Sink sink)
{
    std::lock_guard<std::mutex> lock(sinkMutex_);
    sink_ = std::move(sink);
}

void BluealsaReceiver::setName(const std::string& name)
{
    std::lock_guard<std::mutex> lock(nameMutex_);
    name_ = name;
}

// The image's helper sets BlueZ's adapter alias. Retried every few seconds,
// because bluetoothd comes up after the client.
void BluealsaReceiver::applyName()
{
    std::string wanted;
    {
        std::lock_guard<std::mutex> lock(nameMutex_);
        wanted = name_;
    }
    const auto now = std::chrono::steady_clock::now();
    if (wanted.empty() || wanted == appliedName_ || now < nextNameAttempt_)
        return;
    nextNameAttempt_ = now + std::chrono::seconds(5);
    char* const arguments[] = {const_cast<char*>(kHelper), const_cast<char*>("alias"),
                               const_cast<char*>(wanted.c_str()), nullptr};
    pid_t child = 0;
    int status = 0;
    if (::posix_spawn(&child, kHelper, nullptr, nullptr, arguments, environ) == 0 &&
        ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0)
    {
        appliedName_ = wanted;
        MULTIROOM_INFO("bluetooth: name \"%s\"", wanted.c_str());
    }
}

void BluealsaReceiver::run()
{
    const auto pause = [this](int seconds) {
        for (int i = 0; i < seconds * 10 && !stop_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };
    std::vector<int16_t> buffer;
    // The links an open last failed on, how often, and when to try again: a
    // phone's stream appears seconds after its link; headphones never have one.
    std::string failedLinks;
    int failures = 0;
    auto retryAt = std::chrono::steady_clock::now();
    while (!stop_.load())
    {
        applyName();
        const std::string present = links();
        if (present.empty() ||
            (present == failedLinks && std::chrono::steady_clock::now() < retryAt))
        {
            pause(1);
            continue;
        }
        snd_pcm_t* pcm = nullptr;
        int result = snd_pcm_open(&pcm, kPcmName, SND_PCM_STREAM_CAPTURE, 0);
        if (result < 0)
        {
            // Not (yet) a phone stream: retried, not reported.
            failures = present == failedLinks ? failures + 1 : 1;
            failedLinks = present;
            retryAt = std::chrono::steady_clock::now() +
                      std::chrono::seconds(failures < 6 ? 2 : 60);
            continue;
        }
        failedLinks.clear();
        unsigned rate = 0;
        snd_pcm_uframes_t period = 0;
        // A capture PCM starts on its first read, but the loop below waits for
        // data before reading: start it explicitly.
        if (!configure(pcm, rate, period) || snd_pcm_start(pcm) < 0)
        {
            MULTIROOM_WARN("bluetooth: the A2DP stream could not be configured");
            snd_pcm_close(pcm);
            pause(2);
            continue;
        }
        MULTIROOM_INFO("bluetooth: A2DP receiver ready at %u Hz", rate);
        sampleRate_.store(rate, std::memory_order_relaxed);
        buffer.resize(static_cast<size_t>(period) * 2);

        while (!stop_.load())
        {
            // A source that pauses sends nothing; the wait times out, and the
            // owner sees receivedFrames() stand still.
            result = snd_pcm_wait(pcm, 200);
            if (result == 0)
            {
                applyName();
                if (links().empty())
                    break;
                continue;
            }
            snd_pcm_sframes_t frames = result < 0 ? result : snd_pcm_readi(pcm, buffer.data(), period);
            if (frames == -EAGAIN)
                continue;
            if (frames < 0)
            {
                // A disconnect or codec change ends this PCM; reopen.
                if (snd_pcm_recover(pcm, static_cast<int>(frames), 1) < 0 || snd_pcm_start(pcm) < 0)
                    break;
                continue;
            }
            received_.fetch_add(static_cast<uint32_t>(frames), std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(sinkMutex_);
            if (sink_)
                sink_(buffer.data(), static_cast<uint32_t>(frames));
        }
        snd_pcm_close(pcm);
        sampleRate_.store(0, std::memory_order_relaxed);
        MULTIROOM_INFO("bluetooth: A2DP receiver closed");
    }
}

} // namespace multiroom
