# ESP32 build (ESP32-A1S, ESP32-S3, ...)

The same client, built with ESP-IDF for ESP32 boards with PSRAM. Everything
above the audio backend is the desktop code; the ESP32-specific parts are
`../src/audio/esp32/` (the I2S backend and the ES8388 driver) and `main/`:
`app_main.cpp` (NVS, the file system, Wi-Fi, then the client's own `main`) and
the settings page (`WebUi`, `CaptiveDns`, `WifiSettings`).

Not yet run on hardware -- see the roadmap's phase 22.

## Boards

Each board is a file in `boards/`, and each build is for one board:

| File | Contents |
| --- | --- |
| `<board>.defaults` | sdkconfig lines on top of `sdkconfig.defaults`: `CONFIG_IDF_TARGET`, the PSRAM mode, and the `CONFIG_MULTIROOM_*` pins (setup button, codec, external DAC; see `main/Kconfig.projbuild`). |
| `<board>.inputs.json` | Optional. The board's buttons and encoders, as the `inputs` list of `client.json` (`../README.md`). Compiled in; the first start writes them to `client.json` (below). |

| Board | Chip | |
| --- | --- | --- |
| `esp32-a1s` | ESP32 | AI-Thinker ESP32 Audio Kit v2.2: ES8388, KEY1 and KEY3-KEY6 (KEY2, GPIO 13, shares its line with the SD card's DIP switches). An external DAC's default pins are KEY4-KEY6's. |
| `esp32-s3-devkitc` | ESP32-S3 | DevKitC-1, N8R8/N16R8 (octal PSRAM), a codec or DAC on free pins; BOOT is the setup button and plays/pauses. |

A new board is a copy of the closest `.defaults` (and `.inputs.json`) under a
new name, with its pins. Mind the chip: on the ESP32, MCLK can only be GPIO 0,
1 or 3 and GPIO 34-39 have no pull resistors (`"pull": "none"`); on the
ESP32-S3, 22-25 do not exist, 26-32 are flash and PSRAM and 33-37 octal PSRAM,
and MCLK can be any GPIO. A module with quad PSRAM (N8R2, N4R2) needs
`CONFIG_SPIRAM_MODE_QUAD=y`, octal (N8R8, N16R8) `CONFIG_SPIRAM_MODE_OCT=y`; the
wrong one stops at boot.

## Build and flash

ESP-IDF 5.5.1. From the repository root, which installs it when missing
(on Windows inside WSL Ubuntu):

```
scripts/build-client.sh esp32                                    # Linux; or esp32-a1s, esp32-s3-devkitc
powershell -ExecutionPolicy Bypass -File scripts\build-client.ps1 -Target esp32   # Windows
```

By hand, in an ESP-IDF shell (the board defaults to `esp32-a1s`):

```
cd client/esp32
idf.py -B build-esp32-a1s -DMULTIROOM_BOARD=esp32-a1s build
idf.py -B build-esp32-a1s -p COM5 flash monitor
```

Each board keeps its own build directory and `sdkconfig.<board>`, so boards on
different chips stay configured side by side. The board file wins: when it (or
`sdkconfig.defaults`) is newer than `sdkconfig.<board>`, that is generated
again. `idf.py -B build-<board> menuconfig` still works for a try; put what is
to stay into the board file.

`joltwallet/littlefs` is fetched by the component manager on the first build.
Each build also copies the flash images to `releases/client/<board>`, which can
be flashed from there with esptool alone (see the README it contains). The
images of one chip do not boot on another.

## Wi-Fi and the settings page

A board without `/data/wifi.json` starts in setup mode: it opens an access
point `Galdrun-Setup-XXXXXX` (open, no password). Join it with a phone and the
settings page opens by itself (captive portal); otherwise browse to
`http://192.168.4.1/`. Pick the network, enter its password, optionally the
server address, output and I2S clock, and press *Save and restart*.

The page also names the room (`clientId`, how the server lists it; empty: a
generated id) and the device itself (`deviceName`, its DHCP host name on the
network, kept in `wifi.json`; empty: the `esp32-<mac>` default). The device
name is 1 to 31 letters, digits and hyphens; whether typing it reaches the
board depends on the router forwarding DHCP names to its DNS.

*Access point* (`bssid` in `wifi.json`) picks which access point of the network
the board joins. *Automatic* (empty) scans every channel and takes the one with
the strongest signal; a scan on the page lists the network's access points with
channel and signal and marks the one in use. A fixed access point is useful
when one of them cannot carry the stream (see the roadmap's phase 25 notes); if
it is switched off, the board cannot join and opens setup mode as below.

Setup mode also starts when

- the board's setup button (KEY1 on the `esp32-a1s`, BOOT on the
  `esp32-s3-devkitc`) is held during reset, or
- the saved network cannot be joined for 60 s. The board keeps retrying it
  every 15 s with the access point up, and continues normally once it gets in.

In normal operation the same page is served on the board's LAN address (its
host name is `esp32-<last three MAC bytes>`), with the running client's status
on top, read from its control API. It is the board's one page: a browser
opening the control port (`http://<board>:4720/`) is sent here. It never shows
the stored password; an empty password field keeps it. Like the control API,
the page has no authentication, though it refuses requests by a foreign host
name (DNS rebinding) and settings posts without a JSON content type. `wifi.json`
holds the password in clear text -- flash encryption is what would protect it. Scanning from the page takes the radio off
its channel for a few seconds, which a playing room hears.

## Other settings

Compile time: the board file (above) -- the GPIOs and the setup button.

A board without `/data/client.json` gets one before anything else runs, holding
its buttons from `<board>.inputs.json` and the defaults for the rest. From then
on the file is what counts: the server rebinds a button there
(`SET_INPUT_BINDINGS`), a button taken out of it stays out, and a new firmware
does not bring the board's list back. Deleting `client.json` does.

Run time (`/data/client.json`, the usual file, see `../README.md`; the page
writes the keys below):

| Key | Meaning |
| --- | --- |
| `clientId` | The room's name at the server (see `../README.md`); empty: a generated id. |
| `audioDeviceId` | `"es8388"` (on-board codec), `"i2s"` (external DAC), or `""`: the codec when it answers on I2C, else the external DAC. The server can switch it (`SET_AUDIO_DEVICE`). |
| `i2sClockMode` | `"master"`: the ESP32 drives MCLK/BCLK/WS from its 160 MHz PLL (see *Clock*). `"slave"`: an external clock drives them. |
| `i2sSlaveSampleRate` | Slave mode only: the rate the external clock runs at, and the only rate offered to the server. |
| `i2sExternalFormats` | What the external DAC plays: rate and `int16`/`int32` pairs, ticked in a table on the page; every `int32` pair also offers 24-in-32. Default: 44.1 to 192 kHz in both. |
| `bufferMs` | The size of the stream's buffer (1000 to 11000 ms, default 2500). The server can change it (`SET_BUFFER`); it also decides which formats fit in memory, below. |

The client scans on every start, so a changed clock mode reaches the server
with the next `AUDIO_CAPABILITIES`.

## What each device offers

| Device | Master | Slave |
| --- | --- | --- |
| `es8388` | 8 / 11.025 / 12 / 16 / 22.05 / 24 / 32 / 44.1 / 48 kHz (single speed, MCLK 256 fs) and 88.2 / 96 kHz (double speed, MCLK 128 fs), 16, 24-in-32 and 32 bit | `i2sSlaveSampleRate` if it is one of the single speed rates, same bit depths |
| `i2s` | the pairs in `i2sExternalFormats`, plus 24-in-32 wherever `int32` is declared | those of them at `i2sSlaveSampleRate` |

Always stereo. 24-in-32 (`s24le`) is left-justified as it leaves the ring
buffer and goes out as an ordinary 32-bit slot with a zero low byte, so a
24-bit source reaches the DAC bitperfect. Packed 24-bit (`s24le3`) is not
offered; the server repacks it to `s24le`, which stays bitperfect (MATP
section 2). 88.2 and 96 kHz on the ES8388 run its double speed mode at MCLK
128 fs, the same 11.29 / 12.29 MHz as 44.1 and 48 kHz, and only in master
mode. `CONFIG_MULTIROOM_CODEC_DOUBLE_SPEED` (menuconfig, *On-board codec*) turns
them off again.

Then memory: the server is only offered the formats whose ring buffer --
`bufferMs`, rounded up to a power of two frames -- fits the PSRAM less 1 MB,
about 3 MB of the 4 MB an ESP32 can address (an 8 MB module included, whose
upper half is only reachable by bank switching). An ESP32-S3 maps all of its
PSRAM, so an 8 MB module has about 7 MB for the buffer. On an ESP32, at the
default 2500 ms that leaves out 176.4 and 192 kHz at 32 bit (4 MB each); a larger buffer leaves out
more, and a `SET_BUFFER` that would leave the
device nothing, or not fit the format playing now, is refused.

In slave mode with the codec, the ES8388 is the clock master and derives BCLK
and WS from the MCLK the ESP32 still supplies (160 MHz PLL). The driver makes that
MCLK 8 BCLK -- 256 fs with 16-bit slots, 512 fs with 32-bit ones -- so for
24-in-32 and 32 bit the codec halves it (MASTERMODE bit 6). With the external
DAC, the external clock must be running before playback starts: a bus without
a clock makes the I2S write time out, which the client treats as a lost device
and reopens on its usual backoff.

An AC101 codec (older A1S modules) is detected and reported, not driven.

## Clock

In master mode MCLK (256 fs) comes from the 160 MHz PLL through the I2S
fractional divider, on the ESP32 and the S3 alike. Not the ESP32's APLL: the
ES8388 has no PLL of its own and the APLL's jitter was audible on an A1S.
Every rate the external DAC declares is reachable, but the
divider is exact only at 48 kHz; the others are off by a few ppm (44.1, 88.2)
up to about 30 to 65 ppm (96, 176.4, 192 kHz), and a fractional divide moves
MCLK edges by one 6.25 ns PLL cycle. The client measures the rate like any
other clock and the server corrects it, so the room stays in sync. For a
high-end DAC, use slave mode with the DAC's own oscillator as the clock.

The device position is counted in the I2S DMA interrupt, one descriptor at a
time, and timestamped there with `esp_timer` -- the base `monotonicNowNs()`
reads. Reported as clock quality `driver`. A starved DMA sends a descriptor of
silence; the backend renders and drops the same amount of audio so what it
plays stays on the device's frame axis, and counts it as an underrun.
