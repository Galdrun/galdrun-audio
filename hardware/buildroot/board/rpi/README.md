# Raspberry Pi audio appliance

Build from the repository root:

```sh
scripts/build-client.sh rpi1 rpi3 rpi4 rpi5
```

On Windows:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-client.ps1 -Target rpi1
powershell -ExecutionPolicy Bypass -File scripts/build-client.ps1 -Target rpi3
powershell -ExecutionPolicy Bypass -File scripts/build-client.ps1 -Target rpi4
powershell -ExecutionPolicy Bypass -File scripts/build-client.ps1 -Target rpi5
```

`rpi1` uses ARMv6 hard-float/VFP userspace; `rpi3` uses ARMv7 hard-float/VFPv3-D16.
`rpi4` targets Pi 4 Model B and `rpi5` targets Pi 5, with one image per family
across RAM variants. Both use the ARM64 `linux-armv8` client and 4 KiB kernel
pages. Build/image validation does not qualify physical boards or HATs.
`rpi1` targets Pi 1 Models A/B (26-pin), A+/B+ (40-pin), Zero, Zero W and WH.
One `rpi3` image targets Pi 2 B (rev 1.0/1.1/1.2), Pi 3 B/B+/A+ and Zero 2 W
(all 40-pin, populated or unpopulated). Pi 2 and Zero 2 W use the same
`galdrun-rpi3-sdcard.img.gz` download and default `galdrun-rpi3` hostname as Pi 3;
the older Zero models and A/A+ use `galdrun-rpi1-sdcard.img.gz` / `galdrun-rpi1`.
The boot firmware selects
`bcm2710-rpi-3-b.dtb` for B and `bcm2710-rpi-3-b-plus.dtb` for both B+ and A+;
it adjusts the latter for A+'s RAM and peripherals. This is the firmware's
[documented device-tree selection](https://www.raspberrypi.com/documentation/computers/configuration.html#device-trees-overlays-and-parameters),
not a separately maintained A+ device tree.
The image also includes `bcm2710-rpi-zero-2-w.dtb` and the kernel's identical
`bcm2710-rpi-zero-2.dtb` alias for Zero 2 W.
For Pi 2, both `bcm2709-rpi-2-b.dtb` (BCM2836) and `bcm2710-rpi-2-b.dtb`
(BCM2837) are included. Pi 1 A/A+ use the firmware-customised B/B+ DTBs;
Zero and W/WH use `bcm2708-rpi-zero.dtb` and `bcm2708-rpi-zero-w.dtb`.
All images use Buildroot 2025.02.18 and Raspberry Pi Linux 6.12.112, commit
`9cf0f5c001b790d53689b4aadca873db169cbc44`. Bundled Pi 1–4 boot firmware is
Buildroot's pinned `5476720d52cf579dc1627715262b30ba1242525e`; Pi 5 uses its
board EEPROM (the manifest's boot firmware commit is null). Hashes are checked by Buildroot;
the Pi-only ALSA time32 timestamp patch preserves the driver's system timestamp.
No AM335x/Connector hardware patches or firmware are used.

Images, SHA256SUMS, manifests and licenses are exported to
`releases/client/raspberrypi/rpi{1,3,4,5}/`. These generated artifacts,
downloads, logs, private keys and local test data are ignored by Git. Publish
binaries as release assets. Source licenses and Buildroot's legal-info manifest
travel with each image; boot and wireless firmware contain redistributable
binary blobs. Corresponding downloaded source archives and legal-info sources
remain in the local Buildroot cache for source distribution.

## Server images

`scripts/build-server.sh rpi3 rpi4 rpi5` (Windows: `scripts/build-server.ps1 -Target rpi3`, ...)
turns each finished client image into a server image: the same system plus the
server (web interface on 4723) and the core, started by `S90galdrun-core` and
`S91galdrun-server`, with a 3 GB root. `scripts/build-server-image.sh` assembles it
from the Buildroot output's `rootfs.tar` and `boot.vfat` with its own host tools; it
needs no second Buildroot build. Pi 3 or newer only; results go to
`releases/server/raspberrypi/rpi{3,4,5}/`. Their files are in `board/rpi/server/`,
including the README that ships with them.

## Install and recover

1. Keep the existing working card untouched. Back up its identity/settings first.
2. Check `sha256sum -c SHA256SUMS`. Decompress the matching
   `galdrun-rpi<N>-sdcard.img.gz` for the board's image family above.
3. Write the image to a **spare** SD/microSD card with Raspberry Pi Imager's
   **Use custom** option. Check the selected device; imaging erases that card.
   A 1 GB or larger card is sufficient. Pi 1 Models A/B may require full-size SD.
4. Connect Ethernet where available and a reliable supply. Boot, find
   `galdrun-rpi<N>` in DHCP, and open `http://<address>:4720/`.
   Pi 3 A+, Zero 2 W and Zero W/WH have no onboard Ethernet: join the
   **Galdrun-Pi** setup hotspot and configure station Wi-Fi as described below.
   Pi 1 A/A+ and the original Zero have neither Ethernet nor onboard Wi-Fi:
   attach a supported USB Ethernet or Wi-Fi adapter. AP fallback needs an
   AP-capable radio. Pi 2 has Ethernet and needs a USB adapter for Wi-Fi.
5. Select the audio device explicitly. No output is selected automatically, and
   a missing selected output does not redirect sound to another speaker.

The SD has a 64 MiB FAT boot partition and 512 MiB writable ext4 root partition.
Settings and identity persist in `/etc/galdrun/`; appliance settings are in
`/etc/galdrun-*.conf` and `/etc/galdrun-dac*`. Logs stay in a capped RAM
filesystem under `/run/log`. The boot partition is mounted read-only normally.
Serial boot diagnostics use 3.3 V UART GPIO14/15, 115200 8N1 on Pi 1–4.
Pi 5 uses `ttyAMA10` on its dedicated three-pin debug UART connector.
Bluetooth is disabled
on Pi 3, Pi 4, Zero 2 W and Zero W/WH, keeping PL011 stable when CPU frequency
changes. Model filters leave Pi 2, A/A+, B/B+ and the original Zero untouched.
Never connect UART VCC.

Pi 4 boots `Image` with `start4.elf`/`fixup4.dat` and the BCM2711 device tree.
Pi 5 boots `Image` through its existing EEPROM bootloader, without start/fixup
files or automatic EEPROM updates. Its image includes BCM2712 C1/D0/D device
trees. Explicit `vc4-kms-v3d-pi4`/`vc4-kms-v3d-pi5` overlays enable HDMI audio.
Pi 5 has no analogue headphone output; select USB, HDMI or a compatible I2S HAT.

Recover by reinstalling the original card. For a bad audio profile, shut down,
mount the spare card's FAT partition on another computer and restore
`galdrun-audio.previous.txt` as `galdrun-audio.txt`; selecting USB uses an empty
audio include. Leave `config.txt` and the kernel/DTB names unchanged.

## Network and SSH

Ethernet is preferred. Set DHCP/static addresses, SSID/password, country and a
Wi-Fi radio in the web GUI. A selected radio is stored by MAC; if unplugged,
the supervisor uses the default available radio, then the first by interface
name, and returns to the preferred radio when reattached.
Automatic Ethernet speed advertises all speeds supported by the NIC, including
Gigabit on Pi 3 B+; its USB 2.0 connection still limits actual throughput.
The existing 10/100 Mbps selectors limit advertisement only when selected.
Boards without Ethernet skip Ethernet setup and use the same Wi-Fi supervisor.

Without an Ethernet lease or usable station connection, an AP-capable radio
opens **Galdrun-Pi**, password **galdrun-pi**, at **192.168.77.1**. Open the
captive settings page or `http://192.168.77.1:4720/`. A saved station is retried
after the setup interval.
The AP is local setup access and does not forward Internet traffic.
With a station-only USB Wi-Fi adapter, use Ethernet for setup (a USB Ethernet
adapter on A/A+/Zero) or preconfigure the station credentials on the card.
The images include ASIX (`asix`, `ax88179_178a`), Realtek (`r8152`) and
SMSC95xx USB Ethernet drivers. Connect through a hub when a single USB port
must serve both a network adapter and USB audio.

SSH starts **off**, with a locked root account and no developer keys. As on
Connector 2, set a root password and enable SSH in the web GUI. Each device
generates its own host keys. No test-device password belongs in an image.

For a client-only update on an installed Galdrun Buildroot Pi, build the matching
`linux-armv6` / `linux-armv7` / `linux-armv8` release, enable SSH, then use
`scripts/upload/linux.sh --ip <address> --user root` or
`scripts/upload/linux.ps1 -IP <address> -User root`. The uploader detects BusyBox
and uses its existing init service; it preserves identity/settings and keeps the
previous binary. Configure the server in the device GUI. It updates neither the
kernel nor the SD partitions. Generic DietPi/systemd installs keep their usual
service installation path.

For backup, enable SSH, stop the client and archive `/etc/galdrun/`,
`/etc/galdrun-board.json`, `/etc/galdrun-network.conf`,
`/etc/galdrun-cpu.conf`, `/etc/galdrun-dac*`, `/etc/galdrun-ssh.conf`,
`/etc/shadow` and `/etc/ssh/ssh_host_*` if present. Store this archive privately:
it includes credentials. Restore identity/network/SSH settings only to the same
device, keeping board defaults from the new image. Restart the client after
backup or restore. Do not clone one device's ID or SSH host keys to other Pis.

## Audio cards

The web GUI's Audio board selector offers the maintained catalogue in
`audio-profiles.json`. Applying a changed profile automatically restarts the
device. Before the client starts, the boot service discards cached audio devices
when the profile differs from the previous boot, so the new board is scanned
automatically. Select its ALSA output afterward. Unchanged boots retain the cache.
The Restart button also reboots the device and reconnects the settings page;
generic desktop installs hide it. The selector retains the previous boot include and rejects unknown
profiles, missing overlays, custom conflicting overlays and 26-pin Pi 1 boards.
Modern HAT profiles target all listed 40-pin models: Pi 1 A+/B+, Pi 2 B,
Pi 3 B/B+/A+, Pi 4 B, Pi 5 and all Zero variants. Pi 1 A/B 26-pin/P5 wiring remains a
separate hardware validation task and is rejected by the profile selector.

The Zero family has no onboard analogue headphone output. Use a USB DAC, an I2S HAT
or HDMI audio and select the output in the GUI. For USB audio, use a micro-USB
**OTG adapter** on the port labelled **USB**, with power connected to **PWR IN**.
The built-in `dwc_otg` controller uses the adapter's OTG ID connection for
host mode; no extra USB gadget/host overlay or boot edit is required. Use a
powered hub when the attached devices need more power. GPIO and HAT use
require a fitted 40-pin header; the image is the same with or without one.
See the official [Zero ports](https://www.raspberrypi.com/documentation/computers/zero.html)
and [OTG adapter instructions](https://www.raspberrypi.com/documentation/computers/getting-started.html#raspberry-pi-zero-usb-devices).

Catalogue coverage includes HiFiBerry DAC/DAC+/Pro/HD/ADC/DSP/Digi/Amps,
Raspberry Pi/IQaudio DAC/Codec/Digi, JustBoom, Allo Boss/Boss2/Piano/Katana/DigiOne,
AudioInjector Stereo/Isolated, Fe-Pi, Audiophonics I-Sabre Q2M and INNO-MAKER
HiFi DAC / DAC Mini HAT (PCM5122). The INNO-MAKER profile uses
`allo-boss-dac-pcm512x-audio` as specified in the
[manufacturer's manual](https://github.com/INNO-MAKER/HIFI-DAC-Hat-For-Raspberry-Pi/blob/main/HIFI%20DAC%20User%20ManualV1.7.pdf);
ALSA identifies it as BossDAC. Its driver/overlay are included in all images.
An attached HAT on the Pi 1 B+ enumerated as BossDAC and accepted client PCM
playback; audible output and long-running playback remain hardware checks.
Each entry names its exact overlay, machine module, pins, fixed I2C addresses,
clock policy and pinned upstream source. Driver-reported dependencies are
installed together. Overlay defaults retain safe gain caps; the GUI does not
allow arbitrary I2C register writes or gain-boost overlay parameters.

DAC settings enumerate supported writable boolean, integer and enum mixer
controls on the selected ALSA card. Volatile and clock/rate controls stay out
of persistence. Volume controls require driver dB metadata and are capped at
0 dB; raw volumes without a known dB scale stay with the room's volume path.
Settings and the initial mixer snapshot are stored separately per ALSA card ID.
Reset restores that first-use snapshot, preserving room-owned volume/mute while
playing; Connector 2 retains its original CS4271 list and reset values.

USB Audio Class 1/2/3 devices use `snd-usb-audio`, including common CM108/CM119,
PCM270x/PCM29xx, CM6631A, XMOS and Amanero USB interfaces when they expose the
standard USB audio protocol. No USB DAC model is certified by this build alone.
For identical DACs lacking unique serial identities, use one per appliance;
ALSA's enumeration suffixes cannot establish which physical DAC is intended.

DSP boards need an existing working DSP program. The DAC+DSP profile supplies
I2S playback only; generic SigmaDSP kernel drivers are included, but arbitrary
ADAU1701/1761/1781 boards still need their own topology and firmware. There is
no DSP coefficient/program uploader. Audio capture is outside this playback client.

All profiles use server resampling for drift correction. Nominal rate selection,
CPU MHz and physical sample-clock trim are separate. No Pi profile advertises
hardware clock trim or network wake from system sleep. Standby leaves Linux and
networking awake; deep sleep is disabled. CPU MHz uses the board's actual sysfs
frequency list without a Connector-specific 300 MHz default.

## Buttons and rotary encoders

The shared board wiring file is [`inputs.json`](inputs.json). All images use
it to generate `client-default-40pin.json` and `client-default-26pin.json` under
`/opt/galdrun-client/`. `S94galdrun-inputs` selects the layout from the device-tree
model **only when `/etc/galdrun/client.json` does not exist**. All supported
40-pin models get two encoders with push switches plus four separate buttons.
Pi 1 Models A/B (26-pin) get two encoders with push switches plus one separate
button. Unknown models retain the no-input fallback. Existing settings are
never replaced or automatically migrated.

Numbers in JSON are **BCM GPIO numbers**, also the offsets on the header's GPIO controller;
the physical header positions below are wiring references, not config values.
Pi 1/3 images use `/dev/gpiochip0`. Pi 4/5 board files override this with
`/dev/galdrun-gpio`: `S42galdrun-gpio` discovers `pinctrl-bcm2711` or
`pinctrl-rp1` respectively at each boot. The character-device number can change;
missing or ambiguous controllers fail provisioning rather than selecting another chip.

| Control | BCM GPIO | Physical header pin | Default action |
| --- | --- | --- | --- |
| Volume encoder A / B | 7 / 8 | 26 / 24 | Clockwise louder, counterclockwise quieter; 2% per detent |
| Volume encoder push | 11 | 23 | Mute/unmute |
| Transport encoder A / B | 9 / 10 | 21 / 19 | Clockwise next, counterclockwise previous |
| Transport encoder push | 23 | 16 | Play/pause; hold to stop |
| Play/pause button | 24 | 18 | Play/pause |
| Previous button (40-pin only) | 12 | 32 | Previous |
| Next button (40-pin only) | 13 | 33 | Next |
| Standby button (40-pin only) | 26 | 37 | Audio standby/wake; Linux/network stay awake |

Connect each normally open button between its GPIO and GND (e.g. physical
pin 6). Connect each encoder's common to GND and A/B contacts to their GPIOs;
its push switch is a separate button. Inputs use internal pull-ups, active-low
buttons, and four quadrature transitions per detent. Unconnected controls stay
idle. Use 3.3 V logic only, never 5 V. Swap an encoder's A/B wires to reverse
its direction, or adjust `pinA`/`pinB` and `stepsPerDetent` in `client.json`
with the client stopped. Bindings can be changed through the existing input
settings UI. Buttons support click, double-click and long-press gestures.

The existing Linux GPIO v2 backend requests both rising and falling edge
events: the kernel uses GPIO interrupts and the input thread waits on file
descriptors. It does not periodically sample the pins. Button debounce and
encoder quadrature decoding run outside audio rendering; no libgpiod or
additional kernel overlay is needed.

The board file reserves GPIO0/1 for HAT identification, GPIO2/3 for audio I2C,
GPIO14/15 for the serial console, GPIO18-21 for I2S, and GPIO4/5/6/16/17/22/25
for auxiliary functions across the audio profile catalogue. GPIO27 (physical
13) remains spare on 40-pin boards. SPI0 remains disabled: GPIO7-11 are used
by these controls, so enabling SPI0 conflicts with this wiring. On 26-pin
Model B, physical 13 changes GPIO number between revisions and is deliberately
unused. With USB/onboard audio, additional pins may be free, but keeping the
same reservations lets the wiring survive audio profile changes.

For an existing installation, stop the client, back up `client.json`, then
merge only `inputs` and `inputGpioChip` from the matching generated default
into it; retain identity, audio, network and other settings. Keep `inputs: []`
to disable GPIO controls. Custom wiring belongs in this persistent config;
edit the board file to change defaults for future images. The installed
`/usr/share/galdrun-inputs.json` is a reference copy, not a live config.

This layout avoids the pins listed by the maintained audio profiles; an
attached HAT can have additional switches, displays or undocumented GPIO uses.
Check its schematic before wiring. Host tests cover reservations, header
selection and settings preservation; physical switch/encoder behavior and
electrical compatibility still require hardware checks. GPIO requests fail
without taking over a line already owned by a kernel driver.

References: [Raspberry Pi GPIO, voltage and SPI documentation](https://www.raspberrypi.com/documentation/computers/raspberry-pi.html),
[Linux GPIO v2 line requests](https://docs.kernel.org/userspace-api/gpio/gpio-v2-get-line-ioctl.html).

## USB Wi-Fi coverage

Identify chipset and USB VID:PID with `lsusb`; retail names can change chipsets.
Drivers and firmware are included for these kernel-supported families:

| Family | Examples / kernel module |
| --- | --- |
| Atheros AR9271, AR7010 | `ath9k_htc`; TL-WN722N **v1** (`0cf3:9271`), Netgear WNA1100 (`0846:9030`) |
| Ralink RT3070/RT5370/RT5572 | `rt2800usb` |
| MediaTek MT7601U | `mt7601u` |
| MediaTek MT7610U/MT7612U | `mt76x0u` / `mt76x2u` |
| MediaTek MT7921U | `mt7921u` |
| Realtek RTL8188/8192 USB variants | `rtl8xxxu`; check the kernel's VID:PID table |
| Realtek RTL8822BU/CU, RTL8821CU, RTL8723DU | `rtw88` USB modules |
| Pi 3 Model B internal BCM43430 | `brcmfmac` SDIO plus Pi-specific firmware/NVRAM |
| Pi 3 Models B+/A+ internal CYW43455 | `brcmfmac` SDIO plus model-specific firmware/NVRAM/CLM |
| Pi 4 B / Pi 5 internal CYW43455 | `brcmfmac` SDIO plus `4-model-b` / `5-model-b` firmware/NVRAM/CLM |
| Zero W/WH internal BCM43430 | `brcmfmac` SDIO plus `model-zero-w` firmware/NVRAM/CLM |
| Zero 2 W internal CYW43436S / CYW43436 | `brcmfmac` SDIO plus both firmware/NVRAM variants; separate CLM for CYW43436 |

The pinned driver's firmware lookup names for Zero 2 W are
`brcmfmac43430-sdio.raspberrypi,model-zero-2-w` (43436S) and
`brcmfmac43430b0-sdio.raspberrypi,model-zero-2-w` (43436). The included Pi
firmware package supplies these aliases and their targets. Both variants are
required by image validation; the 43436 variant also needs its `.clm_blob`.
Zero 2 W's onboard Wi-Fi supports 2.4 GHz; configure a matching access point.
Zero W/WH also use 2.4 GHz; WH uses the same W model identity and firmware,
with a factory-fitted GPIO header. Original Zero, Pi 1 A/A+/B/B+ and Pi 2 B
have no onboard Wi-Fi; the USB adapter families above apply to them.

Sources: [Linux Wireless driver list](https://wireless.docs.kernel.org/en/latest/en/users/drivers.html),
[Atheros devices](https://wireless.docs.kernel.org/en/latest/en/users/drivers/ath9k_htc/devices.html),
[pinned Pi overlays](https://github.com/raspberrypi/linux/blob/9cf0f5c001b790d53689b4aadca873db169cbc44/arch/arm/boot/dts/overlays/README),
[HiFiBerry configuration](https://www.hifiberry.com/docs/software/configuring-linux-3-18-x/),
[Raspberry Pi audio HATs](https://www.raspberrypi.com/documentation/accessories/audio.html).

These are driver coverage, not per-adapter station/AP certification. AP support
is probed per selected radio. Validate wrong credentials, unplug/replug, two
radios and continuous audio with the actual adapter and supply before deployment.

## Validation status

Host validation checks the SD partition table, FAT boot contents, ext4 signature,
DTBs/overlays, module dependency metadata, required firmware, safe defaults and
absence of Connector services/credentials. QEMU runs the bundled client against
the actual rootfs on the target CPU model. This cannot prove physical boot,
audio timing, radio reliability or DSP programming.
For Pi 3 B+/A+, image validation requires the shared B+ DTB, the B+'s LAN78xx
Ethernet driver and each model's BCM43455 firmware, NVRAM and CLM files.
Host regression cases cover GPIO provisioning, all audio profiles, and A+'s
Wi-Fi station/setup-AP paths without any Ethernet interface. Physical cold
boot, Wi-Fi on both bands, Ethernet negotiation and audio/GPIO operation on
actual B+/A+ boards remain separate acceptance checks; the historical tests
below used Pi 3 B and Pi 1 B+, not Pi 3 B+/A+.
Zero 2 W validation additionally requires both DTB names, the built-in OTG/host
controller and the firmware aliases for both Wi-Fi chip variants. Its model
receives the full 40-pin controls and audio profile selection in host tests.
Boot, Wi-Fi, USB DAC enumeration, real I2S audio and GPIO on actual Zero 2 W
hardware still need physical acceptance testing.
Pi 2 validation requires both CPU-revision DTBs and the SMSC95xx Ethernet
driver. The ARMv6 image requires both older Zero DTBs and working W/WH
firmware/NVRAM/CLM aliases. Bluetooth model filters preserve PL011 on W/WH
without applying an incompatible overlay to Pi 2 or non-Bluetooth boards.
Host tests cover the 26-/40-pin model matrix, all allowed audio profiles and
network setup without Ethernet. Physical acceptance for the newly added
Pi 2 revisions, A/A+ and older Zero variants is still pending, including the
256 MB A/A+ configurations, actual USB radios and audio/GPIO operation.
Pi 4/5 validation additionally requires ARM64 boot mode, 4 KiB pages,
PCIe/USB/SD drivers, board-specific Ethernet, BCM43455 firmware aliases, KMS
audio and stable header GPIO discovery. Pi 5 also requires the RP1 clock,
pinctrl and DesignWare I2S drivers. Physical cold boot, GPIO interrupts,
Wi-Fi/AP, HDMI/USB/I2S audio and the 24-hour grouped soak remain open for
both new families and their RAM variants.

Verified on 2026-10-25: `scripts/build-client.sh rpi4 rpi5` completes both
ARM64 images. All 32 audio profiles link with KMS on the Pi 4 DTB and all three
Pi 5 DTBs. Exported archives pass checksum, FAT/ext4, manifest and installed-file
checks; client, ethtool and captive portal run from extracted rootfs files under
Cortex-A72/A76 QEMU. GPIO/profile/update and shared-service host regressions pass,
including changed GPIO device numbers, missing/ambiguous controllers and required
Wi-Fi aliases. No Pi 4/5 hardware was flashed or physically tested.

Verified on 2026-10-24: both images build and pass image/QEMU ABI validation;
the Linux client suite, firmware regressions and desktop/mobile browser checks
pass. On the Pi 3, an isolated copy of the actual ARMv6 rootfs starts its BusyBox
client/log services, provisions real SSH through the web API, accepts a root
login with the newly set password, disables SSH again and preserves client ID
across service restart. Explicit audio selection, card-bound control validation
and returning to silence were also checked with its real headphone device.
The test used DietPi's existing kernel and preserved its boot configuration;
it does not certify a fresh Linux 6.12.112 boot or any unattached peripheral.

A subsequent SSH installation of the Pi 3 image onto a spare USB drive passed
filesystem checks and installed kernel/client hash verification. For this USB
test, the kernel root argument and boot mount were changed from the SD devices
to `/dev/sda2` and `/dev/sda1`. The original SD boot files were backed up and
moved aside for the documented `bootcode.bin`-only USB boot method; DietPi's
root filesystem remains intact. After a cold power cycle it booted the actual
Linux 6.12.112 kernel and Buildroot userspace at `10.0.0.27`. Factory SSH was
off with a locked root account. Web password provisioning, actual SSH login,
disable/re-enable, client-only SSH update with retained identity, desktop/mobile
GUI checks and real FAT profile save/rollback passed. Boot backups and
device-specific recovery notes remain in ignored local storage, outside the
source/release images.

The onboard BCM43430 loaded its firmware, reported AP capability and scanned
real access points. With the supervisor's Ethernet device temporarily absent
and an unreachable station configured, the real radio opened `Galdrun-Pi`;
hostapd, DHCP and the captive portal started. Management Ethernet stayed
available separately during this test, and all network settings were restored.
External hotspot-client DHCP and successful station association remain untested.

This boot exposed that the onboard VCHIQ audio module did not coldplug. It
works when explicitly loaded; the images now build it into the kernel so it
registers before the client's initial scan. The corrected Pi 3 kernel was
installed with matching modules and rebooted successfully: headphones registered
at 3.8 seconds without manual module loading, and identity/SSH/profile settings
survived. ALSA discovery, selected headphone
controls, invalid-write validation and clearing output back to silence passed.

Initial Pi 3 inspection at `10.0.0.27`: Raspberry Pi 3 Model B Rev 1.2 with DietPi,
only onboard headphones and Ethernet, internal Wi-Fi disabled, no USB audio/WLAN
peripherals. It reports active undervoltage/throttling (`0x50005`): repair power
before performance or audio qualification. The DietPi card is kept for recovery.
The Pi 1 B+ Rev 1.2 also booted from physical SD with the released partition
layout: Linux 6.12.112-galdrun, Buildroot 2025.02.18 and ARMv6 userspace.
The running kernel/client files match the release hashes; root and boot use
the SD partitions. Its existing client ID was retained and connected to the
server over Ethernet. Onboard headphones registered, the CPU reported its
actual 700 MHz setting, and appliance plus 1200/320 px GUI checks passed.
Temporary SSH provisioning/login and disabling SSH afterward passed. An attached
INNO-MAKER HAT enumerated as BossDAC and the client reported playback through
its selected PCM device. Real GUI Apply/Restore each rebooted automatically,
reloaded the page and rescanned the outputs: switching to USB/onboard removed
BossDAC from the cache and found headphones; restoring INNO did the reverse.
The separate Restart button rebooted and reconnected with the INNO profile,
selected output and client ID retained. Startup request failures retry during
reconnection. SSH was confirmed disabled afterward. Desktop/mobile GUI checks,
the transient-reconnect regression and Windows/Linux client suites passed.

Peripheral certification, wall-power measurement and the 24-hour grouped
playback soak remain hardware checks. The Pi 3 USB boot changes only the two
storage-device paths described above; it exercises the image's actual firmware,
kernel and userspace.
