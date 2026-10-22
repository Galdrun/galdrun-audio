#pragma once

#include <cstdint>
#include <string>

namespace multiroom {

// How the timestamp in a snapshot was obtained. The drift estimator weights
// samples by this, and the control API reports it so a bad driver is visible
// rather than silently degrading sync quality.
//
// One meaning on every platform, so rooms on different systems compare:
//   Hardware  the audio hardware timed the position itself, at the codec link
//             (ALSA link audio_htstamp). Nothing else qualifies.
//   Driver    the driver read the card's position and a system timestamp
//             together (ALSA driver_htstamp, exclusive WASAPI IAudioClock with
//             its QPC, the ESP32 DMA interrupt).
//   Estimated the position is not the card's own, or its time was not taken
//             with it (ALSA status htstamp, shared WASAPI, a stale position).
enum class AudioClockQuality
{
    Unavailable, // no usable position at all
    Estimated,
    Driver,
    Hardware
};

const char* audioClockQualityName(AudioClockQuality quality);

// The reverse, for the server reading PLAYBACK_REPORT. False for a name this
// build does not know, which the caller reports rather than collapsing to
// Unavailable: "no usable position" and "a word I have never heard" are
// different statements about a room, and only one of them is the room's fault.
bool parseAudioClockQuality(const std::string& text, AudioClockQuality& out);

// One observation of the sound card's own clock: how many frames the hardware
// has actually consumed, and when that was true on our monotonic time base.
//
// deviceFramePosition must come from the driver, never from a count of frames
// we handed to the API -- the latter measures our own buffering, not the
// hardware rate, and would make drift unmeasurable.
struct AudioClockSnapshot
{
    uint64_t deviceFramePosition = 0;
    uint64_t monotonicTimeNs = 0;
    uint32_t nominalSampleRate = 0;
    AudioClockQuality quality = AudioClockQuality::Unavailable;

    bool isValid() const
    {
        return quality != AudioClockQuality::Unavailable && nominalSampleRate > 0 &&
               monotonicTimeNs > 0;
    }
};

} // namespace multiroom
