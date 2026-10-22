#include "StubBackend.h"
#include "TestSupport.h"

#include "audio/AudioEngine.h"
#include "audio/AudioShutdown.h"
#include "audio/AudioRingBuffer.h"
#include "audio/RingBufferSource.h"
#include "audio/ToneGenerator.h"
#include "util/Wrap32.h"

#include <cstring>
#include <cmath>
#include <thread>
#include <vector>

using namespace multiroom;
using multiroomtest::StubBackend;
using multiroomtest::StubCallCounts;

namespace {

AudioFormat makeFormat(uint32_t rate, uint16_t channels, AudioFormat::SampleType type)
{
    AudioFormat format;
    format.sampleRate = rate;
    format.channels = channels;
    format.sampleType = type;
    return format;
}

int32_t readInt32(const unsigned char* bytes, size_t sampleIndex)
{
    int32_t value = 0;
    std::memcpy(&value, bytes + sampleIndex * 4, 4);
    return value;
}

int16_t readInt16(const unsigned char* bytes, size_t sampleIndex)
{
    int16_t value = 0;
    std::memcpy(&value, bytes + sampleIndex * 2, 2);
    return value;
}

float readFloat(const unsigned char* bytes, size_t sampleIndex)
{
    float value = 0.0f;
    std::memcpy(&value, bytes + sampleIndex * 4, 4);
    return value;
}

AudioEngine::Settings defaultSettings()
{
    AudioEngine::Settings settings;
    settings.deviceId = "stub";
    settings.format = makeFormat(48000, 2, AudioFormat::SampleType::Int16);
    settings.accessMode = AudioAccessMode::Exclusive;
    return settings;
}

} // namespace

MULTIROOM_TEST(audio_shutdown_fades_all_sample_formats_then_supplies_only_silence)
{
    for (auto type : {AudioFormat::SampleType::Int16, AudioFormat::SampleType::Int24,
                      AudioFormat::SampleType::Int24In32, AudioFormat::SampleType::Int32,
                      AudioFormat::SampleType::Float32})
    {
        struct Source
        {
            AudioFormat format;
            unsigned calls = 0;
            static uint32_t render(void* context, void* target, uint32_t frames)
            {
                auto& self = *static_cast<Source*>(context);
                ++self.calls;
                auto* bytes = static_cast<unsigned char*>(target);
                for (uint32_t i = 0; i < frames * self.format.channels; ++i)
                {
                    int32_t sample = 9000;
                    if (self.format.sampleType == AudioFormat::SampleType::Float32)
                    {
                        const float value = 0.9f;
                        std::memcpy(bytes, &value, 4);
                    }
                    else
                    {
#if defined(_WIN32) || defined(ESP_PLATFORM)
                        if (self.format.sampleType == AudioFormat::SampleType::Int24In32)
                            sample *= 256;
#endif
                        std::memcpy(bytes, &sample, self.format.bytesPerSample());
                    }
                    bytes += self.format.bytesPerSample();
                }
                return frames;
            }
        } source{{1000, 3, type}};
        AudioShutdown shutdown;
        shutdown.prepare(source.format);
        std::vector<unsigned char> buffer(4 * source.format.bytesPerFrame());
        CHECK_EQ(shutdown.render(Source::render, &source, buffer.data(), 4), 4u);
        CHECK(!shutdown.active());
        shutdown.begin(true);
        for (unsigned offset = 0; offset < 24; offset += 4)
        {
            CHECK_EQ(shutdown.render(Source::render, &source, buffer.data(), 4), 4u);
            for (unsigned frame = 0; frame < 4; ++frame)
            {
                const unsigned position = offset + frame;
                for (unsigned channel = 0; channel < source.format.channels; ++channel)
                {
                    const auto* sample = buffer.data() +
                        (frame * source.format.channels + channel) * source.format.bytesPerSample();
                    const double gain = position < 10 ? (9.0 - position) / 9.0 : 0.0;
                    if (type == AudioFormat::SampleType::Float32)
                        CHECK(std::fabs(readFloat(sample, 0) - 0.9 * gain) < 0.00001);
                    else
                    {
                        int32_t value = 0;
                        std::memcpy(&value, sample, source.format.bytesPerSample());
#if defined(_WIN32) || defined(ESP_PLATFORM)
                        if (type == AudioFormat::SampleType::Int24In32)
                        {
                            CHECK_EQ(value & 255, 0);
                            value /= 256;
                        }
#endif
                        CHECK(std::abs(value - static_cast<int32_t>(9000 * gain)) <= 1);
                    }
                }
            }
            CHECK(shutdown.finished() == (offset + 4 >= 20));
        }
        CHECK_EQ(source.calls, 4u); // Normal buffer, then three fade fragments.
        CHECK(shutdown.remainingMs() > 0);
        shutdown.prepare(source.format);
        CHECK(!shutdown.finished());
        CHECK(!shutdown.active());
        shutdown.render(Source::render, &source, buffer.data(), 4);
        CHECK_EQ(source.calls, 5u); // A restart gets fresh, unattenuated audio.
    }
}

MULTIROOM_TEST(audio_shutdown_silences_a_short_source_without_reporting_an_underrun)
{
    AudioShutdown shutdown;
    shutdown.prepare({1000, 2, AudioFormat::SampleType::Int16});
    shutdown.begin(true);
    unsigned char buffer[80];
    std::memset(buffer, 0xff, sizeof(buffer));
    const auto empty = [](void*, void*, uint32_t) -> uint32_t { return 0; };
    CHECK_EQ(shutdown.render(empty, nullptr, buffer, 20), 20u);
    CHECK(shutdown.finished());
    for (auto byte : buffer)
        CHECK_EQ(byte, 0);
}

MULTIROOM_TEST(engine_open_start_stop_close_cycle)
{
    std::unique_ptr<StubBackend> owned(new StubBackend());
    StubBackend* stub = owned.get();
    AudioEngine engine(std::move(owned));

    ToneGenerator tone;
    engine.setSource(&tone);

    std::string error;
    CHECK(engine.open(defaultSettings(), error));
    CHECK(engine.isOpen());
    CHECK(!engine.isRunning());

    CHECK(engine.start(error));
    CHECK(engine.isRunning());

    // Starting twice must be a no-op rather than a second backend start.
    CHECK(engine.start(error));
    CHECK_EQ(stub->startCalls, 1);

    engine.stop();
    CHECK(engine.isOpen());
    CHECK(!engine.isRunning());

    engine.close();
    CHECK(!engine.isOpen());
    CHECK_EQ(stub->closeCalls, 1);

    // Closing twice must not reach the backend again.
    engine.close();
    CHECK_EQ(stub->closeCalls, 1);
}

MULTIROOM_TEST(engine_passes_access_mode_and_device_to_the_backend)
{
    std::unique_ptr<StubBackend> owned(new StubBackend());
    StubBackend* stub = owned.get();
    AudioEngine engine(std::move(owned));

    ToneGenerator tone;
    engine.setSource(&tone);

    AudioEngine::Settings settings = defaultSettings();
    settings.accessMode = AudioAccessMode::Shared;
    settings.deviceId = "hw:CARD=USB,DEV=0";
    settings.bufferFrames = 256;

    std::string error;
    CHECK(engine.open(settings, error));

    // The backend must be told exactly what was asked for: silently switching
    // share mode would cost clock accuracy the caller may have forbidden.
    CHECK(stub->lastConfig.accessMode == AudioAccessMode::Shared);
    CHECK_EQ(stub->lastConfig.deviceId, std::string("hw:CARD=USB,DEV=0"));
    CHECK_EQ(stub->lastConfig.requestedBufferFrames, static_cast<uint32_t>(256));
    CHECK(stub->lastConfig.renderCallback != nullptr);
}

MULTIROOM_TEST(engine_prepares_the_source_with_the_granted_format)
{
    std::unique_ptr<StubBackend> owned(new StubBackend());
    StubBackend* stub = owned.get();
    stub->grantedSampleRate = 44100; // driver overrides the request
    AudioEngine engine(std::move(owned));

    ToneGenerator tone;
    engine.setSource(&tone);

    std::string error;
    CHECK(engine.open(defaultSettings(), error));
    CHECK_EQ(engine.format().sampleRate, static_cast<uint32_t>(44100));

    // The source must have been prepared, so rendering produces frames.
    std::vector<unsigned char> buffer(64 * 2 * 2, 0xEE);
    CHECK_EQ(stub->render(buffer.data(), 64), static_cast<uint32_t>(64));
    CHECK_EQ(tone.renderedFrames(), static_cast<uint32_t>(64));
}

MULTIROOM_TEST(engine_rejects_bad_setup)
{
    std::string error;

    {
        std::unique_ptr<StubBackend> owned(new StubBackend());
        AudioEngine engine(std::move(owned));
        // No source set.
        CHECK(!engine.open(defaultSettings(), error));
        CHECK(!error.empty());
    }
    {
        std::unique_ptr<StubBackend> owned(new StubBackend());
        AudioEngine engine(std::move(owned));
        ToneGenerator tone;
        engine.setSource(&tone);
        AudioEngine::Settings settings = defaultSettings();
        settings.format = AudioFormat(); // invalid
        CHECK(!engine.open(settings, error));
    }
    {
        std::unique_ptr<StubBackend> owned(new StubBackend());
        owned->failOpen = true;
        AudioEngine engine(std::move(owned));
        ToneGenerator tone;
        engine.setSource(&tone);
        CHECK(!engine.open(defaultSettings(), error));
        CHECK_EQ(error, std::string("stub refuses to open"));
        CHECK(!engine.isOpen());
    }
    {
        std::unique_ptr<StubBackend> owned(new StubBackend());
        owned->failStart = true;
        AudioEngine engine(std::move(owned));
        ToneGenerator tone;
        engine.setSource(&tone);
        CHECK(engine.open(defaultSettings(), error));
        CHECK(!engine.start(error));
        CHECK_EQ(error, std::string("stub refuses to start"));
        CHECK(!engine.isRunning());
    }
    {
        // Starting a device that was never opened must fail, not crash.
        std::unique_ptr<StubBackend> owned(new StubBackend());
        AudioEngine engine(std::move(owned));
        ToneGenerator tone;
        engine.setSource(&tone);
        CHECK(!engine.start(error));
    }
}

MULTIROOM_TEST(engine_destructor_closes_a_running_device)
{
    // The engine owns the stub and destroys it, so the counters are read from
    // the caller's own copy rather than from the freed object.
    StubCallCounts counts;
    {
        std::unique_ptr<StubBackend> owned(new StubBackend());
        owned->counts = &counts;
        AudioEngine engine(std::move(owned));
        ToneGenerator tone;
        engine.setSource(&tone);

        std::string error;
        CHECK(engine.open(defaultSettings(), error));
        CHECK(engine.start(error));
        // Leaving the scope must stop and close; a leaked render thread would
        // outlive the source it reads from.
    }
    CHECK_EQ(counts.stopCalls >= 1, true);
    CHECK_EQ(counts.closeCalls, 1);
}

MULTIROOM_TEST(engine_plays_what_another_thread_pushes_into_the_ring_buffer)
{
    // A frame here is one uint32 -- two int16 channels -- so every frame
    // carries its own sequence number and a gap or a reordering is visible.
    AudioRingBuffer ring;
    RingBufferSource source(ring, 50);

    std::unique_ptr<StubBackend> owned(new StubBackend());
    StubBackend* stub = owned.get();
    AudioEngine engine(std::move(owned));
    engine.setSource(&source);

    std::string error;
    CHECK(engine.open(defaultSettings(), error));
    CHECK(engine.start(error));
    CHECK(ring.isConfigured());
    CHECK_EQ(ring.bytesPerFrame(), static_cast<uint32_t>(4));

    const uint32_t kTotalFrames = 48000; // one second of 48 kHz stereo int16
    const uint32_t kProducerBlock = 240;
    const uint32_t kPeriodFrames = 480;

    std::thread producer([&ring] {
        std::vector<unsigned char> block(kProducerBlock * 4);
        uint32_t next = 0;
        while (next < kTotalFrames)
        {
            const uint32_t count =
                kTotalFrames - next < kProducerBlock ? kTotalFrames - next : kProducerBlock;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t value = next + i;
                std::memcpy(block.data() + static_cast<size_t>(i) * 4, &value, 4);
            }

            uint32_t offset = 0;
            while (offset < count)
            {
                const uint32_t pushed =
                    ring.push(block.data() + static_cast<size_t>(offset) * 4, count - offset);
                if (pushed == 0)
                {
                    std::this_thread::yield();
                    continue;
                }
                offset += pushed;
            }
            next += count;
        }
    });

    // This thread stands in for the audio thread: it asks for a period at a
    // time through the engine's render callback and never waits for the
    // producer.
    std::vector<unsigned char> period(kPeriodFrames * 4);
    uint32_t received = 0;
    uint32_t requested = 0;
    bool inOrder = true;

    while (received < kTotalFrames)
    {
        const uint32_t filled = stub->render(period.data(), kPeriodFrames);
        requested += kPeriodFrames;
        for (uint32_t i = 0; i < filled; ++i)
        {
            uint32_t value = 0;
            std::memcpy(&value, period.data() + static_cast<size_t>(i) * 4, 4);
            if (value != received + i)
                inOrder = false;
        }
        received += filled;
        if (filled < kPeriodFrames)
            std::this_thread::yield(); // an empty buffer is silence, not a wait
    }

    producer.join();

    CHECK(inOrder);
    CHECK_EQ(received, kTotalFrames);
    CHECK_EQ(source.renderedFrames(), kTotalFrames);
    // Every frame the device asked for and did not get is counted, rather than
    // quietly turning into silence nobody hears about.
    CHECK_EQ(source.underrunFrames(), requested - received);

    engine.stop();
    engine.close();
}

MULTIROOM_TEST(tone_generator_writes_every_channel)
{
    ToneGenerator tone(1000.0, 0.5);
    tone.prepare(makeFormat(48000, 6, AudioFormat::SampleType::Int16));

    std::vector<unsigned char> buffer(16 * 6 * 2, 0xAA);
    CHECK_EQ(tone.render(buffer.data(), 16), static_cast<uint32_t>(16));

    // Same signal on every channel of a frame.
    const unsigned char* bytes = buffer.data();
    for (size_t frame = 0; frame < 16; ++frame)
    {
        const int16_t first = readInt16(bytes, frame * 6);
        for (size_t channel = 1; channel < 6; ++channel)
            CHECK_EQ(readInt16(bytes, frame * 6 + channel), first);
    }
}

MULTIROOM_TEST(tone_generator_starts_at_zero_and_swings_both_ways)
{
    ToneGenerator tone(1000.0, 1.0);
    tone.prepare(makeFormat(48000, 1, AudioFormat::SampleType::Float32));

    // One full period of a 1 kHz tone at 48 kHz is 48 frames.
    std::vector<unsigned char> buffer(48 * 4, 0);
    CHECK_EQ(tone.render(buffer.data(), 48), static_cast<uint32_t>(48));

    const unsigned char* bytes = buffer.data();
    CHECK_NEAR(readFloat(bytes, 0), 0.0, 1e-6);

    float lowest = 0.0f;
    float highest = 0.0f;
    for (size_t i = 0; i < 48; ++i)
    {
        const float value = readFloat(bytes, i);
        if (value < lowest)
            lowest = value;
        if (value > highest)
            highest = value;
    }
    CHECK_NEAR(highest, 1.0, 0.01);
    CHECK_NEAR(lowest, -1.0, 0.01);
}

MULTIROOM_TEST(sample_conversion_covers_every_format)
{
    unsigned char buffer[16];

    // Full scale positive in each format.
    std::memset(buffer, 0, sizeof(buffer));
    writeSample(buffer, 0, 1.0f, AudioFormat::SampleType::Int16);
    CHECK_EQ(readInt16(buffer, 0), static_cast<int16_t>(32767));

    std::memset(buffer, 0, sizeof(buffer));
    writeSample(buffer, 0, -1.0f, AudioFormat::SampleType::Int16);
    CHECK_EQ(readInt16(buffer, 0), static_cast<int16_t>(-32767));

    std::memset(buffer, 0, sizeof(buffer));
    writeSample(buffer, 0, 1.0f, AudioFormat::SampleType::Float32);
    CHECK_NEAR(readFloat(buffer, 0), 1.0, 1e-6);

    // Packed 24-bit is three little-endian bytes with no container.
    std::memset(buffer, 0, sizeof(buffer));
    writeSample(buffer, 0, 1.0f, AudioFormat::SampleType::Int24);
    CHECK_EQ(static_cast<int>(buffer[0]), 0xFF);
    CHECK_EQ(static_cast<int>(buffer[1]), 0xFF);
    CHECK_EQ(static_cast<int>(buffer[2]), 0x7F);
    // The fourth byte must be untouched: the sample is three bytes wide.
    CHECK_EQ(static_cast<int>(buffer[3]), 0x00);

    // Int32 carries the 24-bit value shifted up, so the low byte is zero.
    std::memset(buffer, 0, sizeof(buffer));
    writeSample(buffer, 0, 1.0f, AudioFormat::SampleType::Int32);
    CHECK_EQ(readInt32(buffer, 0), static_cast<int32_t>(8388607) * 256);

    // 24-in-32 alignment differs between the platforms, and getting it wrong
    // costs 48 dB rather than producing obvious noise.
    std::memset(buffer, 0, sizeof(buffer));
    writeSample(buffer, 0, 1.0f, AudioFormat::SampleType::Int24In32);
#if defined(_WIN32)
    CHECK_EQ(readInt32(buffer, 0), static_cast<int32_t>(8388607) * 256);
#else
    CHECK_EQ(readInt32(buffer, 0), static_cast<int32_t>(8388607));
#endif
}

MULTIROOM_TEST(sample_conversion_clamps_out_of_range_input)
{
    unsigned char buffer[8];

    std::memset(buffer, 0, sizeof(buffer));
    writeSample(buffer, 0, 4.0f, AudioFormat::SampleType::Int16);
    CHECK_EQ(readInt16(buffer, 0), static_cast<int16_t>(32767));

    std::memset(buffer, 0, sizeof(buffer));
    writeSample(buffer, 0, -4.0f, AudioFormat::SampleType::Int16);
    CHECK_EQ(readInt16(buffer, 0), static_cast<int16_t>(-32768));

    std::memset(buffer, 0, sizeof(buffer));
    writeSample(buffer, 0, 9.0f, AudioFormat::SampleType::Float32);
    CHECK_NEAR(readFloat(buffer, 0), 1.0, 1e-6);
}

MULTIROOM_TEST(tone_generator_renders_nothing_without_preparation)
{
    ToneGenerator tone;
    unsigned char buffer[64];
    // No prepare() call: the format is unknown, so rendering must be refused
    // rather than writing garbage into a device buffer.
    CHECK_EQ(tone.render(buffer, 8), static_cast<uint32_t>(0));
    CHECK_EQ(tone.render(nullptr, 8), static_cast<uint32_t>(0));
}

MULTIROOM_TEST(wrap32_extends_a_wrapping_counter)
{
    Wrap32Extender extender;

    CHECK_EQ(extender.extend(0), static_cast<uint64_t>(0));
    CHECK_EQ(extender.extend(1000), static_cast<uint64_t>(1000));
    CHECK_EQ(extender.extend(4000000000u), static_cast<uint64_t>(4000000000u));

    // One wrap: the low word goes backwards, the high word must step up.
    CHECK_EQ(extender.extend(5u), static_cast<uint64_t>(0x100000000ULL) + 5u);
    CHECK_EQ(extender.extend(1000u), static_cast<uint64_t>(0x100000000ULL) + 1000u);

    // A second wrap.
    CHECK_EQ(extender.extend(4294967295u), static_cast<uint64_t>(0x100000000ULL) + 4294967295u);
    CHECK_EQ(extender.extend(0u), static_cast<uint64_t>(0x200000000ULL));
}

MULTIROOM_TEST(wrap32_starts_anywhere_and_resets)
{
    Wrap32Extender extender;

    // The first value must be taken as-is, not treated as a wrap even when it
    // is large: a counter may be read for the first time at any point.
    CHECK_EQ(extender.extend(3000000000u), static_cast<uint64_t>(3000000000u));
    CHECK_EQ(extender.extend(3000000001u), static_cast<uint64_t>(3000000001u));

    extender.reset();
    CHECK_EQ(extender.extend(7u), static_cast<uint64_t>(7));
}

MULTIROOM_TEST(wrap32_survives_many_wraps)
{
    Wrap32Extender extender;

    // Step in chunks that cross the boundary repeatedly; the widened value
    // must stay monotonic throughout.
    const uint32_t step = 1000000000u;
    uint32_t low = 0;
    uint64_t previous = extender.extend(low);

    for (int i = 0; i < 200; ++i)
    {
        low += step; // wraps on its own, which is exactly the case under test
        const uint64_t widened = extender.extend(low);
        CHECK(widened > previous);
        CHECK_EQ(widened - previous, static_cast<uint64_t>(step));
        previous = widened;
    }
}

MULTIROOM_TEST(clock_snapshot_validity_rules)
{
    AudioClockSnapshot snapshot;
    // A default snapshot must never look usable.
    CHECK(!snapshot.isValid());

    snapshot.quality = AudioClockQuality::Hardware;
    snapshot.nominalSampleRate = 48000;
    CHECK(!snapshot.isValid()); // still no timestamp

    snapshot.monotonicTimeNs = 1;
    CHECK(snapshot.isValid());

    // A position without a rate cannot be turned into ppm, so it is not valid.
    snapshot.nominalSampleRate = 0;
    CHECK(!snapshot.isValid());

    snapshot.nominalSampleRate = 48000;
    snapshot.quality = AudioClockQuality::Unavailable;
    CHECK(!snapshot.isValid());

    CHECK_EQ(std::string(audioClockQualityName(AudioClockQuality::Hardware)),
             std::string("hardware"));
    CHECK_EQ(std::string(audioClockQualityName(AudioClockQuality::Driver)),
             std::string("driver"));
    CHECK_EQ(std::string(audioClockQualityName(AudioClockQuality::Estimated)),
             std::string("estimated"));
}
