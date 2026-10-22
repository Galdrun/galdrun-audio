#include "audio/NativePcmStore.h"

#include <algorithm>
#include <limits>
#include <string>

namespace multiroom {
namespace {

std::string frameText(uint64_t frame)
{
    return "frame " + std::to_string(frame);
}

} // namespace

const char* errorName(PcmStoreError error)
{
    switch (error)
    {
    case PcmStoreError::InvalidRetention:
        return "InvalidRetention";
    case PcmStoreError::InvalidBlock:
        return "InvalidBlock";
    case PcmStoreError::OutOfOrder:
        return "OutOfOrder";
    case PcmStoreError::UnknownReader:
        return "UnknownReader";
    case PcmStoreError::Evicted:
        return "Evicted";
    case PcmStoreError::AtFrontier:
        return "AtFrontier";
    }
    return "?";
}

Result<std::unique_ptr<NativePcmStore>, PcmStoreError> NativePcmStore::create(uint64_t retentionNs,
                                                                              size_t maxBlocks)
{
    if (retentionNs == 0 || maxBlocks == 0)
    {
        return fail(PcmStoreError::InvalidRetention, std::to_string(retentionNs) + " ns over " +
                                                         std::to_string(maxBlocks) + " blocks");
    }
    // Private constructor, so make_unique cannot be used here.
    return std::unique_ptr<NativePcmStore>(new NativePcmStore(retentionNs, maxBlocks));
}

NativePcmStore::NativePcmStore(uint64_t retentionNs, size_t maxBlocks) noexcept
    : retentionNs_(retentionNs), maxBlocks_(maxBlocks)
{
}

Status<PcmStoreError> NativePcmStore::publish(BlockPool::Handle block)
{
    if (!block)
        return fail(PcmStoreError::InvalidBlock, "no block");
    if (!block->committed())
        return fail(PcmStoreError::InvalidBlock, "the producer never committed it");
    if (block->frameCount() == 0)
        return fail(PcmStoreError::InvalidBlock, "committed with no frames");

    // A committed block carries a validated format, so neither failure this
    // reports can happen here -- but the duration is what retention is counted
    // in, and a wrong one would silently change the ceiling.
    const auto duration = nanosecondsFromFrames(block->frameCount(), block->format().sampleRate);
    if (!duration)
        return fail(PcmStoreError::InvalidBlock, describe(duration.error()));

    const uint64_t first = std::to_underlying(block->firstFrame());
    const uint64_t count = block->frameCount();
    if (count > std::numeric_limits<uint64_t>::max() - first)
        return fail(PcmStoreError::InvalidBlock, frameText(first) + " would wrap the counter");

    const std::lock_guard<std::mutex> lock(mutex_);

    // The ring is one contiguous run of source frames. A gap in it would be a
    // position that maps to the wrong music, which is inaudible as a defect
    // and obvious as a symptom.
    if (published_ && first != std::to_underlying(nextFrame_))
    {
        return fail(PcmStoreError::OutOfOrder,
                    frameText(first) + " does not continue " +
                        frameText(std::to_underlying(nextFrame_)));
    }

    blocks_.push_back(Entry{SharedBlock(std::move(block)), *duration});
    retainedNs_ += *duration;
    nextFrame_ = SourceFrame{first + count};
    if (!published_)
    {
        oldestFrame_ = SourceFrame{first};
        published_ = true;
    }

    // Never the block just published: a retention shorter than one block still
    // has to leave the writer's own audio readable.
    while (blocks_.size() > 1 && (blocks_.size() > maxBlocks_ || retainedNs_ > retentionNs_))
    {
        retainedNs_ -= blocks_.front().durationNs;
        blocks_.pop_front();
        ++evicted_;
    }
    oldestFrame_ = blocks_.front().block->firstFrame();
    return {};
}

SourceFrame NativePcmStore::nextFrame() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return nextFrame_;
}

SourceFrame NativePcmStore::oldestFrame() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return oldestFrame_;
}

void NativePcmStore::setMaxBlocks(size_t maxBlocks)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    maxBlocks_ = std::max<size_t>(maxBlocks, 1);
}

void NativePcmStore::clear()
{
    std::deque<Entry> dropped;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        dropped.swap(blocks_);
        evicted_ += dropped.size();
        retainedNs_ = 0;
        oldestFrame_ = nextFrame_;
    }
    // Outside the lock: the last reference returns a block to its pool, which
    // takes the pool's own.
}

NativePcmStore::ReaderId NativePcmStore::addReader(SourceFrame position)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const ReaderId id = nextReaderId_++;
    readers_.emplace(id, position);
    return id;
}

Status<PcmStoreError> NativePcmStore::removeReader(ReaderId reader)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (readers_.erase(reader) == 0)
        return fail(PcmStoreError::UnknownReader, std::to_string(reader));
    return {};
}

Status<PcmStoreError> NativePcmStore::seek(ReaderId reader, SourceFrame position)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = readers_.find(reader);
    if (it == readers_.end())
        return fail(PcmStoreError::UnknownReader, std::to_string(reader));
    it->second = position;
    return {};
}

Result<SourceFrame, PcmStoreError> NativePcmStore::cursor(ReaderId reader) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = readers_.find(reader);
    if (it == readers_.end())
        return fail(PcmStoreError::UnknownReader, std::to_string(reader));
    return it->second;
}

Result<NativePcmStore::Read, PcmStoreError> NativePcmStore::read(ReaderId reader)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = readers_.find(reader);
    if (it == readers_.end())
        return fail(PcmStoreError::UnknownReader, std::to_string(reader));

    const uint64_t cursor = std::to_underlying(it->second);
    const uint64_t frontier = std::to_underlying(nextFrame_);
    if (blocks_.empty() || cursor >= frontier)
    {
        return fail(PcmStoreError::AtFrontier,
                    frameText(cursor) + " of " + std::to_string(frontier) + " written");
    }

    const uint64_t oldest = std::to_underlying(oldestFrame_);
    if (cursor < oldest)
    {
        return fail(PcmStoreError::Evicted,
                    frameText(cursor) + " is behind " + frameText(oldest));
    }

    // The run is contiguous and the cursor is inside it, so the block before
    // the first one that starts after the cursor is the block it sits in.
    const auto after = std::upper_bound(blocks_.begin(), blocks_.end(), cursor,
                                        [](uint64_t value, const Entry& entry)
                                        { return value < std::to_underlying(entry.block->firstFrame()); });
    const Entry& entry = *std::prev(after);
    const uint64_t first = std::to_underlying(entry.block->firstFrame());

    it->second = SourceFrame{first + entry.block->frameCount()};
    return Read{entry.block, static_cast<uint32_t>(cursor - first)};
}

size_t NativePcmStore::retainedBlocks() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return blocks_.size();
}

uint64_t NativePcmStore::retainedNs() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return retainedNs_;
}

size_t NativePcmStore::readerCount() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return readers_.size();
}

uint64_t NativePcmStore::evictedCount() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return evicted_;
}

} // namespace multiroom
