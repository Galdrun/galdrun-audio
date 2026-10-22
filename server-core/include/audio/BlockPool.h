#pragma once

// Equally sized audio buffers, handed out and returned.
//
// Decoding 96 kHz/24-bit stereo is about 550 kB/s per zone, and a fresh
// allocation per 20 ms block is fifty allocations a second of near-identical
// size. The pool removes that: blocks are allocated when a zone starts or
// crosses into a format that needs more of them, and recycled in between, so
// nothing is allocated on the audio path.
//
// Since phase 42 **each zone owns its own pool**, sized by what it is playing
// and held only while it plays. One pool shared by every zone let two zones
// starve a third, and let a 192 kHz zone deadlock on its own: the whole shared
// pool held less than its store's retention, so the store never evicted and
// the pool never got a block back. What the core still has one of is the
// ceiling, AudioMemory below -- a counter of the bytes every zone's pool has
// allocated, and not a pool: nothing is set aside up front.
//
// Exhaustion is an explicit, counted failure. A pool never allocates past the
// size it was given to satisfy a request, because that is precisely the
// behaviour that turns a slow consumer into unbounded memory growth.
//
// Threading: blocks are acquired on a zone's pipeline thread and released
// wherever the last client output drops its reference, so the free list is
// under a mutex.

#include "audio/AudioBlock.h"
#include "audio/AudioFormat.h"
#include "core/Error.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace multiroom {

enum class BlockPoolError
{
    InvalidSize, // zero bytes per block or zero blocks
    TooLarge,    // more memory than the pool is willing to reserve
    Exhausted,   // every block is out; the consumer is behind
    OverLimit    // growing would take the core past audio.memoryLimitMb (phase 42)
};

const char* errorName(BlockPoolError error);

// The core's one ceiling on audio memory (phase 42, `audio.memoryLimitMb`): the
// bytes every zone's pool has allocated, against a limit. A counter rather
// than a pool, so a zone that is not playing costs nothing. One block size for
// the whole core -- one packet of the largest format a deployment allows -- so
// a block holds any format and nothing is ever resized.
//
// Thread-safe: zones grow and shrink on their own pipeline threads, and the
// control API reads and changes the limit on its own.
class AudioMemory
{
public:
    AudioMemory(size_t blockBytes, uint64_t limitBytes) noexcept
        : blockBytes_(blockBytes), limitBytes_(limitBytes)
    {
    }

    AudioMemory(const AudioMemory&) = delete;
    AudioMemory& operator=(const AudioMemory&) = delete;

    [[nodiscard]] size_t blockBytes() const noexcept { return blockBytes_; }

    // A new limit applies to the next growth; what is already held is never
    // taken back.
    void setLimitBytes(uint64_t limitBytes);
    [[nodiscard]] uint64_t limitBytes() const;
    [[nodiscard]] uint64_t usedBytes() const;

    // Counts `bytes` against the limit, or counts nothing and answers false
    // when they would go past it.
    [[nodiscard]] bool take(uint64_t bytes);
    void give(uint64_t bytes);

private:
    const size_t blockBytes_;
    mutable std::mutex mutex_;
    uint64_t limitBytes_ = 0; // guarded by mutex_
    uint64_t usedBytes_ = 0;  // guarded by mutex_
};

class BlockPool
{
public:
    // Returns a block to its pool. Held by the smart pointers below, which is
    // why it is a type and not a lambda.
    class Recycler
    {
    public:
        Recycler() = default;
        explicit Recycler(BlockPool* pool) noexcept : pool_(pool) {}
        void operator()(AudioBlock* block) const noexcept;

    private:
        BlockPool* pool_ = nullptr;
    };

    // What a producer writes into. Converts to SharedBlock on publication.
    using Handle = std::unique_ptr<AudioBlock, Recycler>;

    // What every client output of a zone reads from. Const, so the block is
    // immutable from the moment it is shared.
    using SharedBlock = std::shared_ptr<const AudioBlock>;

    // Refuses a total above this rather than reserving it: the backstop
    // against a typo turning into an out-of-memory kill.
    static constexpr size_t kMaxTotalBytes = 1024ULL * 1024ULL * 1024ULL;

    // A pool of exactly `blockCount` blocks, counted against nothing.
    static Result<std::unique_ptr<BlockPool>, BlockPoolError> create(size_t blockBytes,
                                                                     size_t blockCount);

    // One zone's pool (phase 42): empty, with blocks of `memory`'s size, grown
    // and shrunk by resize() and counted against `memory`, which outlives it.
    static std::unique_ptr<BlockPool> create(AudioMemory& memory);

    // Every block is back by now: the pool outlives whatever reads from it.
    ~BlockPool();

    BlockPool(const BlockPool&) = delete;
    BlockPool& operator=(const BlockPool&) = delete;

    // The number of blocks this pool is to hold. Growing allocates the
    // difference at once, so nothing is allocated later on the audio path, and
    // is refused as OverLimit -- with nothing allocated -- where the memory's
    // limit would not hold it. Shrinking frees what is free at once and the
    // rest as it comes back. Zero frees everything.
    [[nodiscard]] Status<BlockPoolError> resize(size_t blockCount);

    [[nodiscard]] Result<Handle, BlockPoolError> acquire();

    size_t blockBytes() const noexcept { return blockBytes_; }

    // What is allocated now, which runs ahead of a shrink until the blocks
    // still out have come back.
    size_t blockCount() const;
    size_t totalBytes() const;

    size_t available() const;
    size_t inUse() const;

    // How often acquire() has been refused. The zone that sees this rise is
    // decoding faster than its slowest reader drains, and the number is what
    // the status API reports instead of a guess.
    uint64_t exhaustedCount() const;

private:
    BlockPool(size_t blockBytes, AudioMemory* memory) noexcept;

    void release(AudioBlock* block) noexcept;

    // Under mutex_. The block's storage and the block, freed together.
    void freeLocked(AudioBlock* block) noexcept;

    struct Slot
    {
        std::unique_ptr<std::byte[]> storage;
        std::unique_ptr<AudioBlock> block;
    };

    const size_t blockBytes_;
    AudioMemory* const memory_;

    mutable std::mutex mutex_;
    std::vector<Slot> slots_;       // guarded by mutex_; every block allocated
    std::vector<AudioBlock*> free_; // guarded by mutex_
    size_t target_ = 0;             // guarded by mutex_; what resize() last asked for
    uint64_t exhausted_ = 0;        // guarded by mutex_
};

// Bytes one packet of audio occupies in the largest format a deployment
// allows. This is the one place that says what "largest" means, so the pool's
// size and the config's ceiling cannot drift apart.
size_t blockBytesFor(uint32_t maxSampleRate, uint16_t maxChannels, uint32_t packetDurationMs);

// What one zone needs to hold `retentionNs` of `format` in blocks of
// `blockBytes` (phase 42): the retention in whole blocks, plus the block being
// written and one being read. Zero for a format no block holds a frame of.
size_t blocksFor(size_t blockBytes, const AudioFormat& format, uint64_t retentionNs);

} // namespace multiroom
