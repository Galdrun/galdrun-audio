#include "TestSupport.h"

#include "audio/NativePcmStore.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

using namespace multiroom;

namespace {

constexpr uint32_t kRate = 44100;
constexpr uint32_t kPacketFrames = 882; // 20 ms at 44.1 kHz
constexpr uint64_t kPacketNs = 20000000ULL;

AudioFormat stereo(uint32_t rate = kRate)
{
    AudioFormat format;
    format.sampleRate = rate;
    format.channels = 2;
    format.sampleType = AudioFormat::SampleType::Int16;
    return format;
}

// Every block is filled with a byte of its own, so a reader handed recycled
// storage is caught by its contents and not only by its position.
std::byte fillFor(uint64_t index)
{
    return static_cast<std::byte>(1 + (index % 250));
}

void publishBlock(NativePcmStore& store, BlockPool& pool, uint64_t firstFrame, uint32_t frames,
                  std::byte fill, const AudioFormat& format = stereo())
{
    auto handle = pool.acquire();
    CHECK_OK(handle);
    const std::span<std::byte> writable = (*handle)->writable();
    std::memset(writable.data(), std::to_integer<int>(fill), writable.size());
    CHECK_OK((*handle)->commit(format, SourceFrame{firstFrame}, frames));
    CHECK_OK(store.publish(std::move(*handle)));
}

// The nth 20 ms packet of one continuous 44.1 kHz run.
void publishPacket(NativePcmStore& store, BlockPool& pool, uint64_t index)
{
    publishBlock(store, pool, index * kPacketFrames, kPacketFrames, fillFor(index));
}

std::unique_ptr<BlockPool> makePool(size_t blocks, uint32_t framesPerBlock = kPacketFrames)
{
    auto pool = BlockPool::create(framesPerBlock * 4, blocks); // 4 bytes a frame
    CHECK_OK(pool);
    return std::move(*pool);
}

std::unique_ptr<NativePcmStore> makeStore(uint64_t retentionNs,
                                          size_t maxBlocks = NativePcmStore::kDefaultMaxBlocks)
{
    auto store = NativePcmStore::create(retentionNs, maxBlocks);
    CHECK_OK(store);
    return std::move(*store);
}

} // namespace

MULTIROOM_TEST(pcm_store_refuses_a_retention_it_cannot_honour)
{
    CHECK_ERR(NativePcmStore::create(0, 8), PcmStoreError::InvalidRetention);
    CHECK_ERR(NativePcmStore::create(kPacketNs, 0), PcmStoreError::InvalidRetention);

    auto store = makeStore(NativePcmStore::kDefaultRetentionNs);
    CHECK_EQ(store->retainedBlocks(), size_t{0});
    CHECK_EQ(store->retainedNs(), uint64_t{0});
    CHECK_EQ(std::to_underlying(store->nextFrame()), uint64_t{0});
    CHECK_EQ(std::to_underlying(store->oldestFrame()), uint64_t{0});
}

MULTIROOM_TEST(pcm_store_publishes_a_contiguous_run)
{
    auto pool = makePool(8);
    auto store = makeStore(NativePcmStore::kDefaultRetentionNs);

    for (uint64_t i = 0; i < 4; ++i)
        publishPacket(*store, *pool, i);

    CHECK_EQ(store->retainedBlocks(), size_t{4});
    CHECK_EQ(store->retainedNs(), 4 * kPacketNs);
    CHECK_EQ(std::to_underlying(store->oldestFrame()), uint64_t{0});
    CHECK_EQ(std::to_underlying(store->nextFrame()), uint64_t{4 * kPacketFrames});
    CHECK_EQ(store->evictedCount(), uint64_t{0});
}

MULTIROOM_TEST(pcm_store_refuses_a_block_it_cannot_publish)
{
    auto pool = makePool(8);
    auto store = makeStore(NativePcmStore::kDefaultRetentionNs);

    // Never committed: the producer has not said what is in it or where.
    auto uncommitted = pool->acquire();
    CHECK_OK(uncommitted);
    CHECK_ERR(store->publish(std::move(*uncommitted)), PcmStoreError::InvalidBlock);

    auto empty = pool->acquire();
    CHECK_OK(empty);
    CHECK_OK((*empty)->commit(stereo(), SourceFrame{0}, 0));
    CHECK_ERR(store->publish(std::move(*empty)), PcmStoreError::InvalidBlock);

    // The first block may begin anywhere; every later one must continue the
    // run, because a gap is a position that maps to the wrong music.
    publishBlock(*store, *pool, 1000, kPacketFrames, fillFor(0));
    auto gap = pool->acquire();
    CHECK_OK(gap);
    CHECK_OK((*gap)->commit(stereo(), SourceFrame{1000 + 2 * kPacketFrames}, kPacketFrames));
    CHECK_ERR(store->publish(std::move(*gap)), PcmStoreError::OutOfOrder);

    // The refusals cost nothing: the run is where it was.
    CHECK_EQ(store->retainedBlocks(), size_t{1});
    CHECK_EQ(std::to_underlying(store->nextFrame()), uint64_t{1000 + kPacketFrames});
}

MULTIROOM_TEST(pcm_store_serves_three_readers_at_different_speeds)
{
    auto pool = makePool(64);
    auto store = makeStore(NativePcmStore::kDefaultRetentionNs);

    const auto fast = store->addReader(SourceFrame{0});
    const auto middle = store->addReader(SourceFrame{0});
    const auto slow = store->addReader(SourceFrame{0});
    CHECK_EQ(store->readerCount(), size_t{3});

    for (uint64_t i = 0; i < 30; ++i)
        publishPacket(*store, *pool, i);

    // One decode, three cursors: each reader sees the same blocks, in order,
    // at its own pace.
    for (uint64_t i = 0; i < 30; ++i)
    {
        auto read = store->read(fast);
        CHECK_OK(read);
        CHECK_EQ(std::to_underlying(read->block->firstFrame()), i * kPacketFrames);
        CHECK_EQ(read->frameOffset, 0u);
        CHECK_EQ(static_cast<unsigned>(read->block->bytes()[0]),
                 static_cast<unsigned>(fillFor(i)));
    }
    CHECK_ERR(store->read(fast), PcmStoreError::AtFrontier);

    for (uint64_t i = 0; i < 15; ++i)
        CHECK_OK(store->read(middle));
    CHECK_OK(store->read(slow));

    CHECK_EQ(std::to_underlying(*store->cursor(fast)), uint64_t{30 * kPacketFrames});
    CHECK_EQ(std::to_underlying(*store->cursor(middle)), uint64_t{15 * kPacketFrames});
    CHECK_EQ(std::to_underlying(*store->cursor(slow)), uint64_t{1 * kPacketFrames});

    // A reader is a cursor and nothing else, so dropping one leaves the others
    // exactly where they were.
    CHECK_OK(store->removeReader(middle));
    CHECK_ERR(store->read(middle), PcmStoreError::UnknownReader);
    CHECK_ERR(store->removeReader(middle), PcmStoreError::UnknownReader);
    CHECK_ERR(store->cursor(middle), PcmStoreError::UnknownReader);
    CHECK_ERR(store->seek(middle, SourceFrame{0}), PcmStoreError::UnknownReader);
    CHECK_EQ(store->readerCount(), size_t{2});
    CHECK_EQ(std::to_underlying(*store->cursor(slow)), uint64_t{1 * kPacketFrames});
}

MULTIROOM_TEST(pcm_store_reads_a_cursor_that_sits_inside_a_block)
{
    auto pool = makePool(8);
    auto store = makeStore(NativePcmStore::kDefaultRetentionNs);
    for (uint64_t i = 0; i < 3; ++i)
        publishPacket(*store, *pool, i);

    // A cursor lands mid-block after a seek, and a reader handed the whole
    // block would play audio it has already played.
    const auto reader = store->addReader(SourceFrame{kPacketFrames + 100});
    auto read = store->read(reader);
    CHECK_OK(read);
    CHECK_EQ(std::to_underlying(read->block->firstFrame()), uint64_t{kPacketFrames});
    CHECK_EQ(read->frameOffset, 100u);

    // The cursor advances past the whole block it sat in.
    CHECK_EQ(std::to_underlying(*store->cursor(reader)), uint64_t{2 * kPacketFrames});

    // Ahead of the writer is allowed: that is a room asking for audio the
    // decoder has not produced yet, not a fault.
    CHECK_OK(store->seek(reader, SourceFrame{99 * kPacketFrames}));
    CHECK_ERR(store->read(reader), PcmStoreError::AtFrontier);
}

MULTIROOM_TEST(pcm_store_tells_a_reader_it_was_evicted_rather_than_serving_stale_audio)
{
    auto pool = makePool(16);
    auto store = makeStore(5 * kPacketNs); // 100 ms of retention

    const auto slow = store->addReader(SourceFrame{0});
    const auto keepingUp = store->addReader(SourceFrame{0});

    for (uint64_t i = 0; i < 20; ++i)
    {
        publishPacket(*store, *pool, i);
        auto read = store->read(keepingUp);
        CHECK_OK(read);
        CHECK_EQ(static_cast<unsigned>(read->block->bytes()[0]),
                 static_cast<unsigned>(fillFor(i)));
    }

    CHECK_EQ(store->retainedBlocks(), size_t{5});
    CHECK_EQ(store->retainedNs(), 5 * kPacketNs);
    CHECK_EQ(store->evictedCount(), uint64_t{15});

    // The audio at frame 0 is gone. Reading on would be this track's later
    // samples under an earlier position, which is worse than a failure.
    CHECK_ERR(store->read(slow), PcmStoreError::Evicted);

    // The store does not move a cursor on the reader's behalf: the zone
    // decides where an evicted room resumes (phase 16's rejoin).
    CHECK_EQ(std::to_underlying(*store->cursor(slow)), uint64_t{0});
    CHECK_ERR(store->read(slow), PcmStoreError::Evicted);

    CHECK_OK(store->seek(slow, store->oldestFrame()));
    auto rejoined = store->read(slow);
    CHECK_OK(rejoined);
    CHECK_EQ(std::to_underlying(rejoined->block->firstFrame()), uint64_t{15 * kPacketFrames});
    CHECK_EQ(static_cast<unsigned>(rejoined->block->bytes()[0]),
             static_cast<unsigned>(fillFor(15)));
}

MULTIROOM_TEST(pcm_store_retention_is_a_duration_and_not_a_block_count)
{
    auto pool = makePool(400, kPacketFrames * 5);

    // 20 ms blocks and 100 ms blocks, one second of retention: the same
    // duration is a different number of blocks, which is the whole reason the
    // limit is expressed in nanoseconds.
    auto small = makeStore(kNanosecondsPerSecond);
    for (uint64_t i = 0; i < 200; ++i)
        publishPacket(*small, *pool, i);
    CHECK_EQ(small->retainedBlocks(), size_t{50});
    CHECK_EQ(small->retainedNs(), kNanosecondsPerSecond);

    auto large = makeStore(kNanosecondsPerSecond);
    for (uint64_t i = 0; i < 40; ++i)
        publishBlock(*large, *pool, i * kPacketFrames * 5, kPacketFrames * 5, fillFor(i));
    CHECK_EQ(large->retainedBlocks(), size_t{10});
    CHECK_EQ(large->retainedNs(), kNanosecondsPerSecond);

    // A rate change costs nothing: each block's own format says how long it
    // is, so 96 kHz audio is retained for the same second.
    auto mixed = makeStore(kNanosecondsPerSecond);
    for (uint64_t i = 0; i < 100; ++i)
        publishBlock(*mixed, *pool, i * 1920, 1920, fillFor(i), stereo(96000));
    CHECK_EQ(mixed->retainedBlocks(), size_t{50});
    CHECK_EQ(mixed->retainedNs(), kNanosecondsPerSecond);
}

MULTIROOM_TEST(pcm_store_block_ceiling_is_the_backstop_under_the_duration)
{
    auto pool = makePool(64);

    // Very short blocks would hold the whole BlockPool to satisfy the same
    // five seconds, so the ceiling evicts first and the store still never
    // grows.
    auto store = makeStore(NativePcmStore::kDefaultRetentionNs, 8);
    for (uint64_t i = 0; i < 500; ++i)
        publishBlock(*store, *pool, i * 4, 4, fillFor(i));

    CHECK_EQ(store->retainedBlocks(), size_t{8});
    CHECK_EQ(store->evictedCount(), uint64_t{492});
    CHECK(store->retainedNs() < NativePcmStore::kDefaultRetentionNs);
}

MULTIROOM_TEST(pcm_store_writer_never_stalls_because_a_reader_never_reads)
{
    // A pool barely larger than what retention holds, and a room that stops
    // reading entirely: the specification's central rule seen from the store's
    // side is that this costs the writer nothing at all.
    auto pool = makePool(8);
    auto store = makeStore(5 * kPacketNs);

    const auto idle = store->addReader(SourceFrame{0});
    for (uint64_t i = 0; i < 5000; ++i)
        publishPacket(*store, *pool, i);

    CHECK_EQ(std::to_underlying(store->nextFrame()), uint64_t{5000 * kPacketFrames});
    CHECK_EQ(store->retainedBlocks(), size_t{5});
    CHECK_EQ(store->evictedCount(), uint64_t{4995});
    CHECK_EQ(pool->exhaustedCount(), uint64_t{0});

    // The room that never read is the only one that pays, and it is told so.
    CHECK_EQ(std::to_underlying(*store->cursor(idle)), uint64_t{0});
    CHECK_ERR(store->read(idle), PcmStoreError::Evicted);
}

MULTIROOM_TEST(pcm_store_wraps_and_reuses_its_blocks_under_sustained_writing)
{
    auto pool = makePool(8);
    auto store = makeStore(3 * kPacketNs);
    const auto reader = store->addReader(SourceFrame{0});

    // One block held by the room, three retained, one being written: eight
    // pool blocks are enough forever, and the pool is never asked for a ninth.
    NativePcmStore::Read held;
    for (uint64_t i = 0; i < 20000; ++i)
    {
        publishPacket(*store, *pool, i);
        auto read = store->read(reader);
        CHECK_OK(read);
        CHECK_EQ(static_cast<unsigned>(read->block->bytes()[0]),
                 static_cast<unsigned>(fillFor(i)));
        held = std::move(*read); // the room is still playing what it was given

        CHECK(store->retainedBlocks() <= size_t{3});
        CHECK(pool->inUse() <= size_t{4});
    }

    CHECK_EQ(pool->exhaustedCount(), uint64_t{0});
    CHECK_EQ(pool->totalBytes(), size_t{8 * kPacketFrames * 4});
    CHECK_EQ(held.block->frameCount(), kPacketFrames);
}

MULTIROOM_TEST(pcm_store_memory_stays_bounded_over_a_long_run)
{
    auto pool = makePool(300);
    auto store = makeStore(NativePcmStore::kDefaultRetentionNs);

    const auto fast = store->addReader(SourceFrame{0});
    const auto middle = store->addReader(SourceFrame{0});
    const auto slow = store->addReader(SourceFrame{0});

    // Half an hour of 44.1 kHz audio through three rooms at three speeds. Five
    // seconds of retention is 250 packets of 20 ms, and that is the ceiling
    // however long the run is.
    size_t peakBlocks = 0;
    size_t peakInUse = 0;
    for (uint64_t i = 0; i < 90000; ++i)
    {
        publishPacket(*store, *pool, i);
        CHECK_OK(store->read(fast));
        if (i >= 100) // two seconds behind the room above, and staying there
            CHECK_OK(store->read(middle));
        if (i % 100 == 0 && i > 0)
            CHECK_OK(store->seek(slow, store->oldestFrame()));

        peakBlocks = std::max(peakBlocks, store->retainedBlocks());
        peakInUse = std::max(peakInUse, pool->inUse());
    }

    CHECK_EQ(peakBlocks, size_t{250});
    CHECK(peakInUse <= size_t{251});
    CHECK_EQ(store->retainedNs(), NativePcmStore::kDefaultRetentionNs);
    CHECK_EQ(pool->exhaustedCount(), uint64_t{0});
    CHECK_EQ(pool->totalBytes(), size_t{300 * kPacketFrames * 4});
}

MULTIROOM_TEST(pcm_store_serves_readers_on_their_own_threads)
{
    // One pipeline thread writing, three client output threads reading: the
    // mutex guards the ring and the cursors, and nothing waits for a room.
    auto pool = makePool(64);
    auto store = makeStore(20 * kPacketNs);

    constexpr uint64_t kBlocks = 4000;
    std::atomic<bool> writerDone{false};
    std::vector<std::thread> rooms;
    std::vector<uint64_t> served(3, 0);
    std::vector<uint64_t> outOfOrder(3, 0);

    for (size_t room = 0; room < 3; ++room)
    {
        rooms.emplace_back([&store, &writerDone, &served, &outOfOrder, room]() {
            const auto reader = store->addReader(SourceFrame{0});
            uint64_t expected = 0;
            for (;;)
            {
                auto read = store->read(reader);
                if (read)
                {
                    const uint64_t first = std::to_underlying(read->block->firstFrame());
                    if (first != expected)
                        ++outOfOrder[room];
                    expected = first + read->block->frameCount();
                    ++served[room];
                    continue;
                }
                if (read.error().code == PcmStoreError::Evicted)
                {
                    // A room that fell behind rejoins where the audio still
                    // is, exactly as phase 16 will.
                    expected = std::to_underlying(store->oldestFrame());
                    (void)store->seek(reader, SourceFrame{expected});
                    continue;
                }
                if (writerDone.load(std::memory_order_acquire))
                    break;
                std::this_thread::yield();
            }
        });
    }

    for (uint64_t i = 0; i < kBlocks; ++i)
        publishPacket(*store, *pool, i);
    writerDone.store(true, std::memory_order_release);

    for (std::thread& room : rooms)
        room.join();

    CHECK_EQ(std::to_underlying(store->nextFrame()), kBlocks * kPacketFrames);
    CHECK_EQ(pool->exhaustedCount(), uint64_t{0});
    for (size_t room = 0; room < 3; ++room)
    {
        CHECK(served[room] > 0);
        CHECK_EQ(outOfOrder[room], uint64_t{0});
    }
}
