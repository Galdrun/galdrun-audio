#include "audio/BlockPool.h"

#include "core/Units.h"

#include <algorithm>
#include <string>

namespace multiroom {

const char* errorName(BlockPoolError error)
{
    switch (error)
    {
    case BlockPoolError::InvalidSize:
        return "InvalidSize";
    case BlockPoolError::TooLarge:
        return "TooLarge";
    case BlockPoolError::Exhausted:
        return "Exhausted";
    case BlockPoolError::OverLimit:
        return "OverLimit";
    }
    return "?";
}

// ---------------------------------------------------------------------------

void AudioMemory::setLimitBytes(uint64_t limitBytes)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    limitBytes_ = limitBytes;
}

uint64_t AudioMemory::limitBytes() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return limitBytes_;
}

uint64_t AudioMemory::usedBytes() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return usedBytes_;
}

bool AudioMemory::take(uint64_t bytes)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (usedBytes_ > limitBytes_ || bytes > limitBytes_ - usedBytes_)
        return false;
    usedBytes_ += bytes;
    return true;
}

void AudioMemory::give(uint64_t bytes)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    usedBytes_ -= std::min(bytes, usedBytes_);
}

// ---------------------------------------------------------------------------

void BlockPool::Recycler::operator()(AudioBlock* block) const noexcept
{
    if (pool_ != nullptr && block != nullptr)
        pool_->release(block);
}

Result<std::unique_ptr<BlockPool>, BlockPoolError> BlockPool::create(size_t blockBytes,
                                                                    size_t blockCount)
{
    if (blockBytes == 0 || blockCount == 0)
    {
        return fail(BlockPoolError::InvalidSize, std::to_string(blockCount) + " blocks of " +
                                                     std::to_string(blockBytes) + " bytes");
    }
    if (blockBytes > kMaxTotalBytes / blockCount)
    {
        return fail(BlockPoolError::TooLarge, std::to_string(blockCount) + " x " +
                                                  std::to_string(blockBytes) +
                                                  " bytes exceeds the pool ceiling");
    }

    // Private constructor, so make_unique cannot be used here.
    std::unique_ptr<BlockPool> pool(new BlockPool(blockBytes, nullptr));
    if (auto sized = pool->resize(blockCount); !sized)
        return std::unexpected(sized.error());
    return pool;
}

std::unique_ptr<BlockPool> BlockPool::create(AudioMemory& memory)
{
    return std::unique_ptr<BlockPool>(new BlockPool(memory.blockBytes(), &memory));
}

BlockPool::BlockPool(size_t blockBytes, AudioMemory* memory) noexcept
    : blockBytes_(blockBytes), memory_(memory)
{
}

BlockPool::~BlockPool()
{
    if (memory_ != nullptr)
        memory_->give(static_cast<uint64_t>(slots_.size()) * blockBytes_);
}

Status<BlockPoolError> BlockPool::resize(size_t blockCount)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    target_ = blockCount;

    // Shrinking: what is free goes now, what is out goes as it comes back.
    if (slots_.size() >= blockCount)
    {
        while (slots_.size() > target_ && !free_.empty())
        {
            AudioBlock* block = free_.back();
            free_.pop_back();
            freeLocked(block);
        }
        return {};
    }

    const size_t more = blockCount - slots_.size();
    if (blockBytes_ == 0 || blockBytes_ > (kMaxTotalBytes - slots_.size() * blockBytes_) / more)
    {
        return fail(BlockPoolError::TooLarge, std::to_string(blockCount) + " x " +
                                                  std::to_string(blockBytes_) +
                                                  " bytes exceeds the pool ceiling");
    }
    const uint64_t bytes = static_cast<uint64_t>(more) * blockBytes_;
    if (memory_ != nullptr && !memory_->take(bytes))
    {
        return fail(BlockPoolError::OverLimit,
                    std::to_string(more) + " more blocks of " + std::to_string(blockBytes_) +
                        " bytes would pass the limit");
    }

    slots_.reserve(blockCount);
    free_.reserve(blockCount);
    for (size_t i = 0; i < more; ++i)
    {
        Slot slot;
        slot.storage = std::make_unique<std::byte[]>(blockBytes_);
        slot.block.reset(new AudioBlock(std::span<std::byte>(slot.storage.get(), blockBytes_)));
        free_.push_back(slot.block.get());
        slots_.push_back(std::move(slot));
    }
    return {};
}

Result<BlockPool::Handle, BlockPoolError> BlockPool::acquire()
{
    AudioBlock* block = nullptr;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (free_.empty())
        {
            ++exhausted_;
            return fail(BlockPoolError::Exhausted,
                        "all " + std::to_string(slots_.size()) + " blocks are in use");
        }
        block = free_.back();
        free_.pop_back();
    }
    return Handle(block, Recycler(this));
}

void BlockPool::release(AudioBlock* block) noexcept
{
    block->reset();
    const std::lock_guard<std::mutex> lock(mutex_);
    if (slots_.size() > target_)
        freeLocked(block);
    else
        free_.push_back(block);
}

void BlockPool::freeLocked(AudioBlock* block) noexcept
{
    const auto found = std::find_if(slots_.begin(), slots_.end(),
                                    [block](const Slot& slot) { return slot.block.get() == block; });
    if (found == slots_.end())
        return;
    *found = std::move(slots_.back());
    slots_.pop_back();
    if (memory_ != nullptr)
        memory_->give(blockBytes_);
}

size_t BlockPool::blockCount() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return slots_.size();
}

size_t BlockPool::totalBytes() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return slots_.size() * blockBytes_;
}

size_t BlockPool::available() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return free_.size();
}

size_t BlockPool::inUse() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return slots_.size() - free_.size();
}

uint64_t BlockPool::exhaustedCount() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return exhausted_;
}

size_t blockBytesFor(uint32_t maxSampleRate, uint16_t maxChannels, uint32_t packetDurationMs)
{
    AudioFormat largest;
    largest.sampleRate = maxSampleRate;
    largest.channels = maxChannels;
    largest.sampleType = kWidestSampleType;
    if (!largest.isValid() || packetDurationMs == 0)
        return 0;

    // A packet duration is a duration and never a frame count: 20 ms is 882
    // frames at 44.1 kHz, 960 at 48 kHz and 1920 at 96 kHz, and one config
    // value covers every room. Rounding up here is the same rule the timeline
    // uses -- a block that is a frame short would need a second block.
    const auto frames = framesFromNanoseconds(
        static_cast<uint64_t>(packetDurationMs) * 1'000'000ULL, maxSampleRate);
    if (!frames)
        return 0;

    return static_cast<size_t>(*frames + 1) * largest.bytesPerFrame();
}

size_t blocksFor(size_t blockBytes, const AudioFormat& format, uint64_t retentionNs)
{
    if (!format.isValid() || format.bytesPerFrame() == 0)
        return 0;
    const uint64_t perBlock = blockBytes / format.bytesPerFrame();
    if (perBlock == 0)
        return 0;

    const uint64_t frames = framesFromNanoseconds(retentionNs, format.sampleRate).value_or(0);
    return static_cast<size_t>((frames + perBlock - 1) / perBlock) + 2;
}

} // namespace multiroom
