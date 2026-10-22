#include "audio/TimelineSource.h"

#include "util/Logger.h"

#include <cstring>

namespace multiroom {
namespace {

// A start further away than this is not a schedule, it is a mistake: section
// 46's prebuffer is measured in seconds and section 20's example start is two
// of them. Refusing is better than arming a device frame position an hour out
// and holding the room silent until then.
const uint64_t kMaxStartLeadNs = 60ULL * 1000000000ULL;

const uint64_t kNanosecondsPerSecond = 1000000000ULL;

// The device frame a held volume change is published for: no cursor reaches
// it, so it waits until placeVolume() moves it.
const uint64_t kHeldDeviceFrame = ~0ULL;

// The de-click's fade: long enough that no edge is a step in the waveform,
// short enough that it is not heard as a fade. At 48 kHz it is 480 frames.
const uint32_t kFadeMilliseconds = 10;

// MATP's s24le is ALSA's S24_LE: the 24 valid bits in the low three bytes, the
// top byte padding (the core writes it sign-extended). WASAPI's 24-in-32 has
// them in the high three bytes and a zero low byte, and so does I2S, which
// sends each word MSB first. Played unshifted, every sample is 48 dB down --
// silence to the ear, and no error anywhere.
void leftJustifyInt24In32(unsigned char* data, uint32_t samples)
{
    for (uint32_t i = 0; i < samples; ++i, data += 4)
    {
        uint32_t raw = 0;
        std::memcpy(&raw, data, 4);
        raw <<= 8; // the padding byte falls off the top
        std::memcpy(data, &raw, 4);
    }
}

} // namespace

const char* timelineStateName(TimelineState state)
{
    switch (state)
    {
    case TimelineState::Idle:
        return "idle";
    case TimelineState::Waiting:
        return "waiting";
    case TimelineState::Playing:
        return "playing";
    case TimelineState::Paused:
        return "paused";
    case TimelineState::Stopped:
        return "stopped";
    case TimelineState::Rejoining:
        return "rejoining";
    case TimelineState::Flushing:
        return "flushing";
    }
    return "idle";
}

TimelineSource::TimelineSource(AudioRingBuffer& buffer, uint32_t capacityMilliseconds)
    : buffer_(buffer), capacityMilliseconds_(capacityMilliseconds), sequence_(0), cursorLow_(0),
      underrun_(0), skipped_(0), drops_(0), flushed_(0), state_(static_cast<uint32_t>(TimelineState::Idle))
{
}

void TimelineSource::prepare(const AudioFormat& format)
{
    // Runs on the control thread with the device closed, so everything the
    // audio thread owns can be reset here without a barrier.
    cancel();
    cursor_ = 0;
    playing_ = false;
    ended_ = false;
    endDroppedBuffer_ = false;
    endWasResync_ = false;
    endWasFlush_ = false;
    appliedEndGeneration_ = 0;
    skipRemaining_ = 0;
    audible_ = false;
    fadeInDone_ = 0;
    fadeOutGeneration_ = 0;
    fadeOutStart_ = 0;
    hasActive_ = false;
    active_ = Schedule();
    cursorLow_.store(0, std::memory_order_relaxed);
    underrun_.store(0, std::memory_order_relaxed);
    skipped_.store(0, std::memory_order_relaxed);
    drops_.store(0, std::memory_order_relaxed);
    flushed_.store(0, std::memory_order_relaxed);
    cursorExtender_.reset();

    // The room's volume survives a reopened device; the ramp and the frame
    // size do not, because both belong to the format that was just granted.
    volume_.prepare(format);

#if defined(_WIN32) || defined(ESP_PLATFORM)
    leftJustify24_ = format.sampleType == AudioFormat::SampleType::Int24In32;
#endif

    if (!format.isValid())
    {
        MULTIROOM_ERROR("timeline source prepared with an invalid format");
        return;
    }

    // Rounded up, so a capacity in milliseconds never turns into zero frames.
    const uint64_t rate = static_cast<uint64_t>(format.sampleRate);
    fadeFrames_ = static_cast<uint32_t>((rate * kFadeMilliseconds + 999ULL) / 1000ULL);
    const uint64_t frames = (rate * capacityMilliseconds_ + 999ULL) / 1000ULL;

    std::string error;
    if (!buffer_.configure(format.bytesPerFrame(), static_cast<uint32_t>(frames), error))
    {
        // render() then produces silence for as long as the device is open,
        // which is the honest outcome: there is no buffer to play from.
        MULTIROOM_ERROR("cannot size the audio ring buffer: %s", error.c_str());
        return;
    }

    MULTIROOM_INFO("audio ring buffer: %lu frames (%lu ms of %s)",
             static_cast<unsigned long>(buffer_.capacityFrames()),
             static_cast<unsigned long>(capacityMilliseconds_), describeFormat(format).c_str());
}

uint32_t TimelineSource::render(void* buffer, uint32_t frameCount)
{
    const uint32_t frameBytes = buffer_.bytesPerFrame();
    if (buffer == nullptr || frameCount == 0 || frameBytes == 0)
        return 0;

    unsigned char* const target = static_cast<unsigned char*>(buffer);
    const uint64_t periodStart = cursor_;

    if (adopt())
    {
        // A start armed, re-armed or dropped while the device was already
        // running. A pause or a stop published on top of the same start is not
        // one of those, and must not begin the start over.
        playing_ = false;
        ended_ = false;
        endDroppedBuffer_ = false;
        endWasResync_ = false;
        endWasFlush_ = false;
        skipRemaining_ = 0;

        if (active_.startAfterFlush)
        {
            // A flush's resume. The audio connection is parked on the first
            // packet after the cut, so what the buffer holds is audio from
            // before the cut that was still in flight, and nothing more can
            // arrive until the count below tells the control thread this is
            // done.
            flushed_.fetch_add(buffer_.discard(buffer_.readableFrames()),
                               std::memory_order_relaxed);
            drops_.fetch_add(1, std::memory_order_release);
        }
    }

    // An end that arrived with nothing left to cut: a stop after a pause, or
    // either of the two for a timeline this room was never going to play.
    // There is no audio to end here, but a stop still has a buffer to drop.
    const bool declick = declick_.load(std::memory_order_relaxed);
    uint32_t tail = 0;
    if (active_.endGeneration != 0 && active_.endGeneration != appliedEndGeneration_ &&
        active_.endDeviceFrame <= cursor_ && (!hasActive_ || ended_))
    {
        // A discarded timeline takes the audio away mid-waveform.
        if (declick && audible_ && active_.endDropsBuffer)
            tail = renderFadeTail(target, frameCount);
        applyEnd();
    }

    // Nothing to play: before the start, or after a pause or a stop has come
    // round. The ring buffer is untouched either way -- everything in it
    // belongs to the timeline at or after the start, and this is where the
    // prebuffer of section 18 gets to fill.
    if (!hasActive_ || ended_ ||
        (!playing_ && cursor_ + frameCount <= active_.startDeviceFrame))
    {
        // After a flush's cut the only exception: what reaches the buffer
        // until the resume is audio from before the cut, and a producer left
        // to fill the buffer with it would block before it ever reached the
        // packet that ends the flush.
        if (ended_ && endWasFlush_)
            flushed_.fetch_add(buffer_.discard(buffer_.readableFrames()),
                               std::memory_order_relaxed);

        std::memset(target + static_cast<size_t>(tail) * frameBytes, 0,
                    static_cast<size_t>(frameCount - tail) * frameBytes);
        volume_.process(target, tail, periodStart);
        volume_.silence(frameCount - tail, periodStart + tail);
        audible_ = false;
        advance(frameCount);
        state_.store(static_cast<uint32_t>(silentState()), std::memory_order_relaxed);
        return frameCount;
    }

    uint32_t produced = 0;
    if (!playing_)
    {
        // The period the start falls into: silence up to the scheduled device
        // frame and the timeline from it. This is where the start becomes
        // sample-accurate rather than period-accurate -- nothing here waits
        // for a wakeup, the frame simply falls where it falls.
        uint64_t late = 0;
        if (active_.startDeviceFrame > cursor_)
            produced = static_cast<uint32_t>(active_.startDeviceFrame - cursor_);
        else
            late = cursor_ - active_.startDeviceFrame;
        std::memset(target, 0, static_cast<size_t>(produced) * frameBytes);

        // Adopted after its frame had passed: a flush's resume that came
        // round before its audio did, or a start the render thread overtook
        // between scheduleStart() and here. The frames owed to the device
        // frames already handed out as silence are skipped, so the room lands
        // on the line and streamFrameAudible() stays true.
        skipRemaining_ = active_.skipFrames + late;
        playing_ = true;
    }

    // How much of this period is still on the timeline. A scheduled pause or
    // stop ends it to the frame, the same way the start began it.
    uint32_t limit = frameCount;
    bool ending = false;
    if (active_.endGeneration != 0 && active_.endDeviceFrame < cursor_ + frameCount)
    {
        ending = true;
        limit = active_.endDeviceFrame > cursor_
                    ? static_cast<uint32_t>(active_.endDeviceFrame - cursor_)
                    : 0;
        if (limit < produced)
            limit = produced; // the end lands inside the silence before the start

        // A stop or a flush whose frame has already been played: the cut is
        // now, and mid-waveform. The buffer it drops still holds what came next.
        if (limit == 0 && declick && audible_ && active_.endDropsBuffer)
            limit = produced = renderFadeTail(target, frameCount);
    }

    // Frames whose place on the timeline has gone past: the audio that was
    // buffered before the scheduled frame, and anything that arrived too late
    // to be played where it belonged. Dropped, not played late -- the client
    // never catches up by playing faster, and the mapping the server is told
    // about has to stay true.
    if (skipRemaining_ != 0)
    {
        const uint32_t dropped = buffer_.discard(
            skipRemaining_ > 0xFFFFFFFFULL ? 0xFFFFFFFFu : static_cast<uint32_t>(skipRemaining_));
        skipRemaining_ -= dropped;
        if (dropped != 0)
            skipped_.fetch_add(dropped, std::memory_order_relaxed);
    }

    bool audibleAtEnd = false;
    if (skipRemaining_ == 0 && limit > produced)
    {
        unsigned char* popped = target + static_cast<size_t>(produced) * frameBytes;
        const uint32_t wanted = limit - produced;
        const uint32_t count = buffer_.pop(popped, wanted);
        if (leftJustify24_)
            leftJustifyInt24In32(popped, count * (frameBytes / 4));
        if (declick)
            fadeEdges(popped, count, cursor_ + produced, count < wanted);
        produced += count;
        audibleAtEnd = count != 0 && count == wanted && !ending;
    }

    if (produced < limit)
    {
        // The backend silences the rest and counts the period as an underrun,
        // which is what phase 8 already reports. The timeline moved on while
        // it did, so the same number of frames is owed to the skip above.
        const uint32_t missing = limit - produced;
        underrun_.fetch_add(missing, std::memory_order_relaxed);
        skipRemaining_ += missing;
    }

    if (ending && produced == limit)
    {
        // The silence after the scheduled frame is written here rather than
        // left to the backend: returning short would have it count an underrun
        // for silence that was asked for.
        std::memset(target + static_cast<size_t>(limit) * frameBytes, 0,
                    static_cast<size_t>(frameCount - limit) * frameBytes);
        produced = frameCount;
    }

    volume_.process(target, produced, periodStart);
    // The frames the backend fills with silence are on the device axis too:
    // a ramp or a scheduled change moves through them as it would through
    // audio, rather than starting where it left off when the audio returns.
    volume_.silence(frameCount - produced, periodStart + produced);

    if (ending)
        applyEnd();

    audible_ = audibleAtEnd;
    advance(frameCount);
    state_.store(static_cast<uint32_t>(ended_ ? silentState() : TimelineState::Playing),
                 std::memory_order_relaxed);
    return produced;
}

void TimelineSource::applyEnd()
{
    playing_ = false;
    ended_ = true;
    endDroppedBuffer_ = active_.endDropsBuffer;
    endWasResync_ = active_.endIsResync;
    endWasFlush_ = active_.endIsFlush;
    appliedEndGeneration_ = active_.endGeneration;

    if (!endDroppedBuffer_)
        return;

    // STOP_AT: what is still buffered belongs to a timeline nobody is playing
    // any more, and a later start would have to skip its way through all of
    // it. Dropping it moves an index and copies nothing, which is what makes
    // it allowable here at all. It is not counted as a skipped frame: those
    // are frames that missed their moment on the timeline, and these had no
    // moment left to miss.
    //
    // A discarded timeline (section 43) takes the same path for the same
    // reason, and the count below is what tells its caller that the buffer is
    // free for the audio of the position the room is rejoining at. A flush's
    // cut takes it too, and counts what it dropped on its own.
    const uint32_t dropped = buffer_.discard(buffer_.readableFrames());
    if (endWasFlush_)
        flushed_.fetch_add(dropped, std::memory_order_relaxed);
    drops_.fetch_add(1, std::memory_order_release);
}

void TimelineSource::fadeEdges(unsigned char* data, uint32_t count, uint64_t firstFrame,
                               bool runsDry)
{
    if (count == 0)
        return;
    const uint32_t frameBytes = buffer_.bytesPerFrame();
    const double frames = static_cast<double>(fadeFrames_);

    // Up from silence: the first audio after a start, a resume or an underrun.
    // A fade-in longer than one period carries on in the next, which is what
    // fadeInDone_ remembers.
    if (!audible_)
        fadeInDone_ = 0;
    if (fadeInDone_ < fadeFrames_)
    {
        const uint32_t left = fadeFrames_ - fadeInDone_;
        const uint32_t n = count < left ? count : left;
        volume_.fade(data, n, static_cast<double>(fadeInDone_) / frames, 1.0 / frames);
        fadeInDone_ += n;
    }

    // Down to the frame an armed pause, stop or flush names. The fade begins
    // one fade ahead of it, or at the first audio after the end arrived when
    // it came with less notice than that, so it always begins at full level.
    // An end at or behind this audio is the render path's limit, not a fade.
    const uint64_t end = active_.endDeviceFrame;
    if (active_.endGeneration != 0 && end > firstFrame)
    {
        if (fadeOutGeneration_ != active_.endGeneration)
        {
            fadeOutGeneration_ = active_.endGeneration;
            fadeOutStart_ = end > firstFrame + fadeFrames_ ? end - fadeFrames_ : firstFrame;
        }
        const uint64_t from = firstFrame > fadeOutStart_ ? firstFrame : fadeOutStart_;
        const uint64_t to = firstFrame + count < end ? firstFrame + count : end;
        if (from < to)
        {
            const double length = static_cast<double>(end - fadeOutStart_);
            volume_.fade(data + static_cast<size_t>(from - firstFrame) * frameBytes,
                         static_cast<uint32_t>(to - from), static_cast<double>(end - from) / length,
                         -1.0 / length);
        }
    }

    // Down into an underrun: the buffer gave less than this period needed, so
    // its last frames are the last audio for now. An underrun that falls on a
    // period's edge, with the buffer empty exactly where the period ended, is
    // not seen coming and still cuts.
    if (runsDry)
    {
        const uint32_t n = count < fadeFrames_ ? count : fadeFrames_;
        volume_.fade(data + static_cast<size_t>(count - n) * frameBytes, n, 1.0,
                     -1.0 / static_cast<double>(n));
    }
}

uint32_t TimelineSource::renderFadeTail(unsigned char* target, uint32_t frameCount)
{
    // Played past the frame the cut names, by at most one fade: the cut is
    // late by then whatever is done, and the buffer is dropped straight after.
    const uint32_t count = buffer_.pop(target, frameCount < fadeFrames_ ? frameCount : fadeFrames_);
    if (leftJustify24_)
        leftJustifyInt24In32(target, count * (buffer_.bytesPerFrame() / 4));
    if (count != 0)
        volume_.fade(target, count, 1.0, -1.0 / static_cast<double>(count));
    return count;
}

void TimelineSource::advance(uint32_t frames)
{
    cursor_ += frames;
    cursorLow_.fetch_add(frames, std::memory_order_relaxed);
}

TimelineState TimelineSource::silentState() const
{
    if (ended_)
    {
        // A discarded timeline and a stopped one both leave an empty buffer
        // and a silent room, and they are not the same thing: one is a room
        // the server stopped, the other a room waiting for the START_AT that
        // puts it back in step (section 43).
        if (endWasResync_)
            return TimelineState::Rejoining;
        if (endWasFlush_)
            return TimelineState::Flushing;
        return endDroppedBuffer_ ? TimelineState::Stopped : TimelineState::Paused;
    }
    // A flush's resume waiting for its frame is still the gap between the cut
    // and the audio after it, not a start the server scheduled.
    if (hasActive_ && active_.startAfterFlush)
        return TimelineState::Flushing;
    return hasActive_ ? TimelineState::Waiting : TimelineState::Idle;
}

bool TimelineSource::scheduleStart(const AudioClockSnapshot& snapshot, uint64_t localStartNs,
                                   uint64_t streamFrame, uint64_t bufferHeadStreamFrame,
                                   std::string& error)
{
    if (state() == TimelineState::Playing)
    {
        // Moving a running stream to another timeline position is a
        // resynchronization (section 32), not a start, and this client does
        // not have one yet.
        error = "playback has already started";
        return false;
    }
    if (!buffer_.isConfigured())
    {
        error = "the audio ring buffer is not configured";
        return false;
    }
    if (!snapshot.isValid())
    {
        // Without the device's own position there is no frame axis to put the
        // start on, and counting from what was submitted is exactly what
        // section 48 forbids.
        error = "the audio device reports no clock position";
        return false;
    }
    if (localStartNs <= snapshot.monotonicTimeNs)
    {
        error = "the start is " +
                std::to_string((snapshot.monotonicTimeNs - localStartNs) / 1000000ULL) +
                " ms in the past";
        return false;
    }

    const uint64_t leadNs = localStartNs - snapshot.monotonicTimeNs;
    if (leadNs > kMaxStartLeadNs)
    {
        error = "the start is " + std::to_string(leadNs / kNanosecondsPerSecond) + " s away";
        return false;
    }

    // The device's nominal rate, not the measured one: over a lead of a few
    // seconds the difference is a frame or two, and the client corrects
    // nothing about its own rate anyway -- the server does that.
    const uint64_t leadFrames =
        leadNs * static_cast<uint64_t>(snapshot.nominalSampleRate) / kNanosecondsPerSecond;
    const uint64_t startDeviceFrame = snapshot.deviceFramePosition + leadFrames;

    const uint64_t cursor = submittedFrames();
    if (startDeviceFrame <= cursor)
    {
        // The frames that cover the start have already been handed to the
        // device as silence. Starting now would start late by exactly that
        // much, so it is refused instead.
        error = "the start falls inside audio already handed to the device";
        return false;
    }

    if (streamFrame < bufferHeadStreamFrame)
    {
        error = "timeline frame " + std::to_string(streamFrame) +
                " is older than the oldest buffered frame " +
                std::to_string(bufferHeadStreamFrame);
        return false;
    }

    // The caller vouches for the head position, which is stable until playback
    // begins: nothing drains the buffer before the start, so what arrives
    // afterwards only moves the far end.
    const uint64_t skip = streamFrame - bufferHeadStreamFrame;
    const uint32_t buffered = buffer_.readableFrames();
    if (skip > buffered)
    {
        error = "timeline frame " + std::to_string(streamFrame) +
                " is past the buffered timeline, which ends at " +
                std::to_string(bufferHeadStreamFrame + buffered);
        return false;
    }

    // A start clears whatever pause or stop preceded it: the end that was
    // armed belonged to the run that has just finished, and re-publishing it
    // would silence the new one at a frame nobody asked about.
    schedule_ = Schedule();
    schedule_.startDeviceFrame = startDeviceFrame;
    schedule_.skipFrames = static_cast<uint32_t>(skip);
    schedule_.startGeneration = ++generation_;
    publish(schedule_);
    forgetFlush();

    scheduled_ = true;
    startDeviceFrame_ = startDeviceFrame;
    startStreamFrame_ = streamFrame;
    hasEnd_ = false;
    endStreamFrame_ = 0;
    endDeviceFrame_ = 0;
    state_.store(static_cast<uint32_t>(TimelineState::Waiting), std::memory_order_relaxed);
    placeVolume();
    return true;
}

void TimelineSource::schedulePause(uint64_t streamFrame)
{
    scheduleEnd(streamFrame, false);
}

void TimelineSource::scheduleStop(uint64_t streamFrame)
{
    scheduleEnd(streamFrame, true);
}

// Sections 43 and 32. Not scheduleEnd() with a frame nobody can place: that
// one *reports* a timeline the room was never going to play, and this one
// takes away a timeline the room may well be playing right now. Frame 0 is
// behind every cursor, so the drop lands in the next callback.
void TimelineSource::discardTimeline()
{
    dropStart();
    forgetFlush();

    schedule_.endDeviceFrame = 0;
    schedule_.endDropsBuffer = true;
    schedule_.endIsResync = true;
    schedule_.endIsFlush = false;
    schedule_.endGeneration = ++generation_;
    publish(schedule_);

    // Not an end the server scheduled, so nothing here is reported as one:
    // hasScheduledEnd() is the armed PAUSE_AT or STOP_AT and a discarded
    // timeline is neither.
    hasEnd_ = false;
    endStreamFrame_ = 0;
    endDeviceFrame_ = 0;
    state_.store(static_cast<uint32_t>(TimelineState::Rejoining), std::memory_order_relaxed);
    placeVolume();
}

void TimelineSource::dropStart()
{
    schedule_.startDeviceFrame = 0;
    schedule_.skipFrames = 0;
    schedule_.startGeneration = 0;
    schedule_.startAfterFlush = false;

    scheduled_ = false;
    startDeviceFrame_ = 0;
    startStreamFrame_ = 0;
}

void TimelineSource::forgetFlush()
{
    hasFlush_ = false;
    flushKeepsLine_ = false;
    flushDeviceFrame_ = 0;
    resumeDeviceFrame_ = 0;
}

// Section 54. Placed like a pause, dropping like a stop, and unlike either it
// leaves the start alone: the audio after the cut is on the same line.
void TimelineSource::scheduleFlush(uint64_t streamFrame)
{
    uint64_t deviceFrame = 0;
    if (!deviceFrameForStream(streamFrame, deviceFrame))
        deviceFrame = 0; // nothing places the cut, so it is made at once

    // A pause or a stop that has ended the line by the cut leaves nothing for
    // the audio after it to carry on, and the room rejoins instead. One armed
    // beyond the cut goes with it: what lay past the cut is not the timeline
    // being played any more, and the server sends again what it still wants.
    flushKeepsLine_ = scheduled_ && !(hasEnd_ && endDeviceFrame_ <= deviceFrame);

    schedule_.endDeviceFrame = deviceFrame;
    schedule_.endDropsBuffer = true;
    schedule_.endIsResync = false;
    schedule_.endIsFlush = true;
    schedule_.endGeneration = ++generation_;
    publish(schedule_);

    hasFlush_ = true;
    flushDeviceFrame_ = deviceFrame;
    resumeDeviceFrame_ = 0;

    // Not an end the server scheduled, so it is not reported as one, and the
    // one it replaced is gone. A room that was already paused or stopped keeps
    // reporting that end: it is where that room stopped.
    if (flushKeepsLine_)
    {
        hasEnd_ = false;
        endStreamFrame_ = 0;
        endDeviceFrame_ = 0;
        placeVolume();
    }
}

bool TimelineSource::resumeAfterFlush(uint64_t streamFrame)
{
    uint64_t deviceFrame = 0;
    if (!hasFlush_ || !flushKeepsLine_ || !deviceFrameForStream(streamFrame, deviceFrame))
        return false;

    // A new start generation for the render path, on the device frame the
    // kept mapping names. The mapping itself -- startDeviceFrame_ and
    // startStreamFrame_ -- does not move, which is the whole difference from
    // a rejoin.
    schedule_.startDeviceFrame = deviceFrame;
    schedule_.skipFrames = 0;
    schedule_.startGeneration = ++generation_;
    schedule_.startAfterFlush = true;
    schedule_.endDeviceFrame = 0;
    schedule_.endGeneration = 0;
    schedule_.endDropsBuffer = false;
    schedule_.endIsResync = false;
    schedule_.endIsFlush = false;
    publish(schedule_);

    resumeDeviceFrame_ = deviceFrame;
    return true;
}

bool TimelineSource::inFlushGap(const AudioClockSnapshot& snapshot) const
{
    if (!hasFlush_ || !snapshot.isValid())
        return false;

    const uint64_t position = snapshot.deviceFramePosition;
    return position >= flushDeviceFrame_ &&
           (resumeDeviceFrame_ == 0 || position < resumeDeviceFrame_);
}

void TimelineSource::scheduleEnd(uint64_t streamFrame, bool dropBuffer)
{
    uint64_t deviceFrame = 0;
    if (deviceFrameForStream(streamFrame, deviceFrame))
    {
        // A frame already behind the render cursor ends playback at the next
        // callback rather than being refused: a room that stops late is better
        // than one that goes on playing after every other room stopped.
        schedule_.endDeviceFrame = deviceFrame;
    }
    else
    {
        // Nothing maps that frame: this room has no start, or the frame is
        // before the one it has. Either way no part of the timeline from there
        // on is to be played here, so the start goes with it -- an armed start
        // left behind would fire after every other room had stopped, which is
        // the one outcome a synchronized pause exists to prevent. The end is
        // then immediate, and frame 0 is behind every cursor.
        dropStart();
        schedule_.endDeviceFrame = 0;
    }

    schedule_.endDropsBuffer = dropBuffer;
    schedule_.endIsResync = false;
    schedule_.endIsFlush = false;
    schedule_.endGeneration = ++generation_;
    publish(schedule_);

    // A pause or a stop replaces a flush's cut, and a line the server has
    // ended is not one the audio after the cut may carry on.
    flushKeepsLine_ = false;

    hasEnd_ = true;
    endStreamFrame_ = streamFrame;
    endDeviceFrame_ = schedule_.endDeviceFrame;
    placeVolume();
}

bool TimelineSource::deviceFrameForStream(uint64_t streamFrame, uint64_t& deviceFrame) const
{
    if (!scheduled_ || streamFrame < startStreamFrame_)
        return false;

    deviceFrame = startDeviceFrame_ + (streamFrame - startStreamFrame_);
    return true;
}

void TimelineSource::setVolume(double gain, bool muted)
{
    volumeAt_ = false;
    volume_.set(gain, muted);
}

bool TimelineSource::setVolumeAt(double gain, bool muted, uint64_t streamFrame)
{
    uint64_t deviceFrame = 0;
    if (!deviceFrameForStream(streamFrame, deviceFrame))
        return false;

    volumeAt_ = true;
    volumeAtStreamFrame_ = streamFrame;
    volume_.setAt(gain, muted, waitsForResume(streamFrame) ? kHeldDeviceFrame : deviceFrame);
    return true;
}

// A paused room's device frames run on while its timeline does not, so a
// change past the pause is held rather than left to fire on elapsed device
// time, and the resume's start places it on its own mapping.
void TimelineSource::placeVolume()
{
    // One the render cursor has reached has come round, or comes round at the
    // next callback whatever is placed now.
    if (volumeAt_ && volume_.scheduledDeviceFrame() != kHeldDeviceFrame &&
        submittedFrames() >= volume_.scheduledDeviceFrame())
        volumeAt_ = false;
    if (!volumeAt_)
        return;

    uint64_t deviceFrame = kHeldDeviceFrame;
    if (!waitsForResume(volumeAtStreamFrame_) &&
        !deviceFrameForStream(volumeAtStreamFrame_, deviceFrame))
    {
        // The start has gone past it: its position has been played, or
        // skipped, so it is in force from now on.
        volumeAt_ = false;
        volume_.set(volume_.gain(), volume_.muted());
        return;
    }
    if (deviceFrame != volume_.scheduledDeviceFrame())
        volume_.setAt(volume_.gain(), volume_.muted(), deviceFrame);
}

void TimelineSource::cancel()
{
    if (!scheduled_ && generation_ == 0)
        return;

    schedule_ = Schedule(); // generation 0: no start and no end at all
    publish(schedule_);
    forgetFlush();

    // No start will place a held volume change any more, so it applies now.
    if (volumeAt_ && volume_.scheduledDeviceFrame() == kHeldDeviceFrame)
        volume_.set(volume_.gain(), volume_.muted());
    volumeAt_ = false;

    scheduled_ = false;
    startDeviceFrame_ = 0;
    startStreamFrame_ = 0;
    hasEnd_ = false;
    endStreamFrame_ = 0;
    endDeviceFrame_ = 0;
    state_.store(static_cast<uint32_t>(TimelineState::Idle), std::memory_order_relaxed);
}

TimelineState TimelineSource::state() const
{
    return static_cast<TimelineState>(state_.load(std::memory_order_relaxed));
}

bool TimelineSource::streamFrameAudible(const AudioClockSnapshot& snapshot,
                                          uint64_t& frame) const
{
    if (!scheduled_ || !snapshot.isValid())
        return false;
    if (snapshot.deviceFramePosition < startDeviceFrame_)
        return false; // the start has not come round yet: there is no such frame

    frame = startStreamFrame_ + (snapshot.deviceFramePosition - startDeviceFrame_);

    // A paused or stopped room stays where it stopped. The device runs on and
    // its position keeps growing, but the timeline it was playing does not, so
    // mapping the new position would report a frame nobody made audible.
    if (hasEnd_ && frame > endStreamFrame_)
        frame = endStreamFrame_;
    return true;
}

uint64_t TimelineSource::submittedFrames() const
{
    return cursorExtender_.extend(cursorLow_.load(std::memory_order_relaxed));
}

uint32_t TimelineSource::underrunFrames() const
{
    return underrun_.load(std::memory_order_relaxed);
}

uint32_t TimelineSource::skippedFrames() const
{
    return skipped_.load(std::memory_order_relaxed);
}

uint32_t TimelineSource::bufferDrops() const
{
    // Acquire against the discard it follows: a caller that sees the count
    // move has to see an empty buffer, because that is what it takes the
    // count to mean.
    return drops_.load(std::memory_order_acquire);
}

uint32_t TimelineSource::flushedFrames() const
{
    return flushed_.load(std::memory_order_relaxed);
}

void TimelineSource::storePublished(const Schedule& schedule)
{
    // Each 64-bit frame travels as two halves rather than as one atomic,
    // because a 64-bit atomic is not lock-free on ARMv6 and the audio thread
    // may not pay for the lock one would cost. A torn pair is a frame nobody
    // asked for, which is exactly what the sequence counter is there to
    // reject.
    published_.startFrameLow.store(static_cast<uint32_t>(schedule.startDeviceFrame),
                                   std::memory_order_relaxed);
    published_.startFrameHigh.store(static_cast<uint32_t>(schedule.startDeviceFrame >> 32),
                                    std::memory_order_relaxed);
    published_.skipFrames.store(schedule.skipFrames, std::memory_order_relaxed);
    published_.startGeneration.store(schedule.startGeneration, std::memory_order_relaxed);
    published_.startAfterFlush.store(schedule.startAfterFlush ? 1u : 0u,
                                     std::memory_order_relaxed);
    published_.endFrameLow.store(static_cast<uint32_t>(schedule.endDeviceFrame),
                                 std::memory_order_relaxed);
    published_.endFrameHigh.store(static_cast<uint32_t>(schedule.endDeviceFrame >> 32),
                                  std::memory_order_relaxed);
    published_.endGeneration.store(schedule.endGeneration, std::memory_order_relaxed);
    published_.endDropsBuffer.store(schedule.endDropsBuffer ? 1u : 0u, std::memory_order_relaxed);
    published_.endIsResync.store(schedule.endIsResync ? 1u : 0u, std::memory_order_relaxed);
    published_.endIsFlush.store(schedule.endIsFlush ? 1u : 0u, std::memory_order_relaxed);
}

TimelineSource::Schedule TimelineSource::loadPublished() const
{
    Schedule schedule;
    schedule.startDeviceFrame =
        static_cast<uint64_t>(published_.startFrameLow.load(std::memory_order_relaxed)) |
        (static_cast<uint64_t>(published_.startFrameHigh.load(std::memory_order_relaxed)) << 32);
    schedule.skipFrames = published_.skipFrames.load(std::memory_order_relaxed);
    schedule.startGeneration = published_.startGeneration.load(std::memory_order_relaxed);
    schedule.startAfterFlush = published_.startAfterFlush.load(std::memory_order_relaxed) != 0;
    schedule.endDeviceFrame =
        static_cast<uint64_t>(published_.endFrameLow.load(std::memory_order_relaxed)) |
        (static_cast<uint64_t>(published_.endFrameHigh.load(std::memory_order_relaxed)) << 32);
    schedule.endGeneration = published_.endGeneration.load(std::memory_order_relaxed);
    schedule.endDropsBuffer = published_.endDropsBuffer.load(std::memory_order_relaxed) != 0;
    schedule.endIsResync = published_.endIsResync.load(std::memory_order_relaxed) != 0;
    schedule.endIsFlush = published_.endIsFlush.load(std::memory_order_relaxed) != 0;
    return schedule;
}

void TimelineSource::publish(const Schedule& schedule)
{
    const uint32_t sequence = sequence_.load(std::memory_order_relaxed);

    // Odd while the fields are written. The fence keeps them from moving above
    // it, which is what would let a reader see a torn schedule behind an even
    // sequence number.
    sequence_.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    storePublished(schedule);
    sequence_.store(sequence + 2, std::memory_order_release);
}

bool TimelineSource::adopt()
{
    const uint32_t before = sequence_.load(std::memory_order_acquire);
    if (before == lastSequence_)
        return false; // nothing new since the last callback
    if ((before & 1u) != 0)
        return false; // a write is in progress; the next callback will see it

    const Schedule schedule = loadPublished();
    // Relaxed is enough for the re-read: the acquire fence keeps the field
    // loads above from moving below it. This is the kernel's seqcount read.
    std::atomic_thread_fence(std::memory_order_acquire);
    if (sequence_.load(std::memory_order_relaxed) != before)
        return false; // it changed while it was being read

    lastSequence_ = before;

    // Only a *start* that is new resets the render path. A pause or a stop
    // arrives as an amendment to the start that is already running, and
    // treating it as a new schedule would replay that start from the top.
    const bool startChanged = schedule.startGeneration != active_.startGeneration;
    active_ = schedule;
    hasActive_ = schedule.startGeneration != 0;
    return startChanged;
}

} // namespace multiroom
