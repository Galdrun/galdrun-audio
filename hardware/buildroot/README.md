# Raumfeld Connector 2 firmware

Buildroot external tree that turns a Teufel Raumfeld **Connector 2** into a Galdrun
client. Board target: **`connector2`**. The build produces a modern Linux firmware
with the Galdrun client preinstalled and a USB stick image that installs it through
the device's own recovery path. It never flashes hardware and never replaces the
bootloader.

Pinned baseline: [Buildroot 2025.02.18 LTS](https://buildroot.org/download.html),
[Linux 6.12.112 LTS](https://www.kernel.org/), Bootlin ARMv7 hard-float glibc
toolchain 2024.05-1 (GCC 13.3). Downloads are checked against SHA256 hashes. The
original NAND partition geometry, SPL and U-Boot are kept.

## Build

At the repository root. This installs the packages and cross toolchains when
missing (on Windows inside WSL Ubuntu), builds the Linux ARMv6 client the firmware
bundles, then the firmware:

```sh
scripts/build-client.sh connector2                                         # Linux
powershell -ExecutionPolicy Bypass -File scripts\build-client.ps1 -Target connector2   # Windows
```

Ubuntu host packages, if you set up Linux yourself:

```sh
sudo apt install build-essential bison flex cpio libssl-dev libncurses-dev patch unzip \
  file wget curl git rsync python3 bc bzip2 xz-utils e2fsprogs
```

Firmware only (needs `releases/client/linux-armv6` built first: `scripts/build-client.sh linux-armv6`):

```powershell
hardware\buildroot\Build.ps1 -Boards connector2     # Windows, runs build.sh in WSL
hardware\buildroot\Build.ps1 -List                  # available boards
```

```sh
bash hardware/buildroot/build.sh connector2          # Linux / WSL
```

On first run the build downloads Buildroot, the kernel and the package sources into
`downloads/`. It builds the firmware, then the USB installer (a second Buildroot
tree, `connector2-installer` in the work directory, see [USB image](#usb-image)).
Results are exported to `output/connector2/`, logs to `logs/`.

- Jobs: 16 by default (`-Jobs 8` / `JOBS=8`).
- Work directory: `${XDG_CACHE_HOME:-$HOME/.cache}/raumfeld-buildroot` on the Linux
  filesystem for speed; override with `RAUMFELD_BUILD_DIR`. Builds are serialized
  with `flock`.
- Linux is rebuilt when its config, driver/DTS/firmware sources, the defconfig or
  `external.mk` change; OpenSSH and BusyBox when their patches/config change.
  Overlays and post-build/image hooks are reapplied on every build. For broader
  package or toolchain changes use a fresh work directory.
- Ubuntu's uutils `install` is incompatible with Buildroot; `PrepareHost.sh` puts a
  GNU `install` symlink into the work directory without changing the system.

### Artifacts

| File | Purpose |
|---|---|
| `usb-install/connect2.img` | USB installer: firmware with the client preinstalled; replaces mtd6–mtd8 |
| `usb-install/installer-manifest.json` | Payload sizes and hashes, host verification results |
| `raumfeld-connector2-firmware.tar.gz` | Kernel, DTB store, NAND rootfs, resolved config and manifest |
| `uImage` | Kernel for the existing U-Boot; load/entry `0x80008000`, at most 6 MiB |
| `am335x-raumfeld-connector2.dtb` | Board device tree on Linux 6.12 SoC definitions |
| `dts.cramfs` | Vendor-compatible DTB store, at most 256 KiB, with all four Connector filenames |
| `rootfs.ubifs` / `rootfs.ubi` | NAND rootfs; UBI with `RootFS`/`Updates`, 128 KiB PEBs, VID offset 2048, data offset 4096 |
| `rootfs.tar.gz` | Filesystem archive including kernel modules and board firmware |
| `uInitrd` | RAM-test root filesystem for U-Boot `bootm` ([RAM test boot](#ram-test-boot)) |
| `linux.config` | Resolved kernel configuration |
| `firmware-manifest.json` | Sizes, SHA256 hashes and validation status |

`scripts/verify-images.py` runs as part of the build and fails it when required
drivers disappear, uImage headers/CRCs are wrong, UBI offsets differ or kernel/DTB
partitions overflow.

## Install

End-user steps (USB stick, Reset button, going back to the Raumfeld software) are in
[releases/client/raumfeld/connector2/README.md](../../releases/client/raumfeld/connector2/README.md).

The installer checks model, partition/page/OOB geometry, payload MD5s and archive
readability before writing anything. It then writes only mtd6–mtd8, verifies every
rootfs file, reads back kernel and DTB, checks that mtd0–mtd5 are unchanged and
reboots on success. LEDs alternate while it runs, both turn on when done; one on
and the other fast-blinking means it failed and stopped without rebooting.
Installing replaces the device's settings. On first boot each device generates its
own client ID and SSH host keys.

Tested end to end on one Connector 2 (board `connector-0-0`, U-Boot
`2014.07-g8fd4890`, Aug 12 2016) on 2026-10-22: Reset-held USB boot, check mode,
install with readback, and first boot of the installed firmware. Other units and
U-Boot versions have not been tested; on a mismatch the preflight refuses before
writing.

Going back to Raumfeld: Teufel's untouched `connect2.img` reinstalls stock 2.21.0
the same way. Tested on the converted unit above on 2026-10-22: afterwards it boots
Raumfeld (Linux 4.0.9) from NAND. Teufel's installer reformats mtd8 completely, so
the UBI erase counters start again from zero.

### USB image

With Reset held, the vendor U-Boot loads the first `0x840000` bytes of
`connect2.img` from a FAT32 stick to `0x80200000`, points `cramfsaddr` at
`0x80a00000` and runs its `usb_update_boot`:

```text
usb_update_boot=echo Booting from USB image ...; setenv bootargs console=${console}; run read_dtb; run fixup_fdt; bootm ${loadaddr} - ${fdtaddr}
read_dtb=cramfsload ${fdtaddr} ${dtb_name}
```

No initrd and no signature check, only the uImage CRCs. Every byte of our image is
built here; it contains nothing of Teufel's:

| Offset | Content |
|---|---|
| `0x000000` | Installer uImage: the board kernel (XZ) with a BusyBox/mtd-utils initramfs built in, at most `0x800000` bytes |
| `0x800000` | `dts.cramfs`, the same DTB store the installed firmware boots with |
| `0x840000` | Payload, a plain tar: `rootfs.tgz`, `kernel.uImage`, `dts.cramfs`, `rootfs-files.md5`, `payload.md5` |

The installer is built from `board/connector2/installer/` (its defconfig, a kernel
config fragment on top of `linux.config`, and `init.sh`). Its `/init` waits for the
stick, mounts it read-only, unpacks the payload to `/payload` and runs
`install-usb.sh` through `start-usb.sh`. Any failure ends in a shell on the UART.

To check an image without any NAND write, stop autoboot on the UART instead of
holding Reset (Reset entry saves the U-Boot environment to mtd5 first) and boot it
in check mode, which runs only the preflight checks and then stops in a shell:

```text
usb start
fatload usb 0:1 0x80200000 connect2.img 0x840000
iminfo 0x80200000
setenv cramfsaddr 0x80a00000
run read_dtb
run fixup_fdt
setenv bootargs console=ttyO0,115200n8 galdrun.install=check
bootm 0x80200000 - 0x82000000
```

## Connect

- **Ethernet** requests DHCP and retries while linked; the hostname is the device
  name (default `connector2`).
- **Setup Wi-Fi:** without Ethernet carrier (or with carrier but no lease after about
  30 s) the device opens the access point **`Connector2`**, password **`connector2`**,
  address **`192.168.77.1`**, DHCP `192.168.77.10–100`, country AT, channel 1,
  WPA2/CCMP. Its captive DNS and port-80 redirect open the settings page on phones.
  The AP does not route to Ethernet or the Internet. An Ethernet lease stops it.
- **Settings page:** `http://<device>:4720/` configures device name, Ethernet
  (DHCP/static, 10/100/auto), Wi-Fi station (scan, SSID/password, DHCP/static),
  access point and country. Settings are stored in `/etc/galdrun-network.conf`
  (0600, parsed without shell evaluation) and applied within about seven seconds;
  a wrong static address can make the device unreachable. With a configured station
  that fails to associate or get an address, the setup AP returns after the grace
  period and the station is retried every two minutes. Board defaults are in
  `/etc/raumfeld-network.conf`; restart with `/etc/init.d/S45raumfeld-network restart`.
- **SSH:** off after installing; the firmware ships with no root password and no
  key. Turn it on in the settings page's **Remote login (SSH)** card, which first
  asks for a root password (at least 8 characters). Once a password is set, every
  change there (off, on, new password) needs it; a wrong one locks the card for a
  few seconds. The card calls `/usr/sbin/raumfeld-ssh`, which sets the password with
  `chpasswd` and writes `/etc/galdrun-ssh.conf`; `/etc/init.d/S50sshd` starts sshd
  only while that says `SSH_ENABLED=1`. The first start creates the host keys and
  takes up to a minute. A forgotten password needs a reinstall. The settings page
  is plain HTTP, so the password crosses the local network unencrypted. Add your
  key with `ssh-copy-id` once SSH is on. A developer build can put a public key in
  the image: `GALDRUN_SSH_PUBKEY=~/.ssh/id_ed25519.pub` (SSH still has to be turned
  on). Never set it for a build you publish.

- **UART:** 115200 8N1, Linux `ttyS0`.

## Client on the device

| Path | Content |
|---|---|
| `/opt/galdrun-client/galdrun-client` | Client executable, started by `/etc/init.d/S95galdrun-client {start\|stop\|restart\|status}` |
| `/opt/galdrun-client/lib/` | Bundled runtime libraries |
| `/etc/galdrun/client.json` | Persistent settings and room ID; seeded from `board/connector2/client-default.json` only when absent |
| `/etc/galdrun-board.json` | Board capabilities (`board/connector2/client-board.json`) |
| `/etc/galdrun-cpu.conf` | Saved CPU frequency |
| `/run/log/galdrun-client.log` | Client log in RAM |

Configure the server on the settings page. When restoring a saved `client.json`,
keep both `clientId` and `name`.

Update the client over SSH without reflashing (`--identity` defaults to the key in
`private/connector2/`):

```sh
scripts/upload/connector2.sh --ip <device>                                  # Linux
powershell -ExecutionPolicy Bypass -File scripts\upload\connector2.ps1 -IP <device>   # Windows
```

By hand: copy the new binary as `galdrun-client.next`, then stop the service,
replace the executable and start it:

```sh
scp -i <key> releases/client/linux-armv6/galdrun-client root@<device>:/opt/galdrun-client/galdrun-client.next
ssh -i <key> root@<device> 'chmod 755 /opt/galdrun-client/galdrun-client.next && /etc/init.d/S95galdrun-client stop && cp -p /opt/galdrun-client/galdrun-client /opt/galdrun-client/galdrun-client.previous && mv /opt/galdrun-client/galdrun-client.next /opt/galdrun-client/galdrun-client && /etc/init.d/S95galdrun-client start'
```

## Device features

The board capability file enables these settings in the client page and ServerGui.

- **CPU:** 300 MHz (default), 600 or 720 MHz with the `performance` governor, restored
  before audio starts. `ondemand` caused underruns and is not offered.
- **Audio formats:** stereo 22.05, 32, 44.1, 48, 88.2, 96, 176.4 and 192 kHz, 16 or 24
  bits in a 32-bit container. Analog and S/PDIF outputs share one PCM.
- **DAC controls:** the CS4271 mixer controls (volume, mute, filters, de-emphasis,
  soft ramp, channel routing, ...) are editable, saved atomically in
  `/etc/galdrun-dac.json`, and can be reset to the driver defaults.
- **Hardware clock drift correction:** ALSA control `Drift compensator` (−500 to
  +500 ppm, positive slows MCLK). Used only when a room's drift correction is set to
  *Hardware clock*; the default is server resampling. Reset to 0 whenever the device
  opens or the stream stops.
- **Status LEDs** (`raumfeld:1` = A, `raumfeld:2` = B), driven by
  `S96raumfeld-status-leds` from the client status API; can be switched off:

  | State | LED A | LED B |
  |---|---|---|
  | Booting | pulse | off |
  | Waiting for server/network | on | slow blink |
  | Connected | on | on |
  | Setup required | on | double blink |
  | Client failure | on | fast blink |
  | Firmware update running | alternating | alternating |

- **Buttons:** `POWER` (`KEY_POWER`) click toggles standby, long press enters deep
  sleep; `SETUP` and `RESET` are unassigned and can be bound in the input editor.
- **Standby and deep sleep:** standby after 600 s idle, deep sleep (system suspend)
  after 1800 s by default; both adjustable. Wake by POWER press or, over Ethernet,
  by Wake-on-LAN (the server sends the magic packet on play or
  `POST /api/v1/rooms/{client}/wake`). The Wi-Fi driver is unloaded during sleep.
  Measured at the plug: playing 2.4 W, standby 2.0 W, deep sleep 0.6 W. POWER is
  not wired to the PMIC, so there is no full power-off.
- **Time:** Chrony steps the clock only during the first three updates and limits
  later slewing to 50 ppm (`board/common/overlay/etc/chrony.conf`). `CLOCK_MONOTONIC`
  follows NTP slewing, and Chrony's default slew rate made the server see false
  drift of thousands of ppm and rejoin rooms repeatedly after boot.
- **Logs** stay in RAM: `/var/log` points to a 4 MiB tmpfs at `/run/log`; syslog
  rotates at 256 KiB, the client log at 1 MiB, each with one backup. Login history
  is not kept. Nothing logs to NAND.
- **Tools:** curl with CA certificates, rsync (over SSH, no daemon), strace, tcpdump,
  BusyBox `timeout`/`nc`, procps-ng `vmstat`, ALSA utilities. None of them starts at
  boot.

## Hardware and audio

The board DTS covers UART, RMII/AR8030 Ethernet, Marvell SD8787 SDIO Wi-Fi, BCH8/ELM
NAND, CS4271 codec (I2C), AK4104 S/PDIF (SPI), McASP0/EDMA, Si5351A clock generator,
TPS65217 PMIC, EEPROM, USB host, buttons, LEDs, RTC and watchdog. CPU OPPs are capped
at the factory's 720 MHz. The TI wakeup Cortex-M3 firmware is built into the kernel.
Wi-Fi firmware comes from Buildroot's `linux-firmware` package; see
[board/connector2/firmware/README.md](board/connector2/firmware/README.md).

The ASoC machine driver (`board/connector2/raumfeld-connector2.c`) exposes **one
stereo PCM** for the shared I2S bus of analog and digital outputs, plus CS4271
capture. Both outputs therefore run at the same rate. It selects the 22.5792 or
24.576 MHz clock family with 32-bit slots and keeps both Si5351 clocks running,
which reduced start/stop clicks.

Kernel patches (`board/connector2/patches/linux/`):

1. Si5351: enable multisynth fanout when CLK2 mirrors MS0.
2. CS4271: read the mute property spelled by the current binding.
3. ALSA: preserve driver timestamps in the 32-bit status conversion.
4. CS4271: expose output mixing controls.
5. CS4271: keep a user mute across streams.
6. ti-sysc: leave the AM335x system timers to the clocksource (fixes resume).

```sh
raumfeld-hardware-check
aplay -l; arecord -l
amixer -c Connector2 controls
amixer -c Connector2 cget name='Drift compensator'
```

The factory device tree does not identify the CS4271 analog supply or the DRV603
mute wiring, so the port does not assign them.

## NAND notes

Investigated on one Connector 2 (Toshiba NAND, READID `98 da 90 15 76 16 08 00`).

| Property | Linux | Factory U-Boot 2014.07 |
|---|---|---|
| Capacity / eraseblock / page | 256 MiB / 128 KiB / 2048 bytes | same |
| OOB size | **128 bytes** (correct: Toshiba 24 nm raw SLC rule) | 64 bytes (older generic ID decoding) |
| ECC | BCH8, GPMC + ELM, four 14-byte slots at OOB bytes 2–57 | same positions |
| Bad-block scan | whole chip | only the first 4 MiB (blocks 0–31); everything beyond counts as good |

- **128-byte OOB is right.** Do not force Linux to 64 bytes or truncate 128-byte raw/OOB pages.
- **U-Boot's bad-block list is not evidence of healthy flash.** Its vendor `create_bbt` returns after 32 blocks, so `nand bad` stays empty even with bad blocks present. Part of mtd6 and all of mtd7 lie outside its scan: a future bad block there would not be handled by U-Boot.
- Each 14-byte ECC slot is 13 BCH bytes plus a ROM compatibility byte; bytes 0–1 hold bad-block markers. Keep `ti,nand-ecc-opt = "bch8"` and the target driver's ROM-compatible slot layout. An arbitrary BCH8 format is not enough.
- Reset-held USB entry saves the U-Boot environment (mtd5) before Linux starts. The official installer also erases mtd5 and reformats mtd8; ours leaves mtd5 alone.

Rules for NAND writes:

1. Keep the original SPL, firmware data, U-Boot and partition geometry (mtd0–mtd5). Original boot regions are read-only in the new DTS. Write and UBI-format only from Linux with full bad-block detection.
2. Keep RootFS in UBI/UBIFS with VID offset 2048 / data offset 4096 (`ubinize.cfg`). RootFS must fit its 191 MiB volume.
3. Check mtd6/mtd7 for bad blocks with Linux before placing the kernel or DTB there, and verify that U-Boot can still read them.
4. Never write FF-padded, ECC-corrected dumps while skipping bad blocks: that shifts image placement. Restoring a raw backup needs a partition-specific procedure with ECC regeneration and UBI handling.
5. Make a verified backup of the device first. The official `connect2.img` reinstalls Raumfeld 2.21.0; returning to an older version needs your own backup.

## RAM test boot

Boots kernel, DTB and `uInitrd` over TFTP into RAM without installing anything; a
normal power cycle returns to the installed firmware. Serve the three files from
Windows (accepts only the given device IP, read-only, exits after 600 s):

```powershell
python hardware\buildroot\scripts\serve-tftp.py --host <pc-ip> --device <board-ip>
```

Stop autoboot on the UART (power-cycle without holding Reset) and run. All `setenv`
changes are volatile; do not `saveenv`:

```text
setenv ipaddr <board-ip>
setenv serverip <pc-ip>
setenv netmask 255.255.255.0
tftpboot 0x80200000 uImage
tftpboot 0x82000000 am335x-raumfeld-connector2.dtb
tftpboot 0x84000000 uInitrd
iminfo 0x80200000
iminfo 0x84000000
setenv fdt_high 0xffffffff
setenv initrd_high 0xffffffff
setenv bootargs console=ttyS0,115200n8 rdinit=/init panic=0
bootm 0x80200000 0x84000000 0x82000000
```

Do not run the factory `fixup_fdt` (it targets another Ethernet node). Recheck the
load addresses if the kernel or rootfs grows a lot. RAM boots generate fresh SSH
host keys every time; the UART console has no password.

## Tests

Host-only, in Linux/WSL; none of them touches a device:

```sh
python3 hardware/buildroot/scripts/test-network.py        # network supervisor, mocked interfaces
python3 hardware/buildroot/scripts/test-network-init.py
python3 hardware/buildroot/scripts/test-wifi-scan.py      # scan success, failure, timeout, cancel
python3 hardware/buildroot/scripts/test-client-image.py   # client service and installer guards
python3 hardware/buildroot/scripts/test-linux-service.py  # Pi uploader's systemd unit (needs systemd-analyze)
python3 hardware/buildroot/scripts/verify-images.py hardware/buildroot/output/connector2
```

## Known limitations

- Reinstalling stock Raumfeld firmware on a converted unit and restoring raw NAND
  backups are untested.
- Deep sleep entered within about 30 s of boot fails the power-domain transition;
  the automatic delay's 60 s minimum avoids it.
- Hardware clock trim needs measurement (sign, granularity, click-free live
  retunes, S/PDIF lock) before relying on it.
- Some start/stop clicks remain on the analog output; S/PDIF and analog input
  quality are not measured.

## Another board

Add `configs/raumfeld_<board>_defconfig` and `board/<board>/` (DTS, kernel config,
image hooks), reuse `board/common/` where it fits, and build with
`build.sh <board>`. Unknown board names fail before building. Other Raumfeld devices
have not been analyzed.
