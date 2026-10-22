# Galdrun Client

The Galdrun client turns a computer, a Raspberry Pi, an ESP32 audio board or a
Raumfeld Connector 2 into a **room** of a Galdrun multiroom system. The Galdrun
server sends the music; every client plays it **in sync** with the other rooms
of its zone.

- **Bit-perfect** where possible: a format the sound card can play is sent
  unchanged; only what it cannot play is converted, for this room alone.
- **In sync**: each client measures its sound card's clock, and the server
  corrects the drift, by resampling or by trimming the hardware clock on boards
  that support it.
- **Found automatically**: the client announces itself on the network, so the
  server can add it without you typing an address.
- **Settings in the browser** at `http://<device>:4720/`: name, audio device,
  volume control, buttons, and on Raumfeld/ESP32 also network and Wi-Fi.
- **Buttons and encoders** on boards that have them: play/pause, volume, mute,
  standby, and more.

## Supported devices

| Target | Runs on |
|---|---|
| `windows-x64` | Windows 10/11, 64-bit |
| `linux-x64` | 64-bit Intel/AMD Linux |
| `linux-x86` | 32-bit Intel/AMD Linux |
| `linux-armv6` | Raspberry Pi 1, Zero, Zero W |
| `linux-armv7` | Raspberry Pi 2, or any newer Pi with a 32-bit OS |
| `linux-armv8` | Raspberry Pi 3, 4, 5, Zero 2 W with a 64-bit OS |
| `linux-armv9` | ARMv9 boards with a 64-bit OS (no current Pi needs this) |
| `esp32-a1s` | ESP32-A1S audio kit (ES8388 codec) |
| `connector2` | Teufel Raumfeld Connector 2, as a complete firmware |

Pick a Linux target by the **installed OS**, not only the CPU: a 64-bit build
does not run on a 32-bit Raspberry Pi OS. Linux clients need ALSA
(`sudo apt install libasound2`, or `libasound2t64` on newer releases).

## Getting started

1. Run the Galdrun server on your network.
2. Put the client on the device, with the [upload scripts](#upload) or by hand
   (below).
3. The server finds the new room; give it a name and add it to a zone, in the
   server app or on the client's settings page.

| Device | First start |
|---|---|
| Windows | Copy `releases\client\windows-x64` anywhere and run `galdrun-client.exe`. |
| Linux / Raspberry Pi | `scripts/upload/linux.sh` installs it as a service that starts at boot. By hand: see [release/README.md](release/README.md). |
| ESP32 | Flash it with `scripts/upload/esp32.sh`. The board then opens the Wi-Fi `Galdrun-Setup-XXXXXX`; join it and pick your network at `http://192.168.4.1/`. See [esp32/README.md](esp32/README.md). |
| Raumfeld Connector 2 | Install the firmware with a USB stick: [releases/client/raumfeld/connector2/README.md](../releases/client/raumfeld/connector2/README.md). Without Ethernet it opens the Wi-Fi `Connector2` (password `connector2`) for setup. |

## Build

One script per host builds everything and installs what is missing first.
Running it again skips what is already there. Run it from the repository root.

```sh
./build-client.sh                       # Linux: all targets except Windows
./build-client.sh connector2            # one target
./build-client.sh linux esp32-a1s       # groups and targets mixed
```

```powershell
powershell -ExecutionPolicy Bypass -File build-client.ps1                          # Windows: all targets
powershell -ExecutionPolicy Bypass -File build-client.ps1 -Target windows,linux-armv8
```

Targets: `all` (default), the groups `windows`, `linux`, `esp32`, and every name
from [Supported devices](#supported-devices). `windows-x64` builds only on
Windows. On Windows, everything else is built inside WSL Ubuntu.

Results go to `releases/client/<target>`; the Connector 2 USB image to
`releases/client/raumfeld/connector2/connect2.img`.

What the scripts install when it is missing:

| What | Where | For |
|---|---|---|
| Visual Studio 2022 Build Tools (C++), CMake | Windows, via `winget` | `windows-x64` |
| WSL Ubuntu | Windows; asks you to create your Ubuntu user, then to run the script again | everything except `windows-x64` |
| Compilers, CMake, Ninja and other packages | Ubuntu, via `apt` (`sudo`) | Linux, ESP32, Connector 2 |
| ARM and x86 cross toolchains, ALSA per target | `~/.local/share/multiroom-linux` | Linux targets, Connector 2 |
| ESP-IDF 5.5.1 | `~/esp/esp-idf-v5.5.1`, tools in `~/.espressif` | ESP32 |
| Buildroot, Linux kernel and package sources | `hardware/buildroot/downloads`, build tree in `~/.cache/raumfeld-buildroot` | Connector 2 |

The Connector 2 firmware and its USB installer are built entirely from source.
Details: [hardware/buildroot/README.md](../hardware/buildroot/README.md).

A first build of everything takes roughly half an hour to an hour, most of it
Buildroot and the downloads. Later builds only rebuild what changed.

## Upload

The scripts in `scripts/upload/` put a build onto a device. Each has a Windows
(`.ps1`) and a Linux (`.sh`) version with the same options; the top of each
script lists them all.

| Device | Script | How |
|---|---|---|
| Raspberry Pi / any Linux | `linux.sh`, `linux.ps1` | SSH: detects the device's architecture, uploads the matching build, installs ALSA if missing and a `galdrun-client` service that starts at boot. `client.json` is kept. |
| Raumfeld Connector 2 | `connector2.sh`, `connector2.ps1` | SSH: replaces only the client and keeps settings and room. Checks that the new client starts on the device first; keeps the previous one as `galdrun-client.previous`. |
| ESP32 | `esp32.sh`, `esp32.ps1` | USB cable: flashes the board with esptool (downloaded once). The settings on the board are kept. |

```sh
scripts/upload/linux.sh --ip 192.168.1.31 --user dietpi
scripts/upload/connector2.sh --ip 192.168.1.38
scripts/upload/esp32.sh --board esp32-s3-devkitc --port /dev/ttyACM0
```

```powershell
scripts\upload\linux.ps1 -IP 192.168.1.31 -User dietpi -IdentityFile $env:USERPROFILE\.ssh\id_ed25519
scripts\upload\connector2.ps1 -IP 192.168.1.38
scripts\upload\esp32.ps1 -Board esp32-a1s -Port COM5
```

SSH logins use a key file (`--identity` / `-IdentityFile`); without one, ssh
asks for the password. For the Connector 2 the key your own firmware build
created in `hardware/buildroot/private/connector2/` is used by default.
`linux` takes an optional server address (`--server host:4711`); without it the
server attaches the client after finding it on the network. The ESP32 port can
be left out when only one serial port is present.

## Scripts

| Script | Purpose |
|---|---|
| `build-client.sh` | Builds the Linux, ESP32 and Connector 2 targets and installs their tools. |
| `build-client.ps1` | Windows: builds `windows-x64`, runs `build-client.sh` in WSL for the rest. |
| `scripts/upload/*` | Upload to devices, see [Upload](#upload). `linux-remote.sh` and `connector2-remote.sh` are their device-side halves. |
| `client/scripts/install-linux-tools.sh` | Downloads the cross toolchains and builds ALSA for each Linux target; called by `build-client.sh`. |
| `client/scripts/verify-linux-binaries.sh` | Starts every Linux build under QEMU to check it runs on its CPU. |
| `hardware/buildroot/build.sh`, `Build.ps1` | Builds only the Connector 2 firmware. |

## Configuration

The client keeps its settings in `client.json`, changed through the settings
page or the server:

| System | Location |
|---|---|
| Windows | `%LOCALAPPDATA%\Galdrun\` |
| Linux | `~/.config/galdrun/` (the upload script uses `~/galdrun-client/`) |
| ESP32 | `/data/` on the board |
| Connector 2 | `/etc/galdrun/` |

A new client generates its room ID on first start. `audioAccessMode` chooses
`exclusive` (default; the sound card directly, falling back to the system mixer),
`exclusive-only` or `shared`. Exclusive mode keeps playback bit-perfect and lets
the client measure the sound card's real clock.

Useful command-line options (they apply to this run only):

```
galdrun-client --server 192.168.1.20:4711     connect to this server
galdrun-client --audio-device <id>            use this sound card
galdrun-client --test-tone 10                 play a 440 Hz tone and report the clock drift
galdrun-client --log-level debug
galdrun-client --help                         all options
```

## Development

C++17 and CMake, no third-party libraries: WASAPI on Windows, alsa-lib on Linux,
the ESP-IDF I2S driver on the ESP32. Code shared with the server is in
[`../shared`](../shared), the wire protocol in
[`../docs/MATP-AudioTransferProtocol.md`](../docs/MATP-AudioTransferProtocol.md).

```sh
cd Client
cmake --preset linux-x64 && cmake --build --preset linux-x64 && ctest --preset linux-x64
```

On Windows: `cmake -S Client -B client/build`, then
`cmake --build client/build --config Release` and
`ctest --test-dir client/build -C Release`.
