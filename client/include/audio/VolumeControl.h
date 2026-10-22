#pragma once

// The render path's gain: what SET_VOLUME (MATP section 39) actually does to
// the samples, and the only place in the client that changes them.
//
// It sits between the ring buffer and the device rather than in front of the
// buffer on purpose. What is buffered is what the server sent this room, byte
// for byte -- scaling it there would end the bitperfect transport of section 2
// for a room that only wanted to be quieter, and the buffer is also what
// BUFFER_STATUS describes. Scaling on the way out leaves all of that true and
// makes a volume change take effect at a known frame instead of whenever the
// buffer happens to drain.
//
// Three decisions the section leaves open:
//
//   * **A change is ramped, not stepped.** A gain that jumps between two
//     periods is a step in the waveform, which is a click, and the quieter the
//     room the more audible it is. The ramp below is short enough to be a
//     volume change and long enough not to be a click.
//   * **The schedule is a device frame.** Section 39 gives a timeline frame;
//     TimelineSource converts it, because it owns the mapping between the two
//     and the render cursor is the only axis the audio thread can compare
//     against without reading a clock.
//   * **The gain is clamped to [0, 1].** There is no headroom in a fixed-point
//     stream, so a server asking for 1.5 would get clipping rather than
//     volume. Clamping here keeps the sample loop free of a per-sample branch,
//     which is the other half of the same decision.
//
// Threading is TimelineSource's: the control thread sets, the audio thread
// scales, and the two meet through a seqlock rather than a mutex. Nothing here
// allocates, locks or logs.
//
// **Single producer, single consumer, and not by accident.** The seqlock below
// is an SPSC construct and nothing else:
//
//   * **One producer, and it is enforced.** `prepare()`, `set()` and `setAt()`
//     must all be called from the same thread. Two of them running at once
//     corrupts the sequence counter -- `publish()` reads it, makes it odd and
//     makes it even again with no interlock, so two producers can leave it
//     even mid-write and the reader will take a torn target as a good one.
//     `generation_` is plain, unsynchronized state for the same reason.
//
//     So the first thread to call any of the three claims the object, and the
//     others are turned away by `claimProducer()` before they write a single
//     field -- not warned and let through, because a warning does not stop a
//     race. A refused call changes nothing and is logged once. The render path
//     pays nothing for this: the check is on the control path only, and
//     `process()` and `silence()` never touch it.
//
//     A refused `prepare()` therefore leaves the old format in place. That is
//     deliberate -- it keeps the gain and mute the room already had -- but it
//     means a client that has tripped this is misconfigured in a way only one
//     producer fixes. The log line says so.
//   * **One consumer.** `process()` and `silence()` must both be called from
//     the render callback. They share `pending_`, `current_`, `lastSequence_`
//     and the rest of the audio-thread block with no synchronization at all,
//     because a device has exactly one render callback in flight.
//   * `prepare()` writes the audio thread's own fields directly, so it is
//     legal only with the device closed and no callback running.
//
// **A mailbox, not a queue.** A change published while an earlier one is still
// waiting *replaces* it; `adopt()` only ever carries the newest target across.
// That is the right semantic here because SET_VOLUME names an absolute gain
// rather than a delta, so the newest message is the state the server wants --
// a dragged volume slider coalesces into one ramp instead of replaying every
// intermediate step. What it gives up: two changes scheduled for two different
// future frames, published inside the same adoption window, collapse to the
// later one, so the room never passes through the first. Nothing in the client
// schedules ahead like that today. If a server ever needs it -- a duck and a
// restore armed together -- this becomes a small fixed-size SPSC ring of
// targets sorted by device frame, which is the point at which the
// coalescing above stops being correct rather than merely economical.

#include "audio/AudioFormat.h"

#include <atomic>
#include <cstdint>

namespace multiroom {

class VolumeControl
{
public:
    VolumeControl();

    // Control thread, with the device closed. Keeps the gain and the mute
    // state -- a reopened device is not a reason for a room to come back at
    // full volume -- and drops the ramp, which belongs to the old format.
    void prepare(const AudioFormat& format);

    // Control thread. Takes effect at the next render callback.
    void set(double gain, bool muted);

    // Control thread. Takes effect when the render cursor reaches
    // `deviceFrame`. One that is already behind the cursor takes effect at the
    // next callback: a room that changes volume late is better than one that
    // never changes it at all.
    //
    // `deviceFrame` is this device's own render cursor and never leaves the
    // client. Section 39 schedules on the *media* timeline, in nanoseconds, and
    // that position has to be mapped twice before it gets here:
    // TimelineSource::setVolumeAt() is the only caller, it converts a stream
    // frame through deviceFrameForStream(), and it refuses when no mapping
    // exists yet rather than inventing one. Two rooms handed the same timeline
    // position arrive at different device frames -- different rates, different
    // ratios -- which is the entire reason the mapping exists. This is why
    // `volume_` is a private member of TimelineSource and this class is not
    // part of any public surface.
    void setAt(double gain, bool muted, uint64_t deviceFrame);

    // Audio thread. Scales `frameCount` frames in place, where `cursor` is the
    // device frame of the first of them.
    void process(void* buffer, uint32_t frameCount, uint64_t cursor);

    // Audio thread, for a period of silence the source produced itself.
    // Carries the gain forward by `frameCount` frames without touching a
    // sample -- multiplying silence by anything is still silence -- and is
    // otherwise process() exactly: a pending change lands on the device frame
    // the server named, ramps from there, and the rest of the period carries
    // that ramp on.
    //
    // These frames count against a ramp at the same rate audible ones do,
    // because a ramp is a span of the device's timeline and not a quantity of
    // audio. A room paused 5 ms into a 15 ms ramp comes back 5 ms into it; one
    // silent for longer than the ramp comes back at the target, which is what
    // puts a volume set during the wait for a start in place on the first
    // audible frame rather than fading into it.
    void silence(uint32_t frameCount, uint64_t cursor);

    // Audio thread. Scales `frameCount` frames in place by a straight line
    // that starts at `gain` and moves by `step` per frame, in the format the
    // last prepare() granted. Stateless and unscheduled: it is the sample loop
    // above lent to TimelineSource's de-click, which fades the edges of the
    // audio it plays and must not touch the room's volume to do it.
    void fade(void* buffer, uint32_t frameCount, double gain, double step) const;

    // Control thread: what the server last asked for.
    double gain() const { return gain_; }
    bool muted() const { return muted_; }
    bool isScheduled() const { return scheduled_; }
    uint64_t scheduledDeviceFrame() const { return scheduledFrame_; }

    // What the audio thread last multiplied by, to a thousandth. It is how a
    // scheduled change is seen to have happened, and it differs from gain()
    // while a ramp is running or a schedule has not come round.
    double appliedGain() const;

private:
    // Published to the audio thread by the same seqlock TimelineSource uses,
    // and for the same reason: no lock-free 64-bit atomic exists on ARMv6.
    struct Target
    {
        double gain = 1.0;
        uint64_t deviceFrame = 0; // 0 with generation != 0 means "at once"
        bool immediate = true;
        uint32_t generation = 0;
    };

    // The seqlock's payload. A sequence number makes a read *consistent*; it
    // does not make it *defined*. Two threads touching a plain `Target` while
    // the number is odd is a data race however the fences are placed, and a
    // compiler entitled to assume there is none may split, reload or sink
    // those accesses. Relaxed atomics are what the fences need and all they
    // need -- the release in publish() and the acquire in adopt() carry the
    // ordering, the sequence number still decides whether a read counts -- and
    // 32-bit halves keep every field lock-free on ARMv6.
    struct PublishedTarget
    {
        std::atomic<uint32_t> gainLow{0};
        std::atomic<uint32_t> gainHigh{0};
        std::atomic<uint32_t> frameLow{0};
        std::atomic<uint32_t> frameHigh{0};
        std::atomic<uint32_t> immediate{0};
        std::atomic<uint32_t> generation{0};
    };

    bool claimProducer(); // control thread: false turns a second one away
    void publish(double gain, uint64_t deviceFrame, bool immediate);
    void storePublished(const Target& target); // control thread
    Target loadPublished() const;              // audio thread
    void adopt();                                  // audio thread
    // audio thread: adopt(), then the frames of this period before the pending change
    uint32_t pendingSplit(uint32_t frameCount, uint64_t cursor);
    void beginRamp(double target);                 // audio thread
    void advance(uint32_t frames);                 // audio thread: scale() with no samples
    void scale(unsigned char* data, uint32_t frames); // audio thread

    // Control thread only.
    double gain_ = 1.0;
    bool muted_ = false;
    bool scheduled_ = false;
    uint64_t scheduledFrame_ = 0;
    uint32_t generation_ = 0;

    // The producer this object has seen, folded to 32 bits, and whether a
    // second one has already been reported. Atomic not because the audio
    // thread reads them -- it never does -- but because the one case they
    // exist to describe is two control threads, and the check may not be a
    // race of its own. Nothing here is on the render path.
    std::atomic<uint32_t> producer_{0};
    std::atomic<uint32_t> producerReported_{0};

    std::atomic<uint32_t> sequence_;
    PublishedTarget published_;

    // Audio thread only.
    AudioFormat::SampleType sampleType_ = AudioFormat::SampleType::Int16;
    uint16_t channels_ = 0;
    uint32_t bytesPerFrame_ = 0;
    uint32_t rampFrames_ = 0;
    uint32_t lastSequence_ = 0;
    bool hasPending_ = false;
    Target pending_;
    double current_ = 1.0;
    double target_ = 1.0;
    double step_ = 0.0;
    uint32_t rampRemaining_ = 0;

    // Milli-units, because there is no lock-free atomic<double> either.
    std::atomic<uint32_t> applied_;
};

} // namespace multiroom
