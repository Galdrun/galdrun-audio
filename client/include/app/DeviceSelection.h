#pragma once

// Choosing the device and the format the client plays on, and reporting both.
//
// The client re-reads its selection rather than capturing it once: a rescan --
// on the command line, from the control API, or during playback recovery --
// may land on a different device or format, and the next open attempt and the
// next capability report both have to use that one. Everything here therefore
// takes ClientRuntime and works from what is in it now.
//
// All of it runs on the thread that owns the client state; none of it is
// reachable from the audio thread.

#include "app/ClientCommandHandler.h"
#include "audio/AudioDeviceCache.h"
#include "audio/AudioEngine.h"
#include "matp/Protocol.h"

#include <string>

namespace multiroom {

// Runs the capability scan and writes the cache. ALSA modes whose probe is
// busy retain their previous formats; removed devices and successful probes
// use the new scan. Returns false only when no
// backend exists for this platform; an empty result is a valid outcome on a
// machine without playback hardware.
bool rescanAudioDevices(const std::string& cachePath, const I2sSettings& i2s,
                        AudioDeviceCacheData& cache, std::string& error);

// One log block per cached device, with its formats at debug level.
void logDeviceSummary(const AudioDeviceCacheData& cache);

// Picks the device and the format the client will play, and records both in
// the runtime, so the control API reports exactly what the engine would open.
// Re-run after every rescan.
void applyDeviceSelection(ClientRuntime& runtime);

// What the engine should open, from the current selection.
bool currentPlaybackTarget(const ClientRuntime& runtime, AudioEngine::Settings& settings,
                           std::string& error);

// What the client tells the server it can play (MATP section 12), from the
// same selection.
bool currentCapabilities(const ClientRuntime& runtime, matp::CapabilityReport& report,
                         std::string& error);

// Every cached device, the configured one and the selected one (MATP section
// 56).
void currentAudioDevices(const ClientRuntime& runtime, matp::AudioDeviceList& out);

enum class AudioDeviceChange
{
    Changed,
    Unchanged,
    UnknownDevice, // not in the cache; nothing was changed
    NotSaved       // client.json could not be written; nothing was changed
};

// The one path a device change takes, from the control API and from the
// server alike: `deviceId` (empty for the system default) is written to
// client.json, the selection is re-run, and a live session reports the new
// list and -- when the device it plays on changed -- its capabilities, which
// is what has the server re-negotiate and the device reopen (section 12).
AudioDeviceChange changeAudioDevice(ClientRuntime& runtime, const std::string& deviceId,
                                    std::string& error);

enum class BufferChange
{
    Changed,
    Unchanged,
    Refused, // out of range, or no room for it; nothing was changed
    NotSaved // client.json could not be written; nothing was changed
};

// The one path a change of the buffer's size takes (MATP section 58).
// Refused, never clamped: a value outside the configured range, one that would
// leave the selected device no format whose ring buffer fits, or one that does
// not fit the format the stream is playing. Accepted, it is written to
// client.json, applied to the stream at once, and a live session reports it
// and -- when it changed which formats fit -- the capabilities.
BufferChange changeBuffer(ClientRuntime& runtime, uint32_t bufferMs, std::string& error);

enum class NameChange
{
    Changed,
    Unchanged,
    Refused, // not a name by MATP section 60's rule; nothing was changed
    NotSaved // client.json could not be written; nothing was changed
};

// The one path a change of the room's name takes (MATP section 60), from the
// control API and from SET_ROOM_NAME alike. Accepted, it is written to
// client.json and a live session reports it. Nothing else changes: the name
// is a label, the client id the identity.
NameChange changeName(ClientRuntime& runtime, const std::string& name, std::string& error);

enum class VolumeControlChange
{
    Changed,
    Unchanged,
    Refused, // software while the room's Audio Control is bitPerfect
    NotSaved // client.json could not be written; nothing was changed
};

// The one path a change of the Volume Control takes (MATP section 59).
// Software is refused outright while the room's Audio Control is bitPerfect,
// never accepted and quietly not played. Accepted, it is written to
// client.json and the volume path resolved again; a live session reports it.
VolumeControlChange changeVolumeControl(ClientRuntime& runtime, matp::VolumeControlMode mode,
                                        std::string& error);

enum class DriftCorrectionChange
{
    Changed,
    Unchanged,
    Refused, // hardwareClock on a device without a verified clock trim
    NotSaved // client.json could not be written; nothing was changed
};

// The one path a change of the drift-correction actuator takes, from the
// control API and from SET_DRIFT_CORRECTION alike. Accepted, it is written to
// client.json and a live session reports its capabilities again, which the
// server renegotiates from: the actuator only changes with a reopened stream.
DriftCorrectionChange changeDriftCorrection(ClientRuntime& runtime,
                                            matp::DriftCorrectionMode mode, std::string& error);

// SESSION_CONFIG's Audio Control (section 59), or BitPerfect where it named
// none. A room that becomes bitPerfect while its Volume Control is software
// has that reset to auto -- saved, logged and reported, never left to be
// resolved to "disabled" without a word.
void applyAudioControl(ClientRuntime& runtime, matp::AudioControl control, bool known);

} // namespace multiroom
