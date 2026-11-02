#!/usr/bin/env bash
# Build the normal NAND-boot kernel/FIT. Never uploads or writes to a device.
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")/.." && pwd)
[[ $# == 1 ]] || { echo "Usage: $0 <FIT signing-key directory>" >&2; exit 2; }
signing=$(realpath -- "$1")
[[ -r "$signing/verified-boot.key" && -r "$signing/verified-boot.crt" ]] || exit 2
bash "$project/scripts/build-connector3-persistent-rootfs.sh" --release
source "$project/PrepareHost.sh"
jobs=${JOBS:-16}
[[ $jobs =~ ^[0-9]+$ && $jobs -ge 1 && $jobs -le 32 ]] || exit 2
exec 9>"$work/build.lock"
flock -n 9 || { echo 'Another firmware build is running.' >&2; exit 1; }
ram="$work/connector3-ram"
output="$work/connector3-persistent"
destination="$project/output/connector3-persistent"
board="$project/board/connector3/persistent"
inputs=$(sha256sum "$ram/images/zImage" | cut -c1-16)
linux="$output/linux-$inputs"
mkdir -p "$output" "$destination" "$project/logs"
if [[ ! -f "$linux/Makefile" ]]; then
    cp -a --reflink=auto "$ram/build/linux-6.12.112" "$linux"
fi
cp "$ram/build/linux-6.12.112/.config" "$linux/.config"
# Refresh the PM source even when the qualified audio RAM cache is reused.
install -m 0644 "$board/../ram/connector3-pm.c" "$linux/arch/arm/mach-imx/connector3-pm.c"
install -m 0644 "$board/../ram/galdrun-connector3-audio.c" "$linux/sound/soc/fsl/galdrun-connector3-audio.c"
# Migrate older caches away from the retired PCIe retention experiment.
python3 - "$linux/drivers/pci/controller/dwc/pci-imx6.c" <<'PY'
from pathlib import Path
import sys
path=Path(sys.argv[1]);text=path.read_text()
guard='''\t/* The Connector 3 suspend port retains the peripheral domain.
\t * Its 88W8897 survives device host-sleep, but not a root-port reset
\t * underneath the retained firmware/DMA state. Keep the link intact. */
\tif (of_machine_is_compatible("fsl,imx7d-teufel-streamer"))
\t\treturn 0;

'''
assert text.count(guard) in (0,2), 'Partial PCIe retention patch in build cache'
if guard in text: path.write_text(text.replace(guard,''))
PY
cp "$board/imx7d-galdrun-connector3.dts" "$board/../imx7d-galdrun-connector3-storage.dtsi" \
    "$board/../ram/dts/nxp/imx/imx7d-galdrun-connector3-ram.dts" \
    "$linux/arch/arm/boot/dts/nxp/imx/"
# Older qualified RAM caches predate the explicit radio sleep-clock consumer.
if ! grep -Fq 'devm_clk_get_enabled(dev, "wifi-32k")' "$linux/drivers/pci/controller/dwc/pci-imx6.c"; then
    patch -d "$linux" -p1 <"$board/../ram/patches/linux/0012-connector3-wifi-sleep-clock.patch"
fi
if ! grep -Fq 'Drain dump/reset work before invalidating' "$linux/drivers/net/wireless/marvell/mwifiex/pcie.c"; then
    patch -d "$linux" -p1 <"$board/../ram/patches/linux/0013-mwifiex-drain-work-before-unregister.patch"
fi
# Built-in PCIe/WLAN probes before the NAND root mounts; provide its approved
# hardware firmware through the kernel's standard built-in firmware loader.
firmware="$output/firmware"
mkdir -p "$firmware/mrvl" "$firmware/imx/sdma"
install -m 0644 "$project/private/connector3/pcie8897-working-reference.bin" "$firmware/mrvl/pcie8897_uapsta.bin"
install -m 0644 "$ram/target/lib/firmware/regulatory.db" "$ram/target/lib/firmware/regulatory.db.p7s" "$firmware/"
install -m 0644 "$ram/target/lib/firmware/imx/sdma/sdma-imx7d.bin" "$firmware/imx/sdma/"
printf '%s  %s\n' b2c5e7f4f88b061b92be8113ddd5ec688da94729c3a5d07bec9e334335499538 \
    "$firmware/mrvl/pcie8897_uapsta.bin" | sha256sum -c -
"$linux/scripts/config" --file "$linux/.config" \
    --enable SUSPEND --enable CPU_PM --enable ARM_PSCI_FW \
    --enable MTD --enable MTD_RAW_NAND --enable MTD_NAND_GPMI_NAND \
    --enable MTD_UBI --enable UBIFS_FS --enable UBIFS_FS_ZLIB \
    --disable MTD_BLOCK --disable MTD_PARTITIONED_MASTER --disable MTD_CMDLINE_PARTS \
    --enable USB_SUPPORT --enable USB --enable USB_EHCI_HCD --enable USB_CHIPIDEA \
    --enable USB_CHIPIDEA_HOST --enable NOP_USB_XCEIV --enable USB_STORAGE \
    --enable SCSI --enable BLK_DEV_SD --enable VFAT_FS \
    --enable NLS_CODEPAGE_437 --enable NLS_ISO8859_1 \
    --disable NFS_FS --disable NFSD --disable SUNRPC --disable LOCKD \
    --disable FUSE_FS --disable EXT2_FS --disable EXT3_FS --disable EXT4_FS \
    --set-str INITRAMFS_SOURCE '' --set-str LOCALVERSION '-galdrun-connector3' \
    --set-str EXTRA_FIRMWARE 'mrvl/pcie8897_uapsta.bin regulatory.db regulatory.db.p7s imx/sdma/sdma-imx7d.bin' \
    --set-str EXTRA_FIRMWARE_DIR "$firmware" \
    --set-str CMDLINE 'console=ttymxc0,115200n8 earlycon=ec_imx6q,0x30860000,115200 clk_ignore_unused maxcpus=1 panic=10 mwifiex.debug_mask=0x7 ubi.mtd=5,4096 root=ubi0:RootFS rootfstype=ubifs ro init=/init'
args=(-C "$linux" ARCH=arm "CROSS_COMPILE=$ram/host/bin/arm-linux-")
make "${args[@]}" olddefconfig >"$project/logs/connector3-persistent-kernel.log" 2>&1
make "${args[@]}" -j"$jobs" zImage nxp/imx/imx7d-galdrun-connector3.dtb \
    >>"$project/logs/connector3-persistent-kernel.log" 2>&1
if [[ -f "$destination/hardware-verification.json" ]]; then
    mv -f "$destination/hardware-verification.json" "$destination/hardware-verification.previous.json"
fi
python3 "$project/scripts/verify-connector3-persistent.py" "$linux" "$work/connector3-diagnostic" \
    "$signing" "$destination" "$project/output/connector3-persistent-rootfs"
