// One room's audio path, and the rule the whole architecture is shaped around:
// one client can never block another.
//
// Nothing here opens a socket. The sink is an interface precisely so that "a
// room that stops reading" is a test fixture rather than a race against the
// operating system's send buffer -- a peer whose window is shut takes nothing,
// which is exactly what a non-blocking send reports. The one test that needs
// two real threads runs them and waits on an event with a deadline, the way
// test_control_server.cpp does; everything else drives pump() by hand with
// multiroomtest::ManualClock and never sleeps.

#include "ManualClock.h"
#include "TestSupport.h"

#include "audio/BlockPool.h"
#include "audio/NativePcmStore.h"
#include "client/ClientOutput.h"
#include "matp/AudioPacket.h"
#include "processing/IClientAudioProcessor.h"
#include "resampler/FakeResampler.h"
#include "resampler/ResamplerRegistry.h"
#include "timeline/SourceTimeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace multiroom;

namespace {

constexpr uint32_t kRate = 44100;
constexpr uint32_t kPacketFrames = 882; // 20 ms
constexpr uint64_t kPacketNs = 20000000ULL;

// How long an assertion about another thread is given before it is a failure.
// It bounds a hang; it measures nothing.
constexpr auto kDeadline = std::chrono::seconds(5);

AudioFormat formatOf(AudioFormat::SampleType type = AudioFormat::SampleType::Int16,
                     uint32_t rate = kRate)
{
    AudioFormat format;
    format.sampleRate = rate;
    format.channels = 2;
    format.sampleType = type;
    return format;
}

SessionId testSessionId()
{
    return SessionId(0x0123456789abcdefULL, 0xfedcba9876543210ULL);
}

// What the sink saw, kept apart from the sink itself: a failed output destroys
// the sink it was given, and a test that held a raw pointer to it would be
// reading freed memory to find out why.
struct SinkState
{
    std::mutex mutex;
    std::string received;

    std::atomic<bool> accepting{true};
    std::atomic<bool> closed{false};
    std::atomic<uint64_t> refusals{0};
};

class FakeSink final : public IPacketSink
{
public:
    explicit FakeSink(std::shared_ptr<SinkState> state) : state_(std::move(state)) {}

    Result<size_t, PacketSinkError> write(std::span<const std::byte> bytes) override
    {
        if (state_->closed.load())
            return fail(PacketSinkError::Closed, "this sink is closed");
        if (!state_->accepting.load())
        {
            // A peer whose receive window is shut. Zero bytes taken, which is
            // not a failure and is the normal case for a full prebuffer.
            state_->refusals.fetch_add(1);
            return size_t{0};
        }

        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return bytes.size();
    }

    void close() override { state_->closed.store(true); }

private:
    std::shared_ptr<SinkState> state_;
};

// A zone, reduced to the two things an output reads: a store of decoded PCM
// and the segment table that says when each frame of it is heard.
struct FakeZone
{
    explicit FakeZone(AudioFormat sourceFormat = formatOf())
        : format(sourceFormat), timeline(), pool(nullptr), store(nullptr)
    {
        auto created = BlockPool::create(
            static_cast<size_t>(kPacketFrames) * format.bytesPerFrame(), 512);
        CHECK_OK(created);
        pool = std::move(*created);

        // Far more retention than the real five seconds, because these tests
        // publish a zone's worth of audio before a room reads any of it and
        // eviction is a case of its own below.
        auto opened = NativePcmStore::create(600ULL * kNanosecondsPerSecond, 4096);
        CHECK_OK(opened);
        store = std::move(*opened);

        CHECK_OK(timeline.stage(SourceFrame{0}, format.sampleRate));
    }

    // One 20 ms block of a ramp whose every byte is decided here, so a packet
    // that carries the wrong frames is caught by its contents.
    void publish(uint32_t frames = kPacketFrames)
    {
        auto handle = pool->acquire();
        CHECK_OK(handle);

        const std::span<std::byte> writable = (*handle)->writable();
        const size_t bytes = static_cast<size_t>(frames) * format.bytesPerFrame();
        for (size_t i = 0; i < bytes; ++i)
            writable[i] = static_cast<std::byte>((published * 7 + i) & 0xFF);

        CHECK_OK((*handle)->commit(format, SourceFrame{published}, frames));
        CHECK_OK(store->publish(std::move(*handle)));
        published += frames;
    }

    void publishBlocks(size_t count)
    {
        for (size_t i = 0; i < count; ++i)
            publish();
    }

    AudioFormat format;
    SourceTimeline timeline;
    std::unique_ptr<BlockPool> pool;
    std::unique_ptr<NativePcmStore> store;
    uint64_t published = 0;
};

OutputPlan bitperfectPlan(const AudioFormat& source, const AudioFormat& output)
{
    OutputPlan plan;
    plan.mode = OutputMode::BitPerfect;
    plan.sourceFormat = source;
    plan.outputFormat = output;
    plan.reason = source.sampleType == output.sampleType ? PlanReason::SourceFormatOffered
                                                         : PlanReason::SourceFormatRepacked;
    return plan;
}

OutputPlan resampledPlan(const AudioFormat& source, const AudioFormat& output)
{
    OutputPlan plan;
    plan.mode = OutputMode::Resampled;
    plan.sourceFormat = source;
    plan.outputFormat = output;
    plan.resamplerId = "fake";
    plan.reason = PlanReason::FormatNotOffered;
    return plan;
}

// The same room, planned for a correction. The drift flag is what builds a
// converter that can move at all, and what every refusal below is measured
// against.
OutputPlan correctedPlan(const AudioFormat& source, const AudioFormat& output)
{
    OutputPlan plan = resampledPlan(source, output);
    plan.driftActuator = DriftActuator::ServerResampling;
    plan.reason = PlanReason::Grouped;
    return plan;
}

// FakeResampler is never registered by the server's own startup -- a room
// served by it would be an audible mistake -- so a test that wants one adds it
// itself. A bitperfect plan never reaches the registry at all, which is what
// makes one helper enough for both kinds of room.
const ResamplerRegistry& testRegistry()
{
    static const ResamplerRegistry& registry = [] () -> const ResamplerRegistry& {
        static ResamplerRegistry built;
        CHECK_OK(built.add(std::make_unique<FakeResamplerFactory>()));
        return built;
    }();
    return registry;
}

std::unique_ptr<IClientAudioProcessor> processorFor(const OutputPlan& plan)
{
    auto processor = makeClientAudioProcessor(plan, testRegistry());
    CHECK_OK(processor);
    return std::move(*processor);
}

// A converter that never moves, which is the one thing the carry below must
// not go on feeding forever.
class StuckProcessor final : public IClientAudioProcessor
{
public:
    explicit StuckProcessor(OutputPlan plan) : plan_(std::move(plan)) {}

    const OutputPlan& plan() const noexcept override { return plan_; }
    double ratio() const noexcept override { return 1.0; }

    Status<ProcessorError> setRatio(double) override
    {
        return fail(ProcessorError::RatioNotAdjustable, "not this room");
    }

    Result<ProcessResult, ProcessorError> process(std::span<const std::byte>,
                                                  std::span<std::byte>) override
    {
        return ProcessResult{0, 0};
    }

    void reset() override {}
    double latencyFrames() const noexcept override { return 0.0; }

private:
    OutputPlan plan_;
};

// A processor that copies frames rather than converting them, and remembers
// every ratio it was handed.
//
// What is under test wherever one of these appears is the number that reaches
// setRatio and whether it arrives at all; the samples are phase 7's own tests
// and are not re-run here. It therefore wants two formats of the same frame
// size, which every plan below gives it.
class CountingProcessor final : public IClientAudioProcessor
{
public:
    explicit CountingProcessor(OutputPlan plan, bool refuse = false)
        : plan_(std::move(plan)), refuse_(refuse),
          frameBytes_(plan_.outputFormat.bytesPerFrame())
    {
        CHECK_EQ(plan_.sourceFormat.bytesPerFrame(), plan_.outputFormat.bytesPerFrame());
    }

    const OutputPlan& plan() const noexcept override { return plan_; }
    double ratio() const noexcept override { return ratio_; }

    Status<ProcessorError> setRatio(double ratio) override
    {
        ++calls_;
        lastRatio_ = ratio;
        if (refuse_)
        {
            return fail(ProcessorError::ResamplerFailed,
                        "this converter's window does not reach that ratio");
        }
        ratio_ = ratio;
        return {};
    }

    Result<ProcessResult, ProcessorError> process(std::span<const std::byte> input,
                                                  std::span<std::byte> output) override
    {
        const size_t frames = std::min(input.size(), output.size()) / frameBytes_;
        if (frames > 0)
            std::memcpy(output.data(), input.data(), frames * frameBytes_);
        return ProcessResult{frames, frames};
    }

    void reset() override {}
    double latencyFrames() const noexcept override { return 0.0; }

    [[nodiscard]] uint64_t calls() const { return calls_; }
    [[nodiscard]] double lastRatio() const { return lastRatio_; }

private:
    OutputPlan plan_;
    bool refuse_;
    size_t frameBytes_;
    double ratio_ = 1.0;
    double lastRatio_ = 0.0;
    uint64_t calls_ = 0;
};

// What is left of a 1000-frame source after one whole packet.
constexpr uint32_t kTailFrames = 118;

// A converter that copies, and runs `onTail` once from inside its call for
// the last kTailFrames of the source. With `lookahead` it keeps back the last
// frame it is shown, as an interpolating converter does, so a single frame
// gets nothing until there is a frame (or silence) behind it.
class TailHookProcessor final : public IClientAudioProcessor
{
public:
    TailHookProcessor(OutputPlan plan, std::function<void()> onTail, bool lookahead)
        : plan_(std::move(plan)), frameBytes_(plan_.outputFormat.bytesPerFrame()),
          onTail_(std::move(onTail)), lookahead_(lookahead)
    {
    }

    const OutputPlan& plan() const noexcept override { return plan_; }
    double ratio() const noexcept override { return 1.0; }
    Status<ProcessorError> setRatio(double) override { return {}; }

    Result<ProcessResult, ProcessorError> process(std::span<const std::byte> input,
                                                  std::span<std::byte> output) override
    {
        const size_t shown = input.size() / frameBytes_;
        const size_t usable = lookahead_ && shown > 0 ? shown - 1 : shown;
        const size_t frames = std::min(usable, output.size() / frameBytes_);
        if (frames > 0)
            std::memcpy(output.data(), input.data(), frames * frameBytes_);
        if (shown == kTailFrames && onTail_)
            std::exchange(onTail_, nullptr)();
        return ProcessResult{frames, frames};
    }

    void reset() override {}
    double latencyFrames() const noexcept override { return 0.0; }

private:
    OutputPlan plan_;
    size_t frameBytes_;
    std::function<void()> onTail_;
    bool lookahead_;
};

std::unique_ptr<ClientOutput> makeOutput(FakeZone& zone, const IClock& clock,
                                         const OutputPlan& plan,
                                         ClientOutputSettings settings = {},
                                         const char* client = "livingroom")
{
    auto output = ClientOutput::create(testSessionId(), ClientId(client), *zone.store,
                                       zone.timeline, processorFor(plan), clock, settings);
    CHECK_OK(output);
    return std::move(*output);
}

// Every packet the sink has seen so far, decoded with the client's own reader.
std::vector<matp::AudioPacket> decode(SinkState& state, matp::AudioPacketReader& reader,
                                      size_t& consumed)
{
    std::string fresh;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        fresh = state.received.substr(consumed);
        consumed = state.received.size();
    }
    reader.append(fresh.data(), fresh.size());

    std::vector<matp::AudioPacket> packets;
    for (;;)
    {
        matp::AudioPacket packet;
        std::string error;
        const matp::AudioPacketReader::Result result = reader.next(packet, error);
        if (result == matp::AudioPacketReader::Result::Packet)
        {
            packets.push_back(packet);
            continue;
        }
        if (result == matp::AudioPacketReader::Result::Invalid)
            multiroomtest::failAt("the client refused this room's stream: " + error, __FILE__,
                            __LINE__);
        break;
    }
    return packets;
}

// What a room is sent of a 1000-frame source -- one whole packet, then the
// frames left over -- when `hook` runs from inside the converter's call for
// those last frames. That is after produce() has sized the second packet and
// before the read that reaches the frontier: the gap a zone's end or a hold
// can be published in. With `lookahead`, the converter's next call is the
// last frame alone, which it does not take: the read past the frontier then
// happens in extendCarry() rather than fill().
std::vector<matp::AudioPacket> buildWithTailHook(FakeZone& zone, const IClock& clock,
                                                 const std::function<void(ClientOutput&)>& hook,
                                                 bool lookahead = false)
{
    zone.publish();
    zone.publish(kTailFrames);

    ClientOutput* room = nullptr;
    auto created = ClientOutput::create(
        testSessionId(), ClientId("livingroom"), *zone.store, zone.timeline,
        std::make_unique<TailHookProcessor>(resampledPlan(zone.format, zone.format),
                                            [&] { hook(*room); }, lookahead),
        clock, {});
    CHECK_OK(created);
    room = created->get();

    auto state = std::make_shared<SinkState>();
    CHECK_OK(room->joinAt(TimelineNs{0}));
    room->attach(std::make_unique<FakeSink>(state));
    room->pump();
    room->pump();

    matp::AudioPacketReader reader;
    size_t consumed = 0;
    return decode(*state, reader, consumed);
}

template <typename Predicate>
bool waitFor(Predicate predicate)
{
    const auto until = std::chrono::steady_clock::now() + kDeadline;
    while (std::chrono::steady_clock::now() < until)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

} // namespace

MULTIROOM_TEST(client_output_refuses_a_room_it_could_not_serve)
{
    FakeZone zone;
    multiroomtest::ManualClock clock;
    const OutputPlan plan = bitperfectPlan(zone.format, zone.format);

    ClientOutputSettings zeroPacket;
    zeroPacket.packetDurationMs = 0;
    CHECK_ERR(ClientOutput::create(testSessionId(), ClientId("a"), *zone.store, zone.timeline,
                                   processorFor(plan), clock, zeroPacket),
              OutputError::InvalidSettings);

    // Section 13's all-zero id is "no session", and audio for it belongs
    // nowhere.
    CHECK_ERR(ClientOutput::create(SessionId(), ClientId("a"), *zone.store, zone.timeline,
                                   processorFor(plan), clock, {}),
              OutputError::InvalidSession);

    CHECK_ERR(ClientOutput::create(testSessionId(), ClientId("a"), *zone.store, zone.timeline,
                                   nullptr, clock, {}),
              OutputError::InvalidPlan);

    // A room cannot be started before it has been told where it starts.
    auto output = makeOutput(zone, clock, plan);
    CHECK(output->state() == OutputState::Idle);
    CHECK_ERR(output->run(), OutputError::NotJoined);
}

MULTIROOM_TEST(client_output_refuses_a_position_the_timeline_does_not_hold)
{
    // Nothing staged: there is no source and therefore no position, which is
    // an explicit failure rather than a room streaming from nowhere.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    SourceTimeline empty;

    auto output = ClientOutput::create(testSessionId(), ClientId("livingroom"), *zone.store,
                                       empty, processorFor(bitperfectPlan(zone.format,
                                                                          zone.format)),
                                       clock);
    CHECK_OK(output);
    CHECK_ERR((*output)->joinAt(TimelineNs{0}), OutputError::TimelineFailed);
}

MULTIROOM_TEST(client_output_sends_a_contiguous_stream_the_client_can_read)
{
    // Section 15 end to end: fifty packets of one room's stream, decoded by
    // the parser the real binary uses, each beginning exactly where the last
    // one ended and carrying the source's own bytes.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(50);

    auto state = std::make_shared<SinkState>();
    auto output = makeOutput(zone, clock, bitperfectPlan(zone.format, zone.format));
    CHECK(output->passThrough());

    CHECK_OK(output->joinAt(TimelineNs{0}));
    CHECK(output->state() == OutputState::Streaming);
    output->attach(std::make_unique<FakeSink>(state));
    output->pump();

    matp::AudioPacketReader reader;
    size_t consumed = 0;
    const std::vector<matp::AudioPacket> packets = decode(*state, reader, consumed);
    CHECK_EQ(packets.size(), size_t(50));

    uint64_t expectedNs = 0;
    for (size_t index = 0; index < packets.size(); ++index)
    {
        const matp::AudioPacket& packet = packets[index];
        CHECK_EQ(packet.header.sequenceNumber, uint64_t(index));
        CHECK_EQ(packet.header.timelineStartNs, expectedNs);
        CHECK_EQ(packet.header.timelineDurationNs, kPacketNs);
        CHECK_EQ(packet.header.outputFrameCount, kPacketFrames);
        CHECK(packet.header.format == zone.format);
        CHECK_EQ(packet.header.sessionIdHigh, testSessionId().high());
        CHECK(packet.header.headerCrc != 0);
        expectedNs = packet.header.timelineStartNs + packet.header.timelineDurationNs;

        // The bytes are the source's own, unaltered: this room is
        // pass-through, so nothing between the store and the socket touched
        // them.
        for (size_t i = 0; i < packet.payloadBytes; ++i)
        {
            const unsigned char expected =
                static_cast<unsigned char>((index * kPacketFrames * 7 + i) & 0xFF);
            if (packet.payload[i] != expected)
            {
                multiroomtest::failAt("packet " + std::to_string(index) + " byte " +
                                    std::to_string(i) + " is not the source's",
                                __FILE__, __LINE__);
            }
        }
    }

    CHECK_EQ(output->packets(), 50ULL);
    CHECK_EQ(output->frames(), 50ULL * kPacketFrames);
    CHECK_EQ(output->queuedPackets(), size_t(0));
}

MULTIROOM_TEST(client_output_repacks_a_room_whose_layout_differs)
{
    // Bitperfect is about values and not about bytes (section 2), so a room
    // that takes the same samples in a wider container is served by phase 7's
    // direct path rather than by the pass-through saving.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(4);

    const AudioFormat wide = formatOf(AudioFormat::SampleType::Int32);
    auto output = makeOutput(zone, clock, bitperfectPlan(zone.format, wide));
    CHECK(!output->passThrough());

    auto state = std::make_shared<SinkState>();
    CHECK_OK(output->joinAt(TimelineNs{0}));
    output->attach(std::make_unique<FakeSink>(state));
    output->pump();

    matp::AudioPacketReader reader;
    size_t consumed = 0;
    const std::vector<matp::AudioPacket> packets = decode(*state, reader, consumed);
    CHECK_EQ(packets.size(), size_t(4));

    for (const matp::AudioPacket& packet : packets)
    {
        // The timeline fields do not move with the layout; the payload does.
        CHECK_EQ(packet.header.timelineDurationNs, kPacketNs);
        CHECK_EQ(packet.header.outputFrameCount, kPacketFrames);
        CHECK(packet.header.format == wide);
        CHECK_EQ(packet.header.payloadBytes, kPacketFrames * wide.bytesPerFrame());
    }
}

MULTIROOM_TEST(client_output_equalizes_a_resampled_room_and_never_a_bitperfect_one)
{
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(4);

    EqualizerSettings halve;
    halve.enabled = true;
    halve.preampDb = -6.0206;

    // The payload of the first four packets, with and without the equalizer.
    const auto payloads = [&](const OutputPlan& plan, const EqualizerSettings* eq, bool& equalizing) {
        auto output = makeOutput(zone, clock, plan);
        if (eq != nullptr)
            output->setEqualizer(*eq);
        auto state = std::make_shared<SinkState>();
        CHECK_OK(output->joinAt(TimelineNs{0}));
        output->attach(std::make_unique<FakeSink>(state));
        output->pump();
        equalizing = output->equalizing();

        matp::AudioPacketReader reader;
        size_t consumed = 0;
        std::vector<unsigned char> bytes;
        for (const matp::AudioPacket& packet : decode(*state, reader, consumed))
            bytes.insert(bytes.end(), packet.payload, packet.payload + packet.payloadBytes);
        CHECK(!bytes.empty());
        return bytes;
    };

    bool equalizing = false;
    const OutputPlan exact = bitperfectPlan(zone.format, zone.format);
    const std::vector<unsigned char> bitperfect = payloads(exact, nullptr, equalizing);
    CHECK(bitperfect == payloads(exact, &halve, equalizing));
    CHECK(!equalizing);

    const OutputPlan converted = resampledPlan(zone.format, zone.format);
    const std::vector<unsigned char> plain = payloads(converted, nullptr, equalizing);
    CHECK(!equalizing);
    CHECK(plain != payloads(converted, &halve, equalizing));
    CHECK(equalizing);
}

MULTIROOM_TEST(client_output_paces_against_the_buffer_status_and_not_a_clock)
{
    // The trap this rule exists for: a paused room stops draining, and a
    // sender paced by its own clock fills the socket and never gets to the
    // resume. Nothing below advances the clock at all, and the room still
    // stops at its target.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(300); // six seconds of audio, far past the target

    ClientOutputSettings settings;
    settings.targetBufferNs = 1000000000ULL; // 1000 ms, fifty packets

    auto state = std::make_shared<SinkState>();
    auto output = makeOutput(zone, clock, bitperfectPlan(zone.format, zone.format), settings);
    CHECK_OK(output->joinAt(TimelineNs{0}));
    output->attach(std::make_unique<FakeSink>(state));

    output->pump();
    CHECK_EQ(output->packets(), 50ULL);
    CHECK_EQ(output->producedSinceReportNs(), settings.targetBufferNs);

    // Time passing changes nothing: what is compared is what the room holds.
    clock.advanceMs(5000);
    output->pump();
    CHECK_EQ(output->packets(), 50ULL);

    // A room that has played 400 ms of it asks for 400 ms more, and not a
    // packet beyond.
    matp::BufferStatus status;
    status.hasContent = true;
    status.bufferedDurationNs = 600000000ULL;
    output->onBufferStatus(status);
    CHECK_EQ(output->bufferedNs(), 600000000ULL);
    CHECK_EQ(output->producedSinceReportNs(), 0ULL); // nothing is still queued here

    output->pump();
    CHECK_EQ(output->packets(), 70ULL);

    // A room whose buffer is full is sent nothing at all.
    status.bufferedDurationNs = settings.targetBufferNs;
    output->onBufferStatus(status);
    output->pump();
    CHECK_EQ(output->packets(), 70ULL);
}

MULTIROOM_TEST(client_output_paces_a_playing_room_on_the_line_when_its_reports_stop)
{
    // The ESP32 bench run of 2026-10-02: a room's BUFFER_STATUS stopped
    // arriving for seconds while it played, the last report said it was full,
    // and it was sent nothing until the store evicted it. A room known to be
    // playing is fed up to the ceiling instead -- and only that room.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(300);

    ClientOutputSettings settings;
    settings.targetBufferNs = 1000000000ULL; // fifty packets

    auto state = std::make_shared<SinkState>();
    auto output = makeOutput(zone, clock, bitperfectPlan(zone.format, zone.format), settings);
    CHECK_OK(output->joinAt(TimelineNs{0}));
    output->attach(std::make_unique<FakeSink>(state));
    output->setCeiling(TimelineNs{settings.targetBufferNs + kPacketNs});
    output->pump();
    CHECK_EQ(output->packets(), 50ULL);
    CHECK(!output->bufferReportAgeNs());

    matp::BufferStatus full;
    full.hasContent = true;
    full.bufferedDurationNs = settings.targetBufferNs;
    output->onBufferStatus(full);
    output->setOnLine(true);

    // Half a second is an ordinary wait between reports.
    clock.advanceMs(500);
    output->pump();
    CHECK_EQ(output->packets(), 50ULL);
    CHECK(!output->pacedOnLine());

    // Two seconds is a gap: the room has played two seconds of what it held,
    // and is built up to one prebuffer and one packet past the line again.
    clock.advanceMs(1500);
    output->setCeiling(TimelineNs{2000000000ULL + settings.targetBufferNs + kPacketNs});
    output->pump();
    output->pump(); // the queue holds 64 packets, and a pass drains it only at its ends
    CHECK_EQ(output->packets(), 151ULL);
    CHECK(output->pacedOnLine());
    CHECK_EQ(output->reportGaps(), 1ULL);
    CHECK_EQ(*output->bufferReportAgeNs(), 2000000000ULL);

    // The reports come back, and with them section 18's rule.
    output->onBufferStatus(full);
    output->pump();
    CHECK(!output->pacedOnLine());
    CHECK_EQ(output->packets(), 151ULL);
    CHECK_EQ(output->longestReportGapNs(), 2000000000ULL);

    // A room that is not playing is not draining, so a gap there is not a
    // reason to send it anything.
    output->setOnLine(false);
    clock.advanceMs(5000);
    output->setCeiling(TimelineNs{7000000000ULL + settings.targetBufferNs + kPacketNs});
    output->pump();
    CHECK_EQ(output->packets(), 151ULL);
    CHECK_EQ(output->reportGaps(), 1ULL);
}

MULTIROOM_TEST(client_output_counts_what_it_still_holds_against_the_next_report)
{
    // A packet built but not yet delivered is audio this room is going to
    // have. Counting a report as if the queue were empty would send that much
    // audio twice over, which the client reads as a sequence gap.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(300);

    ClientOutputSettings settings;
    settings.targetBufferNs = 1000000000ULL;

    auto state = std::make_shared<SinkState>();
    auto output = makeOutput(zone, clock, bitperfectPlan(zone.format, zone.format), settings);
    CHECK_OK(output->joinAt(TimelineNs{0}));
    output->attach(std::make_unique<FakeSink>(state));

    // The peer's window is shut from the start, so everything produced stays
    // queued here.
    state->accepting.store(false);
    output->pump();
    CHECK_EQ(output->queuedPackets(), size_t(50));
    CHECK_EQ(output->packets(), 0ULL); // delivered, not built

    matp::BufferStatus status;
    status.bufferedDurationNs = 0; // it has received none of it
    output->onBufferStatus(status);

    // One second of audio is in this server's queue, which is the whole
    // target: nothing more is produced.
    CHECK_EQ(output->producedSinceReportNs(), settings.targetBufferNs);
    output->pump();
    CHECK_EQ(output->queuedPackets(), size_t(50));
}

MULTIROOM_TEST(client_output_serves_one_room_while_another_stops_reading)
{
    // The mandatory test, and the rule the whole architecture is shaped
    // around. Two rooms on one decode, on their own threads: one stops taking
    // bytes entirely and fills its own bounded queue, and the other keeps
    // receiving at its full rate for the length of the test. The failure is
    // visible in the stalled room's own status and nowhere else.
    const SystemClock clock;
    FakeZone zone;

    ClientOutputSettings settings;
    settings.targetBufferNs = 400000000ULL;        // twenty packets
    settings.maxQueuedPackets = 8;
    settings.queueStallTimeoutNs = 300000000ULL;   // 300 ms of a shut window

    auto deafState = std::make_shared<SinkState>();
    auto livelyState = std::make_shared<SinkState>();

    auto deaf = makeOutput(zone, clock, bitperfectPlan(zone.format, zone.format), settings,
                           "kitchen");
    auto lively = makeOutput(zone, clock, bitperfectPlan(zone.format, zone.format), settings,
                             "livingroom");

    CHECK_OK(deaf->joinAt(TimelineNs{0}));
    CHECK_OK(lively->joinAt(TimelineNs{0}));
    deaf->attach(std::make_unique<FakeSink>(deafState));
    lively->attach(std::make_unique<FakeSink>(livelyState));

    // The room that never reads a byte.
    deafState->accepting.store(false);

    CHECK_OK(deaf->run());
    CHECK_OK(lively->run());

    // One decode, published while both rooms read it.
    matp::BufferStatus drained;
    drained.bufferedDurationNs = 0;

    for (size_t i = 0; i < 200; ++i)
    {
        zone.publish();
        // The lively room plays everything it is sent as fast as it arrives,
        // so its pacing never stops it.
        lively->onBufferStatus(drained);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // The deaf room let go of its own connection, on its own thread, with its
    // own reason. Phase 16: the session is on the control connection, so a
    // socket that stopped taking bytes costs this room its socket and not its
    // place -- it keeps its position and is still streaming, and it is the
    // closed socket that makes a client that is still there re-attach.
    CHECK(waitFor([&] { return deafState->closed.load(); }));
    CHECK_EQ(deaf->connectionDrops(), 1ULL);
    CHECK(deaf->lastDrop().find("QueueStalled") != std::string::npos);
    CHECK(deaf->state() == OutputState::Streaming);
    CHECK_EQ(deaf->lastError(), std::string());
    CHECK_EQ(deaf->packets(), 0ULL);

    // And the other room is untouched: still streaming, and holding a
    // contiguous run of everything the zone published.
    CHECK(waitFor([&] { return lively->packets() >= 150; }));
    CHECK(lively->state() == OutputState::Streaming);
    CHECK_EQ(lively->lastError(), std::string());

    lively->stop();
    deaf->stop();

    matp::AudioPacketReader reader;
    size_t consumed = 0;
    const std::vector<matp::AudioPacket> packets = decode(*livelyState, reader, consumed);
    CHECK(packets.size() >= 150);

    uint64_t expectedNs = 0;
    for (size_t index = 0; index < packets.size(); ++index)
    {
        CHECK_EQ(packets[index].header.sequenceNumber, uint64_t(index));
        CHECK_EQ(packets[index].header.timelineStartNs, expectedNs);
        expectedNs += packets[index].header.timelineDurationNs;
    }
}

MULTIROOM_TEST(client_output_bounds_its_queue_and_lets_go_of_a_connection_that_never_drains)
{
    // Memory is bounded, and the bound is a packet count. A room whose window
    // stays shut has its connection closed after the timeout rather than held
    // forever -- and before the timeout it is merely full, which is not a
    // fault. Phase 16 is why the connection goes and the room does not: a
    // socket that has stopped taking bytes while the room still owes its
    // buffer audio is a connection that went away without TCP noticing, and
    // closing it is what tells a client that is still there to re-attach.
    multiroomtest::ManualClock clock(1000000000ULL);
    FakeZone zone;
    zone.publishBlocks(200);

    ClientOutputSettings settings;
    settings.maxQueuedPackets = 10;
    settings.queueStallTimeoutNs = 10ULL * kNanosecondsPerSecond;

    auto state = std::make_shared<SinkState>();
    auto output = makeOutput(zone, clock, bitperfectPlan(zone.format, zone.format), settings);
    CHECK_OK(output->joinAt(TimelineNs{0}));
    output->attach(std::make_unique<FakeSink>(state));
    state->accepting.store(false);

    output->pump();
    CHECK_EQ(output->queuedPackets(), size_t(10));
    CHECK_EQ(output->queueFullCount(), 1ULL);
    CHECK(output->state() == OutputState::Streaming);

    // Nine seconds of a shut window is a room that is merely full.
    clock.advanceMs(9000);
    output->pump();
    CHECK(output->state() == OutputState::Streaming);

    clock.advanceMs(1500);
    output->pump();
    CHECK_EQ(output->connectionDrops(), 1ULL);
    CHECK(output->lastDrop().find("QueueStalled") != std::string::npos);
    CHECK(state->closed.load());

    // The room itself is untouched: still streaming, still holding everything
    // it built, and no failure recorded against it.
    CHECK(output->state() == OutputState::Streaming);
    CHECK_EQ(output->lastError(), std::string());
    CHECK_EQ(output->queuedPackets(), size_t(10));

    // And it goes on where it left off the moment a socket takes it, which is
    // section 43's "no discontinuity at all": the packet the old socket was
    // half-way through is written again from its first byte and the timeline
    // carries on unbroken.
    auto resumed = std::make_shared<SinkState>();
    output->attach(std::make_unique<FakeSink>(resumed));
    matp::BufferStatus empty;
    output->onBufferStatus(empty);
    output->pump();
    CHECK_EQ(output->connectionDrops(), 1ULL);
    CHECK(output->packets() >= 10ULL);

    matp::AudioPacketReader reader;
    size_t consumed = 0;
    const std::vector<matp::AudioPacket> packets = decode(*resumed, reader, consumed);
    CHECK(packets.size() >= 10);
    uint64_t expectedNs = 0;
    for (size_t index = 0; index < packets.size(); ++index)
    {
        CHECK_EQ(packets[index].header.sequenceNumber, uint64_t(index));
        CHECK_EQ(packets[index].header.timelineStartNs, expectedNs);
        expectedNs += packets[index].header.timelineDurationNs;
    }
}

MULTIROOM_TEST(client_output_does_not_stall_a_room_that_has_not_attached_yet)
{
    // A queue nobody is draining because nobody has connected is not a stall:
    // a room that never attaches is phase 11's readiness timeout, and failing
    // it here would end a room whose client is still opening its device.
    multiroomtest::ManualClock clock(1000000000ULL);
    FakeZone zone;
    zone.publishBlocks(200);

    ClientOutputSettings settings;
    settings.maxQueuedPackets = 10;
    settings.queueStallTimeoutNs = kNanosecondsPerSecond;

    auto output = makeOutput(zone, clock, bitperfectPlan(zone.format, zone.format), settings);
    CHECK_OK(output->joinAt(TimelineNs{0}));

    output->pump();
    CHECK_EQ(output->queuedPackets(), size_t(10));

    clock.advanceMs(60000);
    output->pump();
    CHECK(output->state() == OutputState::Streaming);
    CHECK_EQ(output->queueFullCount(), 0ULL);

    // And the moment a connection arrives, everything held is delivered --
    // and the room carries on producing behind it, because pacing has plenty
    // of room left.
    auto state = std::make_shared<SinkState>();
    output->attach(std::make_unique<FakeSink>(state));
    output->pump();
    CHECK(output->packets() >= 10ULL);
    CHECK(output->state() == OutputState::Streaming);
}

MULTIROOM_TEST(client_output_restarts_the_packet_a_replaced_connection_never_finished)
{
    // Section 43: the same room on a new socket. The new one never saw the
    // prefix of the packet the old one was half-way through, and half a header
    // is not a packet -- so the stream begins again at that packet's first
    // byte, with the sequence carrying on.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(4);

    // A sink that takes the header and then shuts its window, so exactly one
    // packet is left half sent.
    class HalfSink final : public IPacketSink
    {
    public:
        explicit HalfSink(std::shared_ptr<SinkState> state) : state_(std::move(state)) {}

        Result<size_t, PacketSinkError> write(std::span<const std::byte> bytes) override
        {
            if (taken_ >= matp::kAudioHeaderBytes)
                return size_t{0};
            const size_t chunk = std::min(bytes.size(), matp::kAudioHeaderBytes - taken_);
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->received.append(reinterpret_cast<const char*>(bytes.data()), chunk);
            taken_ += chunk;
            return chunk;
        }

        void close() override { state_->closed.store(true); }

    private:
        std::shared_ptr<SinkState> state_;
        size_t taken_ = 0;
    };

    auto half = std::make_shared<SinkState>();
    auto whole = std::make_shared<SinkState>();

    auto output = makeOutput(zone, clock, bitperfectPlan(zone.format, zone.format));
    CHECK_OK(output->joinAt(TimelineNs{0}));
    output->attach(std::make_unique<HalfSink>(half));
    output->pump();

    CHECK_EQ(half->received.size(), matp::kAudioHeaderBytes);
    CHECK_EQ(output->packets(), 0ULL);
    CHECK_EQ(output->queuedPackets(), size_t(4));

    output->attach(std::make_unique<FakeSink>(whole));
    CHECK(half->closed.load());
    output->pump();

    matp::AudioPacketReader reader;
    size_t consumed = 0;
    const std::vector<matp::AudioPacket> packets = decode(*whole, reader, consumed);
    CHECK_EQ(packets.size(), size_t(4));
    CHECK_EQ(packets[0].header.sequenceNumber, 0ULL);
    CHECK_EQ(packets[0].header.timelineStartNs, 0ULL);
}

MULTIROOM_TEST(client_output_strands_a_room_whose_cursor_the_decode_ran_past)
{
    // Evicted, not silently resumed: the audio that cursor named has been
    // dropped, and sending what took its place would be stale samples in the
    // right place. Phase 16 is what it is instead of a failure -- the room is
    // intact and is waiting to be told where it is now, which is the one thing
    // this class cannot decide for itself.
    multiroomtest::ManualClock clock;
    FakeZone zone;

    auto small = NativePcmStore::create(kPacketNs * 4, 4);
    CHECK_OK(small);
    zone.store = std::move(*small);

    zone.publishBlocks(2);

    auto state = std::make_shared<SinkState>();
    auto output = makeOutput(zone, clock, bitperfectPlan(zone.format, zone.format));
    CHECK_OK(output->joinAt(TimelineNs{0}));
    output->attach(std::make_unique<FakeSink>(state));

    // The decode runs a long way past a room that has not read a byte of it.
    zone.publishBlocks(20);

    output->pump();
    CHECK(output->state() == OutputState::Stranded);
    CHECK(output->lastError().find("StoreFailed") != std::string::npos);
    CHECK(output->lastError().find("Evicted") != std::string::npos);

    // The connection is left alone -- nothing is wrong with it, and the
    // packets that follow the rejoin go out on it -- and what was built for
    // the position this room is no longer at is gone.
    CHECK(!state->closed.load());
    CHECK_EQ(output->queuedPackets(), size_t(0));

    // A stranded room produces nothing until it is given a position, and
    // pumping it again does not turn it into a failure.
    output->pump();
    CHECK(output->state() == OutputState::Stranded);
    CHECK_EQ(output->queuedPackets(), size_t(0));

    // And a position the store still holds is all it needs. The same loop
    // carries on -- the packetizer's sequence with it, because the client
    // reads a gap in the numbering on one connection as a server that skipped
    // ahead.
    const Result<TimelineNs, TimelineError> oldest =
        zone.timeline.timelineAt(zone.store->oldestFrame());
    CHECK_OK(oldest);
    CHECK_OK(output->rejoinAt(*oldest));
    CHECK(output->state() == OutputState::Streaming);
    output->pump();
    CHECK(output->packets() > 0ULL);

    matp::AudioPacketReader reader;
    size_t consumed = 0;
    const std::vector<matp::AudioPacket> packets = decode(*state, reader, consumed);
    CHECK(!packets.empty());
    CHECK_EQ(packets.front().header.timelineStartNs, std::to_underlying(*oldest));
}

MULTIROOM_TEST(client_output_waits_for_a_decode_that_has_not_got_there_yet)
{
    // A room that has caught up with its zone is not a fault. It produces
    // nothing, says so in a counter, and carries on the moment there is more.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(3);

    auto state = std::make_shared<SinkState>();
    auto output = makeOutput(zone, clock, bitperfectPlan(zone.format, zone.format));
    CHECK_OK(output->joinAt(TimelineNs{0}));
    output->attach(std::make_unique<FakeSink>(state));

    output->pump();
    CHECK_EQ(output->packets(), 3ULL);
    CHECK(output->starvedCount() >= 1ULL);
    CHECK(output->state() == OutputState::Streaming);

    zone.publishBlocks(2);
    output->pump();
    CHECK_EQ(output->packets(), 5ULL);

    matp::AudioPacketReader reader;
    size_t consumed = 0;
    const std::vector<matp::AudioPacket> packets = decode(*state, reader, consumed);
    CHECK_EQ(packets.size(), size_t(5));
    CHECK_EQ(packets[4].header.timelineStartNs, 4 * kPacketNs);
}

MULTIROOM_TEST(client_output_cuts_a_packet_on_an_end_published_while_it_was_being_built)
{
    // The zone's end arrives in the gap: the packet was sized whole, and is
    // cut on the end rather than finished with silence that would run past it.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    const auto end = zone.timeline.timelineAt(SourceFrame{kPacketFrames + kTailFrames});
    CHECK_OK(end);

    const std::vector<matp::AudioPacket> packets =
        buildWithTailHook(zone, clock, [&](ClientOutput& room) { room.setEnd(*end); });
    CHECK_EQ(packets.size(), size_t(2));
    CHECK_EQ(packets[1].header.outputFrameCount, kTailFrames);
    CHECK_EQ(packets[1].header.timelineStartNs + packets[1].header.timelineDurationNs,
             std::to_underlying(*end));
}

MULTIROOM_TEST(client_output_cuts_a_packet_on_a_hold_installed_while_it_was_being_built)
{
    // Section 24's boundary arriving in the same gap, in the zone's order: the
    // hold, then the first block past it. The packet stops on the boundary
    // rather than being finished with silence across it -- which would also
    // have moved the switch past the boundary, and lost the start of the new
    // source to that silence.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    const auto boundary = zone.timeline.timelineAt(SourceFrame{kPacketFrames + kTailFrames});
    CHECK_OK(boundary);

    const std::vector<matp::AudioPacket> packets =
        buildWithTailHook(zone, clock, [&](ClientOutput& room) {
            CHECK_OK(room.holdAt(*boundary));
            zone.publish();
        });
    CHECK_EQ(packets.size(), size_t(2));
    CHECK_EQ(packets[1].header.outputFrameCount, kTailFrames);
    CHECK_EQ(packets[1].header.timelineStartNs + packets[1].header.timelineDurationNs,
             std::to_underlying(*boundary));
}

MULTIROOM_TEST(client_output_cuts_a_packet_on_an_end_a_converter_s_lookahead_reached)
{
    // The same gap, with a converter that keeps its last frame back until
    // there is something behind it: the end is met while the carry is being
    // extended, and silence is still only added once the packet is cut on it.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    const auto end = zone.timeline.timelineAt(SourceFrame{kPacketFrames + kTailFrames});
    CHECK_OK(end);

    const std::vector<matp::AudioPacket> packets = buildWithTailHook(
        zone, clock, [&](ClientOutput& room) { room.setEnd(*end); }, true);
    CHECK_EQ(packets.size(), size_t(2));
    CHECK_EQ(packets[1].header.outputFrameCount, kTailFrames);
    CHECK_EQ(packets[1].header.timelineStartNs + packets[1].header.timelineDurationNs,
             std::to_underlying(*end));
}

MULTIROOM_TEST(client_output_cuts_a_packet_on_a_hold_a_converter_s_lookahead_reached)
{
    // The hold's counterpart: the block past the boundary is put back while
    // the carry is being extended, and the packet is cut on the boundary.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    const auto boundary = zone.timeline.timelineAt(SourceFrame{kPacketFrames + kTailFrames});
    CHECK_OK(boundary);

    const std::vector<matp::AudioPacket> packets = buildWithTailHook(
        zone, clock,
        [&](ClientOutput& room) {
            CHECK_OK(room.holdAt(*boundary));
            zone.publish();
        },
        true);
    CHECK_EQ(packets.size(), size_t(2));
    CHECK_EQ(packets[1].header.outputFrameCount, kTailFrames);
    CHECK_EQ(packets[1].header.timelineStartNs + packets[1].header.timelineDurationNs,
             std::to_underlying(*boundary));
}

MULTIROOM_TEST(client_output_joins_a_zone_that_is_already_playing)
{
    // Section 44's late join: a room that arrives an hour in begins at its own
    // frame zero, at the timeline position it joined at, and reads the store
    // from the frame that position maps to.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(100);

    const uint64_t joinNs = 50 * kPacketNs; // half way through what is decoded

    auto state = std::make_shared<SinkState>();
    auto output = makeOutput(zone, clock, bitperfectPlan(zone.format, zone.format));
    CHECK_OK(output->joinAt(TimelineNs{joinNs}));
    CHECK_ERR(output->joinAt(TimelineNs{0}), OutputError::AlreadyJoined);

    output->attach(std::make_unique<FakeSink>(state));
    output->pump();

    matp::AudioPacketReader reader;
    size_t consumed = 0;
    const std::vector<matp::AudioPacket> packets = decode(*state, reader, consumed);
    CHECK_EQ(packets.size(), size_t(50));
    CHECK_EQ(packets[0].header.sequenceNumber, 0ULL);
    CHECK_EQ(packets[0].header.timelineStartNs, joinNs);

    // And the audio is the audio of that position, not of the zone's start.
    for (size_t i = 0; i < packets[0].payloadBytes; ++i)
    {
        const unsigned char expected =
            static_cast<unsigned char>((50 * kPacketFrames * 7 + i) & 0xFF);
        if (packets[0].payload[i] != expected)
            multiroomtest::failAt("a late join read the wrong frames", __FILE__, __LINE__);
    }
}

MULTIROOM_TEST(client_output_converts_across_the_block_boundaries_the_store_hands_it)
{
    // A converter needs the frame *after* the one it is interpolating from,
    // and the store hands audio over one block at a time -- so the last frame
    // of a block is not enough on its own. Left alone that is a room which
    // fails with "the converter neither consumed nor produced a frame" the
    // first time a packet ends one frame short of a block, which is a matter
    // of arithmetic rather than of luck. Nothing in phase 9 resampled, which
    // is why nothing in phase 9 met it.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(120); // 2.4 s of 20 ms blocks, at the source's rate

    const AudioFormat output = formatOf(AudioFormat::SampleType::Int32, 48000);
    auto room = makeOutput(zone, clock, resampledPlan(zone.format, output));
    CHECK(!room->passThrough());

    auto state = std::make_shared<SinkState>();
    CHECK_OK(room->joinAt(TimelineNs{0}));
    room->attach(std::make_unique<FakeSink>(state));

    // One pass produces at most a queue's worth, so the prebuffer takes a few
    // of them. Pacing stops the rest.
    for (int pass = 0; pass < 8; ++pass)
        room->pump();

    CHECK(room->state() == OutputState::Streaming);
    CHECK_EQ(room->lastError(), std::string());

    matp::AudioPacketReader reader;
    size_t consumed = 0;
    const std::vector<matp::AudioPacket> packets = decode(*state, reader, consumed);

    // The default 1500 ms target at 20 ms a packet, and not one packet fewer:
    // a room that met a boundary it could not cross would stop short of it.
    CHECK_EQ(packets.size(), size_t(75));
    for (size_t index = 0; index < packets.size(); ++index)
    {
        CHECK_EQ(packets[index].header.timelineStartNs, index * kPacketNs);
        CHECK_EQ(packets[index].header.timelineDurationNs, kPacketNs);
        CHECK_EQ(packets[index].header.outputFrameCount, 960u); // 20 ms at 48 kHz
        CHECK_EQ(packets[index].header.format.sampleRate, 48000u);
    }
}

MULTIROOM_TEST(client_output_ends_a_room_whose_converter_never_moves)
{
    // The carry that crosses a block boundary is bounded, and this is the
    // bound: a converter shown more than a packet's worth of source and still
    // taking nothing is broken rather than short of input, and is said out
    // loud rather than fed forever.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(20);

    const AudioFormat output = formatOf(AudioFormat::SampleType::Int32, 48000);
    auto room = ClientOutput::create(
        testSessionId(), ClientId("livingroom"), *zone.store, zone.timeline,
        std::make_unique<StuckProcessor>(resampledPlan(zone.format, output)), clock, {});
    CHECK_OK(room);

    auto state = std::make_shared<SinkState>();
    CHECK_OK((*room)->joinAt(TimelineNs{0}));
    (*room)->attach(std::make_unique<FakeSink>(state));
    (*room)->pump();

    CHECK((*room)->state() == OutputState::Failed);
    CHECK((*room)->lastError().find("ProcessorFailed") != std::string::npos);
    CHECK_EQ((*room)->packets(), 0u);
}

// --- phase 13: the correction, applied (sections 28 and 30) -------------------

MULTIROOM_TEST(client_output_moves_its_clock_and_its_converter_with_one_trim)
{
    // +500 ppm, which is section 31's convergence limit: an extra frame every
    // fifth packet at 44.1 kHz, large enough to count exactly and still a
    // correction rather than a conversion.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(300); // six seconds

    ClientOutputSettings settings;
    settings.targetBufferNs = 1000000000ULL; // fifty packets a pass

    const OutputPlan plan = correctedPlan(zone.format, zone.format);
    auto processor = std::make_unique<CountingProcessor>(plan);
    CountingProcessor& converter = *processor;

    auto created = ClientOutput::create(testSessionId(), ClientId("livingroom"), *zone.store,
                                        zone.timeline, std::move(processor), clock, settings);
    CHECK_OK(created);
    std::unique_ptr<ClientOutput> output = std::move(*created);

    auto state = std::make_shared<SinkState>();
    CHECK_OK(output->joinAt(TimelineNs{0}));
    output->attach(std::make_unique<FakeSink>(state));

    // Fifty packets of an uncorrected room first: nothing has been asked of
    // the converter, and every packet is the same 882 frames.
    output->pump();
    CHECK_EQ(output->packets(), 50ULL);
    CHECK_EQ(output->frames(), 50ULL * kPacketFrames);
    CHECK_EQ(converter.calls(), 0ULL);
    CHECK_EQ(output->driftTrim(), 1.0);

    // The correction, which is a wish until this room's own thread grants it.
    output->setDriftTrim(1.0005);
    CHECK_EQ(converter.calls(), 0ULL);

    matp::BufferStatus drained;
    drained.hasContent = true;
    drained.bufferedDurationNs = 0;
    output->onBufferStatus(drained);
    output->pump();

    // One call, carrying the whole conversion: this room's rate matches the
    // zone's, so the nominal ratio is one and the trim is all of it.
    CHECK_EQ(converter.calls(), 1ULL);
    CHECK_NEAR(converter.lastRatio(), 1.0005, 1e-12);
    CHECK_EQ(output->corrections(), 1ULL);
    CHECK_EQ(output->correctionsRefused(), 0ULL);
    CHECK_NEAR(output->driftTrim(), 1.0005, 1e-12);

    // And the clock asked for the frames that trim is worth: one second of
    // timeline is 44122 frames rather than 44100, which is the extra frame in
    // every fifth packet and nothing rounded away.
    CHECK_EQ(output->packets(), 100ULL);
    CHECK_EQ(output->frames() - 50ULL * kPacketFrames, 44122ULL);

    // A second pass with the same trim installs nothing new and loses nothing
    // either: a re-anchor carries the fraction of a frame the last one left.
    output->setDriftTrim(1.0005);
    output->onBufferStatus(drained);
    output->pump();
    CHECK_EQ(converter.calls(), 2ULL);
    CHECK_EQ(output->frames() - 50ULL * kPacketFrames, 2ULL * 44122ULL);
}

MULTIROOM_TEST(client_output_carries_the_nominal_ratio_into_every_trim)
{
    // A room resampled from 44.1 to 48 kHz has that conversion in its rate and
    // its correction on top of it, and the converter is told the product --
    // IAudioResampler's ratio is the whole conversion and never the trim
    // alone. Handing it the trim would leave a 48 kHz room playing a 44.1 kHz
    // stream, which is a defect that sounds like a wrong speed rather than
    // like a wrong correction.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(300);

    ClientOutputSettings settings;
    settings.targetBufferNs = 1000000000ULL; // fifty packets, and one second of them

    const AudioFormat wider = formatOf(AudioFormat::SampleType::Int16, 48000);
    auto processor = std::make_unique<CountingProcessor>(correctedPlan(zone.format, wider));
    CountingProcessor& converter = *processor;

    auto created = ClientOutput::create(testSessionId(), ClientId("kitchen"), *zone.store,
                                        zone.timeline, std::move(processor), clock, settings);
    CHECK_OK(created);
    std::unique_ptr<ClientOutput> output = std::move(*created);

    auto state = std::make_shared<SinkState>();
    CHECK_OK(output->joinAt(TimelineNs{0}));
    output->attach(std::make_unique<FakeSink>(state));

    output->setDriftTrim(1.0005);
    output->pump();

    CHECK_EQ(converter.calls(), 1ULL);
    CHECK_NEAR(converter.lastRatio(), (48000.0 / 44100.0) * 1.0005, 1e-12);

    // The clock's trim is the correction alone, because the rate it counts in
    // is already this room's own.
    CHECK_NEAR(output->driftTrim(), 1.0005, 1e-12);
    CHECK_EQ(output->frames(), 48024ULL); // one second of 48 kHz at +500 ppm
}

MULTIROOM_TEST(client_output_never_gives_a_ratio_to_a_room_that_is_not_corrected)
{
    // The roadmap's phase 13, and the test that matters as much as the
    // correction itself: a bitperfect room is left to drift, under every
    // circumstance and whatever the controller says. MATP section 32
    // recommends resyncing one instead; this server declines, and a room that
    // asked to be left alone is left alone.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(300);

    ClientOutputSettings settings;
    settings.targetBufferNs = 1000000000ULL;

    for (const OutputPlan& plan : {bitperfectPlan(zone.format, zone.format),
                                   resampledPlan(zone.format, zone.format)})
    {
        auto processor = std::make_unique<CountingProcessor>(plan);
        CountingProcessor& converter = *processor;

        auto created = ClientOutput::create(testSessionId(), ClientId("livingroom"),
                                            *zone.store, zone.timeline, std::move(processor),
                                            clock, settings);
        CHECK_OK(created);
        std::unique_ptr<ClientOutput> output = std::move(*created);

        auto state = std::make_shared<SinkState>();
        CHECK_OK(output->joinAt(TimelineNs{0}));
        output->attach(std::make_unique<FakeSink>(state));

        CHECK(!output->driftCorrected());

        output->setDriftTrim(1.0005);
        output->pump();

        // Never asked for, never applied, and the stream is the one an
        // uncorrected room gets: 882 frames a packet, every packet.
        CHECK_EQ(converter.calls(), 0ULL);
        CHECK_EQ(output->corrections(), 0ULL);
        CHECK_EQ(output->correctionsRefused(), 0ULL);
        CHECK_EQ(output->driftTrim(), 1.0);
        CHECK_EQ(output->packets(), 50ULL);
        CHECK_EQ(output->frames(), 50ULL * kPacketFrames);
    }
}

MULTIROOM_TEST(client_output_keeps_playing_when_a_converter_refuses_a_trim)
{
    // A converter's trim window is fixed when its filter is designed and a
    // deployment may configure a correction limit wider than it. That is a
    // configuration to correct rather than a room to end: nothing moves, the
    // refusal is counted, and the room plays on at the trim it already had.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(300);

    ClientOutputSettings settings;
    settings.targetBufferNs = 1000000000ULL;

    auto processor =
        std::make_unique<CountingProcessor>(correctedPlan(zone.format, zone.format), true);
    CountingProcessor& converter = *processor;

    auto created = ClientOutput::create(testSessionId(), ClientId("livingroom"), *zone.store,
                                        zone.timeline, std::move(processor), clock, settings);
    CHECK_OK(created);
    std::unique_ptr<ClientOutput> output = std::move(*created);

    auto state = std::make_shared<SinkState>();
    CHECK_OK(output->joinAt(TimelineNs{0}));
    output->attach(std::make_unique<FakeSink>(state));

    output->setDriftTrim(1.05);
    output->pump();

    CHECK_EQ(converter.calls(), 1ULL);
    CHECK_EQ(output->corrections(), 0ULL);
    CHECK_EQ(output->correctionsRefused(), 1ULL);
    CHECK_EQ(output->driftTrim(), 1.0); // the clock was never moved either
    CHECK(output->state() == OutputState::Streaming);
    CHECK_EQ(output->packets(), 50ULL);
    CHECK_EQ(output->frames(), 50ULL * kPacketFrames);
}

MULTIROOM_TEST(client_output_ignores_a_trim_that_is_not_a_ratio)
{
    // Zero is this room's "nothing pending" and a negative or absent ratio is
    // a controller fault, not a correction. None of them reaches a converter.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(50);

    auto processor =
        std::make_unique<CountingProcessor>(correctedPlan(zone.format, zone.format));
    CountingProcessor& converter = *processor;

    auto created = ClientOutput::create(testSessionId(), ClientId("livingroom"), *zone.store,
                                        zone.timeline, std::move(processor), clock, {});
    CHECK_OK(created);
    std::unique_ptr<ClientOutput> output = std::move(*created);

    auto state = std::make_shared<SinkState>();
    CHECK_OK(output->joinAt(TimelineNs{0}));
    output->attach(std::make_unique<FakeSink>(state));

    output->setDriftTrim(0.0);
    output->setDriftTrim(-1.0005);
    output->setDriftTrim(std::numeric_limits<double>::quiet_NaN());
    output->pump();

    CHECK_EQ(converter.calls(), 0ULL);
    CHECK_EQ(output->corrections(), 0ULL);
    CHECK_EQ(output->driftTrim(), 1.0);
}

// --- a room grouped while it plays -----

namespace {

// The converter a room is given when it is grouped, watched: the first audio
// it is shown is where its priming began.
class WatchedProcessor final : public IClientAudioProcessor
{
public:
    explicit WatchedProcessor(const OutputPlan& plan) : inner_(processorFor(plan)) {}

    const OutputPlan& plan() const noexcept override { return inner_->plan(); }
    double ratio() const noexcept override { return inner_->ratio(); }
    Status<ProcessorError> setRatio(double ratio) override { return inner_->setRatio(ratio); }

    Result<ProcessResult, ProcessorError> process(std::span<const std::byte> input,
                                                  std::span<std::byte> output) override
    {
        if (first_.empty() && !input.empty())
            first_.push_back(input.front());
        return inner_->process(input, output);
    }

    void reset() override
    {
        ++resets_;
        inner_->reset();
    }
    double latencyFrames() const noexcept override { return inner_->latencyFrames(); }

    [[nodiscard]] const std::vector<std::byte>& first() const { return first_; }
    [[nodiscard]] uint64_t resets() const { return resets_; }

private:
    std::unique_ptr<IClientAudioProcessor> inner_;
    std::vector<std::byte> first_;
    uint64_t resets_ = 0;
};

} // namespace

MULTIROOM_TEST(client_output_replaces_its_converter_between_two_packets_without_a_seam)
{
    // A bitperfect room becomes a corrected one in the middle of a track, on
    // the output it already has. The client must not be able to tell: one
    // numbering, one grid, and the source's own bytes on both sides -- which
    // through FakeResampler at a ratio of one also proves that the priming
    // was discarded to the frame. And it was primed: the first audio the
    // converter was shown is 100 ms before the frame it carries on from.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(101);

    ClientOutputSettings settings;
    settings.targetBufferNs = 1000000000ULL; // fifty packets

    auto state = std::make_shared<SinkState>();
    auto output = makeOutput(zone, clock, bitperfectPlan(zone.format, zone.format), settings);
    CHECK_ERR(output->replaceProcessor(processorFor(correctedPlan(zone.format, zone.format))),
              OutputError::NotStreaming);
    CHECK_OK(output->joinAt(TimelineNs{0}));
    output->attach(std::make_unique<FakeSink>(state));
    output->pump();
    CHECK_EQ(output->packets(), 50ULL);
    CHECK(output->passThrough());

    // A device that would have to be reopened is not this.
    CHECK_ERR(output->replaceProcessor(processorFor(correctedPlan(
                  zone.format, formatOf(AudioFormat::SampleType::Int24In32)))),
              OutputError::InvalidPlan);

    auto watched = std::make_unique<WatchedProcessor>(correctedPlan(zone.format, zone.format));
    const WatchedProcessor& converter = *watched;
    CHECK_OK(output->replaceProcessor(std::move(watched)));
    CHECK_ERR(output->replaceProcessor(processorFor(correctedPlan(zone.format, zone.format))),
              OutputError::SwitchPending);

    matp::BufferStatus status;
    status.hasContent = true;
    status.bufferedDurationNs = 0;
    output->onBufferStatus(status);
    output->pump();
    CHECK_EQ(output->formatSwitches(), 1ULL);
    CHECK(!output->passThrough());
    CHECK(output->driftCorrected());
    CHECK(output->plan().outputFormat == zone.format);
    CHECK_EQ(output->packets(), 100ULL);

    // Primed from five blocks back, and never started cold.
    constexpr uint64_t kFirstShown = 45ULL * kPacketFrames;
    CHECK_EQ(converter.first().size(), size_t{1});
    CHECK_EQ(std::to_integer<unsigned>(converter.first().front()),
             static_cast<unsigned>((kFirstShown * 7) & 0xFF));
    CHECK_EQ(converter.resets(), 0ULL);

    matp::AudioPacketReader reader;
    size_t consumed = 0;
    const std::vector<matp::AudioPacket> packets = decode(*state, reader, consumed);
    CHECK_EQ(packets.size(), size_t(100));
    for (size_t index = 0; index < packets.size(); ++index)
    {
        const matp::AudioPacket& packet = packets[index];
        CHECK_EQ(packet.header.sequenceNumber, uint64_t(index));
        CHECK_EQ(packet.header.timelineStartNs, uint64_t(index) * kPacketNs);
        CHECK_EQ(packet.header.outputFrameCount, kPacketFrames);
        CHECK(packet.header.format == zone.format);
        for (size_t i = 0; i < packet.payloadBytes; ++i)
        {
            const unsigned char expected =
                static_cast<unsigned char>((index * kPacketFrames * 7 + i) & 0xFF);
            if (packet.payload[i] != expected)
            {
                multiroomtest::failAt("packet " + std::to_string(index) + " byte " +
                                          std::to_string(i) + " is not the source's",
                                      __FILE__, __LINE__);
                break;
            }
        }
    }
}

MULTIROOM_TEST(client_output_drops_a_waiting_replacement_at_a_boundary)
{
    // A format boundary plans the room again, so a converter that was waiting
    // for the next packet is not installed on the boundary in its place --
    // which would also have taken the hold with it.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(20);

    ClientOutputSettings settings;
    settings.targetBufferNs = 200000000ULL; // ten packets

    auto output = makeOutput(zone, clock, bitperfectPlan(zone.format, zone.format), settings);
    CHECK_OK(output->joinAt(TimelineNs{0}));
    output->attach(std::make_unique<FakeSink>(std::make_shared<SinkState>()));
    output->pump();
    CHECK_EQ(output->packets(), 10ULL);

    CHECK_OK(output->replaceProcessor(processorFor(correctedPlan(zone.format, zone.format))));
    CHECK_OK(output->holdAt(TimelineNs{10ULL * kPacketNs}));
    output->pump();
    CHECK_EQ(output->formatSwitches(), 0ULL);
    CHECK_OK(output->switchTo(processorFor(bitperfectPlan(zone.format, zone.format)),
                              TimelineNs{10ULL * kPacketNs}));
    output->pump();
    CHECK_EQ(output->formatSwitches(), 1ULL);
    CHECK(output->plan().mode == OutputMode::BitPerfect);
}

MULTIROOM_TEST(client_output_takes_two_boundaries_ahead_of_it_in_turn)
{
    // Two short sources in different formats inside one lookahead: the
    // second boundary is announced before this stream has reached the first.
    // It is queued behind the first rather than replacing it, so the stream
    // stops at each one and takes the converter named for it there.
    multiroomtest::ManualClock clock;
    FakeZone zone;
    zone.publishBlocks(20);

    ClientOutputSettings settings;
    settings.targetBufferNs = 200000000ULL; // ten packets

    auto output = makeOutput(zone, clock, bitperfectPlan(zone.format, zone.format), settings);
    CHECK_OK(output->joinAt(TimelineNs{0}));
    output->attach(std::make_unique<FakeSink>(std::make_shared<SinkState>()));

    CHECK_OK(output->holdAt(TimelineNs{5ULL * kPacketNs}));
    CHECK_OK(output->switchTo(processorFor(correctedPlan(zone.format, zone.format)),
                              TimelineNs{5ULL * kPacketNs}));
    CHECK_OK(output->holdAt(TimelineNs{8ULL * kPacketNs}));
    CHECK_OK(output->switchTo(processorFor(bitperfectPlan(zone.format, zone.format)),
                              TimelineNs{8ULL * kPacketNs}));

    output->pump();
    CHECK_EQ(output->packets(), 5ULL);
    CHECK_EQ(output->formatSwitches(), 0ULL);

    output->pump();
    CHECK_EQ(output->packets(), 8ULL);
    CHECK_EQ(output->formatSwitches(), 1ULL);
    CHECK(output->plan().mode == OutputMode::Resampled);

    output->pump();
    CHECK_EQ(output->packets(), 10ULL);
    CHECK_EQ(output->formatSwitches(), 2ULL);
    CHECK(output->plan().mode == OutputMode::BitPerfect);
}
