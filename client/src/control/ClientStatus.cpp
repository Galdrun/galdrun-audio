#include "control/ClientStatus.h"

#include "audio/AudioDeviceCache.h"

namespace multiroom {

json::Value toJson(const ClientStatus& status)
{
    json::Value root = json::Value::makeObject();
    root["clientId"] = json::Value(status.clientId);
    if (!status.name.empty())
        root["name"] = json::Value(status.name);
    root["version"] = json::Value(status.version);
    root["platform"] = json::Value(status.platform);
    root["uptimeSeconds"] = json::Value(status.uptimeSeconds);
    if (!status.powerState.empty())
    {
        root["power"] = json::Value::makeObject();
        root["power"]["state"] = json::Value(status.powerState);
        root["power"]["standbyAfterSeconds"] = json::Value(status.standbyAfterSeconds);
        root["power"]["deepSleepAfterSeconds"] = json::Value(status.deepSleepAfterSeconds);
    }

    json::Value server = json::Value::makeObject();
    server["address"] = json::Value(status.server);
    server["state"] = json::Value(serverConnectionStateName(status.serverState));
    root["server"] = server;

    json::Value session = json::Value::makeObject();
    session["state"] = json::Value(matp::sessionStateName(status.sessionState));
    if (!status.sessionId.empty())
        session["sessionId"] = json::Value(status.sessionId);
    if (status.sessionFormatSelected)
    {
        // The format the server chose, in the client's own spelling: the wire
        // names of MATP section 12 belong on the wire, not in a local API.
        session["format"] = device_cache::formatToJson(status.sessionFormat);
        session["formatPlayable"] = json::Value(status.sessionFormatPlayable);
    }
    if (!status.sessionError.empty())
        session["error"] = json::Value(status.sessionError);
    root["session"] = session;

    // Separate from the "clock" block below, which is the sound card. This one
    // is this machine's CPU clock against the server's; mixing the two is the
    // one mistake the whole synchronization model cannot survive.
    if (status.hasClockSync)
    {
        json::Value clockSync = json::Value::makeObject();
        clockSync["offsetNs"] = json::Value(status.clockOffsetNs);
        if (status.hasClockSkew)
            clockSync["skewPpm"] = json::Value(status.clockSkewPpm);
        clockSync["roundTripNs"] = json::Value(status.clockRoundTripNs);
        clockSync["lastRoundTripNs"] = json::Value(status.clockLastRoundTripNs);
        clockSync["uncertaintyNs"] = json::Value(status.clockUncertaintyNs);
        clockSync["samples"] = json::Value(status.clockSampleCount);
        clockSync["windowSeconds"] = json::Value(status.clockWindowSeconds);
        clockSync["requests"] = json::Value(status.clockSyncRequests);
        clockSync["replies"] = json::Value(status.clockSyncReplies);
        clockSync["results"] = json::Value(status.clockSyncResults);
        root["clockSync"] = clockSync;
    }

    // The audio connection, which is a different connection from the one in
    // the "server" block above and can be down while that one is up.
    if (status.streamPrepared)
    {
        json::Value stream = json::Value::makeObject();
        stream["audioServer"] = json::Value(status.audioServer);
        stream["audioState"] = json::Value(serverConnectionStateName(status.audioServerState));
        stream["packets"] = json::Value(status.streamPackets);
        stream["frames"] = json::Value(status.streamFrames);
        stream["bufferedDurationNs"] = json::Value(status.streamBufferedDurationNs);
        stream["bufferedFrames"] = json::Value(status.streamBufferedFrames);
        stream["bufferedMs"] = json::Value(status.streamBufferedMs);
        stream["capacityFrames"] = json::Value(status.streamCapacityFrames);
        stream["capacityMs"] = json::Value(status.streamCapacityMs);
        stream["bufferFullWaits"] = json::Value(status.streamWaits);
        stream["reattachments"] = json::Value(status.streamReattachments);
        stream["rejoins"] = json::Value(status.streamRejoins);
        stream["flushes"] = json::Value(status.streamFlushes);
        if (status.streamHasContent)
        {
            stream["playheadTimelineNs"] = json::Value(status.streamPlayheadTimelineNs);
            stream["bufferedUntilTimelineNs"] =
                json::Value(status.streamBufferedUntilTimelineNs);
        }
        // Where this room is on the zone's media timeline. "timelineState" is
        // always there because "idle" is a real answer; the positions are
        // only there once they exist.
        stream["timelineState"] = json::Value(timelineStateName(status.timelineState));
        stream["underrunFrames"] = json::Value(status.timelineUnderrunFrames);
        stream["skippedFrames"] = json::Value(status.timelineSkippedFrames);
        stream["flushedFrames"] = json::Value(status.timelineFlushedFrames);
        if (status.startScheduled)
        {
            stream["startTimelineNs"] = json::Value(status.startTimelineNs);
            stream["startDeviceFrame"] = json::Value(status.startDeviceFrame);
            stream["startInMs"] = json::Value(status.startInMs);
        }
        if (status.hasTimelineNsAudible)
            stream["timelineNsAudible"] = json::Value(status.timelineNsAudible);
        if (status.endScheduled)
            stream["endTimelineNs"] = json::Value(status.endTimelineNs);

        // The volume is always there: 1.0 and unmuted is a real answer, and a
        // caller that has to guess it from the absence of a key cannot tell a
        // room at full volume from a client that does not do volume at all.
        json::Value volume = json::Value::makeObject();
        volume["gain"] = json::Value(status.volumeGain);
        volume["muted"] = json::Value(status.volumeMuted);
        volume["scheduled"] = json::Value(status.volumeScheduled);
        volume["appliedGain"] = json::Value(status.appliedGain);
        stream["volume"] = volume;

        // What is playing, once the sound card has reached a boundary or the
        // server has named the track outright. Before that the timeline is
        // real and the track name is not.
        stream["pendingTrackBoundaries"] = json::Value(status.pendingTrackBoundaries);
        if (status.trackKnown)
        {
            json::Value track = json::Value::makeObject();
            if (!status.trackId.empty())
                track["trackId"] = json::Value(status.trackId);
            if (!status.trackTitle.empty())
                track["title"] = json::Value(status.trackTitle);
            // Left out for a track a TRACK_INFO named: it carries no position,
            // and 0 ns is a real one.
            if (status.hasTrackTimelineNs)
                track["timelineNs"] = json::Value(status.trackTimelineNs);
            stream["track"] = track;
        }
        if (!status.streamError.empty())
            stream["error"] = json::Value(status.streamError);
        root["stream"] = stream;
    }

    json::Value audio = json::Value::makeObject();
    audio["deviceCount"] = json::Value(status.deviceCount);
    audio["scannedAt"] = json::Value(status.audioScannedAt);
    audio["deviceSelected"] = json::Value(status.deviceSelected);
    if (status.deviceSelected)
    {
        audio["deviceId"] = json::Value(status.deviceId);
        audio["deviceName"] = json::Value(status.deviceName);
    }
    audio["formatSelected"] = json::Value(status.formatSelected);
    if (status.formatSelected)
        audio["accessMode"] = json::Value(audioAccessModeName(status.accessMode));
    // What the sound card runs at, only while it is open: the format chosen at
    // device selection is just where an open starts, not what plays.
    if (status.deviceFormat.isValid())
        audio["format"] = device_cache::formatToJson(status.deviceFormat);
    // Section 59. The Audio Control is the server's and absent until it has
    // said; the Volume Control is this client's own, with the path it resolved
    // to and, where that is not what was asked for, why.
    if (status.audioControlKnown)
        audio["audioControl"] = json::Value(matp::audioControlName(status.audioControl));
    json::Value volumeControl = json::Value::makeObject();
    volumeControl["configured"] = json::Value(matp::volumeControlModeName(status.volumeControl));
    volumeControl["effective"] =
        json::Value(matp::volumeControlModeName(status.volumePath.effective));
    if (!status.volumePath.reason.empty())
        volumeControl["reason"] = json::Value(status.volumePath.reason);
    audio["volumeControl"] = volumeControl;

    // The actuator asked for, the one in use and, where they differ, why (auto
    // asks for the hardware clock too). The hardware clock is reported only
    // for a device that has one.
    const DriftCorrectionState& correction = status.driftCorrectionState;
    json::Value driftCorrection = json::Value::makeObject();
    driftCorrection["configured"] =
        json::Value(matp::driftCorrectionModeName(status.driftCorrection));
    driftCorrection["effective"] = json::Value(matp::driftCorrectionModeName(
        correction.hardwareClockActive ? matp::DriftCorrectionMode::HardwareClock
                                  : matp::DriftCorrectionMode::Resampling));
    if (status.driftCorrection != matp::DriftCorrectionMode::Resampling &&
        !correction.hardwareClockActive)
    {
        driftCorrection["reason"] = json::Value(
            !status.hardwareClock.available ? "the selected device has no verified hardware clock"
            : !correction.hardwareClockError.empty() ? correction.hardwareClockError.c_str()
            : correction.selected != matp::DriftCorrectionMode::HardwareClock
                ? "the server has not selected the hardware clock"
                : "no stream is playing");
    }
    if (status.hardwareClock.available)
    {
        json::Value clock = json::Value::makeObject();
        clock["control"] = json::Value(status.hardwareClock.control);
        clock["minPpm"] = json::Value(status.hardwareClock.minPpm);
        clock["maxPpm"] = json::Value(status.hardwareClock.maxPpm);
        clock["active"] = json::Value(correction.hardwareClockActive);
        clock["appliedPpm"] = json::Value(correction.hardwareClockPpm);
        clock["saturated"] = json::Value(correction.hardwareClockSaturated);
        if (!correction.hardwareClockError.empty())
            clock["error"] = json::Value(correction.hardwareClockError);
        driftCorrection["hardwareClock"] = clock;
    }
    if (correction.hasServerDrift)
    {
        driftCorrection["serverDriftPpm"] = json::Value(correction.serverDriftPpm);
        driftCorrection["serverDriftConfidence"] = json::Value(correction.serverDriftConfidence);
    }
    audio["driftCorrection"] = driftCorrection;

    if (status.deviceSelected)
    {
        // The cache's own description of it, plus whether it is in use and
        // what it reads back as while it is.
        json::Value hardwareVolume = device_cache::hardwareVolumeToJson(status.hardwareVolume);
        hardwareVolume["active"] = json::Value(status.hardwareVolumeActive);
        if (status.hasHardwareVolumeState)
        {
            json::Value current = json::Value::makeObject();
            current["gain"] = json::Value(status.hardwareVolumeState.gain);
            current["muted"] = json::Value(status.hardwareVolumeState.muted);
            if (status.hardwareVolumeState.hasDecibels)
                current["db"] = json::Value(status.hardwareVolumeState.db);
            hardwareVolume["current"] = current;
        }
        audio["hardwareVolume"] = hardwareVolume;
    }
    audio["playing"] = json::Value(status.playbackState == PlaybackState::Playing);
    audio["playbackState"] = json::Value(playbackStateName(status.playbackState));
    audio["underruns"] = json::Value(status.underruns);
    if (status.bufferFrames != 0)
        audio["bufferFrames"] = json::Value(status.bufferFrames);
    audio["recoveries"] = json::Value(status.playbackRecoveries);

    // Only while something is actually wrong: a caller must be able to read
    // "no error key" as "the device is fine" rather than having to compare the
    // message against the last one it saw.
    if (!status.playbackError.empty())
    {
        audio["error"] = json::Value(status.playbackError);
        audio["failedAttempts"] = json::Value(status.playbackAttempts);
        audio["retryInMs"] = json::Value(status.playbackRetryMs);
    }
    root["audio"] = audio;

    // Absent rather than zero: a caller must be able to tell "no clock" from
    // "a clock that happens to read zero".
    if (status.clock.quality != AudioClockQuality::Unavailable)
    {
        json::Value clock = json::Value::makeObject();
        clock["quality"] = json::Value(audioClockQualityName(status.clock.quality));
        clock["deviceFramePosition"] = json::Value(status.clock.deviceFramePosition);
        clock["monotonicTimeNs"] = json::Value(status.clock.monotonicTimeNs);
        clock["nominalSampleRate"] = json::Value(status.clock.nominalSampleRate);
        root["clock"] = clock;
    }

    if (status.hasDrift)
    {
        json::Value drift = json::Value::makeObject();
        drift["measuredSampleRate"] = json::Value(status.measuredSampleRate);
        drift["driftPpm"] = json::Value(status.driftPpm);
        drift["standardErrorPpm"] = json::Value(status.driftStandardErrorPpm);
        drift["confidence"] = json::Value(status.driftConfidence);
        drift["sampleCount"] = json::Value(status.driftSampleCount);
        drift["windowSeconds"] = json::Value(status.driftWindowSeconds);
        root["drift"] = drift;
    }

    if (status.hasSystem)
        root["system"] = matp::makeSystemMetrics(status.system);

    return root;
}

} // namespace multiroom
