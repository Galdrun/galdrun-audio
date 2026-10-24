# Raspberry Pi audio appliance

Build from the repository root:

```sh
scripts/build-client.sh rpi1 rpi3
```

On Windows:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-client.ps1 -Target rpi1
powershell -ExecutionPolicy Bypass -File scripts/build-client.ps1 -Target rpi3
```

`rpi1` uses ARMv6 hard-float/VFP userspace; `rpi3` uses ARMv7 hard-float/VFPv3-D16.
Both use Buildroot 2025.02.18 and Raspberry Pi Linux 6.12.112, commit
`9cf0f5c001b790d53689b4aadca873db169cbc44`. Boot firmware is Buildroot's pinned
`5476720d52cf579dc1627715262b30ba1242525e`. Hashes are checked by Buildroot;
the Pi-only ALSA time32 timestamp patch preserves the driver's system timestamp.
No AM335x/Connector hardware patches or firmware are used.

Images, SHA256SUMS, manifests and licenses are exported to
`releases/client/raspberrypi/rpi1/` and `rpi3/`. These generated artifacts,
downloads, logs, private keys and local test data are ignored by Git. Publish
binaries as release assets. Source licenses and Buildroot's legal-info manifest
travel with each image; boot and wireless firmware contain redistributable
binary blobs. Corresponding downloaded source archives and legal-info sources
remain in the local Buildroot cache for source distribution.

## Install and recover

1. Keep the existing working card untouched. Back up its identity/settings first.
2. Check `sha256sum -c SHA256SUMS`. Decompress the matching
   `galdrun-rpi1-sdcard.img.gz` or `galdrun-rpi3-sdcard.img.gz`.
3. Write the image to a **spare** SD/microSD card with Raspberry Pi Imager's
   **Use custom** option. Check the selected device; imaging erases that card.
   A 1 GB or larger card is sufficient. Pi 1 Model B may require full-size SD.
4. Connect Ethernet and a reliable supply. Boot, find `galdrun-rpi1` or
   `galdrun-rpi3` in DHCP, and open `http://<address>:4720/`.
5. Select the audio device explicitly. No output is selected automatically, and
   a missing selected output does not redirect sound to another speaker.

The SD has a 64 MiB FAT boot partition and 512 MiB writable ext4 root partition.
Settings and identity persist in `/etc/galdrun/`; appliance settings are in
`/etc/galdrun-*.conf` and `/etc/galdrun-dac*`. Logs stay in a capped RAM
filesystem under `/run/log`. The boot partition is mounted read-only normally.
Serial boot diagnostics use 3.3 V UART GPIO14/15, 115200 8N1. Pi 3 disables
Bluetooth to keep PL011 stable when CPU frequency changes. Never connect UART VCC.

Recover by reinstalling the original card. For a bad audio profile, shut down,
mount the spare card's FAT partition on another computer and restore
`galdrun-audio.previous.txt` as `galdrun-audio.txt`; selecting USB uses an empty
audio include. Leave `config.txt` and the kernel/DTB names unchanged.

## Network and SSH

Ethernet is preferred. Set DHCP/static addresses, SSID/password, country and a
Wi-Fi radio in the web GUI. A selected radio is stored by MAC; if unplugged,
the supervisor uses the default available radio, then the first by interface
name, and returns to the preferred radio when reattached.

Without an Ethernet lease or usable station connection, an AP-capable radio
opens **Galdrun-Pi**, password **galdrun-pi**, at **192.168.77.1**. Open the
captive settings page or `http://192.168.77.1:4720/`. A saved station is retried
after the setup interval. Station-only adapters retain Ethernet setup access.
The AP is local setup access and does not forward Internet traffic.

SSH starts **off**, with a locked root account and no developer keys. As on
Connector 2, set a root password and enable SSH in the web GUI. Each device
generates its own host keys. No test-device password belongs in an image.

For a client-only update on an installed Galdrun Buildroot Pi, build the matching
`linux-armv6` / `linux-armv7` release, enable SSH, then use
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
Modern HAT profiles target Pi 3 Model B and Pi 1 B+ 40-pin headers. Pi 1 Model B
revision/P5 wiring remains a separate hardware validation task.

Catalogue coverage includes HiFiBerry DAC/DAC+/Pro/HD/ADC/DSP/Digi/Amps,
Raspberry Pi/IQaudio DAC/Codec/Digi, JustBoom, Allo Boss/Boss2/Piano/Katana/DigiOne,
AudioInjector Stereo/Isolated, Fe-Pi, Audiophonics I-Sabre Q2M and INNO-MAKER
HiFi DAC / DAC Mini HAT (PCM5122). The INNO-MAKER profile uses
`allo-boss-dac-pcm512x-audio` as specified in the
[manufacturer's manual](https://github.com/INNO-MAKER/HIFI-DAC-Hat-For-Raspberry-Pi/blob/main/HIFI%20DAC%20User%20ManualV1.7.pdf);
ALSA identifies it as BossDAC. Its driver/overlay are included in both images.
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
