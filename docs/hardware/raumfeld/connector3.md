# Teufel Streamer / Connector 3: experimental SSH diagnostics

**2026-10-30:** a new signed USB RAM diagnostic incorporates the Ethernet and
early-boot fixes established with the working kexec client. Physical USB boot
reached Linux and its UART shell, but Ethernet failed because the USB kernel
lacked the RAM kernel's RMII clock-direction patch. A rebuild now reuses it and
passed physical Ethernet DHCP and key-only SSH cold boot. Subsequent protected
RAM tests also passed NAND/UBI/UBIFS reads and USB/FAT32 reads. A permanent
installer is not ready. The user's final decision is to use the working Marvell
`15.87.19.p14` blob and accept the available licence's version mismatch while
excluding OEM runtime software. Work is stopped and the board is back on beta;
read [the next-day handover](../connector3-handover-2026-10-31.md) before resuming.
See [the current build and test checkpoint](../connector3.md#usb-installer-preparation--2026-10-30)
for the artifact hash and next step. UART now works on COM5 at 115200. The failed
trials and missing-UART observations below describe the earlier images.

For the **verified vendor beta update and successful SSH access on 2026-10-28**,
see [the Connector 3 procedure](../connector3.md). The RAM diagnostic below
remains unqualified; success with the vendor beta does not validate that image.

The following is the **historical 2026-10-27 unqualified USB RAM boot**, not a Galdrun firmware installer. UART
on the test device has not produced readable output. Nothing here establishes
that the physical device boots this image or that Ethernet works on it.

The first physical trial on 2026-10-27 showed alternating left/right front LEDs
and **no physical Ethernet link after 2–5 minutes**. That did not establish Linux
startup. The diagnostic image does not drive the MCU LEDs, so their pattern is
not a Linux boot-progress indication. Further waiting alone is not verification.
The user subsequently reported that the revised image (SHA256 beginning
`d6edd805`) also did not work. Its actual boot stage remains unknown. Hardware
testing was stopped for the evening; further work below is offline research.

The maintained sources are under
[`hardware/buildroot/board/connector3/diagnostic/`](../../../hardware/buildroot/board/connector3/diagnostic/).
The build creates Linux, its initramfs and OP-TEE from source. It contains no
vendor kernel, OP-TEE binary, recovery filesystem or installer. It has no MTD,
MMC, SCSI, ATA or USB drivers, so its Linux environment cannot access persistent
storage. The factory bootloader runs before it and has not been read from this
device. Wi-Fi, audio and the front-panel MCU are outside this diagnostic image.

## Offline evidence (2026-10-27)

Teufel identifies `connect3.img` as the Streamer's image in its
[reinstallation instructions](https://support.teufel.de/hc/en-us/articles/360001499849).
Analysis copies belong in ignored `hardware/buildroot/downloads/` and `reference/`,
never in a Galdrun release or Git.

- Official `https://updates.raumfeld.com/images/connect3.img`: 86,054,912 bytes,
  SHA256 `0b2c6c47e5093ea5b3fd4d95a1487efe93fdeb05d4e250f29bfdb180c6739d5f`,
  filesystem version 2.21.0. Its Connector DTB identifies `Teufel Streamer (imx7d)`
  and declares 508 MiB usable RAM, reserving the last 4 MiB of 512 MiB.
- The leading FIT contains Linux, several DTBs and OP-TEE. `Connector` selects
  the Streamer DTB. Its configuration has a `sha256,rsa4096` signature, unlike
  Connector 2's legacy unsigned uImage. The stock OP-TEE load address is
  `0x9fd00000`; the kernel is at `0x80800000`, DTB at `0x83000000`.
- The current official beta image at
  `https://testing.raumfeld.com/beta/images/connect3.img` is version 2.22.0,
  SHA256 `2ffec150430d786af40224a745f2f59449e62cf15840b0f494030707b981891b`.
  Both inspected rootfs archives contain `S50dropbear`, gated by
  `/etc/raumfeld/sshd_enabled`, but lack the historical USB SSH-enabling hook.
  Merely putting the historical marker on USB is not a verified access method.

Teufel's [published source package](https://support.teufel.de/hc/en-us/articles/360013550139)
links `https://docs.teufel.de/download/raumfeld/legal-info-2021-10.zip`.
The relevant authoritative files inside it are:

- `legal-info-imx7/sources/uboot-ce8c8947cc/uboot-ce8c8947cc.tar.gz`:
  `include/configs/teufel_imx7.h` enables FIT/RSA verification and defines the
  load addresses and `bootm ${loadaddr}#${bootconfig}` path.
  `board/teufel/imx7/board.c` reads at most 12 MiB of `connect3.img` from USB.
  Its `ft_board_setup()` requires the exact
  `/soc/aips-bus@30800000/ethernet@30bf0000/mdio/ethernet-phy@0` path to fix
  the PHY address. The build preserves that path and the serial aliases.
  `arch/arm/cpu/armv7/nonsec_virt.S` sets LR to `end_init_optee` before entering
  OP-TEE; `arch/arm/lib/bootm.c` then cleans the caches and enters Linux with
  `images->ft_addr`, which may have been relocated and fixed up. The diagnostic
  monitor must return via LR: `CFG_NS_ENTRY_ADDR=` and `CFG_DT_ADDR=0`. The first
  image incorrectly jumped directly into Linux using a fixed original DTB
  address. The revised configuration corrects that mismatch; without UART it
  has not been proven to be the cause of the failed physical trial.
- `legal-info-imx7/sources/raumfeld-dts-1eefb3ed45/raumfeld-dts-1eefb3ed45.tar.gz`:
  `imx7d-sue-s810-0.dtsi` describes UART1, Ethernet FEC2/RMII and PHY reset on
  GPIO4_22. The diagnostic DTS translates those wiring facts to mainline Linux.
- The archive also contains signing material. Its `verified-boot` certificate's
  public modulus successfully decodes the current official Connector signature
  with valid RSA/SHA256 padding. Keep any local signing files private; do not
  print their contents, commit them, bundle them or replace the device bootloader.
  Presence of signatures and matching key material does not establish the actual
  device's bootloader version, key requirements or fuse state.

## Build and checks

Linux/WSL, from the repository root:

```sh
bash hardware/buildroot/scripts/build-connector3-diagnostic.sh /path/to/your-key.pub /path/to/local-fit-keys
```

The signing directory must contain `verified-boot.key` and `verified-boot.crt`.
The script does not fetch vendor firmware or signing keys. Use a public SSH key
whose private counterpart you own. Only the public SSH key enters the initramfs;
host keys are generated in RAM on every boot. Password login is disabled.

This uses the existing Buildroot/Bootlin cache with a separate
`connector3-diagnostic` work tree. Output is
`hardware/buildroot/output/connector3-diagnostic/connect3.img`, with a SHA256
sidecar and `diagnostic-verification.json`. It is not exported as a supported
`connector3` release or added to the general firmware targets.

The build invokes
[`verify-connector3-diagnostic.py`](../../../hardware/buildroot/scripts/verify-connector3-diagnostic.py):
FIT signature verification, the 12 MiB limit, disabled storage drivers, required
Ethernet/console support, and the actual initramfs's SSH files, key permissions,
authentication policy and absence of flash installers/private host keys.
These host checks do not qualify a physical boot. To rerun them:

```sh
python3 hardware/buildroot/scripts/verify-connector3-diagnostic.py ~/.cache/raumfeld-buildroot/connector3-diagnostic
```

The packaged ARM SSH binaries can also generate a host key and validate the
effective SSH policy in a temporary chroot under QEMU user mode. This opens no
listener and does not execute PID 1 or contact hardware:

```sh
sudo python3 hardware/buildroot/scripts/test-connector3-ssh.py ~/.cache/raumfeld-buildroot/connector3-diagnostic
```

## Physical test

1. Use Ethernet to a router with DHCP. Copy **only this diagnostic** `connect3.img`
   as a file into the root of an empty FAT32 USB stick (Teufel specifies at most
   4 GB). Do not combine it with another recovery or install image.
2. With the Streamer unpowered, insert the stick, hold Reset, connect power and
   keep holding Reset for 10 seconds, following Teufel's USB entry procedure.
3. Look in the router's DHCP leases for `connector3-diag`. The MCU's LEDs may
   remain in their bootloader/update pattern: this image does not drive the MCU.
4. Connect using `ssh -i /path/to/your-private-key root@DEVICE_IP`. No password
   is configured or accepted. Initial host-key generation may take time.
5. To leave the diagnostic session, disconnect power, remove the USB stick and
   power up normally. The diagnostic image has no installation step.

Each RAM boot generates a new host key; verify the device/IP and remove only
that device's stale `known_hosts` entry before reconnecting. Do not globally
disable host-key checking. If no SSH service appears, stop and investigate;
do not substitute the Connector 2 installer or attempt a blind flash.

## Online research for the next session (2026-10-27)

These are leads with different levels of evidence, not a confirmed repair or
an authorization to flash another image.

### Similar symptom reported by another owner

[x42's firsthand report near the bottom of page 1](https://www.hifi-forum.de/viewthread-212-2470.html)
describes modified third-generation images failing with alternating left/right
LEDs before reaching the expected installer. This concerns third-generation
speakers, not a verified Streamer failure. The author suspected early image
checks; that suspicion does not diagnose this device. It reinforces the need to
distinguish bootloader rejection from a Linux/Ethernet failure.

### Historical SSH route: older beta, not simply any beta

[Page 2 of the same owner thread](https://www.hifi-forum.de/viewthread-212-2470-2.html)
records a successful third-generation speaker SSH setup using **2.12 Beta7** and
the USB hook `etc/usbmount/mount.d/20sshd-enable`. Its marker filename was
`48fab7623bce0c903d5fe53dd681bb163eba85ae`. A later discussion specifically about
the Streamer mentions an archived `connect3.img` version **2.13.4**, dated
2021-04-14, and a proposed procedure involving the old beta. That is an archive
lead, not proof of a working Streamer procedure or a verified download.
The current official beta inspected locally is 2.22.0 and lacks that hook.

### Console UART and MCU UART are separate signals

The manufacturer **McIntosh RS150 service schematic, page 17**, hosted at
[ManualsLib](https://www.manualslib.com/manual/3570098/Mcintosh-Rs150.html?page=17),
identifies a `STREAM810 X4.4EW` module and separately labels `CONSOLE_RXD` /
`UART1_RXD`, `CONSOLE_TXD` / `UART1_TXD`, and the MCU serial signals. Its indexed
text associates console RX with B34, TX with B36 and ground with B38.

This is another product and module revision. The page's drawing could not be
visually retrieved during this search, so those indexed associations must be
checked against the actual schematic and board before use. They are **not** a
Teufel carrier-board header pinout or a verified UART voltage. The useful lead is
to identify the module and trace its console signals, rather than assume every
serial-looking header reaches the Linux console.

### Module revisions constrain old firmware choices

A [manufacturer change notification filed with the FCC](https://fccid.io/TTUBSCORE/Letter/Cover-Letter-6232245.pdf)
documents the Stream810H3 revision: AXP152 was replaced by AXP313A plus additional
converters. It says software must support the changed PMIC and shows board
identification/layout differences. This establishes a family-level compatibility
issue, not the revision fitted to this Streamer. Identify its module markings
before considering an old beta/downgrade.

The same notification explicitly discusses current injection through attached
USB-UART interfaces while a module is powered down. Its off-state rail voltage
limits are **not** a specification of the console's nominal logic voltage.

### Board photos and an official recovery reference

An owner's [Teufel Streamer teardown post, #451](https://www.diyaudio.com/community/threads/looking-for-a-good-not-expensive-streamer.377596/page-23)
contains three PCB photo attachments useful for comparing board layout. The
attachments themselves could not be retrieved, so no pinout was established.

Teufel separately documents [firmware repair preserving settings](https://support.teufel.de/hc/en-us/articles/360011716460-Teufel-Streaming-Raumfeld-devices-Firmware-recovery-repair)
and reinstallation deleting settings. The Streamer repair download is
`https://updates.raumfeld.com/repair/connect3.img`. This is a stock recovery
reference, **not** an SSH-enabled or RAM-only diagnostic image; it has not been
installed or analyzed here.

Next physical session: first confirm normal stock Ethernet link with the USB
stick removed, identify the exact module/board revision, and obtain an observable
boot stage before changing more firmware. No confirmed Teufel-specific UART
header mapping or verified historical Streamer SSH image was found in this search.

## User-provided SSH archive inspected (2026-10-28)

The user supplied a public [SSH Streamer folder](https://drive.google.com/drive/folders/1-htQzqporcluONsyXVopgBq1JnFk1148).
Both files were downloaded into ignored
`hardware/buildroot/downloads/connector3-ssh-archive/` and inspected without
executing firmware or contacting the device. This supersedes the earlier lack
of an accessible historical download; it does not establish a successful boot.

| File | Bytes | SHA256 |
| --- | ---: | --- |
| `connect3.img` | 86,412,288 | `a26cf9471f8cdb1cf32fa9a886f444bf7bcad860c4b41427ffd3ac40b547ae86` |
| `raumfeld-updates.img` | 164,304,384 | `44bb4c437c6319220b6e65c5bfcc06f440a19e9d4a8d4908d237f984ae36b07a` |

The installer's embedded `rootfs.tgz` reports **2.13.4**. It contains Dropbear,
but neither `/etc/raumfeld/sshd_enabled` nor the historical USB activation hook.
The installer alone therefore does not establish enabled SSH.

The update archive's `19.updates` targets **Teufel Streamer, hardware ID 19**.
Its `version` and `storageVersion` advertise **2.99**, while the referenced
payload's `/etc/raumfeld-version` reports **2.12-snapshot-7**. Treat it as an old
beta firmware downgrade despite the higher advertised number. The payload's
SHA256 matches its archive entry name
`227ec7fdb49bf85008014f178642c7da67b1c6443e4605fafd3b6ba38b99779e`.
This integrity check is not verification of the vendor RSA signatures.

The beta payload contains `etc/usbmount/mount.d/20sshd-enable`. When USB is
mounted, it checks for a file named
`48fab7623bce0c903d5fe53dd681bb163eba85ae` in the mount root. Only existence is
checked, so an empty file suffices. The hook creates the SSH and console enable
cookies and starts Dropbear. An empty marker, `SHA256SUMS` and `README.txt` were
prepared alongside the downloads; the structured inspection result is in ignored
`hardware/buildroot/reference/connector3-ssh-archive/inspection.json`.

These are persistent vendor installer/update files, not the source-built RAM
diagnostic. The archive contains no procedure document. Confirm the actual
module revision and the provider's complete procedure before using the old beta;
no physical install, SSH connection or UART enablement has been verified here.

## Setup WLAN and USB networking candidate (2026-10-28)

The user reports that the phone app discovers the Streamer, then fails after
Next, and that Ethernet is connected without a physical link light. The PC
successfully joined the open `Raumfeld Setup` WLAN, BSSID
`50:1e:2d:3b:77:f6`, obtained `192.168.0.3`, and pinged gateway `192.168.0.1`.
No responding TCP services were found on the tested common ports or ports
1-1024 and 49000-65535; SSDP discovery did not return a response. This does not
identify the running firmware version or establish why the app fails.

Static inspection of the downloaded **2.13.4** filesystem found an alternative
network setup mechanism:

- `etc/usbmount/mount.d/80setup-conf` links a USB root file named
  `raumfeld-setup.json` to `/tmp/raumfeld-setup.json`.
- In `raumfeld/master-process/master-process`, `Master::onSetupButtonPress()`
  constructs `PreconfiguredSetupController`. Its constructor reads `isHost`
  (boolean), `ssid`, `password`, `psk` and `name` (strings). The controller
  connects to the target network and requests device configuration. The
  active setup state can instead be cancelled by the first button press.

A local placeholder template and instructions were prepared under ignored
`hardware/buildroot/private/connector3/raumfeld-setup.json.example` and
`USB-NETWORK-SETUP.txt`. Enter the password locally and save the finished file
as `raumfeld-setup.json` on an otherwise empty FAT32 USB stick. With the device
booted, insert the stick and briefly press **Setup**, not Reset. If that only
cancels an existing setup, press Setup again. This is a candidate based on the
archived parser, not a physically verified procedure or SSH activation. No home
Wi-Fi credentials were retrieved, no setup request was sent, and no device
configuration or firmware was changed during this investigation.

### USB network setup outcome

The user supplied credentials locally, changed `isHost` to **false** because an
existing Raumfeld host is already present, and tried the USB setup file. The
setup WLAN subsequently disappeared. LAN discovery found the same MAC address
at **10.0.0.58**, with a live UPnP SetupService. Read-only SOAP queries confirmed:

- `GetInfo`: **2.13.4**; the old installer has reached a running system.
- `GetNetworkInfo`: **Chris WLAN**, address **10.0.0.58**, signal strength **90**.
- `GetDeviceMode`: **WAIT_FOR_SETUP**. Wi-Fi connectivity is established, but
  joining/configuring the existing Raumfeld system is not established.
- `GetUpdateInfo`: no update currently advertised. No responding SSH service
  was found, and the beta has not been shown to be installed.

The dynamically advertised description was
`http://10.0.0.58:54468/5e222a69-a6a0-42f9-8b26-7e674d30d228.xml`, with control
path `/SetupService/ctrl`. Discover it again after a reboot; the port and URL
can change. A local analysis copy is in ignored
`hardware/buildroot/reference/connector3-ssh-archive/live-device-description.xml`.
The user's USB trial establishes network connectivity for this attempt, not
completed client setup, an Ethernet repair or SSH activation. The PC's manual
setup-WLAN connection dropped when the device stopped advertising that WLAN;
the wired home-network connection remains available.
