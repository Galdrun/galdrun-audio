# Galdrun server for Raspberry Pi

A complete SD-card image that makes a Raspberry Pi the Galdrun server: the
server (library, streaming services, queues, web interface), the core (decoding
and streaming to every room) and a Galdrun client, all started at boot. It is
the Galdrun Pi appliance image with the server added, so network, Wi-Fi, SSH and
audio setup work exactly as described in the
[Raspberry Pi appliance guide](https://github.com/Galdrun/galdrun-audio/blob/main/hardware/buildroot/board/rpi/README.md).

| Raspberry Pi | Image |
|---|---|
| Pi 3 Model B, B+ (1 GB RAM) | `galdrun-server-rpi3-sdcard.img.gz` (32-bit ARMv7) |
| Pi 4 Model B, all RAM variants | `galdrun-server-rpi4-sdcard.img.gz` (64-bit) |
| Pi 5 Model B, all RAM variants | `galdrun-server-rpi5-sdcard.img.gz` (64-bit) |

A Pi 3 with 1 GB is the minimum; a Pi 4 or 5 with 2 GB or more is recommended
for large libraries and many rooms. Pi 2, Pi 3 A+ and Zero 2 W boot the `rpi3`
image but are too slow or have too little memory (512 MB) to run the server.

## Install

1. Check `sha256sum -c SHA256SUMS`.
2. Write the image to a **4 GB or larger** SD card with Raspberry Pi Imager's
   **Use custom** option (imaging erases the card).
3. Connect Ethernet (recommended for a server) and boot. The device is
   `galdrun-server-rpi<N>` in DHCP.

| Address | What |
|---|---|
| `http://<address>:4723/` | The Galdrun web interface: library, rooms, queues |
| `http://<address>:4720/` | This device's settings: network, Wi-Fi, SSH, its own audio output |

Without Ethernet or Wi-Fi, the device opens the **Galdrun-Pi** setup hotspot
(password **galdrun-pi**) at `http://192.168.77.1:4720/`, as the client images do.

The Pi also plays as a room when an audio output is selected on its settings
page. To use it as a server only, set **In the system** to **Deactivated** on
that page: its client then stays out of the server's room list.

## Files on the device

| Path | Holds |
|---|---|
| `/etc/galdrun/server.json`, `/etc/galdrun/core.json` | Server and core settings, created on first boot |
| `/var/lib/galdrun/server/` | Library database, playback state, caches |
| `/run/log/` | Logs of server, core and client (RAM, lost on reboot); also on the web interface's log page |

The root partition is 3 GB; put music on a network share or a USB drive and add
it in the web interface. The web interface and the API (4725) listen on the
network without a login: use the image on a trusted network only.

Licences of everything in the image are in `licenses/`, the server's and the
core's in `licenses/galdrun-server` and `licenses/galdrun-core`.
