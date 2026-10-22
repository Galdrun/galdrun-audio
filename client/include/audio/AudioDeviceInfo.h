#pragma once

#include "audio/AudioFormat.h"

#include <cstdint>
#include <string>
#include <vector>

namespace multiroom {

// How the device is opened.
//
// Windows: WASAPI shared mode goes through the system mixer, which resamples
// and format-converts; exclusive mode owns the endpoint and is bit-perfect.
// Linux: "plughw:" converts in software the same way, "hw:" does not.
//
// The distinction matters because the two modes accept genuinely different
// format lists, and because only the direct path gives a hardware clock that
// is worth regressing for drift.
enum class AudioAccessMode
{
    Shared,
    Exclusive
};

const char* audioAccessModeName(AudioAccessMode mode);

// What the user wants us to do about the two modes.
//
// The default prefers exclusive access: it is bit-perfect, and it puts nothing
// between us and the sound card's own clock. Shared mode routes through the
// system mixer, which resamples to its own rate -- the clock we would then
// measure is the mixer's, not the hardware's, which quietly degrades the drift
// figure the server relies on.
//
// ExclusiveOnly exists for exactly that reason: on a dedicated multiroom node,
// failing loudly beats playing through the mixer without anyone noticing.
enum class AudioAccessPreference
{
    PreferExclusive, // exclusive where offered, otherwise shared
    ExclusiveOnly,   // never fall back to shared
    SharedOnly       // always through the system mixer
};

const char* audioAccessPreferenceName(AudioAccessPreference preference);
bool parseAudioAccessPreference(const std::string& text, AudioAccessPreference& out);

// Buffer and period limits reported by the driver. Zero means "not reported".
//
// Periods are kept in nanoseconds because WASAPI reports them as a time span
// (REFERENCE_TIME) while ALSA reports them in frames; both are recorded so the
// engine can pick a buffer size without re-querying the driver.
struct AudioBufferLimits
{
    uint64_t minPeriodNs = 0;     // shortest period the hardware accepts
    uint64_t defaultPeriodNs = 0; // driver's own preferred period

    uint32_t minPeriodFrames = 0;
    uint32_t maxPeriodFrames = 0;
    uint32_t minBufferFrames = 0;
    uint32_t maxBufferFrames = 0;
};

// A volume control on the device itself, found by the capability scan. When a
// device has one, the room's volume is set there and the PCM stream goes to
// the device unscaled; without one, VolumeControl scales it in the render
// path. See audio/HardwareVolume.h.
struct HardwareVolumeInfo
{
    bool available = false;

    // The platform's name for the control. Linux: the ALSA simple mixer
    // element ("Master", "PCM", "Digital", ...).
    std::string control;

    // The control's range in decibels, when the driver reports one. Without
    // it the room's gain is mapped linearly onto the raw steps.
    bool hasDecibels = false;
    double minDb = 0.0;
    double maxDb = 0.0;

    // The raw step range, always reported by a control that exists.
    int64_t minStep = 0;
    int64_t maxStep = 0;

    // A mute switch of its own; without one, a mute is the lowest step.
    bool hasMute = false;
};

// A clock trim on the device itself, which the server may drive instead of
// resampling. Only where the board file names the control: see
// audio/HardwareClock.h.
struct HardwareClockInfo
{
    bool available = false;
    std::string control; // Linux: the ALSA control element ("Drift compensator")
    int32_t minPpm = 0;
    int32_t maxPpm = 0;
    uint32_t stepPpm = 1;
};

struct AudioDeviceInfo
{
    // Stable platform identifier used to reopen the device across restarts.
    // Windows: the IMMDevice endpoint id. Linux: "hw:CARD=<id>,DEV=<n>", which
    // survives card renumbering, or the literal "default".
    std::string id;

    std::string name;        // friendly name shown to the user
    std::string description; // ALSA long name / WASAPI device description

    bool isDefault = false;

    // Shared-mode mix format on Windows, the driver's preferred format on
    // Linux. Only meaningful when hasMixFormat is true.
    bool hasMixFormat = false;
    AudioFormat mixFormat;

    // Only combinations the driver actually accepted during the capability
    // scan, not the full probe cross-product. Both lists are reported to the
    // server, which picks this client's stream format from them.
    std::vector<AudioFormat> sharedFormats;
    std::vector<AudioFormat> exclusiveFormats;

    AudioBufferLimits buffer;

    HardwareVolumeInfo hardwareVolume;

    HardwareClockInfo hardwareClock;

    // Transient ALSA scan results, not persisted or sent to the server. A busy
    // probe has not established that this mode lost its supported formats.
    bool sharedProbeBusy = false;
    bool exclusiveProbeBusy = false;

    const std::vector<AudioFormat>& formats(AudioAccessMode mode) const;
    std::vector<AudioFormat>& formats(AudioAccessMode mode);

    bool supports(const AudioFormat& format, AudioAccessMode mode) const;
    bool supportsAnyMode(const AudioFormat& format) const;

    size_t formatCount() const { return sharedFormats.size() + exclusiveFormats.size(); }

    // Best format at the given rate within the allowed access modes, taking the
    // widest sample type at the requested channel count. Returns false if the
    // rate is not supported in any mode the preference permits.
    bool bestFormatForRate(uint32_t sampleRate, uint16_t preferredChannels,
                           AudioAccessPreference preference, AudioFormat& out,
                           AudioAccessMode& outMode) const;
};

// Picks the format the client will actually open, trying the preferred rate
// first, then 48000, then 44100, then the highest rate the device offers.
// Returns false if the device reported no usable format under this preference
// -- which is the intended outcome for ExclusiveOnly on a device that has no
// exclusive formats at all.
bool chooseOutputFormat(const AudioDeviceInfo& device, uint32_t preferredSampleRate,
                        uint16_t preferredChannels, AudioAccessPreference preference,
                        AudioFormat& out, AudioAccessMode& outMode);

} // namespace multiroom
