#pragma once

// The audio thread's end of the ring buffer for the server's stream: it plays
// what the server sent this room, and it starts it at the instant the server
// named (MATP sections 20, 21 and 48).
//
// RingBufferSource plays whatever the buffer holds as soon as the device asks
// for it. That is what the test tone wants and it is the one thing section 48
// forbids for a synchronized room, so the stream gets this source instead:
// until a start has been scheduled it feeds the device silence and does not
// touch the buffer. The device therefore runs from the moment the stream is
// prepared -- its clock is established and its buffer primed by the time the
// start comes round, which is what section 21 asks for -- while the prebuffer
// still fills, because nothing is drained out of it yet.
//
// **This class does not speak the zone's media timeline.** It works on the
// *stream* axis: the frames of audio the server actually sent this room, in
// the order it sent them, counted from the first one of the stream. Those are
// not timeline nanoseconds and they are not comparable with another room's
// frames -- each room has its own rate and its own resampling ratio (section
// 33) -- and every frame count below, `startStreamFrame` included, is on that
// axis. Converting a timeline position the server named into a stream frame is
// matp::TimelineMap's work and StreamPlayback's to ask for; what arrives here
// has already been converted. That division is deliberate: the conversion
// needs the packet history, which belongs to the audio connection, and this
// class needs to stay something a test can drive with a ring buffer and no
// protocol at all.
//
// Four decisions the protocol leaves open and this class cannot avoid:
//
//   * **Frames handed to the device are the axis.** The n-th frame written to
//     the device is the n-th frame it plays, so the render cursor below counts
//     the same thing the driver's deviceFramePosition counts, only further
//     ahead by whatever the device still has queued. A scheduled instant is
//     converted once, on the control thread, into a position on that axis; the
//     render callback then only compares two frame counts, which is what makes
//     the start sample-accurate rather than as accurate as a wakeup.
//   * **Silence that was played is time that passed.** When the buffer runs
//     dry the device plays silence, and the timeline moved on while it did.
//     The frames that arrive afterwards belong to positions that are gone, so
//     they are dropped rather than played late: the alternative is a room that
//     is permanently behind by the length of the gap and a
//     streamFrameAudible that reports a position the room is not at.
//   * **The mapping is fixed at the start and never chased.** stream =
//     startStreamFrame + (devicePosition - startDeviceFrame). The client
//     measures and reports; the server resamples (rule 5 of the briefing), so
//     nothing here ever speeds the buffer up to catch up. That the server's
//     resampling makes this room's frames run a few ppm away from the media
//     time they cover is exactly why this equation is on the stream axis: in
//     nanoseconds of timeline it would be false, and slowly.
//
// The same mapping is what makes the rest of the transport exact: a pause, a
// stop and a scheduled volume change all name a frame, and all three land on
// the device frame that mapping gives them rather than on whichever period the
// message happened to arrive in.
//
// **De-click** (setDeclick(), client.json's `declick`). Every one of those
// transport edges cuts the waveform at a frame, and a waveform cut away from
// zero is a click -- loudest in a bitperfect room, where nothing turns it
// down. With the de-click on, the audio is faded over 10 ms at each edge:
// up from the first frame of a start, a resume, a flush's resume and the
// return from an underrun; down to the frame a pause, a stop or a flush's cut
// names; and down over the last frames before the buffer runs dry. A cut that
// has to happen now -- a stop or a flush whose frame has already been played,
// a discarded timeline -- plays up to 10 ms more of the buffer it is about to
// drop, fading to silence. A pause that arrives late is still cut: its buffer
// is the resume's and may not be eaten into. Nothing moves on the timeline,
// and the fade is not the room's volume: SET_VOLUME and status are untouched.
// It does change samples, bitperfect rooms included, but only those at an
// edge, where the audio is being cut anyway.

#include "audio/AudioClockSnapshot.h"
#include "audio/AudioRingBuffer.h"
#include "audio/AudioSource.h"
#include "audio/VolumeControl.h"

#include "util/Wrap32.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace multiroom {

enum class TimelineState
{
    Idle,    // no start scheduled: the device is fed silence
    Waiting, // a start is scheduled and has not come round yet
    Playing, // the timeline is being played
    Paused,  // PAUSE_AT has come round; the buffer is kept for the resume
    Stopped, // STOP_AT has come round; what was buffered has been dropped
    Rejoining, // the timeline was discarded; the room is waiting to rejoin
    Flushing // a FLUSH has cut the line; silence until the audio after it
};

const char* timelineStateName(TimelineState state);

class TimelineSource final : public IAudioSource
{
public:
    // Sized in milliseconds for the same reason RingBufferSource is: the frame
    // size and the rate are only known once the driver has granted a format.
    TimelineSource(AudioRingBuffer& buffer, uint32_t capacityMilliseconds);

    // Control thread, before the audio thread starts. Sizes the buffer for the
    // granted format and drops any schedule: a reopened device is a new frame
    // axis, and a position on the old one means nothing on it.
    void prepare(const AudioFormat& format) override;

    // Control thread. The capacity the next prepare() sizes the buffer for;
    // the buffer that is open now keeps its size.
    void setCapacityMilliseconds(uint32_t capacityMilliseconds)
    {
        capacityMilliseconds_ = capacityMilliseconds;
    }

    uint32_t render(void* buffer, uint32_t frameCount) override;

    // Control thread. Works out where the scheduled instant falls on the
    // device's own frame axis and arms the start. `localStartNs` is the
    // START_AT instant already converted to this machine's monotonic clock,
    // `streamFrame` the frame that must be audible at it, and
    // `bufferHeadStreamFrame` where the oldest frame the
    // holds sits on that same axis -- which is stable until playback begins,
    // because nothing drains the buffer before then.
    //
    // Returns false with a reason rather than starting approximately: a room
    // that starts at the wrong moment is worse than a room that says it could
    // not start.
    bool scheduleStart(const AudioClockSnapshot& snapshot, uint64_t localStartNs,
                       uint64_t streamFrame, uint64_t bufferHeadStreamFrame,
                       std::string& error);

    // Control thread. PAUSE_AT and STOP_AT (sections 37 and 38), placed by the
    // stream frame that the timeline position the server named converts to: a
    // playing room maps that onto its own device axis to the frame, which is
    // what "the same logical position" of section 37 means.
    //
    // The two differ in what is left behind. A pause keeps the buffer, because
    // section 37 resumes with a new START_AT and the audio to resume with is
    // the audio already here. A stop drops it: what is buffered belongs to a
    // timeline nobody is playing any more, and holding seconds of it would
    // make the next start skip its way through them.
    //
    // Neither stops the device. It keeps running and keeps feeding the sound
    // card silence, so its clock stays established and the room can be
    // scheduled again without waiting for a driver to open (section 21).
    //
    // Neither can fail, which is why they return nothing. A frame this room
    // cannot place -- it has no start, or the frame is before the one it has
    // -- means no part of that timeline is to be played here, so the schedule
    // is dropped instead: refusing would leave an armed start to fire after
    // every other room had stopped, which is the one outcome a synchronized
    // pause exists to prevent. `isScheduled()` is false afterwards, and that
    // is how the caller tells the two apart.
    void schedulePause(uint64_t streamFrame);
    void scheduleStop(uint64_t streamFrame);

    // Control thread. Drops the start and everything the ring buffer holds,
    // for a rejoin (section 43) or a hard resync (section 32): the audio that
    // follows belongs to a timeline position this class's mapping knows
    // nothing about, so the mapping and the buffer both have to be gone
    // before it arrives.
    //
    // Only the thread that reads the buffer may empty it, so the drop is
    // armed here and happens in the next render callback; bufferDrops() is
    // how the caller learns that it has, and therefore that the buffer is
    // free for the audio of the new position.
    //
    // Like a pause and a stop it leaves the device alone: it keeps running
    // and keeps its clock, because a rejoin arrives as a START_AT and section
    // 21 wants the device primed before one.
    void discardTimeline();

    // Control thread. FLUSH (section 54), in two steps, because the audio
    // that follows the cut arrives separately from the message that announces
    // it.
    //
    // scheduleFlush() places the cut the way schedulePause() places a pause,
    // through the start mapping, and at that device frame the render callback
    // drops the buffer and goes silent. It does not count that silence as an
    // underrun, because the server asked for it. Unlike discardTimeline() it
    // keeps the start mapping. Until resumeAfterFlush() the callback goes on
    // dropping whatever reaches the buffer: the audio still in flight belongs
    // after the cut, and a producer left to fill the buffer with it would
    // block before reaching the packet that ends the flush. A cut this room
    // cannot place is made at once.
    //
    // resumeAfterFlush() is for once the audio connection has parked on the
    // first packet after the cut. `streamFrame` is where that packet belongs
    // on the stream axis of the kept mapping, and it plays at the device frame
    // that mapping gives it. The callback empties the buffer once more when it
    // adopts this, which is safe because nothing is producing, and
    // bufferDrops() moves when it has. Frames that arrive after their device
    // frame has passed are skipped, the way a late start skips them.
    //
    // False when there is no line to keep: the room had no start, or a pause
    // or a stop ended it at or before the cut, or `streamFrame` is before the
    // start. The caller then rejoins instead, as for a jump the server did not
    // announce.
    void scheduleFlush(uint64_t streamFrame);
    bool resumeAfterFlush(uint64_t streamFrame);

    // Control thread. Whether the sound card is between a cut and the audio
    // after it, where no timeline position is playing. The caller reports the
    // cut's position for that stretch, as it would for a pause.
    bool inFlushGap(const AudioClockSnapshot& snapshot) const;

    // Control thread. Forgets the schedule; the device is fed silence again.
    void cancel();

    TimelineState state() const;

    // The armed start, meaningful while state() is not Idle.
    bool isScheduled() const { return scheduled_; }
    uint64_t startDeviceFrame() const { return startDeviceFrame_; }
    uint64_t startStreamFrame() const { return startStreamFrame_; }

    // The armed pause or stop, meaningful while hasScheduledEnd() is true.
    bool hasScheduledEnd() const { return hasEnd_; }
    uint64_t endStreamFrame() const { return endStreamFrame_; }
    uint64_t endDeviceFrame() const { return endDeviceFrame_; }

    // Control thread. Where a stream frame falls on the device's own frame
    // axis, which only exists once a start has placed the two against each
    // other. False for a frame before the start: nothing maps it.
    bool deviceFrameForStream(uint64_t streamFrame, uint64_t& deviceFrame) const;

    // Control thread. Whether the timeline is held short of `streamFrame`:
    // a pause or a stop is armed at or before it, or there is no start. The
    // device runs on through either, so a change scheduled there waits for
    // the start that resumes the timeline instead of for a device frame.
    bool waitsForResume(uint64_t streamFrame) const
    {
        return !scheduled_ || (hasEnd_ && streamFrame >= endStreamFrame_);
    }

    // Control thread: SET_VOLUME (section 39). The unscheduled form takes
    // effect at the next callback; the scheduled one converts the section's
    // stream frame through the mapping above and returns false when there is
    // no mapping yet, which leaves the caller to decide between applying it now
    // and refusing. A scheduled change keeps its stream frame: while
    // waitsForResume() it is held, and every new start, end or rejoin places
    // it again -- at once when the resume has gone past it.
    void setVolume(double gain, bool muted);
    bool setVolumeAt(double gain, bool muted, uint64_t streamFrame);

    double volumeGain() const { return volume_.gain(); }
    bool volumeMuted() const { return volume_.muted(); }
    bool volumeScheduled() const { return volume_.isScheduled(); }
    double appliedGain() const { return volume_.appliedGain(); }

    // Any thread. Whether the edges of the audio are faded (see De-click
    // above). Off until set; the next callback follows it.
    void setDeclick(bool enabled) { declick_.store(enabled, std::memory_order_relaxed); }
    bool declick() const { return declick_.load(std::memory_order_relaxed); }

    // The stream frame the sound card is putting out at the instant of the
    // snapshot, from the hardware's own position rather than from what has
    // been submitted (section 48). False until the start has come round, which
    // is when there is no such frame rather than when it is zero.
    //
    // A paused or stopped room stays at the frame it ended on: the device runs
    // on, but the timeline it was playing does not.
    bool streamFrameAudible(const AudioClockSnapshot& snapshot, uint64_t& frame) const;

    // Frames handed to the device since prepare(), silence included: the
    // render cursor, and the near end of the axis a schedule sits on.
    uint64_t submittedFrames() const;

    // Frames the device had to silence because the buffer was dry, and frames
    // dropped to stay on the timeline (the audio that was buffered before the
    // scheduled frame, plus whatever arrived too late to be played where it
    // belonged).
    uint32_t underrunFrames() const;
    uint32_t skippedFrames() const;

    // How often the render path has dropped the whole buffer: a STOP_AT that
    // came round, every discardTimeline() that did, a flush's cut and a
    // flush's resume. Not counted as skipped frames -- a skipped frame is one
    // that missed its moment on the timeline, and these had no moment left to
    // miss.
    uint32_t bufferDrops() const;

    // Frames a flush dropped, at the cut and until its resume.
    uint32_t flushedFrames() const;

private:
    // What the audio thread needs to know about a start, published without a
    // lock and without a 64-bit atomic -- there is no lock-free one on ARMv6.
    // A sequence counter is made odd while the two fields are written and even
    // again afterwards; a render callback that sees an odd counter, or a
    // different one on either side of the read, saw a half-written schedule
    // and keeps the one it already had. It never waits for the control thread,
    // which is the whole point of doing it this way.
    struct Schedule
    {
        uint64_t startDeviceFrame = 0;
        uint32_t skipFrames = 0;      // buffered audio that precedes the start
        uint32_t startGeneration = 0; // 0 is no start; a re-arm is noticed even
                                      // when the frame it names is the same
        bool startAfterFlush = false; // a flush's resume: empties the buffer
        uint64_t endDeviceFrame = 0;
        uint32_t endGeneration = 0;  // 0 is no pause and no stop
        bool endDropsBuffer = false; // a stop does, a pause does not
        bool endIsResync = false;    // a discarded timeline, not a stopped one
        bool endIsFlush = false;     // a flush's cut, which keeps the start
    };

    // The seqlock's payload. A sequence counter makes a read *consistent*; it
    // does not make it *defined*. Two threads touching a plain `Schedule`
    // while the counter is odd is a data race however the fences are placed,
    // and a compiler entitled to assume there is none may split, reload or
    // sink those accesses. Relaxed atomics are what the fences need and all
    // they need -- the release in publish() and the acquire in adopt() carry
    // the ordering, the counter still decides whether a read counts -- and
    // 32-bit halves keep every field lock-free on ARMv6.
    struct PublishedSchedule
    {
        std::atomic<uint32_t> startFrameLow{0};
        std::atomic<uint32_t> startFrameHigh{0};
        std::atomic<uint32_t> skipFrames{0};
        std::atomic<uint32_t> startGeneration{0};
        std::atomic<uint32_t> startAfterFlush{0};
        std::atomic<uint32_t> endFrameLow{0};
        std::atomic<uint32_t> endFrameHigh{0};
        std::atomic<uint32_t> endGeneration{0};
        std::atomic<uint32_t> endDropsBuffer{0};
        std::atomic<uint32_t> endIsResync{0};
        std::atomic<uint32_t> endIsFlush{0};
    };

    void dropStart(); // control thread: forgets the start, leaves the end alone
    void placeVolume(); // control thread: the scheduled volume change on the schedule now in force
    void forgetFlush(); // control thread: a new start or no start ends the flush
    void scheduleEnd(uint64_t streamFrame, bool dropBuffer);
    void publish(const Schedule& schedule);
    void storePublished(const Schedule& schedule); // control thread
    Schedule loadPublished() const;                // audio thread
    bool adopt();                  // audio thread: true when the *start* changed
    void applyEnd();               // audio thread: the pause or stop has come round
    void advance(uint32_t frames); // audio thread: moves the render cursor on
    // Audio thread, de-click: fades the `count` frames just taken from the
    // buffer, the first of them at device frame `firstFrame`, where they
    // begin or end the audio; `runsDry` when the buffer gave fewer than asked.
    void fadeEdges(unsigned char* data, uint32_t count, uint64_t firstFrame, bool runsDry);
    // Audio thread, de-click: plays up to one fade of the buffer, going down
    // to silence, ahead of a cut that has to happen now. Returns the frames.
    uint32_t renderFadeTail(unsigned char* target, uint32_t frameCount);
    TimelineState silentState() const; // audio thread: why it is feeding silence

    AudioRingBuffer& buffer_;
    uint32_t capacityMilliseconds_;

    // Control thread only.
    bool scheduled_ = false;
    uint64_t startDeviceFrame_ = 0;
    uint64_t startStreamFrame_ = 0;
    bool hasEnd_ = false;
    uint64_t endStreamFrame_ = 0;
    uint64_t endDeviceFrame_ = 0;
    uint32_t generation_ = 0;
    Schedule schedule_; // what was last published, so an end can amend a start
    // The scheduled volume change, by stream frame, until a start has gone
    // past it or a newer change replaced it.
    bool volumeAt_ = false;
    uint64_t volumeAtStreamFrame_ = 0;
    // The flush in progress, if there is one: where the cut is on the device
    // axis, where the audio after it begins (0 until resumeAfterFlush()), and
    // whether the line it cut is one a resume may carry on.
    bool hasFlush_ = false;
    bool flushKeepsLine_ = false;
    uint64_t flushDeviceFrame_ = 0;
    uint64_t resumeDeviceFrame_ = 0;
    mutable Wrap32Extender cursorExtender_;

    // The seqlock above.
    std::atomic<uint32_t> sequence_;
    PublishedSchedule published_;

    // Audio thread only, from prepare() until the next one.
    Schedule active_;
    bool hasActive_ = false;
    uint32_t lastSequence_ = 0;
    uint64_t cursor_ = 0;
    bool playing_ = false;
    bool ended_ = false;
    bool endDroppedBuffer_ = false;
    bool endWasResync_ = false;
    bool endWasFlush_ = false;
    // The end this callback has already acted on, so that a stop published
    // after a pause is still noticed: the room is silent either way, but one
    // of the two drops the buffer and the other does not.
    uint32_t appliedEndGeneration_ = 0;
    // Frames the timeline owes the device: 64 bits, because an underrun that
    // goes on adds to it every period, and 32 would wrap after 2^32 frames --
    // 25 hours at 48 kHz, 93 minutes at 768 -- and put the room on the wrong
    // position. Audio thread only, so it needs no atomic.
    uint64_t skipRemaining_ = 0;
    // Int24In32 on Windows and the ESP32: the wire's s24le is moved to the
    // left-justified layout WASAPI and I2S take as it leaves the buffer. Set by prepare().
    bool leftJustify24_ = false;

    // De-click. The switch is any thread's; the rest is the audio thread's,
    // from prepare() until the next one: the fade's length in frames, whether
    // the last frame handed to the device was audio, how far the fade-in has
    // got, and where the fade-out toward the armed end began (for which end).
    std::atomic<bool> declick_{false};
    uint32_t fadeFrames_ = 1;
    bool audible_ = false;
    uint32_t fadeInDone_ = 0;
    uint32_t fadeOutGeneration_ = 0;
    uint64_t fadeOutStart_ = 0;

    // The render path's gain (section 39). Owned here because this is the one
    // source the server's stream is played through, and because the mapping a
    // scheduled change needs is the one this class already keeps.
    VolumeControl volume_;

    // Published to the control thread. 32 bits and widened on the reading side
    // for the same reason as everywhere else in this client.
    std::atomic<uint32_t> cursorLow_;
    std::atomic<uint32_t> underrun_;
    std::atomic<uint32_t> skipped_;
    std::atomic<uint32_t> drops_;
    std::atomic<uint32_t> flushed_;
    std::atomic<uint32_t> state_;
};

} // namespace multiroom
