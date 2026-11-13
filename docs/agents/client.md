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
- `ServerConnection` checks once a second that the socket's local address is
  still on an interface and drops the connection when it is gone. A pulled
  cable otherwise shows only through the session's 20 s silence timeout; with
  the check, a device falling back to Wi-Fi reconnects as soon as that is up.
  For 15 s after such a drop, failed attempts keep the 1 s backoff step: the
  switch-over is local, and a backoff grown on "Network is unreachable" only
  adds seconds.
- The core repeats a `START_AT` when no audible `PLAYBACK_REPORT` reached it in
  time. A room that is already playing answers it with an immediate report
  (`Session::handleStartAt`), not an error; it does not resynchronize.
- ARMv6 is supported: keep shared/client code C++17 and preserve the 32-bit
  atomic/wrap handling used by ring indices and render counters.

A board file may list `inputs` (client.json's format):
`client_config::appendBoardInputs` adds those whose id `client.json` lacks at
startup, before device discovery, so fixed buttons added to an image reach an
older settings volume. Connector 3 lists its rear BT/Setup buttons there.
A button press while the power state is not `on` (automatic or manual standby)
only wakes the device: `InputController`'s press gate consumes that press until
its release and `InputPump` sends one `standbyOff`, so a tap in the dark never
also changes volume or skips a track. Buttons bound to a power action
(standby/deep sleep) keep their own gestures. Connector 3's `raumfeld-suspend`
likewise skips restoring manual standby when the wake IRQ is the MCU UART's
(touch); a magic-packet wake keeps it.
Board flag `inputSounds` (Connector 3): each button gesture that was recognized
and dispatched successfully (not the raw touch: a double click clicks once, a
long press at 2 s, a press that makes no gesture never) calls
`FeedbackSound::play` (`src/audio/FeedbackSound.cpp`), and every open
`AudioEngine` mixes the click over its source in the render callback, at the
device rate; `KEY_PLAYPAUSE` gets the play/pause click. The clicks are
synthesized from levels/pitch measured on the stock `touch.flac` and
`touch-play-pause.flac`, because Teufel's files are not redistributed. Level
matches stock (`libraumfelddsp.so`, `hardwared` chain `input -> system-sounds
-> user-eq -> output`): `Rompler::process` adds the file samples at unity gain;
`Sounds` only fades the music linearly toward 0.25 at 3.0 per second while a
sound plays (about -1 dB by the end of one 40 ms click), which is not copied. A
device that is not running (standby, no server) does not click: opening the
exclusive card for 40 ms would race `PREPARE_STREAM` and the MCU rate setup.
`btModeRec`/`btModePlay` are local input actions (like `deepSleep`): with the
board flag `bluetooth` the client runs the image helper
`/usr/sbin/galdrun-bluetooth pair|headphones`; the inputs response's
`bluetooth` gates them in the shared inputs editor.

Bluetooth headphones/speakers (A2DP source) are a **second client process**
started by the image helper (`galdrun-bluetooth headphones|headphones-on
ADDRESS|headphones-off`): its own `client.json`, control port 4730 on
`127.0.0.1` only (the main room's page has the device's settings), the main
room's name plus ` (BT-Client)` (rewritten at start; the helper's `alias`,
which the main client runs on a rename, passes it on through `setName`), and
`--board-file` containing only `{"bluetoothOutput": true}` (so it claims no
board feature and reports `bluetoothOutput` in AUDIO_CAPABILITIES), and a
`$HOME/.asoundrc` whose `default` is the raw `type bluealsa` PCM -- the `bluealsa`
alias is plug-wrapped and lets the server pick formats the link cannot carry.
The receiver (`BluealsaReceiver`) backs off opening the phone PCM on links
without one (headphones), and sets BlueZ's alias to the room name via
`IBluetoothReceiver::setName`. The settings page's Bluetooth card uses
`getBluetooth`/`scanBluetooth`/`setBluetoothHeadphones`/`pairBluetooth`/`setBluetoothQuality`
(`src/bluetooth/linux/BluetoothSettings.cpp` reads the helper's files); all
helper runs share `bluetoothProcess`, one at a time.

Bluetooth (board flag `bluetooth`): the platform receiver only delivers the
phone's 16-bit PCM on its own thread. `BluetoothPlayback` (owner loop) takes the
room after 0.3 s of audio: `Session::setBluetooth(true)` releases the server's
stream and card synchronously and reports `localSource: "bluetooth"` (power
stays `on`; it is not a power state); its own engine then plays
at the phone's rate in the selected device's format, so MCU rate and hardware
volume follow. Invariant: room volume/mute (`runtime.volumeGain`/`volumeMuted`/
`volumePath`, which `StreamPlayback` keeps publishing while closed) apply to
that engine at once, on the device's control or its own `VolumeControl`, and a
changed device/access mode reopens it (or returns the room when the phone's
rate does not fit); otherwise a muted room plays the phone and the reported
output is not the one playing. 3 s without audio restores the earlier power state. Switching the
room on during it only clears the state; `BluetoothPlayback` closes its card
first and then calls `Session::resumeStream()`, because both cannot hold the
exclusive device. Phone and card clocks are not locked: the 500 ms ring drops or
underruns rarely. BlueZ, pairing and bluez-alsa are image services
(Connector 3: `hardware/buildroot/board/connector3/ram/galdrun-bluetooth`).

## Self update

`src/app/SelfUpdate.cpp` with the shipped updaters
`release/galdrun-update.sh` (BusyBox sh) and `release/galdrun-update.ps1`.
The server uploads a release package to `POST /api/update` on the control
port (`X-Galdrun-Sha256`; `ControlHttp.cpp` buffers up to 32 MiB for this
path only) and the `update` command schedules the hand-over; `getUpdate`
reports `version`, `target`, `supported`/`reason` and `trial`. After the main
loop has shut everything down, Linux `exec()`s the script, which checks the
hash, unpacks to `.update/staged`, runs the new binary with `--version`, moves
the replaced entries to `.update/previous` and `exec()`s the client with the
same arguments: the PID never changes, so systemd and `S95galdrun-client`'s
pidfile stay valid. Windows starts the script, exits, and the script waits
for the PID, swaps and starts the client again. Every failure path ends by
starting the current client.

Invariant: the new version runs on trial (`.update/trial`: `pending` ->
`running` at start). It confirms on an established session (or at once with
no server/inactive); 180 s without one, or a start that finds `running`
(the trial run crashed), rolls back through the same script. A normal stop
during the trial resets it to `pending`. The trial file names the client's
config path (`--config`, else the default), which `handOver()` passes to the
updater explicitly (`GALDRUN_TRIAL_OWNER` / `-Owner`) so the scripts never
parse client arguments; another client in the same folder (Connector 3
headphone client) ignores it for confirming and rolling back. A new update is
refused while any client's trial is in the folder (`trialPending()`, rechecked
by both scripts at install), because the folder has one rollback copy. The script frees
the last rollback copy (`.update/previous`) only after the checksum passed,
before unpacking, so small volumes need room for only one extra copy.

Invariant: the appliances never contact the Internet themselves. Time
comes from the router, else from the core's SNTP answer on UDP 4721
(`hardware/buildroot/board/common/overlay/usr/sbin/galdrun-time-sources`).

Invariant: every Raumfeld/Buildroot appliance (Connector 2, Connector 3,
Pi images) mounts its root file system **read-write** and the client updates
itself in place in `/opt/galdrun-client`, the same way on each. Do not make a
root read-only "for safety" or move the client elsewhere for one device:
recovery never depends on it -- the factory bootloader's USB path is in
partitions the system never writes (Connector 3 MTD0) -- and a read-only root
only breaks the self update (2026-11-08: Connector 3 had one; reverted).
The same goes for platform packages and helpers: Connector 3 gets what
Connector 2 has (chrony, `galdrun-restart`, debug tools, BusyBox applets),
in `board/connector3/ram/defconfig` and `ram/post-build.sh`.

Unsupported: ESP32, unknown architectures, no updater beside the binary
(installs from before this feature need one manual update), unwritable
folders, and installs on tmpfs/ramfs (Connector 3 RAM test build).

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
continue replacement-firmware research as a prerequisite. The device state and
installed images are in [the current handover](../hardware/connector3-handover-2026-11-05.md).
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
no dB calibration; no dB scale is invented. Invariant: the room volume sets it
to the slider position (`hardware_volume::positionForGain`), never the linear
gain: Raumfeld's own firmware treats this 0..100 as linear in dB (`libraumfeldcpp`
`VolumeConversion::mapPercentToDb`, -41..+24 dB on the Streamer), so the
amplitude made half the slider about -40 dB. Each MCU request is a UART round
trip of up to 0.2 s, so an unscheduled room volume on a hardware control is set
by the next `StreamPlayback::poll()` (`hardwareDue_`), not per SET_VOLUME: a
dragged slider's burst sets it once, to the newest, instead of queueing
seconds of writes. Clock trim is transient and belongs
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
