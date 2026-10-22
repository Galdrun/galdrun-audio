#include "audio/RingBufferSource.h"

#include "util/Logger.h"

namespace multiroom {

RingBufferSource::RingBufferSource(AudioRingBuffer& buffer, uint32_t capacityMilliseconds)
    : buffer_(buffer), capacityMilliseconds_(capacityMilliseconds), rendered_(0), underrun_(0)
{
}

void RingBufferSource::prepare(const AudioFormat& format)
{
    rendered_.store(0, std::memory_order_relaxed);
    underrun_.store(0, std::memory_order_relaxed);

    if (!format.isValid())
    {
        MULTIROOM_ERROR("ring buffer source prepared with an invalid format");
        return;
    }

    // Rounded up, so a capacity in milliseconds never turns into zero frames.
    const uint64_t rate = static_cast<uint64_t>(format.sampleRate);
    const uint64_t frames = (rate * capacityMilliseconds_ + 999ULL) / 1000ULL;

    std::string error;
    if (!buffer_.configure(format.bytesPerFrame(), static_cast<uint32_t>(frames), error))
    {
        // render() then returns nothing and the device plays silence, which is
        // the honest outcome: there is no buffer to read from.
        MULTIROOM_ERROR("cannot size the audio ring buffer: %s", error.c_str());
        return;
    }

    MULTIROOM_INFO("audio ring buffer: %lu frames (%lu ms of %s)",
             static_cast<unsigned long>(buffer_.capacityFrames()),
             static_cast<unsigned long>(capacityMilliseconds_),
             describeFormat(format).c_str());
}

uint32_t RingBufferSource::render(void* buffer, uint32_t frameCount)
{
    const uint32_t filled = buffer_.pop(buffer, frameCount);

    rendered_.fetch_add(filled, std::memory_order_relaxed);
    if (filled < frameCount)
        underrun_.fetch_add(frameCount - filled, std::memory_order_relaxed);

    return filled;
}

uint32_t RingBufferSource::renderedFrames() const
{
    return rendered_.load(std::memory_order_relaxed);
}

uint32_t RingBufferSource::underrunFrames() const
{
    return underrun_.load(std::memory_order_relaxed);
}

} // namespace multiroom
