# Client implementation map

Paths below are relative to `client/` unless stated otherwise. Build/test
commands: [development.md](development.md). Wire behavior:
[MATP](../protocol/MATP-AudioTransferProtocol.md).

## Entry points and flow

- `src/main.cpp`: desktop `main()` and ESP32 `clientMain()`. Loads config,
  persists identity before applying temporary CLI overrides, selects a device
  from the capability cache, creates control/session/playback services, then
  polls them and drains commands on the owner thread.
- `esp32/main/app_main.cpp`: mounts storage, brings up networking, and calls
  `clientMain()`. Wi-Fi/captive portal UI lives alongside it.
- `src/app/SessionHooks.cpp`: joins the protocol state machine to room settings
  and `StreamPlayback`. Start here when a MATP command reaches the wrong behavior.
- `src/app/StreamPlayback.cpp`: device preparation, audio connection, ring
  buffer, scheduled transport, reports, recovery, volume and hardware trim.

```text
MATP control: ServerConnection -> Session.onData buffer -> Session.poll -> hooks
PCM receive: ServerConnection -> AudioStream -> AudioRingBuffer
PCM render:  TimelineSource -> AudioEngine -> IAudioBackend -> sound card
Device API:  ControlServer -> CommandQueue -> ClientCommandHandler (owner loop)
```

Control and audio use separate `ServerConnection` instances. `PREPARE_STREAM`
opens the device and reports the actual granted format. It primes/runs the
device with silence; only the scheduled `START_AT` makes buffered PCM audible.

## Locate a change

| Area | Files |
| --- | --- |
| CLI, persisted config, room identity | `src/app/CommandLine.cpp`, `src/config/ClientConfig.cpp`, `src/matp/ClientIdentity.cpp` |
| Probe/cache/select audio device | `src/audio/AudioDeviceCache.cpp`, `FormatProbe.cpp`, `src/app/DeviceSelection.cpp` |
| Reconnect, socket handling | `src/net/ServerConnection.cpp`, `src/net/Socket.*` |
| MATP control, clock conversion | `src/matp/Session.cpp`, `ClockModel.cpp`; common serializers in root `shared/matp/` |
| Packet validation, discontinuities, timeline <-> stream frames | `src/matp/AudioStream.cpp`, `TimelineMap.cpp` |
| Render start/pause/stop/flush and de-click | `src/audio/TimelineSource.cpp`, `src/app/StreamPlayback.cpp` |
| Device lifecycle/reopen | `src/audio/AudioEngine.cpp`, `PlaybackSupervisor.cpp` |
| Volume/clock correction execution | `src/audio/VolumeControl.cpp`, `HardwareVolume.cpp`, `HardwareClock.cpp`, `DriftEstimator.cpp` |
| Windows / Linux / ESP32 audio | `src/audio/windows/WasapiBackend.cpp`, `src/audio/linux/AlsaBackend.cpp`, `src/audio/esp32/I2sBackend.cpp` / `Es8388.cpp` |
| Settings commands/status/device web UI | `src/app/ClientCommandHandler.cpp`, `src/control/ControlProtocol.cpp`, `ControlHttp.cpp`, `ControlServer.cpp`, `ClientStatus.cpp` |
| Buttons/encoders and gesture dispatch | `src/input/InputController.cpp`, platform input backends, `src/app/InputPump.cpp`, `InputSettings.cpp` |
| Linux networking, CPU, SSH, DAC options | `src/net/NetworkSettings.cpp`, `SshSettings.cpp`, `src/config/CpuSettings.cpp`, `src/audio/linux/AlsaDacSettings.cpp` |
| ESP32 board configuration/setup UI | `esp32/boards/`, `esp32/main/WifiSettings.cpp`, `WebUi.cpp`, `src/config/BoardConfig.cpp` |

Headers in `include/` document public contracts; backend-private headers stay
beside their `.cpp` files. Shared format, clock snapshot and protocol headers
come from root `shared/matp/include/`, not a second client definition.

## Thread and timing rules

- The owner/main loop owns config, device cache, session decisions and command
  handling. MATP control `onData()` only buffers; `poll()` processes it here.
  Clock-sync receive timestamps still come from the actual socket receive.
- The audio connection thread parses packets and is the ring's sole producer.
  A full ring backpressures TCP; it does not discard arbitrary PCM.
- The backend render thread is the ring's sole consumer. No allocation, I/O,
  synchronous logging or unbounded lock/wait in the render callback.
  Reconfigure the ring only after producer and consumer have been quiesced.
- `TimelineMap` converts zone timeline nanoseconds to this stream's frames;
  `TimelineSource` maps stream frames to device frames. Never substitute one
  axis for another or compare frame positions from different rooms.
- Starts, pauses and volume changes are scheduled on device frames. Underrun
  silence advances device time, so stale audio must be discarded afterward.
- The client measures/reports clock drift; core chooses correction. Hardware
  trim executes core feedback only on verified supported hardware. No client
  software resampling or independent catch-up controller.
- Shut down `CommandQueue` before joining the control listener, releasing any
  request waiting for the owner loop. Preserve recovery/stop ordering in
  `StreamPlayback` and `PlaybackSupervisor` when changing device ownership.
- ARMv6 is supported: keep shared/client code C++17 and preserve the 32-bit
  atomic/wrap handling used by ring indices and render counters.

## Configuration and checks

`client.json` is persistent; CLI overrides apply to that run. A generated
`clientId` must survive restarts. `audio_devices.json` caches expensive full
capability probes; `--rescan-audio` rebuilds it. See `src/app/CommandLine.cpp`
for exact flags and `src/config/ClientConfig.cpp` for defaults/validation.
Use explicit `--config` and `--audio-cache` paths for experiments.

Tests in `tests/` follow the component names: `test_stream_playback.cpp`,
`test_matp_session.cpp`, `test_matp_audio_stream.cpp`, `test_timeline_source.cpp`,
`test_drift_estimator.cpp`, `test_client_command_handler.cpp`, and
`test_matp_end_to_end.cpp`. Linux adds ALSA snapshot/shutdown/rescan tests and
input discovery checks; a Windows pass does not exercise these backends.

New common `.cpp` files must enter `cmake/ClientSources.cmake`, consumed by
desktop CMake and `esp32/main/CMakeLists.txt`. Shared sources added to the
desktop build may also need adding to ESP-IDF's explicit shared-source list.
Read [ESP32 notes](../../client/esp32/README.md) or
[Connector 2 port notes](../hardware/raumfeld/connector2.md) for those targets.
