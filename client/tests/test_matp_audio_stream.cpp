#include "TestListener.h"
#include "TestSupport.h"

#include "matp/AudioStream.h"
#include "matp/TimelineMap.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace multiroom;
using multiroomtest::TestListener;
using multiroomtest::waitUntil;

namespace {

// Four bytes per frame, so a small ring buffer is still a round number of
// frames and a wrong offset shows up as the wrong byte rather than as noise.
AudioFormat streamFormat(uint32_t sampleRate = 48000)
{
    AudioFormat format;
    format.sampleRate = sampleRate;
    format.channels = 2;
    format.sampleType = AudioFormat::SampleType::Int16;
    return format;
}

matp::Identity testIdentity()
{
    matp::Identity identity;
    identity.clientId = "livingroom";
    identity.clientVersion = "0.1.0";
    identity.platform = "linux-armv6";
    return identity;
}

// Where this room's own frame `frame` falls on the timeline, for a stream that
// begins at `baseNs`. Absolute, the way MATP section 15 requires a sender to
// compute it: consecutive packets are contiguous by construction and nothing
// accumulates a rounding error, which matters here because 4 frames at 48 kHz
// is not a whole number of nanoseconds.
uint64_t nsAt(uint64_t baseNs, uint64_t frame, uint32_t rate)
{
    return baseNs + (frame * 1000000000ULL) / rate;
}

// 180 seconds in, so no assertion can pass by accident against a timeline that
// begins at zero.
const uint64_t kBaseNs = 180000000000ULL;

// Every byte says which frame it belongs to, so a stream that lost or
// duplicated one is visible in the buffer rather than only in a counter.
std::vector<unsigned char> frames(uint64_t firstFrame, uint32_t frameCount,
                                  const AudioFormat& format)
{
    const uint32_t bytesPerFrame = format.bytesPerFrame();
    std::vector<unsigned char> bytes(static_cast<size_t>(frameCount) * bytesPerFrame);
    for (uint32_t frame = 0; frame < frameCount; ++frame)
    {
        for (uint32_t byte = 0; byte < bytesPerFrame; ++byte)
            bytes[static_cast<size_t>(frame) * bytesPerFrame + byte] =
                static_cast<unsigned char>(firstFrame + frame);
    }
    return bytes;
}

// One packet, with both of section 14's axes stated outright: where it sits on
// the timeline and how long it is there, against the frames it actually
// carries and which of this room's frames they are.
std::string packetRaw(uint64_t sequence, uint64_t timelineStartNs, uint64_t durationNs,
                      uint64_t firstFrame, uint32_t frameCount,
                      const AudioFormat& format = streamFormat(), uint64_t sessionHigh = 1,
                      uint64_t sessionLow = 2)
{
    matp::AudioPacketHeader header;
    header.protocolVersion = static_cast<uint16_t>(matp::kProtocolVersion);
    header.sessionIdHigh = sessionHigh;
    header.sessionIdLow = sessionLow;
    header.sequenceNumber = sequence;
    header.timelineStartNs = timelineStartNs;
    header.timelineDurationNs = durationNs;
    header.format = format;
    header.outputFrameCount = frameCount;

    const std::vector<unsigned char> payload = frames(firstFrame, frameCount, format);
    return matp::encodeAudioPacket(header, payload.data(), payload.size());
}

// A packet of an unresampled stream: `count` frames beginning at this room's
// frame `firstFrame`, placed on the timeline where that frame belongs.
std::string packet(uint64_t sequence, uint64_t firstFrame, uint32_t count,
                   const AudioFormat& format = streamFormat(), uint64_t sessionHigh = 1,
                   uint64_t sessionLow = 2)
{
    const uint64_t startNs = nsAt(kBaseNs, firstFrame, format.sampleRate);
    const uint64_t endNs = nsAt(kBaseNs, firstFrame + count, format.sampleRate);
    return packetRaw(sequence, startNs, endNs - startNs, firstFrame, count, format, sessionHigh,
                     sessionLow);
}

// A packet of a stream this room is being resampled for: one millisecond of
// timeline carrying 60 frames where its nominal rate would put 48. A quarter
// off rather than the parts per million a real correction runs at, so that a
// client which confused the two axes is wrong by an obvious amount instead of
// by one frame in fifty thousand -- and still inside the factor of two the
// packet parser allows.
const uint64_t kResampledSpanNs = 1000000ULL;
const uint32_t kResampledFrames = 60;

std::string resampledPacket(uint64_t sequence, uint32_t index)
{
    return packetRaw(sequence, kBaseNs + index * kResampledSpanNs, kResampledSpanNs,
                     static_cast<uint64_t>(index) * kResampledFrames, kResampledFrames);
}

// The client side: a ring buffer, the connection that fills it, and the stream
// in between. Nothing here needs a sound card -- the consumer of the ring
// buffer is the test itself.
class Fixture
{
public:
    explicit Fixture(uint32_t capacityFrames = 4096,
                     const AudioFormat& format = streamFormat())
        : stream(connection, buffer), format_(format)
    {
        std::string error;
        configured_ = buffer.configure(format.bytesPerFrame(), capacityFrames, error);
    }

    ~Fixture()
    {
        // The stream first, so the connection thread is out of the ring buffer
        // before anything it points at goes away.
        stream.detach();
        connection.stop();
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    bool start(TestListener& listener)
    {
        if (!configured_)
            return false;

        std::string error;
        if (!connection.start(
                [this](const void* data, size_t size) { stream.onData(data, size); }, error))
            return false;
        return connection.setServer(listener.address(), error);
    }

    // Attaches and drives poll() until STREAM_ATTACH has reached the server,
    // which is the moment the stream starts accepting audio.
    bool attach(TestListener& listener)
    {
        stream.attach("s-1", testIdentity(), format_);
        return waitUntil(
            [this, &listener] {
                stream.poll();
                return readMessage(listener, attachMessage_);
            },
            2000);
    }

    // The same, for a connection that replaced the one the stream was on:
    // nothing is attached again here, the stream binds the new socket by
    // itself. The message reader starts over with it, because a server that
    // was cut off mid-frame is not the start of anything on the new one.
    bool rebind(TestListener& listener)
    {
        reader_.reset();
        return waitUntil(
            [this, &listener] {
                stream.poll();
                return readMessage(listener, attachMessage_);
            },
            5000);
    }

    const matp::Message& attachMessage() const { return attachMessage_; }

    // Reads whatever the client has sent on the audio connection, framed the
    // way the control connection frames its messages.
    bool readMessage(TestListener& listener, matp::Message& out)
    {
        const std::string bytes = listener.receive(20);
        if (!bytes.empty())
            reader_.append(bytes.data(), bytes.size());

        std::string error;
        return reader_.next(out, error) == matp::FrameReader::Result::Message;
    }

    // Pops the whole buffer and checks that it is the frames that were sent,
    // in order, starting at the given stream frame.
    bool bufferHolds(uint64_t firstFrame, uint32_t frameCount)
    {
        std::vector<unsigned char> popped(static_cast<size_t>(frameCount) *
                                          format_.bytesPerFrame());
        if (buffer.pop(popped.data(), frameCount) != frameCount)
            return false;
        return popped == frames(firstFrame, frameCount, format_);
    }

    ServerConnection connection;
    AudioRingBuffer buffer;
    matp::AudioStream stream;

private:
    AudioFormat format_;
    bool configured_ = false;
    matp::FrameReader reader_;
    matp::Message attachMessage_;
};

bool waitForFrames(Fixture& fixture, uint64_t frameCount)
{
    return waitUntil([&] { return fixture.stream.frames() >= frameCount; }, 2000);
}

bool waitForFailure(Fixture& fixture)
{
    return waitUntil([&] { return !fixture.stream.lastError().empty(); }, 2000);
}

} // namespace

MULTIROOM_TEST(matp_audio_stream_binds_the_connection_to_one_session)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    // A fresh TCP connection carries no identity at all, so the first thing on
    // it says which session's audio this is (section 13).
    const matp::Message& attach = fixture.attachMessage();
    CHECK(attach.type == matp::MessageType::StreamAttach);

    std::string sessionId;
    std::string clientId;
    CHECK(json::getString(attach.payload, "sessionId", sessionId));
    CHECK(json::getString(attach.payload, "clientId", clientId));
    CHECK_EQ(sessionId, std::string("s-1"));
    CHECK_EQ(clientId, std::string("livingroom"));
}

MULTIROOM_TEST(matp_audio_stream_fills_the_ring_buffer_with_what_arrived)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    CHECK(listener.send(packet(10, 0, 4)));
    CHECK(listener.send(packet(11, 4, 4)));
    CHECK(waitForFrames(fixture, 8));

    CHECK_EQ(fixture.stream.packets(), static_cast<uint64_t>(2));
    CHECK_EQ(fixture.buffer.readableFrames(), static_cast<uint32_t>(8));

    // Section 19: where the buffered audio sits on the media timeline, and how
    // much media time it is -- not how many frames of it there are.
    const matp::BufferStatus status = fixture.stream.status();
    CHECK(status.hasContent);
    CHECK_EQ(status.playheadTimelineNs, kBaseNs);
    CHECK_EQ(status.bufferedUntilTimelineNs, nsAt(kBaseNs, 8, 48000));
    CHECK_EQ(status.bufferedDurationNs, nsAt(kBaseNs, 8, 48000) - kBaseNs);
    CHECK_EQ(status.streamFrames, static_cast<uint32_t>(8));

    CHECK(fixture.bufferHolds(0, 8));

    // What has been played is no longer buffered, and the first position moves
    // with it.
    const matp::BufferStatus drained = fixture.stream.status();
    CHECK_EQ(drained.bufferedDurationNs, static_cast<uint64_t>(0));
    CHECK_EQ(drained.streamFrames, static_cast<uint32_t>(0));
    CHECK_EQ(drained.playheadTimelineNs, nsAt(kBaseNs, 8, 48000));
    CHECK_EQ(drained.bufferedUntilTimelineNs, nsAt(kBaseNs, 8, 48000));
}

MULTIROOM_TEST(matp_audio_stream_reassembles_packets_split_across_reads)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    // Two packets in one write, then the third cut through its header: one
    // read is not one packet, in either direction.
    const std::string pair = packet(0, 0, 3) + packet(1, 3, 3);
    const std::string third = packet(2, 6, 3);
    CHECK(listener.send(pair + third.substr(0, 20)));
    CHECK(waitForFrames(fixture, 6));
    CHECK_EQ(fixture.stream.packets(), static_cast<uint64_t>(2));

    CHECK(listener.send(third.substr(20)));
    CHECK(waitForFrames(fixture, 9));
    CHECK(fixture.bufferHolds(0, 9));
}

MULTIROOM_TEST(matp_audio_stream_refuses_a_format_it_did_not_confirm)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    // The client confirmed 48 kHz in STREAM_READY. A buffer sized in frames of
    // that format cannot carry another one, and after a format transition
    // (section 24) it is the server that changes first, not the stream.
    CHECK(listener.send(packet(0, 0, 4, streamFormat(44100))));
    CHECK(waitForFailure(fixture));
    CHECK_EQ(fixture.buffer.readableFrames(), static_cast<uint32_t>(0));

    // The connection goes, the stream does not: the next one is bound.
    CHECK(fixture.stream.isAttached());
    CHECK(listener.acceptOne(5000));
    CHECK(fixture.rebind(listener));
}

MULTIROOM_TEST(matp_audio_stream_parks_on_a_timeline_jump_until_the_old_one_is_cleared)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    CHECK(listener.send(packet(0, 0, 4)));
    CHECK(waitForFrames(fixture, 4));

    // The server moves this room to another timeline position (sections 43 and
    // 32). The ring buffer holds frames and not positions, so nothing of the
    // new one may land until what is in it is gone.
    const uint64_t jumpNs = kBaseNs + 10000000ULL;
    CHECK(listener.send(packetRaw(1, jumpNs, nsAt(0, 4, 48000), 100, 4)));
    CHECK(waitUntil([&] { return fixture.stream.resyncPending(); }, 2000));
    CHECK_EQ(fixture.stream.resyncTimelineNs(), jumpNs);
    CHECK_EQ(fixture.stream.rejoins(), static_cast<uint32_t>(1));

    // Parked, not failed and not dropped: the connection is still the one it
    // was, and the old timeline is still whole while the owner deals with it.
    CHECK(fixture.stream.lastError().empty());
    CHECK(fixture.stream.isAttached());
    CHECK(!waitUntil([&] { return fixture.stream.frames() != 4; }, 300));
    CHECK_EQ(fixture.stream.status().playheadTimelineNs, kBaseNs);

    // What the owner thread does: empty the buffer, then say so.
    CHECK(fixture.bufferHolds(0, 4));
    fixture.stream.resume();

    // Section 43's rejoin position, and no frame of it lost to the wait: TCP's
    // window held it while the producer was parked.
    CHECK(waitForFrames(fixture, 8));
    CHECK(fixture.bufferHolds(100, 4));
    CHECK_EQ(fixture.stream.status().bufferedUntilTimelineNs,
             jumpNs + nsAt(0, 4, 48000));
    CHECK(fixture.stream.lastError().empty());
}

MULTIROOM_TEST(matp_audio_stream_rebinds_a_replaced_connection)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    CHECK(listener.send(packet(0, 0, 4)));
    CHECK(waitForFrames(fixture, 4));

    // The audio connection goes and comes back. The session it belongs to
    // lives on the control connection, so it did not go with it: the stream
    // binds the new socket rather than ending (section 43).
    listener.dropClient();
    CHECK(listener.acceptOne(5000));
    CHECK(fixture.rebind(listener));
    CHECK_EQ(fixture.stream.reattachments(), static_cast<uint32_t>(1));

    // A server that resumes where it left off costs the room nothing at all --
    // no rejoin, no dropped buffer -- and its sequence numbering is its own
    // business on a connection the client has never seen numbered.
    CHECK(listener.send(packet(9000, 4, 4)));
    CHECK(waitForFrames(fixture, 8));
    CHECK_EQ(fixture.stream.rejoins(), static_cast<uint32_t>(0));
    CHECK(fixture.stream.lastError().empty());
    CHECK(fixture.bufferHolds(0, 8));
}

MULTIROOM_TEST(matp_audio_stream_rejoins_when_a_new_connection_resumes_elsewhere)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    CHECK(listener.send(packet(0, 0, 4)));
    CHECK(waitForFrames(fixture, 4));

    listener.dropClient();
    CHECK(listener.acceptOne(5000));
    CHECK(fixture.rebind(listener));

    // Thirty seconds of silence and the server carries on where the music is
    // now, not where this room stopped hearing it (section 43: never play the
    // old packets faster to catch up).
    const uint64_t rejoinNs = kBaseNs + 30000000000ULL;
    CHECK(listener.send(packetRaw(0, rejoinNs, nsAt(0, 4, 48000), 100, 4)));
    CHECK(waitUntil([&] { return fixture.stream.resyncPending(); }, 2000));
    CHECK_EQ(fixture.stream.resyncTimelineNs(), rejoinNs);

    CHECK(fixture.bufferHolds(0, 4));
    fixture.stream.resume();

    CHECK(waitForFrames(fixture, 8));
    CHECK(fixture.bufferHolds(100, 4));
    CHECK_EQ(fixture.stream.reattachments(), static_cast<uint32_t>(1));
    CHECK_EQ(fixture.stream.rejoins(), static_cast<uint32_t>(1));
}

MULTIROOM_TEST(matp_audio_stream_refuses_a_gap_in_the_sequence)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    CHECK(listener.send(packet(0, 0, 4)));
    CHECK(waitForFrames(fixture, 4));

    // TCP neither loses nor reorders, so a missing sequence number is the
    // server skipping rather than the network.
    CHECK(listener.send(packet(2, 4, 4)));
    CHECK(waitForFailure(fixture));
    CHECK(listener.acceptOne(5000));
    CHECK(fixture.rebind(listener));
}

MULTIROOM_TEST(matp_audio_stream_refuses_a_session_change_mid_stream)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    CHECK(listener.send(packet(0, 0, 4)));
    CHECK(waitForFrames(fixture, 4));

    // This connection carries exactly one session (section 13); audio from
    // another one on it is stale or crossed, and playing it would be worse
    // than saying so.
    CHECK(listener.send(packet(1, 4, 4, streamFormat(), 9, 9)));
    CHECK(waitForFailure(fixture));
    CHECK(listener.acceptOne(5000));
    CHECK(fixture.rebind(listener));
}

MULTIROOM_TEST(matp_audio_stream_ignores_audio_on_a_connection_it_has_not_bound)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    // Attached but not yet bound: nothing has told the server which stream
    // this connection is, so audio on it belongs to nobody.
    fixture.stream.attach("s-1", testIdentity(), streamFormat());
    CHECK(listener.send(packet(0, 0, 4)));
    CHECK(!waitUntil([&] { return fixture.stream.frames() != 0; }, 300));
    CHECK(fixture.stream.lastError().empty());

    // ...and the stream is not broken by it: the first packet after the bind
    // sets where the stream is.
    CHECK(fixture.attach(listener));
    CHECK(listener.send(packet(7, 200, 4)));
    CHECK(waitForFrames(fixture, 4));
    CHECK(fixture.bufferHolds(200, 4));
}

MULTIROOM_TEST(matp_audio_stream_waits_for_room_rather_than_dropping_frames)
{
    TestListener listener;
    CHECK(listener.ready());

    // Sixteen frames of buffer against forty frames of audio: the producer has
    // to wait for the consumer at least twice.
    Fixture fixture(16);
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    for (uint32_t i = 0; i < 10; ++i)
        CHECK(listener.send(packet(i, i * 4, 4)));

    // Drained in the order it arrives, which is the only way it can still be
    // the same stream afterwards.
    for (uint32_t i = 0; i < 10; ++i)
    {
        CHECK(waitUntil([&] { return fixture.buffer.readableFrames() >= 4; }, 2000));
        CHECK(fixture.bufferHolds(i * 4, 4));
    }

    CHECK(waitForFrames(fixture, 40));
    CHECK(fixture.stream.lastError().empty());
    CHECK(fixture.stream.waits() != 0);
}

MULTIROOM_TEST(matp_audio_stream_stops_producing_when_it_is_detached)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    CHECK(listener.send(packet(0, 0, 4)));
    CHECK(waitForFrames(fixture, 4));

    // detach() returns only once the connection thread is out of the ring
    // buffer, which is what makes it safe to reconfigure afterwards.
    fixture.stream.detach();
    CHECK(!fixture.stream.isAttached());

    CHECK(listener.send(packet(1, 4, 4)));
    CHECK(!waitUntil([&] { return fixture.stream.frames() != 4; }, 300));
    CHECK_EQ(fixture.buffer.readableFrames(), static_cast<uint32_t>(4));
}

// The case the two axes exist for. A server resampling this room sends a
// different number of frames than its nominal rate implies, every packet, for
// as long as it is correcting. Judged on the frame count and the rate, the
// second packet would look like a jump on the timeline and the room would
// rejoin -- and then rejoin again, once per packet, forever.
MULTIROOM_TEST(matp_audio_stream_follows_a_resampled_stream_without_rejoining)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    CHECK(listener.send(resampledPacket(10, 0)));
    CHECK(listener.send(resampledPacket(11, 1)));
    CHECK(listener.send(resampledPacket(12, 2)));
    CHECK(waitForFrames(fixture, 3 * kResampledFrames));

    CHECK_EQ(fixture.stream.rejoins(), static_cast<uint32_t>(0));
    CHECK_EQ(fixture.stream.packets(), static_cast<uint64_t>(3));
    CHECK(fixture.stream.lastError().empty());

    // Timeline progress is the packets' own spans, three times over, and the
    // room's own buffer holds the frames it was actually sent.
    const matp::BufferStatus status = fixture.stream.status();
    CHECK(status.hasContent);
    CHECK_EQ(status.playheadTimelineNs, kBaseNs);
    CHECK_EQ(status.bufferedUntilTimelineNs, kBaseNs + 3 * kResampledSpanNs);
    CHECK_EQ(status.bufferedDurationNs, 3 * kResampledSpanNs);
    CHECK_EQ(status.streamFrames, 3 * kResampledFrames);
}

// The conversion the playback layer asks for, over the same resampled stream.
MULTIROOM_TEST(matp_audio_stream_maps_timeline_positions_onto_the_frames_it_was_sent)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    CHECK(listener.send(resampledPacket(10, 0)));
    CHECK(listener.send(resampledPacket(11, 1)));
    CHECK(waitForFrames(fixture, 2 * kResampledFrames));

    uint64_t streamFrame = 0;
    CHECK(fixture.stream.timelineToStream(kBaseNs, streamFrame));
    CHECK_EQ(streamFrame, static_cast<uint64_t>(0));

    // The start of the second packet: 60 frames into the stream, not the 48
    // that a millisecond of 48 kHz audio would otherwise be.
    CHECK(fixture.stream.timelineToStream(kBaseNs + kResampledSpanNs, streamFrame));
    CHECK_EQ(streamFrame, static_cast<uint64_t>(kResampledFrames));

    // Inside a packet, interpolated at that packet's own ratio.
    CHECK(fixture.stream.timelineToStream(kBaseNs + kResampledSpanNs / 2, streamFrame));
    CHECK_EQ(streamFrame, static_cast<uint64_t>(30));

    // Ahead of everything that has arrived -- which is where a PAUSE_AT lands
    // -- extrapolated at the newest packet's ratio rather than refused. The
    // frontier is stream frame 120, and one more span past it is 60 more of
    // this room's own frames.
    CHECK(fixture.stream.timelineToStream(kBaseNs + 3 * kResampledSpanNs, streamFrame));
    CHECK_EQ(streamFrame, static_cast<uint64_t>(180));

    uint64_t timelineNs = 0;
    CHECK(fixture.stream.streamToTimeline(kResampledFrames, timelineNs));
    CHECK_EQ(timelineNs, kBaseNs + kResampledSpanNs);
    CHECK(fixture.stream.streamToTimeline(2 * kResampledFrames, timelineNs));
    CHECK_EQ(timelineNs, kBaseNs + 2 * kResampledSpanNs);

    // The buffer head is where a start is measured from, and it is on the
    // stream axis because that is the axis the ring buffer counts on.
    CHECK_EQ(fixture.stream.bufferHeadStreamFrame(), static_cast<uint64_t>(0));
}

// Drift correction as it really arrives: the same span of timeline every
// packet, with the frame count moving by one now and then. The timeline must
// stay continuous through it, and the mapping must follow each packet's own
// ratio rather than an average.
MULTIROOM_TEST(matp_audio_stream_follows_a_frame_count_that_changes_between_packets)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    // 1 ms spans carrying 48, then 49, then 48 frames.
    const uint32_t counts[] = {48, 49, 48};
    uint64_t firstFrame = 0;
    for (uint32_t i = 0; i < 3; ++i)
    {
        CHECK(listener.send(packetRaw(i, kBaseNs + i * kResampledSpanNs, kResampledSpanNs,
                                      firstFrame, counts[i])));
        firstFrame += counts[i];
    }
    CHECK(waitForFrames(fixture, 145));

    CHECK_EQ(fixture.stream.rejoins(), static_cast<uint32_t>(0));
    CHECK(fixture.stream.lastError().empty());

    const matp::BufferStatus status = fixture.stream.status();
    CHECK_EQ(status.bufferedDurationNs, 3 * kResampledSpanNs);
    CHECK_EQ(status.streamFrames, static_cast<uint32_t>(145));

    // The third packet begins after 97 frames, not after 3 x 48: the extra
    // frame of the second one is in the mapping and not averaged away.
    uint64_t streamFrame = 0;
    CHECK(fixture.stream.timelineToStream(kBaseNs + 2 * kResampledSpanNs, streamFrame));
    CHECK_EQ(streamFrame, static_cast<uint64_t>(97));
}

// A bitperfect stream at the rate no whole number of nanoseconds lines up
// with. Every packet boundary still has to convert to the exact frame.
MULTIROOM_TEST(matp_audio_stream_follows_a_bitperfect_44_1_khz_stream)
{
    TestListener listener;
    CHECK(listener.ready());

    const AudioFormat fmt = streamFormat(44100);
    Fixture fixture(4096, fmt);
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    for (uint32_t i = 0; i < 8; ++i)
        CHECK(listener.send(packet(i, static_cast<uint64_t>(i) * 480, 480, fmt)));
    CHECK(waitForFrames(fixture, 8 * 480));

    CHECK_EQ(fixture.stream.rejoins(), static_cast<uint32_t>(0));
    CHECK(fixture.stream.lastError().empty());

    for (uint32_t i = 0; i <= 8; ++i)
    {
        const uint64_t frame = static_cast<uint64_t>(i) * 480;
        uint64_t streamFrame = 0;
        CHECK(fixture.stream.timelineToStream(nsAt(kBaseNs, frame, 44100), streamFrame));
        CHECK_EQ(streamFrame, frame);
    }

    const matp::BufferStatus status = fixture.stream.status();
    CHECK_EQ(status.playheadTimelineNs, kBaseNs);
    CHECK_EQ(status.bufferedUntilTimelineNs, nsAt(kBaseNs, 8 * 480, 44100));
}

// A gap in the timeline is still a rejoin when the stream is resampled: what
// makes it one is the timeline arithmetic failing, and that has to keep
// working when the frame count does not match the rate.
MULTIROOM_TEST(matp_audio_stream_still_sees_a_timeline_jump_in_a_resampled_stream)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    CHECK(listener.send(resampledPacket(10, 0)));
    CHECK(waitForFrames(fixture, kResampledFrames));

    // Continues at kBaseNs + 1 ms if nothing moved; this one is 30 s further on.
    const uint64_t jumpNs = kBaseNs + 30000000000ULL;
    CHECK(listener.send(
        packetRaw(11, jumpNs, kResampledSpanNs, kResampledFrames, kResampledFrames)));
    CHECK(waitUntil([&] { return fixture.stream.resyncPending(); }, 2000));
    CHECK_EQ(fixture.stream.resyncTimelineNs(), jumpNs);
    CHECK_EQ(fixture.stream.rejoins(), static_cast<uint32_t>(1));
}

// The first packet after a FLUSH (MATP section 54): packetRaw's header with
// afterFlush set.
static std::string afterFlushPacket(uint64_t sequence, uint64_t timelineStartNs,
                                    uint64_t firstFrame, uint32_t count)
{
    matp::AudioPacketHeader header;
    header.protocolVersion = static_cast<uint16_t>(matp::kProtocolVersion);
    header.sessionIdHigh = 1;
    header.sessionIdLow = 2;
    header.sequenceNumber = sequence;
    header.timelineStartNs = timelineStartNs;
    header.timelineDurationNs = nsAt(0, count, 48000);
    header.format = streamFormat();
    header.outputFrameCount = count;
    header.flags = matp::kAudioFlagAfterFlush;

    const std::vector<unsigned char> payload = frames(firstFrame, count, streamFormat());
    return matp::encodeAudioPacket(header, payload.data(), payload.size());
}

MULTIROOM_TEST(matp_audio_stream_carries_a_flush_on_the_stream_axis_it_had)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    CHECK(listener.send(packet(0, 0, 4)));
    CHECK(waitForFrames(fixture, 4));

    // A seek: the next packet starts 10 ms on and says it follows a flush. It
    // parks like a rejoin, and is not counted as one.
    const uint64_t seekNs = kBaseNs + 10000000ULL;
    CHECK(listener.send(afterFlushPacket(1, seekNs, 100, 4)));
    CHECK(waitUntil([&] { return fixture.stream.resyncPending(); }, 2000));
    CHECK(fixture.stream.resyncAfterFlush());
    CHECK_EQ(fixture.stream.resyncTimelineNs(), seekNs);
    CHECK_EQ(fixture.stream.rejoins(), static_cast<uint32_t>(0));

    // The mapping of the audio before the cut is still there to place the
    // seek on: 10 ms at 48 kHz from stream frame 0.
    uint64_t streamFrame = 0;
    CHECK(fixture.stream.timelineToStream(seekNs, streamFrame));
    CHECK_EQ(streamFrame, static_cast<uint64_t>(480));

    CHECK(fixture.bufferHolds(0, 4));
    fixture.stream.resumeAfterFlush(streamFrame);
    CHECK(!fixture.stream.resyncPending());

    // The stream axis jumped with the timeline, so the new audio sits where
    // the kept line expects it.
    CHECK(waitForFrames(fixture, 8));
    CHECK_EQ(fixture.stream.bufferHeadStreamFrame(), static_cast<uint64_t>(480));
    CHECK(fixture.stream.timelineToStream(seekNs, streamFrame));
    CHECK_EQ(streamFrame, static_cast<uint64_t>(480));
    uint64_t timelineNs = 0;
    CHECK(fixture.stream.streamToTimeline(482, timelineNs));
    CHECK_EQ(timelineNs, seekNs + nsAt(0, 2, 48000));

    CHECK(fixture.bufferHolds(100, 4));
    CHECK_EQ(fixture.stream.flushes(), static_cast<uint32_t>(1));
    CHECK_EQ(fixture.stream.rejoins(), static_cast<uint32_t>(0));
    CHECK(fixture.stream.lastError().empty());
}

MULTIROOM_TEST(matp_audio_stream_parks_on_an_after_flush_packet_that_does_not_jump)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    CHECK(listener.send(packet(0, 0, 4)));
    CHECK(waitForFrames(fixture, 4));

    // It continues exactly where the last packet ended, and still parks: the
    // flush emptied the buffer, so there is nothing in front of it to continue.
    CHECK(listener.send(afterFlushPacket(1, nsAt(kBaseNs, 4, 48000), 4, 4)));
    CHECK(waitUntil([&] { return fixture.stream.resyncPending(); }, 2000));
    CHECK(fixture.stream.resyncAfterFlush());

    // A flagged packet the owner ends as a rejoin is counted as one.
    CHECK(fixture.bufferHolds(0, 4));
    fixture.stream.resume();
    CHECK(waitForFrames(fixture, 8));
    CHECK_EQ(fixture.stream.rejoins(), static_cast<uint32_t>(1));
    CHECK_EQ(fixture.stream.flushes(), static_cast<uint32_t>(0));
}

MULTIROOM_TEST(matp_audio_stream_clears_a_rejoin_that_timed_out)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    CHECK(listener.send(packet(0, 0, 4)));
    CHECK(waitForFrames(fixture, 4));

    CHECK(listener.send(packetRaw(1, kBaseNs + 10000000ULL, nsAt(0, 4, 48000), 100, 4)));
    CHECK(waitUntil([&] { return fixture.stream.resyncPending(); }, 2000));

    // The owner never empties the buffer, so the stream fails after its resync
    // timeout. A rejoin still pending then would have the owner resume a
    // stream whose connection is already gone.
    CHECK(waitUntil([&] { return !fixture.stream.lastError().empty(); }, 7000));
    CHECK(!fixture.stream.resyncPending());
    CHECK(!fixture.stream.resyncAfterFlush());
}

MULTIROOM_TEST(matp_audio_stream_carries_on_after_a_connection_it_failed_on)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    CHECK(listener.send(packet(0, 0, 4)));
    CHECK(waitForFrames(fixture, 4));

    // A byte stream out of step is the connection's failure, not the
    // stream's. The session lives on the control connection (section 13), so
    // the next connection is bound and the server resumes on it (section 43)
    // rather than the room going silent until the next PREPARE_STREAM.
    CHECK(listener.send(std::string(80, 'x')));
    CHECK(waitForFailure(fixture));
    CHECK(listener.acceptOne(5000));
    CHECK(fixture.rebind(listener));
    CHECK_EQ(fixture.stream.reattachments(), static_cast<uint32_t>(1));

    CHECK(listener.send(packet(0, 4, 4)));
    CHECK(waitForFrames(fixture, 8));
    CHECK_EQ(fixture.stream.rejoins(), static_cast<uint32_t>(0));
    CHECK(fixture.bufferHolds(0, 8));
    // Recovered: the failure is no longer the stream's current error.
    CHECK(waitUntil([&] { return fixture.stream.lastError().empty(); }, 2000));
}

MULTIROOM_TEST(matp_audio_stream_waits_rather_than_forget_buffered_positions)
{
    TestListener listener;
    CHECK(listener.ready());

    // Room for more packets than the timeline map keeps: packets far smaller
    // than section 15 asks for fill the map before the ring buffer.
    const uint32_t perPacket = 2;
    const uint32_t count = static_cast<uint32_t>(matp::kTimelineSegments) + 1;
    Fixture fixture(8192);
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    CHECK(fixture.attach(listener));

    std::string all;
    for (uint32_t i = 0; i < count; ++i)
        all += packet(i, static_cast<uint64_t>(i) * perPacket, perPacket);
    CHECK(listener.send(all));

    // The last packet waits instead of overwriting the segment of audio that
    // has not been played, so the buffer keeps a position to start from.
    CHECK(waitForFrames(fixture, static_cast<uint64_t>(count - 1) * perPacket));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(fixture.stream.packets(), static_cast<uint64_t>(count - 1));
    const matp::BufferStatus full = fixture.stream.status();
    CHECK(full.hasContent);
    CHECK_EQ(full.playheadTimelineNs, kBaseNs);

    // Playing the oldest packet makes room for it.
    CHECK(fixture.bufferHolds(0, perPacket));
    CHECK(waitForFrames(fixture, static_cast<uint64_t>(count) * perPacket));
    CHECK(fixture.stream.status().hasContent);
}
