// Phase 18: the whole path, for long, with many rooms.
//
// Every earlier test file pins one module down; this one runs the modules
// together the way the server does and asks what holds after minutes and
// hours rather than after one packet. The rooms are simulated, but not
// scripted: each one is a small model of the real client -- it holds what its
// connection carried, plays it at its own card's rate from the instant a
// START_AT names, pauses where a PAUSE_AT says, answers a re-negotiation with
// the format its zone plans for it, and reports BUFFER_STATUS and
// PLAYBACK_REPORT from that state. What the zone does with those reports is
// then the zone's own business, which is the point.
//
// Time is the injected clock, stepped by the test, so an hour of timeline is
// an hour of *media* and a few seconds of CPU. The rooms' own threads are the
// only thing that runs in real time, and the harness waits for them on a
// condition variable that the arriving packets signal -- never on a sleep --
// and only when a room is actually short of audio.
//
// Every packet each room receives is checked as it arrives, by the client's
// own reader: the numbering, the grid, the format and -- for a bitperfect room
// -- every sample against the generator. Nothing is kept, so an hour costs no
// more memory than a second does, which is also the claim under test.
//
// The memory ceiling is asserted in every test here rather than in one of
// them: each zone allocates its own blocks against the process's one audio
// ceiling (phase 42), and each test records the most blocks its zones ever
// held together and whether a zone's decode was ever refused one.

#include "ManualClock.h"
#include "TestSupport.h"

#include "audio/BlockPool.h"
#include "client/ClientClockModel.h"
#include "client/OutputPlanner.h"
#include "matp/AudioPacket.h"
#include "processing/IClientAudioProcessor.h"
#include "resampler/BuiltinResamplers.h"
#include "resampler/FakeResampler.h"
#include "resampler/ResamplerRegistry.h"
#include "zone/Zone.h"
#include "zone/ZoneManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace multiroom;

namespace {

constexpr uint64_t kMs = 1000000ULL;
constexpr uint64_t kSecond = kNanosecondsPerSecond;
constexpr uint64_t kMinute = 60ULL * kSecond;

// How long the harness waits for one room's thread to top its buffer up.
// A bound on a hang, not a measurement: a healthy room takes milliseconds.
constexpr auto kDeadline = std::chrono::seconds(10);

// The simulated client's clock is this far ahead of the server's, so that a
// report which skipped the clock model would be 1000 s out rather than
// quietly right.
constexpr uint64_t kClientClockAheadNs = 1000ULL * kSecond;

AudioFormat formatOf(uint32_t rate, uint16_t channels = 2,
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
    return SessionId(0x18181818abcdef00ULL, low);
}

ClientCapabilities offering(std::vector<AudioFormat> formats)
{
    ClientCapabilities capabilities;
    capabilities.formats = std::move(formats);
    return capabilities;
}

// The signal, as in test_zone.cpp: every sample a function of its frame within
// its source and its channel, so a room sent the wrong frames is caught by
// their contents rather than by their count.
int16_t sampleAt(uint64_t frame, uint16_t channel)
{
    return static_cast<int16_t>(((frame * 37 + channel * 7919) & 0x7FFF) - 0x4000);
}

// --- the sources ------------------------------------------------------------

class RampDecoder final : public IDecoder
{
public:
    RampDecoder(AudioFormat format, uint64_t frames, std::atomic<uint64_t>& decoded,
                uint64_t start = 0)
        : decoded_(decoded), total_(frames), next_(start)
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
        if (next_ >= total_)
            return fail(DecodeError::EndOfStream, "the ramp is finished");

        const uint32_t bytesPerFrame = info_.format.bytesPerFrame();
        const uint64_t fits = output.size() / bytesPerFrame;
        if (fits == 0)
            return fail(DecodeError::BufferTooSmall, "not one frame of room");

        const uint32_t frames =
            static_cast<uint32_t>(std::min<uint64_t>({fits, kCodecFrames, total_ - next_}));

        // A raw pointer rather than the span's checked subscript: hours of
        // audio go through here, and a debug build checks every index.
        std::byte* out = output.data();
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            for (uint16_t channel = 0; channel < info_.format.channels; ++channel)
            {
                const uint16_t bits = static_cast<uint16_t>(sampleAt(next_ + frame, channel));
                *out++ = static_cast<std::byte>(bits & 0xFF);
                *out++ = static_cast<std::byte>(bits >> 8);
            }
        }

        DecodedSpan span;
        span.firstFrame = SourceFrame{next_};
        span.frameCount = frames;
        span.byteCount = static_cast<size_t>(frames) * bytesPerFrame;
        next_ += frames;
        decoded_.fetch_add(frames);
        return span;
    }

private:
    static constexpr uint64_t kCodecFrames = 4096;

    std::atomic<uint64_t>& decoded_;
    SourceStreamInfo info_;
    uint64_t total_ = 0;
    uint64_t next_ = 0;
};

// A URI is a name this test gave a generated source. Every open is counted
// per URI, because "one decode per zone" is a count of opens and of frames.
class Sources final : public IDecoderFactory
{
public:
    void add(const std::string& uri, AudioFormat format, uint64_t frames)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        known_[uri] = {format, frames};
    }

    Result<std::unique_ptr<IDecoder>, DecodeError> open(const std::string& uri,
                                                        uint64_t startOffsetNs) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = known_.find(uri);
        if (found == known_.end())
            return fail(DecodeError::SourceFailed, "NotFound: " + uri);
        const uint64_t start =
            framesFromNanoseconds(startOffsetNs, found->second.first.sampleRate).value_or(0);
        if (start >= found->second.second)
            return fail(DecodeError::OffsetOutOfRange, uri);
        ++opens_[uri];
        return std::unique_ptr<IDecoder>(std::make_unique<RampDecoder>(
            found->second.first, found->second.second, framesDecoded, start));
    }

    uint64_t opens(const std::string& uri) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = opens_.find(uri);
        return found == opens_.end() ? 0 : found->second;
    }

    std::atomic<uint64_t> framesDecoded{0};

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::pair<AudioFormat, uint64_t>> known_;
    std::map<std::string, uint64_t> opens_;
};

// --- one connection, checked as it arrives ----------------------------------

// Where a bitperfect room's samples come from: a source that began at
// `startNs` on the timeline, at `rate`, with its generator at frame zero.
struct SourceSpan
{
    uint64_t startNs = 0;
    uint32_t rate = 0;
    uint64_t firstFrame = 0; // the generator's frame heard at startNs: non-zero after a seek
};

// Everything one audio connection carried, reduced as it arrives to what the
// client would have noticed. The sink runs on the room's own thread, where an
// assertion cannot be raised, so a fault is recorded as text and the test
// reads it.
struct Wire
{
    std::mutex mutex;
    std::condition_variable arrived;

    matp::AudioPacketReader reader;
    bool anchored = false;
    uint64_t nextSequence = 0;
    uint64_t nextNs = 0;

    uint64_t packets = 0;
    uint64_t frames = 0;
    uint64_t firstNs = 0;
    uint64_t throughNs = 0;
    uint64_t jumps = 0;
    uint64_t flushes = 0;  // packets that carried afterFlush (section 54)
    uint64_t resumedNs = 0; // where the last of them began
    std::vector<AudioFormat> formats; // each change of format, in order
    std::string fault;                // the first thing the client would have refused

    // Set before the first byte arrives, never after.
    std::vector<SourceSpan> verify; // empty: the payload is not checked
    uint64_t expectedFrame = 0;
    uint64_t samplesVerified = 0;

    std::atomic<bool> accepting{true};
    std::atomic<uint64_t> refusals{0};

    void take(const matp::AudioPacket& packet)
    {
        const matp::AudioPacketHeader& header = packet.header;

        // Section 14's numbering: the client anchors on the first packet of a
        // connection and reads any gap after it as the server skipping.
        if (anchored && header.sequenceNumber != nextSequence && fault.empty())
            fault = "sequence " + std::to_string(header.sequenceNumber) + " after " +
                    std::to_string(nextSequence - 1);
        // Section 54: a jump the server announced is not a rejoin, and what the
        // room holds begins there -- the flush dropped the rest.
        const bool flagged = (header.flags & matp::kAudioFlagAfterFlush) != 0;
        const bool jumped = anchored && header.timelineStartNs != nextNs && !flagged;
        if (jumped)
            ++jumps;
        if (flagged)
        {
            ++flushes;
            resumedNs = header.timelineStartNs;
        }
        if (!anchored || flagged)
            firstNs = header.timelineStartNs;

        if (formats.empty() || describeFormat(formats.back()) != describeFormat(header.format))
            formats.push_back(header.format);

        if (!verify.empty())
            check(packet, !anchored || jumped || flagged);

        anchored = true;
        nextSequence = header.sequenceNumber + 1;
        nextNs = header.timelineStartNs + header.timelineDurationNs;
        throughNs = nextNs;
        ++packets;
        frames += header.outputFrameCount;
    }

    void check(const matp::AudioPacket& packet, bool reanchor)
    {
        const matp::AudioPacketHeader& header = packet.header;
        const SourceSpan* span = nullptr;
        for (const SourceSpan& candidate : verify)
        {
            if (candidate.startNs <= header.timelineStartNs)
                span = &candidate;
        }
        if (span == nullptr || header.format.sampleType != AudioFormat::SampleType::Int16 ||
            header.format.sampleRate != span->rate)
        {
            if (fault.empty())
                fault = "a bitperfect packet at " + std::to_string(header.timelineStartNs) +
                        " ns is " + describeFormat(header.format);
            return;
        }

        // A new source starts its generator again; a jump is re-read from the
        // timeline, which is exact on the 20 ms grid at every rate used here.
        if (header.timelineStartNs == span->startNs)
            expectedFrame = span->firstFrame;
        else if (reanchor)
        {
            const auto frame = framesFromNanoseconds(header.timelineStartNs - span->startNs,
                                                     span->rate);
            expectedFrame = span->firstFrame + (frame ? *frame : 0);
        }

        const auto* bytes = reinterpret_cast<const uint8_t*>(packet.payload);
        size_t at = 0;
        for (uint32_t frame = 0; frame < header.outputFrameCount; ++frame)
        {
            for (uint16_t channel = 0; channel < header.format.channels; ++channel)
            {
                const auto actual = static_cast<int16_t>(
                    static_cast<uint16_t>(bytes[at] | (bytes[at + 1] << 8)));
                at += 2;
                if (actual != sampleAt(expectedFrame + frame, channel) && fault.empty())
                    fault = "sample " + std::to_string(expectedFrame + frame) + " of the source at " +
                            std::to_string(span->startNs) + " ns is wrong";
            }
        }
        expectedFrame += header.outputFrameCount;
        samplesVerified += static_cast<uint64_t>(header.outputFrameCount) * header.format.channels;
    }
};

class WireSink final : public IPacketSink
{
public:
    explicit WireSink(std::shared_ptr<Wire> wire) : wire_(std::move(wire)) {}

    Result<size_t, PacketSinkError> write(std::span<const std::byte> bytes) override
    {
        if (!wire_->accepting.load())
        {
            // A receive window that is shut: nothing taken, and not a failure.
            wire_->refusals.fetch_add(1);
            return size_t{0};
        }

        {
            std::lock_guard<std::mutex> lock(wire_->mutex);
            wire_->reader.append(bytes.data(), bytes.size());
            for (;;)
            {
                matp::AudioPacket packet;
                std::string error;
                const auto result = wire_->reader.next(packet, error);
                if (result == matp::AudioPacketReader::Result::Packet)
                {
                    wire_->take(packet);
                    continue;
                }
                if (result == matp::AudioPacketReader::Result::Invalid && wire_->fault.empty())
                    wire_->fault = "the client's reader refused the stream: " + error;
                break;
            }
        }
        wire_->arrived.notify_all();
        return bytes.size();
    }

    void close() override {}

private:
    std::shared_ptr<Wire> wire_;
};

// --- what the zones decided -------------------------------------------------

struct CommandLog
{
    std::mutex mutex;
    std::vector<RoomCommand> sent;

    std::vector<RoomCommand> since(size_t& cursor)
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<RoomCommand> fresh(sent.begin() + static_cast<std::ptrdiff_t>(cursor),
                                       sent.end());
        cursor = sent.size();
        return fresh;
    }

    size_t count(RoomCommandKind kind, const ClientId& client)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<size_t>(std::count_if(sent.begin(), sent.end(), [&](const auto& c) {
            return c.kind == kind && c.client == client;
        }));
    }

    std::vector<RoomCommand> of(RoomCommandKind kind)
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<RoomCommand> matching;
        for (const RoomCommand& command : sent)
        {
            if (command.kind == kind)
                matching.push_back(command);
        }
        return matching;
    }
};

// Section 8's model, fitted so that server time is client time minus
// kClientClockAheadNs. Twenty exchanges a fifth of a second apart, a one
// millisecond round trip, and no scale -- which is what a real room has for
// its first minute.
ClientClockModel clockModel()
{
    ClientClockModel model;
    for (uint64_t i = 0; i < 20; ++i)
    {
        matp::ClockSyncResult result;
        result.clientSendNs = kClientClockAheadNs + 10ULL * kSecond + i * 200ULL * kMs;
        result.serverReceiveNs = result.clientSendNs - kClientClockAheadNs + kMs / 2;
        result.serverSendNs = result.serverReceiveNs;
        result.clientReceiveNs = result.clientSendNs + kMs;
        CHECK(model.addSample(result));
    }
    return model;
}

// Everything the zones of one house share: one clock, one audio memory
// ceiling -- `blockCount` blocks of the largest format -- one planner, one
// decoder factory, one mailbox.
struct House
{
    House(uint32_t packetDurationMs, size_t blockCount, uint32_t maxRate = 96000,
          uint16_t maxChannels = 2)
        : packetMs(packetDurationMs)
    {
        const size_t blockBytes = blockBytesFor(maxRate, maxChannels, packetDurationMs);
        memory = std::make_unique<AudioMemory>(blockBytes, blockCount * blockBytes);

        CHECK_OK(resamplers.add(std::make_unique<FakeResamplerFactory>()));
        auto built = OutputPlanner::create(resamplers, "fake");
        CHECK_OK(built);
        planner.emplace(std::move(*built));

        clock.setNs(100ULL * kSecond);
    }

    // Section 46's defaults unless a test says otherwise, with the one packet
    // duration this house was built for.
    ZoneSettings settings(uint64_t targetNs = 1500ULL * kMs,
                          uint64_t retentionNs = 5ULL * kSecond) const
    {
        ZoneSettings out;
        out.output.packetDurationMs = packetMs;
        out.output.targetBufferNs = targetNs;
        out.retentionNs = retentionNs;
        out.sendCommand = [log = commands](const RoomCommand& command) {
            std::lock_guard<std::mutex> lock(log->mutex);
            log->sent.push_back(command);
            return true;
        };
        return out;
    }

    std::unique_ptr<Zone> zone(const char* id, const ZoneSettings& zoneSettings)
    {
        auto created = Zone::create(ZoneId(id), *memory, *planner, resamplers, sources, clock,
                                    zoneSettings);
        CHECK_OK(created);
        watched.push_back(created->get());
        return std::move(*created);
    }

    // The blocks every zone held together, at their high-water mark since the
    // last reset: the memory this house actually used against its ceiling.
    void sample() { peakBlocks = std::max(peakBlocks, heldBlocks()); }
    void resetPeak() { peakBlocks = heldBlocks(); }
    size_t heldBlocks() const { return memory->usedBytes() / blockBytes(); }
    size_t blockBytes() const { return memory->blockBytes(); }

    // Blocks a zone's decode found all out, over every zone still watched.
    uint64_t refused() const
    {
        uint64_t total = 0;
        for (const Zone* zone : watched)
            total += zone->blocksRefused();
        return total;
    }

    uint64_t packetNs() const { return static_cast<uint64_t>(packetMs) * kMs; }

    uint32_t packetMs;
    multiroomtest::ManualClock clock;
    std::unique_ptr<AudioMemory> memory;
    std::vector<const Zone*> watched; // the zones refused() counts; each outlives its entry
    ResamplerRegistry resamplers;
    std::optional<OutputPlanner> planner;
    Sources sources;
    std::shared_ptr<CommandLog> commands = std::make_shared<CommandLog>();
    ClientClockModel model = clockModel();
    size_t peakBlocks = 0;
};

// --- the simulated rooms ----------------------------------------------------

enum class Phase
{
    Waiting,  // no start yet on this connection, or a re-negotiated device
    Playing,
    Paused,   // stopped where a PAUSE_AT said, until a START_AT
};

struct Room
{
    ClientId id;
    ClientCapabilities capabilities;
    AudioFormat granted;
    SessionId session;
    std::shared_ptr<Wire> wire;

    double ppm = 0.0;     // how fast this room's card runs
    bool stalled = false; // takes no byte, ever
    bool connected = false;

    Phase phase = Phase::Waiting;
    std::optional<matp::StartAt> start;
    std::optional<uint64_t> pauseAt;

    // Section 54 as the client plays it: silence from the cut, the line running
    // on under it, and the seeked audio where the kept mapping puts it.
    std::optional<uint64_t> flushAt;
    bool flushing = false;
    uint64_t cutNs = 0;
    uint64_t resumedAtServerNs = 0;
    double audibleNs = 0.0; // the timeline position the card is playing
    double deviceFrames = 0.0;

    uint64_t underruns = 0;
    uint64_t reports = 0;
    uint64_t sessions = 0;
    std::vector<SourceSpan> verify;

    // How far out this room is, at the last report, by the zone's own measure.
    int64_t lastOffsetNs = 0;
};

// One zone and the rooms in it. A room here is a client: it is given a
// session and a connection, and from then on everything that happens to it is
// what it reports and what the zone sends it.
class Sim
{
public:
    Sim(House& house, Zone& zone, ZoneSettings settings, ZoneManager* manager = nullptr,
        ZoneId zoneId = {})
        : house_(house), zone_(&zone), settings_(std::move(settings)), manager_(manager),
          zoneId_(std::move(zoneId))
    {
    }

    // `verify` names the sources a bitperfect room's samples are checked
    // against; it is handed to each connection before its first byte.
    Room& add(const char* id, std::vector<AudioFormat> offers, double ppm = 0.0,
              std::vector<SourceSpan> verify = {}, bool stalled = false)
    {
        auto room = std::make_unique<Room>();
        room->id = ClientId(id);
        room->capabilities = offering(std::move(offers));
        room->ppm = ppm;
        room->verify = std::move(verify);
        room->stalled = stalled;
        rooms_.push_back(std::move(room));
        connect(*rooms_.back());
        return *rooms_.back();
    }

    // The handshake a real session makes: SESSION_CONFIG announces what the
    // zone plans, the device grants it, STREAM_READY admits the room, and the
    // audio connection attaches.
    void connect(Room& room)
    {
        room.session = sessionOf(sessionCounter_.fetch_add(1) + 1);
        room.wire = std::make_shared<Wire>();
        room.wire->verify = room.verify;
        room.wire->accepting.store(!room.stalled);
        room.phase = Phase::Waiting;
        room.start.reset();
        room.pauseAt.reset();
        room.flushAt.reset();
        room.flushing = false;
        room.deviceFrames = 0.0;
        ++room.sessions;

        const Result<OutputPlan, ZoneError> plan = zone_->planFor(room.id, room.capabilities);
        CHECK_OK(plan);
        room.granted = plan->outputFormat;

        if (manager_ != nullptr)
        {
            CHECK_OK(manager_->assign(zoneId_, room.session, room.id, room.capabilities,
                                      room.granted));
        }
        else
        {
            CHECK_OK(zone_->admit(room.session, room.id, room.capabilities, room.granted));
        }

        StreamAttachment attachment;
        attachment.session = room.session;
        attachment.client = room.id;
        CHECK(zone_->attach(attachment, std::make_unique<WireSink>(room.wire)));
        room.connected = true;
    }

    // The cable (section 43): the control connection goes, and with it the
    // stream and the session.
    void disconnect(Room& room)
    {
        if (manager_ != nullptr)
            manager_->disconnect(room.id);
        else
            zone_->disconnect(room.id);
        room.connected = false;
    }

    // One step of the house's clock, which the caller has already advanced by
    // `stepNs`: every room plays, reports, the zone decides, and the rooms
    // that are short of audio are given the chance to catch up.
    void tick(uint64_t stepNs)
    {
        const uint64_t now = house_.clock.monotonicNowNs();
        for (const auto& owned : rooms_)
            play(*owned, now, stepNs);

        zone_->pump();
        obey();
        for (const auto& owned : rooms_)
            topUp(*owned);

        for (const ClientId& id : zone_->memberIds())
        {
            const std::shared_ptr<ClientOutput> output = zone_->member(id);
            if (output != nullptr)
                maxQueued_ = std::max(maxQueued_, output->queuedPackets());
        }
    }

    Room& room(const char* id)
    {
        for (const auto& owned : rooms_)
        {
            if (owned->id.str() == id)
                return *owned;
        }
        multiroomtest::failAt(std::string("no room ") + id, __FILE__, __LINE__);
        return *rooms_.front();
    }

    std::vector<std::unique_ptr<Room>>& rooms() { return rooms_; }
    Zone& zone() { return *zone_; }
    size_t maxQueued() const { return maxQueued_; }

    // What a room's wire says, read under its lock.
    template <typename F>
    static auto read(const Room& room, F f)
    {
        std::lock_guard<std::mutex> lock(room.wire->mutex);
        return f(*room.wire);
    }

private:
    void play(Room& room, uint64_t now, uint64_t stepNs)
    {
        if (!room.connected || room.stalled)
            return;

        const std::shared_ptr<ClientOutput> output = zone_->member(room.id);
        const double speed =
            (1.0 + room.ppm * 1e-6) / (output != nullptr ? output->driftTrim() : 1.0);

        if (room.start && now >= room.start->serverTimeNs)
        {
            // Section 20: that position becomes audible at that instant, and
            // whatever the room held before it is not played.
            room.audibleNs = static_cast<double>(room.start->timelineNs) +
                             static_cast<double>(now - room.start->serverTimeNs) * speed;
            room.deviceFrames += static_cast<double>(now - room.start->serverTimeNs) *
                                 room.granted.sampleRate * (1.0 + room.ppm * 1e-6) / 1e9;
            room.phase = Phase::Playing;
            room.start.reset();
        }
        else if (room.phase == Phase::Playing)
        {
            room.audibleNs += static_cast<double>(stepNs) * speed;
            room.deviceFrames += static_cast<double>(stepNs) * room.granted.sampleRate *
                                 (1.0 + room.ppm * 1e-6) / 1e9;
        }

        if (room.phase == Phase::Playing && room.pauseAt &&
            room.audibleNs >= static_cast<double>(*room.pauseAt))
        {
            room.audibleNs = static_cast<double>(*room.pauseAt);
            room.phase = Phase::Paused;
            room.pauseAt.reset();
        }

        bool hasContent = false;
        uint64_t firstNs = 0;
        uint64_t throughNs = 0;
        uint64_t resumedNs = 0;
        {
            std::lock_guard<std::mutex> lock(room.wire->mutex);
            hasContent = room.wire->packets > 0;
            firstNs = room.wire->firstNs;
            throughNs = room.wire->throughNs;
            resumedNs = room.wire->resumedNs;
        }

        // Section 54: at the cut the room drops what it holds and plays
        // silence, while `audibleNs` goes on counting the line it kept; the
        // seeked audio is heard the moment the line reaches where it begins.
        if (room.phase == Phase::Playing && room.flushAt &&
            room.audibleNs >= static_cast<double>(*room.flushAt))
        {
            room.flushing = true;
            room.cutNs = *room.flushAt;
            room.flushAt.reset();
        }
        if (room.flushing && resumedNs > room.cutNs &&
            room.audibleNs >= static_cast<double>(resumedNs))
        {
            room.flushing = false;
            room.resumedAtServerNs = now;
        }

        // A card that ran out of audio plays silence and stays where the
        // audio stopped; a zone that has decoded its last source is the one
        // place that is expected rather than a fault.
        if (room.phase == Phase::Playing && !room.flushing &&
            room.audibleNs > static_cast<double>(throughNs))
        {
            if (zone_->state() != ZoneState::Ended)
                ++room.underruns;
            room.audibleNs = static_cast<double>(throughNs);
        }

        // What the room holds begins where its card is, or where its
        // connection began if that is later -- a new connection, or a room
        // that has not been started.
        const uint64_t audible = static_cast<uint64_t>(room.audibleNs);
        const uint64_t heldFrom = std::max(audible, hasContent ? firstNs : audible);

        // A flushed room holds nothing until the seeked audio arrives: what it
        // had was dropped at the cut.
        if (room.flushing && (resumedNs == 0 || resumedNs < room.cutNs))
            hasContent = false;

        matp::BufferStatus status;
        status.hasContent = hasContent && throughNs > heldFrom;
        if (status.hasContent)
        {
            status.playheadTimelineNs = heldFrom;
            status.bufferedUntilTimelineNs = throughNs;
            status.bufferedDurationNs = throughNs - heldFrom;
        }
        zone_->onBufferStatus(room.id, status, 20);

        if (room.phase != Phase::Playing)
            return;

        matp::PlaybackReport report;
        report.clockQuality = AudioClockQuality::Hardware;
        report.localMonotonicTimeNs = now + kClientClockAheadNs;
        report.deviceFramePosition = static_cast<uint64_t>(room.deviceFrames + 0.5);
        report.hasTimelineNsAudible = true;
        report.timelineNsAudible =
            room.flushing ? room.cutNs : static_cast<uint64_t>(room.audibleNs);
        report.bufferedDurationNs = status.bufferedDurationNs;
        zone_->onPlaybackReport(room.id, report, house_.model);
        ++room.reports;
        room.lastOffsetNs = zone_->syncStatus(room.id).offsetNs;
    }

    // What the zone sent, acted on the way the client acts on it.
    void obey()
    {
        for (const RoomCommand& command : house_.commands->since(cursor_))
        {
            Room* room = find(command.client);
            if (room == nullptr || !room->connected)
                continue;

            switch (command.kind)
            {
            case RoomCommandKind::Start:
                room->start = command.start;
                break;
            case RoomCommandKind::Pause:
                room->pauseAt = command.at.timelineNs;
                break;
            case RoomCommandKind::Flush:
                room->flushAt = command.at.timelineNs;
                break;
            case RoomCommandKind::Renegotiate: {
                // Section 12's second pair: the session asks the zone what to
                // announce, and the device reopens in it.
                const Result<OutputPlan, ZoneError> plan =
                    zone_->planFor(room->id, room->capabilities);
                CHECK_OK(plan);
                room->granted = plan->outputFormat;
                room->phase = Phase::Waiting;
                room->pauseAt.reset();
                room->deviceFrames = 0.0;
                zone_->onStreamReady(room->id, true, room->granted);
                break;
            }
            default:
                break;
            }
        }
    }

    Room* find(const ClientId& id)
    {
        for (const auto& owned : rooms_)
        {
            if (owned->id == id)
                return owned.get();
        }
        return nullptr;
    }

    // A room short of audio waits for its own thread to send it some -- on
    // the packets arriving, not on a sleep -- and only as far as the zone has
    // decoded. A paused room is holding exactly what it should.
    void topUp(Room& room)
    {
        if (!room.connected || room.stalled || room.phase == Phase::Paused)
            return;

        // Section 54: before its cut a room plays out what it holds and takes
        // nothing new -- the client parks the connection until the cut.
        if (room.flushAt)
            return;
        const std::shared_ptr<ClientOutput> output = zone_->member(room.id);
        if (output == nullptr || output->state() != OutputState::Streaming)
            return;

        const uint64_t target = settings_.output.targetBufferNs;
        const uint64_t margin = 2 * house_.packetNs();
        const uint64_t frontier = std::to_underlying(zone_->frontierNs());
        const uint64_t audible = static_cast<uint64_t>(room.audibleNs);

        // A room told to pause at a boundary is being held there by its own
        // stream, and nothing past it is coming until it re-negotiates.
        const uint64_t ceiling = room.pauseAt ? *room.pauseAt : UINT64_MAX;

        std::unique_lock<std::mutex> lock(room.wire->mutex);
        const auto enough = [&] {
            const Wire& wire = *room.wire;
            if (wire.packets == 0)
                return frontier < margin;

            // After the cut it holds only the seeked audio, from where it
            // begins, and nothing until that has arrived.
            if (room.flushing)
            {
                if (wire.resumedNs == 0 || wire.resumedNs < room.cutNs)
                    return false;
                return wire.throughNs >= std::min(wire.resumedNs + target / 2,
                                                  frontier > margin ? frontier - margin : 0);
            }
            const uint64_t from = std::max(wire.firstNs, audible);
            const uint64_t want = std::min(
                {from + target / 2, frontier > margin ? frontier - margin : 0, ceiling});
            return wire.throughNs >= want;
        };
        if (enough())
            return;

        const auto until = std::chrono::steady_clock::now() + kDeadline;
        if (!room.wire->arrived.wait_until(lock, until, enough))
            multiroomtest::failAt("room " + room.id.str() + " was not topped up: holds through " +
                                std::to_string(room.wire->throughNs) + " ns, frontier " +
                                std::to_string(frontier) + " ns",
                            __FILE__, __LINE__);
    }

    House& house_;
    Zone* zone_;
    ZoneSettings settings_;
    ZoneManager* manager_;
    ZoneId zoneId_;
    std::vector<std::unique_ptr<Room>> rooms_;
    size_t cursor_ = 0;
    size_t maxQueued_ = 0;

    // Across every zone of a test, so that no two connections share a session.
    static inline std::atomic<uint64_t> sessionCounter_{0};
};

// Runs every zone of a house for `durationNs` in steps of `stepNs`.
void run(House& house, const std::vector<Sim*>& sims, uint64_t durationNs, uint64_t stepNs)
{
    for (uint64_t elapsed = 0; elapsed < durationNs; elapsed += stepNs)
    {
        house.clock.advanceNs(stepNs);
        for (Sim* sim : sims)
            sim->tick(stepNs);
        house.sample();
    }
}

// The checks every room's stream has to pass whatever the test: nothing the
// client would refuse, no hole in what it played.
void checkClean(const Room& room)
{
    Sim::read(room, [&](const Wire& wire) {
        if (!wire.fault.empty())
            multiroomtest::failAt("room " + room.id.str() + ": " + wire.fault, __FILE__, __LINE__);
        return 0;
    });
    CHECK_EQ(room.underruns, 0u);
}

uint64_t jumpsOf(const Room& room)
{
    return Sim::read(room, [](const Wire& wire) { return wire.jumps; });
}

uint64_t packetsOf(const Room& room)
{
    return Sim::read(room, [](const Wire& wire) { return wire.packets; });
}

// How many blocks one zone can have out at once: what the store retains --
// the retention, rounded up to whole blocks, plus the one being written -- and
// one more for each room reading a block the store has already let go of.
size_t blockCeiling(const House& house, const ZoneSettings& settings, AudioFormat source,
                    size_t rooms)
{
    const uint64_t blockFrames = house.blockBytes() / source.bytesPerFrame();
    const uint64_t blockNs = blockFrames * kSecond / source.sampleRate;
    return static_cast<size_t>((settings.retentionNs + blockNs - 1) / blockNs) + 2 + rooms;
}

void measure(const char* what, const std::string& value)
{
    std::printf("[measure] %s: %s\n", what, value.c_str());
}

std::string megabytes(size_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f MB", static_cast<double>(bytes) / 1e6);
    return text;
}

} // namespace

// ---------------------------------------------------------------------------

MULTIROOM_TEST(one_zone_and_one_client_plays_a_whole_source_paced_by_its_own_reports)
{
    // The smallest house there is, run end to end at section 46's defaults:
    // a client that holds 1.5 s, plays in real time from the instant it was
    // given, and is sent only what its reports say it has room for.
    House house(20, 256);
    const AudioFormat source = formatOf(44100);
    house.sources.add("track", source, 44100ULL * 60);

    const ZoneSettings settings = house.settings();
    auto zone = house.zone("living", settings);
    CHECK_OK(zone->stage(SourceSegment{"1", "track", "", ""}));
    CHECK_OK(zone->play());

    Sim sim(house, *zone, settings);
    Room& room = sim.add("living", {source}, 0.0, {{0, 44100}});

    run(house, {&sim}, 70ULL * kSecond, 500ULL * kMs);

    CHECK(zone->state() == ZoneState::Ended);
    CHECK_EQ(zone->lastError(), std::string());
    checkClean(room);

    // One start, and every frame of the source played exactly once: 3000
    // packets of 882 frames, each sample checked against the generator.
    CHECK_EQ(house.commands->count(RoomCommandKind::Start, room.id), size_t{1});
    CHECK_EQ(packetsOf(room), 3000u);
    CHECK_EQ(jumpsOf(room), 0u);
    CHECK_EQ(Sim::read(room, [](const Wire& w) { return w.samplesVerified; }),
             44100ULL * 60 * 2);
    CHECK_EQ(house.sources.opens("track"), 1u);
    CHECK_EQ(house.sources.framesDecoded.load(), 44100ULL * 60);

    // Paced by its reports: the queue here never came near its limit.
    CHECK(sim.maxQueued() < settings.output.maxQueuedPackets);

    CHECK_EQ(house.refused(), 0u);
    CHECK(house.peakBlocks <= blockCeiling(house, settings, source, 1));
    measure("one zone, one room, 44.1 kHz 2ch int16, section 46 defaults",
            std::to_string(house.peakBlocks) + " blocks peak = " +
                megabytes(house.peakBlocks * house.blockBytes()));
}

MULTIROOM_TEST(one_zone_and_ten_clients_in_three_formats_hear_one_timeline)
{
    // Ten rooms is the roadmap's house: five that can play the source
    // exactly, three resampled to 48 kHz and two to 96 kHz float, all on one
    // decode and one START_AT -- mixed bitperfect and resampled rooms on one
    // timeline.
    House house(20, 512);
    const AudioFormat source = formatOf(44100);
    house.sources.add("track", source, 44100ULL * 40);

    const ZoneSettings settings = house.settings();
    auto zone = house.zone("house", settings);
    CHECK_OK(zone->stage(SourceSegment{"1", "track", "", ""}));
    CHECK_OK(zone->play());

    const AudioFormat at48 = formatOf(48000, 2, AudioFormat::SampleType::Int32);
    const AudioFormat at96 = formatOf(96000, 2, AudioFormat::SampleType::Float32);
    const char* bitperfect[] = {"b1", "b2", "b3", "b4", "b5"};
    const char* resampled48[] = {"r1", "r2", "r3"};
    const char* resampled96[] = {"f1", "f2"};

    Sim sim(house, *zone, settings);
    for (const char* id : bitperfect)
        sim.add(id, {at48, source}, 0.0, {{0, 44100}});
    for (const char* id : resampled48)
        sim.add(id, {at48});
    for (const char* id : resampled96)
        sim.add(id, {at96});

    run(house, {&sim}, 30ULL * kSecond, 500ULL * kMs);

    CHECK_EQ(zone->memberCount(), size_t{10});
    CHECK_EQ(house.sources.opens("track"), 1u);
    CHECK(house.sources.framesDecoded.load() <= 44100ULL * 40);

    // One START_AT for the whole house: the same position and the same
    // instant in all ten.
    const std::vector<RoomCommand> starts = house.commands->of(RoomCommandKind::Start);
    CHECK_EQ(starts.size(), size_t{10});
    for (const RoomCommand& start : starts)
    {
        CHECK_EQ(start.start.timelineNs, starts.front().start.timelineNs);
        CHECK_EQ(start.start.serverTimeNs, starts.front().start.serverTimeNs);
    }

    // Every room played for the whole run without a gap, in its own format,
    // on the same grid; and every room is where the line says it should be.
    for (const auto& room : sim.rooms())
    {
        checkClean(*room);
        CHECK_EQ(jumpsOf(*room), 0u);
        CHECK(packetsOf(*room) >= 1400u);
        CHECK(room->phase == Phase::Playing);
        CHECK(std::llabs(room->lastOffsetNs) < 1000);
    }
    CHECK(sim.room("b1").granted == source);
    CHECK(zone->member(ClientId("b1"))->plan().mode == OutputMode::BitPerfect);
    CHECK(zone->member(ClientId("r1"))->plan().mode == OutputMode::Resampled);
    CHECK(zone->member(ClientId("f1"))->plan().mode == OutputMode::Resampled);
    CHECK_EQ(Sim::read(sim.room("f2"), [](const Wire& w) { return w.formats.size(); }),
             size_t{1});
    CHECK_EQ(describeFormat(Sim::read(sim.room("f2"),
                                      [](const Wire& w) { return w.formats.front(); })),
             describeFormat(at96));

    CHECK_EQ(house.refused(), 0u);
    CHECK(house.peakBlocks <= blockCeiling(house, settings, source, 10));
    CHECK(sim.maxQueued() < settings.output.maxQueuedPackets);
    measure("one zone, ten rooms, 44.1 kHz source",
            std::to_string(house.peakBlocks) + " blocks peak = " +
                megabytes(house.peakBlocks * house.blockBytes()));
}

MULTIROOM_TEST(several_zones_play_at_once_under_one_ceiling_and_removing_one_touches_no_other)
{
    // Criterion 1: three zones, three sources at three rates, started at three
    // different moments through the one ZoneManager the listeners route by,
    // each with its own blocks under the one ceiling of the process.
    House house(20, 1024);
    house.sources.add("a", formatOf(44100), 44100ULL * 60);
    house.sources.add("b", formatOf(48000), 48000ULL * 60);
    house.sources.add("c", formatOf(96000), 96000ULL * 60);

    const ZoneSettings settings = house.settings();
    ZoneManager manager(*house.memory, *house.planner, house.resamplers, house.sources,
                        house.clock, settings);

    struct Planned
    {
        const char* zone;
        const char* uri;
        AudioFormat format;
    };
    const Planned planned[] = {{"living", "a", formatOf(44100)},
                               {"kitchen", "b", formatOf(48000)},
                               {"study", "c", formatOf(96000)}};

    std::vector<std::shared_ptr<Zone>> zones;
    std::vector<std::unique_ptr<Sim>> sims;
    std::vector<Sim*> running;
    for (const Planned& p : planned)
    {
        auto created = manager.create(ZoneId(p.zone));
        CHECK_OK(created);
        CHECK_OK((*created)->stage(SourceSegment{"1", p.uri, "", ""}));
        CHECK_OK((*created)->play());
        zones.push_back(*created);
        house.watched.push_back(created->get());

        sims.push_back(std::make_unique<Sim>(house, **created, settings, &manager,
                                             ZoneId(p.zone)));
        sims.back()->add((std::string(p.zone) + "-exact").c_str(), {p.format}, 0.0,
                         {{0, p.format.sampleRate}});
        sims.back()->add((std::string(p.zone) + "-converted").c_str(),
                         {formatOf(88200, 2, AudioFormat::SampleType::Int32)});
        running.push_back(sims.back().get());

        // A second apart, so that no two zones share a start.
        run(house, running, kSecond, 500ULL * kMs);
    }
    CHECK_EQ(manager.size(), size_t{3});

    run(house, running, 20ULL * kSecond, 500ULL * kMs);

    // Three lines, three instants, and each room on its own.
    const std::vector<RoomCommand> starts = house.commands->of(RoomCommandKind::Start);
    CHECK_EQ(starts.size(), size_t{6});
    CHECK(manager.find(ZoneId("living"))->startServerTimeNs() !=
          manager.find(ZoneId("kitchen"))->startServerTimeNs());
    CHECK(manager.zoneOf(ClientId("study-exact")) == manager.find(ZoneId("study")));

    const uint64_t livingBefore = packetsOf(sims[0]->room("living-exact"));
    const uint64_t studyBefore = packetsOf(sims[2]->room("study-exact"));

    // The kitchen goes. Its rooms stop; the other two zones do not notice.
    std::erase(house.watched, zones[1].get());
    zones[1].reset();
    CHECK(manager.remove(ZoneId("kitchen")));
    CHECK(manager.zoneOf(ClientId("kitchen-exact")) == nullptr);
    run(house, {sims[0].get(), sims[2].get()}, 20ULL * kSecond, 500ULL * kMs);

    for (Sim* sim : {sims[0].get(), sims[2].get()})
    {
        for (const auto& room : sim->rooms())
        {
            checkClean(*room);
            CHECK_EQ(jumpsOf(*room), 0u);
        }
    }
    CHECK(packetsOf(sims[0]->room("living-exact")) >= livingBefore + 950);
    CHECK(packetsOf(sims[2]->room("study-exact")) >= studyBefore + 950);

    // One decode per zone, each opened once.
    CHECK_EQ(house.sources.opens("a"), 1u);
    CHECK_EQ(house.sources.opens("b"), 1u);
    CHECK_EQ(house.sources.opens("c"), 1u);

    CHECK_EQ(house.refused(), 0u);
    const size_t ceiling = blockCeiling(house, settings, formatOf(44100), 2) +
                           blockCeiling(house, settings, formatOf(48000), 2) +
                           blockCeiling(house, settings, formatOf(96000), 2);
    CHECK(house.peakBlocks <= ceiling);
    measure("three zones (44.1, 48, 96 kHz 2ch int16), two rooms each",
            std::to_string(house.peakBlocks) + " blocks peak = " +
                megabytes(house.peakBlocks * house.blockBytes()));
}

MULTIROOM_TEST(sources_at_44_1_48_and_96_khz_in_one_staged_sequence)
{
    // Section 24 twice in a row. The room resampled into a fixed 96 kHz plays
    // straight through both boundaries; the bitperfect room's device is
    // reopened at each, paused on the boundary, re-negotiated and placed on
    // the running line again -- and every sample it is sent, in three
    // formats, is the generator's.
    House house(20, 512);
    house.sources.add("a", formatOf(44100), 44100ULL * 4);
    house.sources.add("b", formatOf(48000), 48000ULL * 4);
    house.sources.add("c", formatOf(96000), 96000ULL * 4);

    // A prebuffer long enough that a reopened room still holds the position
    // the running line places it at, a start delay later.
    ZoneSettings settings = house.settings(3ULL * kSecond, 10ULL * kSecond);
    auto zone = house.zone("living", settings);
    CHECK_OK(zone->stage(SourceSegment{"1", "a", "", ""}));
    CHECK_OK(zone->stage(SourceSegment{"2", "b", "", ""}));
    CHECK_OK(zone->stage(SourceSegment{"3", "c", "", ""}));
    CHECK_OK(zone->play());

    const AudioFormat fixed = formatOf(96000, 2, AudioFormat::SampleType::Int32);
    Sim sim(house, *zone, settings);
    Room& through = sim.add("kitchen", {fixed});
    Room& reopen = sim.add("living", {formatOf(44100), formatOf(48000), formatOf(96000)}, 0.0,
                           {{0, 44100}, {4 * kSecond, 48000}, {8 * kSecond, 96000}});

    run(house, {&sim}, 16ULL * kSecond, 250ULL * kMs);

    // The timeline is one line across three rates: each source begins where
    // the last one ended, to the nanosecond.
    const std::vector<StagedSource> sources = zone->sources();
    CHECK_EQ(sources.size(), size_t{3});
    CHECK_EQ(std::to_underlying(sources[1].timelineStartNs), 4 * kSecond);
    CHECK_EQ(std::to_underlying(sources[2].timelineStartNs), 8 * kSecond);

    const std::shared_ptr<ClientOutput> fixedOutput = zone->member(through.id);
    const std::shared_ptr<ClientOutput> reopenOutput = zone->member(reopen.id);
    CHECK(fixedOutput != nullptr);
    CHECK(reopenOutput != nullptr);

    // The fixed-rate room: one start, no pause, no re-negotiation, one format
    // on the wire, and two converters swapped at the two boundaries.
    CHECK_EQ(house.commands->count(RoomCommandKind::Start, through.id), size_t{1});
    CHECK_EQ(house.commands->count(RoomCommandKind::Pause, through.id), size_t{0});
    CHECK_EQ(house.commands->count(RoomCommandKind::Renegotiate, through.id), size_t{0});
    CHECK_EQ(fixedOutput->formatSwitches(), 2u);
    CHECK_EQ(Sim::read(through, [](const Wire& w) { return w.formats.size(); }), size_t{1});

    // The bitperfect room: paused and reopened twice, started three times,
    // three formats on one connection with the numbering unbroken and no
    // jump on the timeline.
    CHECK_EQ(house.commands->count(RoomCommandKind::Pause, reopen.id), size_t{2});
    CHECK_EQ(house.commands->count(RoomCommandKind::Renegotiate, reopen.id), size_t{2});
    CHECK_EQ(house.commands->count(RoomCommandKind::Start, reopen.id), size_t{3});
    CHECK_EQ(reopenOutput->formatSwitches(), 2u);
    const std::vector<AudioFormat> formats =
        Sim::read(reopen, [](const Wire& w) { return w.formats; });
    CHECK_EQ(formats.size(), size_t{3});
    CHECK_EQ(formats[0].sampleRate, 44100u);
    CHECK_EQ(formats[1].sampleRate, 48000u);
    CHECK_EQ(formats[2].sampleRate, 96000u);
    CHECK(Sim::read(reopen, [](const Wire& w) { return w.samplesVerified; }) >
          (44100ULL * 4 + 48000ULL * 4) * 2);

    for (const Room* room : {&through, &reopen})
    {
        checkClean(*room);
        CHECK_EQ(jumpsOf(*room), 0u);
    }
    CHECK_EQ(zone->rejoinCount(), 0u);
    CHECK_EQ(house.refused(), 0u);
    // The retention is a duration, and a block of the 96 kHz source covers
    // the least of it, so that is the source the ceiling is counted in.
    CHECK(house.peakBlocks <= blockCeiling(house, settings, formatOf(96000), 2));
}

MULTIROOM_TEST(a_reconnect_storm_leaves_the_steady_room_and_the_zone_untouched)
{
    // Five rooms losing their control connection over and over, in
    // overlapping waves, while a sixth never does. Every return is the
    // ordinary admit() with the room's state restored; the zone neither fails
    // nor restarts, the steady room's stream never breaks, and nothing any of
    // it does is allowed to grow.
    House house(20, 512);
    const AudioFormat source = formatOf(44100);
    house.sources.add("track", source, 44100ULL * 600);

    const ZoneSettings settings = house.settings();
    auto zone = house.zone("living", settings);
    CHECK_OK(zone->stage(SourceSegment{"1", "track", "", ""}));
    CHECK_OK(zone->play());

    const AudioFormat at48 = formatOf(48000, 2, AudioFormat::SampleType::Int32);
    Sim sim(house, *zone, settings);
    Room& steady = sim.add("steady", {source}, 0.0, {{0, 44100}});
    const char* stormy[] = {"s1", "s2", "s3", "s4", "s5"};
    for (size_t i = 0; i < std::size(stormy); ++i)
        sim.add(stormy[i], {i % 2 == 0 ? source : at48});

    run(house, {&sim}, 5ULL * kSecond, 500ULL * kMs);
    CHECK(zone->started());
    house.resetPeak();

    constexpr int kWaves = 20;
    uint64_t cuts = 0;
    for (int wave = 0; wave < kWaves; ++wave)
    {
        // Three of the five go, a different three each wave.
        std::vector<Room*> going;
        for (int k = 0; k < 3; ++k)
            going.push_back(&sim.room(stormy[(wave + k * 2) % 5]));
        for (Room* room : going)
            sim.disconnect(*room);
        run(house, {&sim}, 1ULL * kSecond, 500ULL * kMs);

        for (Room* room : going)
        {
            sim.connect(*room);
            ++cuts;
        }
        run(house, {&sim}, 5ULL * kSecond, 500ULL * kMs);
    }

    CHECK(zone->state() == ZoneState::Playing);
    CHECK_EQ(zone->lastError(), std::string());
    CHECK_EQ(zone->memberCount(), size_t{6});
    CHECK_EQ(zone->rejoinCount(), cuts);

    // The room that never lost anything: one start, one unbroken stream,
    // every sample right, no underrun.
    checkClean(steady);
    CHECK_EQ(jumpsOf(steady), 0u);
    CHECK_EQ(house.commands->count(RoomCommandKind::Start, steady.id), size_t{1});
    CHECK_EQ(zone->rejoins(steady.id), 0u);

    // Each stormy room is on its latest connection, playing, with a clean
    // stream on it -- and was started again after each return.
    for (const char* id : stormy)
    {
        const Room& room = sim.room(id);
        checkClean(room);
        CHECK_EQ(jumpsOf(room), 0u);
        CHECK(room.phase == Phase::Playing);
        CHECK_EQ(zone->rejoins(room.id), room.sessions - 1);
        CHECK_EQ(house.commands->count(RoomCommandKind::Start, room.id), room.sessions);
    }

    // Twelve waves of cuts, and the memory the zone used did not grow with
    // them.
    CHECK_EQ(house.refused(), 0u);
    CHECK(house.peakBlocks <= blockCeiling(house, settings, source, 6));
    CHECK(sim.maxQueued() <= settings.output.maxQueuedPackets);
    measure("reconnect storm", std::to_string(cuts) + " reconnects over " +
                                   std::to_string(kWaves) + " waves, " +
                                   std::to_string(house.peakBlocks) + " blocks peak");
}

MULTIROOM_TEST(one_permanently_stalled_client_blocks_neither_its_zone_nor_its_neighbours)
{
    // A room that opens its audio connection and never reads a byte, for five
    // minutes. It is admitted like any other, so the barrier waits for it --
    // for the ready timeout and no longer -- and from then on it is a cursor
    // the store evicts and a queue that is full: nothing it does reaches the
    // decode or the two rooms beside it.
    House house(20, 512);
    const AudioFormat source = formatOf(44100);
    house.sources.add("track", source, 44100ULL * 600);

    const ZoneSettings settings = house.settings();
    auto zone = house.zone("living", settings);
    CHECK_OK(zone->stage(SourceSegment{"1", "track", "", ""}));
    CHECK_OK(zone->play());

    Sim sim(house, *zone, settings);
    Room& a = sim.add("a", {source}, 0.0, {{0, 44100}});
    Room& b = sim.add("b", {formatOf(48000, 2, AudioFormat::SampleType::Int32)});
    Room& stalled = sim.add("stalled", {source}, 0.0, {}, true);

    run(house, {&sim}, 10ULL * kSecond, 500ULL * kMs);
    CHECK(zone->started());
    house.resetPeak();
    run(house, {&sim}, 5ULL * kMinute, 500ULL * kMs);

    // The two rooms that read played the whole time, in step with the line.
    for (const Room* room : {&a, &b})
    {
        checkClean(*room);
        CHECK_EQ(jumpsOf(*room), 0u);
        CHECK(packetsOf(*room) >= 15000u);
        CHECK(std::llabs(room->lastOffsetNs) < 1000);
    }
    CHECK_EQ(zone->rejoins(a.id), 0u);
    CHECK_EQ(zone->rejoins(b.id), 0u);

    // The stalled room: never started, never sent a byte, its connection let
    // go on its own stall timeout, and its queue never past its limit.
    CHECK_EQ(house.commands->count(RoomCommandKind::Start, stalled.id), size_t{0});
    CHECK_EQ(packetsOf(stalled), 0u);
    CHECK(stalled.wire->refusals.load() > 0);
    const std::shared_ptr<ClientOutput> stuck = zone->member(stalled.id);
    CHECK(stuck != nullptr);
    CHECK(stuck->connectionDrops() >= 1);
    CHECK(sim.maxQueued() <= settings.output.maxQueuedPackets);

    // And the zone: still decoding, its frontier where the readers need it.
    CHECK(zone->state() == ZoneState::Playing);
    CHECK(std::to_underlying(zone->frontierNs()) > std::to_underlying(zone->playheadNs()));
    CHECK_EQ(house.refused(), 0u);
    CHECK(house.peakBlocks <= blockCeiling(house, settings, source, 3));
    measure("stalled room over five minutes",
            std::to_string(zone->rejoins(stalled.id)) + " rejoins of the stalled room, " +
                std::to_string(stuck->connectionDrops()) + " connection drop(s), " +
                std::to_string(house.peakBlocks) + " blocks peak");
}

MULTIROOM_TEST(drift_correction_holds_two_cards_on_the_line_for_an_hour)
{
    // Phase 13 for an hour instead of for twenty seconds. Two rooms resampled
    // and corrected, one card forty ppm fast and one twenty-five slow; and a
    // bitperfect room five ppm fast, which is left to drift on purpose. The
    // corrected rooms' offset is the zone's own measure of it, and the
    // simulated card plays whatever ratio the room was actually given -- so a
    // correction with the wrong sign, a trim that never reaches the room, or
    // a loop that hunts shows up here as an offset that walks away.
    //
    // 100 ms packets and an 8 kHz mono source, because an hour of 20 ms stereo
    // packets is 180,000 of them per room in a debug build, and neither the
    // packet duration nor the width of a frame is what is under test.
    House house(100, 512, 12000, 1);
    const AudioFormat source = formatOf(8000, 1);
    house.sources.add("hour", source, 8000ULL * 3700);

    // A 40 s prebuffer, so that the harness waits on a room's thread once
    // every twenty simulated seconds rather than once every few.
    ZoneSettings settings = house.settings(40ULL * kSecond, 120ULL * kSecond);
    auto zone = house.zone("living", settings);
    CHECK_OK(zone->stage(SourceSegment{"1", "hour", "", ""}));
    CHECK_OK(zone->play());

    Sim sim(house, *zone, settings);
    Room& fast = sim.add("fast", {formatOf(12000, 1)}, 40.0);
    Room& slow = sim.add("slow", {formatOf(12000, 1)}, -25.0);
    Room& exact = sim.add("exact", {source}, 5.0, {{0, 8000}});

    CHECK(zone->member(fast.id)->driftCorrected());
    CHECK(zone->member(slow.id)->driftCorrected());
    CHECK(!zone->member(exact.id)->driftCorrected());

    // Convergence: the first moment after which a corrected room stays inside
    // a millisecond of the line for the rest of the hour.
    std::map<std::string, uint64_t> settledAt;
    std::map<std::string, uint64_t> worstAfterTenMinutes;
    constexpr uint64_t kStep = kSecond;
    for (uint64_t elapsed = 0; elapsed < 60 * kMinute; elapsed += kStep)
    {
        house.clock.advanceNs(kStep);
        sim.tick(kStep);
        house.sample();
        if (elapsed == 5 * kMinute)
            house.resetPeak();

        for (const Room* room : {&fast, &slow})
        {
            const uint64_t off = static_cast<uint64_t>(std::llabs(room->lastOffsetNs));
            if (off >= kMs || room->phase != Phase::Playing)
                settledAt[room->id.str()] = elapsed + kStep;
            if (elapsed >= 10 * kMinute)
                worstAfterTenMinutes[room->id.str()] =
                    std::max(worstAfterTenMinutes[room->id.str()], off);
        }
    }

    for (const Room* room : {&fast, &slow})
    {
        checkClean(*room);
        CHECK_EQ(jumpsOf(*room), 0u);
        CHECK_EQ(zone->rejoins(room->id), 0u);

        // On the line for the last fifty minutes, and given the card's own
        // rate as its ratio -- to within what a 12 kHz frame counter can say
        // over a 20 s window, which is one frame in 240,000. The offset is
        // what closes the rest, and the offset is the claim.
        constexpr double kFramePpm = 1e6 / (12000.0 * 20.0);
        CHECK(worstAfterTenMinutes[room->id.str()] < kMs);
        CHECK(settledAt[room->id.str()] <= 10 * kMinute);
        CHECK_NEAR((zone->member(room->id)->driftTrim() - 1.0) * 1e6, room->ppm, kFramePpm);
        CHECK_NEAR(zone->syncStatus(room->id).driftPpm, room->ppm, kFramePpm);
    }

    // The bitperfect room was measured and left alone: its offset is its card
    // over an hour, five ppm of 3600 s.
    checkClean(exact);
    CHECK_EQ(zone->member(exact.id)->driftTrim(), 1.0);
    CHECK_EQ(zone->member(exact.id)->corrections(), 0u);
    CHECK_EQ(zone->rejoins(exact.id), 0u);
    CHECK_NEAR(static_cast<double>(exact.lastOffsetNs) / 1e6, 18.0, 1.0);

    // An hour did not grow the memory the first five minutes needed.
    CHECK_EQ(house.refused(), 0u);
    CHECK(house.peakBlocks <= blockCeiling(house, settings, source, 3));

    char text[160];
    std::snprintf(text, sizeof(text),
                  "+40 ppm settled inside 1 ms after %llu s, -25 ppm after %llu s; worst "
                  "offset after 10 min %llu us / %llu us",
                  static_cast<unsigned long long>(settledAt["fast"] / kSecond),
                  static_cast<unsigned long long>(settledAt["slow"] / kSecond),
                  static_cast<unsigned long long>(worstAfterTenMinutes["fast"] / 1000),
                  static_cast<unsigned long long>(worstAfterTenMinutes["slow"] / 1000));
    measure("drift correction convergence (simulated card, no jitter)", text);
}

MULTIROOM_TEST(a_timeline_runs_for_three_hours_exact_to_the_nanosecond_in_bounded_memory)
{
    // Three hours at 11,025 Hz, where 100 ms is 1102.5 frames: every packet
    // alternates between two frame counts and every timeline span is exactly
    // 100 ms, so a mapping that was accumulated rather than computed from the
    // origin shows up as a drifting grid long before the third hour. Every
    // sample the bitperfect room is sent is checked against the generator.
    House house(100, 1024, 11025, 1);
    const AudioFormat source = formatOf(11025, 1);
    constexpr uint64_t kHours = 3;
    house.sources.add("long", source, 11025ULL * 3600 * kHours + 11025ULL * 60);

    // Reports every five seconds and a 40 s prebuffer: nothing here corrects,
    // so nothing needs the report rate, and three hours is then a few
    // hundred waits on the rooms' threads rather than thousands.
    const ZoneSettings settings = house.settings(40ULL * kSecond, 120ULL * kSecond);
    auto zone = house.zone("living", settings);
    CHECK_OK(zone->stage(SourceSegment{"1", "long", "", ""}));
    CHECK_OK(zone->play());

    Sim sim(house, *zone, settings);
    // Two rooms, both bitperfect: the drift test above already runs resampled
    // rooms for an hour, and three hours of a converter in a debug build is
    // time spent on the fake resampler rather than on the timeline.
    Room& exact = sim.add("exact", {source}, 0.0, {{0, 11025}});
    Room& second = sim.add("second", {source});

    run(house, {&sim}, 5ULL * kMinute, 5ULL * kSecond);
    const size_t warmPeak = house.peakBlocks;
    house.resetPeak();
    run(house, {&sim}, kHours * 60 * kMinute - 5ULL * kMinute, 5ULL * kSecond);

    for (const Room* room : {&exact, &second})
    {
        checkClean(*room);
        CHECK_EQ(jumpsOf(*room), 0u);
        CHECK(std::llabs(room->lastOffsetNs) < 1000);
    }

    // The grid, after three hours: the packet the room holds last began on a
    // whole multiple of 100 ms, and the frames under it are exactly the
    // source's frames for that span -- 1102.5 per packet, with nothing lost
    // or doubled in 108,000 halves.
    const auto [packets, frames, throughNs, verified] = Sim::read(exact, [](const Wire& w) {
        return std::tuple{w.packets, w.frames, w.throughNs, w.samplesVerified};
    });
    CHECK_EQ(throughNs % (100 * kMs), 0u);
    CHECK_EQ(throughNs / (100 * kMs), packets);
    const auto framesThrough = framesFromNanoseconds(throughNs, 11025);
    CHECK_OK(framesThrough);
    CHECK_EQ(frames, *framesThrough);
    CHECK_EQ(verified, frames);
    CHECK(throughNs >= kHours * 3600 * kSecond);

    // What the playhead says is what the line says: the start pair plus the
    // server's elapsed time, computed rather than added up.
    const uint64_t now = house.clock.monotonicNowNs();
    CHECK_EQ(std::to_underlying(zone->playheadNs()),
             std::to_underlying(zone->startTimelineNs()) + (now - zone->startServerTimeNs()));

    // Memory: the three hours after the first five minutes never needed more
    // than those five minutes did -- give or take the one block each room may
    // still be reading after the store has let it go, which is the only part
    // of the count that depends on when a thread happened to run.
    CHECK_EQ(house.refused(), 0u);
    CHECK(house.peakBlocks <= warmPeak + 2);
    CHECK(house.peakBlocks <= blockCeiling(house, settings, source, 2));
    CHECK(sim.maxQueued() <= settings.output.maxQueuedPackets);
    measure("three hours at 11.025 kHz, 120 s retention",
            std::to_string(packets) + " packets to the bitperfect room, " +
                std::to_string(house.peakBlocks) + " blocks peak = " +
                megabytes(house.peakBlocks * house.blockBytes()));
}

MULTIROOM_TEST(a_zone_at_section_46_defaults_costs_a_fixed_number_of_blocks)
{
    // Memory per zone, as a number rather than an argument: one zone at the
    // deployment defaults -- 5 s retention, 1.5 s prebuffer, 20 ms packets,
    // a pool sized for 192 kHz stereo -- playing CD audio to two rooms for
    // three minutes. It prints the figure.
    House house(20, 1024, 192000, 2);
    const AudioFormat source = formatOf(44100);
    house.sources.add("track", source, 44100ULL * 240);

    const ZoneSettings settings = house.settings();
    auto zone = house.zone("living", settings);
    CHECK_OK(zone->stage(SourceSegment{"1", "track", "", ""}));
    CHECK_OK(zone->play());

    Sim sim(house, *zone, settings);
    sim.add("a", {source});
    sim.add("b", {formatOf(48000, 2, AudioFormat::SampleType::Int32)});

    run(house, {&sim}, kMinute, 500ULL * kMs);
    const size_t warmPeak = house.peakBlocks;
    house.resetPeak();
    run(house, {&sim}, 2ULL * kMinute, 500ULL * kMs);

    for (const auto& room : sim.rooms())
        checkClean(*room);
    CHECK_EQ(house.refused(), 0u);
    CHECK(house.peakBlocks <= warmPeak + 2); // one block per room, as above
    CHECK(house.peakBlocks <= blockCeiling(house, settings, source, 2));
    measure("memory per zone at section 46 defaults (44.1 kHz 2ch int16, pool for 192 kHz "
            "2ch)",
            std::to_string(house.peakBlocks) + " blocks of " +
                std::to_string(house.blockBytes()) + " bytes = " +
                megabytes(house.peakBlocks * house.blockBytes()));
}

#if MULTIROOM_WITH_SOXR || MULTIROOM_WITH_R8BRAIN

MULTIROOM_TEST(a_resampled_room_costs_a_measured_fraction_of_one_core)
{
    // CPU per resampled room: a minute of CD audio through the processor a
    // room is actually given -- 44.1 kHz int16 to 48 kHz int32, stereo -- for
    // each real converter in this build, timed on this thread. The assertion
    // is only that it keeps up with real time with room to spare; the number
    // is what gets written down, and it is only meaningful from a release
    // build.
    ResamplerRegistry registry;
    CHECK_OK(registerBuiltinResamplers(registry));

    const AudioFormat source = formatOf(44100);
    const AudioFormat device = formatOf(48000, 2, AudioFormat::SampleType::Int32);
    constexpr uint64_t kSeconds = 60;
    constexpr uint32_t kBlockFrames = 882;

    std::vector<std::byte> in(static_cast<size_t>(kBlockFrames) * source.bytesPerFrame());
    std::vector<std::byte> out(static_cast<size_t>(kBlockFrames) * 2 * device.bytesPerFrame());

    for (const std::string& id : registry.ids())
    {
        auto planner = OutputPlanner::create(registry, id);
        CHECK_OK(planner);
        for (const bool grouped : {false, true})
        {
            const RoomAudioSettings roomSettings{AudioControl::Auto, grouped};
            const auto plan = planner->plan(source, roomSettings, offering({device}));
            CHECK_OK(plan);
            auto processor = makeClientAudioProcessor(*plan, registry);
            CHECK_OK(processor);

            uint64_t consumed = 0;
            uint64_t produced = 0;
            const auto began = std::chrono::steady_clock::now();
            for (uint64_t frame = 0; frame < 44100ULL * kSeconds; frame += kBlockFrames)
            {
                size_t at = 0;
                for (uint32_t f = 0; f < kBlockFrames; ++f)
                {
                    for (uint16_t channel = 0; channel < 2; ++channel)
                    {
                        const auto bits = static_cast<uint16_t>(sampleAt(frame + f, channel));
                        in[at++] = static_cast<std::byte>(bits & 0xFF);
                        in[at++] = static_cast<std::byte>(bits >> 8);
                    }
                }
                std::span<const std::byte> rest(in);
                while (!rest.empty())
                {
                    const auto result = (*processor)->process(rest, out);
                    CHECK_OK(result);
                    consumed += result->inputFramesConsumed;
                    produced += result->outputFramesProduced;
                    rest = rest.subspan(result->inputFramesConsumed * source.bytesPerFrame());
                    if (result->inputFramesConsumed == 0 && result->outputFramesProduced == 0)
                        break;
                }
            }
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

            CHECK_EQ(consumed, 44100ULL * kSeconds);
            CHECK_NEAR(static_cast<double>(produced), 48000.0 * kSeconds, 48000.0);
            CHECK(seconds < static_cast<double>(kSeconds));

            char text[160];
            std::snprintf(text, sizeof(text),
                          "%s%s: %.1f ms of CPU per second of audio (%.0fx real time)",
                          id.c_str(), plan->resamplingCorrected() ? ", variable ratio" : "",
                          seconds * 1000.0 / static_cast<double>(kSeconds),
                          static_cast<double>(kSeconds) / seconds);
            measure("CPU per resampled room, 44.1 kHz int16 -> 48 kHz int32 stereo", text);
        }
    }
}

#endif // MULTIROOM_WITH_SOXR || MULTIROOM_WITH_R8BRAIN

MULTIROOM_TEST(a_seek_moves_two_rooms_through_silence_and_back_onto_the_same_line)
{
    // Phase 26 against rooms that play: one bitperfect and one resampled, a
    // seek forty seconds into the source, and on both the client's own
    // reading of it -- no rejoin, silence from the cut, and the seeked audio
    // heard at exactly the instant the kept mapping names, still on the line.
    House house(20, 512);
    const AudioFormat source = formatOf(44100);
    house.sources.add("track", source, 44100ULL * 60);

    const ZoneSettings settings = house.settings();
    auto zone = house.zone("living", settings);
    CHECK_OK(zone->stage(SourceSegment{"1", "track", "", ""}));
    CHECK_OK(zone->play());

    const AudioFormat at48 = formatOf(48000, 2, AudioFormat::SampleType::Int32);
    Sim sim(house, *zone, settings);
    Room& exact = sim.add("exact", {source}, 0.0, {{0, 44100}});
    Room& converted = sim.add("converted", {at48});

    constexpr uint64_t kStep = 50ULL * kMs;
    run(house, {&sim}, 10ULL * kSecond, kStep);
    CHECK(exact.phase == Phase::Playing);
    CHECK(converted.phase == Phase::Playing);

    // Where the seeked audio will be heard, handed to the bitperfect room's
    // checker before a byte of it can have been built.
    const uint64_t resumeAt = std::to_underlying(zone->playheadNs()) + 250ULL * kMs;
    {
        std::lock_guard<std::mutex> lock(exact.wire->mutex);
        exact.wire->verify.push_back({resumeAt, 44100, 44100ULL * 40});
    }
    const Result<SeekPlacement, ZoneError> placed = zone->seek(40ULL * kSecond);
    CHECK_OK(placed);
    CHECK_EQ(std::to_underlying(placed->resumeAt), resumeAt);
    const uint64_t heardAt =
        zone->startServerTimeNs() + (resumeAt - std::to_underlying(zone->startTimelineNs()));
    CHECK_EQ(house.commands->count(RoomCommandKind::Flush, converted.id), size_t{1});
    CHECK_EQ(std::to_underlying(zone->member(converted.id)->producedThroughNs()), resumeAt);
    CHECK_EQ(std::to_underlying(zone->member(exact.id)->producedThroughNs()), resumeAt);
    CHECK(zone->roomStart(converted.id) == RoomStartState::Playing);

    run(house, {&sim}, 10ULL * kSecond, kStep);

    for (Room* room : {&exact, &converted})
    {
        checkClean(*room);
        CHECK_EQ(jumpsOf(*room), 0u);
        CHECK_EQ(Sim::read(*room, [](const Wire& w) { return w.flushes; }), 1u);
        CHECK_EQ(Sim::read(*room, [](const Wire& w) { return w.resumedNs; }), resumeAt);
        CHECK(!room->flushing);
        CHECK(room->resumedAtServerNs >= heardAt);
        CHECK(room->resumedAtServerNs < heardAt + kStep);
        CHECK(std::llabs(room->lastOffsetNs) < 1000);
        CHECK_EQ(house.commands->count(RoomCommandKind::Start, room->id), size_t{1});
        CHECK_EQ(house.commands->count(RoomCommandKind::Flush, room->id), size_t{1});
        CHECK_EQ(zone->rejoins(room->id), 0u);
    }

    // Every sample of the bitperfect room checked, the seeked ones against the
    // generator from its fortieth second.
    CHECK(Sim::read(exact, [](const Wire& w) { return w.samplesVerified; }) >
          44100ULL * 19 * 2);
    CHECK_EQ(house.refused(), 0u);
}

MULTIROOM_TEST(a_room_with_an_output_latency_plays_that_far_ahead_and_is_corrected_around_it)
{
    // Phase 27 against rooms that play. A room behind a receiver is 80 ms
    // late in the room, so it is started 80 ms early and its offset is held
    // there, with its card thirty ppm fast and drift correction on; a
    // soundbar room is bitperfect with 120 ms of its own, which it gets from
    // its start alone. Built like the hour of drift correction above, and for
    // the same reasons.
    House house(100, 512, 12000, 1);
    const AudioFormat source = formatOf(8000, 1);
    house.sources.add("track", source, 8000ULL * 900);

    ZoneSettings settings = house.settings(40ULL * kSecond, 120ULL * kSecond);
    auto zone = house.zone("living", settings);
    CHECK_OK(zone->stage(SourceSegment{"1", "track", "", ""}));
    CHECK_OK(zone->play());

    Sim sim(house, *zone, settings);
    Room& plain = sim.add("plain", {formatOf(12000, 1)});
    Room& receiver = sim.add("receiver", {formatOf(12000, 1)}, 30.0);
    Room& soundbar = sim.add("soundbar", {source}, 0.0, {{0, 8000}});
    constexpr int64_t kReceiverNs = 80LL * 1000000LL;
    constexpr int64_t kSoundbarNs = 120LL * 1000000LL;
    CHECK_OK(zone->setOutputLatency(receiver.id, static_cast<uint64_t>(kReceiverNs)));
    CHECK_OK(zone->setOutputLatency(soundbar.id, static_cast<uint64_t>(kSoundbarNs)));
    CHECK(zone->member(receiver.id)->driftCorrected());
    CHECK(!zone->member(soundbar.id)->driftCorrected());

    uint64_t worstAfterTenMinutes = 0;
    constexpr uint64_t kStep = kSecond;
    for (uint64_t elapsed = 0; elapsed < 12 * kMinute; elapsed += kStep)
    {
        house.clock.advanceNs(kStep);
        sim.tick(kStep);
        house.sample();
        if (elapsed >= 10 * kMinute)
            worstAfterTenMinutes = std::max(
                worstAfterTenMinutes,
                static_cast<uint64_t>(std::llabs(receiver.lastOffsetNs - kReceiverNs)));
    }

    // One barrier: one position, and each room's instant earlier by exactly
    // its latency.
    const std::vector<RoomCommand> starts = house.commands->of(RoomCommandKind::Start);
    CHECK_EQ(starts.size(), size_t{3});
    const auto startOf = [&](const Room& room) {
        return *std::find_if(starts.begin(), starts.end(),
                             [&](const RoomCommand& c) { return c.client == room.id; });
    };
    const RoomCommand toPlain = startOf(plain);
    CHECK_EQ(startOf(receiver).start.timelineNs, toPlain.start.timelineNs);
    CHECK_EQ(startOf(soundbar).start.timelineNs, toPlain.start.timelineNs);
    CHECK_EQ(toPlain.start.serverTimeNs - startOf(receiver).start.serverTimeNs,
             static_cast<uint64_t>(kReceiverNs));
    CHECK_EQ(toPlain.start.serverTimeNs - startOf(soundbar).start.serverTimeNs,
             static_cast<uint64_t>(kSoundbarNs));

    // Held at its target, not pulled back to the line, and never moved for
    // being where it was put.
    CHECK(worstAfterTenMinutes < kMs);
    CHECK_EQ(zone->syncStatus(receiver.id).targetOffsetNs, kReceiverNs);
    constexpr double kFramePpm = 1e6 / (12000.0 * 20.0);
    CHECK_NEAR((zone->member(receiver.id)->driftTrim() - 1.0) * 1e6, 30.0, kFramePpm);
    CHECK(std::llabs(plain.lastOffsetNs) < static_cast<int64_t>(kMs));
    CHECK(std::llabs(soundbar.lastOffsetNs - kSoundbarNs) < 1000);

    for (const Room* room : {&plain, &receiver, &soundbar})
    {
        checkClean(*room);
        CHECK_EQ(jumpsOf(*room), 0u);
        CHECK_EQ(zone->rejoins(room->id), 0u);
    }

    // And the bitperfect room was sent the source's own samples throughout.
    CHECK_EQ(zone->member(soundbar.id)->driftTrim(), 1.0);
    CHECK(Sim::read(soundbar, [](const Wire& w) { return w.samplesVerified; }) >
          8000ULL * 60 * 11);
    CHECK_EQ(house.refused(), 0u);
}
