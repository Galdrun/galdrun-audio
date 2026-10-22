#pragma once

// One decode per zone, many readers at slightly different speeds, bounded
// memory.
//
// A zone decodes its source once, into blocks it publishes here. Every client
// output of that zone then reads the same blocks at its own pace -- a room
// half a second ahead of another is two cursors over one ring, not two
// decodes. What the store holds is what the decoder produced: the source's own
// frames, in the source's own format. Where those frames sit on the zone's
// timeline is the business of timeline/SourceTimeline, which lives beside this
// and is not consulted here, because decoded audio cannot be re-anchored while
// its mapping can.
//
// **Publication is the memory barrier.** A block is filled through the
// BlockPool::Handle the pipeline thread owns, and publish() takes that handle
// by value: the writer gives up its only means of touching the block in the
// same call that makes it visible. From then on it is shared, const, and
// readable by any number of threads without further synchronization. That is
// the whole story for the audio data itself; the mutex below guards the ring
// and the cursors, never a sample.
//
// **A slow reader is evicted, never waited for.** The writer does not block on
// a reader -- the specification's central rule, seen from the store's side. A
// reader whose cursor falls behind the oldest retained block is told Evicted
// on its next read; that is the trigger for a controlled rejoin (phase 16) and
// never a reason to grow the store. The store does not move a cursor on the
// reader's behalf, so an evicted reader keeps reporting Evicted until its
// owner decides where to resume.
//
// **Retention is a duration, and it is not the prebuffer.** A client is sent
// audio ahead of time and holds it in its own buffer; the store only has to
// cover the spread between the fastest and the slowest reader plus the
// pipeline's own lookahead. Five seconds is the starting value, and 5 s of
// 96 kHz 24-bit stereo is 2.9 MB per zone -- the ceiling this component exists
// to make explicit. The block ceiling beside it is the backstop: since phase
// 42 it is the zone's own pool less the block being written and one being
// read, so the zone recycles its own blocks and never asks for more than it
// owns.

#include "audio/BlockPool.h"
#include "core/Error.h"
#include "core/Units.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace multiroom {

enum class PcmStoreError
{
    InvalidRetention, // zero retention, or a block ceiling of zero
    InvalidBlock,     // not committed, no frames, or a duration 64 bits cannot hold
    OutOfOrder,       // a block that does not begin where the last one ended
    UnknownReader,    // a reader id that was never added, or was removed

    // The cursor fell behind the oldest retained block. The audio it names has
    // been dropped, and reading on would be stale samples in the right place,
    // which is worse than a failure.
    Evicted,

    // The cursor has caught up with the writer. Not a fault: the reader is
    // simply faster than the decoder for the moment.
    AtFrontier
};

const char* errorName(PcmStoreError error);

class NativePcmStore
{
public:
    using SharedBlock = BlockPool::SharedBlock;
    using ReaderId = uint64_t;

    // What a reader gets: the block its cursor sits in, and how far into that
    // block the cursor was. A cursor lands mid-block after a seek, and a
    // reader that was handed the whole block would play audio it has already
    // played.
    struct Read
    {
        SharedBlock block;
        uint32_t frameOffset = 0;
    };

    // MATP section 46's native PCM store retention, as the roadmap's starting
    // value. It is a starting value and not a measurement.
    static constexpr uint64_t kDefaultRetentionNs = 5ULL * kNanosecondsPerSecond;

    // 512 blocks is 10 s at the default 20 ms packet, so retention decides in
    // every normal configuration and this only bites on a pathological stream
    // of very short blocks.
    static constexpr size_t kDefaultMaxBlocks = 512;

    static Result<std::unique_ptr<NativePcmStore>, PcmStoreError>
    create(uint64_t retentionNs = kDefaultRetentionNs, size_t maxBlocks = kDefaultMaxBlocks);

    NativePcmStore(const NativePcmStore&) = delete;
    NativePcmStore& operator=(const NativePcmStore&) = delete;

    // --- writer side: the zone's pipeline thread ----------------------------

    // Publishes a committed block and evicts whatever retention no longer
    // covers. The block must begin exactly where the previous one ended: the
    // ring is a contiguous run of source frames, and a gap in it would be
    // audible as a position that maps to the wrong music.
    Status<PcmStoreError> publish(BlockPool::Handle block);

    // The frame the next published block must begin at. The first block may
    // begin anywhere; after that this is the only value publish() accepts.
    SourceFrame nextFrame() const;

    // The oldest frame still retained. Equal to nextFrame() when the store is
    // empty.
    SourceFrame oldestFrame() const;

    // Phase 42: the block ceiling follows the zone's pool. Applied from the
    // next publish().
    void setMaxBlocks(size_t maxBlocks);

    // Phase 42: drops every block, for a zone that stops or has played out
    // and gives its memory back. The run of frames carries on: the next block
    // must still begin at nextFrame(), and a cursor behind it reads as
    // Evicted once there is audio again.
    void clear();

    // --- reader side: one client output thread each -------------------------

    // Cursors are independent and a reader is never implied by a read: a room
    // that attaches, drops and comes back is the same room with a new cursor,
    // and the zone decides where it resumes.
    ReaderId addReader(SourceFrame position);
    Status<PcmStoreError> removeReader(ReaderId reader);

    // Moves a cursor. A position ahead of the frontier is allowed and reads as
    // AtFrontier until the writer gets there; one behind the oldest retained
    // block reads as Evicted.
    Status<PcmStoreError> seek(ReaderId reader, SourceFrame position);
    [[nodiscard]] Result<SourceFrame, PcmStoreError> cursor(ReaderId reader) const;

    // Hands over the block the cursor sits in and advances the cursor past it.
    [[nodiscard]] Result<Read, PcmStoreError> read(ReaderId reader);

    // --- what the status API reports ----------------------------------------

    size_t retainedBlocks() const;
    uint64_t retainedNs() const;
    size_t readerCount() const;

    // Blocks dropped by retention. A reader that was told Evicted can be
    // explained by this rising, rather than by guesswork.
    uint64_t evictedCount() const;

private:
    NativePcmStore(uint64_t retentionNs, size_t maxBlocks) noexcept;

    struct Entry
    {
        SharedBlock block;
        uint64_t durationNs = 0; // this block's own, so a rate change costs nothing
    };

    mutable std::mutex mutex_;
    std::deque<Entry> blocks_;                              // guarded by mutex_
    std::unordered_map<ReaderId, SourceFrame> readers_;     // guarded by mutex_
    ReaderId nextReaderId_ = 1;                             // guarded by mutex_
    SourceFrame nextFrame_{};                               // guarded by mutex_
    SourceFrame oldestFrame_{};                             // guarded by mutex_
    uint64_t retainedNs_ = 0;                               // guarded by mutex_
    uint64_t evicted_ = 0;                                  // guarded by mutex_
    bool published_ = false;                                // guarded by mutex_
    uint64_t retentionNs_ = kDefaultRetentionNs;
    size_t maxBlocks_ = kDefaultMaxBlocks;                  // guarded by mutex_ (phase 42)
};

} // namespace multiroom
