// End to end against a fake MATP v2 server: the handshake, the clock exchange,
// the audio connection and the whole transport, with no sound card anywhere.
//
// What is real here is everything the protocol touches -- matp::Session,
// matp::AudioStream, matp::TimelineMap, the ring buffer and two TCP
// connections. What is played by the test rather than by hardware is the
// sound card at one end and the server at the other, so these tests cover the
// one thing the per-class tests cannot: that the pieces agree with each other
// about which frame of this room's own stream a timeline position is.
//
// `Room` below wires the client's protocol layer together the way
// StreamPlayback does, including the conversions StreamPlayback owns (section
// 33: timeline nanoseconds inward for a schedule, this room's frames outward
// for a report). StreamPlayback itself is not used because it constructs a
// platform audio backend, and these tests must run on a machine with no
// device -- so the conversions are reproduced here, against the same
// AudioStream calls it makes.

#include "TestListener.h"
#include "TestSupport.h"

#include "matp/AudioStream.h"
#include "matp/Session.h"
#include "util/Time.h"

#include <string>
#include <vector>

using namespace multiroom;
using multiroomtest::TestListener;
using multiroomtest::waitUntil;

namespace {

AudioFormat fmt(uint32_t rate, AudioFormat::SampleType type)
{
    AudioFormat format;
    format.sampleRate = rate;
    format.channels = 2;
    format.sampleType = type;
    return format;
}

const char* sampleFormatOf(const AudioFormat& format)
{
    return matp::sampleFormatName(format.sampleType);
}

matp::Identity roomIdentity()
{
    matp::Identity identity;
    identity.clientId = "livingroom";
    identity.clientVersion = "0.1.0";
    identity.platform = "linux-armv6";
    return identity;
}

// A device that accepts all three rates, so the server's choice is the
// server's and not the device's (section 12). 24-bit packed at 44.1 and 96 is
// the bitperfect path; 48 kHz is what a resampled room gets.
matp::CapabilityReport wideCapabilities()
{
    matp::CapabilityReport report;
    report.deviceId = "device-1";
    report.deviceName = "Test DAC";
    report.preferredSampleRate = 48000;
    report.formats.push_back(fmt(44100, AudioFormat::SampleType::Int24));
    report.formats.push_back(fmt(48000, AudioFormat::SampleType::Int24));
    report.formats.push_back(fmt(96000, AudioFormat::SampleType::Int24));
    return report;
}

// Where frame `frame` of a stream at `rate` falls, measured absolutely from
// the stream's own start. Section 15's rule, so that consecutive packets are
// contiguous by construction and nothing accumulates.
uint64_t nsAt(uint64_t baseNs, uint64_t frame, uint32_t rate)
{
    return baseNs + (frame * 1000000000ULL) / rate;
}

// The server's side of one room's audio: it hands out contiguous packets and
// remembers where it has got to on both axes.
class PacketSource
{
public:
    PacketSource(const AudioFormat& format, uint64_t baseTimelineNs)
        : format_(format), baseNs_(baseTimelineNs), timelineNs_(baseTimelineNs)
    {
    }

    // A bitperfect packet: the frames are the source's own, so the span comes
    // from the difference of two absolute positions.
    std::string nextBitperfect(uint32_t frameCount)
    {
        const uint64_t startNs = nsAt(baseNs_, frameOfBase_, format_.sampleRate);
        const uint64_t endNs = nsAt(baseNs_, frameOfBase_ + frameCount, format_.sampleRate);
        frameOfBase_ += frameCount;
        return build(startNs, endNs - startNs, frameCount);
    }

    // A resampled or drift-corrected packet: the span is the server's to
    // choose and the frame count is whatever this room's ratio produced.
    std::string nextSpan(uint64_t durationNs, uint32_t frameCount)
    {
        const uint64_t startNs = timelineNs_;
        frameOfBase_ += frameCount;
        return build(startNs, durationNs, frameCount);
    }

    // A hard resync or a rejoin (sections 32 and 43): the next packet simply
    // begins somewhere else.
    void jumpTo(uint64_t timelineNs)
    {
        baseNs_ = timelineNs;
        timelineNs_ = timelineNs;
        frameOfBase_ = 0;
    }

    void setFormat(const AudioFormat& format) { format_ = format; }

    uint64_t nextTimelineNs() const { return timelineNs_; }
    uint64_t streamFrame() const { return streamFrame_; }
    uint64_t sequence() const { return sequence_; }

private:
    std::string build(uint64_t startNs, uint64_t durationNs, uint32_t frameCount)
    {
        matp::AudioPacketHeader header;
        header.protocolVersion = static_cast<uint16_t>(matp::kProtocolVersion);
        header.sessionIdHigh = 0x0123456789ABCDEFULL;
        header.sessionIdLow = 0xFEDCBA9876543210ULL;
        header.sequenceNumber = sequence_++;
        header.timelineStartNs = startNs;
        header.timelineDurationNs = durationNs;
        header.format = format_;
        header.outputFrameCount = frameCount;

        // Every byte says which frame of this room's stream it is, so audio
        // that arrived in the wrong order or in the wrong place is visible in
        // the buffer rather than only in a counter.
        const uint32_t bytesPerFrame = format_.bytesPerFrame();
        std::vector<unsigned char> payload(static_cast<size_t>(frameCount) * bytesPerFrame);
        for (uint32_t frame = 0; frame < frameCount; ++frame)
        {
            const unsigned char value = static_cast<unsigned char>(streamFrame_ + frame);
            for (uint32_t byte = 0; byte < bytesPerFrame; ++byte)
                payload[static_cast<size_t>(frame) * bytesPerFrame + byte] = value;
        }

        streamFrame_ += frameCount;
        timelineNs_ = startNs + durationNs;
        return matp::encodeAudioPacket(header, payload.data(), payload.size());
    }

    AudioFormat format_;
    uint64_t baseNs_ = 0;
    uint64_t frameOfBase_ = 0;
    uint64_t timelineNs_ = 0;
    uint64_t streamFrame_ = 0;
    uint64_t sequence_ = 0;
};

// The client, as far as the protocol is concerned: a control connection with a
// session on it, an audio connection with a stream on it, and the ring buffer
// between the stream and the sound card that is not here.
class Room
{
public:
    Room()
        : stream(audioConnection, buffer),
          session(controlConnection, roomIdentity(),
                  [this](matp::CapabilityReport& out, std::string& error) {
                      if (report.formats.empty())
                      {
                          error = "no usable audio playback device";
                          return false;
                      }
                      out = report;
                      return true;
                  })
    {
        report = wideCapabilities();
        session.setStreamHooks(hooks());
    }

    ~Room()
    {
        // Both connection threads call into objects declared above them, so
        // they go first -- and the stream is the ring buffer's producer, so it
        // leaves the buffer before the buffer does.
        controlConnection.stop();
        audioConnection.stop();
        stream.detach();
    }

    Room(const Room&) = delete;
    Room& operator=(const Room&) = delete;

    bool start(TestListener& control, TestListener& audio)
    {
        audioAddress = audio.address();

        std::string error;
        if (!audioConnection.start(
                [this](const void* data, size_t size) { stream.onData(data, size); }, error))
            return false;
        if (!controlConnection.start(
                [this](const void* data, size_t size) { session.onData(data, size); }, error))
            return false;
        return controlConnection.setServer(control.address(), error);
    }

    // What the test's sound card does: takes frames out of the ring buffer and
    // counts them, which is also how the audible position below moves.
    uint32_t drain(uint32_t frameCount)
    {
        std::vector<unsigned char> popped(static_cast<size_t>(frameCount) *
                                          buffer.bytesPerFrame());
        const uint32_t count = buffer.pop(popped.data(), frameCount);
        for (uint32_t frame = 0; frame < count; ++frame)
        {
            const unsigned char expected = static_cast<unsigned char>(drained + frame);
            if (popped[static_cast<size_t>(frame) * buffer.bytesPerFrame()] != expected)
                ordered = false;
        }
        drained += count;
        return count;
    }

    // What StreamPlayback publishes from the sound card's position: the
    // audible frame of this room's own stream, converted outward to the one
    // axis every room shares (section 25).
    void setAudible(uint64_t streamFrame)
    {
        audibleStreamFrame = streamFrame;
        hasAudible = true;
    }

    matp::CapabilityReport report;
    std::string audioAddress;

    // Declared in the order the destructor needs them.
    ServerConnection controlConnection;
    ServerConnection audioConnection;
    AudioRingBuffer buffer;
    matp::AudioStream stream;
    matp::Session session;

    // What the fake sound card has taken, and whether it was the frames the
    // server sent, in order.
    uint64_t drained = 0;
    bool ordered = true;

    bool hasAudible = false;
    uint64_t audibleStreamFrame = 0;

    // What the hooks recorded. Everything named `...TimelineNs` is the
    // server's own number; everything named `...StreamFrame` is what this
    // room converted it to.
    AudioFormat negotiated;
    AudioFormat granted;
    uint32_t prepares = 0;
    uint32_t releases = 0;

    uint32_t starts = 0;
    uint64_t startTimelineNs = 0;
    uint64_t startStreamFrame = 0;
    bool startPlaced = false;

    uint32_t pauses = 0;
    uint32_t stops = 0;
    uint64_t endTimelineNs = 0;
    uint64_t endStreamFrame = 0;
    bool endPlaced = false;

    uint32_t volumes = 0;
    double volumeGain = 1.0;
    uint64_t volumeStreamFrame = 0;
    bool volumePlaced = false;

    uint32_t boundaries = 0;
    matp::TrackBoundary lastBoundary;
    uint64_t boundaryStreamFrame = 0;
    bool boundaryPlaced = false;

private:
    matp::Session::StreamHooks hooks()
    {
        matp::Session::StreamHooks result;

        result.prepare = [this](const matp::PrepareStream& request,
                                const AudioFormat& format, matp::StreamReady& out) {
            ++prepares;
            negotiated = format;

            // What StreamPlayback does, minus the device: the stream is the
            // buffer's only producer, so it leaves before the buffer is
            // resized for a format that may be a different size per frame.
            stream.detach();

            std::string error;
            if (!buffer.configure(format.bytesPerFrame(), 65536, error))
            {
                out.reason = error;
                return;
            }

            std::string address =
                request.hasAudioPort
                    ? audioAddress.substr(0, audioAddress.find(':') + 1) +
                          std::to_string(request.audioPort)
                    : audioAddress;
            if (!audioConnection.setServer(address, error))
            {
                out.reason = error;
                return;
            }

            // The driver granted what was asked for: the interesting refusals
            // are matp::Session's own tests, not these.
            granted = format;
            stream.attach(sessionIdOf(), roomIdentity(), format);

            drained = 0;
            hasAudible = false;
            audibleStreamFrame = 0;

            out.ready = true;
            out.format = format;
            out.periodFrames = format.sampleRate / 100;
        };

        result.release = [this]() {
            ++releases;
            stream.detach();
        };

        result.start = [this](const matp::StartAt& request, uint64_t, std::string& error) {
            // Section 33 inward: the server names a position in the media and
            // the buffer holds this room's own frames.
            if (!stream.timelineToStream(request.timelineNs, startStreamFrame))
            {
                error = "timeline " + std::to_string(request.timelineNs) +
                        " ns is older than the audio this room still holds";
                startPlaced = false;
                return false;
            }
            ++starts;
            startTimelineNs = request.timelineNs;
            startPlaced = true;
            return true;
        };

        result.pause = [this](const matp::TransportAt& request, std::string&) {
            ++pauses;
            endTimelineNs = request.timelineNs;
            endPlaced = stream.timelineToStream(request.timelineNs, endStreamFrame);
            return true;
        };

        result.stop = [this](const matp::TransportAt& request, std::string&) {
            ++stops;
            endTimelineNs = request.timelineNs;
            endPlaced = stream.timelineToStream(request.timelineNs, endStreamFrame);
            return true;
        };

        result.volume = [this](const matp::SetVolume& request, std::string&) {
            ++volumes;
            if (request.hasValue)
                volumeGain = request.value;
            volumePlaced = request.hasApplyAtTimelineNs &&
                           stream.timelineToStream(request.applyAtTimelineNs, volumeStreamFrame);
            return true;
        };

        result.trackBoundary = [this](const matp::TrackBoundary& boundary) {
            ++boundaries;
            lastBoundary = boundary;
            boundaryPlaced = stream.timelineToStream(boundary.timelineNs, boundaryStreamFrame);
        };

        result.bufferStatus = [this](matp::BufferStatus& out) {
            if (!stream.isAttached())
                return false;
            out = stream.status();
            return true;
        };

        result.playbackReport = [this](matp::PlaybackReport& out) {
            // Section 25: the frame position and the instant are this room's
            // own; the audible position is the shared timeline, and it is left
            // out entirely while there is none.
            out.localMonotonicTimeNs = monotonicNowNs();
            out.deviceFramePosition = drained;
            out.clockQuality = AudioClockQuality::Hardware;
            out.bufferedDurationNs = stream.status().bufferedDurationNs;
            if (hasAudible)
                out.hasTimelineNsAudible =
                    stream.streamToTimeline(audibleStreamFrame, out.timelineNsAudible);
            return true;
        };

        return result;
    }

    std::string sessionIdOf() const { return session.sessionId(); }
};

// The fake server: it answers the handshake, the clock exchange and nothing
// else on its own, and every message it sends is the test's decision.
class Server
{
public:
    Server(Room& room, TestListener& control, TestListener& audio)
        : room_(room), control_(control), audio_(audio)
    {
    }

    // One turn of both sides' loops: the client polls, and whatever it sent is
    // taken off the socket here rather than a loop turn later. A clock
    // exchange measures each leg, so a server that only looks every twenty
    // milliseconds puts all of the delay on the inbound one.
    //
    // `waitMs` is how long the control socket is allowed to block, which is
    // what paces the loops below: every one of them counts real milliseconds
    // because this is where they are spent. Spinning instead would make a
    // timeout expire in no time at all, while the client's own intervals --
    // the clock burst, the report period -- are real.
    void step(int waitMs = 0)
    {
        room_.session.poll(monotonicNowNs());
        room_.stream.poll();

        const std::string first = control_.receive(waitMs);
        if (!first.empty())
            controlReader_.append(first.data(), first.size());
        for (;;)
        {
            const std::string bytes = control_.receive(0);
            if (bytes.empty())
                break;
            controlReader_.append(bytes.data(), bytes.size());
        }
        for (;;)
        {
            const std::string bytes = audio_.receive(0);
            if (bytes.empty())
                break;
            audioReader_.append(bytes.data(), bytes.size());
        }

        matp::Message message;
        std::string error;
        while (controlReader_.next(message, error) == matp::FrameReader::Result::Message)
        {
            if (message.type == matp::MessageType::ClockSyncRequest)
            {
                answerClockSync(message);
                continue;
            }
            if (message.type == matp::MessageType::ClockSyncResult)
            {
                // Section 8: the four timestamps the server needs to build a
                // client-to-server clock model of its own. Counted rather than
                // queued -- every established session sends them.
                ++clockSyncResults_;
                continue;
            }
            pending_.push_back(message);
        }
        while (audioReader_.next(message, error) == matp::FrameReader::Result::Message)
        {
            if (message.type == matp::MessageType::StreamAttach)
                ++streamAttaches_;
        }
    }

    bool next(matp::Message& out, int timeoutMs)
    {
        for (int waited = 0; waited <= timeoutMs; waited += kTickMs)
        {
            step(kTickMs);
            if (take(out))
                return true;
        }
        return take(out);
    }

    bool nextOfType(matp::MessageType type, matp::Message& out, int timeoutMs)
    {
        for (int waited = 0; waited <= timeoutMs; waited += kTickMs)
        {
            step(kTickMs);
            while (take(out))
            {
                if (out.type == type)
                    return true;
            }
        }
        return false;
    }

    template <typename Predicate> bool until(Predicate predicate, int timeoutMs)
    {
        for (int waited = 0; waited <= timeoutMs; waited += kTickMs)
        {
            step(kTickMs);
            if (predicate())
                return true;
        }
        return predicate();
    }

    bool send(matp::MessageType type, const json::Value& payload)
    {
        return control_.send(matp::encode(type, payload));
    }

    bool sendAudio(const std::string& packet) { return audio_.send(packet); }

    uint32_t clockSyncResults() const { return clockSyncResults_; }
    uint32_t streamAttaches() const { return streamAttaches_; }

    // The audio connection came back on a new socket: what the old one left
    // half-read is not the start of anything on this one.
    void audioReconnected() { audioReader_.reset(); }

private:
    void answerClockSync(const matp::Message& request)
    {
        const uint64_t receivedNs = monotonicNowNs();

        int64_t clientSendNs = 0;
        json::getInt(request.payload, "clientSendNs", clientSendNs);

        json::Value payload = json::Value::makeObject();
        payload["type"] = json::Value("clockSyncReply");
        payload["clientSendNs"] = json::Value(static_cast<uint64_t>(clientSendNs));
        payload["serverReceiveNs"] = json::Value(receivedNs);
        payload["serverSendNs"] = json::Value(monotonicNowNs());
        send(matp::MessageType::ClockSyncReply, payload);
    }

    bool take(matp::Message& out)
    {
        if (pending_.empty())
            return false;
        out = pending_.front();
        pending_.erase(pending_.begin());
        return true;
    }

    // Twenty milliseconds of blocking read per turn, which is what the
    // client's own loop period looks like from here.
    static const int kTickMs = 20;

    Room& room_;
    TestListener& control_;
    TestListener& audio_;
    matp::FrameReader controlReader_;
    matp::FrameReader audioReader_;
    std::vector<matp::Message> pending_;
    uint32_t clockSyncResults_ = 0;
    uint32_t streamAttaches_ = 0;
};

json::Value helloAck()
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("helloAck");
    payload["protocolVersion"] = json::Value(matp::kProtocolVersion);
    return payload;
}

json::Value sessionConfig(const char* sessionId, const AudioFormat& format)
{
    json::Value formatValue = json::Value::makeObject();
    formatValue["sampleRate"] = json::Value(format.sampleRate);
    formatValue["channels"] = json::Value(static_cast<uint32_t>(format.channels));
    formatValue["sampleFormat"] = json::Value(sampleFormatOf(format));

    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("sessionConfig");
    payload["sessionId"] = json::Value(sessionId);
    payload["format"] = formatValue;
    return payload;
}

json::Value prepareStream(uint16_t audioPort)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("prepareStream");
    payload["audioPort"] = json::Value(static_cast<uint32_t>(audioPort));
    return payload;
}

json::Value startAt(uint64_t serverTimeNs, uint64_t timelineNs)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("startAt");
    payload["serverTimeNs"] = json::Value(serverTimeNs);
    payload["timelineNs"] = json::Value(timelineNs);
    return payload;
}

json::Value transportAt(const char* type, uint64_t serverTimeNs, uint64_t timelineNs)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value(type);
    payload["serverTimeNs"] = json::Value(serverTimeNs);
    payload["timelineNs"] = json::Value(timelineNs);
    return payload;
}

json::Value trackBoundary(uint64_t timelineNs, const char* trackId, const char* title)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("trackBoundary");
    payload["timelineNs"] = json::Value(timelineNs);
    payload["trackId"] = json::Value(trackId);
    payload["title"] = json::Value(title);
    return payload;
}

json::Value setVolume(double gain, uint64_t applyAtTimelineNs)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("setVolume");
    payload["value"] = json::Value(gain);
    payload["applyAtTimelineNs"] = json::Value(applyAtTimelineNs);
    return payload;
}

// The whole of sections 10 to 12, plus the clock exchange of section 8 and the
// PREPARE_STREAM of section 21: after this the room has a session, a clock
// mapping and an attached audio stream in `format`.
void bringUp(Room& room, Server& server, TestListener& audio, const char* sessionId,
             const AudioFormat& format)
{
    matp::Message message;
    CHECK(server.next(message, 3000));
    CHECK(message.type == matp::MessageType::Hello);

    CHECK(server.send(matp::MessageType::HelloAck, helloAck()));
    CHECK(server.next(message, 3000));
    CHECK(message.type == matp::MessageType::AudioCapabilities);

    CHECK(server.send(matp::MessageType::SessionConfig, sessionConfig(sessionId, format)));
    CHECK(server.until([&] { return room.session.state() == matp::SessionState::Established; },
                       3000));

    // Section 10 puts the clock exchange before any audio: without a mapping a
    // START_AT is a number from another clock and is refused.
    CHECK(server.until([&] { return room.session.clock().hasEstimate(); }, 5000));
    CHECK(server.clockSyncResults() != 0);

    CHECK(server.send(matp::MessageType::PrepareStream, prepareStream(audio.port())));
    CHECK(server.nextOfType(matp::MessageType::StreamReady, message, 3000));
    bool ready = false;
    CHECK(json::getBool(message.payload, "ready", ready));
    CHECK(ready);

    // The audio connection the prepare brought up, and the STREAM_ATTACH that
    // binds it to the session (section 13).
    CHECK(audio.acceptOne(3000));
    const uint32_t attaches = server.streamAttaches();
    CHECK(server.until([&] { return server.streamAttaches() > attaches; }, 3000));
}

// The first PLAYBACK_REPORT of a stream can have been queued before the sound
// card had a position at all -- section 25 leaves the audible position out
// entirely until there is one -- so the report that matters is the first one
// that carries it.
bool waitForAudible(Server& server, uint64_t& audibleNs)
{
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        matp::Message report;
        if (!server.nextOfType(matp::MessageType::PlaybackReport, report, 2000))
            return false;

        int64_t value = 0;
        if (json::getInt(report.payload, "timelineNsAudible", value))
        {
            audibleNs = static_cast<uint64_t>(value);
            return true;
        }
    }
    return false;
}

// Waits for the client to have taken a number of frames into the ring buffer.
bool waitForFrames(Room& room, Server& server, uint64_t frameCount)
{
    return server.until([&] { return room.stream.frames() >= frameCount; }, 3000);
}

} // namespace

// 1. A bitperfect 44.1 kHz stream: the source's own rate, transported
// untouched, on a timeline that names no rate at all (sections 2 and 5).
MULTIROOM_TEST(end_to_end_plays_a_bitperfect_44_1_khz_stream)
{
    TestListener control;
    TestListener audio;
    CHECK(control.ready());
    CHECK(audio.ready());

    Room room;
    CHECK(room.start(control, audio));
    CHECK(control.acceptOne(3000));

    Server server(room, control, audio);
    const AudioFormat format = fmt(44100, AudioFormat::SampleType::Int24);
    bringUp(room, server, audio, "s-441", format);
    CHECK(room.granted.sampleRate == static_cast<uint32_t>(44100));

    // 180 s in, so nothing can pass by accident against a timeline that
    // begins at zero. 441 frames is 10 ms at 44.1 kHz exactly; the source
    // still places every packet absolutely (section 15).
    const uint64_t base = 180000000000ULL;
    PacketSource source(format, base);
    for (uint32_t i = 0; i < 20; ++i)
        CHECK(server.sendAudio(source.nextBitperfect(441)));
    CHECK(waitForFrames(room, server, 20 * 441));

    CHECK_EQ(room.stream.rejoins(), static_cast<uint32_t>(0));
    CHECK(room.stream.lastError().empty());

    const matp::BufferStatus status = room.stream.status();
    CHECK(status.hasContent);
    CHECK_EQ(status.playheadTimelineNs, base);
    CHECK_EQ(status.bufferedUntilTimelineNs, nsAt(base, 20 * 441, 44100));
    CHECK_EQ(status.bufferedDurationNs, 200000000ULL); // 200 ms, whatever the rate

    // The start names a media position; this room converts it to a frame of
    // its own stream, and at 44.1 kHz that frame is not what 48 kHz would
    // have made of the same nanoseconds.
    const uint64_t startNs = nsAt(base, 4410, 44100);
    CHECK(server.send(matp::MessageType::StartAt,
                      startAt(monotonicNowNs() + 500000000ULL, startNs)));
    CHECK(server.until([&] { return room.starts != 0; }, 3000));
    CHECK(room.startPlaced);
    CHECK_EQ(room.startStreamFrame, static_cast<uint64_t>(4410));

    // And outward again: what the sound card is playing, on the axis every
    // room shares (section 25).
    room.setAudible(4410 + 441);
    uint64_t audibleNs = 0;
    CHECK(waitForAudible(server, audibleNs));
    CHECK_EQ(audibleNs, nsAt(base, 4851, 44100));

    CHECK(room.drain(441) == static_cast<uint32_t>(441));
    CHECK(room.ordered);
}

// 2. The same span of timeline at twice the rate: twice the frames, and no
// number in any message changes.
MULTIROOM_TEST(end_to_end_plays_a_bitperfect_96_khz_stream)
{
    TestListener control;
    TestListener audio;
    CHECK(control.ready());
    CHECK(audio.ready());

    Room room;
    CHECK(room.start(control, audio));
    CHECK(control.acceptOne(3000));

    Server server(room, control, audio);
    const AudioFormat format = fmt(96000, AudioFormat::SampleType::Int24);
    bringUp(room, server, audio, "s-96", format);
    CHECK(room.granted.sampleRate == static_cast<uint32_t>(96000));

    const uint64_t base = 180000000000ULL;
    PacketSource source(format, base);
    for (uint32_t i = 0; i < 10; ++i)
        CHECK(server.sendAudio(source.nextBitperfect(960))); // 10 ms each
    CHECK(waitForFrames(room, server, 9600));

    const matp::BufferStatus status = room.stream.status();
    CHECK_EQ(status.bufferedDurationNs, 100000000ULL); // 100 ms
    CHECK_EQ(status.streamFrames, static_cast<uint32_t>(9600));

    // 50 ms in is frame 4800 here and would be frame 2205 in the 44.1 kHz room
    // next door, on the same timeline and the same music.
    CHECK(server.send(matp::MessageType::StartAt,
                      startAt(monotonicNowNs() + 500000000ULL, base + 50000000ULL)));
    CHECK(server.until([&] { return room.starts != 0; }, 3000));
    CHECK_EQ(room.startStreamFrame, static_cast<uint64_t>(4800));
    CHECK_EQ(room.stream.rejoins(), static_cast<uint32_t>(0));
}

// 3. A resampled room: the device cannot do the source's rate, so the server
// converts for this room alone (section 2). The timeline is unaffected.
MULTIROOM_TEST(end_to_end_plays_a_resampled_stream)
{
    TestListener control;
    TestListener audio;
    CHECK(control.ready());
    CHECK(audio.ready());

    Room room;
    // A device that can only do 48 kHz, which is what makes the conversion the
    // server's only option for a 44.1 kHz source.
    room.report.formats.clear();
    room.report.formats.push_back(fmt(48000, AudioFormat::SampleType::Int24));

    CHECK(room.start(control, audio));
    CHECK(control.acceptOne(3000));

    Server server(room, control, audio);
    const AudioFormat format = fmt(48000, AudioFormat::SampleType::Int24);
    bringUp(room, server, audio, "s-resampled", format);

    // 10 ms spans of a 44.1 kHz source, arriving as 480 frames of 48 kHz
    // audio: the frame count is this room's and the span is everybody's.
    const uint64_t base = 180000000000ULL;
    PacketSource source(format, base);
    for (uint32_t i = 0; i < 10; ++i)
        CHECK(server.sendAudio(source.nextSpan(10000000ULL, 480)));
    CHECK(waitForFrames(room, server, 4800));

    CHECK_EQ(room.stream.rejoins(), static_cast<uint32_t>(0));
    const matp::BufferStatus status = room.stream.status();
    CHECK_EQ(status.bufferedDurationNs, 100000000ULL);
    CHECK_EQ(status.streamFrames, static_cast<uint32_t>(4800));

    CHECK(server.send(matp::MessageType::StartAt,
                      startAt(monotonicNowNs() + 500000000ULL, base + 50000000ULL)));
    CHECK(server.until([&] { return room.starts != 0; }, 3000));
    CHECK_EQ(room.startStreamFrame, static_cast<uint64_t>(2400));
}

// 4. Drift correction as it really arrives: the same span every packet, with
// the frame count moving. A client that derived the timeline from the frame
// count and the rate would call every other packet a rejoin.
MULTIROOM_TEST(end_to_end_follows_a_drift_corrected_output_ratio)
{
    TestListener control;
    TestListener audio;
    CHECK(control.ready());
    CHECK(audio.ready());

    Room room;
    CHECK(room.start(control, audio));
    CHECK(control.acceptOne(3000));

    Server server(room, control, audio);
    const AudioFormat format = fmt(48000, AudioFormat::SampleType::Int24);
    bringUp(room, server, audio, "s-drift", format);

    // 10 ms spans carrying 480 frames, with a 481st every twentieth packet:
    // this room's clock runs slow, so it is fed slightly more audio for the
    // same music. Forty packets, which is 400 ms of timeline and 19,202
    // frames rather than 19,200.
    const uint64_t base = 180000000000ULL;
    PacketSource source(format, base);
    uint64_t expectedFrames = 0;
    for (uint32_t i = 0; i < 40; ++i)
    {
        const uint32_t count = (i % 20 == 0) ? 481u : 480u;
        CHECK(server.sendAudio(source.nextSpan(10000000ULL, count)));
        expectedFrames += count;
    }
    CHECK(waitForFrames(room, server, expectedFrames));

    CHECK_EQ(expectedFrames, static_cast<uint64_t>(19202));
    CHECK_EQ(room.stream.rejoins(), static_cast<uint32_t>(0));
    CHECK(room.stream.lastError().empty());

    // The timeline is exactly 400 ms whatever the frame counts did, which is
    // the property section 28 promises: the ratio moves the frames and not the
    // timeline.
    const matp::BufferStatus status = room.stream.status();
    CHECK_EQ(status.bufferedUntilTimelineNs, base + 400000000ULL);
    CHECK_EQ(status.bufferedDurationNs, 400000000ULL);
    CHECK_EQ(status.streamFrames, static_cast<uint32_t>(19202));

    // And the mapping followed each packet's own ratio rather than an average:
    // 200 ms in is frame 9601, because one extra frame has been inserted
    // before it and another one at it.
    CHECK(server.send(matp::MessageType::StartAt,
                      startAt(monotonicNowNs() + 500000000ULL, base + 200000000ULL)));
    CHECK(server.until([&] { return room.starts != 0; }, 3000));
    CHECK_EQ(room.startStreamFrame, static_cast<uint64_t>(9601));
}

// 5. A track boundary inside one format: metadata, and nothing else happens
// (sections 22 and 23). No reattach, no rejoin, no gap.
MULTIROOM_TEST(end_to_end_crosses_a_track_boundary_at_one_rate_without_a_gap)
{
    TestListener control;
    TestListener audio;
    CHECK(control.ready());
    CHECK(audio.ready());

    Room room;
    CHECK(room.start(control, audio));
    CHECK(control.acceptOne(3000));

    Server server(room, control, audio);
    const AudioFormat format = fmt(44100, AudioFormat::SampleType::Int24);
    bringUp(room, server, audio, "s-gapless", format);

    const uint64_t base = 180000000000ULL;
    PacketSource source(format, base);
    for (uint32_t i = 0; i < 10; ++i)
        CHECK(server.sendAudio(source.nextBitperfect(441)));
    CHECK(waitForFrames(room, server, 4410));

    const uint32_t attachesBefore = server.streamAttaches();
    const uint64_t boundaryNs = nsAt(base, 2205, 44100);
    CHECK(server.send(matp::MessageType::TrackBoundary,
                      trackBoundary(boundaryNs, "track-B", "Second Song")));
    CHECK(server.until([&] { return room.boundaries != 0; }, 3000));

    CHECK_EQ(room.lastBoundary.timelineNs, boundaryNs);
    CHECK_EQ(room.lastBoundary.title, std::string("Second Song"));
    CHECK(room.boundaryPlaced);
    CHECK_EQ(room.boundaryStreamFrame, static_cast<uint64_t>(2205));

    // The next track's audio simply continues the same stream: same format,
    // same connection, contiguous timeline.
    for (uint32_t i = 0; i < 10; ++i)
        CHECK(server.sendAudio(source.nextBitperfect(441)));
    CHECK(waitForFrames(room, server, 8820));

    CHECK_EQ(room.stream.rejoins(), static_cast<uint32_t>(0));
    CHECK_EQ(room.stream.reattachments(), static_cast<uint32_t>(0));
    CHECK_EQ(server.streamAttaches(), attachesBefore);
    CHECK_EQ(room.prepares, static_cast<uint32_t>(1)); // the device was never reopened
    CHECK_EQ(room.stream.status().bufferedUntilTimelineNs, nsAt(base, 8820, 44100));
    CHECK(room.stream.lastError().empty());
}

// 6. Section 24's format transition: 44.1 kHz to 96 kHz for a bitperfect room,
// which cannot be gapless and must be sample-accurate on both sides of the
// gap. PAUSE_AT at X, renegotiate, reopen, buffer from X, START_AT at X.
MULTIROOM_TEST(end_to_end_reconfigures_a_bitperfect_room_for_a_rate_change)
{
    TestListener control;
    TestListener audio;
    CHECK(control.ready());
    CHECK(audio.ready());

    Room room;
    CHECK(room.start(control, audio));
    CHECK(control.acceptOne(3000));

    Server server(room, control, audio);
    const AudioFormat first = fmt(44100, AudioFormat::SampleType::Int24);
    bringUp(room, server, audio, "s-transition", first);

    const uint64_t base = 180000000000ULL;
    PacketSource source(first, base);
    for (uint32_t i = 0; i < 10; ++i)
        CHECK(server.sendAudio(source.nextBitperfect(441)));
    CHECK(waitForFrames(room, server, 4410));

    CHECK(server.send(matp::MessageType::StartAt,
                      startAt(monotonicNowNs() + 200000000ULL, base)));
    CHECK(server.until([&] { return room.starts != 0; }, 3000));

    // Step 1 and 2: the old track ends exactly at X, and the room is stopped
    // there. X is the first position of the *new* track, so the old one is
    // played to its end.
    const uint64_t transitionNs = source.nextTimelineNs();
    CHECK(server.send(matp::MessageType::PauseAt,
                      transportAt("pauseAt", monotonicNowNs() + 100000000ULL, transitionNs)));
    CHECK(server.until([&] { return room.pauses != 0; }, 3000));
    CHECK_EQ(room.endTimelineNs, transitionNs);
    CHECK(room.endPlaced);
    CHECK_EQ(room.endStreamFrame, static_cast<uint64_t>(4410));

    // Steps 3 to 5: a new SESSION_CONFIG on an established session is a
    // renegotiation (section 12), and PREPARE_STREAM reopens the device.
    const AudioFormat second = fmt(96000, AudioFormat::SampleType::Int24);
    CHECK(server.send(matp::MessageType::SessionConfig, sessionConfig("s-transition", second)));
    CHECK(server.until([&] { return room.session.streamFormat() == second; }, 3000));
    CHECK(room.session.state() == matp::SessionState::Established);

    const uint32_t attachesBefore = server.streamAttaches();
    CHECK(server.send(matp::MessageType::PrepareStream, prepareStream(audio.port())));

    matp::Message message;
    CHECK(server.nextOfType(matp::MessageType::StreamReady, message, 3000));
    bool ready = false;
    CHECK(json::getBool(message.payload, "ready", ready));
    CHECK(ready);
    CHECK_EQ(room.prepares, static_cast<uint32_t>(2));
    CHECK(room.granted == second);

    // The audio connection is bound again: the session did not end, so neither
    // did the stream (section 13). On a fresh socket rather than the one that
    // carried the old format, whose byte stream a re-attach cannot pick up
    // mid-packet -- which is also what StreamPlayback's release does.
    CHECK(audio.acceptOne(5000));
    server.audioReconnected();
    CHECK(server.until([&] { return server.streamAttaches() > attachesBefore; }, 5000));

    // Step 7: the new format's audio, from exactly where the old track ended.
    PacketSource resumed(second, transitionNs);
    for (uint32_t i = 0; i < 10; ++i)
        CHECK(server.sendAudio(resumed.nextBitperfect(960)));
    CHECK(server.until([&] { return room.stream.frames() >= 9600; }, 3000));
    CHECK(room.stream.lastError().empty());

    const matp::BufferStatus status = room.stream.status();
    CHECK(status.hasContent);
    CHECK_EQ(status.playheadTimelineNs, transitionNs);
    CHECK_EQ(status.bufferedDurationNs, 100000000ULL);

    // Steps 8 and 9: the resume is a new START_AT at the same position the
    // pause named, and it lands on the first frame of the new stream. No media
    // time is lost or repeated across the gap.
    CHECK(server.send(matp::MessageType::StartAt,
                      startAt(monotonicNowNs() + 200000000ULL, transitionNs)));
    CHECK(server.until([&] { return room.starts == 2; }, 3000));
    CHECK(room.startPlaced);
    CHECK_EQ(room.startTimelineNs, transitionNs);
    CHECK_EQ(room.startStreamFrame, static_cast<uint64_t>(0));
    CHECK_EQ(room.stream.rejoins(), static_cast<uint32_t>(0));
}

// 7. Pause and resume: the room stops at the position the server named and
// starts again from it, with the audio it had kept (section 37).
MULTIROOM_TEST(end_to_end_pauses_and_resumes_at_the_same_position)
{
    TestListener control;
    TestListener audio;
    CHECK(control.ready());
    CHECK(audio.ready());

    Room room;
    CHECK(room.start(control, audio));
    CHECK(control.acceptOne(3000));

    Server server(room, control, audio);
    const AudioFormat format = fmt(48000, AudioFormat::SampleType::Int24);
    bringUp(room, server, audio, "s-pause", format);

    const uint64_t base = 180000000000ULL;
    PacketSource source(format, base);
    for (uint32_t i = 0; i < 20; ++i)
        CHECK(server.sendAudio(source.nextBitperfect(480)));
    CHECK(waitForFrames(room, server, 9600));

    CHECK(server.send(matp::MessageType::StartAt,
                      startAt(monotonicNowNs() + 200000000ULL, base)));
    CHECK(server.until([&] { return room.starts != 0; }, 3000));

    // A pause 100 ms in, scheduled while the audio for it has already
    // arrived, and a scheduled volume change before it.
    CHECK(server.send(matp::MessageType::SetVolume, setVolume(0.25, base + 50000000ULL)));
    CHECK(server.until([&] { return room.volumes != 0; }, 3000));
    CHECK(room.volumePlaced);
    CHECK_EQ(room.volumeStreamFrame, static_cast<uint64_t>(2400));
    CHECK_NEAR(room.volumeGain, 0.25, 1e-12);

    const uint64_t pauseNs = base + 100000000ULL;
    CHECK(server.send(matp::MessageType::PauseAt,
                      transportAt("pauseAt", monotonicNowNs() + 100000000ULL, pauseNs)));
    CHECK(server.until([&] { return room.pauses != 0; }, 3000));
    CHECK(room.endPlaced);
    CHECK_EQ(room.endStreamFrame, static_cast<uint64_t>(4800));

    // The resume is a new START_AT at the position the pause named, and the
    // audio is still the audio that was buffered: a pause keeps it.
    CHECK_EQ(room.stream.status().bufferedDurationNs, 200000000ULL);
    CHECK(server.send(matp::MessageType::StartAt,
                      startAt(monotonicNowNs() + 200000000ULL, pauseNs)));
    CHECK(server.until([&] { return room.starts == 2; }, 3000));
    CHECK_EQ(room.startStreamFrame, static_cast<uint64_t>(4800));
    CHECK_EQ(room.stream.rejoins(), static_cast<uint32_t>(0));
    CHECK(room.stream.lastError().empty());
}

// 8. The audio connection drops and the server resumes this room somewhere
// else entirely (sections 43 and 32): everything buffered for the old position
// goes, and nothing of the new one is lost while it does.
MULTIROOM_TEST(end_to_end_rejoins_after_the_audio_connection_drops)
{
    TestListener control;
    TestListener audio;
    CHECK(control.ready());
    CHECK(audio.ready());

    Room room;
    CHECK(room.start(control, audio));
    CHECK(control.acceptOne(3000));

    Server server(room, control, audio);
    const AudioFormat format = fmt(48000, AudioFormat::SampleType::Int24);
    bringUp(room, server, audio, "s-rejoin", format);

    const uint64_t base = 180000000000ULL;
    PacketSource source(format, base);
    for (uint32_t i = 0; i < 10; ++i)
        CHECK(server.sendAudio(source.nextBitperfect(480)));
    CHECK(waitForFrames(room, server, 4800));

    // The socket goes. The session is on the control connection and is
    // untouched, so the stream binds the new socket rather than ending.
    audio.dropClient();
    CHECK(audio.acceptOne(5000));
    server.audioReconnected();
    const uint32_t attachesBefore = server.streamAttaches();
    CHECK(server.until([&] { return server.streamAttaches() > attachesBefore; }, 5000));
    CHECK_EQ(room.stream.reattachments(), static_cast<uint32_t>(1));

    // Thirty seconds have passed on the timeline, so the server carries on
    // where the music is now rather than where this room stopped hearing it.
    const uint64_t rejoinNs = base + 30000000000ULL;
    source.jumpTo(rejoinNs);
    CHECK(server.sendAudio(source.nextBitperfect(480)));
    CHECK(server.until([&] { return room.stream.resyncPending(); }, 3000));
    CHECK_EQ(room.stream.resyncTimelineNs(), rejoinNs);

    // The client's half of it: the old timeline's audio is dropped and the
    // stream is told so. Until then the producer is parked and the audio of
    // the new position waits in TCP's window.
    CHECK(room.drain(4800) == static_cast<uint32_t>(4800));
    room.stream.resume();

    for (uint32_t i = 0; i < 9; ++i)
        CHECK(server.sendAudio(source.nextBitperfect(480)));
    CHECK(server.until([&] { return room.stream.frames() >= 9600; }, 3000));

    const matp::BufferStatus status = room.stream.status();
    CHECK(status.hasContent);
    CHECK_EQ(status.playheadTimelineNs, rejoinNs);
    CHECK_EQ(status.bufferedDurationNs, 100000000ULL);
    CHECK_EQ(room.stream.rejoins(), static_cast<uint32_t>(1));
    CHECK(room.stream.lastError().empty());

    // And the room is scheduled again at the new position, which is what
    // section 43 step 5 has the server do from exactly this buffer status.
    CHECK(server.send(matp::MessageType::StartAt,
                      startAt(monotonicNowNs() + 200000000ULL, rejoinNs)));
    CHECK(server.until([&] { return room.starts != 0; }, 3000));
    CHECK_EQ(room.startStreamFrame, static_cast<uint64_t>(4800));
    CHECK(room.ordered);
}
