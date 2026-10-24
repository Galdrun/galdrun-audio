// One decode, many rooms -- the roadmap's criterion 15, asserted rather than
// argued.
//
// Nothing here opens a socket, a file or a device. The decoder is a generator
// whose every sample is decided in this file and which counts what it was
// asked for, so "two rooms were served from one decode" is a number and not a
// belief; the sink is an interface, so "a room that stopped reading" is a
// fixture rather than a race against the operating system's send buffer; and
// the zone's own pipeline is driven by calling pump(), so the decode is
// deterministic and no test sleeps through it.
//
// The rooms do run on their own threads, because that is the design and
// admit() starts them. Assertions about them go through waitFor, which bounds
// a hang and measures nothing -- the same shape test_client_output.cpp uses.

#include "ManualClock.h"
#include "TestSupport.h"

#include "audio/BlockPool.h"
#include "client/OutputPlanner.h"
#include "matp/AudioPacket.h"
#include "resampler/FakeResampler.h"
#include "resampler/ResamplerRegistry.h"
#include "zone/Zone.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace multiroom;

namespace {

constexpr uint32_t kRate = 44100;
constexpr uint16_t kChannels = 2;
constexpr uint32_t kPacketMs = 20;
constexpr uint32_t kPacketFrames = 882; // 20 ms at 44.1 kHz, exactly
constexpr uint64_t kPacketNs = 20000000ULL;

// Three seconds of source, and a lookahead and a retention that both hold all
// of it. That is what makes these tests deterministic: the zone decodes the
// whole source on the first pump, no room is ever paced into waiting, and
// nothing is evicted, so what a room received can be compared against what was
// generated rather than against what happened to survive.
constexpr uint64_t kSourceFrames = kRate * 3;
constexpr uint64_t kSourcePackets = kSourceFrames / kPacketFrames; // 150, exactly

constexpr auto kDeadline = std::chrono::seconds(5);

AudioFormat formatOf(uint32_t rate = kRate, uint16_t channels = kChannels,
                     AudioFormat::SampleType type = AudioFormat::SampleType::Int16)
{
    AudioFormat format;
    format.sampleRate = rate;
    format.channels = channels;
    format.sampleType = type;
    return format;
}

SessionId sessionOf(uint64_t low)
{
    return SessionId(0x0123456789abcdefULL, low);
}

// The signal, decided here and nowhere else. Every sample is a function of its
// absolute frame and channel, so a packet carrying the wrong frames is caught
// by its contents rather than by its count.
int16_t sampleAt(uint64_t frame, uint16_t channel)
{
    return static_cast<int16_t>(((frame * 37 + channel * 7919) & 0x7FFF) - 0x4000);
}

void writeRamp(std::span<std::byte> out, uint64_t firstFrame, uint32_t frames, uint16_t channels)
{
    size_t at = 0;
    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        for (uint16_t channel = 0; channel < channels; ++channel)
        {
            const uint16_t bits = static_cast<uint16_t>(sampleAt(firstFrame + frame, channel));
            out[at++] = static_cast<std::byte>(bits & 0xFF);
            out[at++] = static_cast<std::byte>((bits >> 8) & 0xFF);
        }
    }
}

// --- the decoder, and what it counts ---------------------------------------

struct DecoderCounters
{
    std::atomic<uint64_t> opens{0};
    std::atomic<uint64_t> decodeCalls{0};
    std::atomic<uint64_t> framesDecoded{0};
    std::atomic<uint64_t> lastStartFrame{0};
    std::atomic<bool> refuseOpens{false};
};

class RampDecoder final : public IDecoder
{
public:
    RampDecoder(AudioFormat format, uint64_t frames, DecoderCounters& counters,
                uint64_t start = 0)
        : counters_(counters), total_(frames), next_(start)
    {
        info_.format = format;
        info_.totalFrames = frames;
        info_.codecName = "ramp";
        info_.containerName = "generated";
    }

    Status<DecodeError> open(std::unique_ptr<IMediaSource>) override { return {}; }
    bool opened() const noexcept override { return true; }
    const SourceStreamInfo& info() const noexcept override { return info_; }

    Result<DecodedSpan, DecodeError> decode(std::span<std::byte> output) override
    {
        counters_.decodeCalls.fetch_add(1);
        if (next_ >= total_)
            return fail(DecodeError::EndOfStream, "the ramp is finished");

        const uint32_t bytesPerFrame = info_.format.bytesPerFrame();
        const uint32_t fits = static_cast<uint32_t>(output.size() / bytesPerFrame);
        if (fits == 0)
            return fail(DecodeError::BufferTooSmall, "not one frame of room");

        // A real codec hands back one of its own frames at a time and rarely
        // fills the caller's buffer, so this does not either -- that is what
        // exercises the pipeline's fill loop instead of pretending it away.
        const uint32_t frames = static_cast<uint32_t>(
            std::min<uint64_t>({fits, kCodecFrames, total_ - next_}));

        writeRamp(output, next_, frames, info_.format.channels);

        DecodedSpan span;
        span.firstFrame = SourceFrame{next_};
        span.frameCount = frames;
        span.byteCount = static_cast<size_t>(frames) * bytesPerFrame;

        next_ += frames;
        counters_.framesDecoded.fetch_add(frames);
        return span;
    }

private:
    static constexpr uint64_t kCodecFrames = 1024;

    DecoderCounters& counters_;
    SourceStreamInfo info_;
    uint64_t total_ = 0;
    uint64_t next_ = 0;
};

// Every URI this factory knows, and the one that is not a URI at all.
class RampDecoderFactory final : public IDecoderFactory
{
public:
    explicit RampDecoderFactory(AudioFormat format = formatOf(),
                                uint64_t frames = kSourceFrames)
        : format_(format), frames_(frames)
    {
    }

    Result<std::unique_ptr<IDecoder>, DecodeError> open(const std::string& uri,
                                                        uint64_t startOffsetNs) override
    {
        counters.opens.fetch_add(1);
        if (uri == "missing")
            return fail(DecodeError::SourceFailed, "NotFound: no such thing as " + uri);
        if (counters.refuseOpens.load())
            return fail(DecodeError::SourceFailed, "ServerError: 503 for " + uri);

        // "other" is the same generator in another format, which is what the
        // format-transition case needs, and "hires" is the ceiling format a
        // zone's memory is sized for (phase 42).
        const AudioFormat format =
            uri == "other"   ? formatOf(48000)
            : uri == "hires" ? formatOf(192000, kChannels, AudioFormat::SampleType::Int32)
                             : format_;

        // Phase 26: a ramp begins wherever it is opened, except "live", which
        // is a stream and begins only at its start.
        if (startOffsetNs != 0 && uri == "live")
            return fail(DecodeError::NotSeekable, uri + " cannot seek");
        const uint64_t start =
            framesFromNanoseconds(startOffsetNs, format.sampleRate).value_or(frames_);
        if (start >= frames_)
            return fail(DecodeError::OffsetOutOfRange, uri + ": frame " + std::to_string(start));
        counters.lastStartFrame.store(start);
        return std::unique_ptr<IDecoder>(
            std::make_unique<RampDecoder>(format, frames_, counters, start));
    }

    DecoderCounters counters;

private:
    AudioFormat format_;
    uint64_t frames_ = 0;
};

// --- the room, reduced to the bytes it was sent -----------------------------

struct SinkState
{
    std::mutex mutex;
    std::string received;

    std::atomic<bool> accepting{true};
    std::atomic<uint64_t> refusals{0};
};

class FakeSink final : public IPacketSink
{
public:
    explicit FakeSink(std::shared_ptr<SinkState> state) : state_(std::move(state)) {}

    Result<size_t, PacketSinkError> write(std::span<const std::byte> bytes) override
    {
        if (!state_->accepting.load())
        {
            // A peer whose receive window is shut. Zero bytes taken, which is
            // not a failure -- it is what a room that stopped reading looks
            // like from here.
            state_->refusals.fetch_add(1);
            return size_t{0};
        }

        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return bytes.size();
    }

    void close() override {}

private:
    std::shared_ptr<SinkState> state_;
};

// --- the messages a zone decided on -----------------------------------------

// Every message the zone handed to its sender, kept rather than written: there
// is no session behind these rooms, and what a test wants to see is the
// decision -- which room, which kind, which position.
struct CommandLog
{
    std::mutex mutex;
    std::vector<RoomCommand> sent;

    std::vector<RoomCommand> all() const
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex));
        return sent;
    }

    std::vector<RoomCommand> of(RoomCommandKind kind) const
    {
        std::vector<RoomCommand> matching;
        for (const RoomCommand& command : all())
        {
            if (command.kind == kind)
                matching.push_back(command);
        }
        return matching;
    }

    std::vector<RoomCommand> of(RoomCommandKind kind, const char* client) const
    {
        std::vector<RoomCommand> matching;
        for (const RoomCommand& command : of(kind))
        {
            if (command.client.str() == client)
                matching.push_back(command);
        }
        return matching;
    }

    size_t count(RoomCommandKind kind) const { return of(kind).size(); }

    std::optional<matp::StartAt> forRoom(const char* client) const
    {
        const std::vector<RoomCommand> starts = of(RoomCommandKind::Start, client);
        if (starts.empty())
            return std::nullopt;
        return starts.front().start;
    }
};

// Everything a zone needs that is not the zone: the memory ceiling its blocks
// are counted against, the registry with the one converter a test may use,
// and the planner built on it.
struct ZoneRig
{
    explicit ZoneRig(AudioFormat sourceFormat = formatOf(), uint64_t frames = kSourceFrames)
        : factory(sourceFormat, frames)
    {
        // FakeResampler is never registered by the server's own startup, and a
        // test that wants one adds it itself.
        CHECK_OK(resamplers.add(std::make_unique<FakeResamplerFactory>()));

        auto built = OutputPlanner::create(resamplers, "fake");
        CHECK_OK(built);
        planner.emplace(std::move(*built));
    }

    ZoneSettings settings() const
    {
        ZoneSettings out;
        out.output.packetDurationMs = kPacketMs;

        // Ten seconds of target and lookahead against three of source: every
        // room may produce the whole thing without ever being paced, and the
        // zone decodes all of it on the first pump.
        out.output.targetBufferNs = 10ULL * kNanosecondsPerSecond;
        out.output.maxQueuedPackets = 1024;
        out.retentionNs = 60ULL * kNanosecondsPerSecond;

        out.sendCommand = [starts = starts](const RoomCommand& command) {
            std::lock_guard<std::mutex> lock(starts->mutex);
            starts->sent.push_back(command);
            return true;
        };
        return out;
    }

    std::unique_ptr<Zone> zone(ZoneSettings custom) const
    {
        auto created = Zone::create(ZoneId("living"), memory, *planner, resamplers, factory,
                                    clock, custom);
        CHECK_OK(created);
        return std::move(*created);
    }

    std::unique_ptr<Zone> zone() const { return zone(settings()); }

    mutable RampDecoderFactory factory;
    multiroomtest::ManualClock clock;
    mutable AudioMemory memory{blockBytesFor(192000, 2, kPacketMs), 256ULL * 1024 * 1024};
    mutable ResamplerRegistry resamplers;
    std::optional<OutputPlanner> planner;

    // Shared rather than owned, because the sender outlives the settings it
    // was copied into and a test reads it after the zone has used it.
    std::shared_ptr<CommandLog> starts = std::make_shared<CommandLog>();
};

SourceSegment segmentOf(const char* id, const char* uri)
{
    SourceSegment segment;
    segment.sourceId = id;
    segment.mediaUri = uri;
    return segment;
}

ClientCapabilities offering(std::vector<AudioFormat> formats)
{
    ClientCapabilities capabilities;
    capabilities.formats = std::move(formats);
    return capabilities;
}

// Attaches a sink to a room and hands back what that room was sent.
std::shared_ptr<SinkState> connect(Zone& zone, const SessionId& session, const ClientId& client)
{
    auto state = std::make_shared<SinkState>();
    StreamAttachment attachment;
    attachment.session = session;
    attachment.client = client;
    CHECK(zone.attach(attachment, std::make_unique<FakeSink>(state)));
    return state;
}

// Every packet the sink has seen, decoded with the client's own reader -- so a
// header field that is wrong is wrong according to the parser on the other end
// of the socket rather than according to this file.
std::vector<matp::AudioPacketHeader> decodeHeaders(SinkState& state, std::string& payloads)
{
    std::string received;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        received = state.received;
    }

    matp::AudioPacketReader reader;
    reader.append(received.data(), received.size());

    std::vector<matp::AudioPacketHeader> headers;
    payloads.clear();
    for (;;)
    {
        matp::AudioPacket packet;
        std::string error;
        const matp::AudioPacketReader::Result result = reader.next(packet, error);
        if (result == matp::AudioPacketReader::Result::Packet)
        {
            headers.push_back(packet.header);
            payloads.append(reinterpret_cast<const char*>(packet.payload), packet.payloadBytes);
            continue;
        }
        if (result == matp::AudioPacketReader::Result::Invalid)
            multiroomtest::failAt("the client refused this room's stream: " + error, __FILE__,
                            __LINE__);
        break;
    }
    return headers;
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

// Pumps the pipeline until it has nothing left to do, which with this rig is
// the whole source.
void decodeEverything(Zone& zone)
{
    zone.pump();
}

} // namespace

// ---------------------------------------------------------------------------

MULTIROOM_TEST(zone_refuses_settings_it_could_not_serve)
{
    ZoneRig rig;

    CHECK_ERR(Zone::create(ZoneId(), rig.memory, *rig.planner, rig.resamplers, rig.factory,
                           rig.clock, rig.settings()),
              ZoneError::InvalidSettings);

    ZoneSettings noPacket = rig.settings();
    noPacket.output.packetDurationMs = 0;
    CHECK_ERR(Zone::create(ZoneId("living"), rig.memory, *rig.planner, rig.resamplers,
                           rig.factory, rig.clock, noPacket),
              ZoneError::InvalidSettings);

    // A room reads the oldest block the zone still holds, so a lookahead the
    // retention cannot cover would evict audio out from under a room that has
    // done nothing wrong.
    ZoneSettings tooFar = rig.settings();
    tooFar.lookaheadNs = tooFar.retentionNs;
    CHECK_ERR(Zone::create(ZoneId("living"), rig.memory, *rig.planner, rig.resamplers,
                           rig.factory, rig.clock, tooFar),
              ZoneError::InvalidSettings);
}

MULTIROOM_TEST(zone_refuses_a_segment_that_names_nothing)
{
    ZoneRig rig;
    auto zone = rig.zone();

    CHECK_ERR(zone->stage(segmentOf("", "ramp")), ZoneError::InvalidSegment);
    CHECK_ERR(zone->stage(segmentOf("one", "")), ZoneError::InvalidSegment);

    // The core never chooses what to play, so a zone with nothing staged has
    // nothing to start.
    CHECK_ERR(zone->play(), ZoneError::NothingStaged);
    CHECK(zone->state() == ZoneState::Idle);
    CHECK_EQ(rig.factory.counters.opens.load(), 0u);
}

MULTIROOM_TEST(zone_plays_a_generated_source_to_one_room_contiguously)
{
    ZoneRig rig;
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());
    CHECK(zone->state() == ZoneState::Playing);
    CHECK_EQ(describeFormat(zone->sourceFormat()), describeFormat(formatOf()));

    // The zone's timeline starts at zero when it starts playing (section 5).
    const std::vector<StagedSource> staged = zone->sources();
    CHECK_EQ(staged.size(), 1u);
    CHECK_EQ(std::to_underlying(staged.front().timelineStartNs), 0u);
    CHECK_EQ(std::to_underlying(staged.front().firstFrame), 0u);

    const ClientId room("livingroom");
    const SessionId session = sessionOf(1);
    CHECK_OK(zone->admit(session, room, offering({formatOf()}), formatOf()));

    const std::shared_ptr<ClientOutput> output = zone->member(room);
    CHECK(output != nullptr);

    // The device offers the source's own layout, so this room is bitperfect
    // and its PCM never meets a converter at all.
    CHECK(output->plan().mode == OutputMode::BitPerfect);
    CHECK(output->passThrough());

    auto sink = connect(*zone, session, room);
    decodeEverything(*zone);

    CHECK(waitFor([&] { return output->packets() >= kSourcePackets; }));
    CHECK(zone->state() == ZoneState::Ended);

    std::string payloads;
    const std::vector<matp::AudioPacketHeader> headers = decodeHeaders(*sink, payloads);
    CHECK_EQ(headers.size(), static_cast<size_t>(kSourcePackets));

    for (size_t index = 0; index < headers.size(); ++index)
    {
        const matp::AudioPacketHeader& header = headers[index];

        // Absolute, never accumulated: packet n covers exactly
        // [n * 20 ms, (n + 1) * 20 ms) and consecutive packets are contiguous
        // by construction rather than by rounding.
        CHECK_EQ(header.timelineStartNs, static_cast<uint64_t>(index) * kPacketNs);
        CHECK_EQ(header.timelineDurationNs, kPacketNs);
        CHECK_EQ(header.sequenceNumber, static_cast<uint64_t>(index));
        CHECK_EQ(header.outputFrameCount, kPacketFrames);
        CHECK_EQ(describeFormat(header.format), describeFormat(formatOf()));
        CHECK_EQ(header.sessionIdHigh, session.high());
        CHECK_EQ(header.sessionIdLow, session.low());
    }

    // Complete, and the frames that were generated rather than some other
    // frames of the right length.
    std::vector<std::byte> expected(static_cast<size_t>(kSourceFrames) *
                                    formatOf().bytesPerFrame());
    writeRamp(expected, 0, static_cast<uint32_t>(kSourceFrames), kChannels);
    CHECK_EQ(payloads.size(), expected.size());
    CHECK_EQ(std::memcmp(payloads.data(), expected.data(), expected.size()), 0);

    // One source, opened once.
    CHECK_EQ(rig.factory.counters.opens.load(), 1u);
    CHECK_EQ(rig.factory.counters.framesDecoded.load(), kSourceFrames);
}

MULTIROOM_TEST(two_rooms_with_different_capabilities_share_one_decode)
{
    ZoneRig rig;
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    // One device that can play the source exactly, and one that offers only
    // 48 kHz in a wider container.
    const AudioFormat resampled = formatOf(48000, kChannels, AudioFormat::SampleType::Int32);

    const ClientId bitperfect("livingroom");
    const ClientId converted("kitchen");
    const SessionId bitperfectSession = sessionOf(1);
    const SessionId convertedSession = sessionOf(2);

    CHECK_OK(zone->admit(bitperfectSession, bitperfect, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(convertedSession, converted, offering({resampled}), resampled));

    const std::shared_ptr<ClientOutput> a = zone->member(bitperfect);
    const std::shared_ptr<ClientOutput> b = zone->member(converted);
    CHECK(a != nullptr);
    CHECK(b != nullptr);

    // Two plans out of one source format, and the second one names the
    // converter this server was built with.
    CHECK(a->plan().mode == OutputMode::BitPerfect);
    CHECK(a->plan().resamplerId.empty());
    CHECK(b->plan().mode == OutputMode::Resampled);
    CHECK_EQ(b->plan().resamplerId, std::string("fake"));
    CHECK_EQ(describeFormat(b->plan().outputFormat), describeFormat(resampled));

    auto sinkA = connect(*zone, bitperfectSession, bitperfect);
    auto sinkB = connect(*zone, convertedSession, converted);

    decodeEverything(*zone);

    CHECK(waitFor([&] { return a->packets() >= kSourcePackets; }));

    // The resampled room's last output frame is interpolated from an input
    // frame just past the last one the source has. Nothing is staged after
    // it, so that frame is silence and the room finishes like the other.
    CHECK(waitFor([&] { return b->packets() >= kSourcePackets; }));

    // The claim of this phase, as a number: two rooms, two formats, one
    // decode. The source was opened once and decoded once through -- serving
    // the second room cost the decoder nothing.
    CHECK_EQ(rig.factory.counters.opens.load(), 1u);
    CHECK_EQ(rig.factory.counters.framesDecoded.load(), kSourceFrames);

    std::string payloadsA;
    std::string payloadsB;
    const std::vector<matp::AudioPacketHeader> headersA = decodeHeaders(*sinkA, payloadsA);
    const std::vector<matp::AudioPacketHeader> headersB = decodeHeaders(*sinkB, payloadsB);

    CHECK_EQ(headersA.size(), static_cast<size_t>(kSourcePackets));
    CHECK_EQ(headersB.size(), static_cast<size_t>(kSourcePackets));

    for (size_t index = 0; index < headersB.size(); ++index)
    {
        // Every room's copy carries the same two timeline numbers -- that is
        // what makes them one zone -- while the frames and the format are its
        // own.
        CHECK_EQ(headersA[index].timelineStartNs, headersB[index].timelineStartNs);
        CHECK_EQ(headersA[index].timelineDurationNs, headersB[index].timelineDurationNs);

        CHECK_EQ(describeFormat(headersA[index].format), describeFormat(formatOf()));
        CHECK_EQ(describeFormat(headersB[index].format), describeFormat(resampled));
        CHECK_EQ(headersA[index].outputFrameCount, kPacketFrames);
        CHECK_EQ(headersB[index].outputFrameCount, 960u); // 20 ms at 48 kHz
    }

    // Two different streams: the same span of timeline, a different number of
    // bytes in it.
    CHECK(payloadsA.size() != payloadsB.size());
    CHECK_EQ(payloadsB.size(), headersB.size() * 960u * resampled.bytesPerFrame());
    CHECK_EQ(payloadsA.size(), headersA.size() * kPacketFrames * formatOf().bytesPerFrame());
}

MULTIROOM_TEST(a_room_that_stops_reading_blocks_neither_the_zone_nor_the_other_room)
{
    ZoneRig rig;

    // A queue small enough that the silent room fills it long before the
    // source runs out, which is the whole point: it has to be *full* for its
    // isolation to mean anything.
    ZoneSettings settings = rig.settings();
    settings.output.maxQueuedPackets = 4;
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId silent("bathroom");
    const ClientId reading("livingroom");
    const SessionId silentSession = sessionOf(1);
    const SessionId readingSession = sessionOf(2);

    CHECK_OK(zone->admit(silentSession, silent, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(readingSession, reading, offering({formatOf()}), formatOf()));

    auto silentSink = connect(*zone, silentSession, silent);
    silentSink->accepting.store(false);
    auto readingSink = connect(*zone, readingSession, reading);

    decodeEverything(*zone);

    // The room that reads gets the whole source at its full rate.
    const std::shared_ptr<ClientOutput> good = zone->member(reading);
    CHECK(waitFor([&] { return good->packets() >= kSourcePackets; }));

    // The room that does not gets its own queue full and nothing else: its
    // failure is visible on itself, and the zone and its neighbour are
    // untouched.
    const std::shared_ptr<ClientOutput> bad = zone->member(silent);
    CHECK(waitFor([&] { return bad->queuedPackets() >= settings.output.maxQueuedPackets; }));
    CHECK_EQ(bad->packets(), 0u);
    CHECK(silentSink->refusals.load() > 0);

    CHECK(zone->state() == ZoneState::Ended);
    CHECK_EQ(zone->lastError(), std::string());
    CHECK(good->state() == OutputState::Streaming);
}

MULTIROOM_TEST(membership_never_restarts_the_zone)
{
    ZoneRig rig;
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId first("livingroom");
    const SessionId firstSession = sessionOf(1);
    CHECK_OK(zone->admit(firstSession, first, offering({formatOf()}), formatOf()));
    auto firstSink = connect(*zone, firstSession, first);

    decodeEverything(*zone);
    const std::shared_ptr<ClientOutput> a = zone->member(first);
    CHECK(waitFor([&] { return a->packets() >= kSourcePackets; }));

    const uint64_t decodedBefore = rig.factory.counters.framesDecoded.load();
    const uint64_t blocksBefore = zone->publishedBlocks();

    // A second room arrives after the first has heard everything. Nothing
    // about the zone rewinds: the timeline keeps its origin, the store is not
    // re-decoded, and the first room's stream carries on untouched.
    const ClientId second("kitchen");
    const SessionId secondSession = sessionOf(2);
    CHECK_OK(zone->admit(secondSession, second, offering({formatOf()}), formatOf()));
    auto secondSink = connect(*zone, secondSession, second);

    decodeEverything(*zone);

    const std::shared_ptr<ClientOutput> b = zone->member(second);
    CHECK(waitFor([&] { return b->packets() >= kSourcePackets; }));

    CHECK_EQ(rig.factory.counters.framesDecoded.load(), decodedBefore);
    CHECK_EQ(zone->publishedBlocks(), blocksBefore);
    CHECK_EQ(rig.factory.counters.opens.load(), 1u);

    std::string payloads;
    const std::vector<matp::AudioPacketHeader> headers = decodeHeaders(*secondSink, payloads);
    CHECK_EQ(headers.size(), static_cast<size_t>(kSourcePackets));
    CHECK_EQ(headers.front().timelineStartNs, 0u);

    CHECK_EQ(a->packets(), kSourcePackets);
    CHECK(a->state() == OutputState::Streaming);

    // And leaving stops that room's output and nothing else.
    CHECK_OK(zone->release(first));
    CHECK_EQ(zone->memberCount(), 1u);
    CHECK(zone->member(first) == nullptr);
    CHECK(b->state() == OutputState::Streaming);
    CHECK_ERR(zone->release(first), ZoneError::NotMember);
}

MULTIROOM_TEST(a_zone_that_runs_out_of_sources_ends_and_a_new_one_continues_the_timeline)
{
    ZoneRig rig;
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    decodeEverything(*zone);

    // The core plays what it has and stops; it never chooses the next one.
    CHECK(zone->state() == ZoneState::Ended);
    CHECK_EQ(zone->lastError(), std::string());
    CHECK_EQ(zone->stagedCount(), 0u);

    // Staging another reopens it, and the timeline carries on from where the
    // first one ended rather than restarting -- section 22's gapless boundary
    // as a property of the arithmetic.
    CHECK_OK(zone->stage(segmentOf("two", "ramp")));
    CHECK(zone->state() == ZoneState::Playing);
    decodeEverything(*zone);

    const std::vector<StagedSource> staged = zone->sources();
    CHECK_EQ(staged.size(), 2u);
    CHECK_EQ(std::to_underlying(staged[0].timelineStartNs), 0u);
    CHECK(staged[0].ended);
    CHECK_EQ(staged[0].frames, kSourceFrames);

    const auto expectedStart = nanosecondsFromFrames(kSourceFrames, kRate);
    CHECK_OK(expectedStart);
    CHECK_EQ(std::to_underlying(staged[1].timelineStartNs), *expectedStart);
    CHECK_EQ(std::to_underlying(staged[1].firstFrame), kSourceFrames);

    CHECK_EQ(rig.factory.counters.opens.load(), 2u);
}

MULTIROOM_TEST(a_source_that_cannot_be_opened_is_skipped_and_named)
{
    ZoneRig rig;
    auto zone = rig.zone();

    // Nothing but a source that will not open: play() says which and why, and
    // the zone is left where it was rather than failed for good (phase 22).
    CHECK_OK(zone->stage(segmentOf("gone", "missing")));
    const auto refused = zone->play();
    CHECK_ERR(refused, ZoneError::SourceFailed);
    if (!refused)
    {
        CHECK(refused.error().detail.find("gone") != std::string::npos);
        CHECK(refused.error().detail.find("NotFound") != std::string::npos);
    }
    CHECK(zone->state() == ZoneState::Idle);
    CHECK(zone->lastError().empty());

    // No source, no format to plan against: nobody is admitted to be sent
    // nothing.
    CHECK(!zone->admit(sessionOf(1), ClientId("livingroom"), offering({formatOf()}),
                       formatOf()));

    // A broken source ahead of a good one is skipped for it, with nothing on
    // the timeline for the one that never opened.
    CHECK_OK(zone->stage(segmentOf("gone again", "missing")));
    CHECK_OK(zone->stage(segmentOf("fine", "ramp")));
    CHECK_OK(zone->play());
    CHECK(zone->state() == ZoneState::Playing);

    const std::vector<StagedSource> opened = zone->sources();
    CHECK_EQ(opened.size(), 1u);
    CHECK_EQ(opened[0].segment.sourceId, std::string("fine"));
    CHECK_EQ(std::to_underlying(opened[0].timelineStartNs), 0u);

    const std::vector<FailedSource> failed = zone->failedSources();
    CHECK_EQ(failed.size(), 2u);
    CHECK_EQ(failed[0].sourceId, std::string("gone"));
    CHECK_EQ(failed[1].sourceId, std::string("gone again"));
    CHECK(failed[1].reason.find("NotFound") != std::string::npos);
}

MULTIROOM_TEST(a_zone_keeps_only_the_most_recent_failed_sources)
{
    ZoneRig rig;
    auto zone = rig.zone();

    for (size_t index = 0; index < Zone::kMaxFailedSources + 3; ++index)
    {
        const std::string id = "gone " + std::to_string(index);
        CHECK_OK(zone->stage(segmentOf(id.c_str(), "missing")));
    }
    CHECK_OK(zone->stage(segmentOf("fine", "ramp")));
    CHECK_OK(zone->play());

    const std::vector<FailedSource> failed = zone->failedSources();
    CHECK_EQ(failed.size(), Zone::kMaxFailedSources);
    CHECK_EQ(failed.front().sourceId, std::string("gone 3"));
    CHECK_EQ(failed.back().sourceId,
             "gone " + std::to_string(Zone::kMaxFailedSources + 2));
}

MULTIROOM_TEST(a_room_the_zone_cannot_serve_is_refused_and_the_zone_keeps_playing)
{
    ZoneRig rig;
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId served("livingroom");
    CHECK_OK(zone->admit(sessionOf(1), served, offering({formatOf()}), formatOf()));

    // Nothing in this server changes a channel count, so a device that offers
    // only mono has no plan to be made for it. That room's failure and not the
    // zone's -- the fault-isolation table's second row.
    const AudioFormat mono = formatOf(kRate, 1);
    CHECK_ERR(zone->admit(sessionOf(2), ClientId("kitchen"), offering({mono}), mono),
              ZoneError::PlanFailed);

    CHECK_EQ(zone->memberCount(), 1u);
    CHECK(zone->state() == ZoneState::Playing);
    CHECK_EQ(zone->lastError(), std::string());

    // And the same room twice is refused rather than given a second stream.
    CHECK_ERR(zone->admit(sessionOf(3), served, offering({formatOf()}), formatOf()),
              ZoneError::AlreadyMember);
}

MULTIROOM_TEST(a_zone_with_no_members_decodes_its_lookahead_and_stops)
{
    ZoneRig rig;

    // A lookahead far shorter than the source, so that "it stopped" is
    // distinguishable from "it ran out".
    ZoneSettings settings = rig.settings();
    settings.lookaheadNs = 500ULL * 1000000ULL;
    settings.retentionNs = 60ULL * kNanosecondsPerSecond;
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    decodeEverything(*zone);
    CHECK(zone->state() == ZoneState::Playing);

    // Within one block of the lookahead, and nowhere near the whole source: an
    // idle zone costs one lookahead of store and then nothing at all.
    const uint64_t frontier = std::to_underlying(zone->frontierNs());
    CHECK(frontier >= settings.lookaheadNs);
    CHECK(frontier < settings.lookaheadNs + 100ULL * 1000000ULL);

    // Pumping again changes nothing, because nothing is asking for audio.
    const uint64_t blocks = zone->publishedBlocks();
    decodeEverything(*zone);
    CHECK_EQ(zone->publishedBlocks(), blocks);

    // A room joining is what moves it on, and it joins where the zone's audio
    // still begins.
    const ClientId room("livingroom");
    const SessionId session = sessionOf(1);
    CHECK_OK(zone->admit(session, room, offering({formatOf()}), formatOf()));
    auto sink = connect(*zone, session, room);

    const std::shared_ptr<ClientOutput> output = zone->member(room);
    CHECK(waitFor([&] { return output->packets() > 0; }));

    std::string payloads;
    const std::vector<matp::AudioPacketHeader> headers = decodeHeaders(*sink, payloads);
    CHECK(!headers.empty());
    CHECK_EQ(headers.front().timelineStartNs, 0u);

    // The decode moves on because the room has, and it stays one lookahead in
    // front of it rather than racing to the end of the source.
    for (int pass = 0; pass < 200; ++pass)
    {
        decodeEverything(*zone);
        if (zone->state() == ZoneState::Ended)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(waitFor([&] { return output->packets() >= kSourcePackets; }));
    CHECK_EQ(rig.factory.counters.framesDecoded.load(), kSourceFrames);
}

MULTIROOM_TEST(an_attachment_for_another_session_is_refused)
{
    ZoneRig rig;
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId room("livingroom");
    CHECK_OK(zone->admit(sessionOf(1), room, offering({formatOf()}), formatOf()));

    auto state = std::make_shared<SinkState>();

    // A socket naming a room this zone does not have.
    StreamAttachment stranger;
    stranger.session = sessionOf(1);
    stranger.client = ClientId("garage");
    CHECK(!zone->attach(stranger, std::make_unique<FakeSink>(state)));

    // A socket naming the right room under a session that has already been
    // torn down and replaced. It is a socket to close, not a room to feed.
    StreamAttachment stale;
    stale.session = sessionOf(9);
    stale.client = room;
    CHECK(!zone->attach(stale, std::make_unique<FakeSink>(state)));

    StreamAttachment good;
    good.session = sessionOf(1);
    good.client = room;
    CHECK(zone->attach(good, std::make_unique<FakeSink>(state)));
}

// --- phase 11: the scheduled start ------------------------------------------

namespace {

// Section 19, as a room that holds `bufferedNs` beginning at `fromNs` reports
// it -- plus the completed clock exchanges the same connection carries, which
// is the third of section 20's conditions.
void reportBuffer(Zone& zone, const ClientId& client, uint64_t fromNs, uint64_t bufferedNs,
                  uint64_t exchanges = 20)
{
    matp::BufferStatus status;
    status.hasContent = true;
    status.playheadTimelineNs = fromNs;
    status.bufferedUntilTimelineNs = fromNs + bufferedNs;
    status.bufferedDurationNs = bufferedNs;
    zone.onBufferStatus(client, status, exchanges);
}

// How far a room's client holds its stream: the end of the last whole packet
// its sink took. That is what a real client can report. It is read while
// nothing is queued here and the sink is the same on both sides of that
// check, because a packet still in the queue -- or written but not yet popped
// -- is counted against the target from the report on, and counting it inside
// the report as well stops the room short. A packet built but not yet queued
// is in neither, and is counted once when it is.
uint64_t deliveredThroughNs(const ClientOutput& output, SinkState& sink)
{
    const auto lastEnd = [&] {
        std::string payloads;
        const std::vector<matp::AudioPacketHeader> headers = decodeHeaders(sink, payloads);
        return headers.empty() ? uint64_t{0}
                               : headers.back().timelineStartNs + headers.back().timelineDurationNs;
    };
    uint64_t delivered = 0;
    CHECK(waitFor([&] {
        delivered = lastEnd();
        return output.queuedPackets() == 0 && lastEnd() == delivered;
    }));
    return delivered;
}

} // namespace

MULTIROOM_TEST(zone_refuses_to_exist_without_somewhere_to_send_a_start)
{
    ZoneRig rig;

    // A zone that cannot start a room is a zone that cannot play, and the
    // barrier is five seconds too late to find that out.
    ZoneSettings mute = rig.settings();
    mute.sendCommand = nullptr;
    CHECK_ERR(Zone::create(ZoneId("living"), rig.memory, *rig.planner, rig.resamplers,
                           rig.factory, rig.clock, mute),
              ZoneError::InvalidSettings);

    // And one whose late-join lead does not fit inside the retention, because
    // a latecomer is the room the decode runs a lookahead past.
    ZoneSettings tooFar = rig.settings();
    tooFar.lateJoinLeadNs = tooFar.retentionNs;
    CHECK_ERR(Zone::create(ZoneId("living"), rig.memory, *rig.planner, rig.resamplers,
                           rig.factory, rig.clock, tooFar),
              ZoneError::InvalidSettings);

    // The latecomer's own prebuffer counts too, and leaving it out of the sum
    // is what the first two-room run found: this lead clears the lookahead
    // with room to spare and still evicts the room that was playing, because
    // the joiner produces its whole target before anything paces it.
    ZoneSettings exactly = rig.settings();
    exactly.output.targetBufferNs = 10ULL * kNanosecondsPerSecond;
    exactly.lookaheadNs = 10ULL * kNanosecondsPerSecond;
    exactly.retentionNs = 25ULL * kNanosecondsPerSecond;
    exactly.lateJoinLeadNs = 6ULL * kNanosecondsPerSecond; // 6 + 10 + 10 > 25
    CHECK_ERR(Zone::create(ZoneId("living"), rig.memory, *rig.planner, rig.resamplers,
                           rig.factory, rig.clock, exactly),
              ZoneError::InvalidSettings);

    // ...and one packet less is the same three numbers fitting.
    exactly.lateJoinLeadNs = 4ULL * kNanosecondsPerSecond;
    CHECK_OK(Zone::create(ZoneId("living"), rig.memory, *rig.planner, rig.resamplers,
                          rig.factory, rig.clock, exactly));
}

MULTIROOM_TEST(a_derived_late_join_lead_leaves_room_for_the_joiners_own_prebuffer)
{
    // Section 46's defaults, which is what a deployment that sets nothing
    // gets: 5 s of retention, a 1500 ms prebuffer and the lookahead derived
    // from it. The frontier runs to `lead + prebuffer + lookahead` past what
    // is audible, and the retention has to cover all three -- the earlier
    // derivation covered two of them and left one packet of margin, which a
    // real room ate in under a second.
    ZoneRig rig;

    ZoneSettings defaults = rig.settings();
    defaults.output.targetBufferNs = 1500ULL * 1000000ULL;
    defaults.retentionNs = 5ULL * kNanosecondsPerSecond;
    defaults.lookaheadNs = 0;      // derived: target + one packet
    defaults.lateJoinLeadNs = 0;   // derived: what is left of the retention

    auto zone = Zone::create(ZoneId("living"), rig.memory, *rig.planner, rig.resamplers,
                             rig.factory, rig.clock, defaults);
    CHECK_OK(zone);

    // 5000 - (1520 + 1500 + 20) = 1960 ms, and the sum below the retention
    // rather than level with it.
    CHECK_OK((*zone)->stage(segmentOf("one", "ramp")));
    CHECK_OK((*zone)->play());

    const ClientId room("living");
    CHECK_OK((*zone)->admit(sessionOf(1), room, offering({formatOf()}), formatOf()));
    reportBuffer(**zone, room, 0, 1500 * 1000000ULL);
    (*zone)->pump();
    CHECK((*zone)->started());

    matp::PlaybackReport audible;
    audible.clockQuality = AudioClockQuality::Hardware;
    audible.hasTimelineNsAudible = true;
    audible.timelineNsAudible = std::to_underlying((*zone)->startTimelineNs());
    (*zone)->onPlaybackReport(room, audible, ClientClockModel{});

    rig.clock.advanceMs(1000);
    const uint64_t playhead = std::to_underlying((*zone)->playheadNs());

    const ClientId late("kitchen");
    CHECK_OK((*zone)->admit(sessionOf(2), late, offering({formatOf()}), formatOf()));
    const std::shared_ptr<ClientOutput> joined = (*zone)->member(late);
    CHECK(joined != nullptr);
    CHECK_EQ(std::to_underlying(joined->producedThroughNs()), playhead + 1960ULL * 1000000ULL);
}

MULTIROOM_TEST(a_zone_starts_every_ready_room_at_one_position_and_one_instant)
{
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    settings.start.startDelayNs = kNanosecondsPerSecond;
    settings.start.readyTimeoutNs = 5ULL * kNanosecondsPerSecond;
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId kitchen("kitchen");
    const ClientId living("living");
    CHECK_OK(zone->admit(sessionOf(1), kitchen, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), living, offering({formatOf(48000)}), formatOf(48000)));

    // Section 48 forbids playing audio the moment it arrives, and nothing here
    // has said when: pumping the pipeline decodes and sends and starts nobody.
    zone->pump();
    CHECK(!zone->started());
    CHECK(rig.starts->of(RoomCommandKind::Start).empty());

    // One room is ready and the other is not, so the house waits.
    reportBuffer(*zone, kitchen, 0, 1500 * 1000000ULL);
    zone->pump();
    CHECK(!zone->started());

    reportBuffer(*zone, living, 0, 1200 * 1000000ULL);
    zone->pump();

    CHECK(zone->started());
    const std::vector<RoomCommand> sent = rig.starts->of(RoomCommandKind::Start);
    CHECK_EQ(sent.size(), size_t{2});
    CHECK_EQ(sent.front().start.timelineNs, sent.back().start.timelineNs);
    CHECK_EQ(sent.front().start.serverTimeNs, sent.back().start.serverTimeNs);
    CHECK_EQ(sent.front().start.serverTimeNs,
             rig.clock.monotonicNowNs() + kNanosecondsPerSecond);

    // Each room is told its own session, because a START_AT that named another
    // one is a message the client refuses.
    const std::optional<matp::StartAt> toKitchen = rig.starts->forRoom("kitchen");
    CHECK(toKitchen.has_value());
    CHECK_EQ(toKitchen->sessionId, sessionOf(1).toHex());

    CHECK_EQ(std::string(roomStartStateName(zone->roomStart(kitchen))), std::string("scheduled"));
    CHECK(zone->roomStart(living) == RoomStartState::Scheduled);
    CHECK_EQ(zone->startsSent(), uint64_t{2});

    // The line is running from here, whatever else happens.
    CHECK_EQ(std::to_underlying(zone->playheadNs()),
             std::to_underlying(zone->startTimelineNs()));
    rig.clock.advanceMs(3000);
    CHECK_EQ(std::to_underlying(zone->playheadNs()),
             std::to_underlying(zone->startTimelineNs()) + 2000ULL * 1000000ULL);
}

MULTIROOM_TEST(a_room_that_misses_the_barrier_does_not_hold_the_others)
{
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    settings.start.readyTimeoutNs = 5ULL * kNanosecondsPerSecond;
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    // A room whose device is still opening: SESSION_CONFIG named a format for
    // it and STREAM_READY has not come back, so it is not a member at all.
    const ClientId slow("slow");
    CHECK_OK(zone->planFor(slow, offering({formatOf()})));

    const ClientId quick("quick");
    CHECK_OK(zone->admit(sessionOf(1), quick, offering({formatOf()}), formatOf()));
    reportBuffer(*zone, quick, 0, 1500 * 1000000ULL);

    zone->pump();
    CHECK(!zone->started());

    // Five seconds is section 46's client ready timeout, and one room with a
    // slow driver must never keep the others silent past it.
    rig.clock.advanceMs(5000);
    zone->pump();

    CHECK(zone->started());
    const std::vector<RoomCommand> sent = rig.starts->of(RoomCommandKind::Start);
    CHECK_EQ(sent.size(), size_t{1});
    CHECK_EQ(sent.front().client.str(), std::string("quick"));
}

MULTIROOM_TEST(a_room_joining_a_playing_zone_begins_ahead_of_the_playhead)
{
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    settings.lateJoinLeadNs = 2ULL * kNanosecondsPerSecond;
    settings.start.startDelayNs = kNanosecondsPerSecond;
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId first("first");
    CHECK_OK(zone->admit(sessionOf(1), first, offering({formatOf()}), formatOf()));
    reportBuffer(*zone, first, 0, 1500 * 1000000ULL);
    zone->pump();
    CHECK(zone->started());

    matp::PlaybackReport audible;
    audible.clockQuality = AudioClockQuality::Hardware;
    audible.hasTimelineNsAudible = true;
    audible.timelineNsAudible = std::to_underlying(zone->startTimelineNs());
    zone->onPlaybackReport(first, audible, ClientClockModel{});
    CHECK(zone->roomStart(first) == RoomStartState::Playing);

    // A second later the start has happened and the timeline is running.
    rig.clock.advanceMs(1000);
    const uint64_t playhead = std::to_underlying(zone->playheadNs());

    const ClientId second("second");
    CHECK_OK(zone->admit(sessionOf(2), second, offering({formatOf()}), formatOf()));

    // Section 44: its stream begins on a position nobody has heard yet, so
    // what it prebuffers is audio it will be started into rather than audio
    // the house is already past.
    // A floor rather than an equality: admit() starts that room's own thread,
    // so on a loaded machine the stream may already have been built past where
    // it begins by the time this reads it, and "ahead of the playhead" is the
    // claim rather than any particular packet.
    const std::shared_ptr<ClientOutput> late = zone->member(second);
    CHECK(late != nullptr);
    CHECK(std::to_underlying(late->producedThroughNs()) >=
          playhead + 2ULL * kNanosecondsPerSecond);

    reportBuffer(*zone, second, playhead + 2ULL * kNanosecondsPerSecond, 1500 * 1000000ULL);
    zone->pump();

    // And it is started at that position, at the instant the running line says
    // that position becomes audible -- the room that is playing is untouched.
    const std::optional<matp::StartAt> toSecond = rig.starts->forRoom("second");
    CHECK(toSecond.has_value());
    CHECK_EQ(toSecond->timelineNs, playhead + 2ULL * kNanosecondsPerSecond);
    CHECK_EQ(toSecond->serverTimeNs, rig.clock.monotonicNowNs() + 2ULL * kNanosecondsPerSecond);
    CHECK_EQ(rig.starts->count(RoomCommandKind::Start), size_t{2});
    CHECK(zone->roomStart(first) == RoomStartState::Playing);
}

// --- phase 12: telemetry reaches the room it belongs to ----------------------

MULTIROOM_TEST(a_zone_measures_each_rooms_card_against_that_rooms_own_stream_rate)
{
    // One decode at 44.1 kHz, two rooms, and one of them resampled to 48 kHz.
    // Section 26's nominal rate is the room's *stream* rate, so the two are
    // measured against two different numbers out of one source -- which is the
    // thing a server that measured against the zone's rate would get wrong
    // without ever looking broken.
    ZoneRig rig;
    rig.clock.setNs(90ULL * kNanosecondsPerSecond);
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId native("native");
    const ClientId converted("converted");
    CHECK_OK(zone->admit(sessionOf(1), native, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), converted, offering({formatOf(48000)}), formatOf(48000)));

    CHECK_EQ(zone->syncStatus(native).nominalRateHz, kRate);
    CHECK_EQ(zone->syncStatus(converted).nominalRateHz, 48000u);

    // A room this zone does not hold has no sync state at all, rather than a
    // zeroed one that reads like a measurement.
    CHECK_EQ(zone->syncStatus(ClientId("absent")).nominalRateHz, 0u);
    CHECK(!zone->syncStatus(ClientId("absent")).hasDrift);
}

MULTIROOM_TEST(a_zone_feeds_playback_reports_into_the_rooms_sync_controller)
{
    ZoneRig rig;
    rig.clock.setNs(90ULL * kNanosecondsPerSecond);
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId room("living");
    CHECK_OK(zone->admit(sessionOf(1), room, offering({formatOf()}), formatOf()));

    // Phase 8's model, fitted: without it every report is refused, which is
    // itself the point of feeding one here rather than a default.
    ClientClockModel model;
    for (int i = 0; i < 20; ++i)
    {
        matp::ClockSyncResult result;
        result.clientSendNs = 500ULL * kNanosecondsPerSecond + static_cast<uint64_t>(i) * 200000000ULL;
        result.serverReceiveNs = result.clientSendNs - 410ULL * kNanosecondsPerSecond + 500000ULL;
        result.serverSendNs = result.serverReceiveNs;
        result.clientReceiveNs = result.clientSendNs + 1000000ULL;
        CHECK(model.addSample(result));
    }

    // A card running twenty ppm fast, reporting twice a second for twenty
    // seconds. Nothing here is corrected; the number is only measured.
    constexpr double kPpm = 20.0;
    const double actualRate = static_cast<double>(kRate) * (1.0 + kPpm * 1e-6);
    uint64_t clientNs = 500ULL * kNanosecondsPerSecond;
    for (int i = 1; i <= 40; ++i)
    {
        clientNs += 500000000ULL;
        matp::PlaybackReport report;
        report.clockQuality = AudioClockQuality::Hardware;
        report.localMonotonicTimeNs = clientNs;
        report.deviceFramePosition = static_cast<uint64_t>(
            actualRate * (static_cast<double>(i) * 0.5) + 0.5);
        report.bufferedDurationNs = 1500000000ULL;
        zone->onPlaybackReport(room, report, model);
    }

    const ClientSyncStatus status = zone->syncStatus(room);
    CHECK(status.hasDrift);
    CHECK_EQ(status.accepted, 40ULL);
    CHECK_NEAR(status.driftPpm, kPpm, 1.0);

    // The correction exists and is the drift term alone -- this zone never
    // started, so there is no line for an offset to be measured against.
    CHECK(!status.hasOffset);
    CHECK(status.ratio > 1.0);
}

MULTIROOM_TEST(a_reopened_device_ends_that_rooms_drift_series)
{
    // Section 12's re-negotiation read the way section 25 asks: a device that
    // answers STREAM_READY again started its frame counter over, so the window
    // the room's drift was fitted from describes a clock that is gone.
    ZoneRig rig;
    rig.clock.setNs(90ULL * kNanosecondsPerSecond);
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId room("living");
    CHECK_OK(zone->admit(sessionOf(1), room, offering({formatOf()}), formatOf()));

    ClientClockModel model;
    for (int i = 0; i < 20; ++i)
    {
        matp::ClockSyncResult result;
        result.clientSendNs =
            500ULL * kNanosecondsPerSecond + static_cast<uint64_t>(i) * 200000000ULL;
        result.serverReceiveNs = result.clientSendNs - 410ULL * kNanosecondsPerSecond + 500000ULL;
        result.serverSendNs = result.serverReceiveNs;
        result.clientReceiveNs = result.clientSendNs + 1000000ULL;
        CHECK(model.addSample(result));
    }

    uint64_t clientNs = 500ULL * kNanosecondsPerSecond;
    for (int i = 1; i <= 20; ++i)
    {
        clientNs += 500000000ULL;
        matp::PlaybackReport report;
        report.clockQuality = AudioClockQuality::Hardware;
        report.localMonotonicTimeNs = clientNs;
        report.deviceFramePosition = static_cast<uint64_t>(kRate) * static_cast<uint64_t>(i) / 2;
        zone->onPlaybackReport(room, report, model);
    }
    CHECK(zone->syncStatus(room).hasDrift);

    zone->onStreamReady(room, true);
    const ClientSyncStatus after = zone->syncStatus(room);
    CHECK(!after.hasDrift);
    CHECK_EQ(after.resets, 1ULL);
    CHECK_EQ(after.nominalRateHz, kRate); // the plan's rate, not a guess
    CHECK_EQ(after.reports, 20ULL);       // and the lifetime counters survive

    // A room that reports it is *not* ready has not reopened anything: section
    // 20's table drops it from scheduling and keeps everything else about it.
    zone->onStreamReady(room, false);
    CHECK_EQ(zone->syncStatus(room).resets, 1ULL);
}

MULTIROOM_TEST(a_reopened_device_resumes_without_a_partial_packet_from_before_standby)
{
    class HeaderOnlySink final : public IPacketSink
    {
    public:
        explicit HeaderOnlySink(std::shared_ptr<SinkState> state) : state_(std::move(state)) {}
        Result<size_t, PacketSinkError> write(std::span<const std::byte> bytes) override
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            const size_t count = std::min(bytes.size(), matp::kAudioHeaderBytes - state_->received.size());
            state_->received.append(reinterpret_cast<const char*>(bytes.data()), count);
            return count;
        }
        void close() override {}
    private:
        std::shared_ptr<SinkState> state_;
    };

    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto zone = rig.zone();
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());
    const ClientId room("living");
    CHECK_OK(zone->admit(sessionOf(1), room, offering({formatOf()}), formatOf()));
    auto partial = std::make_shared<SinkState>();
    zone->member(room)->attach(std::make_unique<HeaderOnlySink>(partial));
    reportBuffer(*zone, room, 0, 3ULL * kNanosecondsPerSecond);
    zone->pump();
    CHECK(waitFor([&] {
        std::lock_guard<std::mutex> lock(partial->mutex);
        return partial->received.size() == matp::kAudioHeaderBytes;
    }));
    rig.clock.advanceMs(1500);
    const auto paused = zone->pausePlayback();
    CHECK_OK(paused);
    CHECK(std::to_underlying(*paused) > 0);
    zone->onStreamReady(room, false);
    CHECK_OK(zone->resumePlayback());
    zone->onStreamReady(room, true, formatOf());
    auto resumed = connect(*zone, sessionOf(1), room);
    CHECK(waitFor([&] { return zone->member(room)->packets() > 0; }));
    std::string payloads;
    const auto headers = decodeHeaders(*resumed, payloads);
    CHECK(!headers.empty());
    CHECK_EQ(headers.front().timelineStartNs, std::to_underlying(*paused));
    reportBuffer(*zone, room, headers.front().timelineStartNs, 1500ULL * 1000000ULL);
    zone->pump();
    CHECK(zone->transport() == TransportState::Playing);
    CHECK(zone->roomStart(room) == RoomStartState::Scheduled);
    CHECK_EQ(rig.starts->of(RoomCommandKind::Start).back().start.timelineNs,
             std::to_underlying(*paused));
}

MULTIROOM_TEST(a_room_moved_to_a_card_in_another_format_is_replanned_for_it)
{
    // The room's device changed, not the source: moved to a card that opens
    // only 48 kHz 32-bit, it is resampled into that rather than sent the
    // 44.1 kHz 16-bit bytes of the card it left, and its drift is measured at
    // the new rate.
    ZoneRig rig;
    rig.clock.setNs(90ULL * kNanosecondsPerSecond);
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId room("living");
    CHECK_OK(zone->admit(sessionOf(1), room, offering({formatOf()}), formatOf()));
    const std::shared_ptr<ClientOutput> output = zone->member(room);
    CHECK(output->plan().mode == OutputMode::BitPerfect);

    const AudioFormat hdmi = formatOf(48000, kChannels, AudioFormat::SampleType::Int32);
    const ClientCapabilities card = offering({hdmi});
    zone->onStreamReady(room, true, hdmi, &card);

    CHECK(zone->member(room) == output);
    CHECK_EQ(describeFormat(output->plan().outputFormat), describeFormat(hdmi));
    CHECK_EQ(describeFormat(output->plan().sourceFormat), describeFormat(formatOf()));
    CHECK_EQ(output->formatSwitches(), 1ULL);
    CHECK_EQ(zone->syncStatus(room).nominalRateHz, 48000u);

    // The same format again is a plain reopen: nothing is replanned.
    zone->onStreamReady(room, true, hdmi, &card);
    CHECK_EQ(output->formatSwitches(), 1ULL);
}

MULTIROOM_TEST(a_room_that_confirms_its_clock_trim_on_reopen_is_replanned_for_it)
{
    // MATP section 61: switching the actuator renegotiates the room, and its
    // device answers in the same format. That is not a plain reopen -- the
    // actuator, and with it the converter's ratio mode, changed.
    ZoneRig rig;
    rig.clock.setNs(90ULL * kNanosecondsPerSecond);
    ZoneSettings settings = rig.settings();
    settings.roomPreference = [](const ClientId&) {
        return RoomAudioSettings{AudioControl::Auto, true};
    };
    auto zone = rig.zone(settings);
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId room("living");
    CHECK_OK(zone->admit(sessionOf(1), room, offering({formatOf()}), formatOf()));
    const std::shared_ptr<ClientOutput> output = zone->member(room);
    CHECK(output->driftActuator() == DriftActuator::ServerResampling);
    CHECK(output->plan().mode == OutputMode::Resampled);

    ClientCapabilities trimmed = offering({formatOf()});
    trimmed.driftFeedback = true;
    trimmed.hardwareClock.available = true;
    trimmed.hardwareClock.minPpm = -500;
    trimmed.hardwareClock.maxPpm = 500;
    zone->onStreamReady(room, true, formatOf(), &trimmed);
    CHECK(output->driftActuator() == DriftActuator::ClientHardwareClock);
    CHECK(output->plan().mode == OutputMode::BitPerfect);
    CHECK_EQ(output->formatSwitches(), 1ULL);

    // Confirmed again: a plain reopen.
    zone->onStreamReady(room, true, formatOf(), &trimmed);
    CHECK_EQ(output->formatSwitches(), 1ULL);

    // And back to resampling when the trim is no longer confirmed.
    const ClientCapabilities plain = offering({formatOf()});
    zone->onStreamReady(room, true, formatOf(), &plain);
    CHECK(output->driftActuator() == DriftActuator::ServerResampling);
}

MULTIROOM_TEST(a_zone_corrects_the_room_it_planned_a_correction_for_and_leaves_the_other)
{
    // Phase 13 end to end, and the shape of the first two-room run: one zone,
    // one decode, one room bitperfect and one resampled, and a correction that
    // reaches exactly one of them.
    ZoneRig rig;
    rig.clock.setNs(90ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    // A device that offers the source format is bitperfect and is therefore
    // left alone, whatever this zone asked for; one that does not is
    // resampled, and resampled is what makes a correction possible.
    const AudioFormat resampled = formatOf(48000, kChannels, AudioFormat::SampleType::Int32);
    const ClientId left("livingroom");
    const ClientId right("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), left, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), right, offering({resampled}), resampled));

    const std::shared_ptr<ClientOutput> bitperfect = zone->member(left);
    const std::shared_ptr<ClientOutput> corrected = zone->member(right);
    CHECK(bitperfect != nullptr);
    CHECK(corrected != nullptr);

    CHECK(!bitperfect->driftCorrected());
    CHECK(bitperfect->plan().driftDisabledBy == DriftDisabledReason::NotDisabled);
    CHECK(corrected->driftCorrected());

    // Phase 8's model, fitted, and a card running twenty ppm fast in both
    // rooms: the same reports, and two different answers to them.
    ClientClockModel model;
    for (int i = 0; i < 20; ++i)
    {
        matp::ClockSyncResult result;
        result.clientSendNs =
            500ULL * kNanosecondsPerSecond + static_cast<uint64_t>(i) * 200000000ULL;
        result.serverReceiveNs = result.clientSendNs - 410ULL * kNanosecondsPerSecond + 500000ULL;
        result.serverSendNs = result.serverReceiveNs;
        result.clientReceiveNs = result.clientSendNs + 1000000ULL;
        CHECK(model.addSample(result));
    }

    constexpr double kPpm = 20.0;
    uint64_t clientNs = 500ULL * kNanosecondsPerSecond;
    for (int i = 1; i <= 40; ++i)
    {
        clientNs += 500000000ULL;
        const double seconds = static_cast<double>(i) * 0.5;

        matp::PlaybackReport report;
        report.clockQuality = AudioClockQuality::Hardware;
        report.localMonotonicTimeNs = clientNs;
        report.bufferedDurationNs = 1500000000ULL;

        report.deviceFramePosition = static_cast<uint64_t>(
            static_cast<double>(kRate) * (1.0 + kPpm * 1e-6) * seconds + 0.5);
        zone->onPlaybackReport(left, report, model);

        report.deviceFramePosition =
            static_cast<uint64_t>(48000.0 * (1.0 + kPpm * 1e-6) * seconds + 0.5);
        zone->onPlaybackReport(right, report, model);
    }

    // Both rooms measured the same card error against their own rates.
    CHECK_NEAR(zone->syncStatus(left).driftPpm, kPpm, 1.0);
    CHECK_NEAR(zone->syncStatus(right).driftPpm, kPpm, 1.0);
    CHECK(zone->syncStatus(right).ratio > 1.0);

    // And only one of them was given the answer. The room's own thread is what
    // installs it, so the assertion is bounded rather than immediate -- and it
    // waits for the *final* trim rather than for any trim, because that thread
    // runs while the reports above are still arriving and may well have
    // installed report five's answer before report forty was computed.
    const double answer = zone->syncStatus(right).ratio;
    CHECK(waitFor([&] { return std::fabs(corrected->driftTrim() - answer) <= 1e-9; }));
    CHECK(corrected->driftTrim() > 1.0);
    CHECK(corrected->corrections() > 0);
    CHECK_EQ(corrected->correctionsRefused(), 0ULL);

    // The bitperfect room is left to drift, which is the roadmap's phase 13
    // declining MATP section 32's recommendation, and is deliberate.
    CHECK_EQ(bitperfect->corrections(), 0ULL);
    CHECK_EQ(bitperfect->driftTrim(), 1.0);
}

MULTIROOM_TEST(a_bitperfect_room_in_a_shared_zone_sets_no_ratio_at_all)
{
    // Bit-Perfect is the one way out of correction, even in a zone shared with
    // other rooms: the ratio is never set, asserted on the room rather than on
    // the controller. The measurement still happens -- an operator who keeps
    // a room bitperfect still wants to know how far it drifts.
    ZoneRig rig;
    rig.clock.setNs(90ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    settings.roomPreference = [](const ClientId&) {
        return RoomAudioSettings{AudioControl::BitPerfect, true};
    };
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId room("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), room, offering({formatOf()}), formatOf()));

    const std::shared_ptr<ClientOutput> output = zone->member(room);
    CHECK(output != nullptr);
    CHECK(output->plan().mode == OutputMode::BitPerfect);
    CHECK(!output->driftCorrected());
    CHECK(output->plan().driftDisabledBy == DriftDisabledReason::BitperfectForced);

    ClientClockModel model;
    for (int i = 0; i < 20; ++i)
    {
        matp::ClockSyncResult result;
        result.clientSendNs =
            500ULL * kNanosecondsPerSecond + static_cast<uint64_t>(i) * 200000000ULL;
        result.serverReceiveNs = result.clientSendNs - 410ULL * kNanosecondsPerSecond + 500000ULL;
        result.serverSendNs = result.serverReceiveNs;
        result.clientReceiveNs = result.clientSendNs + 1000000ULL;
        CHECK(model.addSample(result));
    }

    uint64_t clientNs = 500ULL * kNanosecondsPerSecond;
    for (int i = 1; i <= 40; ++i)
    {
        clientNs += 500000000ULL;
        matp::PlaybackReport report;
        report.clockQuality = AudioClockQuality::Hardware;
        report.localMonotonicTimeNs = clientNs;
        report.bufferedDurationNs = 1500000000ULL;
        report.deviceFramePosition = static_cast<uint64_t>(
            static_cast<double>(kRate) * 1.00002 * static_cast<double>(i) * 0.5 + 0.5);
        zone->onPlaybackReport(room, report, model);
    }

    // Measured, and given to nobody.
    CHECK(zone->syncStatus(room).hasDrift);
    CHECK_NEAR(zone->syncStatus(room).driftPpm, 20.0, 1.0);
    CHECK_EQ(output->corrections(), 0ULL);
    CHECK_EQ(output->correctionsRefused(), 0ULL);
    CHECK_EQ(output->driftTrim(), 1.0);
}

MULTIROOM_TEST(a_trimmed_room_is_sent_its_target_and_never_a_ratio)
{
    // a bitperfect room in a shared
    // zone whose client trims its own clock is corrected through it. The
    // stream keeps its nominal ratio; the target goes out as DRIFT_FEEDBACK,
    // and a room the server resamples is sent the measurement alone.
    ZoneRig rig;
    rig.clock.setNs(90ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    settings.roomPreference = [](const ClientId&) {
        return RoomAudioSettings{AudioControl::BitPerfect, true};
    };
    auto zone = rig.zone(settings);
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    ClientCapabilities trimmed = offering({formatOf()});
    trimmed.driftFeedback = true;
    trimmed.hardwareClock.available = true;
    trimmed.hardwareClock.minPpm = -500;
    trimmed.hardwareClock.maxPpm = 500;
    ClientCapabilities reading = offering({formatOf()});
    reading.driftFeedback = true;
    const ClientId room("kitchen");
    const ClientId other("hall");
    const ClientId old("garage");
    CHECK_OK(zone->admit(sessionOf(1), room, trimmed, formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), other, reading, formatOf()));
    CHECK_OK(zone->admit(sessionOf(3), old, offering({formatOf()}), formatOf()));

    const std::shared_ptr<ClientOutput> output = zone->member(room);
    CHECK(output != nullptr);
    CHECK(output->plan().mode == OutputMode::BitPerfect);
    CHECK(output->driftActuator() == DriftActuator::ClientHardwareClock);
    CHECK(output->driftCorrected());

    ClientClockModel model;
    for (int i = 0; i < 20; ++i)
    {
        matp::ClockSyncResult result;
        result.clientSendNs =
            500ULL * kNanosecondsPerSecond + static_cast<uint64_t>(i) * 200000000ULL;
        result.serverReceiveNs = result.clientSendNs - 410ULL * kNanosecondsPerSecond + 500000ULL;
        result.serverSendNs = result.serverReceiveNs;
        result.clientReceiveNs = result.clientSendNs + 1000000ULL;
        CHECK(model.addSample(result));
    }

    uint64_t clientNs = 500ULL * kNanosecondsPerSecond;
    for (int i = 1; i <= 40; ++i)
    {
        clientNs += 500000000ULL;
        matp::PlaybackReport report;
        report.clockQuality = AudioClockQuality::Hardware;
        report.localMonotonicTimeNs = clientNs;
        report.bufferedDurationNs = 1500000000ULL;
        report.deviceFramePosition = static_cast<uint64_t>(
            static_cast<double>(kRate) * 1.00002 * static_cast<double>(i) * 0.5 + 0.5);
        report.hasHardwareClock = true;
        zone->onPlaybackReport(room, report, model);
        report.hasHardwareClock = false;
        zone->onPlaybackReport(other, report, model);
        zone->onPlaybackReport(old, report, model);
    }

    // Measured and synchronized through the clock, never through a ratio.
    CHECK(zone->syncStatus(room).hardwareClock);
    CHECK_EQ(zone->syncStatus(room).ratio, 1.0);
    CHECK_EQ(output->corrections(), 0ULL);
    CHECK_EQ(output->driftTrim(), 1.0);

    size_t targets = 0;
    size_t measurements = 0;
    {
        std::lock_guard<std::mutex> lock(rig.starts->mutex);
        for (const RoomCommand& command : rig.starts->sent)
        {
            if (command.kind != RoomCommandKind::DriftFeedback)
                continue;
            CHECK(command.client != old);
            CHECK(!command.driftFeedback.sessionId.empty());
            if (command.client == room)
            {
                CHECK(command.driftFeedback.hasHardwareTarget);
                ++targets;
            }
            else
            {
                CHECK(!command.driftFeedback.hasHardwareTarget);
                ++measurements;
            }
        }
    }
    CHECK(targets > 0);
    CHECK(measurements > 0);
}

// --- phase 14: transport controls, volume and track metadata -----------------

namespace {

// The only acknowledgement the protocol has (section 25), which is what moves
// a room from Scheduled to Playing and therefore onto the line a pause acts on.
void acknowledge(Zone& zone, const ClientId& client, uint64_t timelineNs)
{
    matp::PlaybackReport audible;
    audible.clockQuality = AudioClockQuality::Hardware;
    audible.hasTimelineNsAudible = true;
    audible.timelineNsAudible = timelineNs;
    zone.onPlaybackReport(client, audible, ClientClockModel{});
}

SourceSegment trackOf(const char* id, const char* trackId, const char* title)
{
    SourceSegment segment = segmentOf(id, "ramp");
    segment.trackId = trackId;
    segment.title = title;
    return segment;
}

} // namespace

MULTIROOM_TEST(a_same_format_track_boundary_is_metadata_and_the_stream_carries_on)
{
    // Not a whole number of packets, on purpose: the boundary then falls
    // *inside* a packet rather than conveniently between two, so a stream that
    // is contiguous across it is contiguous by arithmetic rather than by luck.
    constexpr uint64_t kFrames = 132700;
    constexpr size_t kPackets = (2 * kFrames) / kPacketFrames; // 300, and 800 frames over
    constexpr uint32_t kOver = static_cast<uint32_t>(2 * kFrames - kPackets * kPacketFrames);

    ZoneRig rig(formatOf(), kFrames);
    auto zone = rig.zone();

    CHECK_OK(zone->stage(trackOf("one", "track-A", "First Song")));
    CHECK_OK(zone->stage(trackOf("two", "track-B", "Second Song")));
    CHECK_OK(zone->play());

    const ClientId room("livingroom");
    const SessionId session = sessionOf(1);
    CHECK_OK(zone->admit(session, room, offering({formatOf()}), formatOf()));

    const std::shared_ptr<ClientOutput> output = zone->member(room);
    CHECK(output != nullptr);
    CHECK(output->passThrough());

    auto sink = connect(*zone, session, room);
    decodeEverything(*zone);
    CHECK(waitFor([&] { return output->packets() >= kPackets + 1; }));

    // Both sources were opened, and the second begins where the first ended.
    const std::vector<StagedSource> staged = zone->sources();
    CHECK_EQ(staged.size(), 2u);
    const auto boundary = nanosecondsFromFrames(kFrames, kRate);
    CHECK_OK(boundary);
    CHECK_EQ(std::to_underlying(staged[1].timelineStartNs), *boundary);
    CHECK_EQ(rig.factory.counters.opens.load(), 2u);

    // Section 23: the boundary is one message, and nothing else happened.
    // This room's output was never replaced -- a re-prepare would be a second
    // SESSION_CONFIG and a second PREPARE_STREAM, and this zone asks for
    // neither -- and the room it was announced to is still the same object
    // with the same plan.
    const std::vector<RoomCommand> boundaries = rig.starts->of(RoomCommandKind::TrackBoundary);
    CHECK_EQ(boundaries.size(), size_t{1});
    CHECK_EQ(boundaries.front().client.str(), std::string("livingroom"));
    CHECK_EQ(boundaries.front().boundary.timelineNs, *boundary);
    CHECK_EQ(boundaries.front().boundary.trackId, std::string("track-B"));
    CHECK_EQ(boundaries.front().boundary.title, std::string("Second Song"));
    CHECK_EQ(boundaries.front().boundary.sessionId, session.toHex());
    CHECK(zone->member(room) == output);

    std::string payloads;
    const std::vector<matp::AudioPacketHeader> headers = decodeHeaders(*sink, payloads);
    CHECK_EQ(headers.size(), kPackets + 1);

    for (size_t index = 0; index < kPackets; ++index)
    {
        // Section 22, stated as a number: there is no gap at the boundary,
        // the timeline is not reset, and the frame count does not change.
        CHECK_EQ(headers[index].timelineStartNs, static_cast<uint64_t>(index) * kPacketNs);
        CHECK_EQ(headers[index].timelineDurationNs, kPacketNs);
        CHECK_EQ(headers[index].sequenceNumber, static_cast<uint64_t>(index));
        CHECK_EQ(headers[index].outputFrameCount, kPacketFrames);
    }

    // Nothing is staged after the second, so the frames past the last whole
    // packet go out in a short one that ends on the zone's frontier.
    const matp::AudioPacketHeader& last = headers.back();
    CHECK_EQ(last.timelineStartNs, static_cast<uint64_t>(kPackets) * kPacketNs);
    CHECK_EQ(last.timelineStartNs + last.timelineDurationNs, std::to_underlying(zone->frontierNs()));
    CHECK_EQ(last.sequenceNumber, static_cast<uint64_t>(kPackets));
    CHECK_EQ(last.outputFrameCount, kOver);

    // And the bytes across the boundary are the two tracks, end to end, with
    // nothing inserted and nothing repeated.
    const uint32_t bytesPerFrame = formatOf().bytesPerFrame();
    std::vector<std::byte> expected(static_cast<size_t>(2 * kFrames) * bytesPerFrame);
    writeRamp(expected, 0, static_cast<uint32_t>(kFrames), kChannels);
    writeRamp(std::span<std::byte>(expected).subspan(static_cast<size_t>(kFrames) *
                                                     bytesPerFrame),
              0, static_cast<uint32_t>(kFrames), kChannels);

    CHECK_EQ(payloads.size(), expected.size());
    CHECK_EQ(std::memcmp(payloads.data(), expected.data(), payloads.size()), 0);
}

MULTIROOM_TEST(the_last_source_is_sent_to_its_last_frame_and_a_later_one_carries_on_from_it)
{
    // Shorter than two packets and not a whole number of them: the second
    // packet can only ever be a short one.
    constexpr uint64_t kFrames = 1000;
    constexpr uint32_t kTail = static_cast<uint32_t>(kFrames) - kPacketFrames; // 118

    ZoneRig rig(formatOf(), kFrames);
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId room("livingroom");
    const SessionId session = sessionOf(1);
    CHECK_OK(zone->admit(session, room, offering({formatOf()}), formatOf()));
    const std::shared_ptr<ClientOutput> output = zone->member(room);
    CHECK(output != nullptr);
    CHECK(output->passThrough());

    auto sink = connect(*zone, session, room);
    decodeEverything(*zone);
    CHECK(zone->state() == ZoneState::Ended);
    CHECK(waitFor([&] { return output->packets() >= 2; }));
    const TimelineNs firstEnd = zone->frontierNs();

    std::string payloads;
    std::vector<matp::AudioPacketHeader> headers = decodeHeaders(*sink, payloads);
    CHECK_EQ(headers.size(), size_t{2});
    CHECK_EQ(headers[1].timelineStartNs, kPacketNs);
    CHECK_EQ(headers[1].timelineStartNs + headers[1].timelineDurationNs,
             std::to_underlying(firstEnd));
    CHECK_EQ(headers[1].outputFrameCount, kTail);
    CHECK_EQ(payloads.size(), static_cast<size_t>(kFrames) * formatOf().bytesPerFrame());

    // Staged once the room has sent the end: it carries on from exactly
    // there, on the same connection and the same numbering.
    CHECK_OK(zone->stage(segmentOf("two", "ramp")));
    decodeEverything(*zone);
    CHECK(waitFor([&] { return output->packets() >= 4; }));

    headers = decodeHeaders(*sink, payloads);
    CHECK_EQ(headers.size(), size_t{4});
    for (size_t index = 0; index + 1 < headers.size(); ++index)
    {
        CHECK_EQ(headers[index].sequenceNumber + 1, headers[index + 1].sequenceNumber);
        CHECK_EQ(headers[index].timelineStartNs + headers[index].timelineDurationNs,
                 headers[index + 1].timelineStartNs);
    }
    CHECK_EQ(headers[2].timelineStartNs, std::to_underlying(firstEnd));
    CHECK_EQ(headers[2].outputFrameCount, kPacketFrames);
    CHECK_EQ(headers[3].outputFrameCount, kTail);
    CHECK_EQ(headers[3].timelineStartNs + headers[3].timelineDurationNs,
             std::to_underlying(zone->frontierNs()));

    const uint32_t bytesPerFrame = formatOf().bytesPerFrame();
    std::vector<std::byte> expected(static_cast<size_t>(2 * kFrames) * bytesPerFrame);
    writeRamp(expected, 0, static_cast<uint32_t>(kFrames), kChannels);
    writeRamp(std::span<std::byte>(expected).subspan(static_cast<size_t>(kFrames) * bytesPerFrame),
              0, static_cast<uint32_t>(kFrames), kChannels);
    CHECK_EQ(payloads.size(), expected.size());
    CHECK_EQ(std::memcmp(payloads.data(), expected.data(), payloads.size()), 0);
}

MULTIROOM_TEST(a_room_joining_mid_track_is_told_what_is_playing_and_what_comes_next)
{
    ZoneRig rig;
    auto zone = rig.zone();

    CHECK_OK(zone->stage(trackOf("one", "track-A", "First Song")));
    CHECK_OK(zone->stage(trackOf("two", "track-B", "Second Song")));
    CHECK_OK(zone->play());

    // Both sources are open before this room is in the zone, so both of their
    // boundaries were announced to nobody.
    decodeEverything(*zone);
    CHECK_EQ(zone->sources().size(), 2u);
    CHECK(rig.starts->all().empty());

    const ClientId late("late");
    CHECK_OK(zone->admit(sessionOf(7), late, offering({formatOf()}), formatOf()));

    // Section 44: it begins in the middle of the first track, so it is told
    // what is playing there -- and told the boundary it is about to reach,
    // which it would otherwise never hear about.
    const std::vector<RoomCommand> info = rig.starts->of(RoomCommandKind::TrackInfo);
    CHECK_EQ(info.size(), size_t{1});
    CHECK_EQ(info.front().client.str(), std::string("late"));
    CHECK_EQ(info.front().info.trackId, std::string("track-A"));
    CHECK_EQ(info.front().info.title, std::string("First Song"));
    CHECK_EQ(info.front().info.sessionId, sessionOf(7).toHex());

    const auto boundary = nanosecondsFromFrames(kSourceFrames, kRate);
    CHECK_OK(boundary);

    const std::vector<RoomCommand> boundaries = rig.starts->of(RoomCommandKind::TrackBoundary);
    CHECK_EQ(boundaries.size(), size_t{1});
    CHECK_EQ(boundaries.front().client.str(), std::string("late"));
    CHECK_EQ(boundaries.front().boundary.timelineNs, *boundary);
    CHECK_EQ(boundaries.front().boundary.trackId, std::string("track-B"));
}

MULTIROOM_TEST(a_scheduled_pause_and_resume_land_on_the_named_position)
{
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId kitchen("kitchen");
    const ClientId living("living");
    CHECK_OK(zone->admit(sessionOf(1), kitchen, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), living, offering({formatOf()}), formatOf()));

    reportBuffer(*zone, kitchen, 0, 1500 * 1000000ULL);
    reportBuffer(*zone, living, 0, 1500 * 1000000ULL);
    zone->pump();
    CHECK(zone->transport() == TransportState::Playing);

    acknowledge(*zone, kitchen, 0);
    acknowledge(*zone, living, 0);

    // A second of the start delay and a second of timeline.
    rig.clock.advanceMs(2000);
    CHECK_EQ(std::to_underlying(zone->playheadNs()), 1ULL * kNanosecondsPerSecond);

    const Result<TimelineNs, ZoneError> paused = zone->pausePlayback();
    CHECK_OK(paused);
    CHECK_EQ(std::to_underlying(*paused), 2ULL * kNanosecondsPerSecond);

    // Section 37: one logical position, and every room told the same one.
    const std::vector<RoomCommand> pauses = rig.starts->of(RoomCommandKind::Pause);
    CHECK_EQ(pauses.size(), size_t{2});
    CHECK_EQ(pauses.front().at.timelineNs, std::to_underlying(*paused));
    CHECK_EQ(pauses.back().at.timelineNs, std::to_underlying(*paused));
    const std::vector<RoomCommand> toKitchen =
        rig.starts->of(RoomCommandKind::Pause, "kitchen");
    CHECK_EQ(toKitchen.size(), size_t{1});
    CHECK_EQ(pauses.front().client.str(), std::string("kitchen"));
    CHECK_EQ(toKitchen.front().at.sessionId, sessionOf(1).toHex());

    // The playhead stands still, and nothing is scheduled while it does --
    // however ready the rooms go on saying they are.
    rig.clock.advanceMs(5000);
    CHECK_EQ(std::to_underlying(zone->playheadNs()), std::to_underlying(*paused));
    reportBuffer(*zone, kitchen, std::to_underlying(*paused), 1500 * 1000000ULL);
    reportBuffer(*zone, living, std::to_underlying(*paused), 1500 * 1000000ULL);
    zone->pump();
    CHECK_EQ(rig.starts->count(RoomCommandKind::Start), size_t{2});

    // The resume is a new START_AT at exactly the frame the pause named, and
    // nothing in either room was dropped to get there.
    CHECK_OK(zone->resumePlayback());
    zone->pump();

    const std::vector<RoomCommand> starts = rig.starts->of(RoomCommandKind::Start);
    CHECK_EQ(starts.size(), size_t{4});
    CHECK_EQ(starts[2].start.timelineNs, std::to_underlying(*paused));
    CHECK_EQ(starts[3].start.timelineNs, std::to_underlying(*paused));
    CHECK_EQ(starts[2].start.serverTimeNs, starts[3].start.serverTimeNs);
    CHECK(zone->transport() == TransportState::Playing);

    // A pause on a zone that is not playing its timeline says so rather than
    // half-doing it.
    CHECK_OK(zone->pausePlayback());
    CHECK_ERR(zone->pausePlayback(), ZoneError::NotStarted);
    CHECK_OK(zone->resumePlayback());
    CHECK_ERR(zone->resumePlayback(), ZoneError::NotStarted);
}

MULTIROOM_TEST(a_stop_drops_what_a_room_holds_and_leaves_a_zone_play_can_restart)
{
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);

    // A prebuffer smaller than the source, so this room's stream has *not*
    // reached the end of the first track when the stop arrives: the audio
    // between where it stopped and where the next track begins is what
    // section 38 is about throwing away.
    ZoneSettings settings = rig.settings();
    settings.output.targetBufferNs = 1000ULL * 1000000ULL;
    settings.lookaheadNs = 2ULL * kNanosecondsPerSecond;
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId room("livingroom");
    const SessionId session = sessionOf(1);
    CHECK_OK(zone->admit(session, room, offering({formatOf()}), formatOf()));

    const std::shared_ptr<ClientOutput> output = zone->member(room);
    auto sink = connect(*zone, session, room);

    // The whole prebuffer before the report that stops the pacing: the decode
    // follows what this room has built, and only that much reaches the end.
    CHECK(waitFor([&] {
        zone->pump();
        return std::to_underlying(output->producedThroughNs()) >= settings.output.targetBufferNs;
    }));
    reportBuffer(*zone, room, 0, 1500 * 1000000ULL);
    zone->pump();
    CHECK(zone->started());
    acknowledge(*zone, room, 0);

    // The whole source is decoded, and this room's stream is nowhere near the
    // end of it.
    CHECK(waitFor([&] { zone->pump(); return zone->state() == ZoneState::Ended; }));
    const uint64_t producedBefore = std::to_underlying(output->producedThroughNs());
    const auto sourceEnd = nanosecondsFromFrames(kSourceFrames, kRate);
    CHECK_OK(sourceEnd);
    CHECK(producedBefore < *sourceEnd);

    rig.clock.advanceMs(2000);
    const Result<TimelineNs, ZoneError> stopped = zone->stopPlayback();
    CHECK_OK(stopped);

    const std::vector<RoomCommand> stops = rig.starts->of(RoomCommandKind::Stop);
    CHECK_EQ(stops.size(), size_t{1});
    CHECK_EQ(stops.front().at.timelineNs, std::to_underlying(*stopped));
    CHECK_EQ(stops.front().at.sessionId, session.toHex());

    // Dropped on this side too: nothing is queued for a timeline nobody is
    // going to play, and nothing more is produced until a position is named.
    CHECK(output->state() == OutputState::Parked);
    CHECK_EQ(output->queuedPackets(), size_t{0});
    CHECK(zone->transport() == TransportState::Stopped);
    CHECK(!zone->started());

    // And the zone is where play() can restart it (section 5: the timeline is
    // still never reset, so the next source continues after this one).
    CHECK(zone->state() == ZoneState::Idle);
    const uint64_t packetsBefore = output->packets();
    std::string beforePayloads;
    const size_t headersBefore = decodeHeaders(*sink, beforePayloads).size();
    CHECK(headersBefore > 0);

    CHECK_OK(zone->stage(segmentOf("two", "ramp")));
    CHECK_OK(zone->play());
    CHECK(zone->state() == ZoneState::Playing);

    const std::vector<StagedSource> staged = zone->sources();
    CHECK_EQ(staged.size(), 2u);
    const uint64_t base = std::to_underlying(staged[1].timelineStartNs);
    CHECK_EQ(base, *sourceEnd);

    // This room begins again at the first position of what was opened, rather
    // than playing out the tail of a track that was stopped.
    CHECK(output->state() == OutputState::Streaming);
    CHECK_EQ(std::to_underlying(output->producedThroughNs()), base);

    CHECK(waitFor([&] { zone->pump(); return output->packets() > packetsBefore; }));

    std::string payloads;
    const std::vector<matp::AudioPacketHeader> headers = decodeHeaders(*sink, payloads);
    CHECK(headers.size() > headersBefore);

    // One move on the timeline, which the client reads as a rejoin -- and no
    // gap at all in the sequence, which it would read as a server that skipped
    // ahead and would drop the connection over.
    const matp::AudioPacketHeader& first = headers[headersBefore];
    CHECK_EQ(first.timelineStartNs, base);
    CHECK_EQ(first.sequenceNumber, headers[headersBefore - 1].sequenceNumber + 1);
    CHECK_EQ(first.outputFrameCount, kPacketFrames);
}

MULTIROOM_TEST(volume_is_forwarded_to_one_room_and_not_a_byte_of_its_stream_changes)
{
    ZoneRig rig;
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId room("livingroom");
    const SessionId session = sessionOf(1);
    CHECK_OK(zone->admit(session, room, offering({formatOf()}), formatOf()));

    const std::shared_ptr<ClientOutput> output = zone->member(room);
    auto sink = connect(*zone, session, room);

    matp::SetVolume request;
    request.hasValue = true;
    request.value = 0.75;
    request.hasApplyAtTimelineNs = true;
    request.applyAtTimelineNs = 142000000ULL;
    CHECK_OK(zone->setVolume(room, request));

    const std::vector<RoomCommand> sent = rig.starts->of(RoomCommandKind::Volume);
    CHECK_EQ(sent.size(), size_t{1});
    CHECK_EQ(sent.front().client.str(), std::string("livingroom"));
    CHECK(sent.front().volume.hasValue);
    CHECK_EQ(sent.front().volume.value, 0.75);
    CHECK_EQ(sent.front().volume.applyAtTimelineNs, request.applyAtTimelineNs);
    CHECK_EQ(sent.front().volume.sessionId, session.toHex());

    // A room this zone does not hold, and a message that would ask for
    // nothing: both are the caller's mistake and neither reaches a socket.
    CHECK_ERR(zone->setVolume(ClientId("elsewhere"), request), ZoneError::NotMember);
    CHECK_ERR(zone->setVolume(room, matp::SetVolume{}), ZoneError::InvalidCommand);
    CHECK_EQ(rig.starts->count(RoomCommandKind::Volume), size_t{1});

    // And the stream is what it was: the server does not touch a sample for
    // volume, which is what keeps this bitperfect room bitperfect.
    decodeEverything(*zone);
    CHECK(waitFor([&] { return output->packets() >= kSourcePackets; }));

    std::string payloads;
    const std::vector<matp::AudioPacketHeader> headers = decodeHeaders(*sink, payloads);
    CHECK_EQ(headers.size(), static_cast<size_t>(kSourcePackets));

    std::vector<std::byte> expected(static_cast<size_t>(kSourceFrames) *
                                    formatOf().bytesPerFrame());
    writeRamp(expected, 0, static_cast<uint32_t>(kSourceFrames), kChannels);
    CHECK_EQ(payloads.size(), expected.size());
    CHECK_EQ(std::memcmp(payloads.data(), expected.data(), expected.size()), 0);
}

// --- phase 16: reconnect, rejoin, hard resync --------------------------------

namespace {

// Phase 8's model, fitted well enough for an offset: section 46's burst of
// twenty exchanges on a half-millisecond link, from a client whose clock reads
// 410 s more than this server's.
ClientClockModel fittedModel()
{
    ClientClockModel model;
    for (int i = 0; i < 20; ++i)
    {
        matp::ClockSyncResult result;
        result.clientSendNs =
            500ULL * kNanosecondsPerSecond + static_cast<uint64_t>(i) * 200000000ULL;
        result.serverReceiveNs = result.clientSendNs - 410ULL * kNanosecondsPerSecond + 500000ULL;
        result.serverSendNs = result.serverReceiveNs;
        result.clientReceiveNs = result.clientSendNs + 1000000ULL;
        CHECK(model.addSample(result));
    }
    return model;
}

// A card running `ppm` fast, reported twice a second at that room's own stream
// rate. Section 26's regression is what this is for, and nothing here names an
// audible position, so the offset is left alone.
void driveDrift(Zone& zone, const ClientId& client, const ClientClockModel& model,
                uint32_t rateHz, double ppm, int reports)
{
    uint64_t clientNs = 500ULL * kNanosecondsPerSecond;
    for (int i = 1; i <= reports; ++i)
    {
        clientNs += 500000000ULL;
        matp::PlaybackReport report;
        report.clockQuality = AudioClockQuality::Hardware;
        report.localMonotonicTimeNs = clientNs;
        report.bufferedDurationNs = 1500000000ULL;
        report.deviceFramePosition = static_cast<uint64_t>(
            static_cast<double>(rateHz) * (1.0 + ppm * 1e-6) * static_cast<double>(i) * 0.5 +
            0.5);
        zone.onPlaybackReport(client, report, model);
    }
}

// The same reports, with an audible position `errorNs` away from where this
// zone's line says that room should be. That is section 29's offset, and past
// section 32's threshold it is a step rather than something to resample away.
void driveOffset(Zone& zone, const std::vector<ClientId>& rooms, const ClientClockModel& model,
                 uint32_t rateHz, int64_t errorNs, int reports)
{
    uint64_t clientNs = 500ULL * kNanosecondsPerSecond;
    for (int i = 1; i <= reports; ++i)
    {
        clientNs += 500000000ULL;
        const uint64_t serverNs = model.toServerTimeNs(clientNs);
        const uint64_t expected = std::to_underlying(zone.startTimelineNs()) +
                                  (serverNs - zone.startServerTimeNs());

        matp::PlaybackReport report;
        report.clockQuality = AudioClockQuality::Hardware;
        report.localMonotonicTimeNs = clientNs;
        report.bufferedDurationNs = 1500000000ULL;
        report.deviceFramePosition = static_cast<uint64_t>(
            static_cast<double>(rateHz) * static_cast<double>(i) * 0.5 + 0.5);
        report.hasTimelineNsAudible = true;
        report.timelineNsAudible =
            static_cast<uint64_t>(static_cast<int64_t>(expected) + errorNs);

        for (const ClientId& room : rooms)
            zone.onPlaybackReport(room, report, model);
    }
}

// Every packet a room was sent, checked the way the client checks it: the
// numbering has no gap on one connection, whatever the timeline does. The
// answer is how many times the timeline jumped, which is how many rejoins that
// room saw.
size_t jumpsIn(SinkState& state, size_t& packets, uint64_t* firstJumpNs = nullptr)
{
    std::string payloads;
    const std::vector<matp::AudioPacketHeader> headers = decodeHeaders(state, payloads);
    packets = headers.size();
    if (firstJumpNs != nullptr)
        *firstJumpNs = 0;
    if (headers.empty())
        return 0;

    size_t jumps = 0;
    uint64_t expectedNs = headers.front().timelineStartNs;
    for (size_t index = 0; index < headers.size(); ++index)
    {
        CHECK_EQ(headers[index].sequenceNumber, uint64_t(index));
        if (headers[index].timelineStartNs != expectedNs)
        {
            if (jumps == 0 && firstJumpNs != nullptr)
                *firstJumpNs = headers[index].timelineStartNs;
            ++jumps;
        }
        expectedNs = headers[index].timelineStartNs + headers[index].timelineDurationNs;
    }
    return jumps;
}

} // namespace

MULTIROOM_TEST(a_cut_connection_keeps_what_belongs_to_the_room_and_the_others_do_not_notice)
{
    // The thirty-second cut, from this end. Section 43: what went away is the
    // connection and not the room, so the stream stops and the membership goes
    // -- there is no session to send anything under -- and the drift model and
    // the correction in force are kept against the return. The room that lost
    // nothing is asserted on its packet stream rather than observed.
    ZoneRig rig(formatOf(), static_cast<uint64_t>(kRate) * 120);
    rig.clock.setNs(90ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    settings.output.targetBufferNs = 2ULL * kNanosecondsPerSecond;
    settings.lookaheadNs = 3ULL * kNanosecondsPerSecond;
    settings.lateJoinLeadNs = 2ULL * kNanosecondsPerSecond;
    settings.retentionNs = 60ULL * kNanosecondsPerSecond;
    settings.start.startDelayNs = kNanosecondsPerSecond;
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    // A device that cannot play the source format is resampled, and resampled
    // is what makes a correction -- and therefore a correction worth keeping.
    const AudioFormat resampled = formatOf(48000, kChannels, AudioFormat::SampleType::Int32);
    const ClientId steady("livingroom");
    const ClientId cut("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), steady, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), cut, offering({resampled}), resampled));

    auto steadySink = connect(*zone, sessionOf(1), steady);
    auto cutSink = connect(*zone, sessionOf(2), cut);
    CHECK(zone->member(cut)->driftCorrected());

    reportBuffer(*zone, steady, 0, 1500 * 1000000ULL);
    reportBuffer(*zone, cut, 0, 1500 * 1000000ULL);
    zone->pump();
    CHECK(zone->started());
    acknowledge(*zone, steady, std::to_underlying(zone->startTimelineNs()));
    acknowledge(*zone, cut, std::to_underlying(zone->startTimelineNs()));

    const ClientClockModel model = fittedModel();
    driveDrift(*zone, cut, model, 48000, 30.0, 40);
    const ClientSyncStatus measured = zone->syncStatus(cut);
    CHECK_NEAR(measured.driftPpm, 30.0, 1.0);
    CHECK(measured.ratio > 1.0);

    const std::shared_ptr<ClientOutput> playing = zone->member(steady);
    CHECK(waitFor([&] { return playing->packets() > 0; }));
    const uint64_t sentBefore = playing->packets();

    // The cable. Its control connection goes, and with it its stream, its
    // session and its place in the barrier -- and nothing else.
    zone->disconnect(cut);
    CHECK_EQ(zone->memberCount(), size_t{1});
    CHECK(zone->member(cut) == nullptr);

    rig.clock.advanceMs(30000);
    zone->pump();
    CHECK(zone->state() != ZoneState::Failed);
    CHECK(playing->state() == OutputState::Streaming);
    CHECK_EQ(zone->rejoins(steady), 0ULL);

    // And it comes back: a new session, the ordinary handshake, the ordinary
    // admit -- with the state it left behind put back under it.
    const uint64_t playhead = std::to_underlying(zone->playheadNs());
    CHECK_OK(zone->admit(sessionOf(3), cut, offering({resampled}), resampled));

    const ClientSyncStatus back = zone->syncStatus(cut);
    CHECK_EQ(back.reports, measured.reports); // the lifetime counters are the same room's
    CHECK(!back.hasDrift);                    // and the window is honestly empty
    // The filtered drift the room was being given, which a full window of a
    // steady card has all but caught up with the last fit.
    CHECK_NEAR(back.carriedPpm, measured.driftPpm, 0.01);
    CHECK_NEAR(back.ratio, measured.ratio, 1e-12);
    CHECK_EQ(zone->rejoins(cut), 1ULL);
    CHECK_EQ(zone->rejoinCount(), 1ULL);

    // Section 43 step 5: it begins on a position nobody has heard yet, so what
    // it prebuffers is audio it will be started into. Old packets are never
    // replayed at speed to catch up.
    // A floor rather than an equality: admit() starts that room's own thread,
    // so by the time this reads it the stream may already have been built past
    // where it begins -- and "ahead of what is audible" is the claim, not any
    // particular packet.
    const std::shared_ptr<ClientOutput> returned = zone->member(cut);
    CHECK(returned != nullptr);
    CHECK(std::to_underlying(returned->producedThroughNs()) >=
          playhead + 2ULL * kNanosecondsPerSecond);

    // The room that never lost anything: one unbroken stream, no gap in the
    // numbering and no jump on the timeline.
    size_t packets = 0;
    CHECK_EQ(jumpsIn(*steadySink, packets), size_t{0});
    CHECK(packets >= sentBefore);
    CHECK_EQ(rig.starts->of(RoomCommandKind::Start, "livingroom").size(), size_t{1});

    // And the one that did lost only the connection: everything it was sent
    // before the cut is a contiguous run, because a cut is not a discontinuity
    // -- the discontinuity is where it comes back.
    CHECK_EQ(jumpsIn(*cutSink, packets), size_t{0});
}

MULTIROOM_TEST(a_room_the_decode_ran_past_is_rejoined_at_a_position_the_store_holds)
{
    // The store eviction of phase 16's "done when". A room stops reporting,
    // which is what a connection that is not there looks like to the pacing
    // rule; the decode follows the line and the room that is still draining,
    // and the store drops the audio the first room's cursor named. That is not
    // a failure -- the room is intact and the only thing it is missing is
    // where it is now. Since phase 39 it needs a running line: before there is
    // one, no room may be built more than a prebuffer past where it began.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    settings.output.targetBufferNs = 200ULL * 1000000ULL;
    settings.lookaheadNs = 300ULL * 1000000ULL;
    settings.lateJoinLeadNs = 300ULL * 1000000ULL;
    settings.retentionNs = kNanosecondsPerSecond;
    settings.start.minimumStartBufferNs = 100ULL * 1000000ULL;
    settings.start.startDelayNs = 100ULL * 1000000ULL;
    std::mutex movesMutex;
    std::vector<ZoneEvent> moves;
    settings.onEvent = [&](const ZoneEvent& event) {
        std::lock_guard<std::mutex> lock(movesMutex);
        if (event.kind == ZoneEventKind::RoomFellBehind)
            moves.push_back(event);
    };
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId draining("livingroom");
    const ClientId quiet("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), draining, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), quiet, offering({formatOf()}), formatOf()));
    auto drainingSink = connect(*zone, sessionOf(1), draining);
    auto quietSink = connect(*zone, sessionOf(2), quiet);

    const std::shared_ptr<ClientOutput> ahead = zone->member(draining);
    const std::shared_ptr<ClientOutput> behind = zone->member(quiet);

    // Both start together; then one room drains as a playing room does, and
    // the other's device has stopped: it keeps reporting the same full buffer.
    // (A room whose reports stop altogether is assumed to be draining and is
    // paced on the line -- see ClientOutput::setOnLine.)
    zone->pump();
    CHECK(waitFor([&] { return behind->packets() > 0 && ahead->packets() > 0; }));
    reportBuffer(*zone, draining, 0, 200ULL * 1000000ULL);
    reportBuffer(*zone, quiet, 0, 200ULL * 1000000ULL);
    zone->pump();
    CHECK(zone->started());
    acknowledge(*zone, draining, std::to_underlying(zone->startTimelineNs()));
    acknowledge(*zone, quiet, std::to_underlying(zone->startTimelineNs()));

    // Two and a half seconds of a three-second source against a retention of
    // one: the decode follows the line, and the quiet room's audio ages out.
    for (int step = 0; step < 25; ++step)
    {
        rig.clock.advanceMs(100);
        const uint64_t playhead = std::to_underlying(zone->playheadNs());
        const uint64_t built = deliveredThroughNs(*ahead, *drainingSink);
        reportBuffer(*zone, draining, playhead, built > playhead ? built - playhead : 0);
        reportBuffer(*zone, quiet, 0, 200ULL * 1000000ULL);
        CHECK(waitFor([&] {
            zone->pump();
            return std::to_underlying(ahead->producedThroughNs()) >=
                   playhead + 200ULL * 1000000ULL;
        }));
    }
    CHECK(behind->state() == OutputState::Streaming);
    const uint64_t heldBefore = behind->packets();
    CHECK(heldBefore > 0);

    // Its connection comes back and it reports the empty buffer of section 43
    // step 5, which is the one thing that lets it produce again -- and what it
    // finds is that the audio its cursor named is gone.
    reportBuffer(*zone, quiet, 0, 0);
    CHECK(waitFor([&] { return behind->state() == OutputState::Stranded; }));
    CHECK(behind->lastError().find("Evicted") != std::string::npos);

    // The pipeline is what notices, on its next pass, and it moves that room
    // and no other.
    zone->pump();
    CHECK(waitFor([&] { return behind->state() == OutputState::Streaming; }));
    CHECK_EQ(zone->rejoins(quiet), 1ULL);
    CHECK_EQ(zone->rejoins(draining), 0ULL);
    CHECK(std::to_underlying(behind->producedThroughNs()) >=
          std::to_underlying(zone->oldestNs()));
    CHECK(zone->state() != ZoneState::Failed);

    // Phase 24: said once, naming the trigger.
    {
        std::lock_guard<std::mutex> lock(movesMutex);
        CHECK_EQ(moves.size(), size_t{1});
        if (!moves.empty())
        {
            CHECK_EQ(moves.front().client.str(), std::string("kitchen"));
            CHECK(moves.front().trigger == RejoinTrigger::Evicted);
        }
    }

    // One jump on the moved room's stream and no gap in its numbering: the
    // client reads the first as a rejoin and the second as a server that
    // skipped ahead and would drop the connection over it.
    size_t packets = 0;
    CHECK(waitFor([&] { return behind->packets() > heldBefore; }));
    CHECK_EQ(jumpsIn(*quietSink, packets), size_t{1});
    CHECK(packets > heldBefore);

    // And the room that never stopped draining was never moved, and its stream
    // is unbroken.
    CHECK_EQ(jumpsIn(*drainingSink, packets), size_t{0});
    CHECK(packets > 0);
}

MULTIROOM_TEST(a_forced_hard_resync_moves_one_room_and_no_other)
{
    // The third of phase 16's "done when", and the one a caller asks for.
    // Section 32 reaches the wire as section 43's rejoin -- a discontinuity in
    // timelineStartNs and a START_AT for the new position -- which is why
    // there is no HARD_RESYNC message and why this is one call.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    settings.lateJoinLeadNs = 2ULL * kNanosecondsPerSecond;
    settings.start.startDelayNs = kNanosecondsPerSecond;
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId still("livingroom");
    const ClientId moved("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), still, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), moved, offering({formatOf()}), formatOf()));
    auto stillSink = connect(*zone, sessionOf(1), still);
    auto movedSink = connect(*zone, sessionOf(2), moved);

    reportBuffer(*zone, still, 0, 1500 * 1000000ULL);
    reportBuffer(*zone, moved, 0, 1500 * 1000000ULL);
    zone->pump();
    CHECK(zone->started());
    acknowledge(*zone, still, std::to_underlying(zone->startTimelineNs()));
    acknowledge(*zone, moved, std::to_underlying(zone->startTimelineNs()));
    CHECK(waitFor([&] { return zone->member(moved)->packets() > 0; }));

    rig.clock.advanceMs(1000);
    const uint64_t playhead = std::to_underlying(zone->playheadNs());

    CHECK_OK(zone->rejoin(moved, RejoinTrigger::HardResync));
    CHECK_EQ(zone->rejoins(moved), 1ULL);
    CHECK_EQ(zone->rejoins(still), 0ULL);
    CHECK_EQ(zone->rejoinCount(), 1ULL);

    const std::shared_ptr<ClientOutput> restarted = zone->member(moved);
    CHECK(restarted->state() == OutputState::Streaming);
    CHECK(std::to_underlying(restarted->producedThroughNs()) >=
          playhead + 2ULL * kNanosecondsPerSecond);

    // It is placed on the running line again from what it reports about the
    // audio it holds *now*: what it said about the stream it used to have is
    // not an answer about the one it has.
    CHECK(zone->roomStart(moved) == RoomStartState::Waiting);
    zone->pump();
    CHECK_EQ(rig.starts->of(RoomCommandKind::Start, "kitchen").size(), size_t{1});

    reportBuffer(*zone, moved, playhead + 2ULL * kNanosecondsPerSecond, 1500 * 1000000ULL);
    zone->pump();
    const std::vector<RoomCommand> starts = rig.starts->of(RoomCommandKind::Start, "kitchen");
    CHECK_EQ(starts.size(), size_t{2});
    CHECK_EQ(starts.back().start.timelineNs, playhead + 2ULL * kNanosecondsPerSecond);

    // The room that was not moved was told nothing and heard nothing: no
    // second start, no pause, and one unbroken stream.
    CHECK_EQ(rig.starts->of(RoomCommandKind::Start, "livingroom").size(), size_t{1});
    CHECK_EQ(rig.starts->count(RoomCommandKind::Pause), size_t{0});
    CHECK(zone->roomStart(still) == RoomStartState::Playing);

    // Its packets are sent by its own thread, so they are waited for.
    size_t packets = 0;
    CHECK(waitFor([&] { return jumpsIn(*stillSink, packets) == 0 && packets > 0; }));
    CHECK_EQ(jumpsIn(*stillSink, packets), size_t{0});

    // One jump on the moved one, landing exactly where it was moved to, with
    // its numbering running straight through it. This is where the position is
    // asserted exactly: the packets are what the client sees, and they do not
    // race this thread the way a counter does.
    uint64_t jumpNs = 0;
    CHECK(waitFor([&] { return jumpsIn(*movedSink, packets, &jumpNs) == 1; }));
    CHECK_EQ(jumpNs, playhead + 2ULL * kNanosecondsPerSecond);

    // A room this zone does not hold is not a room to move.
    CHECK_ERR(zone->rejoin(ClientId("elsewhere"), RejoinTrigger::HardResync),
              ZoneError::NotMember);

    // And neither is a room a stop parked: that one is waiting for a source
    // rather than for a position, and the next play() is what moves it
    // (section 38). Moving it here would start it into a timeline nobody is
    // playing.
    CHECK_OK(zone->stopPlayback());
    CHECK(waitFor([&] { return zone->member(still)->state() == OutputState::Parked; }));
    CHECK_ERR(zone->rejoin(still, RejoinTrigger::HardResync), ZoneError::OutputFailed);
    CHECK_EQ(zone->rejoins(still), 0ULL);
}

MULTIROOM_TEST(a_bitperfect_room_is_never_resynced_for_drift_and_a_corrected_one_is)
{
    // Phase 13's rule carried into phase 16, and the test the roadmap says
    // matters as much as the correction itself. Two rooms the same distance
    // off the timeline: the one nobody is correcting has no threshold at all,
    // because the offset it is accumulating is what the operator chose for it
    // and a reposition every quarter of an hour is the worse answer. The one
    // being corrected is moved, because no ratio inside section 31's limits
    // closes eighty milliseconds before the room is audibly out.
    ZoneRig rig;
    rig.clock.setNs(90ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    settings.lateJoinLeadNs = 2ULL * kNanosecondsPerSecond;
    settings.start.startDelayNs = kNanosecondsPerSecond;
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const AudioFormat resampled = formatOf(48000, kChannels, AudioFormat::SampleType::Int32);
    const ClientId left("livingroom");
    const ClientId right("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), left, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), right, offering({resampled}), resampled));
    CHECK(!zone->member(left)->driftCorrected());
    CHECK(zone->member(right)->driftCorrected());

    reportBuffer(*zone, left, 0, 1500 * 1000000ULL);
    reportBuffer(*zone, right, 0, 1500 * 1000000ULL);
    zone->pump();
    CHECK(zone->started());

    // Both rooms eighty milliseconds ahead of where this zone's line says they
    // should be, reported together, through one clock model.
    const ClientClockModel model = fittedModel();
    driveOffset(*zone, {left, right}, model, kRate, 80LL * 1000000LL, 12);

    CHECK(zone->syncStatus(left).hasOffset);
    CHECK_NEAR(static_cast<double>(zone->syncStatus(left).offsetNs), 80e6, 2e6);
    CHECK(zone->syncStatus(left).resyncWanted); // measured on both...
    CHECK_EQ(zone->rejoins(left), 0ULL);        // ...and acted on for neither of them
    CHECK_EQ(zone->member(left)->driftTrim(), 1.0);

    // The corrected one was moved.
    CHECK(zone->rejoins(right) >= 1ULL);
    CHECK_EQ(zone->rejoinCount(), zone->rejoins(right));
    CHECK(zone->member(right)->state() == OutputState::Streaming);
    CHECK(zone->roomStart(right) == RoomStartState::Waiting);

    // And the move dropped the offset that asked for it, so the room is not
    // moved again for an error it is no longer at.
    CHECK(!zone->syncStatus(right).resyncWanted);
    CHECK(!zone->syncStatus(right).hasOffset);
    CHECK(zone->state() != ZoneState::Failed);
}

// --- phase 15: the format transition barrier ---------------------------------

namespace {

// 44.1 kHz ("ramp") followed by 48 kHz ("other"), three seconds of frames
// each. The boundary is at exactly 3 s, which is a whole number of packets, so
// every stream's packets before it are whole and the check is the transition
// rather than a short packet.
constexpr uint64_t kBoundaryNs = 3ULL * kNanosecondsPerSecond;
constexpr uint32_t kHighRatePacketFrames = 960; // 20 ms at 48 kHz

// A device that can open either source exactly, and one that can open
// neither and is resampled into one fixed rate for both.
ClientCapabilities eitherRate()
{
    return offering({formatOf(), formatOf(48000)});
}

AudioFormat fixedRate()
{
    return formatOf(96000, kChannels, AudioFormat::SampleType::Int32);
}

// Two tracks in two formats staged into a zone whose line has started, with
// both tracks decoded: every room has been held or switched at the boundary
// before this returns.
void playTwoFormats(Zone& zone, const std::vector<ClientId>& rooms)
{
    for (const ClientId& room : rooms)
        reportBuffer(zone, room, 0, 1500 * 1000000ULL);
    zone.pump();
    CHECK(zone.started());
    for (const ClientId& room : rooms)
        acknowledge(zone, room, std::to_underlying(zone.startTimelineNs()));
    CHECK_EQ(zone.sources().size(), 2u);
}

} // namespace

MULTIROOM_TEST(a_room_on_a_fixed_rate_plays_through_a_format_boundary_the_other_room_reopens_for)
{
    // Section 24's table, both rows on one timeline. The corrected room is
    // resampled into 96 kHz whatever the source is, so its device never has
    // to change: it is given the new converter at the boundary and is not
    // paused, prepared or restarted. The bitperfect room's device has to be
    // reopened at 48 kHz, so it is paused on the boundary, asked to
    // re-negotiate when the line gets there, and carries on from the boundary
    // in the new format -- while the line runs on for the room that did not
    // stop.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->stage(segmentOf("two", "other")));
    CHECK_OK(zone->play());

    const ClientId living("livingroom");
    const ClientId kitchen("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), living, eitherRate(), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), kitchen, offering({fixedRate()}), fixedRate()));
    auto livingSink = connect(*zone, sessionOf(1), living);
    auto kitchenSink = connect(*zone, sessionOf(2), kitchen);

    const std::shared_ptr<ClientOutput> reopening = zone->member(living);
    const std::shared_ptr<ClientOutput> through = zone->member(kitchen);
    CHECK(reopening->plan().mode == OutputMode::BitPerfect);
    CHECK(through->driftCorrected());

    playTwoFormats(*zone, {living, kitchen});
    CHECK(zone->state() != ZoneState::Failed);
    CHECK_EQ(std::to_underlying(zone->sources()[1].timelineStartNs), kBoundaryNs);

    // The room that plays through has its converter replaced at the boundary
    // and carries on in the format its device already has.
    CHECK(waitFor([&] { return through->formatSwitches() == 1; }));
    CHECK(through->driftCorrected());
    CHECK_EQ(describeFormat(through->plan().sourceFormat), describeFormat(formatOf(48000)));
    CHECK_EQ(describeFormat(through->plan().outputFormat), describeFormat(fixedRate()));

    // The room that reopens plays the old source to its end and stops exactly
    // on the boundary, and is paused there on the instant the line reaches it
    // -- a position and an instant on the same line as everything else.
    CHECK(waitFor([&] { return reopening->packets() >= kSourcePackets; }));
    CHECK_EQ(std::to_underlying(reopening->producedThroughNs()), kBoundaryNs);
    const std::vector<RoomCommand> pauses = rig.starts->of(RoomCommandKind::Pause);
    CHECK_EQ(pauses.size(), size_t{1});
    CHECK_EQ(pauses.front().client.str(), std::string("livingroom"));
    CHECK_EQ(pauses.front().at.timelineNs, kBoundaryNs);
    CHECK_EQ(pauses.front().at.serverTimeNs,
             zone->startServerTimeNs() +
                 (kBoundaryNs - std::to_underlying(zone->startTimelineNs())));
    CHECK(zone->roomStart(living) == RoomStartState::Paused);

    // Nobody is asked to reopen anything before the old source has been
    // played out of the device that is going to be reopened.
    zone->pump();
    CHECK_EQ(rig.starts->count(RoomCommandKind::Renegotiate), size_t{0});

    rig.clock.advanceMs(4100);
    zone->pump();
    const std::vector<RoomCommand> renegotiations =
        rig.starts->of(RoomCommandKind::Renegotiate);
    CHECK_EQ(renegotiations.size(), size_t{1});
    CHECK_EQ(renegotiations.front().client.str(), std::string("livingroom"));

    // The other room is still on the line, so the line does not wait.
    CHECK(zone->transport() == TransportState::Playing);

    // Step 5: the device answers in the new format, and the stream carries on
    // from the boundary on the same connection with the numbering unbroken.
    // The converter is installed by the room's own thread, so it is waited
    // for rather than read.
    zone->onStreamReady(living, true, formatOf(48000));
    CHECK(zone->member(living) == reopening);
    CHECK(waitFor([&] { return reopening->formatSwitches() == 1; }));
    CHECK_EQ(describeFormat(reopening->plan().outputFormat), describeFormat(formatOf(48000)));
    CHECK(reopening->plan().mode == OutputMode::BitPerfect);
    CHECK(zone->roomStart(living) == RoomStartState::Waiting);

    // The new source to its last frame: 137 whole packets, then the 780
    // frames left over as a short one that ends where the source does.
    constexpr size_t kHighRateWholePackets = kSourceFrames / kHighRatePacketFrames; // 137
    constexpr auto kHighRateTailFrames =
        static_cast<uint32_t>(kSourceFrames % kHighRatePacketFrames); // 780
    constexpr size_t kHighRatePackets = kHighRateWholePackets + 1;
    constexpr uint64_t kSecondEndNs = kBoundaryNs + kSourceFrames * kNanosecondsPerSecond / 48000;
    CHECK(waitFor([&] { return reopening->packets() >= kSourcePackets + kHighRateWholePackets; }));

    // It is placed on the running line wherever the line has got to, inside
    // what it holds: in sync with the room that never stopped, at the cost of
    // the beginning of the new track -- two rooms on one timeline, one of them
    // silent while its device reopened.
    const uint64_t playhead = std::to_underlying(zone->playheadNs());
    reportBuffer(*zone, living, kBoundaryNs, 1500 * 1000000ULL);
    zone->pump();
    const std::vector<RoomCommand> livingStarts =
        rig.starts->of(RoomCommandKind::Start, "livingroom");
    CHECK_EQ(livingStarts.size(), size_t{2});
    const matp::StartAt& resumed = livingStarts.back().start;
    CHECK_EQ(resumed.timelineNs, playhead + kNanosecondsPerSecond);
    CHECK_EQ(resumed.serverTimeNs,
             zone->startServerTimeNs() +
                 (resumed.timelineNs - std::to_underlying(zone->startTimelineNs())));

    // Sample-accurate on both sides of the gap: the old source to its last
    // frame, then the new one from its first to its last, with no jump on the
    // timeline and no gap in the numbering. What the sink holds is waited for,
    // because the short last packet is built once the zone has published its
    // end, which can be after the whole packets before it.
    std::string payloads;
    std::vector<matp::AudioPacketHeader> headers;
    CHECK(waitFor([&] {
        headers = decodeHeaders(*livingSink, payloads);
        return headers.size() >= kSourcePackets + kHighRatePackets;
    }));
    size_t packets = 0;
    CHECK_EQ(jumpsIn(*livingSink, packets), size_t{0});
    CHECK_EQ(headers.size(), kSourcePackets + kHighRatePackets);
    if (!headers.empty())
    {
        const matp::AudioPacketHeader& last = headers.back();
        CHECK_EQ(last.outputFrameCount, kHighRateTailFrames);
        CHECK_EQ(last.timelineStartNs, kBoundaryNs + kHighRateWholePackets * kPacketNs);
        CHECK_EQ(last.timelineStartNs + last.timelineDurationNs, kSecondEndNs);
    }
    CHECK_EQ(headers[kSourcePackets - 1].timelineStartNs + kPacketNs, kBoundaryNs);
    CHECK_EQ(headers[kSourcePackets].timelineStartNs, kBoundaryNs);
    CHECK_EQ(describeFormat(headers[kSourcePackets - 1].format), describeFormat(formatOf()));
    CHECK_EQ(describeFormat(headers[kSourcePackets].format), describeFormat(formatOf(48000)));
    CHECK_EQ(headers[kSourcePackets].outputFrameCount, kHighRatePacketFrames);

    const uint32_t bytesPerFrame = formatOf().bytesPerFrame();
    const size_t newFrames = static_cast<size_t>(kSourceFrames);
    std::vector<std::byte> expected((static_cast<size_t>(kSourceFrames) + newFrames) *
                                    bytesPerFrame);
    writeRamp(expected, 0, static_cast<uint32_t>(kSourceFrames), kChannels);
    writeRamp(std::span<std::byte>(expected).subspan(static_cast<size_t>(kSourceFrames) *
                                                     bytesPerFrame),
              0, static_cast<uint32_t>(newFrames), kChannels);
    CHECK_EQ(payloads.size(), expected.size());
    CHECK_EQ(std::memcmp(payloads.data(), expected.data(), expected.size()), 0);

    // And the room that played through was not paused, not prepared and not
    // restarted: one start, the same output, one unbroken stream in one
    // format across the boundary.
    CHECK(zone->member(kitchen) == through);
    CHECK_EQ(rig.starts->of(RoomCommandKind::Start, "kitchen").size(), size_t{1});
    CHECK_EQ(rig.starts->of(RoomCommandKind::Pause, "kitchen").size(), size_t{0});
    CHECK_EQ(rig.starts->of(RoomCommandKind::Renegotiate, "kitchen").size(), size_t{0});
    CHECK(zone->roomStart(kitchen) == RoomStartState::Playing);

    CHECK(waitFor([&] { return through->packets() > kSourcePackets + 100; }));
    CHECK_EQ(jumpsIn(*kitchenSink, packets), size_t{0});
    const std::vector<matp::AudioPacketHeader> kitchenHeaders =
        decodeHeaders(*kitchenSink, payloads);
    CHECK_EQ(kitchenHeaders[kSourcePackets].timelineStartNs, kBoundaryNs);
    for (const matp::AudioPacketHeader& header : kitchenHeaders)
        CHECK_EQ(describeFormat(header.format), describeFormat(fixedRate()));

    CHECK(zone->state() != ZoneState::Failed);
    CHECK_EQ(zone->rejoinCount(), 0ULL);
}

MULTIROOM_TEST(a_room_reopened_in_another_format_after_a_boundary_is_planned_for_the_new_source)
{
    // A room playing through a boundary that has been opened, whose device is
    // then reopened in another format before the line gets there. Its move
    // lands past the boundary -- a join never starts before the newest
    // source -- so its new converter is for that source, not for the one the
    // room was playing.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto zone = rig.zone(rig.settings());

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->stage(segmentOf("two", "other")));
    CHECK_OK(zone->play());

    const ClientId kitchen("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), kitchen, offering({fixedRate()}), fixedRate()));
    connect(*zone, sessionOf(1), kitchen);
    const std::shared_ptr<ClientOutput> output = zone->member(kitchen);

    playTwoFormats(*zone, {kitchen});
    CHECK_EQ(std::to_underlying(zone->sources()[1].timelineStartNs), kBoundaryNs);
    CHECK_EQ(describeFormat(output->plan().sourceFormat), describeFormat(formatOf()));

    const AudioFormat hdmi = formatOf(48000, kChannels, AudioFormat::SampleType::Int32);
    const ClientCapabilities card = offering({hdmi});
    zone->onStreamReady(kitchen, true, hdmi, &card);

    CHECK(zone->member(kitchen) == output);
    CHECK_EQ(zone->rejoinCount(), 1ULL);
    CHECK_EQ(describeFormat(output->plan().sourceFormat), describeFormat(formatOf(48000)));
    CHECK_EQ(describeFormat(output->plan().outputFormat), describeFormat(hdmi));
    CHECK(std::to_underlying(output->producedThroughNs()) >= kBoundaryNs);
    CHECK(zone->state() != ZoneState::Failed);
}

MULTIROOM_TEST(rooms_that_all_reopen_resume_together_exactly_where_the_old_source_ended)
{
    // Nobody plays through, so the line itself waits at the boundary (section
    // 24 steps 6 and 8): both rooms are started at the boundary, at one
    // instant, and their streams are contiguous across it.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->stage(segmentOf("two", "other")));
    CHECK_OK(zone->play());

    const ClientId living("livingroom");
    const ClientId bedroom("bedroom");
    CHECK_OK(zone->admit(sessionOf(1), living, eitherRate(), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), bedroom, eitherRate(), formatOf()));
    auto livingSink = connect(*zone, sessionOf(1), living);
    auto bedroomSink = connect(*zone, sessionOf(2), bedroom);

    playTwoFormats(*zone, {living, bedroom});
    const std::vector<RoomCommand> pauses = rig.starts->of(RoomCommandKind::Pause);
    CHECK_EQ(pauses.size(), size_t{2});
    CHECK_EQ(pauses.front().at.timelineNs, kBoundaryNs);
    CHECK_EQ(pauses.front().at.serverTimeNs, pauses.back().at.serverTimeNs);

    rig.clock.advanceMs(4100);
    zone->pump();
    CHECK_EQ(rig.starts->count(RoomCommandKind::Renegotiate), size_t{2});
    CHECK(zone->transport() == TransportState::Starting);
    CHECK_EQ(std::to_underlying(zone->startTimelineNs()), kBoundaryNs);
    CHECK_EQ(std::to_underlying(zone->playheadNs()), kBoundaryNs);

    zone->onStreamReady(living, true, formatOf(48000));
    zone->onStreamReady(bedroom, true, formatOf(48000));
    reportBuffer(*zone, living, kBoundaryNs, 1500 * 1000000ULL);
    reportBuffer(*zone, bedroom, kBoundaryNs, 1500 * 1000000ULL);
    zone->pump();

    // Step 8, for both: START_AT the boundary, at one instant, a start delay
    // ahead of the moment the last of them was ready.
    const std::vector<RoomCommand> starts = rig.starts->of(RoomCommandKind::Start);
    CHECK_EQ(starts.size(), size_t{4});
    CHECK_EQ(starts[2].start.timelineNs, kBoundaryNs);
    CHECK_EQ(starts[3].start.timelineNs, kBoundaryNs);
    CHECK_EQ(starts[2].start.serverTimeNs, starts[3].start.serverTimeNs);
    CHECK_EQ(starts[2].start.serverTimeNs, rig.clock.monotonicNowNs() + kNanosecondsPerSecond);
    CHECK(zone->transport() == TransportState::Playing);

    // And the streams say the same thing the starts do: nothing lost and
    // nothing repeated at the boundary.
    const std::shared_ptr<ClientOutput> a = zone->member(living);
    const std::shared_ptr<ClientOutput> b = zone->member(bedroom);
    CHECK(waitFor([&] {
        return a->packets() > kSourcePackets && b->packets() > kSourcePackets;
    }));
    size_t packets = 0;
    CHECK_EQ(jumpsIn(*livingSink, packets), size_t{0});
    CHECK_EQ(jumpsIn(*bedroomSink, packets), size_t{0});
    CHECK_EQ(a->formatSwitches(), 1u);
    CHECK_EQ(b->formatSwitches(), 1u);
    CHECK_EQ(zone->rejoinCount(), 0ULL);
}

MULTIROOM_TEST(a_room_that_misses_the_ready_timeout_joins_during_the_new_track)
{
    // Section 24 step 6's bound and step 10: the line waits for a reopening
    // device for the ready timeout and not a moment longer, and a room that
    // answers afterwards is placed on the line that is by then running.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->stage(segmentOf("two", "other")));
    CHECK_OK(zone->play());

    const ClientId quick("livingroom");
    const ClientId slow("bedroom");
    CHECK_OK(zone->admit(sessionOf(1), quick, eitherRate(), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), slow, eitherRate(), formatOf()));
    playTwoFormats(*zone, {quick, slow});

    rig.clock.advanceMs(4100);
    zone->pump();
    CHECK(zone->transport() == TransportState::Starting);

    // One device answers; the other is still opening, so the line holds.
    zone->onStreamReady(quick, true, formatOf(48000));
    reportBuffer(*zone, quick, kBoundaryNs, 1500 * 1000000ULL);
    zone->pump();
    CHECK_EQ(rig.starts->count(RoomCommandKind::Start), size_t{2});

    // Five seconds later it does not: the room that is ready is started at
    // the boundary without the one that is not.
    rig.clock.advanceMs(5000);
    zone->pump();
    const std::vector<RoomCommand> quickStarts =
        rig.starts->of(RoomCommandKind::Start, "livingroom");
    CHECK_EQ(quickStarts.size(), size_t{2});
    CHECK_EQ(quickStarts.back().start.timelineNs, kBoundaryNs);
    CHECK_EQ(rig.starts->of(RoomCommandKind::Start, "bedroom").size(), size_t{1});
    CHECK(zone->transport() == TransportState::Playing);

    // The slow device answers at last, and joins during the new track: its
    // stream still begins at the boundary, and the line places it wherever
    // it has got to inside that.
    zone->onStreamReady(slow, true, formatOf(48000));
    CHECK(waitFor([&] { return zone->member(slow)->formatSwitches() == 1; }));
    CHECK_EQ(describeFormat(zone->member(slow)->plan().outputFormat),
             describeFormat(formatOf(48000)));
    reportBuffer(*zone, slow, kBoundaryNs, 1500 * 1000000ULL);
    zone->pump();

    const std::vector<RoomCommand> slowStarts = rig.starts->of(RoomCommandKind::Start, "bedroom");
    CHECK_EQ(slowStarts.size(), size_t{2});
    const matp::StartAt& late = slowStarts.back().start;
    CHECK(late.timelineNs > kBoundaryNs);
    CHECK_EQ(late.serverTimeNs,
             quickStarts.back().start.serverTimeNs + (late.timelineNs - kBoundaryNs));
    CHECK(zone->state() != ZoneState::Failed);
}

MULTIROOM_TEST(a_room_that_reconnects_during_a_transition_comes_back_in_the_new_format)
{
    // Section 43 inside section 24: the room's control connection goes while
    // its device is being reopened. It leaves the membership like any other
    // cut, and it comes back through the ordinary admit(), planned against
    // the new source and beginning past the boundary -- never in the old
    // source's blocks. The room that plays through notices nothing.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    settings.lateJoinLeadNs = 2ULL * kNanosecondsPerSecond;
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->stage(segmentOf("two", "other")));
    CHECK_OK(zone->play());

    const ClientId living("livingroom");
    const ClientId kitchen("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), living, eitherRate(), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), kitchen, offering({fixedRate()}), fixedRate()));
    auto kitchenSink = connect(*zone, sessionOf(2), kitchen);
    const std::shared_ptr<ClientOutput> through = zone->member(kitchen);

    playTwoFormats(*zone, {living, kitchen});
    rig.clock.advanceMs(4100);
    zone->pump();
    CHECK_EQ(rig.starts->count(RoomCommandKind::Renegotiate), size_t{1});

    // The cut, mid-transition, and the return under a new session.
    zone->disconnect(living);
    CHECK_EQ(zone->memberCount(), size_t{1});
    rig.clock.advanceMs(500);
    zone->pump();

    const uint64_t playhead = std::to_underlying(zone->playheadNs());
    CHECK_OK(zone->admit(sessionOf(3), living, eitherRate(), formatOf(48000)));
    const std::shared_ptr<ClientOutput> returned = zone->member(living);
    CHECK(returned != nullptr);
    CHECK(returned->plan().mode == OutputMode::BitPerfect);
    CHECK_EQ(describeFormat(returned->plan().sourceFormat), describeFormat(formatOf(48000)));
    CHECK_EQ(zone->rejoins(living), 1ULL);

    auto returnedSink = connect(*zone, sessionOf(3), living);
    CHECK(waitFor([&] { return returned->packets() > 0; }));
    std::string payloads;
    const std::vector<matp::AudioPacketHeader> headers = decodeHeaders(*returnedSink, payloads);
    CHECK(!headers.empty());
    CHECK(headers.front().timelineStartNs >= playhead + 2ULL * kNanosecondsPerSecond);
    for (const matp::AudioPacketHeader& header : headers)
        CHECK_EQ(describeFormat(header.format), describeFormat(formatOf(48000)));

    // And the other room never noticed any of it.
    CHECK(waitFor([&] { return through->packets() > kSourcePackets; }));
    size_t packets = 0;
    CHECK_EQ(jumpsIn(*kitchenSink, packets), size_t{0});
    CHECK_EQ(rig.starts->of(RoomCommandKind::Start, "kitchen").size(), size_t{1});
    CHECK_EQ(rig.starts->of(RoomCommandKind::Pause, "kitchen").size(), size_t{0});
    CHECK(zone->state() != ZoneState::Failed);
}

MULTIROOM_TEST(a_room_that_cannot_be_served_after_a_boundary_stays_held_and_says_why)
{
    // A build with no resampler and a device that can only open the old
    // format: nothing can be sent to it after the boundary. That is this
    // room's failure and not the zone's -- the kitchen plays the new source
    // -- and it is visible where an operator looks -- the room's status --
    // rather than only in the log.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    const OutputPlanner planner = OutputPlanner::withoutResampler();
    std::vector<ZoneEvent> silent;
    ZoneSettings settings = rig.settings();
    settings.onEvent = [&silent](const ZoneEvent& event) {
        if (event.kind == ZoneEventKind::RoomNotReady)
            silent.push_back(event);
    };
    auto created = Zone::create(ZoneId("living"), rig.memory, planner, rig.resamplers,
                                rig.factory, rig.clock, settings);
    CHECK_OK(created);
    std::unique_ptr<Zone> zone = std::move(*created);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->stage(segmentOf("two", "other")));
    CHECK_OK(zone->play());

    const ClientId living("livingroom");
    const ClientId kitchen("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), living, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), kitchen, eitherRate(), formatOf()));
    const std::shared_ptr<ClientOutput> output = zone->member(living);
    playTwoFormats(*zone, {living, kitchen});

    rig.clock.advanceMs(4100);
    zone->pump();
    CHECK_EQ(rig.starts->of(RoomCommandKind::Renegotiate, "livingroom").size(), size_t{1});

    // The device answers in the only format it has, and there is no plan
    // from the new source into it.
    zone->onStreamReady(living, true, formatOf());
    const std::string detail = zone->roomStartDetail(living);
    CHECK(detail.find("format boundary") != std::string::npos);
    CHECK(detail.find("NoResampler") != std::string::npos);

    // Phase 24: said as it happens, with the same reason status gives.
    CHECK_EQ(silent.size(), size_t{1});
    if (!silent.empty())
    {
        CHECK_EQ(silent.front().client.str(), std::string("livingroom"));
        CHECK_EQ(silent.front().reason, detail);
    }

    // Held and silent: nothing of the new source is sent in the old format.
    CHECK(waitFor([&] { return std::to_underlying(output->producedThroughNs()) == kBoundaryNs; }));
    CHECK(output->state() == OutputState::Streaming);
    CHECK_EQ(output->formatSwitches(), 0u);
    CHECK(zone->state() != ZoneState::Failed);
}

MULTIROOM_TEST(a_source_no_room_can_play_is_skipped_for_the_next)
{
    // The one room cannot be planned for the second source at all: held at
    // its boundary, it would hold the whole zone there for good. The source
    // is skipped as one that will not open is, and the third plays on.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    const OutputPlanner planner = OutputPlanner::withoutResampler();
    std::vector<ZoneEvent> failed;
    ZoneSettings settings = rig.settings();
    settings.onEvent = [&failed](const ZoneEvent& event) {
        if (event.kind == ZoneEventKind::SourceFailed)
            failed.push_back(event);
    };
    auto created = Zone::create(ZoneId("living"), rig.memory, planner, rig.resamplers,
                                rig.factory, rig.clock, settings);
    CHECK_OK(created);
    std::unique_ptr<Zone> zone = std::move(*created);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->stage(segmentOf("two", "other")));
    CHECK_OK(zone->stage(segmentOf("three", "ramp")));
    CHECK_OK(zone->play());

    const ClientId living("livingroom");
    CHECK_OK(zone->admit(sessionOf(1), living, offering({formatOf()}), formatOf()));
    playTwoFormats(*zone, {living});

    CHECK_EQ(zone->sources()[1].segment.sourceId, std::string("three"));
    CHECK_EQ(std::to_underlying(zone->sources()[1].timelineStartNs), kBoundaryNs);
    CHECK_EQ(failed.size(), size_t{1});
    if (!failed.empty())
    {
        CHECK_EQ(failed.front().sourceId, std::string("two"));
        CHECK(failed.front().reason.find("no room can play") != std::string::npos);
        CHECK(failed.front().reason.find("livingroom") != std::string::npos);
    }
    CHECK_EQ(rig.starts->count(RoomCommandKind::Renegotiate), size_t{0});
    CHECK(zone->state() != ZoneState::Failed);
}

MULTIROOM_TEST(a_zone_stopped_and_played_again_in_another_format_reopens_the_rooms_that_need_it)
{
    // Section 38 followed by section 24: every room was parked by the stop, so
    // there is no line to wait for and nothing left to play out. The room
    // whose device must change is asked to at once, and the first position of
    // what play() opened is its boundary.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId living("livingroom");
    CHECK_OK(zone->admit(sessionOf(1), living, eitherRate(), formatOf()));
    auto sink = connect(*zone, sessionOf(1), living);
    const std::shared_ptr<ClientOutput> output = zone->member(living);

    reportBuffer(*zone, living, 0, 1500 * 1000000ULL);
    zone->pump();
    CHECK(zone->started());
    CHECK(waitFor([&] { return output->packets() >= kSourcePackets; }));

    CHECK_OK(zone->stopPlayback());
    CHECK(output->state() == OutputState::Parked);

    CHECK_OK(zone->stage(segmentOf("two", "other")));
    CHECK_OK(zone->play());
    const uint64_t base = std::to_underlying(zone->sources().back().timelineStartNs);

    zone->pump();
    CHECK_EQ(rig.starts->of(RoomCommandKind::Renegotiate, "livingroom").size(), size_t{1});

    // Held at the boundary until its device answers: nothing of the new
    // source goes out in the old format.
    const uint64_t before = output->packets();
    zone->onStreamReady(living, true, formatOf(48000));
    CHECK(waitFor([&] { return output->packets() > before; }));

    std::string payloads;
    const std::vector<matp::AudioPacketHeader> headers = decodeHeaders(*sink, payloads);
    CHECK(headers.size() > before);
    CHECK_EQ(headers[before].timelineStartNs, base);
    CHECK_EQ(headers[before].sequenceNumber, headers[before - 1].sequenceNumber + 1);
    CHECK_EQ(describeFormat(headers[before].format), describeFormat(formatOf(48000)));
    CHECK(zone->state() != ZoneState::Failed);
}

// ---------------------------------------------------------------------------
// Phase 24: what a zone announces instead of being polled, and exactly when.
// The pipeline is pumped by hand and the clock moved by hand, so "at the
// audible instant rather than at the decode" is a statement about two numbers
// a test sets, not about a race it hopes to win.

namespace {

// Every event the zone handed to its sink, in the order it handed them.
struct EventLog
{
    mutable std::mutex mutex;
    std::vector<ZoneEvent> seen;

    std::vector<ZoneEvent> all() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return seen;
    }

    std::vector<ZoneEvent> of(ZoneEventKind kind) const
    {
        std::vector<ZoneEvent> matching;
        for (const ZoneEvent& event : all())
        {
            if (event.kind == kind)
                matching.push_back(event);
        }
        return matching;
    }

    std::vector<ZoneEventKind> kinds() const
    {
        std::vector<ZoneEventKind> out;
        for (const ZoneEvent& event : all())
            out.push_back(event.kind);
        return out;
    }
};

ZoneSettings listenedTo(const ZoneRig& rig, const std::shared_ptr<EventLog>& log)
{
    ZoneSettings settings = rig.settings();
    settings.onEvent = [log](const ZoneEvent& event) {
        std::lock_guard<std::mutex> lock(log->mutex);
        log->seen.push_back(event);
    };
    return settings;
}

uint64_t sourceNs()
{
    const auto length = nanosecondsFromFrames(kSourceFrames, kRate);
    CHECK_OK(length);
    return *length;
}

} // namespace

MULTIROOM_TEST(a_zone_asks_for_its_next_source_at_the_open_and_names_it_when_it_is_heard)
{
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto log = std::make_shared<EventLog>();
    auto zone = rig.zone(listenedTo(rig, log));

    // Opened with nothing behind it: asked at once, with the whole source
    // still to answer in, because nothing of it has been decoded yet.
    CHECK_OK(zone->stage(trackOf("one", "track-A", "First Song")));
    CHECK_OK(zone->play());
    std::vector<ZoneEvent> needed = log->of(ZoneEventKind::SourceNeeded);
    CHECK_EQ(needed.size(), size_t{1});
    CHECK_EQ(needed.back().sourceId, std::string("one"));
    CHECK_EQ(needed.back().zone.str(), std::string("living"));
    CHECK(needed.back().remainingNs.has_value());
    CHECK_EQ(needed.back().remainingNs.value_or(0), sourceNs());

    // An answer taken back is the question asked again; clearing an empty
    // list takes nothing back and asks nothing.
    CHECK_OK(zone->stage(segmentOf("wrong", "ramp")));
    CHECK_EQ(zone->clearStaged(), size_t{1});
    CHECK_EQ(zone->clearStaged(), size_t{0});
    needed = log->of(ZoneEventKind::SourceNeeded);
    CHECK_EQ(needed.size(), size_t{2});
    CHECK_EQ(needed.back().sourceId, std::string("one"));

    // The answer. It is consumed when the first source has been decoded, and
    // that open is what asks again -- once, for the source after it.
    CHECK_OK(zone->stage(trackOf("two", "track-B", "Second Song")));
    const ClientId room("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), room, offering({formatOf()}), formatOf()));
    zone->pump();
    CHECK_EQ(zone->sources().size(), size_t{2});
    CHECK(zone->state() == ZoneState::Ended); // decoded out, and nothing staged
    needed = log->of(ZoneEventKind::SourceNeeded);
    CHECK_EQ(needed.size(), size_t{3});
    CHECK_EQ(needed.back().sourceId, std::string("two"));

    // Both sources are decoded, and neither has been heard: the line has not
    // started, so nothing is "now playing" and nothing has played out.
    CHECK(log->of(ZoneEventKind::SourceStarted).empty());
    CHECK(log->of(ZoneEventKind::Ended).empty());

    reportBuffer(*zone, room, 0, 1500 * 1000000ULL);
    zone->pump();
    CHECK(zone->started());
    const uint64_t startedAt = zone->startServerTimeNs();
    const std::vector<ZoneEvent> transport = log->of(ZoneEventKind::Transport);
    CHECK_EQ(transport.size(), size_t{1});
    CHECK(transport.back().transport == TransportState::Playing);
    CHECK_EQ(std::to_underlying(transport.back().timelineNs), uint64_t{0});

    // A start names an instant a start delay ahead, and the first source is
    // heard then -- not when the START_AT went out.
    zone->pump();
    CHECK(log->of(ZoneEventKind::SourceStarted).empty());
    rig.clock.setNs(startedAt);
    zone->pump();
    acknowledge(*zone, room, 0);
    std::vector<ZoneEvent> started = log->of(ZoneEventKind::SourceStarted);
    CHECK_EQ(started.size(), size_t{1});
    CHECK_EQ(started.back().sourceId, std::string("one"));
    CHECK_EQ(started.back().trackId, std::string("track-A"));
    CHECK_EQ(std::to_underlying(started.back().timelineNs), uint64_t{0});
    CHECK_EQ(started.back().serverTimeNs, startedAt);

    // The second source was decoded seconds ago; it is announced the instant
    // the line reaches its first frame, which is what TRACK_BOUNDARY names.
    rig.clock.setNs(startedAt + sourceNs() - 1);
    zone->pump();
    CHECK_EQ(log->of(ZoneEventKind::SourceStarted).size(), size_t{1});
    rig.clock.setNs(startedAt + sourceNs());
    zone->pump();
    started = log->of(ZoneEventKind::SourceStarted);
    CHECK_EQ(started.size(), size_t{2});
    CHECK_EQ(started.back().sourceId, std::string("two"));
    CHECK_EQ(started.back().trackId, std::string("track-B"));
    CHECK_EQ(std::to_underlying(started.back().timelineNs), sourceNs());
    CHECK_EQ(started.back().serverTimeNs, startedAt + sourceNs());
    const std::vector<RoomCommand> boundaries = rig.starts->of(RoomCommandKind::TrackBoundary);
    CHECK(!boundaries.empty());
    CHECK_EQ(boundaries.back().boundary.timelineNs, sourceNs());

    // Ended when it has been played out, not when it was decoded out.
    rig.clock.setNs(startedAt + 2 * sourceNs() - 1);
    zone->pump();
    CHECK(log->of(ZoneEventKind::Ended).empty());
    rig.clock.setNs(startedAt + 2 * sourceNs());
    zone->pump();
    zone->pump();
    const std::vector<ZoneEvent> ended = log->of(ZoneEventKind::Ended);
    CHECK_EQ(ended.size(), size_t{1});
    CHECK_EQ(ended.back().sourceId, std::string("two"));

    // Each of them once, and nothing else said.
    CHECK(log->kinds() ==
          std::vector<ZoneEventKind>({ZoneEventKind::SourceNeeded, ZoneEventKind::SourceNeeded,
                                      ZoneEventKind::SourceNeeded, ZoneEventKind::Transport,
                                      ZoneEventKind::SourceStarted, ZoneEventKind::SourceStarted,
                                      ZoneEventKind::Ended}));
}

MULTIROOM_TEST(a_zone_announces_every_change_of_its_transport_once)
{
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto log = std::make_shared<EventLog>();
    auto zone = rig.zone(listenedTo(rig, log));

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());
    const ClientId room("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), room, offering({formatOf()}), formatOf()));
    reportBuffer(*zone, room, 0, 1500 * 1000000ULL);
    zone->pump();
    rig.clock.setNs(zone->startServerTimeNs());
    acknowledge(*zone, room, 0);
    zone->pump();

    const auto paused = zone->pausePlayback();
    CHECK_OK(paused);
    CHECK_OK(zone->resumePlayback());
    reportBuffer(*zone, room, std::to_underlying(*paused), 1500 * 1000000ULL);
    zone->pump();
    zone->pump();
    const auto stopped = zone->stopPlayback();
    CHECK_OK(stopped);

    // Starting is the resume's barrier forming at the paused position, and a
    // stop names where it left the line, which the line itself forgets.
    const std::vector<ZoneEvent> transport = log->of(ZoneEventKind::Transport);
    CHECK_EQ(transport.size(), size_t{5});
    const std::vector<TransportState> states{TransportState::Playing, TransportState::Paused,
                                             TransportState::Starting, TransportState::Playing,
                                             TransportState::Stopped};
    for (size_t i = 0; i < std::min(transport.size(), states.size()); ++i)
        CHECK(transport[i].transport == states[i]);
    if (transport.size() == states.size())
    {
        CHECK_EQ(std::to_underlying(transport[1].timelineNs), std::to_underlying(*paused));
        CHECK_EQ(std::to_underlying(transport[2].timelineNs), std::to_underlying(*paused));
        CHECK_EQ(std::to_underlying(transport[3].timelineNs), std::to_underlying(*paused));
        CHECK_EQ(std::to_underlying(transport[4].timelineNs), std::to_underlying(*stopped));
    }
}

MULTIROOM_TEST(a_zone_announces_a_skipped_source_and_the_rooms_it_moved_or_gave_up_on)
{
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto log = std::make_shared<EventLog>();
    auto zone = rig.zone(listenedTo(rig, log));

    // Phase 22's skip, said as it happens rather than found in status.
    CHECK_OK(zone->stage(segmentOf("gone", "missing")));
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());
    const std::vector<ZoneEvent> failed = log->of(ZoneEventKind::SourceFailed);
    CHECK_EQ(failed.size(), size_t{1});
    CHECK_EQ(failed.back().sourceId, std::string("gone"));
    CHECK(failed.back().reason.find("NotFound") != std::string::npos);
    CHECK(log->kinds() == std::vector<ZoneEventKind>(
                              {ZoneEventKind::SourceFailed, ZoneEventKind::SourceNeeded}));

    const ClientId moved("kitchen");
    const ClientId deaf("hall");
    CHECK_OK(zone->admit(sessionOf(1), moved, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), deaf, offering({formatOf()}), formatOf()));
    reportBuffer(*zone, moved, 0, 30ULL * kNanosecondsPerSecond);
    reportBuffer(*zone, deaf, 0, 30ULL * kNanosecondsPerSecond);
    zone->pump();
    CHECK(zone->started());
    rig.clock.setNs(zone->startServerTimeNs());
    acknowledge(*zone, moved, 0);

    // A move, with the trigger that made it and the position it went to.
    CHECK_OK(zone->rejoin(moved, RejoinTrigger::HardResync));
    const std::vector<ZoneEvent> behind = log->of(ZoneEventKind::RoomFellBehind);
    CHECK_EQ(behind.size(), size_t{1});
    CHECK_EQ(behind.back().client.str(), std::string("kitchen"));
    CHECK(behind.back().trigger == RejoinTrigger::HardResync);
    CHECK(std::to_underlying(behind.back().timelineNs) > 0);

    // A room that plays none of its starts is given up on after the third,
    // once, with the scheduler's own reason.
    for (int pass = 0; pass < 8 && zone->roomStart(deaf) != RoomStartState::LeftBehind; ++pass)
    {
        rig.clock.advanceMs(2500);
        zone->pump();
    }
    CHECK(zone->roomStart(deaf) == RoomStartState::LeftBehind);
    zone->pump();
    const std::vector<ZoneEvent> given = log->of(ZoneEventKind::RoomLeftBehind);
    CHECK_EQ(given.size(), size_t{1});
    if (!given.empty())
    {
        CHECK_EQ(given.back().client.str(), std::string("hall"));
        CHECK_EQ(given.back().reason, zone->roomStartDetail(deaf));
    }
}

MULTIROOM_TEST(a_zone_hands_its_events_on_without_holding_its_own_locks)
{
    // A sink that calls straight back into the zone would deadlock on a zone
    // that emitted under its lock -- which is the rule, stated as a test.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    Zone* self = nullptr;
    size_t reentered = 0;
    ZoneSettings settings = rig.settings();
    settings.onEvent = [&](const ZoneEvent&) {
        if (self == nullptr)
            return;
        static_cast<void>(self->state());
        static_cast<void>(self->playheadNs());
        static_cast<void>(self->staged());
        ++reentered;
    };
    auto zone = rig.zone(settings);
    self = zone.get();

    CHECK_OK(zone->stage(segmentOf("gone", "missing")));
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());
    const ClientId room("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), room, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->stage(segmentOf("two", "ramp")));
    CHECK_EQ(zone->clearStaged(), size_t{1});
    reportBuffer(*zone, room, 0, 1500 * 1000000ULL);
    zone->pump();
    rig.clock.setNs(zone->startServerTimeNs());
    zone->pump();
    CHECK_OK(zone->rejoin(room, RejoinTrigger::HardResync));
    CHECK_OK(zone->pausePlayback());
    CHECK_OK(zone->resumePlayback());
    CHECK_OK(zone->stopPlayback());

    // sourceFailed, sourceNeeded twice, the transport four times, one source
    // heard, one move -- every kind that has a lock near it.
    CHECK_EQ(reentered, size_t{9});
}

// --- phase 26: instant seek (FLUSH) -------------------------------------------

namespace {

constexpr uint64_t kMs = 1000000ULL;

// One room on a running line from timeline zero, acknowledged, and a second of
// that line audible.
std::shared_ptr<SinkState> startLine(ZoneRig& rig, Zone& zone, const ClientId& room,
                                     const SessionId& session)
{
    CHECK_OK(zone.admit(session, room, offering({formatOf()}), formatOf()));
    auto sink = connect(zone, session, room);
    reportBuffer(zone, room, 0, 1500 * kMs);
    zone.pump();
    CHECK(zone.started());
    acknowledge(zone, room, 0);
    rig.clock.advanceMs(2000);
    return sink;
}

// What one room was sent around a flush: every header, every payload byte, and
// where the first packet carrying `afterFlush` is -- or `headers.size()` when
// there is none yet.
struct Flushed
{
    std::vector<matp::AudioPacketHeader> headers;
    std::string payloads;
    size_t at = 0;
    size_t flagged = 0;
    size_t bytesBefore = 0; // payload bytes of every packet before the flagged one
};

Flushed flushedIn(SinkState& sink)
{
    Flushed out;
    out.headers = decodeHeaders(sink, out.payloads);
    out.at = out.headers.size();
    for (size_t index = 0; index < out.headers.size(); ++index)
    {
        if ((out.headers[index].flags & matp::kAudioFlagAfterFlush) == 0)
            continue;
        if (out.flagged++ == 0)
            out.at = index;
    }
    for (size_t index = 0; index < out.at; ++index)
        out.bytesBefore += out.headers[index].payloadBytes;
    return out;
}

// The ramp's samples of `frame`, as a bitperfect room's payload holds them.
bool rampAt(const std::string& payloads, size_t offset, uint64_t frame)
{
    std::vector<std::byte> expected(formatOf().bytesPerFrame());
    writeRamp(expected, frame, 1, kChannels);
    return payloads.size() >= offset + expected.size() &&
           std::memcmp(payloads.data() + offset, expected.data(), expected.size()) == 0;
}

} // namespace

MULTIROOM_TEST(a_seek_flushes_every_room_on_the_line_and_moves_it_without_a_start)
{
    // Phase 26's "done when": a playing two-room zone, one room bitperfect and
    // one resampled, one FLUSH each and no START_AT, and on each connection a
    // next packet that carries afterFlush, starts at the resume position and
    // is numbered one past the last.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto zone = rig.zone();
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const AudioFormat resampled = formatOf(48000, kChannels, AudioFormat::SampleType::Int32);
    const ClientId exact("livingroom");
    const ClientId converted("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), exact, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), converted, offering({resampled}), resampled));
    CHECK(zone->member(exact)->plan().mode == OutputMode::BitPerfect);
    CHECK(zone->member(converted)->plan().mode == OutputMode::Resampled);
    auto exactSink = connect(*zone, sessionOf(1), exact);
    auto convertedSink = connect(*zone, sessionOf(2), converted);

    reportBuffer(*zone, exact, 0, 1500 * kMs);
    reportBuffer(*zone, converted, 0, 1500 * kMs);
    zone->pump();
    CHECK(zone->started());
    acknowledge(*zone, exact, 0);
    acknowledge(*zone, converted, 0);
    CHECK(waitFor([&] { return zone->member(exact)->packets() >= kSourcePackets; }));

    rig.clock.advanceMs(2000);
    CHECK_EQ(std::to_underlying(zone->playheadNs()), 1000 * kMs);
    const uint64_t anchor = std::to_underlying(zone->startTimelineNs());
    const uint64_t anchoredAt = zone->startServerTimeNs();

    const Result<SeekPlacement, ZoneError> placed = zone->seek(500 * kMs);
    CHECK_OK(placed);
    CHECK_EQ(placed->sourceId, std::string("one"));
    CHECK_EQ(std::to_underlying(placed->cutAt), 1100 * kMs);
    CHECK_EQ(std::to_underlying(placed->resumeAt), 1250 * kMs);
    CHECK_EQ(rig.factory.counters.lastStartFrame.load(), uint64_t{22050});

    // One FLUSH per room, at the cut and the instant the line reaches it, and
    // not a single START_AT more: the line did not stop.
    const std::vector<RoomCommand> flushes = rig.starts->of(RoomCommandKind::Flush);
    CHECK_EQ(flushes.size(), size_t{2});
    for (const RoomCommand& flush : flushes)
    {
        CHECK_EQ(flush.at.timelineNs, 1100 * kMs);
        CHECK_EQ(flush.at.serverTimeNs, anchoredAt + (1100 * kMs - anchor));
    }
    const std::vector<RoomCommand> toExact = rig.starts->of(RoomCommandKind::Flush, "livingroom");
    CHECK_EQ(toExact.size(), size_t{1});
    CHECK_EQ(toExact.front().at.sessionId, sessionOf(1).toHex());
    CHECK_EQ(rig.starts->count(RoomCommandKind::Start), size_t{2});
    CHECK(zone->roomStart(exact) == RoomStartState::Playing);
    CHECK(zone->roomStart(converted) == RoomStartState::Playing);
    CHECK(zone->transport() == TransportState::Playing);

    // The seeked source is a new segment heard from the resume position, and
    // keeps its id.
    const std::vector<StagedSource> sources = zone->sources();
    CHECK_EQ(sources.size(), size_t{2});
    CHECK(sources.front().ended);
    CHECK_EQ(sources.back().segment.sourceId, std::string("one"));
    CHECK_EQ(sources.back().segment.startOffsetNs, 500 * kMs);
    CHECK_EQ(std::to_underlying(sources.back().timelineStartNs), 1250 * kMs);

    zone->pump();
    CHECK(waitFor([&] { return flushedIn(*exactSink).flagged > 0; }));
    CHECK(waitFor([&] { return flushedIn(*convertedSink).flagged > 0; }));

    for (SinkState* sink : {exactSink.get(), convertedSink.get()})
    {
        const Flushed seen = flushedIn(*sink);
        CHECK_EQ(seen.flagged, size_t{1});
        CHECK(seen.at > 0);
        CHECK_EQ(seen.headers[seen.at].timelineStartNs, 1250 * kMs);
        CHECK_EQ(seen.headers[seen.at].sequenceNumber,
                 seen.headers[seen.at - 1].sequenceNumber + 1);
    }

    // Bitperfect is untouched: the first sample after the resume position is
    // the source's own sample at the offset.
    const Flushed bitperfect = flushedIn(*exactSink);
    CHECK(rampAt(bitperfect.payloads, bitperfect.bytesBefore, 22050));
}

MULTIROOM_TEST(a_rooms_reports_between_the_cut_and_the_seeked_audio_feed_nothing)
{
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto zone = rig.zone();
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());
    const ClientId room("livingroom");
    auto sink = startLine(rig, *zone, room, sessionOf(1));

    const Result<SeekPlacement, ZoneError> placed = zone->seek(500 * kMs);
    CHECK_OK(placed);
    const uint64_t heardAt = zone->startServerTimeNs() +
                             (std::to_underlying(placed->resumeAt) -
                              std::to_underlying(zone->startTimelineNs()));

    // The room is playing silence at the cut and reports the cut as audible:
    // a position that is not playing, which must not become an offset.
    const ClientClockModel model = fittedModel();
    const auto report = [&](uint64_t serverNs, uint64_t audibleNs) {
        matp::PlaybackReport out;
        out.clockQuality = AudioClockQuality::Hardware;
        out.localMonotonicTimeNs = serverNs + 410ULL * kNanosecondsPerSecond;
        out.deviceFramePosition = (serverNs / kMs) * (kRate / 1000);
        out.hasTimelineNsAudible = true;
        out.timelineNsAudible = audibleNs;
        zone->onPlaybackReport(room, out, model);
    };

    const uint64_t before = zone->syncStatus(room).reports;
    report(heardAt - 50 * kMs, std::to_underlying(placed->cutAt));
    CHECK_EQ(zone->syncStatus(room).reports, before);

    // Once the seeked audio is heard, the room is on the line it never left.
    report(heardAt + 10 * kMs, std::to_underlying(placed->resumeAt) + 10 * kMs);
    CHECK_EQ(zone->syncStatus(room).reports, before + 1);
}

MULTIROOM_TEST(a_seek_that_cannot_be_made_is_answered_and_the_line_plays_on)
{
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto zone = rig.zone();
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    // No line yet: a source that has not started takes its offset where it is
    // staged instead.
    CHECK_ERR(zone->seek(500 * kMs), ZoneError::NotStarted);

    const ClientId room("livingroom");
    auto sink = startLine(rig, *zone, room, sessionOf(1));
    const uint64_t opens = rig.factory.counters.opens.load();

    // Not a position in the source: three seconds of it end at frame 132300.
    CHECK_ERR(zone->seek(3000 * kMs), ZoneError::InvalidPosition);

    // A source that will not open again. The zone plays on as though nothing
    // had been asked, because nothing was cut before the open.
    rig.factory.counters.refuseOpens.store(true);
    CHECK_ERR(zone->seek(500 * kMs), ZoneError::SourceFailed);
    rig.factory.counters.refuseOpens.store(false);
    CHECK_EQ(rig.factory.counters.opens.load(), opens + 2);

    CHECK_EQ(rig.starts->count(RoomCommandKind::Flush), size_t{0});
    CHECK_EQ(zone->sources().size(), size_t{1});
    CHECK(zone->roomStart(room) == RoomStartState::Playing);
    CHECK(zone->member(room)->state() == OutputState::Streaming);
    CHECK(waitFor([&] { return zone->member(room)->packets() >= kSourcePackets; }));
    CHECK_EQ(flushedIn(*sink).flagged, size_t{0});
    size_t packets = 0;
    CHECK_EQ(jumpsIn(*sink, packets), size_t{0});
}

MULTIROOM_TEST(a_source_that_cannot_seek_is_refused_by_name)
{
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto zone = rig.zone();
    CHECK_OK(zone->stage(segmentOf("radio", "live")));
    CHECK_OK(zone->play());
    auto sink = startLine(rig, *zone, ClientId("livingroom"), sessionOf(1));

    CHECK_ERR(zone->seek(500 * kMs), ZoneError::NotSeekable);
    CHECK_EQ(rig.starts->count(RoomCommandKind::Flush), size_t{0});
}

MULTIROOM_TEST(a_seek_is_refused_once_the_next_source_is_being_decoded)
{
    // The whole of both sources is decoded on the first pump here, so the
    // next source is open and its boundary announced: the line past "one" is
    // already "two"'s.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto zone = rig.zone();
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->stage(segmentOf("two", "ramp")));
    CHECK_OK(zone->play());
    auto sink = startLine(rig, *zone, ClientId("livingroom"), sessionOf(1));
    CHECK_EQ(zone->sources().size(), size_t{2});

    CHECK_ERR(zone->seek(500 * kMs), ZoneError::NotSeekable);
    CHECK_EQ(rig.starts->count(RoomCommandKind::Flush), size_t{0});
}

MULTIROOM_TEST(a_seek_on_a_paused_zone_resumes_at_the_new_position)
{
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto zone = rig.zone();
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());
    const ClientId room("livingroom");
    auto sink = startLine(rig, *zone, room, sessionOf(1));
    CHECK(waitFor([&] { return zone->member(room)->packets() >= kSourcePackets; }));

    const Result<TimelineNs, ZoneError> paused = zone->pausePlayback();
    CHECK_OK(paused);
    const uint64_t at = std::to_underlying(*paused);

    // On a paused line the cut and the resume position are where it stands.
    const Result<SeekPlacement, ZoneError> placed = zone->seek(2000 * kMs);
    CHECK_OK(placed);
    CHECK_EQ(std::to_underlying(placed->cutAt), at);
    CHECK_EQ(std::to_underlying(placed->resumeAt), at);
    const std::vector<RoomCommand> flushes = rig.starts->of(RoomCommandKind::Flush);
    CHECK_EQ(flushes.size(), size_t{1});
    CHECK_EQ(flushes.front().at.timelineNs, at);
    CHECK(zone->transport() == TransportState::Paused);

    zone->pump();
    CHECK(waitFor([&] { return flushedIn(*sink).flagged > 0; }));
    const Flushed seen = flushedIn(*sink);
    CHECK_EQ(seen.headers[seen.at].timelineStartNs, at);
    CHECK(rampAt(seen.payloads, seen.bytesBefore, 88200));

    // The ordinary resume, at the position that now holds the seeked audio.
    reportBuffer(*zone, room, at, 1500 * kMs);
    CHECK_OK(zone->resumePlayback());
    zone->pump();
    const std::vector<RoomCommand> starts = rig.starts->of(RoomCommandKind::Start);
    CHECK_EQ(starts.size(), size_t{2});
    CHECK_EQ(starts.back().start.timelineNs, at);
}

MULTIROOM_TEST(a_source_staged_behind_a_seek_follows_it_gaplessly)
{
    // Thirty seconds of source against ten of lookahead, so "two" is still
    // staged when "one" is seeked, and follows the seeked audio at exactly
    // the timeline position where its remaining five seconds end.
    ZoneRig rig(formatOf(), kRate * 30);
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto zone = rig.zone();
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->stage(segmentOf("two", "ramp")));
    CHECK_OK(zone->play());
    const ClientId room("livingroom");
    auto sink = startLine(rig, *zone, room, sessionOf(1));
    CHECK_EQ(zone->stagedCount(), size_t{1});

    const Result<SeekPlacement, ZoneError> placed = zone->seek(25000 * kMs);
    CHECK_OK(placed);
    CHECK_EQ(zone->stagedCount(), size_t{1});
    const uint64_t resume = std::to_underlying(placed->resumeAt);

    // Decoding the last five seconds of "one" opens "two" at their end.
    for (int pass = 0; pass < 100 && zone->sources().size() < 3; ++pass)
    {
        CHECK(waitFor([&] {
            return std::to_underlying(zone->member(room)->producedThroughNs()) + kMs >=
                   std::to_underlying(zone->frontierNs());
        }));
        zone->pump();
    }
    const std::vector<StagedSource> sources = zone->sources();
    CHECK_EQ(sources.size(), size_t{3});
    CHECK_EQ(sources[1].frames, uint64_t{kRate * 5});
    CHECK_EQ(sources[2].segment.sourceId, std::string("two"));
    CHECK_EQ(std::to_underlying(sources[2].timelineStartNs), resume + 5000 * kMs);

    // One jump on the wire -- the announced one -- and none at the boundary.
    CHECK(waitFor([&] {
        const Flushed received = flushedIn(*sink);
        return received.flagged > 0 && !received.headers.empty() &&
               received.headers.back().timelineStartNs > resume + 5100 * kMs;
    }));
    const Flushed seen = flushedIn(*sink);
    CHECK_EQ(seen.flagged, size_t{1});
    uint64_t expected = seen.headers[seen.at].timelineStartNs;
    for (size_t index = seen.at; index < seen.headers.size(); ++index)
    {
        CHECK_EQ(seen.headers[index].timelineStartNs, expected);
        expected += seen.headers[index].timelineDurationNs;
    }
}

// --- phase 27: a room's own output latency -------------------------------------

MULTIROOM_TEST(a_change_of_output_latency_on_a_playing_room_is_one_rejoin)
{
    // A bitperfect room cannot slew and tens of milliseconds is a step, so a
    // room already on the line is moved once and started again with the new
    // figure; one that is not on the line simply takes it at its start.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    settings.lateJoinLeadNs = 2ULL * kNanosecondsPerSecond;
    settings.start.startDelayNs = kNanosecondsPerSecond;
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId still("livingroom");
    const ClientId moved("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), still, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), moved, offering({formatOf()}), formatOf()));
    auto stillSink = connect(*zone, sessionOf(1), still);
    auto movedSink = connect(*zone, sessionOf(2), moved);

    reportBuffer(*zone, still, 0, 1500 * 1000000ULL);
    reportBuffer(*zone, moved, 0, 1500 * 1000000ULL);
    zone->pump();
    CHECK(zone->started());
    acknowledge(*zone, still, std::to_underlying(zone->startTimelineNs()));
    acknowledge(*zone, moved, std::to_underlying(zone->startTimelineNs()));
    CHECK(waitFor([&] { return zone->member(moved)->packets() > 0; }));

    rig.clock.advanceMs(1000);
    const uint64_t playhead = std::to_underlying(zone->playheadNs());

    constexpr uint64_t kLatencyNs = 60ULL * 1000000ULL;
    const Result<bool, ZoneError> set = zone->setOutputLatency(moved, kLatencyNs);
    CHECK_OK(set);
    CHECK(*set);
    CHECK_EQ(zone->rejoins(moved), 1ULL);
    CHECK_EQ(zone->rejoins(still), 0ULL);
    CHECK(zone->roomStart(moved) == RoomStartState::Waiting);
    CHECK_EQ(zone->syncStatus(moved).targetOffsetNs, static_cast<int64_t>(kLatencyNs));

    // The same figure again is no change and no move.
    const Result<bool, ZoneError> again = zone->setOutputLatency(moved, kLatencyNs);
    CHECK_OK(again);
    CHECK(!*again);
    CHECK_EQ(zone->rejoins(moved), 1ULL);

    // Placed again with the new start: the line's instant for its position,
    // less its latency.
    const uint64_t origin = playhead + 2ULL * kNanosecondsPerSecond;
    reportBuffer(*zone, moved, origin, 1500 * 1000000ULL);
    zone->pump();
    const std::vector<RoomCommand> starts = rig.starts->of(RoomCommandKind::Start, "kitchen");
    CHECK_EQ(starts.size(), size_t{2});
    CHECK_EQ(starts.back().start.timelineNs, origin);
    CHECK_EQ(starts.back().start.serverTimeNs,
             zone->startServerTimeNs() + (origin - std::to_underlying(zone->startTimelineNs())) -
                 kLatencyNs);

    // The other room heard nothing of it.
    CHECK_EQ(rig.starts->of(RoomCommandKind::Start, "livingroom").size(), size_t{1});
    CHECK(zone->roomStart(still) == RoomStartState::Playing);
    size_t packets = 0;
    CHECK_EQ(jumpsIn(*stillSink, packets), size_t{0});
    CHECK(waitFor([&] { return jumpsIn(*movedSink, packets) == 1; }));

    // A room that is not on the line yet is not moved: it is started with the
    // figure when it is started at all.
    const ClientId late("bedroom");
    CHECK_OK(zone->admit(sessionOf(3), late, offering({formatOf()}), formatOf()));
    const Result<bool, ZoneError> waiting = zone->setOutputLatency(late, 40ULL * 1000000ULL);
    CHECK_OK(waiting);
    CHECK(!*waiting);
    CHECK_EQ(zone->rejoins(late), 0ULL);

    reportBuffer(*zone, late, origin, 1500 * 1000000ULL);
    zone->pump();
    const std::optional<matp::StartAt> toLate = rig.starts->forRoom("bedroom");
    CHECK(toLate.has_value());
    CHECK_EQ(toLate->serverTimeNs,
             zone->startServerTimeNs() +
                 (toLate->timelineNs - std::to_underlying(zone->startTimelineNs())) -
                 40ULL * 1000000ULL);

    CHECK_ERR(zone->setOutputLatency(ClientId("elsewhere"), kLatencyNs), ZoneError::NotMember);
}

MULTIROOM_TEST(a_room_with_an_output_latency_is_held_at_its_target_and_not_resynced_for_it)
{
    // The room was started 80 ms ahead of the line and reports being there.
    // A controller aiming at zero would call that a step no ratio closes and
    // move the room straight back; aiming at its target, it asks for nothing.
    ZoneRig rig;
    rig.clock.setNs(90ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    settings.start.startDelayNs = kNanosecondsPerSecond;
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const AudioFormat resampled = formatOf(48000, kChannels, AudioFormat::SampleType::Int32);
    const ClientId plain("livingroom");
    const ClientId receiver("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), plain, offering({resampled}), resampled));
    CHECK_OK(zone->admit(sessionOf(2), receiver, offering({resampled}), resampled));
    CHECK(zone->member(receiver)->driftCorrected());
    CHECK_OK(zone->setOutputLatency(receiver, 80ULL * 1000000ULL));

    reportBuffer(*zone, plain, 0, 1500 * 1000000ULL);
    reportBuffer(*zone, receiver, 0, 1500 * 1000000ULL);
    zone->pump();
    CHECK(zone->started());

    // One position, 80 ms apart in server time.
    const std::optional<matp::StartAt> toPlain = rig.starts->forRoom("livingroom");
    const std::optional<matp::StartAt> toReceiver = rig.starts->forRoom("kitchen");
    CHECK(toPlain.has_value() && toReceiver.has_value());
    CHECK_EQ(toPlain->timelineNs, toReceiver->timelineNs);
    CHECK_EQ(toPlain->serverTimeNs - toReceiver->serverTimeNs, 80ULL * 1000000ULL);

    const ClientClockModel model = fittedModel();
    driveOffset(*zone, {receiver}, model, 48000, 80LL * 1000000LL, 12);

    const ClientSyncStatus status = zone->syncStatus(receiver);
    CHECK(status.hasOffset);
    CHECK_NEAR(static_cast<double>(status.offsetNs), 80e6, 2e6);
    CHECK_EQ(status.targetOffsetNs, 80LL * 1000000LL);
    CHECK(!status.resyncWanted);
    CHECK_EQ(zone->rejoins(receiver), 0ULL);
    CHECK(zone->roomStart(receiver) != RoomStartState::Waiting);
}

MULTIROOM_TEST(a_bitperfect_room_with_an_output_latency_is_sent_the_sources_own_bytes)
{
    // A latency moves when a room plays and never what it plays.
    ZoneRig rig;
    auto zone = rig.zone();

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId room("livingroom");
    const SessionId session = sessionOf(1);
    CHECK_OK(zone->admit(session, room, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->setOutputLatency(room, 120ULL * 1000000ULL));

    const std::shared_ptr<ClientOutput> output = zone->member(room);
    CHECK(output->plan().mode == OutputMode::BitPerfect);
    auto sink = connect(*zone, session, room);

    decodeEverything(*zone);
    CHECK(waitFor([&] { return output->packets() >= kSourcePackets; }));

    std::string payloads;
    const std::vector<matp::AudioPacketHeader> headers = decodeHeaders(*sink, payloads);
    CHECK_EQ(headers.size(), static_cast<size_t>(kSourcePackets));

    std::vector<std::byte> expected(static_cast<size_t>(kSourceFrames) *
                                    formatOf().bytesPerFrame());
    writeRamp(expected, 0, static_cast<uint32_t>(kSourceFrames), kChannels);
    CHECK_EQ(payloads.size(), expected.size());
    CHECK_EQ(std::memcmp(payloads.data(), expected.data(), expected.size()), 0);
}

MULTIROOM_TEST(a_latency_at_the_ceiling_fits_the_defaults_without_a_longer_lead)
{
    // The latency is taken out of the prebuffer, not added to the retention:
    // at section 46's defaults the derived late-join lead is still 1960 ms,
    // and a room given the whole ceiling -- the prebuffer less one packet --
    // is still started from audio it holds, at the barrier and as a late
    // joiner, a whole start delay ahead.
    ZoneRig rig;

    ZoneSettings defaults = rig.settings();
    defaults.output.targetBufferNs = 1500ULL * 1000000ULL;
    defaults.retentionNs = 5ULL * kNanosecondsPerSecond;
    defaults.lookaheadNs = 0;
    defaults.lateJoinLeadNs = 0;
    auto created = Zone::create(ZoneId("living"), rig.memory, *rig.planner, rig.resamplers,
                                rig.factory, rig.clock, defaults);
    CHECK_OK(created);
    Zone& zone = **created;

    constexpr uint64_t kCeilingNs = 1480ULL * 1000000ULL;
    CHECK_OK(zone.stage(segmentOf("one", "ramp")));
    CHECK_OK(zone.play());

    const ClientId first("living");
    CHECK_OK(zone.admit(sessionOf(1), first, offering({formatOf()}), formatOf()));
    CHECK_OK(zone.setOutputLatency(first, kCeilingNs));
    reportBuffer(zone, first, 0, 1500 * 1000000ULL);
    zone.pump();
    CHECK(zone.started());
    const std::optional<matp::StartAt> toFirst = rig.starts->forRoom("living");
    CHECK(toFirst.has_value());
    CHECK_EQ(toFirst->serverTimeNs, rig.clock.monotonicNowNs() + kNanosecondsPerSecond);
    CHECK_EQ(zone.startServerTimeNs(), toFirst->serverTimeNs + kCeilingNs);
    acknowledge(zone, first, std::to_underlying(zone.startTimelineNs()));

    rig.clock.advanceMs(3000);
    const uint64_t playhead = std::to_underlying(zone.playheadNs());

    const ClientId late("kitchen");
    CHECK_OK(zone.admit(sessionOf(2), late, offering({formatOf()}), formatOf()));
    CHECK_OK(zone.setOutputLatency(late, kCeilingNs));
    const uint64_t origin = playhead + 1960ULL * 1000000ULL;
    CHECK_EQ(std::to_underlying(zone.member(late)->producedThroughNs()), origin);

    reportBuffer(zone, late, origin, 1500 * 1000000ULL);
    zone.pump();
    const std::optional<matp::StartAt> toLate = rig.starts->forRoom("kitchen");
    CHECK(toLate.has_value());
    CHECK(zone.roomStart(late) == RoomStartState::Scheduled);
    CHECK_EQ(toLate->timelineNs, playhead + kNanosecondsPerSecond + kCeilingNs);
    CHECK_EQ(toLate->serverTimeNs, rig.clock.monotonicNowNs() + kNanosecondsPerSecond);
}

// --- phase 31: the house's dither --------------------------------------------

namespace {

struct DitheredRooms
{
    std::string narrow; // 48 kHz int16: resampled and narrowed
    std::string wide;   // 48 kHz int24in32: resampled, not narrowed
};

// One zone with the given dither, two resampled rooms of different depths,
// and what each was sent.
DitheredRooms playDithered(DitherMode dither)
{
    ZoneRig rig;
    ZoneSettings settings = rig.settings();
    settings.dither = dither;
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const AudioFormat narrow = formatOf(48000, kChannels, AudioFormat::SampleType::Int16);
    const AudioFormat wide = formatOf(48000, kChannels, AudioFormat::SampleType::Int24In32);
    const ClientId kitchen("kitchen");
    const ClientId study("study");
    CHECK_OK(zone->admit(sessionOf(1), kitchen, offering({narrow}), narrow));
    CHECK_OK(zone->admit(sessionOf(2), study, offering({wide}), wide));

    const std::shared_ptr<ClientOutput> a = zone->member(kitchen);
    const std::shared_ptr<ClientOutput> b = zone->member(study);
    CHECK(a->plan().mode == OutputMode::Resampled);
    CHECK(b->plan().mode == OutputMode::Resampled);

    auto sinkA = connect(*zone, sessionOf(1), kitchen);
    auto sinkB = connect(*zone, sessionOf(2), study);

    decodeEverything(*zone);
    CHECK(waitFor([&] { return a->packets() >= kSourcePackets - 1; }));
    CHECK(waitFor([&] { return b->packets() >= kSourcePackets - 1; }));

    DitheredRooms out;
    decodeHeaders(*sinkA, out.narrow);
    decodeHeaders(*sinkB, out.wide);
    return out;
}

} // namespace

MULTIROOM_TEST(the_house_dither_moves_a_16_bit_room_by_one_step_at_most_and_a_24_bit_room_not_at_all)
{
    const DitheredRooms plain = playDithered(DitherMode::No);
    const DitheredRooms dithered = playDithered(DitherMode::TriangularPdf);

    // The same zone, the same source and the same rooms twice: with dither
    // off the narrow room's samples are rounded to nearest, and with it on
    // each one may land on the next code either side and no further.
    const size_t narrowBytes = std::min(plain.narrow.size(), dithered.narrow.size());
    CHECK(narrowBytes >= (kSourcePackets - 1) * 960u * 4u);
    size_t moved = 0;
    for (size_t at = 0; at + 1 < narrowBytes; at += 2)
    {
        const auto sample = [at](const std::string& bytes) {
            return static_cast<int16_t>(static_cast<uint16_t>(
                static_cast<uint8_t>(bytes[at]) | (static_cast<uint8_t>(bytes[at + 1]) << 8)));
        };
        const int difference = sample(dithered.narrow) - sample(plain.narrow);
        CHECK(difference >= -1 && difference <= 1);
        if (difference != 0)
            ++moved;
    }
    CHECK(moved > 0);

    // The 24-bit room of the same zone is sent the same bytes either way.
    const size_t wideBytes = std::min(plain.wide.size(), dithered.wide.size());
    CHECK(wideBytes >= (kSourcePackets - 1) * 960u * 8u);
    CHECK_EQ(std::memcmp(plain.wide.data(), dithered.wide.data(), wideBytes), 0);
}

// --- phase 39: one room can never evict another ------------------------------

MULTIROOM_TEST(a_room_on_a_slow_link_cannot_pull_the_decode_past_the_room_beside_it)
{
    // The ESP of 2026-10-01, reduced to its two facts: its socket takes every
    // byte, and its buffer report says it holds nothing. Pacing counts only
    // what this process still holds, so without a ceiling that room is built
    // further and further ahead, the decode follows it, and the store evicts
    // audio the room beside it has not played yet. Section 46's defaults, so
    // the budget is the one create() checks against the retention.
    ZoneRig rig(formatOf(), static_cast<uint64_t>(kRate) * 40);
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    settings.output.targetBufferNs = 1500ULL * 1000000ULL;
    settings.output.maxQueuedPackets = 64;
    settings.retentionNs = 5ULL * kNanosecondsPerSecond;
    settings.lookaheadNs = 0;
    settings.lateJoinLeadNs = 0;
    settings.start.readyTimeoutNs = 500ULL * 1000000ULL;
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId healthy("livingroom");
    const ClientId slow("esp");
    CHECK_OK(zone->admit(sessionOf(1), healthy, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), slow, offering({formatOf()}), formatOf()));
    auto healthySink = connect(*zone, sessionOf(1), healthy);
    connect(*zone, sessionOf(2), slow);
    const std::shared_ptr<ClientOutput> steady = zone->member(healthy);
    const std::shared_ptr<ClientOutput> esp = zone->member(slow);

    // The healthy room fills and starts alone; the slow one never holds
    // enough to be started, and the barrier stops waiting for it.
    CHECK(waitFor([&] {
        zone->pump();
        return std::to_underlying(steady->producedThroughNs()) >= 1500ULL * 1000000ULL;
    }));
    reportBuffer(*zone, healthy, 0, 1500ULL * 1000000ULL);
    reportBuffer(*zone, slow, 0, 0);
    rig.clock.advanceMs(500);
    zone->pump();
    CHECK(zone->started());
    acknowledge(*zone, healthy, std::to_underlying(zone->startTimelineNs()));

    constexpr uint64_t kCeilingAheadNs = 1500ULL * 1000000ULL + kPacketNs;
    uint64_t worstAheadNs = 0;
    for (int step = 0; step < 60; ++step)
    {
        rig.clock.advanceMs(500);
        const uint64_t playhead = std::to_underlying(zone->playheadNs());
        const uint64_t built = deliveredThroughNs(*steady, *healthySink);
        reportBuffer(*zone, healthy, playhead, built > playhead ? built - playhead : 0);
        reportBuffer(*zone, slow, playhead, 0);

        CHECK(waitFor([&] {
            zone->pump();
            return std::to_underlying(steady->producedThroughNs()) >=
                   playhead + 1500ULL * 1000000ULL;
        }));
        CHECK(steady->state() == OutputState::Streaming);

        const uint64_t ahead = std::to_underlying(esp->producedThroughNs()) - playhead;
        worstAheadNs = std::max(worstAheadNs, ahead);
        CHECK(ahead <= kCeilingAheadNs);
        CHECK(std::to_underlying(zone->frontierNs()) - playhead < settings.retentionNs);
    }

    // Thirty seconds of line, and nobody was moved: the healthy room was never
    // stranded, and the slow one was fed as far as its ceiling and no further.
    CHECK_EQ(zone->rejoinCount(), 0ULL);
    CHECK(esp->state() == OutputState::Streaming);
    CHECK(worstAheadNs >= kCeilingAheadNs - 2 * kPacketNs);
    CHECK(zone->state() != ZoneState::Failed);
}

// --- phase 40: a rejoin lands on audio the store still holds ------------------

MULTIROOM_TEST(a_room_moved_while_the_store_holds_nothing_at_playhead_plus_lead_is_moved_once)
{
    // A late-join lead of one packet, and a lookahead that leaves the
    // retention a single packet of margin. The decode stops one lookahead past
    // the room furthest ahead, but it stops on a whole block, and a block here
    // is 174 ms: the frontier ends up more than `retention - lead` past the
    // playhead, so the oldest audio held is already past `playhead + lead`.
    // Before phase 40 a room moved there was stranded on its first read and
    // moved again on the next pass, every pass.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    settings.output.targetBufferNs = 200 * kMs;
    settings.retentionNs = 1000 * kMs;
    settings.lookaheadNs = 760 * kMs;
    settings.lateJoinLeadNs = 20 * kMs;
    settings.start.minimumStartBufferNs = 100 * kMs;
    settings.start.startDelayNs = 100 * kMs;
    std::atomic<int> moves{0};
    settings.onEvent = [&](const ZoneEvent& event) {
        if (event.kind == ZoneEventKind::RoomFellBehind)
            moves.fetch_add(1);
    };
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    // The retention leaves the slowest room barely a block behind the
    // furthest, so nothing more is decoded while the rooms' own threads build
    // their prebuffers: a decode racing them would strand whichever started
    // later, which is not the move this test is about. The first lookahead is
    // decoded before anyone joins, and the rooms join on the oldest audio it
    // left.
    zone->pump();
    const uint64_t joinedAt = std::to_underlying(zone->oldestNs());

    const ClientId steady("livingroom");
    const ClientId mover("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), steady, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), mover, offering({formatOf()}), formatOf()));
    auto steadySink = connect(*zone, sessionOf(1), steady);
    auto moverSink = connect(*zone, sessionOf(2), mover);
    const std::shared_ptr<ClientOutput> a = zone->member(steady);
    const std::shared_ptr<ClientOutput> b = zone->member(mover);

    CHECK(waitFor([&] {
        return std::to_underlying(a->producedThroughNs()) >= joinedAt + 200 * kMs &&
               std::to_underlying(b->producedThroughNs()) >= joinedAt + 200 * kMs;
    }));
    reportBuffer(*zone, steady, joinedAt, 200 * kMs);
    reportBuffer(*zone, mover, joinedAt, 200 * kMs);
    zone->pump();
    CHECK(zone->started());
    acknowledge(*zone, steady, std::to_underlying(zone->startTimelineNs()));
    acknowledge(*zone, mover, std::to_underlying(zone->startTimelineNs()));

    // Play on until the frontier has overshot by enough.
    uint64_t playhead = 0;
    for (int step = 0; step < 20; ++step)
    {
        rig.clock.advanceMs(100);
        playhead = std::to_underlying(zone->playheadNs());
        for (const auto& [client, output, sink] :
             {std::tuple{steady, a, steadySink}, std::tuple{mover, b, moverSink}})
        {
            const uint64_t built = deliveredThroughNs(*output, *sink);
            reportBuffer(*zone, client, playhead, built > playhead ? built - playhead : 0);
        }
        CHECK(waitFor([&] {
            zone->pump();
            return std::to_underlying(a->producedThroughNs()) >= playhead + 200 * kMs &&
                   std::to_underlying(b->producedThroughNs()) >= playhead + 200 * kMs;
        }));
        if (std::to_underlying(zone->oldestNs()) > playhead + 40 * kMs)
            break;
    }
    const uint64_t oldest = std::to_underlying(zone->oldestNs());
    CHECK(oldest > playhead + 40 * kMs);

    // Placed one packet into what the store holds, not at `playhead + lead`.
    const Result<TimelineNs, ZoneError> moved = zone->rejoin(mover, RejoinTrigger::HardResync);
    CHECK_OK(moved);
    if (moved)
        CHECK_EQ(std::to_underlying(*moved), oldest + kPacketNs);

    // And it stays there: one move, however many passes follow.
    CHECK(waitFor([&] { return std::to_underlying(b->producedThroughNs()) > oldest + kPacketNs; }));
    for (int pass = 0; pass < 20; ++pass)
    {
        zone->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(b->state() == OutputState::Streaming);
    CHECK_EQ(zone->rejoins(mover), 1ULL);
    CHECK_EQ(moves.load(), 1);
    CHECK_EQ(zone->rejoins(steady), 0ULL);
}

// --- phase 41: a room that keeps falling behind degrades alone ----------------

MULTIROOM_TEST(a_room_that_keeps_falling_behind_backs_off_is_announced_once_and_never_stopped)
{
    // A room whose link cannot keep its buffer ahead of the line: five times
    // in its first ten seconds it holds nothing a start could name, and the
    // line asks for it to be moved. It can no longer hurt the room beside it
    // (phase 39), so what is left is how often it is moved and how much the
    // high-end server hears about it.
    ZoneRig rig(formatOf(), static_cast<uint64_t>(kRate) * 60);
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);

    ZoneSettings settings = rig.settings();
    settings.output.targetBufferNs = 500 * kMs;
    settings.retentionNs = 5ULL * kNanosecondsPerSecond;
    settings.lookaheadNs = 0;
    settings.lateJoinLeadNs = 300 * kMs;
    settings.start.minimumStartBufferNs = 100 * kMs;
    settings.start.readyTimeoutNs = 500 * kMs;

    struct Seen
    {
        std::mutex mutex;
        std::vector<ZoneEvent> events;
    };
    auto seen = std::make_shared<Seen>();
    settings.onEvent = [seen](const ZoneEvent& event) {
        if (event.kind != ZoneEventKind::RoomFellBehind &&
            event.kind != ZoneEventKind::RoomUnstable && event.kind != ZoneEventKind::RoomStable)
            return;
        std::lock_guard<std::mutex> lock(seen->mutex);
        seen->events.push_back(event);
    };
    auto zone = rig.zone(settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId healthy("livingroom");
    const ClientId flaky("esp");
    CHECK_OK(zone->admit(sessionOf(1), healthy, offering({formatOf()}), formatOf()));
    CHECK_OK(zone->admit(sessionOf(2), flaky, offering({formatOf()}), formatOf()));
    auto healthySink = connect(*zone, sessionOf(1), healthy);
    connect(*zone, sessionOf(2), flaky);
    const std::shared_ptr<ClientOutput> steady = zone->member(healthy);
    const std::shared_ptr<ClientOutput> esp = zone->member(flaky);

    CHECK(waitFor([&] {
        zone->pump();
        return std::to_underlying(steady->producedThroughNs()) >= 500 * kMs;
    }));
    reportBuffer(*zone, healthy, 0, 500 * kMs);
    reportBuffer(*zone, flaky, 0, 0);
    rig.clock.advanceMs(500);
    zone->pump();
    CHECK(zone->started());
    acknowledge(*zone, healthy, std::to_underlying(zone->startTimelineNs()));

    const uint64_t beganNs = rig.clock.monotonicNowNs();
    std::vector<uint64_t> movedAtMs;
    uint64_t movesSoFar = 0;
    bool wasUnstable = false;
    for (int step = 0; step < 100; ++step)
    {
        rig.clock.advanceMs(500);
        const uint64_t elapsedMs = (rig.clock.monotonicNowNs() - beganNs) / kMs;
        const uint64_t playhead = std::to_underlying(zone->playheadNs());
        const uint64_t built = deliveredThroughNs(*steady, *healthySink);
        reportBuffer(*zone, healthy, playhead, built > playhead ? built - playhead : 0);

        // Holding 100 ms from `playhead + lead`, a start delay of one second
        // cannot name a position it holds: the line has run past it.
        if (elapsedMs <= 10000)
            reportBuffer(*zone, flaky, playhead + 300 * kMs, 100 * kMs);

        CHECK(waitFor([&] {
            zone->pump();
            return std::to_underlying(steady->producedThroughNs()) >= playhead + 500 * kMs;
        }));
        CHECK(steady->state() == OutputState::Streaming);

        if (const uint64_t moves = zone->rejoins(flaky); moves != movesSoFar)
        {
            CHECK_EQ(moves, movesSoFar + 1);
            movesSoFar = moves;
            movedAtMs.push_back(elapsedMs);
        }
        wasUnstable = wasUnstable || zone->stability(flaky).unstable;
    }

    // Moved at once, then 1 s, 2 s, 4 s and 8 s apart: five moves for five
    // strands, spread over fifteen seconds rather than made back to back.
    CHECK_EQ(movedAtMs.size(), size_t{5});
    if (movedAtMs.size() == 5)
    {
        CHECK_EQ(movedAtMs[1] - movedAtMs[0], 1000ULL);
        CHECK_EQ(movedAtMs[2] - movedAtMs[1], 2000ULL);
        CHECK_EQ(movedAtMs[3] - movedAtMs[2], 4000ULL);
        CHECK_EQ(movedAtMs[4] - movedAtMs[3], 8000ULL);
    }

    // Two moves announced as moves, then one room.unstable standing for the
    // third and everything after it, and room.stable thirty seconds after the
    // last. Nothing about the room beside it.
    std::vector<ZoneEvent> events;
    {
        std::lock_guard<std::mutex> lock(seen->mutex);
        events = seen->events;
    }
    CHECK_EQ(events.size(), size_t{4});
    if (events.size() == 4)
    {
        CHECK(events[0].kind == ZoneEventKind::RoomFellBehind);
        CHECK(events[1].kind == ZoneEventKind::RoomFellBehind);
        CHECK(events[2].kind == ZoneEventKind::RoomUnstable);
        CHECK(events[2].trigger == RejoinTrigger::LeftBehind);
        CHECK_EQ(events[2].rejoins, 3ULL);
        CHECK_EQ(events[2].backoffNs, 4ULL * kNanosecondsPerSecond);
        CHECK(!events[2].reason.empty());
        CHECK(events[3].kind == ZoneEventKind::RoomStable);
        CHECK_EQ(events[3].rejoins, 5ULL);
        CHECK_EQ(events[3].unstableForNs, (movedAtMs.back() - movedAtMs[2] + 30000) * kMs);
    }
    for (const ZoneEvent& event : events)
        CHECK_EQ(event.client.str(), std::string("esp"));

    CHECK(wasUnstable);
    CHECK(!zone->stability(flaky).unstable);
    CHECK_EQ(zone->stability(flaky).backoffNs, 0ULL);

    // It was never stopped, never taken out of the zone and never told to
    // stop: that is the high-end server's decision, or the user's.
    CHECK(esp->state() == OutputState::Streaming);
    CHECK(zone->member(flaky) != nullptr);
    CHECK_EQ(rig.starts->count(RoomCommandKind::Stop), size_t{0});
    CHECK_EQ(zone->rejoins(healthy), 0ULL);
}

// --- phase 42: each zone allocates its own audio memory ----------------------

namespace {

AudioFormat hiRes()
{
    return formatOf(192000, kChannels, AudioFormat::SampleType::Int32);
}

// A line small enough to run through in a few seconds of fake clock: a
// one-second retention, which is 8 blocks of 44.1 kHz int16 and 52 of
// 192 kHz int32 in the rig's 20 ms blocks.
ZoneSettings shortLine(const ZoneRig& rig)
{
    ZoneSettings settings = rig.settings();
    settings.output.targetBufferNs = 200 * kMs;
    settings.retentionNs = 1000 * kMs;
    settings.lookaheadNs = 300 * kMs;
    settings.lateJoinLeadNs = 300 * kMs;
    settings.start.minimumStartBufferNs = 100 * kMs;
    settings.start.startDelayNs = 100 * kMs;
    return settings;
}

std::unique_ptr<Zone> zoneIn(ZoneRig& rig, AudioMemory& memory, const char* id,
                             const ZoneSettings& settings)
{
    auto created = Zone::create(ZoneId(id), memory, *rig.planner, rig.resamplers, rig.factory,
                                rig.clock, settings);
    CHECK_OK(created);
    return std::move(*created);
}

// One room admitted, attached and placed on the line; what it is sent.
std::shared_ptr<SinkState> startOneRoom(ZoneRig& rig, Zone& zone, const ClientId& room,
                                        const AudioFormat& format, uint64_t targetNs)
{
    CHECK_OK(zone.admit(sessionOf(7), room, offering({format}), format));
    auto sink = connect(zone, sessionOf(7), room);
    const std::shared_ptr<ClientOutput> output = zone.member(room);
    CHECK(waitFor([&] {
        zone.pump();
        return std::to_underlying(output->producedThroughNs()) >= targetNs;
    }));
    reportBuffer(zone, room, 0, targetNs);
    rig.clock.advanceMs(1);
    zone.pump();
    CHECK(zone.started());
    acknowledge(zone, room, std::to_underlying(zone.startTimelineNs()));
    return sink;
}

// The line runs on in steps, the room reporting what a healthy one holds;
// `each` sees every step. Waits for the room to fill up to its target, or for
// a zone that has stopped decoding to have nothing more to give it.
template <typename Each>
void playOn(ZoneRig& rig, Zone& zone, const ClientId& room, SinkState& sink, int steps,
            uint64_t stepMs, uint64_t targetNs, Each each)
{
    const std::shared_ptr<ClientOutput> output = zone.member(room);
    for (int step = 0; step < steps; ++step)
    {
        rig.clock.advanceMs(stepMs);
        const uint64_t playhead = std::to_underlying(zone.playheadNs());
        const uint64_t built = deliveredThroughNs(*output, sink);
        reportBuffer(zone, room, playhead, built > playhead ? built - playhead : 0);
        CHECK(waitFor([&] {
            zone.pump();
            const uint64_t produced = std::to_underlying(output->producedThroughNs());
            return produced >= playhead + targetNs ||
                   (zone.state() != ZoneState::Playing &&
                    produced + kPacketNs > std::to_underlying(zone.frontierNs()));
        }));
        if (!each(playhead))
            return;
    }
}

} // namespace

MULTIROOM_TEST(a_zone_past_the_memory_limit_is_refused_by_name_and_the_zones_playing_are_untouched)
{
    // Room for two zones of CD audio and a block to spare: the third play()
    // is refused with the numbers in it, and the two that play never miss a
    // block -- the old shared pool let them starve it silently instead.
    ZoneRig rig;
    const ZoneSettings settings = shortLine(rig);
    const size_t blockBytes = rig.memory.blockBytes();
    CHECK_EQ(blocksFor(blockBytes, formatOf(), settings.retentionNs), size_t{8});
    AudioMemory memory(blockBytes, 17 * blockBytes);

    auto a = zoneIn(rig, memory, "a", settings);
    auto b = zoneIn(rig, memory, "b", settings);
    auto c = zoneIn(rig, memory, "c", settings);
    CHECK_EQ(memory.usedBytes(), 0ULL); // three zones that exist cost nothing

    for (Zone* zone : {a.get(), b.get(), c.get()})
        CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(a->play());
    CHECK_OK(b->play());
    CHECK_EQ(a->blocks(), size_t{8});
    CHECK_EQ(memory.usedBytes(), 16ULL * blockBytes);

    const Status<ZoneError> refused = c->play();
    CHECK_ERR(refused, ZoneError::StoreFailed);
    if (!refused)
    {
        CHECK(refused.error().detail.find("zone c at") != std::string::npos);
        CHECK(refused.error().detail.find("needs 0.2 MB and 0.0 MB of the 0 MB limit are free") !=
              std::string::npos);
    }
    CHECK(c->state() == ZoneState::Idle);
    CHECK_EQ(c->stagedCount(), size_t{1});
    CHECK_EQ(c->blocks(), size_t{0});

    for (int pass = 0; pass < 5; ++pass)
    {
        a->pump();
        b->pump();
    }
    for (Zone* zone : {a.get(), b.get()})
    {
        CHECK(zone->state() == ZoneState::Playing);
        CHECK(zone->publishedBlocks() > 0);
        CHECK_EQ(zone->blocksRefused(), 0ULL);
        CHECK_EQ(zone->blocks(), size_t{8});
    }

    // A zone that stops gives its memory back, and the refused one plays.
    CHECK_OK(a->stopPlayback());
    CHECK_EQ(a->blocks(), size_t{0});
    CHECK_OK(c->play());
    CHECK_EQ(c->blocks(), size_t{8});
}

MULTIROOM_TEST(a_zone_at_192_khz_int32_plays_past_its_retention_without_ever_waiting_for_a_block)
{
    // The deadlock phase 42 is for. The old shared pool held 2.56 s of this
    // format against a 5 s retention: the store never evicted, the pool never
    // got a block back, and the decode never resumed. Its own blocks cover
    // its own retention.
    ZoneRig rig(hiRes(), 192000ULL * 15);
    ZoneSettings settings = rig.settings();
    settings.output.targetBufferNs = 1500 * kMs;
    settings.retentionNs = 5ULL * kNanosecondsPerSecond;
    settings.lookaheadNs = 0;
    settings.lateJoinLeadNs = 0;
    AudioMemory memory(rig.memory.blockBytes(), 64ULL * 1024 * 1024);
    auto zone = zoneIn(rig, memory, "study", settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());
    CHECK_EQ(zone->blocks(), size_t{252});
    CHECK_EQ(zone->memoryBytes(), 252ULL * rig.memory.blockBytes());

    const ClientId room("study");
    const auto sink = startOneRoom(rig, *zone, room, hiRes(), 1500 * kMs);
    const std::shared_ptr<ClientOutput> output = zone->member(room);

    // Twelve seconds of line, each step filled to the target before the next.
    playOn(rig, *zone, room, *sink, 24, 500, 1500 * kMs, [&](uint64_t) {
        CHECK(output->state() == OutputState::Streaming);
        return true;
    });
    CHECK(std::to_underlying(zone->oldestNs()) > 0); // the retention was reached, and evicted
    CHECK_EQ(zone->blocksRefused(), 0ULL);
    CHECK_EQ(zone->rejoinCount(), 0ULL);
    CHECK_EQ(zone->blocks(), size_t{252});
}

MULTIROOM_TEST(a_boundary_to_192_khz_grows_the_zone_and_the_way_back_frees_the_surplus)
{
    // CD audio, a 192 kHz track, and CD audio again, through a room resampled
    // into a fixed rate that plays straight through both boundaries.
    ZoneRig rig(formatOf(), kRate * 3);
    const ZoneSettings settings = shortLine(rig);
    AudioMemory memory(rig.memory.blockBytes(), 64ULL * 1024 * 1024);
    auto zone = zoneIn(rig, memory, "living", settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->stage(segmentOf("two", "hires")));
    CHECK_OK(zone->stage(segmentOf("three", "ramp")));
    CHECK_OK(zone->play());
    CHECK_EQ(zone->blocks(), size_t{8});

    const ClientId room("living");
    const auto sink = startOneRoom(rig, *zone, room, fixedRate(), 200 * kMs);

    size_t peak = 0;
    size_t atHiRes = 0;
    playOn(rig, *zone, room, *sink, 120, 100, 200 * kMs, [&](uint64_t) {
        peak = std::max(peak, zone->blocks());
        const std::vector<StagedSource> opened = zone->sources();
        if (opened.size() == 2 && atHiRes == 0)
            atHiRes = zone->blocks();
        // Back down once the store holds nothing of the 192 kHz track.
        return !(opened.size() == 3 && zone->blocks() == 8);
    });

    CHECK_EQ(atHiRes, size_t{52}); // grown before a block of it was decoded
    CHECK_EQ(peak, size_t{52});
    CHECK_EQ(zone->sources().size(), size_t{3});
    CHECK_EQ(zone->blocks(), size_t{8});
    CHECK_EQ(memory.usedBytes(), 8ULL * rig.memory.blockBytes());
    CHECK(zone->state() != ZoneState::Failed);
    CHECK_EQ(zone->blocksRefused(), 0ULL);
}

MULTIROOM_TEST(a_boundary_the_memory_limit_cannot_hold_fails_the_zone_there_by_name)
{
    // Room for the CD track and not for the 192 kHz one after it: the zone
    // fails at the boundary, says why in its error and its event, and does
    // not stall part-way into the new source -- nothing of it is decoded.
    ZoneRig rig(formatOf(), kRate * 3);
    ZoneSettings settings = shortLine(rig);
    std::mutex failedMutex;
    std::vector<ZoneEvent> failed;
    settings.onEvent = [&](const ZoneEvent& event) {
        std::lock_guard<std::mutex> lock(failedMutex);
        if (event.kind == ZoneEventKind::SourceFailed)
            failed.push_back(event);
    };
    AudioMemory memory(rig.memory.blockBytes(), 20 * rig.memory.blockBytes());
    auto zone = zoneIn(rig, memory, "living", settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->stage(segmentOf("two", "hires")));
    CHECK_OK(zone->play());

    const ClientId room("living");
    const auto sink = startOneRoom(rig, *zone, room, fixedRate(), 200 * kMs);
    playOn(rig, *zone, room, *sink, 60, 100, 200 * kMs,
           [&](uint64_t) { return zone->state() != ZoneState::Failed; });

    CHECK(zone->state() == ZoneState::Failed);
    CHECK(zone->lastError().find("StoreFailed") != std::string::npos);
    CHECK(zone->lastError().find("needs 1.5 MB") != std::string::npos);
    CHECK_EQ(zone->sources().size(), size_t{1});
    CHECK_EQ(zone->blocks(), size_t{8});
    {
        std::lock_guard<std::mutex> lock(failedMutex);
        CHECK_EQ(failed.size(), size_t{1});
        if (!failed.empty())
        {
            CHECK_EQ(failed.front().sourceId, std::string("two"));
            CHECK(failed.front().reason.find("needs 1.5 MB") != std::string::npos);
        }
    }
}

MULTIROOM_TEST(a_paused_zone_keeps_its_memory_and_a_stopped_one_gives_all_of_it_back)
{
    ZoneRig rig;
    const ZoneSettings settings = shortLine(rig);
    AudioMemory memory(rig.memory.blockBytes(), 64ULL * 1024 * 1024);
    auto zone = zoneIn(rig, memory, "living", settings);

    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());
    const ClientId room("living");
    const auto sink = startOneRoom(rig, *zone, room, formatOf(), 200 * kMs);
    playOn(rig, *zone, room, *sink, 5, 100, 200 * kMs, [](uint64_t) { return true; });
    const uint64_t held = memory.usedBytes();
    CHECK_EQ(held, 8ULL * rig.memory.blockBytes());

    // The resume plays what was held, so a pause keeps it.
    CHECK_OK(zone->pausePlayback());
    rig.clock.advanceMs(2000);
    zone->pump();
    CHECK_EQ(memory.usedBytes(), held);
    CHECK_EQ(zone->blocks(), size_t{8});

    // A stop drops what every room holds, and the zone holds nothing at all.
    CHECK_OK(zone->stopPlayback());
    CHECK_EQ(zone->blocks(), size_t{0});
    CHECK_EQ(memory.usedBytes(), 0ULL);

    // And play() sizes it again.
    CHECK_OK(zone->stage(segmentOf("two", "ramp")));
    CHECK_OK(zone->play());
    CHECK_EQ(zone->blocks(), size_t{8});
}

// --- a room that played a zone alone is joined (GroupedDriftCorrection.md) ---

namespace {

// Reports from a room heard `aheadNs` past a line that began at `lineNs` at
// `instantNs` -- the line as it was, whatever the zone has done to it since --
// carrying on from the twelve driveOffset() sends.
void reportFrom(Zone& zone, const ClientId& client, const ClientClockModel& model,
                uint64_t lineNs, uint64_t instantNs, int64_t aheadNs, int reports)
{
    uint64_t clientNs = 506ULL * kNanosecondsPerSecond;
    for (int i = 13; i < 13 + reports; ++i)
    {
        clientNs += 500000000ULL;
        const uint64_t serverNs = model.toServerTimeNs(clientNs);

        matp::PlaybackReport report;
        report.clockQuality = AudioClockQuality::Hardware;
        report.localMonotonicTimeNs = clientNs;
        report.bufferedDurationNs = 1500000000ULL;
        report.deviceFramePosition = static_cast<uint64_t>(
            static_cast<double>(kRate) * static_cast<double>(i) * 0.5 + 0.5);
        report.hasTimelineNsAudible = true;
        report.timelineNsAudible =
            static_cast<uint64_t>(static_cast<int64_t>(lineNs + (serverNs - instantNs)) + aheadNs);
        zone.onPlaybackReport(client, report, model);
    }
}

ZoneSettings joinable(const ZoneRig& rig, std::function<RoomAudioSettings()> preference)
{
    ZoneSettings settings = rig.settings();
    settings.lateJoinLeadNs = 2ULL * kNanosecondsPerSecond;
    settings.start.startDelayNs = kNanosecondsPerSecond;
    settings.roomPreference = [preference = std::move(preference)](const ClientId&) {
        return preference();
    };
    return settings;
}

} // namespace

MULTIROOM_TEST(a_room_grouped_while_it_plays_alone_keeps_its_stream_and_the_line_moves_onto_it)
{
    // The room has played the zone alone, bitperfect and uncorrected, for long
    // enough that its card is 120 ms ahead of the line. It is grouped while it
    // plays: a converter on the stream it has, in the format its device has,
    // and the line moved onto it -- so it is corrected from no offset rather
    // than hard resynced from that one on its next report, and the room that
    // joins is placed where it is heard. It is sent nothing at all.
    ZoneRig rig;
    rig.clock.setNs(90ULL * kNanosecondsPerSecond);

    auto grouped = std::make_shared<std::atomic<bool>>(false);
    auto zone = rig.zone(joinable(rig, [grouped] {
        RoomAudioSettings room;
        room.grouped = grouped->load();
        return room;
    }));
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId living("livingroom");
    const ClientId kitchen("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), living, offering({formatOf()}), formatOf()));
    auto livingSink = connect(*zone, sessionOf(1), living);
    const std::shared_ptr<ClientOutput> alone = zone->member(living);
    CHECK(alone->plan().mode == OutputMode::BitPerfect);
    CHECK(!alone->driftCorrected());
    CHECK(zone->replanInPlace(living) == Replan::Unchanged);

    reportBuffer(*zone, living, 0, 1500 * 1000000ULL);
    zone->pump();
    CHECK(zone->started());

    constexpr int64_t kAheadNs = 120LL * 1000000LL;
    const ClientClockModel model = fittedModel();
    driveOffset(*zone, {living}, model, kRate, kAheadNs, 12);
    CHECK(zone->roomStart(living) == RoomStartState::Playing);
    CHECK_NEAR(static_cast<double>(zone->syncStatus(living).offsetNs), 120e6, 1e3);
    CHECK_EQ(zone->rejoins(living), 0ULL);

    const uint64_t line = std::to_underlying(zone->startTimelineNs());
    const uint64_t instant = zone->startServerTimeNs();
    grouped->store(true);
    CHECK(zone->replanInPlace(living) == Replan::InPlace);
    CHECK(waitFor([&] { return alone->formatSwitches() == 1; }));
    CHECK(zone->member(living) == alone);
    CHECK(alone->driftCorrected());
    CHECK(alone->plan().reason == PlanReason::Grouped);
    CHECK(alone->plan().outputFormat == formatOf());
    CHECK(zone->replanInPlace(living) == Replan::Unchanged);

    CHECK_EQ(std::to_underlying(zone->startTimelineNs()), line + kAheadNs);
    CHECK_EQ(zone->startServerTimeNs(), instant);
    CHECK(!zone->syncStatus(living).hasOffset);

    // Heard where it was all along, which is now on the line.
    reportFrom(*zone, living, model, line, instant, kAheadNs, 6);
    CHECK(zone->syncStatus(living).hasOffset);
    CHECK_NEAR(static_cast<double>(zone->syncStatus(living).offsetNs), 0.0, 1e3);
    CHECK(!zone->syncStatus(living).resyncWanted);
    CHECK_EQ(zone->rejoins(living), 0ULL);

    // The second room joins that line, and the first is left exactly as it is.
    CHECK_OK(zone->admit(sessionOf(2), kitchen, offering({formatOf()}), formatOf()));
    CHECK(zone->member(kitchen)->driftCorrected());
    CHECK_EQ(std::to_underlying(zone->startTimelineNs()), line + kAheadNs);
    CHECK_EQ(rig.starts->of(RoomCommandKind::Start, "livingroom").size(), size_t{1});
    CHECK_EQ(rig.starts->of(RoomCommandKind::Renegotiate, "livingroom").size(), size_t{0});
    CHECK_EQ(rig.starts->of(RoomCommandKind::Pause, "livingroom").size(), size_t{0});
    CHECK(zone->roomStart(living) == RoomStartState::Playing);

    CHECK(waitFor([&] { return alone->packets() > 0; }));
    size_t packets = 0;
    CHECK_EQ(jumpsIn(*livingSink, packets), size_t{0});
    CHECK(packets > 0);
    CHECK_EQ(zone->rejoinCount(), 0ULL);
    CHECK(zone->state() != ZoneState::Failed);
}

MULTIROOM_TEST(a_room_joining_one_that_drifts_alone_is_placed_where_that_room_is_heard)
{
    // A bitPerfect room stays uncorrected in a group, so nothing is replanned
    // -- but the room that joins it still starts where it is heard rather
    // than where the line has walked away from it. A room 80 ms behind moves
    // the line back; one that is not steady, or not far enough off to matter,
    // moves nothing.
    ZoneRig rig;
    rig.clock.setNs(90ULL * kNanosecondsPerSecond);
    auto zone = rig.zone(
        joinable(rig, [] { return RoomAudioSettings{AudioControl::BitPerfect, true}; }));
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId living("livingroom");
    CHECK_OK(zone->admit(sessionOf(1), living, offering({formatOf()}), formatOf()));
    reportBuffer(*zone, living, 200 * 1000000ULL, 1300 * 1000000ULL);
    zone->pump();
    CHECK(zone->started());
    CHECK(zone->replanInPlace(living) == Replan::Unchanged);

    constexpr int64_t kBehindNs = -80LL * 1000000LL;
    const ClientClockModel model = fittedModel();
    driveOffset(*zone, {living}, model, kRate, kBehindNs, 12);
    const uint64_t line = std::to_underlying(zone->startTimelineNs());

    const ClientId kitchen("kitchen");
    CHECK_OK(zone->admit(sessionOf(2), kitchen, offering({formatOf()}), formatOf()));
    CHECK_EQ(std::to_underlying(zone->startTimelineNs()), line - 80ULL * 1000000ULL);
    CHECK(!zone->syncStatus(living).hasOffset);

    // Two rooms on the line now: a third joining moves nothing, however far
    // off either of them is.
    reportBuffer(*zone, kitchen,
                 std::to_underlying(zone->playheadNs()) + 2ULL * kNanosecondsPerSecond,
                 1000 * 1000000ULL);
    zone->pump();
    CHECK_EQ(std::string(roomStartStateName(zone->roomStart(kitchen))), std::string("scheduled"));
    const uint64_t twoRooms = std::to_underlying(zone->startTimelineNs());
    reportFrom(*zone, living, model, twoRooms, zone->startServerTimeNs(), 30LL * 1000000LL, 6);
    CHECK_NEAR(static_cast<double>(zone->syncStatus(living).offsetNs), 30e6, 1e3);
    CHECK_OK(zone->admit(sessionOf(3), ClientId("hall"), offering({formatOf()}), formatOf()));
    CHECK_EQ(std::to_underlying(zone->startTimelineNs()), twoRooms);
    CHECK_EQ(zone->rejoins(living), 0ULL);
    CHECK(zone->state() != ZoneState::Failed);
}

MULTIROOM_TEST(a_bitperfect_room_with_a_trimmable_clock_is_trimmed_once_it_is_grouped)
{
    // The one bitPerfect room grouping does change: its own clock can be
    // trimmed, which leaves its samples alone. Grouped while it plays, it is
    // trimmed from then on -- the same bytes, no rejoin, and the actuator the
    // controller is told about on its next report.
    ZoneRig rig;
    rig.clock.setNs(90ULL * kNanosecondsPerSecond);
    auto grouped = std::make_shared<std::atomic<bool>>(false);
    auto zone = rig.zone(joinable(rig, [grouped] {
        return RoomAudioSettings{AudioControl::BitPerfect, grouped->load()};
    }));
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    ClientCapabilities trimmed = offering({formatOf()});
    trimmed.driftFeedback = true;
    trimmed.hardwareClock.available = true;
    trimmed.hardwareClock.minPpm = -500;
    trimmed.hardwareClock.maxPpm = 500;
    const ClientId room("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), room, trimmed, formatOf()));
    const std::shared_ptr<ClientOutput> output = zone->member(room);
    CHECK(output->driftActuator() == DriftActuator::None);

    grouped->store(true);
    CHECK(zone->replanInPlace(room) == Replan::InPlace);
    CHECK(waitFor([&] { return output->formatSwitches() == 1; }));
    CHECK(zone->member(room) == output);
    CHECK(output->plan().mode == OutputMode::BitPerfect);
    CHECK(output->passThrough());
    CHECK(output->driftActuator() == DriftActuator::ClientHardwareClock);
    CHECK_EQ(rig.starts->of(RoomCommandKind::Renegotiate, "kitchen").size(), size_t{0});
    CHECK_EQ(zone->rejoinCount(), 0ULL);
}

// --- a room left alone in its zone (roadmap/zone-leave-restores-solo-plan.md) ---

namespace {

// Settings for a room whose grouping a test changes while it plays.
ZoneSettings groupable(ZoneSettings settings, std::shared_ptr<std::atomic<bool>> grouped,
                       AudioControl audioControl = AudioControl::Auto)
{
    settings.roomPreference = [grouped, audioControl](const ClientId&) {
        return RoomAudioSettings{audioControl, grouped->load()};
    };
    return settings;
}

// One room, alone and bitperfect, then grouped in place: resampled and
// corrected on the stream it has, with the line started under it.
std::shared_ptr<ClientOutput> groupOnTheLine(Zone& zone, const ClientId& room,
                                             std::atomic<bool>& grouped,
                                             std::shared_ptr<SinkState>* sink = nullptr)
{
    CHECK_OK(zone.admit(sessionOf(1), room, offering({formatOf()}), formatOf()));
    const std::shared_ptr<SinkState> connected = connect(zone, sessionOf(1), room);
    if (sink != nullptr)
        *sink = connected;
    const std::shared_ptr<ClientOutput> output = zone.member(room);
    CHECK(output->plan().mode == OutputMode::BitPerfect);

    grouped.store(true);
    CHECK(zone.replanInPlace(room) == Replan::InPlace);
    CHECK(waitFor([&] { return output->formatSwitches() == 1; }));
    CHECK(output->plan().reason == PlanReason::Grouped);

    reportBuffer(zone, room, 0, 1500 * kMs);
    zone.pump();
    CHECK(zone.started());
    CHECK(zone.transport() == TransportState::Playing);
    acknowledge(zone, room, std::to_underlying(zone.startTimelineNs()));
    return output;
}

// What a room was sent from `boundaryNs` on is `frames` of the ramp from its
// first frame, byte for byte: nothing skipped, nothing converted.
void rampFrom(SinkState& sink, uint64_t boundaryNs, uint64_t frames)
{
    std::string payloads;
    const std::vector<matp::AudioPacketHeader> headers = decodeHeaders(sink, payloads);
    const uint32_t bytesPerFrame = formatOf().bytesPerFrame();
    size_t offset = 0;
    for (const matp::AudioPacketHeader& header : headers)
    {
        if (header.timelineStartNs >= boundaryNs)
            break;
        offset += static_cast<size_t>(header.outputFrameCount) * bytesPerFrame;
    }
    std::vector<std::byte> expected(static_cast<size_t>(frames) * bytesPerFrame);
    writeRamp(expected, 0, static_cast<uint32_t>(frames), kChannels);
    CHECK(payloads.size() >= offset + expected.size());
    if (payloads.size() >= offset + expected.size())
        CHECK_EQ(std::memcmp(payloads.data() + offset, expected.data(), expected.size()), 0);
}

// A grouped room on a short line, played on until the decode has opened the
// second of two sources and the room has not built into it yet.
struct NearBoundary
{
    std::unique_ptr<Zone> zone;
    std::shared_ptr<SinkState> sink;
    std::shared_ptr<ClientOutput> output;
    uint64_t boundaryNs = 0;
};

NearBoundary groupedNearBoundary(ZoneRig& rig, const ClientId& room,
                                 std::shared_ptr<std::atomic<bool>> grouped)
{
    NearBoundary near;
    near.zone = rig.zone(groupable(shortLine(rig), grouped));
    CHECK_OK(near.zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(near.zone->stage(segmentOf("two", "ramp")));
    CHECK_OK(near.zone->play());
    near.sink = startOneRoom(rig, *near.zone, room, formatOf(), 200 * kMs);
    near.output = near.zone->member(room);

    grouped->store(true);
    CHECK(near.zone->replanInPlace(room) == Replan::InPlace);
    CHECK(waitFor([&] { return near.output->formatSwitches() == 1; }));

    playOn(rig, *near.zone, room, *near.sink, 60, 100, 200 * kMs,
           [&](uint64_t) { return near.zone->sources().size() < 2; });
    CHECK_EQ(near.zone->sources().size(), size_t{2});
    near.boundaryNs = std::to_underlying(near.zone->sources().back().timelineStartNs);
    CHECK(std::to_underlying(near.output->producedThroughNs()) < near.boundaryNs);
    return near;
}

} // namespace

MULTIROOM_TEST(a_room_left_alone_while_it_plays_is_bitperfect_again_from_its_next_track)
{
    // Grouped, it is resampled to be corrected. Left alone in the middle of a
    // track, it keeps that converter to the end of the track -- a converter
    // changed mid-track is heard -- and the next one is bitperfect, on the
    // same device and the same stream: no re-negotiation, nothing skipped.
    ZoneRig rig;
    rig.clock.setNs(90ULL * kNanosecondsPerSecond);
    auto grouped = std::make_shared<std::atomic<bool>>(false);
    auto zone = rig.zone(groupable(rig.settings(), grouped));
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId room("livingroom");
    std::shared_ptr<SinkState> sink;
    const std::shared_ptr<ClientOutput> output = groupOnTheLine(*zone, room, *grouped, &sink);

    grouped->store(false);
    CHECK(zone->replanAlone(room) == Replan::Pending);
    CHECK(zone->replanPending(room));
    CHECK_EQ(zone->pendingAlone().size(), size_t{1});
    CHECK(waitFor([&] { return output->packets() >= kSourcePackets; }));
    CHECK_EQ(output->formatSwitches(), 1u);
    CHECK(output->plan().reason == PlanReason::Grouped);

    // The next track is the boundary.
    CHECK_OK(zone->stage(segmentOf("two", "ramp")));
    zone->pump();
    CHECK(waitFor([&] { return output->formatSwitches() == 2; }));
    CHECK(zone->member(room) == output);
    CHECK(output->plan().mode == OutputMode::BitPerfect);
    CHECK(!output->driftCorrected());
    CHECK(zone->pendingAlone().empty());

    // Pending until the line reaches the boundary, which is when it is heard.
    CHECK(zone->replanPending(room));
    rig.clock.advanceMs(4100);
    CHECK(!zone->replanPending(room));

    CHECK(waitFor([&] { return output->packets() >= 2 * kSourcePackets; }));
    size_t packets = 0;
    CHECK_EQ(jumpsIn(*sink, packets), size_t{0});
    rampFrom(*sink, 3ULL * kNanosecondsPerSecond, kSourceFrames);
    CHECK_EQ(rig.starts->of(RoomCommandKind::Renegotiate, "livingroom").size(), size_t{0});
    CHECK_EQ(rig.starts->of(RoomCommandKind::Pause, "livingroom").size(), size_t{0});
    CHECK_EQ(zone->rejoinCount(), 0ULL);
    CHECK(zone->state() != ZoneState::Failed);
}

MULTIROOM_TEST(a_room_left_alone_after_the_next_track_was_opened_switches_at_that_track)
{
    // The decode opens a source a lookahead before it is heard. A room left
    // alone after that, but before it built into the new track, switches on
    // that track's first frame rather than one track later.
    ZoneRig rig;
    const ClientId room("livingroom");
    auto grouped = std::make_shared<std::atomic<bool>>(false);
    NearBoundary near = groupedNearBoundary(rig, room, grouped);

    grouped->store(false);
    CHECK(near.zone->replanAlone(room) == Replan::Pending);
    CHECK(near.zone->pendingAlone().empty()); // switched there already
    CHECK(near.zone->replanPending(room));
    CHECK(near.output->plan().reason == PlanReason::Grouped);

    playOn(rig, *near.zone, room, *near.sink, 30, 100, 200 * kMs,
           [&](uint64_t playhead) { return playhead < near.boundaryNs + 300 * kMs; });
    CHECK(waitFor([&] { return near.output->formatSwitches() == 2; }));
    CHECK(near.output->plan().mode == OutputMode::BitPerfect);
    CHECK(!near.zone->replanPending(room));

    size_t packets = 0;
    CHECK_EQ(jumpsIn(*near.sink, packets), size_t{0});
    std::string payloads;
    const std::vector<matp::AudioPacketHeader> headers = decodeHeaders(*near.sink, payloads);
    uint64_t framesAfter = 0;
    for (const matp::AudioPacketHeader& header : headers)
    {
        if (header.timelineStartNs >= near.boundaryNs)
            framesAfter += header.outputFrameCount;
    }
    CHECK(framesAfter > 0);
    rampFrom(*near.sink, near.boundaryNs, framesAfter);
    CHECK_EQ(rig.starts->of(RoomCommandKind::Renegotiate, "livingroom").size(), size_t{0});
    CHECK_EQ(rig.starts->of(RoomCommandKind::Pause, "livingroom").size(), size_t{0});
    CHECK(near.zone->state() != ZoneState::Failed);
}

MULTIROOM_TEST(a_room_grouped_again_before_its_boundary_keeps_its_grouped_plan)
{
    // Waiting for its next track: grouped again, nothing is waited for.
    {
        ZoneRig rig;
        rig.clock.setNs(90ULL * kNanosecondsPerSecond);
        auto grouped = std::make_shared<std::atomic<bool>>(false);
        auto zone = rig.zone(groupable(rig.settings(), grouped));
        CHECK_OK(zone->stage(segmentOf("one", "ramp")));
        CHECK_OK(zone->play());
        const ClientId room("livingroom");
        const std::shared_ptr<ClientOutput> output = groupOnTheLine(*zone, room, *grouped);

        grouped->store(false);
        CHECK(zone->replanAlone(room) == Replan::Pending);
        grouped->store(true);
        CHECK(zone->replanInPlace(room) == Replan::Unchanged);
        CHECK(zone->pendingAlone().empty());
        CHECK(!zone->replanPending(room));

        CHECK_OK(zone->stage(segmentOf("two", "ramp")));
        zone->pump();
        CHECK(waitFor([&] { return output->packets() >= 2 * kSourcePackets; }));
        CHECK_EQ(output->formatSwitches(), 1u);
        CHECK(output->plan().reason == PlanReason::Grouped);
    }

    // Its switch made at a boundary it has not reached: the switch there is to
    // the grouped plan instead, so the stream changes nothing mid-track.
    {
        ZoneRig rig;
        const ClientId room("livingroom");
        auto grouped = std::make_shared<std::atomic<bool>>(false);
        NearBoundary near = groupedNearBoundary(rig, room, grouped);

        grouped->store(false);
        CHECK(near.zone->replanAlone(room) == Replan::Pending);
        grouped->store(true);
        CHECK(near.zone->replanInPlace(room) == Replan::InPlace);
        CHECK(!near.zone->replanPending(room));
        CHECK(near.output->plan().reason == PlanReason::Grouped);

        playOn(rig, *near.zone, room, *near.sink, 30, 100, 200 * kMs,
               [&](uint64_t playhead) { return playhead < near.boundaryNs + 300 * kMs; });
        CHECK(waitFor([&] { return near.output->formatSwitches() == 2; }));
        CHECK(near.output->plan().reason == PlanReason::Grouped);
        CHECK(near.output->driftCorrected());
        size_t packets = 0;
        CHECK_EQ(jumpsIn(*near.sink, packets), size_t{0});
        CHECK_EQ(rig.starts->of(RoomCommandKind::Renegotiate, "livingroom").size(), size_t{0});
    }
}

MULTIROOM_TEST(a_room_the_join_reopened_in_another_format_is_reopened_at_its_next_track)
{
    // Its device was opened at 96 kHz for the group. Only the source's own
    // format makes it bitperfect again, so it plays the track out, is held on
    // the boundary and reopened there, as at a change of format -- and is
    // then planned exactly as it would have been had nobody joined it.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto grouped = std::make_shared<std::atomic<bool>>(true);
    auto zone = rig.zone(groupable(rig.settings(), grouped));
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    const ClientId room("livingroom");
    const ClientCapabilities capabilities = offering({formatOf(), fixedRate()});
    CHECK_OK(zone->admit(sessionOf(1), room, capabilities, fixedRate()));
    const std::shared_ptr<ClientOutput> output = zone->member(room);
    CHECK(output->plan().outputFormat == fixedRate());
    reportBuffer(*zone, room, 0, 1500 * kMs);
    zone->pump();
    CHECK(zone->started());
    acknowledge(*zone, room, std::to_underlying(zone->startTimelineNs()));

    // In place it could only be resampled into the format its device has.
    grouped->store(false);
    CHECK(zone->replanAlone(room) == Replan::Pending);
    CHECK_EQ(output->formatSwitches(), 0u);
    CHECK(output->plan().outputFormat == fixedRate());

    CHECK_OK(zone->stage(segmentOf("two", "ramp")));
    zone->pump();
    CHECK(zone->pendingAlone().empty());
    const std::vector<RoomCommand> pauses = rig.starts->of(RoomCommandKind::Pause, "livingroom");
    CHECK_EQ(pauses.size(), size_t{1});
    if (!pauses.empty())
        CHECK_EQ(pauses.front().at.timelineNs, 3ULL * kNanosecondsPerSecond);
    CHECK(waitFor([&] {
        return std::to_underlying(output->producedThroughNs()) == 3ULL * kNanosecondsPerSecond;
    }));
    CHECK_EQ(rig.starts->count(RoomCommandKind::Renegotiate), size_t{0});

    rig.clock.advanceMs(4100);
    zone->pump();
    CHECK_EQ(rig.starts->of(RoomCommandKind::Renegotiate, "livingroom").size(), size_t{1});

    zone->onStreamReady(room, true, formatOf());
    CHECK(waitFor([&] { return output->formatSwitches() == 1; }));
    CHECK(zone->member(room) == output);
    const auto solo = rig.planner->plan(formatOf(), RoomAudioSettings{}, capabilities);
    CHECK_OK(solo);
    CHECK(output->plan().mode == OutputMode::BitPerfect);
    CHECK(output->plan().mode == solo->mode);
    CHECK(output->plan().outputFormat == solo->outputFormat);
    CHECK(output->plan().driftActuator == solo->driftActuator);
    CHECK(zone->state() != ZoneState::Failed);
}

MULTIROOM_TEST(a_room_left_alone_while_its_zone_does_not_play_is_planned_alone_at_once)
{
    // Paused: nothing is audible to change in the middle of, so the converter
    // goes on the stream it has, now.
    {
        ZoneRig rig;
        rig.clock.setNs(90ULL * kNanosecondsPerSecond);
        auto grouped = std::make_shared<std::atomic<bool>>(false);
        auto zone = rig.zone(groupable(rig.settings(), grouped));
        CHECK_OK(zone->stage(segmentOf("one", "ramp")));
        CHECK_OK(zone->play());
        const ClientId room("livingroom");
        const std::shared_ptr<ClientOutput> output = groupOnTheLine(*zone, room, *grouped);
        CHECK_OK(zone->pausePlayback());

        grouped->store(false);
        CHECK(zone->replanAlone(room) == Replan::InPlace);
        CHECK(waitFor([&] { return output->formatSwitches() == 2; }));
        CHECK(output->plan().mode == OutputMode::BitPerfect);
        CHECK(!zone->replanPending(room));
        CHECK_EQ(rig.starts->count(RoomCommandKind::Renegotiate), size_t{0});
    }

    // Pending when the line stopped playing: due then.
    {
        ZoneRig rig;
        rig.clock.setNs(90ULL * kNanosecondsPerSecond);
        auto grouped = std::make_shared<std::atomic<bool>>(false);
        auto zone = rig.zone(groupable(rig.settings(), grouped));
        CHECK_OK(zone->stage(segmentOf("one", "ramp")));
        CHECK_OK(zone->play());
        const ClientId room("livingroom");
        const std::shared_ptr<ClientOutput> output = groupOnTheLine(*zone, room, *grouped);

        grouped->store(false);
        CHECK(zone->replanAlone(room) == Replan::Pending);
        CHECK_OK(zone->pausePlayback());
        CHECK_EQ(zone->pendingAlone().size(), size_t{1});
        CHECK(zone->replanAlone(room) == Replan::InPlace);
        CHECK(zone->pendingAlone().empty());
        CHECK(waitFor([&] { return output->plan().mode == OutputMode::BitPerfect; }));
    }

    // Stopped, with a device the join reopened in another format: only a
    // re-negotiation gives it its own plan back.
    {
        ZoneRig rig;
        rig.clock.setNs(90ULL * kNanosecondsPerSecond);
        auto grouped = std::make_shared<std::atomic<bool>>(true);
        auto zone = rig.zone(groupable(rig.settings(), grouped));
        CHECK_OK(zone->stage(segmentOf("one", "ramp")));
        CHECK_OK(zone->play());
        const ClientId room("livingroom");
        CHECK_OK(zone->admit(sessionOf(1), room, offering({formatOf(), fixedRate()}), fixedRate()));
        reportBuffer(*zone, room, 0, 1500 * kMs);
        zone->pump();
        CHECK(zone->started());
        CHECK_OK(zone->stopPlayback());

        grouped->store(false);
        CHECK(zone->replanAlone(room) == Replan::Renegotiate);
        CHECK(zone->pendingAlone().empty());
    }

    // A bitPerfect room is planned the same alone: nothing to do.
    {
        ZoneRig rig;
        auto grouped = std::make_shared<std::atomic<bool>>(true);
        auto zone = rig.zone(groupable(rig.settings(), grouped, AudioControl::BitPerfect));
        CHECK_OK(zone->stage(segmentOf("one", "ramp")));
        CHECK_OK(zone->play());
        const ClientId room("livingroom");
        CHECK_OK(zone->admit(sessionOf(1), room, offering({formatOf()}), formatOf()));
        grouped->store(false);
        CHECK(zone->replanAlone(room) == Replan::Unchanged);
        CHECK(!zone->replanPending(room));
    }
}

MULTIROOM_TEST(a_trimmed_room_left_alone_is_untrimmed_at_its_next_track)
{
    // A bitPerfect room whose own clock was trimmed for the group keeps its
    // samples either way; the trim stops where the next track begins.
    ZoneRig rig;
    rig.clock.setNs(90ULL * kNanosecondsPerSecond);
    auto grouped = std::make_shared<std::atomic<bool>>(true);
    auto zone = rig.zone(groupable(rig.settings(), grouped, AudioControl::BitPerfect));
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->play());

    ClientCapabilities trimmed = offering({formatOf()});
    trimmed.driftFeedback = true;
    trimmed.hardwareClock.available = true;
    trimmed.hardwareClock.minPpm = -500;
    trimmed.hardwareClock.maxPpm = 500;
    const ClientId room("kitchen");
    CHECK_OK(zone->admit(sessionOf(1), room, trimmed, formatOf()));
    const std::shared_ptr<ClientOutput> output = zone->member(room);
    CHECK(output->driftActuator() == DriftActuator::ClientHardwareClock);
    reportBuffer(*zone, room, 0, 1500 * kMs);
    zone->pump();
    CHECK(zone->started());
    acknowledge(*zone, room, std::to_underlying(zone->startTimelineNs()));

    grouped->store(false);
    CHECK(zone->replanAlone(room) == Replan::Pending);
    CHECK(output->driftActuator() == DriftActuator::ClientHardwareClock);

    CHECK_OK(zone->stage(segmentOf("two", "ramp")));
    zone->pump();
    CHECK(waitFor([&] { return output->formatSwitches() == 1; }));
    CHECK(output->plan().mode == OutputMode::BitPerfect);
    CHECK(output->driftActuator() == DriftActuator::None);
    CHECK_EQ(rig.starts->of(RoomCommandKind::Renegotiate, "kitchen").size(), size_t{0});
}

MULTIROOM_TEST(a_room_left_alone_while_it_reopens_for_a_format_boundary_is_planned_alone_by_it)
{
    // Held at a change of format, its device reopening: it stays pending, and
    // the plan the reopened device is given is already the one for a room
    // alone -- so the next track has nothing left to do.
    ZoneRig rig;
    rig.clock.setNs(50ULL * kNanosecondsPerSecond);
    auto grouped = std::make_shared<std::atomic<bool>>(true);
    auto zone = rig.zone(groupable(rig.settings(), grouped));
    CHECK_OK(zone->stage(segmentOf("one", "ramp")));
    CHECK_OK(zone->stage(segmentOf("two", "other")));
    CHECK_OK(zone->play());

    const ClientId room("livingroom");
    CHECK_OK(zone->admit(sessionOf(1), room, eitherRate(), formatOf()));
    const std::shared_ptr<ClientOutput> output = zone->member(room);
    CHECK(output->plan().reason == PlanReason::Grouped);
    playTwoFormats(*zone, {room});
    CHECK_EQ(rig.starts->of(RoomCommandKind::Pause, "livingroom").size(), size_t{1});

    grouped->store(false);
    CHECK(zone->replanAlone(room) == Replan::Pending);
    CHECK_EQ(zone->pendingAlone().size(), size_t{1});

    CHECK(waitFor([&] { return std::to_underlying(output->producedThroughNs()) == kBoundaryNs; }));
    rig.clock.advanceMs(4100);
    zone->pump();
    CHECK_EQ(rig.starts->of(RoomCommandKind::Renegotiate, "livingroom").size(), size_t{1});
    zone->onStreamReady(room, true, formatOf(48000));
    CHECK(waitFor([&] { return output->formatSwitches() == 1; }));
    CHECK(output->plan().mode == OutputMode::BitPerfect);
    CHECK(output->plan().outputFormat == formatOf(48000));

    CHECK_OK(zone->stage(segmentOf("three", "other")));
    zone->pump();
    CHECK(zone->pendingAlone().empty());
    CHECK(!zone->replanPending(room));
    CHECK_EQ(output->formatSwitches(), 1u);
    CHECK_EQ(rig.starts->of(RoomCommandKind::Renegotiate, "livingroom").size(), size_t{1});
    CHECK(zone->state() != ZoneState::Failed);
}
