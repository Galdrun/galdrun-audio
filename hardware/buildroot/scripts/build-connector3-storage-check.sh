#!/usr/bin/env bash
# Reuse the audited USB diagnostic userspace for a read-only NAND RAM test.
# No vendor filesystem/binaries, USB installer or hardware writes.
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")/.." && pwd)
source "$project/PrepareHost.sh"
jobs=${JOBS:-16}
[[ $jobs =~ ^[0-9]+$ && $jobs -ge 1 && $jobs -le 32 ]] || exit 2
exec 9>"$work/build.lock"
flock -n 9 || { echo 'Another firmware build is running.' >&2; exit 1; }
diagnostic="$work/connector3-diagnostic"
python3 "$project/scripts/verify-connector3-diagnostic.py" "$diagnostic"
output="$work/connector3-storage-check"
board="$project/board/connector3/storage-check"
inputs=$(sha256sum "$diagnostic/images/zImage" "$board/readonly-nand.patch" | sha256sum | cut -c1-16)
linux="$output/linux-$inputs"
destination="$project/output/connector3-storage-check"
mkdir -p "$output" "$destination" "$project/logs"
if [[ ! -f "$linux/Makefile" ]]; then
    cp -a --reflink=auto "$diagnostic/build/linux-6.12.112" "$linux"
fi
if ! grep -q 'NAND write refused: read-only qualification' "$linux/drivers/mtd/nand/raw/gpmi-nand/gpmi-nand.c"; then
    patch -d "$linux" -p1 <"$board/readonly-nand.patch"
fi
cp -p "$diagnostic/images/rootfs.cpio" "$output/rootfs.cpio"
cp "$diagnostic/build/linux-6.12.112/.config" "$linux/.config"
cp "$board/imx7d-galdrun-connector3-storage-check.dts" "$linux/arch/arm/boot/dts/nxp/imx/"
"$linux/scripts/config" --file "$linux/.config" \
    --enable MTD --enable MTD_RAW_NAND --enable MTD_NAND_GPMI_NAND \
    --enable MTD_UBI --enable UBIFS_FS --disable MTD_BLOCK \
    --enable USB_SUPPORT --enable USB --enable USB_EHCI_HCD \
    --enable USB_CHIPIDEA --enable USB_CHIPIDEA_HOST --enable NOP_USB_XCEIV \
    --enable USB_STORAGE --enable SCSI --enable BLK_DEV_SD \
    --enable VFAT_FS --enable NLS_CODEPAGE_437 --enable NLS_ISO8859_1 \
    --set-str LOCALVERSION '-galdrun-storage-ro' --set-str INITRAMFS_SOURCE "$output/rootfs.cpio"
args=(-C "$linux" ARCH=arm "CROSS_COMPILE=$diagnostic/host/bin/arm-linux-")
make "${args[@]}" olddefconfig >"$project/logs/connector3-storage-check.log" 2>&1
make "${args[@]}" -j"$jobs" zImage nxp/imx/imx7d-galdrun-connector3-storage-check.dtb \
    >>"$project/logs/connector3-storage-check.log" 2>&1
"$diagnostic/host/bin/arm-linux-gcc" -std=c11 -D_GNU_SOURCE -Os -Wall -Wextra -Werror \
    "$board/attach-readonly.c" -o "$destination/attach-readonly"
python3 "$project/scripts/verify-connector3-storage-check.py" "$output" "$diagnostic" "$linux"
if [[ -f "$destination/hardware-verification.json" ]]; then
    mv -f "$destination/hardware-verification.json" "$destination/hardware-verification.previous.json"
fi
install -m 0644 "$linux/arch/arm/boot/zImage" "$destination/zImage"
install -m 0644 "$linux/arch/arm/boot/dts/nxp/imx/imx7d-galdrun-connector3-storage-check.dtb" "$destination/connector3.dtb"
install -m 0644 "$output/storage-verification.json" "$destination/"
cd "$destination"
sha256sum zImage connector3.dtb >SHA256SUMS
printf 'Read-only NAND RAM test components (not an installer): %s\n' "$destination"
