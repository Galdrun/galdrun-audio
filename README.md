# Galdrun

Galdrun is a self-hosted multiroom audio system. A server on your network plays
your music; every room plays it **in sync** with the other rooms of its zone,
**bit-perfect** where the sound card allows. A room can be a PC, a Raspberry Pi,
an ESP32 audio board or a Teufel Raumfeld Connector 2 with a custom firmware.

- **Music sources**: local folders and SMB shares, Tidal ([unofficial](#tidal)), TuneIn and
  web radio, with one search across all of them.
- **Zones and rooms**: group rooms into zones, one queue per zone, per-room volume
  and equalizer.
- **In sync**: each room's clock is measured and its drift corrected, by
  resampling or by trimming the DAC clock on boards that support it.
- **Control** in the browser, through a REST API, or with an AI assistant via MCP.

## Components

| Folder | What it is |
|---|---|
| [`server/`](server/README.md) | The Galdrun server (.NET): library, sources, zones and queues, web interface, API and MCP. |
| [`server-core/`](server-core/README.md) | The core (C++): decodes the music and streams it to the rooms on one shared timeline. |
| [`server-gui/`](server-gui/www) | The server's web interface, packed into every server build. |
| [`client/`](client/README.md) | The client (C++) that turns a device into a room: Windows, Linux, Raspberry Pi, [ESP32](client/esp32/README.md). |
| [`hardware/buildroot/`](hardware/buildroot/README.md) | Firmware that turns a Raumfeld Connector 2 into a Galdrun client. |
| [`mobile-gui/`](mobile-gui/README.md) | A remote-control app (Avalonia) for desktop, Android and iOS. Early work. |
| [`shared/`](shared) | Code and web assets shared by the C++ parts: protocol, discovery, JSON, branding. |
| [`docs/`](docs) | [MATP](docs/protocol/MATP-AudioTransferProtocol.md), the protocol between core and clients; hardware port notes. |

A minimal setup is the server and the core on one machine, plus one client per
room. The server finds new clients on the network by itself.

## Download

Ready-to-run builds for every target are on the
[Releases](../../releases) page. Each component's README says which file fits
which device and how to start it.

## Build

One script per component, run from the repository root. It installs whatever is
missing first (compilers, SDKs, cross toolchains; on Windows inside WSL Ubuntu)
and skips what is already there. Results go to `releases/<component>/<target>/`.

| Component | Linux | Windows |
|---|---|---|
| Server | `scripts/build-server.sh` | `powershell -ExecutionPolicy Bypass -File scripts\build-server.ps1` |
| Core | `scripts/build-server-core.sh` | `powershell -ExecutionPolicy Bypass -File scripts\build-server-core.ps1` |
| Client | `scripts/build-client.sh` | `powershell -ExecutionPolicy Bypass -File scripts\build-client.ps1` |
| App | `scripts/build-mobile-gui.sh` | `powershell -ExecutionPolicy Bypass -File scripts\build-mobile-gui.ps1` |

Every script takes targets, for example `scripts/build-client.sh connector2` or
`scripts\build-server.ps1 -Target windows-x64`; the component READMEs list them.
[`scripts/upload/`](scripts/upload) puts a built client onto a Linux device,
an ESP32 or a Connector 2. On Windows, [`scripts/start-local.ps1`](scripts/start-local.ps1)
starts the built core, server and one client on this PC; `stop-local.ps1` and
`restart-local.ps1` next to it stop or restart them (stop them before a build, which
cannot replace running programs). Their configs and data live in `scripts/.run/`,
which git ignores.

## License

Galdrun is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE.md).
You may use, change and share it for any **noncommercial** purpose: at home, for
study, research or a hobby, and in charities, schools and public institutions.
Selling it, or using it to make money, is not permitted. For commercial use, ask
the author.

This is a *source-available* license, not an OSI-approved open-source license.
The exceptions are listed below: the third-party components, and the files that
patch or extend GPL software, keep their own licenses.

The name "Galdrun" and its logos are not covered by this license; see
[TRADEMARK.md](TRADEMARK.md).

## Third-party components

| Component | Where | License |
|---|---|---|
| [FFmpeg](https://ffmpeg.org/) 9.0.2, prebuilt Windows x64 libraries and headers | [`server-core/third_party/ffmpeg/`](server-core/third_party/ffmpeg/README.md) | LGPL-2.1-or-later ([`LICENSE.txt`](server-core/third_party/ffmpeg/LICENSE.txt)). Built without GPL parts by [`server-core/ffmpeg/`](server-core/ffmpeg/README.md), which rebuilds it from source. |
| [libsoxr](https://sourceforge.net/projects/soxr/) | [`server-core/third_party/soxr/`](server-core/third_party/soxr) | LGPL-2.1-or-later ([`LICENCE`](server-core/third_party/soxr/LICENCE)) |
| [r8brain-free-src](https://github.com/avaneev/r8brain-free-src) | [`server-core/third_party/r8brain/`](server-core/third_party/r8brain) | MIT ([`LICENSE`](server-core/third_party/r8brain/LICENSE)) |
| TI AM335x power-management firmware (`am335x-pm-firmware.elf`) | [`hardware/buildroot/board/connector2/firmware/`](hardware/buildroot/board/connector2/firmware/README.md) | TI Technology and Software Publicly Available License ([`TI-LICENSE.txt`](hardware/buildroot/board/connector2/firmware/TI-LICENSE.txt)) |
| Connector 2 device tree and kernel driver (`am335x-raumfeld-connector2.dts`, `raumfeld-connector2.c`) | [`hardware/buildroot/board/connector2/`](hardware/buildroot/board/connector2) | GPL-2.0-only, as marked in each file |
| Patches for Linux, OpenSSH and BusyBox configuration | [`hardware/buildroot/board/`](hardware/buildroot/board) | The license of the project they patch (Linux: GPL-2.0-only) |

Downloaded at build time and not stored here: Buildroot, the Linux kernel, the
Bootlin toolchain, ESP-IDF and its components, and the NuGet packages of the .NET
projects. Each comes under its own license. The Connector 2 USB image contains
no Teufel software; it is built entirely from these sources.

## Tidal

The Tidal integration is **unofficial**. It is not made, endorsed or supported by
TIDAL, and it uses an interface TIDAL does not offer to third parties. It can stop
working at any time, and TIDAL's terms of service may not permit it.

- You need your own paid TIDAL subscription. Galdrun only plays what your account
  may play, and never encrypted (DRM-protected) streams.
- Galdrun ships **no** TIDAL client ID or secret; you enter your own in the server's
  config file. This project does not say how to obtain one, so please don't ask
  for or post them in issues.
- Tracks are cached only while the server runs, to play them seekable, and
  deleted when it stops. Galdrun is not a downloader.
- You use it at your own risk and are responsible for following TIDAL's terms.

## Trademarks

Galdrun is an independent project. It is not affiliated with, endorsed by or
supported by Lautsprecher Teufel GmbH or TIDAL. Teufel, Raumfeld and TIDAL are
trademarks of their owners and are used here only to name the hardware Galdrun
runs on and the services it works with.
Installing the Galdrun firmware replaces the device's original software; Teufel's
official reinstallation brings it back
([how](releases/client/raumfeld/connector2/README.md#going-back-to-the-raumfeld-software)).
