#include "TestSupport.h"

#include "audio/AudioRingBuffer.h"
#include "audio/RingBufferSource.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

using namespace multiroom;

namespace {

// The ring buffer promises that push() and pop() never allocate, and the audio
// thread depends on it. Counting every allocation in the whole test binary is
// the only way to hold it to that from the outside.
std::atomic<int> g_allocations(0);

// Frames are a single uint32 here, so a value carries its own sequence number
// and any reordering or gap shows up immediately.
void writeSequence(std::vector<unsigned char>& block, uint32_t first, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t value = first + i;
        std::memcpy(block.data() + static_cast<size_t>(i) * 4, &value, 4);
    }
}

uint32_t frameAt(const std::vector<unsigned char>& block, uint32_t index)
{
    uint32_t value = 0;
    std::memcpy(&value, block.data() + static_cast<size_t>(index) * 4, 4);
    return value;
}

AudioFormat makeFormat(uint32_t rate, uint16_t channels, AudioFormat::SampleType type)
{
    AudioFormat format;
    format.sampleRate = rate;
    format.channels = channels;
    format.sampleType = type;
    return format;
}

} // namespace

void* operator new(std::size_t size)
{
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    void* memory = std::malloc(size != 0 ? size : 1);
    if (memory == nullptr)
        throw std::bad_alloc();
    return memory;
}

void* operator new[](std::size_t size)
{
    return operator new(size);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

MULTIROOM_TEST(ring_buffer_refuses_an_unusable_configuration)
{
    AudioRingBuffer buffer;
    std::string error;

    CHECK(!buffer.configure(0, 1024, error));
    CHECK(!error.empty());
    CHECK(!buffer.configure(4, 0, error));
    CHECK(!buffer.configure(4, 1, error)); // one frame cannot be full and empty
    CHECK(!buffer.configure(4, 1u << 23, error));
    // Frame size times capacity: plausible on their own, far too large together.
    CHECK(!buffer.configure(64, 1u << 22, error));

    // Nothing usable was configured, so both sides must refuse to move data
    // rather than write into a buffer that does not exist.
    unsigned char frame[4] = {0, 0, 0, 0};
    CHECK(!buffer.isConfigured());
    CHECK_EQ(buffer.push(frame, 1), static_cast<uint32_t>(0));
    CHECK_EQ(buffer.pop(frame, 1), static_cast<uint32_t>(0));
}

MULTIROOM_TEST(ring_buffer_refused_reconfiguration_does_not_keep_the_old_buffer)
{
    AudioRingBuffer buffer;
    std::string error;
    CHECK(buffer.configure(4, 1024, error));
    unsigned char frame[4] = {1, 2, 3, 4};
    CHECK_EQ(buffer.push(frame, 1), static_cast<uint32_t>(1));

    // isConfigured() is what callers read as "sized for the new format": the
    // old buffer, with its old frame size and audio, must not answer for it.
    CHECK(!buffer.configure(32, 1u << 22, error));
    CHECK(!buffer.isConfigured());
    CHECK_EQ(buffer.bytesPerFrame(), static_cast<uint32_t>(0));
    CHECK_EQ(buffer.readableFrames(), static_cast<uint32_t>(0));
    CHECK_EQ(buffer.pop(frame, 1), static_cast<uint32_t>(0));

    // And the next good configuration starts clean.
    CHECK(buffer.configure(8, 256, error));
    CHECK_EQ(buffer.bytesPerFrame(), static_cast<uint32_t>(8));
    CHECK_EQ(buffer.readableFrames(), static_cast<uint32_t>(0));
}

MULTIROOM_TEST(ring_buffer_rounds_the_capacity_up_to_a_power_of_two)
{
    AudioRingBuffer buffer;
    std::string error;
    CHECK(buffer.configure(4, 1000, error));

    // 1000 frames were asked for, 1024 are granted: the index arithmetic is a
    // mask, which only works on a power of two.
    CHECK_EQ(buffer.capacityFrames(), static_cast<uint32_t>(1024));
    CHECK_EQ(buffer.bytesPerFrame(), static_cast<uint32_t>(4));
    CHECK_EQ(buffer.writableFrames(), static_cast<uint32_t>(1024));
    CHECK_EQ(buffer.readableFrames(), static_cast<uint32_t>(0));

    // A power of two is kept as it is.
    CHECK(buffer.configure(8, 256, error));
    CHECK_EQ(buffer.capacityFrames(), static_cast<uint32_t>(256));
}

MULTIROOM_TEST(ring_buffer_names_the_bytes_configure_would_allocate)
{
    // Rounded like configure(), so a budget check before the call is exact.
    CHECK_EQ(AudioRingBuffer::storageBytes(4, 1000), static_cast<uint64_t>(4096));
    CHECK_EQ(AudioRingBuffer::storageBytes(8, 120000), static_cast<uint64_t>(131072 * 8));
    // Zero wherever configure() refuses.
    CHECK_EQ(AudioRingBuffer::storageBytes(0, 1000), static_cast<uint64_t>(0));
    CHECK_EQ(AudioRingBuffer::storageBytes(4, 1), static_cast<uint64_t>(0));
    CHECK_EQ(AudioRingBuffer::storageBytes(64, 1u << 22), static_cast<uint64_t>(0));

    AudioRingBuffer buffer;
    std::string error;
    CHECK(buffer.configure(8, 1000, error));
    CHECK(buffer.configure(8, 100000, error)); // grows
    CHECK_EQ(buffer.capacityFrames(), static_cast<uint32_t>(131072));
}

MULTIROOM_TEST(ring_buffer_fills_empties_and_wraps)
{
    AudioRingBuffer buffer;
    std::string error;
    CHECK(buffer.configure(4, 8, error));

    std::vector<unsigned char> block(8 * 4);
    std::vector<unsigned char> out(8 * 4);

    writeSequence(block, 0, 8);
    CHECK_EQ(buffer.push(block.data(), 8), static_cast<uint32_t>(8));
    CHECK_EQ(buffer.readableFrames(), static_cast<uint32_t>(8));
    CHECK_EQ(buffer.writableFrames(), static_cast<uint32_t>(0));

    // Full: a further push takes nothing rather than overwriting what the
    // consumer has not read.
    writeSequence(block, 100, 8);
    CHECK_EQ(buffer.push(block.data(), 8), static_cast<uint32_t>(0));

    CHECK_EQ(buffer.pop(out.data(), 3), static_cast<uint32_t>(3));
    CHECK_EQ(frameAt(out, 0), static_cast<uint32_t>(0));
    CHECK_EQ(frameAt(out, 2), static_cast<uint32_t>(2));
    CHECK_EQ(buffer.writableFrames(), static_cast<uint32_t>(3));

    // This block starts at index 0 of the storage again while the reader is
    // still in the middle of it: the wrap is the case that goes wrong when the
    // second memcpy is missing.
    writeSequence(block, 8, 3);
    CHECK_EQ(buffer.push(block.data(), 3), static_cast<uint32_t>(3));
    CHECK_EQ(buffer.readableFrames(), static_cast<uint32_t>(8));

    // Everything comes back in order, across the wrap.
    CHECK_EQ(buffer.pop(out.data(), 8), static_cast<uint32_t>(8));
    for (uint32_t i = 0; i < 8; ++i)
        CHECK_EQ(frameAt(out, i), i + 3);

    CHECK_EQ(buffer.readableFrames(), static_cast<uint32_t>(0));
    CHECK_EQ(buffer.pop(out.data(), 1), static_cast<uint32_t>(0));
}

MULTIROOM_TEST(ring_buffer_is_short_rather_than_blocking_at_either_end)
{
    AudioRingBuffer buffer;
    std::string error;
    CHECK(buffer.configure(4, 16, error));

    std::vector<unsigned char> block(32 * 4);
    std::vector<unsigned char> out(32 * 4);

    // A producer faster than the consumer: the push is short by exactly what
    // does not fit, and the frames that were taken are the first ones.
    writeSequence(block, 0, 32);
    CHECK_EQ(buffer.push(block.data(), 32), static_cast<uint32_t>(16));

    // A consumer faster than the producer: the pop is short by exactly what is
    // missing, and never waits for it.
    CHECK_EQ(buffer.pop(out.data(), 32), static_cast<uint32_t>(16));
    for (uint32_t i = 0; i < 16; ++i)
        CHECK_EQ(frameAt(out, i), i);
    CHECK_EQ(buffer.pop(out.data(), 32), static_cast<uint32_t>(0));
}

MULTIROOM_TEST(ring_buffer_does_not_allocate_in_push_or_pop)
{
    AudioRingBuffer buffer;
    std::string error;
    CHECK(buffer.configure(4, 64, error));

    std::vector<unsigned char> block(16 * 4);
    std::vector<unsigned char> out(16 * 4);
    writeSequence(block, 0, 16);

    // Everything above this line was allowed to allocate; nothing below it is.
    const int before = g_allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i)
    {
        CHECK_EQ(buffer.push(block.data(), 16), static_cast<uint32_t>(16));
        CHECK_EQ(buffer.pop(out.data(), 16), static_cast<uint32_t>(16));
    }
    // Also the full/empty paths, which take a different branch.
    for (int i = 0; i < 8; ++i)
        buffer.push(block.data(), 16);
    for (int i = 0; i < 8; ++i)
        buffer.pop(out.data(), 16);

    CHECK_EQ(g_allocations.load(std::memory_order_relaxed), before);
}

MULTIROOM_TEST(ring_buffer_carries_a_stream_between_two_threads)
{
    AudioRingBuffer buffer;
    std::string error;
    // Deliberately tiny: at 64 frames a stream of a quarter million wraps
    // thousands of times, and both ends spend most of their time at a boundary.
    CHECK(buffer.configure(4, 64, error));

    const uint32_t kTotalFrames = 250000;
    const uint32_t kBlockFrames = 24; // not a divisor of the capacity, on purpose

    std::atomic<uint32_t> produced(0);
    std::thread producer([&buffer, &produced] {
        std::vector<unsigned char> block(kBlockFrames * 4);
        uint32_t next = 0;
        while (next < kTotalFrames)
        {
            const uint32_t count =
                kTotalFrames - next < kBlockFrames ? kTotalFrames - next : kBlockFrames;
            writeSequence(block, next, count);

            uint32_t offset = 0;
            while (offset < count)
            {
                const uint32_t pushed =
                    buffer.push(block.data() + static_cast<size_t>(offset) * 4, count - offset);
                if (pushed == 0)
                {
                    std::this_thread::yield();
                    continue;
                }
                offset += pushed;
            }
            next += count;
            produced.store(next, std::memory_order_relaxed);
        }
    });

    std::vector<unsigned char> out(kBlockFrames * 4);
    uint32_t expected = 0;
    bool inOrder = true;
    while (expected < kTotalFrames)
    {
        const uint32_t popped = buffer.pop(out.data(), kBlockFrames);
        if (popped == 0)
        {
            std::this_thread::yield();
            continue;
        }
        for (uint32_t i = 0; i < popped; ++i)
        {
            if (frameAt(out, i) != expected + i)
                inOrder = false;
        }
        expected += popped;
    }

    producer.join();

    // Every frame, exactly once, in order: a lost or duplicated frame is a
    // click, and a reordered one is worse.
    CHECK(inOrder);
    CHECK_EQ(expected, kTotalFrames);
    CHECK_EQ(produced.load(), kTotalFrames);
    CHECK_EQ(buffer.readableFrames(), static_cast<uint32_t>(0));
}

MULTIROOM_TEST(ring_buffer_source_sizes_itself_from_the_granted_format)
{
    AudioRingBuffer buffer;
    RingBufferSource source(buffer, 100); // 100 ms

    source.prepare(makeFormat(48000, 2, AudioFormat::SampleType::Int16));

    // 4800 frames of 4 bytes, rounded up to the next power of two.
    CHECK(buffer.isConfigured());
    CHECK_EQ(buffer.bytesPerFrame(), static_cast<uint32_t>(4));
    CHECK_EQ(buffer.capacityFrames(), static_cast<uint32_t>(8192));
    CHECK_EQ(source.bufferedFrames(), static_cast<uint32_t>(0));

    // An invalid format must leave the buffer unusable rather than sized from
    // a zero sample rate.
    AudioRingBuffer second;
    RingBufferSource unprepared(second, 100);
    unprepared.prepare(AudioFormat());
    CHECK(!second.isConfigured());
    unsigned char frame[8];
    CHECK_EQ(unprepared.render(frame, 1), static_cast<uint32_t>(0));
}

MULTIROOM_TEST(ring_buffer_source_reports_the_frames_it_could_not_deliver)
{
    AudioRingBuffer buffer;
    RingBufferSource source(buffer, 20);
    source.prepare(makeFormat(48000, 1, AudioFormat::SampleType::Int32));
    CHECK(buffer.isConfigured());

    std::vector<unsigned char> block(100 * 4);
    std::vector<unsigned char> out(256 * 4);
    writeSequence(block, 1, 100);
    CHECK_EQ(buffer.push(block.data(), 100), static_cast<uint32_t>(100));
    CHECK_EQ(source.bufferedFrames(), static_cast<uint32_t>(100));

    // A short render is the underrun: the backend silences the rest of the
    // period, and the source says how much was missing.
    CHECK_EQ(source.render(out.data(), 256), static_cast<uint32_t>(100));
    CHECK_EQ(source.renderedFrames(), static_cast<uint32_t>(100));
    CHECK_EQ(source.underrunFrames(), static_cast<uint32_t>(156));
    CHECK_EQ(frameAt(out, 0), static_cast<uint32_t>(1));
    CHECK_EQ(frameAt(out, 99), static_cast<uint32_t>(100));

    // An empty buffer is the same case with nothing delivered at all.
    CHECK_EQ(source.render(out.data(), 64), static_cast<uint32_t>(0));
    CHECK_EQ(source.underrunFrames(), static_cast<uint32_t>(220));

    // prepare() starts the counters over, because it is the start of a stream.
    source.prepare(makeFormat(48000, 1, AudioFormat::SampleType::Int32));
    CHECK_EQ(source.renderedFrames(), static_cast<uint32_t>(0));
    CHECK_EQ(source.underrunFrames(), static_cast<uint32_t>(0));
}
