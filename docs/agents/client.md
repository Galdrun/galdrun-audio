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
| Device restart / profile reboot | Local `restart` command and `getPower.restart` in `ClientCommandHandler.cpp`; Buildroot `board/common/overlay/usr/sbin/galdrun-restart`; ESP32 `WebUi.cpp` `/api/restart`. Pi `S41galdrun-audio-profile` invalidates the device cache before startup when the boot profile changes. |
| Buttons/encoders and gesture dispatch | `src/input/InputController.cpp`, platform input backends, `src/app/InputPump.cpp`, `InputSettings.cpp` |
| Linux networking, CPU, SSH, DAC options | `src/net/NetworkSettings.cpp`, `SshSettings.cpp`, `src/config/CpuSettings.cpp`, `src/audio/linux/AlsaDacSettings.cpp` |
| Raspberry Pi Buildroot images/profiles/GPIO wiring | Root `hardware/buildroot/board/rpi/README.md`, `board/rpi/audio-profiles.json`, `board/rpi/inputs.json`, `configs/raumfeld_rpi{1,3,4,5}_defconfig`; `S42galdrun-gpio` resolves the Pi 4/5 controller, `S94galdrun-inputs` seeds header-specific inputs on first boot; shared services in `board/common/` |
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

Pi firmware uses the same network/SSH/standby services, with MAC-based Wi-Fi
radio selection/hotplug and AP fallback. `getAudioProfile`/`setAudioProfile`
manage its boot audio include; only the explicit profile selector remounts FAT.
`DeviceSelection.cpp` binds DAC persistence to the selected ALSA card on opted-in
Pi images; Connector 2 retains its CS4271 defaults. See the
[Pi guide](../../hardware/buildroot/board/rpi/README.md) for capability limits.
The `rpi3` image covers Pi 2 B (both CPU variants), Pi 3 B/B+/A+ and Zero 2 W;
firmware also uses the B+ DTB for Pi 3 A+. `rpi1` covers Pi 1 A/B/A+/B+ and
Zero/W/WH. Pi 1 A/A+ use the B/B+ DTBs; W and WH share their model identity.
Zero 2 W has both DTB aliases and both pinned SDIO Wi-Fi firmware variants;
the built-in OTG controller supports USB DACs with an OTG adapter. It uses
the same 40-pin inputs and audio-profile selector, with no analogue headphone output.
Network startup skips absent Ethernet devices and restores all supported link
modes for automatic speed (including B+ Gigabit).
Bluetooth overlays are filtered by model (`pi0w` in `rpi1`, `pi3`/`pi02` in
`rpi3`), so Pi 2 and non-Bluetooth models retain their native PL011 setup.
Original Zero and A/A+ need a USB network adapter; Pi 2 has Ethernet but no Wi-Fi.
`rpi4` and `rpi5` use separate ARM64 images and the `linux-armv8` client.
Their kernels use 4 KiB pages; Pi 5 includes RP1 GPIO/I2S/Ethernet support.
GPIO defaults use `/dev/galdrun-gpio`, resolved from the controller label on
every boot rather than assuming a gpiochip number. Pi 4 uses start4 firmware;
Pi 5 boots through its existing EEPROM firmware and uses ttyAMA10 on its debug
connector. Both include explicit KMS overlays for HDMI audio. Hardware
qualification, including HATs and grouped playback, remains separate from builds.
