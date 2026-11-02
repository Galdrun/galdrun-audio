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
| Bluetooth receive (phone takeover) | `include/bluetooth/BluetoothReceiver.h` (per-platform interface), `src/bluetooth/linux/BluealsaReceiver.cpp`, `src/app/BluetoothPlayback.cpp`; `Session::setBluetooth` reports MATP `localSource: "bluetooth"` |
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

A board file may list `inputs` (client.json's format):
`client_config::appendBoardInputs` adds those whose id `client.json` lacks at
startup, before device discovery, so fixed buttons added to an image reach an
older settings volume. Connector 3 lists its rear BT/Setup buttons there.
`btModeRec`/`btModePlay` are local input actions (like `deepSleep`): with the
board flag `bluetooth` the client runs the image helper
`/usr/sbin/galdrun-bluetooth pair|headphones`; the inputs response's
`bluetooth` gates them in the shared inputs editor.

Bluetooth headphones/speakers (A2DP source) are a **second client process**
started by the image helper (`galdrun-bluetooth headphones|headphones-on
ADDRESS|headphones-off`): its own `client.json`, control port 4730 and
`--board-file` containing only `{"bluetoothOutput": true}` (so it claims no
board feature and reports `bluetoothOutput` in AUDIO_CAPABILITIES), and a
`$HOME/.asoundrc` whose `default` is the raw `type bluealsa` PCM -- the `bluealsa`
alias is plug-wrapped and lets the server pick formats the link cannot carry.
The receiver (`BluealsaReceiver`) backs off opening the phone PCM on links
without one (headphones), and sets BlueZ's alias to the room name via
`IBluetoothReceiver::setName`. The settings page's Bluetooth card uses
`getBluetooth`/`scanBluetooth`/`setBluetoothHeadphones`/`pairBluetooth`
(`src/bluetooth/linux/BluetoothSettings.cpp` reads the helper's files); all
helper runs share `bluetoothProcess`, one at a time.

Bluetooth (board flag `bluetooth`): the platform receiver only delivers the
phone's 16-bit PCM on its own thread. `BluetoothPlayback` (owner loop) takes the
room after 0.3 s of audio: `Session::setBluetooth(true)` releases the server's
stream and card synchronously and reports `localSource: "bluetooth"` (power
stays `on`; it is not a power state); its own engine then plays
at the phone's rate in the selected device's format, so MCU rate and hardware
volume follow. 3 s without audio restores the earlier power state. Switching the
room on during it only clears the state; `BluetoothPlayback` closes its card
first and then calls `Session::resumeStream()`, because both cannot hold the
exclusive device. Phone and card clocks are not locked: the 500 ms ring drops or
underruns rarely. BlueZ, pairing and bluez-alsa are image services
(Connector 3: `hardware/buildroot/board/connector3/ram/galdrun-bluetooth`).

## Configuration and checks

`client.json` is persistent; CLI overrides apply to that run. `active: false`
(`setActive`, the settings page's "In the system") stops the server connection
but keeps `server`, the control API and discovery with `active: false`, so cores
can hide cached rooms even after restarting. Clients default to active. `main.cpp` and
`handleSetServer` both honour it. A generated
`clientId` must survive restarts. `audio_devices.json` caches expensive full
capability probes; `--rescan-audio` rebuilds it. See `src/app/CommandLine.cpp`
for exact flags and `src/config/ClientConfig.cpp` for defaults/validation.
Use explicit `--config` and `--audio-cache` paths for experiments.

Advanced Wi-Fi controls are shared by managed Linux appliances and ESP32.
Linux uses `NetworkSettings.cpp`; ESP32's `WifiSettings` persists/validates
country selection and `WebUi.cpp` handles explicit password clearing. Empty
ESP32 country keeps the SDK's automatic/world-safe policy; `app_main.cpp`
applies the choice through `esp_wifi_set_country_code`. OS-managed network
settings remain hidden. Verify `test_wifi_settings.cpp`, the ESP32 board builds,
and both WebGui backends without applying live WLAN changes.

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
Connector 3's experimental RAM userland, native MCU UART/uinput driver,
vendor-kernel D-Bus bridge and
network-manager qualification limits are in
[its hardware guide](../hardware/connector3.md#touch-buttons-audio-names-and-network-settings).
Connector 3's clean installer must exclude OEM runtime files. The user's final
2026-10-30 decision explicitly approves the working Marvell `15.87.19.p14`
hardware blob and accepts the available licence's version mismatch; do not
continue replacement-firmware research as a prerequisite. Read
[the next-day handover](../hardware/connector3-handover-2026-10-31.md) first:
  Galdrun is installed in NAND and its physical cold boot passed. The approved
  radio exception is implemented while OEM-runtime rejection remains active.
  The Wi-Fi clock/early-firmware fix passes a RAM boot but still needs installation
  and cold-boot/Wi-Fi-only qualification; use the handover's latest checkpoint.
  `hardware/buildroot/scripts/connector3_content.py` checks unpacked content
against source/license metadata; `test-connector3-content.py` runs its guards.
The standalone RAM image omits the old vendor D-Bus bridge. The explicit
`prepare-connector3-vendor-ram.py` test now builds/uploads that bridge separately.
`build-connector3-storage-check.sh` and `verify-connector3-storage-check.py`
implement the separate RAM-only NAND/USB qualification profile with a
controller-level write refusal guard and read-only MTD partitions. They are
not a persistent installer. Default generic RAM builds run the clean-content
  audit, including builds with the exact approved device-radio exception.
Its RAM image's ASoC machine driver is
`hardware/buildroot/board/connector3/ram/galdrun-connector3-audio.c` (from the
repository root), integrated by `hardware/buildroot/external.mk`; it selects
the S810 PLL/MCLK family for native 44.1/48 kHz audio. It also exposes the
stock-sign `Drift compensator` (-500..500 ppm; positive slows). The board file
opts into the existing hardware clock path. `test-connector3-audio.sh` checks
native rates and live trim readback while PCM stays RUNNING. See the hardware
guide's latest handoff for build and hardware qualification status.
Its CPU page reuses `CpuSettings.cpp` and the shared WebGui: the RAM board opts
into `cpuFrequency`, mainline i.MX cpufreq reads the speed fuse, and the DTS
connects the AXP152 DCDC4 supply (small regulator patch under that board's
`patches/linux/`). Only 792/996 MHz are exposed. The selected speed is stored in
`/etc/galdrun-cpu.conf`; RAM deployments must preserve it in their private
overlay. `test_connector3_cpu_power_web.cjs <url>` is an explicit hardware
integration check that changes/restores speed and standby. Connector 3's
persistent image installs `board/connector3/raumfeld-suspend`; the existing
client detects that executable to enable manual and timed deep sleep.
`connector3-pm.c` powers off CPU0 through the secure PSCI monitor, with DDR
self-refresh and SCU/peripheral power retained for Ethernet wake. Stock PLL
handling is preserved. The monitor must restore CNTFRQ, and CPU-PM notifications
restore CNTKCTL. The older nonsecure `connector3-suspend.S` path is unused.
The persistent DT enables `galdrun,cpu-core-suspend`; private RAM tests may use
`galdrun,experimental-suspend` or boot parameter `connector3_suspend=1`.
The helper detaches the built-in WLAN driver before native PCIe suspend/reset,
then binds it and restarts saved networking on wake. Keep native PCIe resume
enabled: the retired retention bypass loses the radio link. Ethernet supports
magic wake; the top touch controls wake through MCU UART. WLAN-only deep sleep
uses local touch wake, with Wi-Fi restored afterward; no WoWLAN is advertised.
The idle deep-sleep timer only fires with a wired wake path to the server
(`wiredWakeAvailable`, all boards); explicit requests still sleep over WLAN.
The explicit hardware diagnostic invokes the helper with RTC fallback:
`hardware/buildroot/scripts/test-connector3-suspend.sh`;
read the current hardware checkpoint before running it.
Connector 3's `board/connector3/ram/mcu-buttons.cpp` (under
`hardware/buildroot/`) also owns audio MCU initialization and its root-only
`/run/galdrun-connector3-mcu.sock`. `Connector3Mcu.cpp` shares that service between
`AlsaMixer.cpp` (room hardware volume/mute, card `Connector3` only) and
`AlsaDacSettings.cpp` when
the board enables `mcuAudioControls`, reusing the existing DAC page and
card-specific persistence for hardware output level/mute. These two MCU values
are read-only in DAC settings: API writes are rejected and reset preserves them;
firmware restoration remains internal. The MCU reports 0..100 raw level with
no dB calibration; no dB scale is invented. Clock trim is transient and belongs
to synchronization; soundbar DSP commands are not exposed. MCU volume/mute API
readbacks passed, but their audible effect remains a pending hardware test,
explicitly deferred by the user; both RCA channels playing are confirmed.
Run the MCU helper's `--self-test` and the RAM
startup check, then verify power/mute readback and real RCA playback on hardware.

Connector 3 startup synchronizes the MCU, selects host I2S, powers On and
verifies the source-qualified encoded sample rate before publishing its socket.
`AlsaBackend::open` then sends the actual PCM rate through that socket before
playback; failure stops the device open. The persistent init waits for this
readiness before starting the client. The rate is restored on startup rather
than relying on volatile MCU state surviving a reboot.
Connector 3 builds the existing `board/connector2/status-leds.cpp` monitor with
`CONNECTOR3_STATUS_LEDS`, passing `/settings/client.json` (persistent) or
`/run/client.json` (RAM). It uses the MCU's three-front-LED indications through
the same socket; Connector 2 retains its GPIO patterns. `ledsOff` and standby
switch off the status LEDs. The rear Bluetooth indicator is separate and is
not used to imply an available Bluetooth playback backend. Verify with
`hardware/buildroot/scripts/test-status-leds.py`,
`hardware/buildroot/scripts/test-connector3-status-leds.py`, the MCU self-test,
and `test-connector3-persistent-init.py`, then real boot/listening checks.

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
