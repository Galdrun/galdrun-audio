#include "TestSupport.h"

#include "audio/BlockPool.h"

#include <cstring>
#include <thread>
#include <utility>
#include <vector>

using namespace multiroom;

namespace {

AudioFormat stereo(uint32_t rate, AudioFormat::SampleType type)
{
    AudioFormat format;
    format.sampleRate = rate;
    format.channels = 2;
    format.sampleType = type;
    return format;
}

} // namespace

MULTIROOM_TEST(block_pool_rejects_a_size_it_cannot_honour)
{
    CHECK_ERR(BlockPool::create(0, 16), BlockPoolError::InvalidSize);
    CHECK_ERR(BlockPool::create(4096, 0), BlockPoolError::InvalidSize);

    // The ceiling exists so a config typo is a refusal at startup rather than
    // an out-of-memory kill an hour later. It is checked before anything is
    // reserved, so asking for it costs nothing.
    CHECK_ERR(BlockPool::create(BlockPool::kMaxTotalBytes, 2), BlockPoolError::TooLarge);
    CHECK_ERR(BlockPool::create(BlockPool::kMaxTotalBytes / 2 + 1, 2), BlockPoolError::TooLarge);
    CHECK_ERR(BlockPool::create(1024, BlockPool::kMaxTotalBytes), BlockPoolError::TooLarge);
}

MULTIROOM_TEST(block_pool_reserves_its_memory_once)
{
    auto pool = BlockPool::create(4096, 8);
    CHECK_OK(pool);

    CHECK_EQ((*pool)->blockBytes(), size_t{4096});
    CHECK_EQ((*pool)->blockCount(), size_t{8});
    CHECK_EQ((*pool)->totalBytes(), size_t{4096 * 8});
    CHECK_EQ((*pool)->available(), size_t{8});
    CHECK_EQ((*pool)->inUse(), size_t{0});
}

MULTIROOM_TEST(block_pool_exhaustion_is_an_explicit_counted_failure)
{
    auto created = BlockPool::create(1024, 3);
    CHECK_OK(created);
    BlockPool& pool = **created;

    std::vector<BlockPool::Handle> held;
    for (int i = 0; i < 3; ++i)
    {
        auto block = pool.acquire();
        CHECK_OK(block);
        held.push_back(std::move(*block));
    }

    CHECK_EQ(pool.available(), size_t{0});
    CHECK_EQ(pool.inUse(), size_t{3});
    CHECK_EQ(pool.exhaustedCount(), uint64_t{0});

    // The pool never allocates a fourth block: that is what turns a slow
    // consumer into unbounded memory rather than a visible failure.
    CHECK_ERR(pool.acquire(), BlockPoolError::Exhausted);
    CHECK_ERR(pool.acquire(), BlockPoolError::Exhausted);
    CHECK_EQ(pool.exhaustedCount(), uint64_t{2});
    CHECK_EQ(pool.totalBytes(), size_t{3 * 1024});
}

MULTIROOM_TEST(block_pool_reuses_a_released_block)
{
    auto created = BlockPool::create(1024, 2);
    CHECK_OK(created);
    BlockPool& pool = **created;

    const AudioBlock* first = nullptr;
    {
        auto block = pool.acquire();
        CHECK_OK(block);
        first = block->get();
        CHECK_EQ(pool.available(), size_t{1});
    }
    CHECK_EQ(pool.available(), size_t{2});

    // Same storage, and the pool hands out the same number of blocks forever.
    auto again = pool.acquire();
    CHECK_OK(again);
    CHECK(again->get() == first);
    CHECK_EQ(pool.exhaustedCount(), uint64_t{0});
}

MULTIROOM_TEST(block_pool_recycles_a_block_only_when_the_last_reader_lets_go)
{
    auto created = BlockPool::create(1024, 1);
    CHECK_OK(created);
    BlockPool& pool = **created;

    {
        auto handle = pool.acquire();
        CHECK_OK(handle);
        CHECK_OK((*handle)->commit(stereo(44100, AudioFormat::SampleType::Int16), SourceFrame{0},
                                   8));

        // Publication: one decode, handed to every client output of the zone.
        BlockPool::SharedBlock published = std::move(*handle);

        BlockPool::SharedBlock roomA = published;
        BlockPool::SharedBlock roomB = published;
        published.reset();
        roomA.reset();

        CHECK_EQ(pool.available(), size_t{0}); // roomB is still reading
        CHECK_EQ(roomB->frameCount(), 8u);
    }

    CHECK_EQ(pool.available(), size_t{1});
}

MULTIROOM_TEST(block_pool_resets_a_block_before_handing_it_out_again)
{
    auto created = BlockPool::create(1024, 1);
    CHECK_OK(created);
    BlockPool& pool = **created;

    {
        auto handle = pool.acquire();
        CHECK_OK(handle);
        CHECK_OK((*handle)->commit(stereo(96000, AudioFormat::SampleType::Int24In32),
                                   SourceFrame{4242}, 16));
    }

    auto reused = pool.acquire();
    CHECK_OK(reused);

    // A recycled block must not look published, or the next producer would
    // ship the previous track's position with this track's audio.
    CHECK(!(*reused)->committed());
    CHECK_EQ((*reused)->frameCount(), 0u);
    CHECK_EQ(std::to_underlying((*reused)->firstFrame()), uint64_t{0});
    CHECK(!(*reused)->format().isValid());
    CHECK((*reused)->bytes().empty());
}

MULTIROOM_TEST(audio_block_commits_format_position_and_length)
{
    auto created = BlockPool::create(4096, 1);
    CHECK_OK(created);

    auto handle = (*created)->acquire();
    CHECK_OK(handle);
    AudioBlock& block = **handle;

    const AudioFormat format = stereo(44100, AudioFormat::SampleType::Int24);
    CHECK_EQ(block.capacityBytes(), size_t{4096});
    CHECK_EQ(block.capacityFrames(format), 682u); // 4096 / 6 bytes per frame
    CHECK_EQ(block.capacityFrames(AudioFormat{}), 0u);

    CHECK(!block.committed());
    CHECK(block.bytes().empty());
    CHECK_EQ(block.writable().size(), size_t{4096});

    std::memset(block.writable().data(), 0x5A, block.writable().size());
    CHECK_OK(block.commit(format, SourceFrame{88200}, 100));

    CHECK(block.committed());
    CHECK(block.format() == format);
    CHECK_EQ(std::to_underlying(block.firstFrame()), uint64_t{88200});
    CHECK_EQ(block.frameCount(), 100u);

    // bytes() is the committed audio, not the capacity.
    CHECK_EQ(block.bytes().size(), size_t{600});
    CHECK_EQ(static_cast<unsigned>(block.bytes()[0]), 0x5Au);
    CHECK_EQ(static_cast<unsigned>(block.bytes()[599]), 0x5Au);
}

MULTIROOM_TEST(audio_block_refuses_a_second_commit_and_an_oversized_one)
{
    auto created = BlockPool::create(64, 2);
    CHECK_OK(created);

    auto first = (*created)->acquire();
    CHECK_OK(first);
    const AudioFormat format = stereo(48000, AudioFormat::SampleType::Int32); // 8 bytes a frame

    // 9 frames is 72 bytes into a 64 byte block. Refused, not truncated: a
    // short block is silently wrong audio.
    CHECK_ERR((*first)->commit(format, SourceFrame{0}, 9), BlockError::TooLarge);
    CHECK(!(*first)->committed());

    CHECK_OK((*first)->commit(format, SourceFrame{0}, 8));
    CHECK_ERR((*first)->commit(format, SourceFrame{0}, 4), BlockError::AlreadyCommitted);
    CHECK_EQ((*first)->frameCount(), 8u);

    auto second = (*created)->acquire();
    CHECK_OK(second);
    CHECK_ERR((*second)->commit(AudioFormat{}, SourceFrame{0}, 1), BlockError::InvalidFormat);
}

MULTIROOM_TEST(block_pool_is_safe_to_release_from_other_threads)
{
    // Blocks are acquired on a zone's pipeline thread and released wherever
    // the last client output drops its reference, which is a different thread
    // per room.
    auto created = BlockPool::create(256, 32);
    CHECK_OK(created);
    BlockPool& pool = **created;

    for (int round = 0; round < 50; ++round)
    {
        std::vector<BlockPool::SharedBlock> published;
        for (int i = 0; i < 32; ++i)
        {
            auto handle = pool.acquire();
            CHECK_OK(handle);
            CHECK_OK((*handle)->commit(stereo(48000, AudioFormat::SampleType::Int16),
                                       SourceFrame{static_cast<uint64_t>(i)}, 4));
            published.push_back(std::move(*handle));
        }
        CHECK_EQ(pool.available(), size_t{0});

        std::vector<std::thread> rooms;
        for (int room = 0; room < 4; ++room)
        {
            rooms.emplace_back([&published, room]() {
                for (size_t i = room; i < published.size(); i += 4)
                    published[i].reset();
            });
        }
        for (std::thread& room : rooms)
            room.join();

        CHECK_EQ(pool.available(), size_t{32});
    }

    CHECK_EQ(pool.exhaustedCount(), uint64_t{0});
}

MULTIROOM_TEST(block_bytes_for_covers_one_packet_of_the_largest_format)
{
    // 20 ms of 192 kHz stereo in the widest layout: 3840 frames of 8 bytes,
    // plus the one frame of headroom the rounding rule allows for.
    CHECK_EQ(blockBytesFor(192000, 2, 20), size_t{(3840 + 1) * 8});

    // A duration, never a frame count: the same 20 ms is a different number of
    // frames per rate, and one config value covers every room.
    CHECK_EQ(blockBytesFor(44100, 2, 20), size_t{(882 + 1) * 8});
    CHECK_EQ(blockBytesFor(48000, 2, 20), size_t{(960 + 1) * 8});
    CHECK_EQ(blockBytesFor(96000, 6, 20), size_t{(1920 + 1) * 24});

    CHECK_EQ(blockBytesFor(0, 2, 20), size_t{0});
    CHECK_EQ(blockBytesFor(48000, 0, 20), size_t{0});
    CHECK_EQ(blockBytesFor(48000, 2, 0), size_t{0});

    // And a block sized this way really does hold a packet of any format
    // underneath the ceiling.
    auto created = BlockPool::create(blockBytesFor(192000, 2, 20), 1);
    CHECK_OK(created);
    auto handle = (*created)->acquire();
    CHECK_OK(handle);
    for (const uint32_t rate : {44100u, 48000u, 96000u, 192000u})
    {
        const AudioFormat format = stereo(rate, AudioFormat::SampleType::Int24In32);
        const uint64_t frames = framesFromNanoseconds(20'000'000, rate).value();
        CHECK((*handle)->capacityFrames(format) >= frames);
    }
}

// --- phase 42: one pool per zone, one ceiling for the core -------------------

MULTIROOM_TEST(blocks_for_a_zone_are_its_retention_in_blocks_plus_two)
{
    // The roadmap's table, with the default 20 ms blocks at the 192 kHz int32
    // stereo ceiling and a 5 s retention.
    const size_t blockBytes = blockBytesFor(192000, 2, 20);
    const uint64_t retention = 5ULL * 1000000000ULL;
    CHECK_EQ(blockBytes, size_t{30728});
    CHECK_EQ(blocksFor(blockBytes, stereo(44100, AudioFormat::SampleType::Int16), retention),
             size_t{31});
    CHECK_EQ(blocksFor(blockBytes, stereo(44100, AudioFormat::SampleType::Float32), retention),
             size_t{60});
    CHECK_EQ(blocksFor(blockBytes, stereo(96000, AudioFormat::SampleType::Int32), retention),
             size_t{127});
    CHECK_EQ(blocksFor(blockBytes, stereo(192000, AudioFormat::SampleType::Int32), retention),
             size_t{252});
    CHECK_EQ(blocksFor(4, stereo(48000, AudioFormat::SampleType::Int32), retention), size_t{0});
}

MULTIROOM_TEST(a_zones_pool_grows_against_the_core_limit_and_is_refused_with_nothing_taken)
{
    AudioMemory memory(1024, 10 * 1024);
    std::unique_ptr<BlockPool> first = BlockPool::create(memory);
    std::unique_ptr<BlockPool> second = BlockPool::create(memory);
    CHECK_EQ(first->blockCount(), size_t{0});
    CHECK_ERR(first->acquire(), BlockPoolError::Exhausted); // nothing until it is sized

    CHECK_OK(first->resize(6));
    CHECK_EQ(first->blockCount(), size_t{6});
    CHECK_EQ(memory.usedBytes(), 6ULL * 1024);

    // Five more would pass the limit: refused, and nothing allocated or counted.
    CHECK_ERR(second->resize(5), BlockPoolError::OverLimit);
    CHECK_EQ(second->blockCount(), size_t{0});
    CHECK_EQ(memory.usedBytes(), 6ULL * 1024);
    CHECK_OK(second->resize(4));
    CHECK_EQ(memory.usedBytes(), 10ULL * 1024);

    // A lower limit takes nothing back from what is held.
    memory.setLimitBytes(4 * 1024);
    CHECK_EQ(first->blockCount(), size_t{6});
    CHECK_OK(first->acquire());

    // A pool that goes gives everything back.
    second.reset();
    CHECK_EQ(memory.usedBytes(), 6ULL * 1024);
}

MULTIROOM_TEST(a_shrinking_pool_frees_what_is_free_now_and_the_rest_as_it_comes_back)
{
    AudioMemory memory(512, 1024 * 1024);
    std::unique_ptr<BlockPool> pool = BlockPool::create(memory);
    CHECK_OK(pool->resize(8));

    std::vector<BlockPool::SharedBlock> held;
    for (int i = 0; i < 5; ++i)
    {
        auto handle = pool->acquire();
        CHECK_OK(handle);
        CHECK_OK((*handle)->commit(stereo(48000, AudioFormat::SampleType::Int16), SourceFrame{0},
                                   1));
        held.emplace_back(std::move(*handle));
    }

    // Three are free and go at once; two of the five still read go with them
    // when they come back, and three are recycled.
    CHECK_OK(pool->resize(3));
    CHECK_EQ(pool->blockCount(), size_t{5});
    CHECK_EQ(memory.usedBytes(), 5ULL * 512);
    held.clear();
    CHECK_EQ(pool->blockCount(), size_t{3});
    CHECK_EQ(pool->available(), size_t{3});
    CHECK_EQ(memory.usedBytes(), 3ULL * 512);

    // And zero is nothing at all.
    CHECK_OK(pool->resize(0));
    CHECK_EQ(memory.usedBytes(), 0ULL);
}
