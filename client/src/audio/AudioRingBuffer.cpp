#include "audio/AudioRingBuffer.h"

#include <cstring>
#include <utility>

namespace multiroom {
namespace {

// Two frames is the smallest thing that can be full and empty at once; the
// upper bound is there so a bad capacity cannot ask for a gigabyte.
const uint32_t kMinCapacityFrames = 2;
const uint32_t kMaxCapacityFrames = 1u << 22; // 87 s at 48 kHz

// Eight channels of 8-byte samples, well beyond anything the client opens.
const uint32_t kMaxBytesPerFrame = 64;

// The target board has 512 MB in total, so a buffer this large is a bug in the
// caller rather than a configuration.
const size_t kMaxStorageBytes = 64u * 1024u * 1024u;

uint32_t roundUpToPowerOfTwo(uint32_t value)
{
    uint32_t result = 1;
    while (result < value)
        result <<= 1;
    return result;
}

} // namespace

AudioRingBuffer::AudioRingBuffer() : writeIndex_(0), readIndex_(0) {}

bool AudioRingBuffer::configure(uint32_t bytesPerFrame, uint32_t capacityFrames,
                                std::string& error)
{
    // A refusal leaves the buffer unconfigured rather than as it was: callers
    // read isConfigured() as "sized for the format just asked for", and the
    // old buffer has the old frame size -- played into a device opened for the
    // new one, that is the wrong number of bytes per frame.
    const auto refuse = [this, &error](std::string reason) {
        std::vector<unsigned char>().swap(storage_);
        capacityFrames_ = 0;
        mask_ = 0;
        bytesPerFrame_ = 0;
        writeIndex_.store(0, std::memory_order_relaxed);
        readIndex_.store(0, std::memory_order_relaxed);
        error = std::move(reason);
        return false;
    };

    if (bytesPerFrame == 0 || bytesPerFrame > kMaxBytesPerFrame)
        return refuse("unusable frame size " + std::to_string(bytesPerFrame) + " byte(s)");
    if (capacityFrames < kMinCapacityFrames || capacityFrames > kMaxCapacityFrames)
        return refuse("unusable ring buffer capacity " + std::to_string(capacityFrames) +
                      " frame(s)");

    const uint32_t rounded = roundUpToPowerOfTwo(capacityFrames);
    const size_t bytes = static_cast<size_t>(rounded) * static_cast<size_t>(bytesPerFrame);
    if (bytes > kMaxStorageBytes)
        return refuse("ring buffer of " + std::to_string(rounded) + " frames x " +
                      std::to_string(bytesPerFrame) + " bytes is too large");

    // Released before the new block is taken: assign() would hold both at
    // once while it grows, and on the ESP32 the two together may not fit
    // where either one alone does.
    if (bytes > storage_.capacity())
        std::vector<unsigned char>().swap(storage_);
    storage_.assign(bytes, 0);
    capacityFrames_ = rounded;
    mask_ = rounded - 1;
    bytesPerFrame_ = bytesPerFrame;
    writeIndex_.store(0, std::memory_order_relaxed);
    readIndex_.store(0, std::memory_order_relaxed);
    return true;
}

uint64_t AudioRingBuffer::storageBytes(uint32_t bytesPerFrame, uint32_t capacityFrames)
{
    if (bytesPerFrame == 0 || bytesPerFrame > kMaxBytesPerFrame ||
        capacityFrames < kMinCapacityFrames || capacityFrames > kMaxCapacityFrames)
        return 0;
    const uint64_t bytes =
        static_cast<uint64_t>(roundUpToPowerOfTwo(capacityFrames)) * bytesPerFrame;
    return bytes > kMaxStorageBytes ? 0 : bytes;
}

uint32_t AudioRingBuffer::writableFrames() const
{
    const uint32_t used =
        writeIndex_.load(std::memory_order_acquire) - readIndex_.load(std::memory_order_acquire);
    return capacityFrames_ - used;
}

uint32_t AudioRingBuffer::readableFrames() const
{
    return writeIndex_.load(std::memory_order_acquire) - readIndex_.load(std::memory_order_acquire);
}

uint32_t AudioRingBuffer::push(const void* frames, uint32_t frameCount)
{
    if (capacityFrames_ == 0 || frames == nullptr || frameCount == 0)
        return 0;

    // The producer owns writeIndex_, so it can read its own with relaxed
    // ordering; the acquire on readIndex_ pairs with the consumer's release
    // and is what makes the freed space visible.
    const uint32_t write = writeIndex_.load(std::memory_order_relaxed);
    const uint32_t free = capacityFrames_ - (write - readIndex_.load(std::memory_order_acquire));
    const uint32_t count = frameCount < free ? frameCount : free;
    if (count == 0)
        return 0;

    // Up to two copies: the second one only when the block crosses the end of
    // the storage.
    const uint32_t offset = write & mask_;
    const uint32_t firstFrames = count < capacityFrames_ - offset ? count : capacityFrames_ - offset;
    const unsigned char* source = static_cast<const unsigned char*>(frames);

    std::memcpy(storage_.data() + static_cast<size_t>(offset) * bytesPerFrame_, source,
                static_cast<size_t>(firstFrames) * bytesPerFrame_);
    if (count > firstFrames)
    {
        std::memcpy(storage_.data(), source + static_cast<size_t>(firstFrames) * bytesPerFrame_,
                    static_cast<size_t>(count - firstFrames) * bytesPerFrame_);
    }

    // Release: the frames above must be visible before the consumer can see
    // the index that exposes them.
    writeIndex_.store(write + count, std::memory_order_release);
    return count;
}

uint32_t AudioRingBuffer::pop(void* frames, uint32_t frameCount)
{
    if (capacityFrames_ == 0 || frames == nullptr || frameCount == 0)
        return 0;

    const uint32_t read = readIndex_.load(std::memory_order_relaxed);
    const uint32_t used = writeIndex_.load(std::memory_order_acquire) - read;
    const uint32_t count = frameCount < used ? frameCount : used;
    if (count == 0)
        return 0;

    const uint32_t offset = read & mask_;
    const uint32_t firstFrames = count < capacityFrames_ - offset ? count : capacityFrames_ - offset;
    unsigned char* target = static_cast<unsigned char*>(frames);

    std::memcpy(target, storage_.data() + static_cast<size_t>(offset) * bytesPerFrame_,
                static_cast<size_t>(firstFrames) * bytesPerFrame_);
    if (count > firstFrames)
    {
        std::memcpy(target + static_cast<size_t>(firstFrames) * bytesPerFrame_, storage_.data(),
                    static_cast<size_t>(count - firstFrames) * bytesPerFrame_);
    }

    readIndex_.store(read + count, std::memory_order_release);
    return count;
}

uint32_t AudioRingBuffer::discard(uint32_t frameCount)
{
    if (capacityFrames_ == 0 || frameCount == 0)
        return 0;

    const uint32_t read = readIndex_.load(std::memory_order_relaxed);
    const uint32_t used = writeIndex_.load(std::memory_order_acquire) - read;
    const uint32_t count = frameCount < used ? frameCount : used;
    if (count == 0)
        return 0;

    // No copy at all: the frames are simply not the consumer's any more,
    // which is the whole operation.
    readIndex_.store(read + count, std::memory_order_release);
    return count;
}

} // namespace multiroom
