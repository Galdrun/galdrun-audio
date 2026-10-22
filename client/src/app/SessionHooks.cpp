#include "app/SessionHooks.h"

#include "app/DeviceSelection.h"
#include "app/InputSettings.h"

namespace multiroom {

void connectSessionHooks(matp::Session& session, ClientRuntime& runtime)
{
    session.setSystemProvider([&runtime](SystemMetrics& out) {
        out = runtime.system;
        return runtime.hasSystem;
    });
    // SET_AUDIO_DEVICE takes the control API's path, persistence included.
    session.setAudioDeviceHooks(
        [&runtime](matp::AudioDeviceList& out) { currentAudioDevices(runtime, out); },
        [&runtime](const std::string& deviceId, std::string& error) {
            const AudioDeviceChange result = changeAudioDevice(runtime, deviceId, error);
            return result == AudioDeviceChange::Changed || result == AudioDeviceChange::Unchanged;
        });
    session.setAudioRescanHook(runtime.rescanAudio);
    // SET_INPUT_BINDINGS takes the control API's path too.
    session.setInputHooks(
        [&runtime](std::vector<matp::InputControl>& out) { currentInputControls(runtime, out); },
        [&runtime](const matp::SetInputBindings& request, std::string& error) {
            const InputBindingChange result =
                changeInputBindings(runtime, request.inputId, request.bindings, error);
            return result == InputBindingChange::Changed ||
                   result == InputBindingChange::Unchanged;
        },
        deepSleepSupported());
    // SET_BUFFER: validated, persisted and applied here; refused, not clamped.
    session.setBufferHooks([&runtime]() { return runtime.config->bufferMs; },
                           [&runtime](uint32_t bufferMs, std::string& error) {
                               const BufferChange result = changeBuffer(runtime, bufferMs, error);
                               return result == BufferChange::Changed ||
                                      result == BufferChange::Unchanged;
                           });
    // SET_ROOM_NAME takes the control API's path (section 60).
    session.setRoomNameHooks([&runtime]() { return runtime.config->name; },
                             [&runtime](const std::string& name, std::string& error) {
                                 const NameChange result = changeName(runtime, name, error);
                                 return result == NameChange::Changed ||
                                        result == NameChange::Unchanged;
                             });
    session.setVolumeControlHooks(
        [&runtime](matp::AudioControl control, bool known) {
            applyAudioControl(runtime, control, known);
        },
        [&runtime]() {
            matp::VolumeConfig report;
            report.volumeControl = runtime.config->volumeControl;
            report.effective = runtime.volumePath.effective;
            report.reason = runtime.volumePath.reason;
            return report;
        },
        [&runtime](matp::VolumeControlMode mode, std::string& error) {
            const VolumeControlChange result = changeVolumeControl(runtime, mode, error);
            return result == VolumeControlChange::Changed ||
                   result == VolumeControlChange::Unchanged;
        });
    // The server decides the correction; the stream only executes it.
    session.setDriftCorrectionHooks(
        [&runtime](matp::DriftCorrectionMode selected) {
            if (runtime.playback != nullptr)
                runtime.playback->setDriftCorrection(selected);
        },
        [&runtime](const matp::DriftFeedback& feedback) {
            if (runtime.playback != nullptr)
                runtime.playback->driftFeedback(feedback);
        },
        [&runtime](matp::DriftCorrectionMode mode, std::string& error) {
            const DriftCorrectionChange result = changeDriftCorrection(runtime, mode, error);
            return result == DriftCorrectionChange::Changed ||
                   result == DriftCorrectionChange::Unchanged;
        });
}

void connectStreamHooks(matp::Session& session, StreamPlayback& playback)
{
    matp::Session::StreamHooks hooks;
    hooks.prepare = [&playback](const matp::PrepareStream& request,
                                const AudioFormat& negotiated, matp::StreamReady& ready) {
        playback.prepare(request, negotiated, ready);
    };
    hooks.release = [&playback]() { playback.release(); };
    hooks.start = [&playback](const matp::StartAt& request, uint64_t localStartNs,
                              std::string& error) {
        return playback.start(request, localStartNs, error);
    };
    hooks.bufferStatus = [&playback](matp::BufferStatus& status) {
        return playback.bufferStatus(status);
    };
    hooks.playbackReport = [&playback](matp::PlaybackReport& report) {
        return playback.playbackReport(report);
    };
    hooks.pause = [&playback](const matp::TransportAt& request, std::string& error) {
        return playback.pause(request, error);
    };
    hooks.stop = [&playback](const matp::TransportAt& request, std::string& error) {
        return playback.stop(request, error);
    };
    hooks.flush = [&playback](const matp::TransportAt& request, std::string& error) {
        return playback.flush(request, error);
    };
    hooks.volume = [&playback](const matp::SetVolume& request, std::string& error) {
        return playback.volume(request, error);
    };
    hooks.trackBoundary = [&playback](const matp::TrackBoundary& boundary) {
        playback.trackBoundary(boundary);
    };
    hooks.trackInfo = [&playback](const matp::TrackInfo& info) {
        playback.trackInfo(info);
    };
    session.setStreamHooks(hooks);
}

} // namespace multiroom
