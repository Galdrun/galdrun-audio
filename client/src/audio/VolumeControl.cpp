#include "audio/VolumeControl.h"

#include "util/Logger.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <thread>

namespace multiroom {

// The seqlock below publishes every field as a 32-bit atomic precisely so the
// audio thread never touches a lock. If a target ever fails this, the class is
// silently taking one per volume change inside the render callback, which is
// the one thing the render callback may not do. ARMv6 is the target that makes
// this worth asserting: it has no 64-bit atomic, which is why nothing here is
// one. TimelineSource's seqlock rests on the same guarantee.
//
// The ESP32 is exempt because ESP-IDF compiles every atomic as a call once
// PSRAM is enabled (-mdisable-hardware-atomics: S32C1I does not work on
// external RAM), so the compiler cannot promise it. The call still uses S32C1I
// for internal RAM, where these objects live, and a few-instruction
// interrupt-masked section for PSRAM; neither can sleep.
#if !defined(ESP_PLATFORM)
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "the render path's seqlock needs a lock-free 32-bit atomic");
#endif

namespace {

// Long enough that the step is not a click, short enough that a volume change
// is a volume change and not a fade. At 48 kHz it is 720 frames.
const uint32_t kRampMilliseconds = 15;

// A 32-bit stand-in for the calling thread's identity. `std::thread::id` is
// not an integer and `std::atomic<std::thread::id>` is not portably lock-free,
// so it is hashed and folded to something this class can already hold. Zero is
// kept for "nobody has published yet". Two threads can in principle fold to
// the same tag, which costs a missed report and never a false one.
uint32_t currentThreadTag()
{
    const uint64_t hash = static_cast<uint64_t>(std::hash<std::thread::id>()(
        std::this_thread::get_id()));
    // Shifting a uint64_t by 32 is defined whether size_t was 32 or 64 bits.
    const uint32_t tag =
        static_cast<uint32_t>(hash) ^ static_cast<uint32_t>(hash >> 32);
    return tag == 0u ? 1u : tag;
}

double clampGain(double gain)
{
    // NaN falls through both comparisons and lands on the mute end, which is
    // the safe one: a gain nobody can interpret must not be a loud room.
    if (!(gain > 0.0))
        return 0.0;
    return gain > 1.0 ? 1.0 : gain;
}

// One scaled sample, rounded to nearest. lrint rather than a comparison
// against zero and an added half: the rounding mode does it in one
// instruction, and the loops below are meant to carry no branch per sample.
int32_t scaled(int32_t sample, double gain)
{
    return static_cast<int32_t>(std::lrint(static_cast<double>(sample) * gain));
}

// The four integer layouts differ only in how a sample is loaded and stored;
// the arithmetic is the same, and the gain is clamped to [0, 1] before it gets
// here, so no product can leave the range its own container already held.
void scaleInt16(unsigned char* data, uint32_t samples, double gain, double step,
                uint16_t channels)
{
    uint16_t channel = 0;
    for (uint32_t i = 0; i < samples; ++i)
    {
        int16_t sample = 0;
        std::memcpy(&sample, data, 2);
        sample = static_cast<int16_t>(scaled(sample, gain));
        std::memcpy(data, &sample, 2);
        data += 2;

        if (++channel == channels)
        {
            channel = 0;
            gain += step;
        }
    }
}

void scaleInt24(unsigned char* data, uint32_t samples, double gain, double step,
                uint16_t channels)
{
    uint16_t channel = 0;
    for (uint32_t i = 0; i < samples; ++i)
    {
        // Three packed bytes, little endian, sign carried by the top one.
        int32_t sample = static_cast<int32_t>(static_cast<uint32_t>(data[0]) |
                                              (static_cast<uint32_t>(data[1]) << 8) |
                                              (static_cast<uint32_t>(data[2]) << 16));
        if ((sample & 0x800000) != 0)
            sample |= static_cast<int32_t>(0xFF000000u);

        sample = scaled(sample, gain);
        const uint32_t stored = static_cast<uint32_t>(sample);
        data[0] = static_cast<unsigned char>(stored & 0xFF);
        data[1] = static_cast<unsigned char>((stored >> 8) & 0xFF);
        data[2] = static_cast<unsigned char>((stored >> 16) & 0xFF);
        data += 3;

        if (++channel == channels)
        {
            channel = 0;
            gain += step;
        }
    }
}

// Int32: every one of the 32 bits is signal, so the container *is* the value.
void scaleInt32(unsigned char* data, uint32_t samples, double gain, double step,
                uint16_t channels)
{
    uint16_t channel = 0;
    for (uint32_t i = 0; i < samples; ++i)
    {
        int32_t sample = 0;
        std::memcpy(&sample, data, 4);
        sample = scaled(sample, gain);
        std::memcpy(data, &sample, 4);
        data += 4;

        if (++channel == channels)
        {
            channel = 0;
            gain += step;
        }
    }
}

// Int24In32 is one enum value and two layouts, which is exactly why it is not
// Int32 and not this function's caller's problem: ALSA's S24_LE keeps the 24
// valid bits in the low three bytes, WASAPI and I2S left-justify them and
// leave the low byte as padding. ToneGenerator splits on the same `#if` for the same
// reason. Treating either as a plain 32-bit sample is wrong in a different way
// on each platform, so the loops below never see a bit that is not signal.
void scaleInt24In32(unsigned char* data, uint32_t samples, double gain, double step,
                    uint16_t channels)
{
    uint16_t channel = 0;
    for (uint32_t i = 0; i < samples; ++i)
    {
        int32_t raw = 0;
        std::memcpy(&raw, data, 4);

#if defined(_WIN32) || defined(ESP_PLATFORM)
        // Valid bits 31..8. The 24-bit value is scaled and shifted back rather
        // than the container scaled and masked: masking the product would
        // floor every sample toward negative infinity by up to one LSB, which
        // is a DC offset on quiet material, and it would leave the low byte
        // holding whatever the multiply produced instead of the zero padding
        // WASAPI or I2S was handed.
        //
        // The shift down is unsigned and the sign is put back by hand. `raw >>
        // 8` would be implementation-defined for a negative sample in C++17 --
        // every compiler this builds on shifts arithmetically, but the value
        // here is not masked afterwards, so the fill bits would land in the
        // result rather than being discarded. The same idiom as the branch
        // below and as scaleInt24.
        int32_t sample = static_cast<int32_t>(static_cast<uint32_t>(raw) >> 8);
        if ((sample & 0x800000) != 0)
            sample |= static_cast<int32_t>(0xFF000000u);
        const uint32_t stored = static_cast<uint32_t>(scaled(sample, gain)) << 8;
#else
        // Valid bits 23..0. The top byte is padding that arrived straight off
        // the wire, so it is rebuilt here rather than trusted: a stray 0xFF in
        // it turns a quiet sample into a very loud one.
        int32_t sample = raw & 0x00FFFFFF;
        if ((sample & 0x800000) != 0)
            sample |= static_cast<int32_t>(0xFF000000u);

        // Padding goes back as zero rather than as the sign extension the
        // arithmetic produced. S24_LE is three bytes of signal in a four-byte
        // word and the fourth is not the driver's to interpret; the read above
        // masks before it sign-extends, so this round-trips.
        const uint32_t stored = static_cast<uint32_t>(scaled(sample, gain)) & 0x00FFFFFFu;
#endif

        std::memcpy(data, &stored, 4);
        data += 4;

        if (++channel == channels)
        {
            channel = 0;
            gain += step;
        }
    }
}

void scaleFloat32(unsigned char* data, uint32_t samples, double gain, double step,
                  uint16_t channels)
{
    uint16_t channel = 0;
    for (uint32_t i = 0; i < samples; ++i)
    {
        float sample = 0.0f;
        std::memcpy(&sample, data, 4);
        sample = static_cast<float>(static_cast<double>(sample) * gain);
        std::memcpy(data, &sample, 4);
        data += 4;

        if (++channel == channels)
        {
            channel = 0;
            gain += step;
        }
    }
}

} // namespace

VolumeControl::VolumeControl() : sequence_(0), applied_(1000)
{
    storePublished(Target());
}

void VolumeControl::prepare(const AudioFormat& format)
{
    // Before a single field: prepare() writes everything set() does and the
    // audio thread's own state besides, and a reopened device is not a reason
    // for the thread that drives this to change.
    if (!claimProducer())
        return;

    // The device is closed, so everything the audio thread owns is written
    // here without a barrier -- the same reason TimelineSource::prepare can.
    sampleType_ = format.sampleType;
    channels_ = format.channels;
    bytesPerFrame_ = format.isValid() ? format.bytesPerFrame() : 0;

    // Rounded up, so a ramp never becomes a step on a very low rate.
    rampFrames_ = static_cast<uint32_t>(
        (static_cast<uint64_t>(format.sampleRate) * kRampMilliseconds + 999ULL) / 1000ULL);

    scheduled_ = false;
    scheduledFrame_ = 0;

    // The gain the room already had, in place from the first frame: a reopened
    // device is not a reason to come back at full volume, and a ramp up from
    // silence would be one.
    const double effective = muted_ ? 0.0 : clampGain(gain_);
    storePublished(Target());
    sequence_.store(0, std::memory_order_relaxed);
    lastSequence_ = 0;

    hasPending_ = false;
    pending_ = Target();
    current_ = effective;
    target_ = effective;
    step_ = 0.0;
    rampRemaining_ = 0;
    applied_.store(static_cast<uint32_t>(std::lrint(effective * 1000.0)),
                   std::memory_order_relaxed);
}

void VolumeControl::set(double gain, bool muted)
{
    if (!claimProducer())
        return;

    gain_ = clampGain(gain);
    muted_ = muted;
    scheduled_ = false;
    scheduledFrame_ = 0;
    publish(muted_ ? 0.0 : gain_, 0, true);
}

void VolumeControl::setAt(double gain, bool muted, uint64_t deviceFrame)
{
    if (!claimProducer())
        return;

    gain_ = clampGain(gain);
    muted_ = muted;
    scheduled_ = true;
    scheduledFrame_ = deviceFrame;
    publish(muted_ ? 0.0 : gain_, deviceFrame, false);
}

uint32_t VolumeControl::pendingSplit(uint32_t frameCount, uint64_t cursor)
{
    adopt();

    // Where in this period a pending change takes effect, which is what makes
    // a scheduled volume change land on the frame the server named rather than
    // on a period boundary.
    uint32_t split = frameCount;
    if (hasPending_)
    {
        if (pending_.immediate || pending_.deviceFrame <= cursor)
            split = 0;
        else if (pending_.deviceFrame - cursor < frameCount)
            split = static_cast<uint32_t>(pending_.deviceFrame - cursor);
    }
    return split;
}

void VolumeControl::process(void* buffer, uint32_t frameCount, uint64_t cursor)
{
    if (buffer == nullptr || frameCount == 0 || bytesPerFrame_ == 0 || channels_ == 0)
        return;

    unsigned char* data = static_cast<unsigned char*>(buffer);
    const uint32_t split = pendingSplit(frameCount, cursor);

    scale(data, split);
    if (split == frameCount)
        return;

    beginRamp(pending_.gain);
    hasPending_ = false;
    scale(data + static_cast<size_t>(split) * bytesPerFrame_, frameCount - split);
}

void VolumeControl::silence(uint32_t frameCount, uint64_t cursor)
{
    if (frameCount == 0)
        return;

    // The same split as process(), because these frames are on the same
    // device axis and a scheduled change has to land on the frame the server
    // named whether or not that frame happens to be audible. advance() moves
    // the gain without touching a sample: silence times anything is silence.
    const uint32_t split = pendingSplit(frameCount, cursor);

    advance(split);
    if (split == frameCount)
        return;

    beginRamp(pending_.gain);
    hasPending_ = false;
    advance(frameCount - split);
}

double VolumeControl::appliedGain() const
{
    return static_cast<double>(applied_.load(std::memory_order_relaxed)) / 1000.0;
}

void VolumeControl::storePublished(const Target& target)
{
    // The double travels as its bit pattern in two halves rather than as a
    // double, because a 64-bit atomic is not lock-free on ARMv6 and the audio
    // thread may not take the lock one would cost. A torn pair is a bit
    // pattern nobody asked for, which is exactly what the sequence number is
    // there to reject.
    uint64_t bits = 0;
    std::memcpy(&bits, &target.gain, sizeof(bits));

    published_.gainLow.store(static_cast<uint32_t>(bits), std::memory_order_relaxed);
    published_.gainHigh.store(static_cast<uint32_t>(bits >> 32), std::memory_order_relaxed);
    published_.frameLow.store(static_cast<uint32_t>(target.deviceFrame),
                              std::memory_order_relaxed);
    published_.frameHigh.store(static_cast<uint32_t>(target.deviceFrame >> 32),
                               std::memory_order_relaxed);
    published_.immediate.store(target.immediate ? 1u : 0u, std::memory_order_relaxed);
    published_.generation.store(target.generation, std::memory_order_relaxed);
}

VolumeControl::Target VolumeControl::loadPublished() const
{
    const uint64_t bits =
        static_cast<uint64_t>(published_.gainLow.load(std::memory_order_relaxed)) |
        (static_cast<uint64_t>(published_.gainHigh.load(std::memory_order_relaxed)) << 32);

    Target target;
    std::memcpy(&target.gain, &bits, sizeof(bits));
    target.deviceFrame =
        static_cast<uint64_t>(published_.frameLow.load(std::memory_order_relaxed)) |
        (static_cast<uint64_t>(published_.frameHigh.load(std::memory_order_relaxed)) << 32);
    target.immediate = published_.immediate.load(std::memory_order_relaxed) != 0;
    target.generation = published_.generation.load(std::memory_order_relaxed);
    return target;
}

bool VolumeControl::claimProducer()
{
    // The first thread to reach set(), setAt() or prepare() becomes this
    // object's producer for its lifetime; every other thread is turned away
    // before it writes anything. Refusing rather than logging and carrying on
    // is the point: gain_, muted_, scheduled_ and generation_ are plain
    // fields, and the seqlock has no interlock at all -- publish() makes the
    // counter odd and even again, so a second writer can leave it even
    // mid-write and the render callback will take a torn target for a good
    // one. A dropped volume command is recoverable. A torn one is not.
    //
    // The claim is a compare-exchange so that the check is not itself the race
    // it exists to find: exactly one thread can win the 0 -> tag transition.
    // Relaxed throughout, because this orders nothing but itself, and it never
    // runs on the render path.
    const uint32_t tag = currentThreadTag();
    uint32_t expected = 0u;
    if (producer_.compare_exchange_strong(expected, tag, std::memory_order_relaxed,
                                          std::memory_order_relaxed))
        return true; // first caller: this thread is the producer now
    if (expected == tag)
        return true;

    // Reported once. The client is misconfigured rather than misbehaving, so
    // one line says it; a dragged volume slider would otherwise put a line in
    // the log per message. Logging is a control-path facility and this is the
    // control path.
    if (producerReported_.exchange(1u, std::memory_order_relaxed) == 0u)
    {
        MULTIROOM_ERROR("volume control refused a change from a second thread: the render "
                  "path's seqlock is single-producer, so set(), setAt() and "
                  "prepare() belong to one thread and this one is not it");
    }
    return false;
}

void VolumeControl::publish(double gain, uint64_t deviceFrame, bool immediate)
{
    Target target;
    target.gain = gain;
    target.deviceFrame = deviceFrame;
    target.immediate = immediate;
    target.generation = ++generation_;

    const uint32_t sequence = sequence_.load(std::memory_order_relaxed);
    sequence_.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    storePublished(target);
    sequence_.store(sequence + 2, std::memory_order_release);
}

void VolumeControl::adopt()
{
    const uint32_t before = sequence_.load(std::memory_order_acquire);
    if (before == lastSequence_)
        return; // nothing new since the last callback
    if ((before & 1u) != 0)
        return; // a write is in progress; the next callback will see it

    const Target target = loadPublished();
    // Relaxed is enough for the re-read: the acquire fence keeps the field
    // loads above from moving below it. This is the kernel's seqcount read.
    std::atomic_thread_fence(std::memory_order_acquire);
    if (sequence_.load(std::memory_order_relaxed) != before)
        return; // it changed while it was being read

    lastSequence_ = before;
    if (hasPending_ && target.generation == pending_.generation)
        return;

    // The mailbox: a change that arrives while an earlier one is still waiting
    // replaces it rather than queueing behind it. SET_VOLUME carries an
    // absolute gain, so the newest message is the state the server wants and
    // the ones it overtook were only ever on the way there. The case this
    // gives up -- two changes armed for two different future frames inside one
    // adoption window -- is written out in the header.
    pending_ = target;
    hasPending_ = true;
}

void VolumeControl::advance(uint32_t frames)
{
    if (frames == 0)
        return;

    // scale() without the sample loops. A ramp is a number of frames on the
    // device's own axis, not a number of frames that happened to carry audio,
    // so silence consumes it at exactly the rate music would: a room paused
    // 5 ms into a 15 ms ramp comes back 5 ms into it, and one silent longer
    // than the ramp comes back at the target rather than still sliding.
    const uint32_t ramp = rampRemaining_ < frames ? rampRemaining_ : frames;
    if (ramp != 0)
    {
        current_ += step_ * static_cast<double>(ramp);
        rampRemaining_ -= ramp;
        if (rampRemaining_ == 0)
            current_ = target_; // the accumulated step is not the target
    }

    applied_.store(static_cast<uint32_t>(std::lrint(current_ * 1000.0)),
                   std::memory_order_relaxed);
}

void VolumeControl::beginRamp(double target)
{
    target_ = target;
    if (rampFrames_ == 0 || current_ == target_)
    {
        current_ = target_;
        step_ = 0.0;
        rampRemaining_ = 0;
        return;
    }

    step_ = (target_ - current_) / static_cast<double>(rampFrames_);
    rampRemaining_ = rampFrames_;
}

void VolumeControl::fade(void* buffer, uint32_t frameCount, double gain, double step) const
{
    if (buffer == nullptr || frameCount == 0)
        return;

    unsigned char* const data = static_cast<unsigned char*>(buffer);
    const uint32_t samples = frameCount * channels_;
    switch (sampleType_)
    {
    case AudioFormat::SampleType::Int16:
        scaleInt16(data, samples, gain, step, channels_);
        break;
    case AudioFormat::SampleType::Int24:
        scaleInt24(data, samples, gain, step, channels_);
        break;
    case AudioFormat::SampleType::Int24In32:
        scaleInt24In32(data, samples, gain, step, channels_);
        break;
    case AudioFormat::SampleType::Int32:
        scaleInt32(data, samples, gain, step, channels_);
        break;
    case AudioFormat::SampleType::Float32:
        scaleFloat32(data, samples, gain, step, channels_);
        break;
    }
}

void VolumeControl::scale(unsigned char* data, uint32_t frames)
{
    if (frames == 0)
        return;

    // The ramp and the steady part are two loops rather than one with a test
    // in it: the sample loops carry the gain multiply and nothing else.
    const uint32_t ramp = rampRemaining_ < frames ? rampRemaining_ : frames;
    if (ramp != 0)
    {
        fade(data, ramp, current_, step_);

        current_ += step_ * static_cast<double>(ramp);
        rampRemaining_ -= ramp;
        if (rampRemaining_ == 0)
            current_ = target_; // the accumulated step is not the target
        data += static_cast<size_t>(ramp) * bytesPerFrame_;
        frames -= ramp;
    }

    if (frames != 0 && current_ != 1.0)
        fade(data, frames, current_, 0.0);

    applied_.store(static_cast<uint32_t>(std::lrint(current_ * 1000.0)),
                   std::memory_order_relaxed);
}

} // namespace multiroom
