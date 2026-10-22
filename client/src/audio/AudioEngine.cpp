#include "audio/AudioEngine.h"

#include "util/Logger.h"

namespace multiroom {

AudioEngine::AudioEngine(std::unique_ptr<IAudioBackend> backend)
    : backend_(std::move(backend))
{
}

AudioEngine::~AudioEngine()
{
    close();
}

void AudioEngine::setSource(IAudioSource* source)
{
    source_ = source;
}

uint32_t AudioEngine::renderCallback(void* userData, void* buffer, uint32_t frameCount)
{
    AudioEngine* engine = static_cast<AudioEngine*>(userData);
    if (engine == nullptr || engine->source_ == nullptr)
        return 0;
    return engine->source_->render(buffer, frameCount);
}

bool AudioEngine::open(const Settings& settings, std::string& error)
{
    if (!backend_)
    {
        error = "no audio backend";
        return false;
    }
    if (source_ == nullptr)
    {
        error = "no audio source set";
        return false;
    }
    if (!settings.format.isValid())
    {
        error = "invalid audio format";
        return false;
    }

    close();

    AudioBackendConfig config;
    config.deviceId = settings.deviceId;
    config.format = settings.format;
    config.accessMode = settings.accessMode;
    config.requestedBufferFrames = settings.bufferFrames;
    config.renderCallback = &AudioEngine::renderCallback;
    config.userData = this;

    if (!backend_->open(config))
    {
        error = backend_->lastError();
        if (error.empty())
            error = "could not open the audio device";
        return false;
    }

    // The driver may have granted something other than what was asked for, so
    // the source is prepared against what we actually got.
    source_->prepare(backend_->currentFormat());

    MULTIROOM_INFO("audio engine open: %s, %u frames per period, %s mode",
             describeFormat(backend_->currentFormat()).c_str(),
             static_cast<unsigned>(backend_->bufferFrames()),
             audioAccessModeName(settings.accessMode));
    return true;
}

bool AudioEngine::start(std::string& error)
{
    if (!backend_ || backend_->state() == BackendState::Closed)
    {
        error = "audio device is not open";
        return false;
    }
    if (backend_->state() == BackendState::Running)
        return true;

    if (!backend_->start())
    {
        error = backend_->lastError();
        if (error.empty())
            error = "could not start playback";
        return false;
    }

    MULTIROOM_INFO("audio engine started");
    return true;
}

void AudioEngine::stop()
{
    if (!backend_ || backend_->state() != BackendState::Running)
        return;
    backend_->stop();
    MULTIROOM_INFO("audio engine stopped after %llu underrun(s)",
             static_cast<unsigned long long>(backend_->underrunCount()));
}

void AudioEngine::close()
{
    if (!backend_ || backend_->state() == BackendState::Closed)
        return;
    stop();
    backend_->close();
}

bool AudioEngine::isOpen() const
{
    return backend_ && backend_->state() != BackendState::Closed;
}

bool AudioEngine::isRunning() const
{
    return backend_ && backend_->state() == BackendState::Running;
}

AudioFormat AudioEngine::format() const
{
    return backend_ ? backend_->currentFormat() : AudioFormat();
}

uint32_t AudioEngine::bufferFrames() const
{
    return backend_ ? backend_->bufferFrames() : 0;
}

AudioClockSnapshot AudioEngine::clockSnapshot()
{
    return backend_ ? backend_->clockSnapshot() : AudioClockSnapshot();
}

uint64_t AudioEngine::underruns() const
{
    return backend_ ? backend_->underrunCount() : 0;
}

} // namespace multiroom
