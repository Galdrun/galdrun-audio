# Galdrun Core

The core is the audio engine of a Galdrun multiroom system. It decodes the music,
plays it into zones on one shared timeline and streams each room exactly the
audio it needs, so that all rooms of a zone stay in sync. The Galdrun server
controls it; the [clients](../client/README.md) play what it sends.

- **Decodes almost anything**: FLAC, ALAC, WavPack, APE, DSD (DSF/DFF), WAV/AIFF at
  any depth, MP3, AAC, Vorbis, Opus, AC-3, DTS and more, from files or HTTP(S)
  streams, through an audio-only FFmpeg build.
- **Bit-perfect where possible**: a room whose sound card can play the source
  format gets it unchanged. Otherwise it is resampled for that room alone, with
  soxr or r8brain.
- **In sync**: every room's clock is measured, and its drift is corrected by
  resampling or, on clients that support it, by trimming the DAC clock.
- **Finds clients by itself** through their discovery beacons.
- **Per-room equalizer**, volume and grouping of rooms into zones.

## Supported systems

| Target | Runs on |
|---|---|
| `windows-x64` | Windows 10/11, 64-bit |
| `linux-x64` | 64-bit Intel/AMD Linux (glibc 2.43 or newer, e.g. Ubuntu 26.04) |
| `linux-x86` | 32-bit Intel/AMD Linux (same glibc) |
| `linux-armv7` | Raspberry Pi 2 or newer with a 32-bit OS (ARMv7 with NEON) |
| `linux-armv8` | Raspberry Pi 3, 4, 5 with a 64-bit OS |
| `linux-armv9` | ARMv9-A hardware only (a Pi 5 is ARMv8.2) |

ARM builds need glibc 2.31 or newer (Raspberry Pi OS / DietPi Bullseye or newer).
A Raspberry Pi 2 or 3 serves a few rooms; how many depends on how many of them
need resampling.

## Running it

| System | Start |
|---|---|
| Windows | Run `releases\server-core\windows-x64\galdrun-core.exe`. As a Windows service: `galdrun-core.exe --service --config <path>`. |
| Linux | Copy `releases/server-core/linux-<arch>` to the machine, then `cp server.example.json server.json` and `./run.sh --config server.json`. |

Settings are in `server.json`: `%LOCALAPPDATA%\Galdrun\` on Windows,
`~/.config/galdrun/` on Linux, or the file given with `--config`.

| Port | Use |
|---|---|
| 4711/tcp | Clients connect here (MATP control) |
| 4712/tcp | Audio to the clients |
| 4721/udp | Discovery beacons from the clients |
| 4713/tcp | Control API for the Galdrun server, on `127.0.0.1` only |

The control API has no authentication, so it listens on loopback by default.
Only change `control.bindAddress` if the server runs on another machine and the
network is trusted.

```
galdrun-core --config <path>       settings file
galdrun-core --log-level debug     error | warn | info | debug | trace
galdrun-core --log-file <path>     also write the log to a file
galdrun-core --help
```

## Build

One script per host builds everything and installs what is missing first.
Running it again skips what is already there. Run it from the repository root.

```sh
./build-server-core.sh                   # Linux: all Linux targets
./build-server-core.sh linux-armv7       # one target
```

```powershell
powershell -ExecutionPolicy Bypass -File build-server-core.ps1                        # Windows: all targets
powershell -ExecutionPolicy Bypass -File build-server-core.ps1 -Target windows,linux-armv8
```

Targets: `all` (default), the groups `windows` and `linux`, and every name from
[Supported systems](#supported-systems). `windows-x64` builds only on Windows; on
Windows the Linux targets are built inside WSL Ubuntu. Results go to
`releases/server-core/<target>`.

What the scripts install when it is missing:

| What | Where | For |
|---|---|---|
| Visual Studio 2022 Build Tools (C++), CMake | Windows, via `winget` | `windows-x64` |
| WSL Ubuntu | Windows; asks you to create your Ubuntu user, then to run the script again | Linux targets |
| Compilers, CMake, Ninja, nasm, QEMU and other packages | Ubuntu, via `apt` (`sudo`) | Linux targets |
| ARM and x86 cross toolchains | `~/.local/share/multiroom-linux` (shared with the client) | Linux targets except `x64` |
| OpenSSL 3.5.8, per target | `~/.local/share/multiroom-linux/openssl/<arch>` | Linux targets |
| FFmpeg 9.0.2, per target | `server-core/ffmpeg/dist/linux-<arch>-shared` | Linux targets |

FFmpeg is built from its official source, checked against a pinned SHA256, with
only the audio decoders, demuxers and network protocols the core uses
([ffmpeg/README.md](ffmpeg/README.md)). Windows uses the prebuilt FFmpeg DLLs in
[third_party/ffmpeg](third_party/ffmpeg/README.md), made by the same script. A
first build of all targets takes about 20 minutes, most of it FFmpeg.

## Scripts

| Script | Purpose |
|---|---|
| `build-server-core.sh`, `.ps1` | Build all targets and install their tools, see [Build](#build). |
| `server-core/scripts/install-linux-dependencies.sh` | Builds OpenSSL and FFmpeg for one or more Linux targets; called by `build-server-core.sh`. |
| `server-core/scripts/verify-linux-binaries.sh` | Starts every Linux release under QEMU: version, help, opening its ports and a clean shutdown. |
| `server-core/ffmpeg/build.sh`, `build-windows.ps1` | The audio-only FFmpeg build (Linux, and Windows through a portable MSYS2). |

## Licences

The core bundles FFmpeg (LGPL-2.1-or-later, built without GPL parts), OpenSSL 3.5.8
(Apache-2.0) on Linux, libsoxr (LGPL-2.1-or-later) and r8brain-free-src (MIT).
Every release folder carries their notices in `licenses/`.

## Development

C++23 and CMake. Code shared with the client is in [`../shared`](../shared), the
protocol in [`../docs/MATP-AudioTransferProtocol.md`](../docs/MATP-AudioTransferProtocol.md).

```sh
cd server-core
cmake --preset linux-x64 && cmake --build --preset linux-x64 && ctest --preset linux-x64
```

On Windows: `cmake --preset windows-x64`, `cmake --build --preset windows-x64`,
`ctest --preset windows-x64`. A few tests depend on timing and can fail on a busy
machine; run them again before looking for a bug.
