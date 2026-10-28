# Raumfeld Connector 2: how the Galdrun port works

Notes for an agent porting Galdrun to another Raumfeld device (for example a
Connector 3). Everything below was observed on **one** Connector 2 on 2026-10-22
unless marked otherwise. Treat Connector 2 values as hypotheses for any other
device and verify each one on the new hardware first.

The maintained build lives in `hardware/buildroot/`; its
[README](../../../hardware/buildroot/README.md) covers the firmware itself (DTS,
audio, NAND notes, network, LEDs). This file covers the boot path, the USB
installer, and how they were found and tested.

## Ground rules

- **Ship no vendor bytes.** The USB image is built entirely from source. Teufel's
  official image is used only to go back to Raumfeld and is never downloaded or
  embedded by the build. Vendor binaries also contain credentials (Teufel's
  `libraumfeldcpp` holds a TIDAL client ID); never extract, copy or commit them.
- **Never touch the bootloader region.** mtd0–mtd5 (SPL copies, firmware data,
  U-Boot, U-Boot env) stay as they are. The new DTS marks them read-only, and the
  installer checksums them before and after it writes.
- **Never run `saveenv`** or change U-Boot variables persistently. Every `setenv`
  used for tests was volatile (lost on power cycle).
- **Prove before writing:** UART read-out, then RAM boot, then check mode, then
  the real install. Each step writes less than the next.
- **No shared credentials in images.** The firmware ships without a root password
  and without SSH keys, and sshd stays off. The client's settings page turns it on
  (`getSsh`/`setSsh` in `client/src/net/SshSettings.cpp`, board flag
  `sshSettings`). It calls `/usr/sbin/raumfeld-ssh`, which needs a new password to
  turn SSH on and the current one for every later change. A new device port should
  reuse this, not a default password.

## Hardware (Connector 2)

| | |
|---|---|
| SoC | TI AM335x ES2.1 (Cortex-A8, NEON), CPU capped at the factory 720 MHz |
| RAM | 256 MiB DDR2 |
| NAND | Toshiba 256 MiB SLC, 2048-byte page, **128-byte OOB**, 128 KiB eraseblock, BCH8 (GPMC + ELM) |
| Ethernet | CPSW + Qualcomm Atheros AR8030 PHY (RMII), 100 Mbit |
| Wi-Fi | Marvell SD8787 (SDIO, `mwifiex_sdio`) |
| Audio | CS4271 codec (I2C 2-0010), AK4104 S/PDIF (SPI), McASP0, Si5351A clock |
| Other | TPS65217 PMIC, AT24 24c08 EEPROM (I2C 0-0050), USB host (MUSB, CPPI41 DMA) |
| LEDs | `raumfeld:1` (gpio3 4), `raumfeld:2` (gpio3 10) |
| Buttons | Reset gpio1 9, Setup gpio2 18, Power (see DTS) |
| UART | 3.3 V, 115200 8N1. U-Boot calls it `ttyO0`, Linux 6.12 `ttyS0` |

U-Boot board detection on this unit:

```text
module-revision    (ADC0): 0xD1F
runtime-config     (ADC2): 0x8D6
baseboard-revision (ADC3): 0x729
dtb_name is am33xx-raumfeld-connector-0-0.dtb
```

## Bootloader

- SPL `2014.07-00010-g89000bc (Jul 22 2014)`, U-Boot `2014.07-g8fd4890 (Aug 12 2016)`,
  built with Buildroot 2014.11, GCC 4.9.3.
- `bootdelay=1`. Space or Ctrl-C during `Hit any key to stop autoboot` reaches
  the `U-Boot#` prompt. That is safe: without Reset held, nothing is saved.
- Useful commands present: `cramfsload`, `cramfsls`, `fatload`, `fatls`,
  `fatwrite`, `ext2load`, `ext4load`, `tftpboot`, `dhcp`, `bootm`, `bootz`,
  `iminfo`, `usb`, `nand`, `fdt`, `mtdparts`, `loady`.
- No FIT, no signature check: legacy uImage with CRC only.

Environment (`printenv`, MAC address left out):

```text
bootcmd=run nand_boot
bootdelay=1
console=ttyO0,115200n8
cramfsaddr=0x82600000
fdtaddr=0x82000000
fdt_high=0xffffffff
loadaddr=0x80200000
dtb_name=am33xx-raumfeld-connector-0-0.dtb
mtdids=nand0=omap2-nand.0
mtdparts=mtdparts=omap2-nand.0:128k(SPL),128k(SPL.backup1),128k(SPL.backup2),128k(SPL.backup3),1m(U-Boot),128k(U-Boot Env),6m(Kernel),256k(DTB),-(rootfs)
nand_args=setenv bootargs console=${console} root=${nand_root} rootfstype=${nand_rootfstype}
nand_boot=echo Booting from nand ...; nand read ${loadaddr} ${nand_kernel_offset} ${nand_kernel_size}; run nand_cramfs_load_dtb; run fixup_fdt; run fixup_led; run nand_args; bootm ${loadaddr} - ${fdtaddr}
nand_cramfs_load_dtb=nand read ${cramfsaddr} ${nand_cramfs_offset} ${nand_cramfs_size}; run read_dtb
nand_cramfs_offset=0x7a0000
nand_cramfs_size=0x40000
nand_kernel_offset=0x1a0000
nand_kernel_size=0x600000
nand_root=ubi0:RootFS rw ubi.mtd=8,2048
nand_rootfstype=ubifs
read_dtb=cramfsload ${fdtaddr} ${dtb_name}
fixup_fdt=fdt addr ${fdtaddr}; fdt resize; fdt set /ocp/ethernet@4A100000/slave@0 mac-address [${ethaddr_dt}]; fdt set / uboot-version "${ver}"; fdt set / baseboard-revision "${baseboard_revision}"; fdt set / module-revision "${module_revision}";
fixup_led=if test "${disable_leds}" = "1"; then fdt set /ocp/leds/d2 default-state off; fi;
usb_update_boot=echo Booting from USB image ...; setenv bootargs console=${console}; run read_dtb; run fixup_fdt; bootm ${loadaddr} - ${fdtaddr}
net_update_boot=echo Updating from network image ...; run read_dtb; run fixup_fdt; bootm ${loadaddr} - ${fdtaddr}
update_uboot=... nand erase 0x80000 0x100000; nand write.i ...   (never run this)
```

NAND partitions (`mtdparts`; Linux names mtd3 "Firmware Data" and mtd8 "File System"):

| # | Name | Offset | Size |
|---|---|---|---|
| 0–3 | SPL + 3 backups | 0x000000 | 4 × 128 KiB |
| 4 | U-Boot | 0x080000 | 1 MiB |
| 5 | U-Boot Env | 0x180000 | 128 KiB |
| 6 | Kernel | 0x1a0000 | 6 MiB (uImage limit for NAND boot) |
| 7 | DTB | 0x7a0000 | 256 KiB (cramfs DTB store) |
| 8 | rootfs | 0x7e0000 | rest, about 248 MiB, UBI |

### The Reset-held USB path

Hold Reset **before** plugging in power and keep holding about 10 s. Pressing it
after power-on is too late: U-Boot then prints `Booting from nand ...`. When it
works, U-Boot prints:

```text
#### Rescue button pressed, or 'usbboot_once=yes' flag ####
#### set in environment. Preparing for USB boot ...    ####
(Re)start USB...
reading connect2.img
8650752 bytes read in 14467 ms (583 KiB/s)
stopping USB..
Hit any key to stop autoboot:  1  0
Booting from USB image ...
```

So the compiled-in Reset code does the following (the README says it also saves
the environment to mtd5, which is vendor behaviour we cannot change):

1. `usb start`, then `fatload usb 0:1 0x80200000 connect2.img 0x840000`: only the
   first 0x840000 bytes of `connect2.img` in the stick's root, FAT32.
2. Sets `cramfsaddr` to `0x80a00000`, which is `loadaddr` + 0x800000. The
   environment default `0x82600000` is only used for NAND boot.
3. `run usb_update_boot`: `bootargs` becomes just `console=ttyO0,115200n8`, the
   DTB comes from the cramfs at image offset 0x800000 by `dtb_name`, then
   `bootm` with **no initrd**. So any installer must be a kernel with its
   initramfs built in.

`usbboot_once=yes` in the environment would trigger the same path without the
button, but setting it needs `saveenv`. Not used.

`fixup_fdt` fails on our DTB (`libfdt fdt_path_offset() returned
FDT_ERR_NOTFOUND`, the vendor Ethernet node path does not exist). This is harmless:
the NAND boot of our firmware runs the same chain.

## Teufel's official image (for reference only)

`https://updates.raumfeld.com/images/connect2.img` (Raumfeld 2.21.0), 69,579,776
bytes, SHA256 `dc84e224579e72334560d3d79cf6ccc3354a40a1e3d0f5e66cf0fe16e0ab273c`.
Layout:

| Offset | Content |
|---|---|
| 0x000000 | uImage `Linux-4.0.9`, 5.2 MB, load/entry 0x80008000, type kernel, "uncompressed" (a zImage); initramfs (XZ) built in |
| 0x5289e0–0x800000 | zero padding |
| 0x800000 | cramfs, 36 DTBs: `am33xx-raumfeld-{base,connector,cube,element,one,soundbar,sounddeck,speaker-l,speaker-ml,test-jig}-<rev>.dtb`, `am33xx-teufel-rosenthal-*`, `am33xx-sue-s800-evm-0-1.dtb`, `raumfeld-fallback.dtb` |
| 0x842000 | ext2: Teufel's recovery userland (`/raumfeld/hardwared`, `/usr/lib/libraumfeldcpp-1.0.so`, BusyBox, `init.sh`, `/start.sh`, `rootfs.tgz` of 31 MB) |

The family shares one image format and DTB naming. For another model, read its
`dtb_name` from the U-Boot console. That model's official image should have the
same layout; check this before relying on it.

The first Galdrun installer kept Teufel's bytes up to 0x842000 and replaced files
in the ext2. That redistributed Teufel's proprietary code, so it was replaced by
the design below.

## The Galdrun USB image

Built by `hardware/buildroot/scripts/build-usb-installer.py` after the firmware:

| Offset | Content | Limit |
|---|---|---|
| 0x000000 | Installer uImage: our Linux 6.12.112, XZ, with initramfs built in (4.7 MiB) | 0x800000 |
| 0x800000 | `dts.cramfs`, the same DTB store the NAND firmware uses (four connector names, 20 KiB) | 0x40000 |
| 0x840000 | Payload, plain USTAR tar: `rootfs.tgz`, `kernel.uImage`, `dts.cramfs`, `rootfs-files.md5`, `payload.md5` | stick size |

Installer pieces:

- `board/connector2/installer/defconfig`: second Buildroot tree
  (`connector2-installer` in the work directory). `BR2_INIT_NONE`, BusyBox,
  mtd-utils (flash_erase, nanddump, nandwrite, ubi-utils), `BR2_TARGET_ROOTFS_INITRAMFS`,
  `BR2_LINUX_KERNEL_XZ`, same kernel config, DTS and patches as the firmware.
- `installer/linux.fragment`: `# CONFIG_MODULES is not set`. Every driver the
  installer needs is built in: NAND, MUSB, usb-storage, VFAT, GPIO LEDs.
- `external.mk`: for builds with an initramfs, sets
  `CONFIG_INITRAMFS_COMPRESSION_NONE` through `LINUX_KCONFIG_FIXUP_CMDS`. The XZ
  kernel compresses the initramfs along with everything else.
- `installer/post-build.sh`: installs `init.sh` as `/init`, `install-usb.sh` as
  `galdrun-install`, `start-usb.sh` as `galdrun-start`. Removes `libstdc++` and
  `libgomp`, which the Bootlin toolchain copies in (they were 2.3 MB).
- `installer/init.sh` (PID 1):
  1. Mounts devtmpfs, proc, sysfs and tmpfs.
  2. Waits up to 30 s for `/dev/sd*` and mounts the stick **read-only**.
  3. `dd if=connect2.img bs=4096 skip=2112 | tar -x -C /payload`.
  4. Runs `galdrun-install --check` when `galdrun.install=check` is on the
     command line; otherwise runs `galdrun-start`, which installs and reboots.
  5. Any failure ends in `/bin/sh` on the UART. PID 1 must never exit.
- `install-usb.sh`:
  1. **Preflight**, before any write: model string
     `Custom Raumfeld Connector 2`, exact `/proc/mtd`, page 2048, OOB 128, no
     UBIFS mounted, all tools present, payload MD5s, gzip and tar readable.
  2. **Install**: MD5 of mtd0–5; `ubiformat /dev/mtd8 -s 512 -O 2048`; attach;
     volumes `RootFS` 191 MiB and `Updates` 42 MiB; untar rootfs; MD5-verify
     every file; write mtd7, then mtd6, with nanddump readback; check mtd0–5
     unchanged.
  3. **LEDs**: alternate while running, both on when done, fast blink on failure.
  4. **Finish**: reboot.

Measured: the whole install takes about 50 s from kernel start, U-Boot's
`fatload` about 15 s. The RAM budget is fine (225 MB free in the installer).

## How it was tested (reuse this sequence)

All UART automation was done from Windows PowerShell with
`System.IO.Ports.SerialPort` (COM6, 115200). Writing a command and waiting for
the `U-Boot# ` or `~ # ` prompt is enough. Interrupt autoboot by sending space
and Ctrl-C repeatedly as soon as `Hit any key` appears.

1. **Read-only UART dump**: power on without Reset, stop autoboot, run `version`,
   `help`, `printenv`, `mtdparts`, `bdinfo`, `usb start`, `fatls usb 0:1`.
2. **RAM boot over TFTP** (no stick and no flash involved):
   1. In U-Boot, `setenv autoload no; dhcp`.
   2. Serve the files with `hardware/buildroot/scripts/serve-tftp.py --host <pc>
      --device <board> --images <dir>`. It serves only `uImage`, `uInitrd` and
      `am335x-raumfeld-connector2.dtb`, read-only.
   3. Boot:
      ```text
      tftpboot 0x80200000 uImage
      tftpboot 0x82000000 am335x-raumfeld-connector2.dtb
      run fixup_fdt
      setenv bootargs console=ttyO0,115200n8 galdrun.install=check
      bootm 0x80200000 - 0x82000000
      ```
3. **Check mode from the stick**, exactly what the Reset path loads:
   ```text
   usb start
   fatload usb 0:1 0x80200000 connect2.img 0x840000
   iminfo 0x80200000
   setenv cramfsaddr 0x80a00000
   cramfsls
   run read_dtb
   run fixup_fdt
   setenv bootargs console=ttyO0,115200n8 galdrun.install=check
   bootm 0x80200000 - 0x82000000
   ```
   Expect `PREFLIGHT PASSED` and `INSTALLER STOPPED: check mode: nothing was written`.
4. **Real install**: listen on the UART **without sending anything**, then do the
   Reset-held power-on. Expect `INSTALL COMPLETE`, a reboot, `Booting from nand ...`,
   and `connector2 login:` with `galdrun-client` running.

5. **Way back**: Teufel's untouched `connect2.img` with the same Reset procedure.
   Tested: afterwards `Booting from nand ...` boots Raumfeld (Linux 4.0.9, model
   `Raumfeld Connector (AM33xx)`) without stick. Its installer reformats mtd8 and
   resets the UBI erase counters (1174 before, 1 after). On first start Raumfeld
   prints `raumfeld-key-creator: creating new keys`; its console shows no login prompt.

## Pitfalls hit

- **Buildroot overrides kernel compression.** `BR2_LINUX_KERNEL_GZIP` is the
  default and wins over `CONFIG_KERNEL_*` in a fragment; use
  `BR2_LINUX_KERNEL_XZ`. `CONFIG_INITRAMFS_COMPRESSION_*` in a fragment is
  dropped too, because `INITRAMFS_SOURCE` is empty when fragments are merged. Set
  it in a kconfig fixup. The first installer was 7.8 MiB with gzip, modules and
  the C++ runtime; after the fixes it was 4.7 MiB.
- **CRLF breaks shell scripts on the device.** Files written on Windows with
  Python's `write_text` got `\r\n`, and `#!/bin/sh\r` reported "not found".
  Write with `newline="\n"` and check with `file` before building.
- **USB writes from Linux to the stick fail.** The MUSB/CPPI41 DMA stalled, the
  stick was reset and went offline, and the FAT was damaged. Reads are fine; a
  69 MB checksum passed. Never write to the stick from the device. If reads ever
  fail, try `CONFIG_MUSB_PIO_ONLY` for the installer.
- **Two console arguments.** U-Boot passes `console=ttyO0,...`, which the 6.12
  kernel no longer has. `CONFIG_CMDLINE="console=ttyS0,115200n8"` with
  `CONFIG_CMDLINE_EXTEND=y` adds the working one.
- **Harmless boot noise**, present with the old firmware too:
  - `ti-sysc 5600fe00.target-module ... error -110` (gfx);
  - `mwifiex_sdio ... cmd 0x242 error`;
  - `regulatory.db` load failure at kernel init;
  - `ubidetach: error 22` in the installer, when nothing is attached yet.
- **U-Boot's NAND view is incomplete.** It decodes 64-byte OOB and scans for bad
  blocks only in the first 4 MiB. See "NAND notes" in the hardware README.

## Checklist for a new model (Connector 3 or others)

For Connector 3, first read the [offline findings and experimental RAM SSH
image](connector3.md). Its i.MX7D signed FIT boot path differs from Connector 2;
physical boot remains unqualified. Other models have not been verified. Start here:

1. **Find a 3.3 V UART** and dump everything read-only, as in test step 1. Note
   the SoC, RAM, NAND geometry, U-Boot version, the full environment, the
   `dtb_name` logic and the partition table.
2. **Look at the USB update path**: the Reset message, which file name, how many
   bytes are loaded, where the DTB comes from, and whether anything is
   signature-checked. FIT images or `CONFIG_FIT_SIGNATURE` would rule out
   replacing the kernel. This works on the Connector 2 only because U-Boot checks
   nothing beyond the uImage CRC.
3. **Analyse that model's official image offline**: the uImage header, the
   container at the DTB offset, the filesystem behind it. Never ship any of it.
4. **Create a board directory** next to `board/connector2/`: DTS (start from the
   vendor DTB in the official image and decompile it for reference only),
   `linux.config`, partition labels, model string, NAND geometry checks in
   `install-usb.sh`, size limits (kernel partition, DTB slot, USB load size).
5. **Prove it** with RAM boot, then check mode, then the install. Keep U-Boot and
   its environment untouched, so the vendor Reset path and Teufel's image remain
   the way back.

## File map

| Path | Role |
|---|---|
| `hardware/buildroot/configs/raumfeld_connector2_defconfig` | Firmware Buildroot config |
| `hardware/buildroot/board/connector2/` | DTS, kernel config and patches, ASoC driver, post-build/image hooks, `install-usb.sh`, `start-usb.sh` |
| `hardware/buildroot/board/connector2/installer/` | USB installer: defconfig, kernel fragment, `init.sh`, post-build |
| `hardware/buildroot/board/common/overlay/` | Init scripts, network, LEDs, client service, SSH switch (`usr/sbin/raumfeld-ssh`, `etc/init.d/S50sshd`) |
| `hardware/buildroot/scripts/build-usb-installer.py` | Builds the installer and lays out `connect2.img` |
| `hardware/buildroot/scripts/verify-images.py` | Build-time checks of drivers, CRCs and sizes |
| `hardware/buildroot/scripts/test-client-image.py` | Host test of installer guards and write scope |
| `hardware/buildroot/scripts/serve-tftp.py` | Read-only TFTP for RAM boots |
| `releases/client/raumfeld/connector2/README.md` | End-user install steps |
