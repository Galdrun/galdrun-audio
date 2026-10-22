// StreamPlayback against a fake server's audio connection and a sound card
// played by the test (StubBackend). What is real is everything between the
// two: the audio connection, matp::AudioStream, the ring buffer,
// TimelineSource and StreamPlayback's own handshakes, which are what MATP
// section 54's flush is made of. The FLUSH itself is handed to the hook the
// way matp::Session hands it over; the session's dispatch has its own test.

#include "StubBackend.h"
#include "TestListener.h"
#include "TestSupport.h"

#include "app/StreamPlayback.h"
#include "config/ClientConfig.h"
#include "matp/AudioPacket.h"
#include "util/Time.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace multiroom;
using multiroomtest::TestListener;
using multiroomtest::waitUntil;

namespace {

const uint32_t kRate = 48000;
const uint32_t kPeriod = 48; // one millisecond
const uint64_t kMsNs = 1000000ULL;

// 180 s in, so nothing passes by accident against a timeline that starts at 0.
const uint64_t kBaseNs = 180000ULL * kMsNs;

// Int16 stereo: four bytes a frame, which carry the frame's own value.
AudioFormat roomFormat()
{
    AudioFormat format;
    format.sampleRate = kRate;
    format.channels = 2;
    format.sampleType = AudioFormat::SampleType::Int16;
    return format;
}

// One millisecond of audio whose frames are numbered from `firstValue`.
std::string packetAt(uint64_t sequence, uint64_t timelineNs, uint32_t firstValue,
                     uint32_t flags = 0)
{
    matp::AudioPacketHeader header;
    header.protocolVersion = static_cast<uint16_t>(matp::kProtocolVersion);
    header.sessionIdHigh = 1;
    header.sessionIdLow = 2;
    header.sequenceNumber = sequence;
    header.timelineStartNs = timelineNs;
    header.timelineDurationNs = kMsNs;
    header.format = roomFormat();
    header.outputFrameCount = kPeriod;
    header.flags = flags;

    std::vector<unsigned char> payload(static_cast<size_t>(kPeriod) * 4);
    for (uint32_t i = 0; i < kPeriod; ++i)
    {
        const uint32_t value = firstValue + i;
        std::memcpy(payload.data() + static_cast<size_t>(i) * 4, &value, 4);
    }
    return matp::encodeAudioPacket(header, payload.data(), payload.size());
}

// `count` milliseconds from `timelineNs`, numbered from `firstValue`. Only the
// first carries `flags`, which is where section 54 puts afterFlush.
bool sendRun(TestListener& listener, uint64_t& sequence, uint64_t timelineNs,
             uint32_t firstValue, uint32_t count, uint32_t flags = 0)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!listener.send(packetAt(sequence++, timelineNs + i * kMsNs, firstValue + i * kPeriod,
                                    i == 0 ? flags : 0)))
            return false;
    }
    return true;
}

matp::Identity roomIdentity()
{
    matp::Identity identity;
    identity.clientId = "livingroom";
    identity.clientVersion = "0.1.0";
    identity.platform = "test";
    return identity;
}

// One room: the client's playback layer, the sound card it plays through,
// and what that sound card was handed, one value per device frame.
class Room
{
public:
    Room()
    {
        config.server = "127.0.0.1:1"; // PREPARE_STREAM names the audio port
        // Every sample here is its own frame number; the de-click would fade
        // the first and last of them. test_timeline_source covers it.
        config.declick = false;
        runtime.config = &config;
        runtime.deviceSelected = true;
        runtime.deviceId = "stub";

        std::unique_ptr<multiroomtest::StubBackend> backend(new multiroomtest::StubBackend());
        stub = backend.get();
        stub->clock.nominalSampleRate = kRate;
        stub->clock.quality = AudioClockQuality::Hardware;
        clockBaseNs_ = monotonicNowNs();
        stub->clock.monotonicTimeNs = clockBaseNs_;

        playback.reset(new StreamPlayback(runtime, roomIdentity(), 1200, std::move(backend)));
    }

    // Prepares the stream and waits for STREAM_ATTACH on the audio connection.
    bool open(TestListener& listener)
    {
        std::string error;
        if (!playback->start(error))
            return false;

        matp::PrepareStream request;
        request.hasAudioPort = true;
        request.audioPort = listener.port();
        matp::StreamReady ready;
        playback->prepare(request, roomFormat(), ready);
        lastReady = ready;
        if (!ready.ready || !listener.acceptOne(2000))
            return false;

        return waitUntil(
            [&] {
                playback->poll();
                return !listener.receive(20).empty();
            },
            2000);
    }

    // Arms the start 10 ms after the device's current position: device frame
    // cursor + 480 plays `timelineNs`.
    bool start(uint64_t timelineNs)
    {
        matp::StartAt request;
        request.timelineNs = timelineNs;
        std::string error;
        return playback->start(request, stub->clock.monotonicTimeNs + 10 * kMsNs, error);
    }

    // The audio thread, one period at a time, up to `deviceFrame`. The device
    // plays what it is handed at once, so its position is the render cursor.
    void play(uint64_t deviceFrame)
    {
        std::vector<unsigned char> block(static_cast<size_t>(kPeriod) * 4);
        while (cursor_ < deviceFrame)
        {
            const uint32_t produced = stub->render(block.data(), kPeriod);
            for (uint32_t i = 0; i < kPeriod; ++i)
            {
                uint32_t value = 0; // a short period is the backend's silence
                if (i < produced)
                    std::memcpy(&value, block.data() + static_cast<size_t>(i) * 4, 4);
                output.push_back(value);
            }
            cursor_ += kPeriod;
            stub->clock.deviceFramePosition = cursor_;
            stub->clock.monotonicTimeNs = clockBaseNs_ + cursor_ * 1000000000ULL / kRate;
        }
    }

    // What the room would tell the server now: the device frame at which the
    // timeline's origin kBaseNs falls. Constant for as long as one line lasts.
    int64_t line()
    {
        playback->poll();
        if (!runtime.hasTimelineNsAudible)
            return -1;
        return static_cast<int64_t>(stub->clock.deviceFramePosition) -
               static_cast<int64_t>((runtime.timelineNsAudible - kBaseNs) * kRate / 1000000000ULL);
    }

    bool silent(uint64_t from, uint64_t to) const
    {
        for (uint64_t frame = from; frame < to; ++frame)
        {
            if (output[frame] != 0)
                return false;
        }
        return true;
    }

    uint64_t cursor() const { return cursor_; }

    // Gives the room's device a volume control of its own, as a scan that
    // found one would. Before open(): the control is opened with the device.
    void offerHardwareVolume()
    {
        AudioDeviceInfo device;
        device.id = runtime.deviceId;
        device.name = "Stub";
        device.hardwareVolume.available = true;
        device.hardwareVolume.control = "Master";
        device.hardwareVolume.maxStep = 100;
        device.hardwareVolume.hasMute = true;
        cache.devices.push_back(device);
        runtime.deviceCache = &cache;
        stub->hardwareVolume.offered = true;
    }

    // Connector2's trim: -500 to +500 ppm in whole steps.
    void offerHardwareClock()
    {
        AudioDeviceInfo device;
        device.id = runtime.deviceId;
        device.name = "Stub";
        device.hardwareClock.available = true;
        device.hardwareClock.control = "Drift compensator";
        device.hardwareClock.minPpm = -500;
        device.hardwareClock.maxPpm = 500;
        cache.devices.push_back(device);
        runtime.deviceCache = &cache;
        stub->hardwareClock.offered = true;
    }

    matp::StreamReady lastReady;
    ClientConfig config;
    AudioDeviceCacheData cache;
    ClientRuntime runtime;
    multiroomtest::StubBackend* stub = nullptr; // owned by the playback's engine
    std::unique_ptr<StreamPlayback> playback;
    std::vector<uint32_t> output;

private:
    uint64_t cursor_ = 0;
    uint64_t clockBaseNs_ = 0;
};

// 100 ms of audio from kBaseNs, frames numbered from 1, started so that device
// frame 480 plays kBaseNs, and played up to device frame 1008.
bool playing(Room& room, TestListener& listener, uint64_t& sequence)
{
    if (!room.open(listener) || !sendRun(listener, sequence, kBaseNs, 1, 100))
        return false;
    if (!waitUntil([&] { return room.playback->stream().frames() >= 4800; }, 2000))
        return false;
    if (!room.start(kBaseNs))
        return false;
    room.play(1008);
    return room.output[479] == 0 && room.output[480] == 1;
}

// A seek: the cut C at kBaseNs + 20 ms (device frame 1440) and the audio after
// it from N = kBaseNs + 40 ms, which the kept line puts at device frame 2400.
void seek(bool flushFirst)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));
    const int64_t lineBefore = room.line();
    CHECK_EQ(lineBefore, static_cast<int64_t>(480));

    const uint64_t cutNs = kBaseNs + 20 * kMsNs;
    const uint64_t seekNs = kBaseNs + 40 * kMsNs;
    matp::TransportAt flush;
    flush.timelineNs = cutNs;
    std::string error;

    if (flushFirst)
    {
        CHECK(room.playback->flush(flush, error));
        CHECK(sendRun(listener, sequence, seekNs, 50001, 20, matp::kAudioFlagAfterFlush));
        CHECK(waitUntil([&] { return room.playback->stream().resyncAfterFlush(); }, 2000));
    }
    else
    {
        // The packet overtook its FLUSH: it waits for it, parked.
        CHECK(sendRun(listener, sequence, seekNs, 50001, 20, matp::kAudioFlagAfterFlush));
        CHECK(waitUntil([&] { return room.playback->stream().resyncAfterFlush(); }, 2000));
        room.playback->poll();
        CHECK(room.playback->stream().resyncPending());
        CHECK(room.playback->flush(flush, error));
    }

    // The cut comes round, and the room plays the old line up to it exactly.
    room.play(1488);
    CHECK_EQ(room.output[1439], 960u);
    CHECK(room.silent(1440, 1488));
    room.playback->poll(); // the audio after the cut is placed on the line
    room.play(1536);       // the render callback takes that
    room.playback->poll(); // and the audio connection is let go
    CHECK(!room.playback->stream().resyncPending());
    CHECK(waitUntil([&] { return room.playback->stream().frames() >= 4800 + 960; }, 2000));

    // Between the cut and the audio after it the room reports the cut.
    room.play(2016);
    room.playback->poll();
    CHECK(room.runtime.timelineState == TimelineState::Flushing);
    CHECK(room.runtime.hasTimelineNsAudible);
    CHECK_EQ(room.runtime.timelineNsAudible, cutNs);

    // N plays at the device frame the kept mapping gives it, and not a frame
    // earlier.
    room.play(2592);
    CHECK(room.silent(1440, 2400));
    CHECK_EQ(room.output[2400], 50001u);
    CHECK_EQ(room.output[2591], 50192u);

    // The mapping check of the roadmap: the same before and after the seek.
    CHECK_EQ(room.line(), lineBefore);
    CHECK(room.runtime.timelineState == TimelineState::Playing);
    CHECK_EQ(room.playback->stream().rejoins(), 0u);
    CHECK_EQ(room.playback->stream().flushes(), 1u);
    CHECK_EQ(room.runtime.timelineFlushedFrames, 3840u); // 4800 buffered, 960 played
    CHECK_EQ(room.runtime.timelineUnderrunFrames, 0u);
    CHECK_EQ(room.runtime.timelineSkippedFrames, 0u);
}

matp::SetVolume volumeRequest(double value)
{
    matp::SetVolume request;
    request.hasValue = true;
    request.value = value;
    return request;
}

// Plays and polls until the parked jump is resolved; how long that took.
uint64_t resolve(Room& room)
{
    const uint64_t startedNs = monotonicNowNs();
    waitUntil(
        [&] {
            room.play(room.cursor() + kPeriod);
            room.playback->poll();
            return !room.playback->stream().resyncPending();
        },
        3000);
    return monotonicNowNs() - startedNs;
}

} // namespace

MULTIROOM_TEST(stream_playback_flush_before_its_packet_keeps_the_line)
{
    seek(true);
}

MULTIROOM_TEST(stream_playback_flush_after_its_packet_keeps_the_line)
{
    seek(false);
}

// The boundaries announced for the audio a FLUSH cuts go with it; the server
// announces the replacement's own after the FLUSH.
MULTIROOM_TEST(stream_playback_flush_drops_the_track_boundaries_after_its_cut)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));
    room.playback->poll();

    matp::TrackBoundary boundary;
    boundary.timelineNs = kBaseNs;
    boundary.trackId = "a";
    room.playback->trackBoundary(boundary);
    boundary.timelineNs = kBaseNs + 80 * kMsNs;
    boundary.trackId = "b";
    room.playback->trackBoundary(boundary);

    matp::TransportAt flush;
    flush.timelineNs = kBaseNs + 20 * kMsNs;
    std::string error;
    CHECK(room.playback->flush(flush, error));
    CHECK(sendRun(listener, sequence, kBaseNs + 40 * kMsNs, 50001, 60,
                  matp::kAudioFlagAfterFlush));
    boundary.timelineNs = kBaseNs + 120 * kMsNs;
    room.playback->trackBoundary(boundary);
    CHECK(waitUntil([&] { return room.playback->stream().resyncAfterFlush(); }, 2000));

    room.play(1488);
    room.playback->poll();
    room.play(1536);
    room.playback->poll();
    CHECK(waitUntil([&] { return room.playback->stream().frames() >= 4800 + 2880; }, 2000));

    // Timeline kBaseNs + 90 ms: past the dropped 80 ms boundary, short of 120.
    room.play(4800);
    room.playback->poll();
    CHECK_EQ(room.output[4799], 52400u);
    CHECK(room.runtime.trackKnown);
    CHECK_EQ(room.runtime.trackId, std::string("a"));
    CHECK_EQ(room.runtime.pendingTrackBoundaries, 1u);
}

MULTIROOM_TEST(stream_playback_flagged_packet_without_a_flush_rejoins_after_a_second)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));

    CHECK(sendRun(listener, sequence, kBaseNs + 40 * kMsNs, 50001, 1,
                  matp::kAudioFlagAfterFlush));
    CHECK(waitUntil([&] { return room.playback->stream().resyncAfterFlush(); }, 2000));

    // A server that flags a packet and never sends the FLUSH would otherwise
    // leave the room silent for good.
    const uint64_t tookNs = resolve(room);
    CHECK(tookNs >= 900 * kMsNs);
    CHECK(!room.playback->stream().resyncPending());
    CHECK_EQ(room.playback->stream().rejoins(), 1u);
    CHECK_EQ(room.playback->stream().flushes(), 0u);
    CHECK(room.runtime.timelineState == TimelineState::Rejoining);
    CHECK(!room.runtime.startScheduled);
}

// The FLUSH reached a prepared room before any audio did, and its cut has
// already been taken: the flagged first packet has no line to keep, so it
// rejoins (section 54) rather than leaving the room flushing for good.
MULTIROOM_TEST(stream_playback_flush_before_the_first_packet_rejoins)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    uint64_t sequence = 0;
    CHECK(room.open(listener));

    matp::TransportAt flush;
    flush.timelineNs = kBaseNs;
    std::string error;
    CHECK(room.playback->flush(flush, error));
    room.play(480); // the render callback takes the cut

    CHECK(sendRun(listener, sequence, kBaseNs, 1, 20, matp::kAudioFlagAfterFlush));
    CHECK(waitUntil([&] { return room.playback->stream().resyncAfterFlush(); }, 2000));

    resolve(room);
    CHECK(!room.playback->stream().resyncPending());
    CHECK_EQ(room.playback->stream().rejoins(), 1u);
    CHECK_EQ(room.playback->stream().flushes(), 0u);
    CHECK(room.runtime.timelineState == TimelineState::Rejoining);

    // The replacement audio is kept and plays from where the server starts it.
    CHECK(waitUntil([&] { return room.playback->stream().frames() >= 960; }, 2000));
    CHECK(room.start(kBaseNs));
    const uint64_t startFrame = room.cursor() + 480;
    room.play(startFrame + kPeriod);
    CHECK_EQ(room.output[startFrame], 1u);
}

MULTIROOM_TEST(stream_playback_unflagged_jump_is_still_a_rejoin)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));

    CHECK(sendRun(listener, sequence, kBaseNs + 40 * kMsNs, 50001, 1));
    CHECK(waitUntil([&] { return room.playback->stream().resyncPending(); }, 2000));
    CHECK(!room.playback->stream().resyncAfterFlush());

    const uint64_t tookNs = resolve(room);
    CHECK(tookNs < 900 * kMsNs);
    CHECK_EQ(room.playback->stream().rejoins(), 1u);
    CHECK_EQ(room.playback->stream().flushes(), 0u);
    CHECK(room.runtime.timelineState == TimelineState::Rejoining);
}

// SET_BUFFER applied to a running stream (MATP section 58). The room's ring
// buffer is 1200 ms rounded up to 65,536 frames: 1300 ms rounds to the same
// buffer and changes nothing, 3000 ms does not and reopens the device with a
// buffer of that size.
MULTIROOM_TEST(stream_playback_applies_a_new_buffer_size_at_once)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    CHECK(room.open(listener));
    CHECK_EQ(room.playback->stream().capacityFrames(), static_cast<uint32_t>(65536));
    const int opens = room.stub->openCalls;

    std::string error;
    CHECK(room.playback->bufferFits(1300, error));
    room.playback->setBufferMs(1300);
    CHECK_EQ(room.stub->openCalls, opens);
    CHECK_EQ(room.playback->stream().capacityFrames(), static_cast<uint32_t>(65536));

    room.playback->setBufferMs(3000);
    CHECK_EQ(room.stub->openCalls, opens + 1);
    CHECK(room.playback->stream().isAttached());
    CHECK_EQ(room.playback->stream().capacityFrames(), static_cast<uint32_t>(262144));
}

MULTIROOM_TEST(stream_playback_reopened_device_rebinds_on_a_fresh_connection)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    CHECK(room.open(listener));

    uint64_t sequence = 0;
    CHECK(sendRun(listener, sequence, kBaseNs, 0, 20));
    CHECK(waitUntil(
        [&] {
            room.playback->poll();
            return room.playback->stream().packets() == 20;
        },
        2000));

    // Half a packet is on the wire when the device is reopened. Carrying on
    // decoding that socket would begin in the middle of the packet, so the
    // stream gives it up and binds the next connection instead.
    const std::string inFlight = packetAt(sequence++, kBaseNs + 20 * kMsNs, 20 * kPeriod);
    CHECK(listener.send(inFlight.substr(0, inFlight.size() / 2)));
    room.playback->setBufferMs(3000);
    CHECK(room.playback->stream().isAttached());

    CHECK(listener.acceptOne(5000));
    CHECK(waitUntil(
        [&] {
            room.playback->poll();
            return !listener.receive(20).empty();
        },
        5000));

    // The server resumes on the new socket, and the audio lands.
    CHECK(sendRun(listener, sequence, kBaseNs + 21 * kMsNs, 21 * kPeriod, 5));
    CHECK(waitUntil(
        [&] {
            room.playback->poll();
            return room.playback->stream().packets() == 5;
        },
        2000));
    CHECK(room.playback->stream().lastError().empty());
}

matp::DriftFeedback driftTarget(uint64_t sequence, double ppm)
{
    matp::DriftFeedback feedback;
    feedback.sequence = sequence;
    feedback.hasDrift = true;
    feedback.measuredDriftPpm = 3.5;
    feedback.driftConfidence = 0.9;
    feedback.hasHardwareTarget = true;
    feedback.hardwareTargetPpm = ppm;
    return feedback;
}

MULTIROOM_TEST(stream_playback_resampled_room_resets_and_never_drives_the_clock)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.offerHardwareClock();
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));

    // Back to nominal on open, whatever a previous run left; ready for the
    // server's resampling only.
    CHECK_EQ(room.stub->hardwareClock.ppm, 0);
    CHECK(room.lastReady.hasDriftCorrection);
    CHECK(room.lastReady.driftCorrection == matp::DriftCorrectionMode::Resampling);

    // A target that reaches a resampled room is never written.
    const int sets = room.stub->hardwareClock.sets;
    room.playback->driftFeedback(driftTarget(1, 25.0));
    CHECK_EQ(room.stub->hardwareClock.sets, sets);
    CHECK_EQ(room.stub->hardwareClock.ppm, 0);
    CHECK(room.runtime.driftCorrection.hasServerDrift);
    CHECK_EQ(room.runtime.driftCorrection.serverDriftPpm, 3.5);
    matp::PlaybackReport report;
    room.playback->poll();
    CHECK(room.playback->playbackReport(report));
    CHECK(!report.hasHardwareClock);
}

MULTIROOM_TEST(stream_playback_hardware_clock_follows_the_server_target)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.offerHardwareClock();
    room.playback->setDriftCorrection(matp::DriftCorrectionMode::HardwareClock);
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));
    CHECK(room.lastReady.driftCorrection == matp::DriftCorrectionMode::HardwareClock);
    CHECK(room.runtime.driftCorrection.hardwareClockActive);

    room.playback->driftFeedback(driftTarget(1, 12.4));
    CHECK_EQ(room.stub->hardwareClock.ppm, 12);

    // Within three quarters of a step: no write.
    const int sets = room.stub->hardwareClock.sets;
    room.playback->driftFeedback(driftTarget(2, 12.6));
    CHECK_EQ(room.stub->hardwareClock.sets, sets);

    // Beyond the device: its limit, and said so.
    room.playback->driftFeedback(driftTarget(3, -900.0));
    CHECK_EQ(room.stub->hardwareClock.ppm, -500);
    room.playback->poll();
    matp::PlaybackReport report;
    CHECK(room.playback->playbackReport(report));
    CHECK(report.hasHardwareClock);
    CHECK_EQ(report.hardwareClockPpm, -500);
    CHECK_EQ(report.hardwareClockSequence, static_cast<uint64_t>(3));
    CHECK(report.hardwareClockSaturated);

    // Deselected: nominal at once, and targets are ignored from then on.
    room.playback->setDriftCorrection(matp::DriftCorrectionMode::Resampling);
    CHECK_EQ(room.stub->hardwareClock.ppm, 0);
    room.playback->driftFeedback(driftTarget(4, 40.0));
    CHECK_EQ(room.stub->hardwareClock.ppm, 0);
}

MULTIROOM_TEST(stream_playback_failed_clock_write_returns_to_nominal_and_reports)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.offerHardwareClock();
    room.playback->setDriftCorrection(matp::DriftCorrectionMode::HardwareClock);
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));
    room.playback->driftFeedback(driftTarget(1, 20.0));
    CHECK_EQ(room.stub->hardwareClock.ppm, 20);

    // The write fails and so does the reset: the rate is unknown, which the
    // report says, and the clock takes no further command.
    room.stub->hardwareClock.failSet = true;
    room.playback->driftFeedback(driftTarget(2, 30.0));
    CHECK(!room.runtime.driftCorrection.hardwareClockActive);
    room.playback->poll();
    matp::PlaybackReport report;
    CHECK(room.playback->playbackReport(report));
    CHECK(!report.hardwareClockError.empty());

    // A device it cannot reset to nominal is not ready for either actuator.
    matp::PrepareStream request;
    request.hasAudioPort = true;
    request.audioPort = listener.port();
    matp::StreamReady ready;
    room.playback->prepare(request, roomFormat(), ready);
    CHECK(!ready.ready);
    CHECK(ready.reason.find("nominal") != std::string::npos);
}

MULTIROOM_TEST(stream_playback_hardware_volume_leaves_the_samples_unscaled)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.offerHardwareVolume();
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));

    // Opened with the device, and left where its owner had it: the server has
    // not asked for a volume yet.
    CHECK_EQ(room.stub->hardwareVolume.opens, 1);
    CHECK_EQ(room.stub->hardwareVolume.sets, 0);
    room.playback->poll();
    CHECK(room.runtime.hardwareVolumeActive);
    CHECK(room.runtime.hasHardwareVolumeState);
    CHECK_EQ(room.runtime.hardwareVolumeState.gain, 0.5);

    std::string error;
    CHECK(room.playback->volume(volumeRequest(0.25), error));
    CHECK_EQ(room.stub->hardwareVolume.sets, 1);
    CHECK_EQ(room.stub->hardwareVolume.gain, 0.25);

    // The device is handed what the server sent, frame for frame.
    room.play(3008);
    CHECK_EQ(room.output[1007], 528u);
    CHECK_EQ(room.output[3007], 2528u);
    room.playback->poll();
    CHECK_EQ(room.runtime.volumeGain, 0.25);
    CHECK_EQ(room.runtime.appliedGain, 1.0);
    CHECK_EQ(room.runtime.hardwareVolumeState.gain, 0.25);
}

MULTIROOM_TEST(stream_playback_hardware_volume_waits_for_its_frame)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.offerHardwareVolume();
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));

    // kBaseNs + 30 ms plays at device frame 480 + 1440.
    matp::SetVolume request = volumeRequest(0.1);
    request.hasApplyAtTimelineNs = true;
    request.applyAtTimelineNs = kBaseNs + 30 * kMsNs;
    std::string error;
    CHECK(room.playback->volume(request, error));

    room.play(1872);
    room.playback->poll();
    CHECK_EQ(room.stub->hardwareVolume.sets, 0);
    CHECK(room.runtime.volumeScheduled);

    room.play(1920);
    room.playback->poll();
    CHECK_EQ(room.stub->hardwareVolume.sets, 1);
    CHECK_EQ(room.stub->hardwareVolume.gain, 0.1);
    CHECK(!room.runtime.volumeScheduled);
    CHECK_EQ(room.output[1919], 1440u);
}

MULTIROOM_TEST(stream_playback_hardware_volume_waits_through_a_pause_for_its_position)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.offerHardwareVolume();
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));

    // Armed for kBaseNs + 30 ms (device frame 1920), behind a pause at
    // kBaseNs + 20 ms (device frame 1440).
    matp::SetVolume request = volumeRequest(0.1);
    request.hasApplyAtTimelineNs = true;
    request.applyAtTimelineNs = kBaseNs + 30 * kMsNs;
    std::string error;
    CHECK(room.playback->volume(request, error));
    matp::TransportAt pause;
    pause.timelineNs = kBaseNs + 20 * kMsNs;
    CHECK(room.playback->pause(pause, error));

    // The device runs past 1920; the timeline does not.
    room.play(2400);
    room.playback->poll();
    CHECK(room.runtime.timelineState == TimelineState::Paused);
    CHECK_EQ(room.stub->hardwareVolume.sets, 0);
    CHECK(room.runtime.volumeScheduled);

    // Resumed at device frame 2880, which puts kBaseNs + 30 ms at 3360.
    CHECK(room.start(kBaseNs + 20 * kMsNs));
    room.play(3312);
    room.playback->poll();
    CHECK_EQ(room.stub->hardwareVolume.sets, 0);

    room.play(3360);
    room.playback->poll();
    CHECK_EQ(room.stub->hardwareVolume.sets, 1);
    CHECK_EQ(room.stub->hardwareVolume.gain, 0.1);
    CHECK(!room.runtime.volumeScheduled);
    CHECK_EQ(room.output[3359], 1440u);
}

MULTIROOM_TEST(stream_playback_refused_hardware_volume_falls_back_to_software)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.runtime.audioControl = matp::AudioControl::Auto;
    room.offerHardwareVolume();
    room.stub->hardwareVolume.failSet = true;
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));

    std::string error;
    CHECK(room.playback->volume(volumeRequest(0.5), error));
    room.playback->poll();
    CHECK(!room.runtime.hardwareVolumeActive);
    CHECK(room.runtime.volumePath.effective == matp::VolumeControlMode::Software);

    // Past the ramp, the render path halves what it plays.
    room.play(3008);
    CHECK_EQ(room.output[3007], 1264u);
    CHECK_EQ(room.runtime.volumeGain, 0.5);
}

MULTIROOM_TEST(stream_playback_hardware_mute_and_zero_gain_are_silent)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.runtime.audioControl = matp::AudioControl::Auto;
    room.offerHardwareVolume();
    room.cache.devices[0].hardwareVolume.hasMute = false; // its lowest step is all it has
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));
    CHECK(room.runtime.hardwareVolumeActive);

    matp::SetVolume mute;
    mute.hasMuted = true;
    mute.muted = true;
    std::string error;
    CHECK(room.playback->volume(mute, error));
    room.play(2400);
    CHECK_EQ(room.output[2399], 0u);

    // Unmuted, the device plays at its level again, the samples untouched.
    mute.muted = false;
    CHECK(room.playback->volume(mute, error));
    room.play(3408);
    CHECK_EQ(room.output[3407], 2928u);

    CHECK(room.playback->volume(volumeRequest(0.0), error));
    room.play(4416);
    CHECK_EQ(room.output[4415], 0u);
}

MULTIROOM_TEST(stream_playback_giving_up_hardware_volume_resets_its_level)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.runtime.audioControl = matp::AudioControl::Auto;
    room.offerHardwareVolume();
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));

    std::string error;
    CHECK(room.playback->volume(volumeRequest(0.25), error));
    CHECK_EQ(room.stub->hardwareVolume.gain, 0.25);

    // One write refused: the control is given up, but put back to 0 dB first,
    // so the software path does not play under its attenuation.
    room.stub->hardwareVolume.failNext = 1;
    CHECK(room.playback->volume(volumeRequest(0.5), error));
    CHECK(room.runtime.volumePath.effective == matp::VolumeControlMode::Software);
    CHECK_EQ(room.stub->hardwareVolume.gain, 1.0);
    CHECK(!room.stub->hardwareVolume.muted);
    room.play(3008);
    CHECK_EQ(room.output[3007], 1264u);
}

MULTIROOM_TEST(stream_playback_refused_unmute_keeps_the_hardware_control)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.runtime.audioControl = matp::AudioControl::Auto;
    room.offerHardwareVolume();
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));

    matp::SetVolume mute;
    mute.hasMuted = true;
    mute.muted = true;
    std::string error;
    CHECK(room.playback->volume(mute, error));
    CHECK(room.stub->hardwareVolume.muted);

    // The device stays muted, and no other path can make the room heard:
    // the control is kept, and the status shows what it says.
    room.stub->hardwareVolume.failSet = true;
    matp::SetVolume unmute;
    unmute.hasMuted = true;
    unmute.muted = false;
    CHECK(room.playback->volume(unmute, error));
    room.playback->poll();
    CHECK(room.runtime.volumePath.effective == matp::VolumeControlMode::Hardware);
    CHECK(room.runtime.hardwareVolumeActive);
    CHECK(room.runtime.hasHardwareVolumeState);
    CHECK(room.runtime.hardwareVolumeState.muted);

    // The next change tries it again.
    room.stub->hardwareVolume.failSet = false;
    CHECK(room.playback->volume(unmute, error));
    CHECK(!room.stub->hardwareVolume.muted);
}

// Disabled is Disabled with or without the control, so giving the switch up is
// no path change -- and the mute it refused still has to silence the room.
MULTIROOM_TEST(stream_playback_refused_mute_under_disabled_silences_the_samples)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.config.volumeControl = matp::VolumeControlMode::Disabled;
    room.offerHardwareVolume();
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));
    CHECK(room.runtime.volumePath.effective == matp::VolumeControlMode::Disabled);

    // The mute refused, the reset to unmuted made: the control is given up.
    room.stub->hardwareVolume.failNext = 1;
    matp::SetVolume mute;
    mute.hasMuted = true;
    mute.muted = true;
    std::string error;
    CHECK(room.playback->volume(mute, error));
    room.playback->poll();
    CHECK(room.runtime.volumePath.effective == matp::VolumeControlMode::Disabled);
    CHECK(!room.runtime.hasHardwareVolumeState);
    CHECK(!room.stub->hardwareVolume.muted);
    CHECK(room.runtime.volumeMuted);
    room.play(2400);
    CHECK_EQ(room.output[2399], 0u);

    // Unmuted, the samples play untouched: Disabled plays no gain.
    mute.muted = false;
    CHECK(room.playback->volume(mute, error));
    room.play(3408);
    CHECK_EQ(room.output[3407], 2928u);
}

MULTIROOM_TEST(stream_playback_volume_during_device_recovery_is_kept)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.runtime.audioControl = matp::AudioControl::Auto;
    room.offerHardwareVolume();
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));

    std::string error;
    CHECK(room.playback->volume(volumeRequest(0.25), error));
    room.stub->simulateDeviceLoss("device lost");
    room.playback->poll(); // closed, reopened on the supervisor's backoff

    CHECK(room.playback->volume(volumeRequest(0.1), error));
    CHECK_EQ(room.runtime.volumeGain, 0.1);
    CHECK(waitUntil(
        [&] {
            room.playback->poll();
            return room.stub->hardwareVolume.gain == 0.1;
        },
        3000));
}

MULTIROOM_TEST(stream_playback_leaving_hardware_volume_resets_its_level)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.runtime.audioControl = matp::AudioControl::Auto;
    room.offerHardwareVolume();
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));

    std::string error;
    CHECK(room.playback->volume(volumeRequest(0.25), error));
    CHECK_EQ(room.stub->hardwareVolume.gain, 0.25);

    // The software path plays the gain, so the device's control goes back to
    // 0 dB rather than attenuating a second time.
    room.config.volumeControl = matp::VolumeControlMode::Software;
    room.playback->updateVolumePath();
    CHECK(room.runtime.volumePath.effective == matp::VolumeControlMode::Software);
    CHECK_EQ(room.stub->hardwareVolume.gain, 1.0);
    CHECK(!room.stub->hardwareVolume.muted);
    room.play(3008);
    CHECK_EQ(room.output[3007], 632u);
}

// A reset that fails on the way out of the hardware path is owed, not
// forgotten: the next volume change makes it once the control works again.
MULTIROOM_TEST(stream_playback_failed_reset_leaving_hardware_volume_is_retried)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.runtime.audioControl = matp::AudioControl::Auto;
    room.offerHardwareVolume();
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));

    std::string error;
    matp::SetVolume request = volumeRequest(0.25);
    request.hasMuted = true;
    request.muted = true;
    CHECK(room.playback->volume(request, error));
    CHECK(room.stub->hardwareVolume.muted);

    // The reset and the path's own applyVolume() retry both refused.
    room.stub->hardwareVolume.failNext = 2;
    room.config.volumeControl = matp::VolumeControlMode::Software;
    room.playback->updateVolumePath();
    CHECK(room.runtime.volumePath.effective == matp::VolumeControlMode::Software);
    CHECK(room.stub->hardwareVolume.muted);

    matp::SetVolume unmute;
    unmute.hasMuted = true;
    unmute.muted = false;
    CHECK(room.playback->volume(unmute, error));
    CHECK(!room.stub->hardwareVolume.muted);
    CHECK_EQ(room.stub->hardwareVolume.gain, 1.0);
    room.play(3008);
    CHECK_EQ(room.output[3007], 632u);
}

// Disabled plays the mute on the device's switch: the owed level reset, made
// later by the status poll, keeps that mute on.
MULTIROOM_TEST(stream_playback_owed_reset_into_disabled_keeps_the_mute)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.runtime.audioControl = matp::AudioControl::Auto;
    room.offerHardwareVolume();
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));

    std::string error;
    matp::SetVolume request = volumeRequest(0.25);
    request.hasMuted = true;
    request.muted = true;
    CHECK(room.playback->volume(request, error));

    // The reset and its retry refuse; Disabled's own mute goes through.
    room.stub->hardwareVolume.failNext = 2;
    room.config.volumeControl = matp::VolumeControlMode::Disabled;
    room.playback->updateVolumePath();
    CHECK(room.runtime.volumePath.effective == matp::VolumeControlMode::Disabled);
    CHECK(room.stub->hardwareVolume.muted);
    CHECK_EQ(room.stub->hardwareVolume.gain, 0.25);

    room.playback->poll();
    CHECK_EQ(room.stub->hardwareVolume.gain, 1.0);
    CHECK(room.stub->hardwareVolume.muted);
    CHECK(room.runtime.volumeMuted);
}

// The owed level reset keeps the mute in force, not a scheduled one: that
// waits for its frame, in either direction.
MULTIROOM_TEST(stream_playback_owed_reset_into_disabled_keeps_a_scheduled_mute_waiting)
{
    for (const bool initiallyMuted : {true, false})
    for (const bool resetFailsAtPoll : {false, true})
    {
        TestListener listener;
        CHECK(listener.ready());
        Room room;
        room.runtime.audioControl = matp::AudioControl::Auto;
        room.offerHardwareVolume();
        uint64_t sequence = 0;
        CHECK(playing(room, listener, sequence));

        std::string error;
        matp::SetVolume request = volumeRequest(0.25);
        request.hasMuted = true;
        request.muted = initiallyMuted;
        CHECK(room.playback->volume(request, error));

        room.stub->hardwareVolume.failNext = 2;
        room.config.volumeControl = matp::VolumeControlMode::Disabled;
        room.playback->updateVolumePath();
        CHECK_EQ(room.stub->hardwareVolume.muted, initiallyMuted);

        // kBaseNs + 80 ms plays at device frame 480 + 3840.
        matp::SetVolume later;
        later.hasMuted = true;
        later.muted = !initiallyMuted;
        later.hasApplyAtTimelineNs = true;
        later.applyAtTimelineNs = kBaseNs + 80 * kMsNs;
        CHECK(room.playback->volume(later, error));

        // Also recover at the scheduled frame when the earlier poll's reset
        // fails. The mute already in force must survive either recovery time.
        room.stub->hardwareVolume.failNext = resetFailsAtPoll ? 1 : 0;
        room.playback->poll();
        CHECK(room.cursor() < 4320);
        CHECK(room.runtime.volumeScheduled);
        CHECK_EQ(room.stub->hardwareVolume.gain, resetFailsAtPoll ? 0.25 : 1.0);
        CHECK_EQ(room.stub->hardwareVolume.muted, initiallyMuted);

        room.play(4320 - kPeriod);
        room.playback->poll();
        CHECK(room.runtime.volumeScheduled);
        CHECK_EQ(room.stub->hardwareVolume.muted, initiallyMuted);

        room.play(4320);
        room.playback->poll();
        CHECK(!room.runtime.volumeScheduled);
        CHECK_EQ(room.stub->hardwareVolume.gain, 1.0);
        CHECK_EQ(room.stub->hardwareVolume.muted, !initiallyMuted);

        const int writes = room.stub->hardwareVolume.sets + room.stub->hardwareVolume.mutes;
        room.play(4320 + kPeriod);
        room.playback->poll();
        CHECK(!room.runtime.volumeScheduled);
        CHECK_EQ(room.stub->hardwareVolume.muted, !initiallyMuted);
        CHECK_EQ(room.stub->hardwareVolume.sets + room.stub->hardwareVolume.mutes, writes);
    }
}

// A level still owed when the path changes again is not traded for a mute
// reset that would leave the attenuation in place.
MULTIROOM_TEST(stream_playback_owed_level_reset_survives_a_further_path_change)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.runtime.audioControl = matp::AudioControl::Auto;
    room.offerHardwareVolume();
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));

    std::string error;
    matp::SetVolume request = volumeRequest(0.25);
    request.hasMuted = true;
    request.muted = true;
    CHECK(room.playback->volume(request, error));

    room.stub->hardwareVolume.failNext = 2;
    room.config.volumeControl = matp::VolumeControlMode::Disabled;
    room.playback->updateVolumePath();
    CHECK_EQ(room.stub->hardwareVolume.gain, 0.25);

    room.config.volumeControl = matp::VolumeControlMode::Software;
    room.playback->updateVolumePath();
    CHECK(room.runtime.volumePath.effective == matp::VolumeControlMode::Software);
    CHECK_EQ(room.stub->hardwareVolume.gain, 1.0);
    CHECK(!room.stub->hardwareVolume.muted);
}

MULTIROOM_TEST(stream_playback_bitperfect_never_scales_even_when_the_hardware_refuses)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.runtime.audioControl = matp::AudioControl::BitPerfect;
    room.offerHardwareVolume();
    room.stub->hardwareVolume.failSet = true;
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));

    std::string error;
    CHECK(room.playback->volume(volumeRequest(0.5), error));
    room.playback->poll();

    // The control gone, bitPerfect leaves nothing but Disabled -- and says so.
    CHECK(room.runtime.volumePath.effective == matp::VolumeControlMode::Disabled);
    CHECK(!room.runtime.volumePath.reason.empty());
    room.play(3008);
    CHECK_EQ(room.output[3007], 2528u);
    CHECK_EQ(room.runtime.appliedGain, 1.0);
}

MULTIROOM_TEST(stream_playback_disabled_plays_the_mute_and_not_the_gain)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.runtime.audioControl = matp::AudioControl::Auto;
    room.config.volumeControl = matp::VolumeControlMode::Disabled;
    room.offerHardwareVolume();
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));
    CHECK(room.runtime.volumePath.effective == matp::VolumeControlMode::Disabled);

    // The gain is not played anywhere.
    std::string error;
    CHECK(room.playback->volume(volumeRequest(0.25), error));
    CHECK_EQ(room.stub->hardwareVolume.sets, 0);
    CHECK_EQ(room.stub->hardwareVolume.gain, 0.5);

    // The mute is, on the device's own switch, level untouched.
    matp::SetVolume mute;
    mute.hasMuted = true;
    mute.muted = true;
    CHECK(room.playback->volume(mute, error));
    CHECK(room.stub->hardwareVolume.mutes > 0);
    CHECK(room.stub->hardwareVolume.muted);
    CHECK_EQ(room.stub->hardwareVolume.sets, 0);
    room.play(3008);
    CHECK_EQ(room.output[3007], 2528u);
}

MULTIROOM_TEST(stream_playback_disabled_without_a_switch_mutes_by_silence)
{
    TestListener listener;
    CHECK(listener.ready());
    Room room;
    room.runtime.audioControl = matp::AudioControl::BitPerfect;
    room.config.volumeControl = matp::VolumeControlMode::Disabled;
    uint64_t sequence = 0;
    CHECK(playing(room, listener, sequence));

    std::string error;
    CHECK(room.playback->volume(volumeRequest(0.25), error));
    room.play(2016);
    CHECK_EQ(room.output[2015], 1536u); // the gain is not played

    matp::SetVolume mute;
    mute.hasMuted = true;
    mute.muted = true;
    CHECK(room.playback->volume(mute, error));
    room.play(3008);
    CHECK_EQ(room.output[3007], 0u);
}
