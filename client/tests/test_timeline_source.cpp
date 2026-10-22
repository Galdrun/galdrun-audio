#include "TestSupport.h"

#include "audio/AudioRingBuffer.h"
#include "audio/TimelineSource.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

using namespace multiroom;

namespace {

// One uint32 per frame, so a rendered frame carries its own timeline position
// and a skipped or duplicated one is visible immediately.
const uint32_t kFrameBytes = 4;

AudioFormat monoInt16(uint32_t rate)
{
    AudioFormat format;
    format.sampleRate = rate;
    format.channels = 2;
    format.sampleType = AudioFormat::SampleType::Int16; // 2 channels x 2 bytes
    return format;
}

// Pushes `count` frames whose values continue from `first`.
void pushFrames(AudioRingBuffer& buffer, uint32_t first, uint32_t count)
{
    std::vector<unsigned char> block(static_cast<size_t>(count) * kFrameBytes);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t value = first + i;
        std::memcpy(block.data() + static_cast<size_t>(i) * kFrameBytes, &value, kFrameBytes);
    }
    CHECK_EQ(buffer.push(block.data(), count), count);
}

uint32_t frameAt(const std::vector<unsigned char>& block, uint32_t index)
{
    uint32_t value = 0;
    std::memcpy(&value, block.data() + static_cast<size_t>(index) * kFrameBytes, kFrameBytes);
    return value;
}

bool isSilent(const std::vector<unsigned char>& block, uint32_t from, uint32_t to)
{
    for (uint32_t i = from; i < to; ++i)
    {
        if (frameAt(block, i) != 0)
            return false;
    }
    return true;
}

const uint64_t kMsNs = 1000000ULL;

// Somewhere in the middle of a monotonic clock's life: a snapshot taken at
// zero is not a snapshot, it is an unset one.
const uint64_t kBaseNs = 3600ULL * 1000ULL * kMsNs;

uint64_t startAt(uint64_t elapsedNs)
{
    return kBaseNs + elapsedNs;
}

// A device that reports its own position, which is what a schedule is placed
// against: 48 kHz, so a millisecond is 48 frames.
AudioClockSnapshot snapshotAt(uint64_t deviceFrame, uint64_t elapsedNs)
{
    AudioClockSnapshot snapshot;
    snapshot.deviceFramePosition = deviceFrame;
    snapshot.monotonicTimeNs = kBaseNs + elapsedNs;
    snapshot.nominalSampleRate = 48000;
    snapshot.quality = AudioClockQuality::Hardware;
    return snapshot;
}

} // namespace

MULTIROOM_TEST(timeline_source_plays_silence_until_a_start_is_scheduled)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 1, 64);

    std::vector<unsigned char> out(64 * kFrameBytes, 0xAB);
    // The whole period is filled by the source itself, so the backend counts
    // no underrun: this silence is deliberate, not a buffer that ran dry.
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK(isSilent(out, 0, 64));

    // ...and the buffer was not touched, which is what lets the prebuffer fill
    // while the device is already running (MATP sections 18 and 21).
    CHECK_EQ(buffer.readableFrames(), 64u);
    CHECK(source.state() == TimelineState::Idle);
    CHECK_EQ(source.underrunFrames(), 0u);
    CHECK_EQ(source.submittedFrames(), 64u);
}

MULTIROOM_TEST(timeline_source_starts_on_the_scheduled_device_frame)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 1000, 512);

    // The device has played 480 frames and the start is 10 ms away: frame 960.
    std::string error;
    CHECK(source.scheduleStart(snapshotAt(480, 100 * kMsNs), startAt(110 * kMsNs), 1000, 1000,
                               error));
    CHECK_EQ(source.startDeviceFrame(), 960u);
    CHECK_EQ(source.startStreamFrame(), 1000u);
    CHECK(source.state() == TimelineState::Waiting);

    std::vector<unsigned char> out(480 * kFrameBytes, 0xAB);
    CHECK_EQ(source.render(out.data(), 480), 480u); // frames 0..479: silence
    CHECK(isSilent(out, 0, 480));
    CHECK_EQ(buffer.readableFrames(), 512u);

    CHECK_EQ(source.render(out.data(), 480), 480u); // frames 480..959: silence
    CHECK(isSilent(out, 0, 480));
    CHECK(source.state() == TimelineState::Waiting);

    // Frame 960 is the first audible one, and it is timeline frame 1000.
    CHECK_EQ(source.render(out.data(), 480), 480u);
    CHECK_EQ(frameAt(out, 0), 1000u);
    CHECK_EQ(frameAt(out, 479), 1479u);
    CHECK(source.state() == TimelineState::Playing);
    CHECK_EQ(source.underrunFrames(), 0u);
}

MULTIROOM_TEST(timeline_source_starts_inside_a_period)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 7000, 512);

    // 2.5 ms away: device frame 120, which is a third of the way into the
    // second period. The start lands on the frame, not on the period.
    std::string error;
    CHECK(source.scheduleStart(snapshotAt(0, 0), startAt(2500000ULL), 7000, 7000, error));
    CHECK_EQ(source.startDeviceFrame(), 120u);

    std::vector<unsigned char> out(96 * kFrameBytes, 0xAB);
    CHECK_EQ(source.render(out.data(), 96), 96u); // frames 0..95, all silence
    CHECK(isSilent(out, 0, 96));

    CHECK_EQ(source.render(out.data(), 96), 96u); // frames 96..191
    CHECK(isSilent(out, 0, 24));                  // up to device frame 119
    CHECK_EQ(frameAt(out, 24), 7000u);            // device frame 120
    CHECK_EQ(frameAt(out, 95), 7071u);
}

MULTIROOM_TEST(timeline_source_skips_audio_buffered_before_the_scheduled_frame)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 500, 256); // timeline 500..755

    // Start at timeline frame 600: the 100 frames before it have already been
    // buffered and are dropped rather than played.
    std::string error;
    CHECK(source.scheduleStart(snapshotAt(0, 0), startAt(1 * kMsNs), 600, 500, error));

    std::vector<unsigned char> out(96 * kFrameBytes, 0xAB);
    CHECK_EQ(source.render(out.data(), 96), 96u);
    CHECK_EQ(frameAt(out, 48), 600u);
    CHECK_EQ(source.skippedFrames(), 100u);
    CHECK_EQ(source.underrunFrames(), 0u);
}

MULTIROOM_TEST(timeline_source_refuses_a_frame_that_is_not_buffered)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 500, 256); // timeline 500..755

    std::string error;
    // Older than anything the buffer still holds: only a new stream position
    // can fix that, which is what a rejoin is for.
    CHECK(!source.scheduleStart(snapshotAt(0, 0), startAt(1 * kMsNs), 499, 500, error));
    CHECK(!error.empty());
    // Past the end of what has arrived.
    CHECK(!source.scheduleStart(snapshotAt(0, 0), startAt(1 * kMsNs), 800, 500, error));
    CHECK(!error.empty());
    CHECK(source.state() == TimelineState::Idle);
    CHECK(!source.isScheduled());
}

MULTIROOM_TEST(timeline_source_refuses_a_start_it_cannot_place)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 0, 256);

    std::string error;
    // A device with no usable position: there is no frame axis to put the
    // start on, and counting submitted frames instead is what section 48
    // forbids.
    AudioClockSnapshot unusable;
    CHECK(!source.scheduleStart(unusable, startAt(10 * kMsNs), 0, 0, error));

    // Already past.
    CHECK(!source.scheduleStart(snapshotAt(0, 10 * kMsNs), startAt(9 * kMsNs), 0, 0, error));
    // Further away than any schedule this protocol produces.
    CHECK(!source.scheduleStart(snapshotAt(0, 0), startAt(120000ULL * kMsNs), 0, 0, error));
    CHECK(!source.isScheduled());
}

MULTIROOM_TEST(timeline_source_refuses_a_start_inside_audio_already_submitted)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 0, 256);

    // Four periods of silence have gone to the device while the driver still
    // reports position 0, so the next 1920 frames are already spoken for.
    std::vector<unsigned char> out(480 * kFrameBytes, 0);
    for (int i = 0; i < 4; ++i)
        CHECK_EQ(source.render(out.data(), 480), 480u);
    CHECK_EQ(source.submittedFrames(), 1920u);

    std::string error;
    CHECK(!source.scheduleStart(snapshotAt(0, 0), startAt(10 * kMsNs), 0, 0, error)); // frame 480
    CHECK(source.scheduleStart(snapshotAt(0, 0), startAt(50 * kMsNs), 0, 0, error));  // frame 2400
    CHECK_EQ(source.startDeviceFrame(), 2400u);
}

MULTIROOM_TEST(timeline_source_stays_on_the_timeline_across_an_underrun)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 100, 48); // one millisecond of audio

    std::string error;
    CHECK(source.scheduleStart(snapshotAt(0, 0), startAt(1 * kMsNs), 100, 100, error));

    std::vector<unsigned char> out(96 * kFrameBytes, 0xAB);
    // Device frames 0..47 are silence, 48..95 would be timeline 100..147 but
    // only 48 frames are there: the period is short and the backend silences
    // the rest, which is the underrun phase 8 already counts.
    CHECK_EQ(source.render(out.data(), 96), 96u);
    CHECK_EQ(frameAt(out, 48), 100u);
    CHECK_EQ(source.underrunFrames(), 0u);

    CHECK_EQ(source.render(out.data(), 96), 0u); // nothing arrived: a dry period
    CHECK_EQ(source.underrunFrames(), 96u);

    // The audio for those 96 positions arrives late. It is dropped rather than
    // played, because the timeline moved on while the room was silent -- and
    // the frame after them lands exactly where the mapping says it should.
    pushFrames(buffer, 148, 192);
    CHECK_EQ(source.render(out.data(), 96), 96u);
    CHECK_EQ(source.skippedFrames(), 96u);
    CHECK_EQ(frameAt(out, 0), 244u); // 148 + 96 dropped
    CHECK_EQ(frameAt(out, 95), 339u);
}

MULTIROOM_TEST(timeline_source_volume_ramp_moves_through_an_underrun)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 100, 48);

    std::string error;
    CHECK(source.scheduleStart(snapshotAt(0, 0), startAt(1 * kMsNs), 100, 100, error));
    std::vector<unsigned char> out(1024 * kFrameBytes, 0);
    CHECK_EQ(source.render(out.data(), 96), 96u); // the audio runs out
    CHECK_NEAR(source.appliedGain(), 1.0, 1e-12);

    // Turned down while the buffer is dry: 21 ms of silence is longer than
    // the ramp, so the gain is there by the time the audio returns.
    source.setVolume(0.0, false);
    CHECK_EQ(source.render(out.data(), 1024), 0u);
    CHECK_NEAR(source.appliedGain(), 0.0, 1e-12);
}

MULTIROOM_TEST(timeline_source_reports_the_audible_frame_from_the_device_position)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 2000, 512);

    uint64_t audible = 0;
    CHECK(!source.streamFrameAudible(snapshotAt(480, 10 * kMsNs), audible));

    std::string error;
    CHECK(source.scheduleStart(snapshotAt(480, 10 * kMsNs), startAt(20 * kMsNs), 2000, 2000,
                               error));
    CHECK_EQ(source.startDeviceFrame(), 960u);

    // Before the start there is no audible timeline frame, which is not the
    // same as frame zero.
    CHECK(!source.streamFrameAudible(snapshotAt(959, 19 * kMsNs), audible));

    CHECK(source.streamFrameAudible(snapshotAt(960, 20 * kMsNs), audible));
    CHECK_EQ(audible, 2000u);
    CHECK(source.streamFrameAudible(snapshotAt(1440, 30 * kMsNs), audible));
    CHECK_EQ(audible, 2480u);
}

MULTIROOM_TEST(timeline_source_will_not_reschedule_a_running_stream)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 0, 512);

    std::string error;
    CHECK(source.scheduleStart(snapshotAt(0, 0), startAt(1 * kMsNs), 0, 0, error));

    std::vector<unsigned char> out(96 * kFrameBytes, 0);
    CHECK_EQ(source.render(out.data(), 96), 96u);
    CHECK(source.state() == TimelineState::Playing);

    // Moving a playing stream is a resynchronization (section 32), not a
    // start, and this client does not have one yet.
    CHECK(!source.scheduleStart(snapshotAt(96, 2 * kMsNs), startAt(10 * kMsNs), 96, 96, error));
    CHECK(!error.empty());
}

MULTIROOM_TEST(timeline_source_rearms_while_it_is_still_waiting)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 300, 512);

    std::string error;
    CHECK(source.scheduleStart(snapshotAt(0, 0), startAt(2 * kMsNs), 300, 300, error));
    CHECK_EQ(source.startDeviceFrame(), 96u);

    std::vector<unsigned char> out(48 * kFrameBytes, 0xAB);
    CHECK_EQ(source.render(out.data(), 48), 48u);

    // The server moved the start, which it may do until the room is playing.
    CHECK(source.scheduleStart(snapshotAt(0, 0), startAt(4 * kMsNs), 310, 300, error));
    CHECK_EQ(source.startDeviceFrame(), 192u);

    CHECK_EQ(source.render(out.data(), 48), 48u); // 48..95, still silent now
    CHECK(isSilent(out, 0, 48));
    CHECK_EQ(source.render(out.data(), 48), 48u); // 96..143
    CHECK(isSilent(out, 0, 48));
    CHECK_EQ(source.render(out.data(), 48), 48u); // 144..191
    CHECK(isSilent(out, 0, 48));
    CHECK_EQ(source.render(out.data(), 48), 48u); // 192: the new start
    CHECK_EQ(frameAt(out, 0), 310u);
    CHECK_EQ(source.skippedFrames(), 10u);
}

MULTIROOM_TEST(timeline_source_drops_its_schedule_when_the_stream_is_released)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 0, 512);

    std::string error;
    CHECK(source.scheduleStart(snapshotAt(0, 0), startAt(10 * kMsNs), 0, 0, error));
    CHECK(source.isScheduled());

    source.cancel();
    CHECK(!source.isScheduled());
    CHECK(source.state() == TimelineState::Idle);

    std::vector<unsigned char> out(960 * kFrameBytes, 0xAB);
    CHECK_EQ(source.render(out.data(), 960), 960u);
    CHECK(isSilent(out, 0, 960));
    CHECK_EQ(buffer.readableFrames(), 512u);
}

MULTIROOM_TEST(ring_buffer_discards_without_copying)
{
    AudioRingBuffer buffer;
    std::string error;
    CHECK(buffer.configure(kFrameBytes, 16, error));

    pushFrames(buffer, 1, 12);
    CHECK_EQ(buffer.discard(4), 4u);
    CHECK_EQ(buffer.readableFrames(), 8u);

    std::vector<unsigned char> out(8 * kFrameBytes, 0);
    CHECK_EQ(buffer.pop(out.data(), 8), 8u);
    CHECK_EQ(frameAt(out, 0), 5u);

    // Short rather than blocking, like every other call on this class.
    CHECK_EQ(buffer.discard(4), 0u);
}

// Brings a source to the point where it is playing the timeline, so the
// transport tests below start where the protocol's transport does. The start
// lands on device frame 960 and timeline frame 1000, and 512 frames are
// buffered from there.
static void playFromTimelineFrame1000(TimelineSource& source, AudioRingBuffer& buffer)
{
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 1000, 512);

    std::string error;
    CHECK(source.scheduleStart(snapshotAt(480, 100 * kMsNs), startAt(110 * kMsNs), 1000, 1000,
                               error));

    std::vector<unsigned char> out(64 * kFrameBytes, 0);
    for (uint32_t cursor = 0; cursor < 960; cursor += 64)
        CHECK_EQ(source.render(out.data(), 64), 64u);

    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK_EQ(frameAt(out, 0), 1000u);
    CHECK(source.state() == TimelineState::Playing);
}

MULTIROOM_TEST(timeline_source_holds_a_scheduled_volume_through_a_pause)
{
    // Resumed from `resumeStreamFrame`; the change is for stream frame 1200.
    const auto check = [](uint64_t resumeStreamFrame, uint64_t changeDeviceFrame) {
        AudioRingBuffer buffer;
        TimelineSource source(buffer, 100);
        playFromTimelineFrame1000(source, buffer);

        // Stream frame 1200 is device frame 1160 on this start; the pause at
        // 1100 (device frame 1060) comes first, and the device runs on past
        // 1160 while the timeline stays at 1100.
        CHECK(source.setVolumeAt(0.0, false, 1200));
        source.schedulePause(1100);
        std::vector<unsigned char> out(64 * kFrameBytes, 0);
        for (uint32_t cursor = 1024; cursor < 3136; cursor += 64)
            CHECK_EQ(source.render(out.data(), 64), 64u);
        CHECK(source.state() == TimelineState::Paused);
        CHECK_NEAR(source.appliedGain(), 1.0, 1e-12);
        CHECK(source.volumeScheduled());

        // The resume's start is device frame 3616.
        std::string error;
        CHECK(source.scheduleStart(snapshotAt(3136, 200 * kMsNs), startAt(210 * kMsNs),
                                   resumeStreamFrame, 1100, error));
        uint64_t cursor = 3136;
        for (; cursor + 64 <= changeDeviceFrame; cursor += 64)
            source.render(out.data(), 64);
        CHECK_NEAR(source.appliedGain(), 1.0, 1e-12);
        for (; cursor < changeDeviceFrame + 768; cursor += 64)
            source.render(out.data(), 64);
        CHECK_NEAR(source.appliedGain(), 0.0, 1e-12);
    };

    // On the resume's own mapping: 100 frames after it.
    check(1100, 3716);
    // A resume from beyond it: in force from the next callback.
    check(1300, 3136);
}

MULTIROOM_TEST(timeline_source_pauses_on_the_frame_the_server_named)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    playFromTimelineFrame1000(source, buffer);

    // Timeline frame 1100 is device frame 1060, which falls 36 frames into the
    // period that begins at 1024 (MATP section 37).
    std::string error;
    source.schedulePause(1100);
    CHECK(source.hasScheduledEnd());
    CHECK_EQ(source.endDeviceFrame(), static_cast<uint64_t>(1060));

    std::vector<unsigned char> out(64 * kFrameBytes, 0xAB);
    // The whole period is filled: the silence after the pause is deliberate,
    // and returning short would have the backend count it as an underrun.
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK_EQ(frameAt(out, 0), 1064u);
    CHECK_EQ(frameAt(out, 35), 1099u);
    CHECK(isSilent(out, 36, 64));
    CHECK(source.state() == TimelineState::Paused);
    CHECK_EQ(source.underrunFrames(), 0u);

    // What was not played is still buffered: section 37 resumes with a new
    // START_AT, and the audio to resume with is the audio already here.
    CHECK_EQ(buffer.readableFrames(), 412u);

    // And the device keeps running on silence rather than being stopped.
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK(isSilent(out, 0, 64));
    CHECK_EQ(buffer.readableFrames(), 412u);
}

MULTIROOM_TEST(timeline_source_stops_and_drops_what_was_buffered)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    playFromTimelineFrame1000(source, buffer);

    std::string error;
    source.scheduleStop(1100);

    std::vector<unsigned char> out(64 * kFrameBytes, 0xAB);
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK_EQ(frameAt(out, 35), 1099u);
    CHECK(isSilent(out, 36, 64));
    CHECK(source.state() == TimelineState::Stopped);

    // A stop is not a pause: what is buffered belongs to a timeline nobody is
    // playing any more (section 38). It is not counted as skipped, though --
    // a skipped frame is one that missed its moment on the timeline, and these
    // had no moment left to miss.
    CHECK_EQ(buffer.readableFrames(), 0u);
    CHECK_EQ(source.skippedFrames(), 0u);
}

MULTIROOM_TEST(timeline_source_resumes_a_pause_with_a_new_start)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    playFromTimelineFrame1000(source, buffer);

    std::string error;
    source.schedulePause(1100);

    std::vector<unsigned char> out(64 * kFrameBytes, 0);
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK(source.state() == TimelineState::Paused);

    // Section 37: the resume is a new START_AT, not a message of its own. The
    // device has kept running, so its clock and its position are still there
    // to place one against.
    CHECK(source.scheduleStart(snapshotAt(1100, 200 * kMsNs), startAt(210 * kMsNs), 1100, 1100,
                               error));
    CHECK_EQ(source.startDeviceFrame(), static_cast<uint64_t>(1580));
    CHECK(!source.hasScheduledEnd()); // the pause belonged to the run that ended
    CHECK(source.state() == TimelineState::Waiting);

    for (uint32_t cursor = 1088; cursor + 64 <= 1580; cursor += 64)
    {
        CHECK_EQ(source.render(out.data(), 64), 64u);
        CHECK(isSilent(out, 0, 64));
    }

    // The period the resume falls into: silence to device frame 1580, then the
    // timeline from where the pause left it, to the frame.
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK(isSilent(out, 0, 44));
    CHECK_EQ(frameAt(out, 44), 1100u);
    CHECK(source.state() == TimelineState::Playing);
}

MULTIROOM_TEST(timeline_source_stays_at_the_frame_it_paused_on)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    playFromTimelineFrame1000(source, buffer);

    std::string error;
    source.schedulePause(1100);

    std::vector<unsigned char> out(64 * kFrameBytes, 0);
    CHECK_EQ(source.render(out.data(), 64), 64u);

    // The device runs on and its position keeps growing, but the timeline it
    // was playing does not: a report that mapped the new position would name a
    // frame the room never made audible.
    uint64_t frame = 0;
    CHECK(source.streamFrameAudible(snapshotAt(1040, 130 * kMsNs), frame));
    CHECK_EQ(frame, static_cast<uint64_t>(1080));
    CHECK(source.streamFrameAudible(snapshotAt(2000, 150 * kMsNs), frame));
    CHECK_EQ(frame, static_cast<uint64_t>(1100));
}

MULTIROOM_TEST(timeline_source_drops_a_start_an_end_arrives_in_front_of)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 1000, 512);

    std::string error;
    CHECK(source.scheduleStart(snapshotAt(480, 100 * kMsNs), startAt(110 * kMsNs), 1000, 1000,
                               error));
    CHECK(source.isScheduled());

    // A pause for a frame this room was going to start after. Refusing it
    // would leave the start armed, and the room would begin playing after
    // every other room had paused — which is the one outcome a synchronized
    // pause exists to prevent. The schedule goes instead.
    source.schedulePause(999);
    CHECK(!source.isScheduled());
    CHECK(source.hasScheduledEnd());
    CHECK_EQ(source.endStreamFrame(), static_cast<uint64_t>(999));

    std::vector<unsigned char> out(64 * kFrameBytes, 0xAB);
    for (uint32_t period = 0; period < 20; ++period)
    {
        CHECK_EQ(source.render(out.data(), 64), 64u);
        CHECK(isSilent(out, 0, 64));
    }
    CHECK(source.state() == TimelineState::Paused);

    // A pause keeps the buffer, even this one: the server resumes with a new
    // start, and the audio to resume with is still the audio that is here.
    CHECK_EQ(buffer.readableFrames(), 512u);
}

MULTIROOM_TEST(timeline_source_ends_where_nothing_is_playing)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 1000, 512);

    // Nothing is scheduled, so there is no timeline to cut — but a stop still
    // has a buffer to drop, and the room is stopped rather than merely idle.
    source.scheduleStop(1100);
    CHECK(!source.isScheduled());

    std::vector<unsigned char> out(64 * kFrameBytes, 0xAB);
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK(isSilent(out, 0, 64));
    CHECK(source.state() == TimelineState::Stopped);
    CHECK_EQ(buffer.readableFrames(), 0u);
}

MULTIROOM_TEST(timeline_source_stops_a_room_that_was_already_paused)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    playFromTimelineFrame1000(source, buffer);

    source.schedulePause(1100);

    std::vector<unsigned char> out(64 * kFrameBytes, 0);
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK(source.state() == TimelineState::Paused);
    CHECK_EQ(buffer.readableFrames(), 412u);

    // Pause, then stop: the room is silent either way, so the stop is visible
    // only in what it does to the buffer. A render path that noticed nothing
    // after the first end would leave the room "paused" and sitting on audio
    // nobody is going to play.
    source.scheduleStop(1100);
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK(isSilent(out, 0, 64));
    CHECK(source.state() == TimelineState::Stopped);
    CHECK_EQ(buffer.readableFrames(), 0u);
}

MULTIROOM_TEST(timeline_source_applies_a_scheduled_volume_on_the_timeline_frame)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 1000, 512);

    // Nothing maps a timeline frame onto the device until a start has placed
    // the two against each other (MATP section 39 schedules against the
    // timeline, and this room has none yet).
    CHECK(!source.setVolumeAt(0.5, false, 1100));

    std::string error;
    CHECK(source.scheduleStart(snapshotAt(480, 100 * kMsNs), startAt(110 * kMsNs), 1000, 1000,
                               error));
    CHECK(source.setVolumeAt(0.5, false, 1100));
    CHECK(source.volumeScheduled());
    CHECK_NEAR(source.volumeGain(), 0.5, 1e-12);

    // An unscheduled change replaces it and applies at the next callback,
    // which is what a volume knob does.
    source.setVolume(0.25, true);
    CHECK(!source.volumeScheduled());
    CHECK(source.volumeMuted());
    CHECK_NEAR(source.volumeGain(), 0.25, 1e-12);
}

MULTIROOM_TEST(timeline_source_discards_the_timeline_for_a_rejoin)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    playFromTimelineFrame1000(source, buffer);
    CHECK_EQ(buffer.readableFrames(), 448u);
    CHECK_EQ(source.bufferDrops(), 0u);

    // The server has moved this room somewhere else on the timeline (MATP
    // sections 43 and 32). The mapping goes at once, on this thread; the
    // buffer can only be emptied by the one that reads it.
    source.discardTimeline();
    CHECK(!source.isScheduled());
    CHECK(!source.hasScheduledEnd());
    CHECK_EQ(buffer.readableFrames(), 448u);

    std::vector<unsigned char> out(64 * kFrameBytes, 0xAB);
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK(isSilent(out, 0, 64));
    CHECK_EQ(buffer.readableFrames(), 0u);
    CHECK_EQ(source.bufferDrops(), 1u);

    // A room waiting to rejoin is not a room the server stopped, and the
    // frames that went are not frames that missed their moment: nothing was
    // scheduled to play them any more.
    CHECK(source.state() == TimelineState::Rejoining);
    CHECK_EQ(source.skippedFrames(), 0u);

    // And the rejoin itself is an ordinary start, against the audio of the new
    // position: the device never stopped, so its frame axis carried on through
    // all of it.
    CHECK_EQ(source.submittedFrames(), 1088u);
    pushFrames(buffer, 5000, 512);

    std::string error;
    CHECK(source.scheduleStart(snapshotAt(1088, 200 * kMsNs), startAt(210 * kMsNs), 5000, 5000,
                               error));
    CHECK_EQ(source.startDeviceFrame(), static_cast<uint64_t>(1568));

    for (uint32_t cursor = 1088; cursor < 1536; cursor += 64)
        CHECK_EQ(source.render(out.data(), 64), 64u);

    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK(isSilent(out, 0, 32));
    CHECK_EQ(frameAt(out, 32), 5000u);
    CHECK(source.state() == TimelineState::Playing);
}

MULTIROOM_TEST(timeline_source_discards_a_timeline_that_was_never_started)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(monoInt16(48000));
    pushFrames(buffer, 1000, 512);

    // A room that reconnected before it was ever scheduled still holds audio
    // for a timeline position the server has since moved on from, and a full
    // buffer is also a producer that cannot put the new position into it.
    source.discardTimeline();

    std::vector<unsigned char> out(64 * kFrameBytes, 0xAB);
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK(isSilent(out, 0, 64));
    CHECK_EQ(buffer.readableFrames(), 0u);
    CHECK_EQ(source.bufferDrops(), 1u);
    CHECK(source.state() == TimelineState::Rejoining);
}

// MATP section 54. The helper leaves the device at frame 1024, the mapping at
// device frame 960 = stream frame 1000, and stream frames 1064..1511 buffered.
MULTIROOM_TEST(timeline_source_flush_cuts_at_the_frame_and_keeps_the_mapping)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    playFromTimelineFrame1000(source, buffer);

    // Stream frame 1100 is device frame 1060: 36 frames into the next period.
    source.scheduleFlush(1100);
    CHECK(source.state() == TimelineState::Playing); // not until the callback cuts

    std::vector<unsigned char> out(64 * kFrameBytes, 0xAB);
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK_EQ(frameAt(out, 0), 1064u);
    CHECK_EQ(frameAt(out, 35), 1099u);
    CHECK(isSilent(out, 36, 64));

    // Everything at or after the cut went, counted as flushed and as nothing
    // else: the silence was asked for, and those frames had no moment to miss.
    CHECK_EQ(buffer.readableFrames(), 0u);
    CHECK_EQ(source.flushedFrames(), 412u);
    CHECK_EQ(source.bufferDrops(), 1u);
    CHECK_EQ(source.underrunFrames(), 0u);
    CHECK_EQ(source.skippedFrames(), 0u);
    CHECK(source.state() == TimelineState::Flushing);

    // The mapping is the one thing a flush does not touch.
    CHECK(source.isScheduled());
    CHECK_EQ(source.startDeviceFrame(), static_cast<uint64_t>(960));
    CHECK_EQ(source.startStreamFrame(), static_cast<uint64_t>(1000));
    CHECK(!source.hasScheduledEnd());

    // Audio from before the cut that was still in flight is dropped as it
    // lands, so a producer can never fill the buffer with it and stall.
    pushFrames(buffer, 1512, 20);
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK(isSilent(out, 0, 64));
    CHECK_EQ(buffer.readableFrames(), 0u);
    CHECK_EQ(source.flushedFrames(), 432u);
    CHECK_EQ(source.bufferDrops(), 1u);

    CHECK(!source.inFlushGap(snapshotAt(1059, 122 * kMsNs)));
    CHECK(source.inFlushGap(snapshotAt(1060, 122 * kMsNs)));
}

MULTIROOM_TEST(timeline_source_flush_resumes_on_the_mapped_device_frame)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    playFromTimelineFrame1000(source, buffer);

    source.scheduleFlush(1100);
    std::vector<unsigned char> out(64 * kFrameBytes, 0xAB);
    CHECK_EQ(source.render(out.data(), 64), 64u); // the cut; cursor 1088
    CHECK_EQ(source.bufferDrops(), 1u);

    // The audio after the cut belongs at stream frame 1300 of the kept line,
    // which is device frame 1260.
    CHECK(source.resumeAfterFlush(1300));
    CHECK(source.isScheduled());
    CHECK_EQ(source.startDeviceFrame(), static_cast<uint64_t>(960));

    // What landed before the producer parked is dropped when the resume is
    // adopted, and the count moving is what lets the producer go.
    pushFrames(buffer, 1512, 8);
    CHECK_EQ(source.render(out.data(), 64), 64u); // 1088..1151
    CHECK(isSilent(out, 0, 64));
    CHECK_EQ(buffer.readableFrames(), 0u);
    CHECK_EQ(source.bufferDrops(), 2u);
    CHECK_EQ(source.flushedFrames(), 420u);
    CHECK(source.state() == TimelineState::Flushing);

    pushFrames(buffer, 9000, 256);
    CHECK_EQ(source.render(out.data(), 64), 64u); // 1152..1215
    CHECK(isSilent(out, 0, 64));
    CHECK_EQ(buffer.readableFrames(), 256u);

    // Device frame 1260 is the first frame of the new audio, and not a frame
    // earlier.
    CHECK_EQ(source.render(out.data(), 64), 64u); // 1216..1279
    CHECK(isSilent(out, 0, 44));
    CHECK_EQ(frameAt(out, 44), 9000u);
    CHECK_EQ(frameAt(out, 63), 9019u);
    CHECK(source.state() == TimelineState::Playing);
    CHECK_EQ(source.underrunFrames(), 0u);
    CHECK_EQ(source.skippedFrames(), 0u);

    uint64_t audible = 0;
    CHECK(source.streamFrameAudible(snapshotAt(1260, 126 * kMsNs), audible));
    CHECK_EQ(audible, static_cast<uint64_t>(1300));
    CHECK(source.inFlushGap(snapshotAt(1259, 126 * kMsNs)));
    CHECK(!source.inFlushGap(snapshotAt(1260, 126 * kMsNs)));
}

MULTIROOM_TEST(timeline_source_flush_behind_the_device_cuts_at_once)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    playFromTimelineFrame1000(source, buffer);

    // Stream frame 1010 is device frame 970, which the device has passed.
    source.scheduleFlush(1010);

    std::vector<unsigned char> out(64 * kFrameBytes, 0xAB);
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK(isSilent(out, 0, 64));
    CHECK_EQ(buffer.readableFrames(), 0u);
    CHECK_EQ(source.flushedFrames(), 448u);
    CHECK_EQ(source.underrunFrames(), 0u);
    CHECK(source.state() == TimelineState::Flushing);
    CHECK(source.isScheduled());
}

MULTIROOM_TEST(timeline_source_flush_skips_late_audio_up_to_the_line)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    playFromTimelineFrame1000(source, buffer);

    source.scheduleFlush(1100);
    std::vector<unsigned char> out(64 * kFrameBytes, 0xAB);
    CHECK_EQ(source.render(out.data(), 64), 64u); // the cut; cursor 1088

    // Stream frame 1110 is device frame 1070, already behind the cursor when
    // the resume is adopted, and its audio has not arrived either.
    CHECK(source.resumeAfterFlush(1110));
    source.render(out.data(), 64); // 1088..1151: the buffer is dry
    CHECK_EQ(source.underrunFrames(), 64u);

    // So the audio lands on the line and not behind it: device frame 1152 is
    // stream frame 1110 + 82.
    pushFrames(buffer, 9000, 256);
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK_EQ(frameAt(out, 0), 9082u);
    CHECK_EQ(source.skippedFrames(), 82u);

    uint64_t audible = 0;
    CHECK(source.streamFrameAudible(snapshotAt(1152, 130 * kMsNs), audible));
    CHECK_EQ(audible, static_cast<uint64_t>(1192));
}

MULTIROOM_TEST(timeline_source_flush_keeps_no_line_for_a_paused_room)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    playFromTimelineFrame1000(source, buffer);

    source.schedulePause(1100);
    std::vector<unsigned char> out(64 * kFrameBytes, 0);
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK(source.state() == TimelineState::Paused);
    CHECK_EQ(buffer.readableFrames(), 412u);

    // Section 54 on a paused line: the cut is the pause position, the room
    // drops what it holds, and the audio after the cut waits for a START_AT
    // rather than resuming on its own.
    source.scheduleFlush(1100);
    CHECK_EQ(source.render(out.data(), 64), 64u);
    CHECK(isSilent(out, 0, 64));
    CHECK_EQ(buffer.readableFrames(), 0u);
    CHECK_EQ(source.flushedFrames(), 412u);
    CHECK(source.hasScheduledEnd());
    CHECK(!source.resumeAfterFlush(1200));
}

// The wire's s24le is 24 bits at the bottom of four bytes; WASAPI plays the top
// three. Unshifted, a room on a 24-in-32 device is 48 dB down and says nothing.
MULTIROOM_TEST(timeline_source_hands_int24in32_to_the_device_in_its_layout)
{
    AudioFormat format;
    format.sampleRate = 48000;
    format.channels = 1;
    format.sampleType = AudioFormat::SampleType::Int24In32;

    AudioRingBuffer buffer;
    TimelineSource source(buffer, 100);
    source.prepare(format);
    // A positive sample, and a negative one with the core's sign-extended pad.
    const uint32_t wire[2] = {0x00123456u, 0xFFEDCBAAu};
    CHECK_EQ(buffer.push(reinterpret_cast<const unsigned char*>(wire), 2), 2u);

    std::string error;
    // 1 ms out: device frame 48.
    CHECK(source.scheduleStart(snapshotAt(0, 0), startAt(kMsNs), 0, 0, error));
    uint32_t out[50] = {};
    CHECK_EQ(source.render(reinterpret_cast<unsigned char*>(out), 50), 50u);
#if defined(_WIN32)
    CHECK_EQ(out[48], 0x12345600u);
    CHECK_EQ(out[49], 0xEDCBAA00u);
#else
    CHECK_EQ(out[48] & 0x00FFFFFFu, 0x00123456u);
    CHECK_EQ(out[49] & 0x00FFFFFFu, 0x00EDCBAAu);
#endif
}

namespace {

// The de-click tests play a steady level rather than a counter: what they look
// at is the gain on each frame, and a level makes that the sample itself.
const int16_t kLevel = 16000;

void pushLevel(AudioRingBuffer& buffer, uint32_t count)
{
    std::vector<int16_t> block(static_cast<size_t>(count) * 2, kLevel);
    CHECK_EQ(buffer.push(reinterpret_cast<const unsigned char*>(block.data()), count), count);
}

// Renders `count` frames and answers the left channel of each, with what the
// source left short silenced the way the backend silences it.
std::vector<int16_t> renderLevels(TimelineSource& source, uint32_t count)
{
    std::vector<int16_t> out(static_cast<size_t>(count) * 2, 0x5A5A);
    const uint32_t produced = source.render(reinterpret_cast<unsigned char*>(out.data()), count);
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(produced) * 2, out.end(), int16_t(0));
    std::vector<int16_t> left(count);
    for (uint32_t i = 0; i < count; ++i)
        left[i] = out[static_cast<size_t>(i) * 2];
    return left;
}

// Never a step bigger than a fade of `fadeFrames` frames moves by, plus
// rounding: what a click is not. 480 is the full 10 ms.
bool smooth(const std::vector<int16_t>& levels, int fadeFrames = 480)
{
    const int most = kLevel / fadeFrames + 1;
    for (size_t i = 1; i < levels.size(); ++i)
    {
        const int step = levels[i] - levels[i - 1];
        if (step > most || step < -most)
            return false;
    }
    return true;
}

} // namespace

// 10 ms at 48 kHz is 480 frames: up from the start's frame, down to the pause's.
MULTIROOM_TEST(timeline_source_declick_fades_in_at_a_start_and_out_to_a_pause)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 200);
    source.prepare(monoInt16(48000));
    source.setDeclick(true);
    pushLevel(buffer, 4800);

    std::string error;
    // 10 ms out: device frame 480 is timeline frame 0.
    CHECK(source.scheduleStart(snapshotAt(0, 0), startAt(10 * kMsNs), 0, 0, error));
    std::vector<int16_t> played = renderLevels(source, 480);
    CHECK(isSilent(std::vector<unsigned char>(reinterpret_cast<unsigned char*>(played.data()),
                                              reinterpret_cast<unsigned char*>(played.data()) +
                                                  played.size() * 2),
                   0, 240)); // 480 int16 values are 240 four-byte frames
    std::vector<int16_t> rising = renderLevels(source, 480);
    CHECK_EQ(rising[0], 0);
    CHECK_EQ(rising[240], kLevel / 2);
    CHECK(rising[479] > kLevel - 40 && rising[479] < kLevel);
    std::vector<int16_t> steady = renderLevels(source, 480);
    CHECK_EQ(steady[0], kLevel);
    CHECK_EQ(steady[479], kLevel);

    // A pause on timeline frame 2000, device frame 2480: the fade runs over
    // device frames 2000..2479 and the timeline position does not move.
    source.schedulePause(2000);
    std::vector<int16_t> before = renderLevels(source, 480);  // 1440..1919
    std::vector<int16_t> falling = renderLevels(source, 480); // 1920..2399
    std::vector<int16_t> last = renderLevels(source, 480);    // 2400..2879
    CHECK_EQ(before[479], kLevel);
    CHECK_EQ(falling[79], kLevel); // device frame 1999
    CHECK_EQ(falling[80], kLevel); // 2000: the fade's first frame is full
    CHECK(falling[320] > kLevel / 2 - 40 && falling[320] < kLevel / 2 + 40); // 2240
    CHECK(last[79] > 0 && last[79] < 100); // 2479, the last frame before the pause
    CHECK_EQ(last[80], 0);                 // 2480: paused
    CHECK(source.state() == TimelineState::Paused);
    CHECK(smooth(rising));
    CHECK(smooth(falling));
    CHECK(smooth(last));
}

MULTIROOM_TEST(timeline_source_declick_fades_into_an_underrun_and_out_of_it)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 200);
    source.prepare(monoInt16(48000));
    source.setDeclick(true);
    pushLevel(buffer, 1200);

    std::string error;
    CHECK(source.scheduleStart(snapshotAt(0, 0), startAt(10 * kMsNs), 0, 0, error));
    renderLevels(source, 480);
    renderLevels(source, 480);
    renderLevels(source, 480);
    // 240 frames left for a 480-frame period: they fade to nothing.
    std::vector<int16_t> dry = renderLevels(source, 480);
    CHECK_EQ(dry[0], kLevel);
    CHECK(dry[239] > 0 && dry[239] < 100);
    CHECK_EQ(dry[240], 0);
    // Over the 240 frames there were: a fade, if a quicker one.
    CHECK(smooth(std::vector<int16_t>(dry.begin(), dry.begin() + 240), 240));
    CHECK_EQ(source.underrunFrames(), 240u);

    // The audio comes back after the 240 frames it owes the timeline, and
    // comes back from silence.
    pushLevel(buffer, 2000);
    std::vector<int16_t> back = renderLevels(source, 480);
    CHECK_EQ(back[0], 0);
    CHECK(smooth(back));
    CHECK_EQ(source.skippedFrames(), 240u);
}

// A discarded timeline has no frame to fade toward: up to 10 ms of what the
// buffer held is played going down, and then the buffer is dropped as before.
MULTIROOM_TEST(timeline_source_declick_fades_a_cut_that_comes_at_once)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 200);
    source.prepare(monoInt16(48000));
    source.setDeclick(true);
    pushLevel(buffer, 4800);

    std::string error;
    CHECK(source.scheduleStart(snapshotAt(0, 0), startAt(10 * kMsNs), 0, 0, error));
    renderLevels(source, 480);
    renderLevels(source, 480);
    renderLevels(source, 480);

    source.discardTimeline();
    std::vector<int16_t> cut = renderLevels(source, 960);
    CHECK_EQ(cut[0], kLevel);
    CHECK(cut[479] > 0 && cut[479] < 100);
    CHECK_EQ(cut[480], 0);
    CHECK(smooth(cut));
    CHECK_EQ(source.bufferDrops(), 1u);
    CHECK_EQ(buffer.readableFrames(), 0u);
    CHECK(source.state() == TimelineState::Rejoining);
}

// Off, the edges are cut exactly where the server put them: the first frame
// of a start is the stream's first sample.
MULTIROOM_TEST(timeline_source_without_declick_starts_at_full_level)
{
    AudioRingBuffer buffer;
    TimelineSource source(buffer, 200);
    source.prepare(monoInt16(48000));
    pushLevel(buffer, 960);

    std::string error;
    CHECK(source.scheduleStart(snapshotAt(0, 0), startAt(10 * kMsNs), 0, 0, error));
    renderLevels(source, 480);
    const std::vector<int16_t> first = renderLevels(source, 480);
    CHECK_EQ(first[0], kLevel);
}
