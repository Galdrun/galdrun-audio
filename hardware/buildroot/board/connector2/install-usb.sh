#!/bin/sh
# Runs in the USB installer's initramfs (installer/init.sh unpacked the payload
# to /payload), not in the installed firmware.
set -eu
case "${1:-}" in
    --check|--install) action=$1 ;;
    *) echo 'Usage: galdrun-install {--check|--install}' >&2; exit 2 ;;
esac

fail() { echo "INSTALL REFUSED: $*" >&2; exit 1; }
model=$(tr -d '\000' < /proc/device-tree/model)
[ "$model" = 'Custom Raumfeld Connector 2' ] || fail "Unexpected model: $model"
[ "$(grep -c '^mtd[0-9]' /proc/mtd)" = 9 ] || fail 'Unexpected partition count'
for entry in \
    'mtd0: 00020000 00020000 "SPL"' \
    'mtd1: 00020000 00020000 "SPL.backup1"' \
    'mtd2: 00020000 00020000 "SPL.backup2"' \
    'mtd3: 00020000 00020000 "Firmware Data"' \
    'mtd4: 00100000 00020000 "U-Boot"' \
    'mtd5: 00020000 00020000 "U-Boot Env"' \
    'mtd6: 00600000 00020000 "Kernel"' \
    'mtd7: 00040000 00020000 "DTB"' \
    'mtd8: 0f820000 00020000 "File System"'; do
    grep -Fqx "$entry" /proc/mtd || fail "Partition mismatch: $entry"
done
[ "$(cat /sys/class/mtd/mtd8/writesize)" = 2048 ] || fail 'Unexpected NAND page size'
[ "$(cat /sys/class/mtd/mtd8/oobsize)" = 128 ] || fail 'Unexpected NAND OOB size'
if grep -q ' ubifs ' /proc/mounts; then fail 'An installed UBIFS volume is already mounted'; fi
for tool in md5sum ubidetach ubiformat ubiattach ubimkvol flash_erase nandwrite nanddump gunzip tar head cmp; do
    command -v "$tool" >/dev/null || fail "Missing installer tool: $tool"
done
(cd /payload && md5sum -c payload.md5) || fail 'Payload checksum mismatch'
kernel_size=$(wc -c < /payload/kernel.uImage)
dtb_size=$(wc -c < /payload/dts.cramfs)
[ "$kernel_size" -gt 0 ] && [ "$kernel_size" -le 6291456 ] || fail 'Kernel exceeds mtd6'
[ "$dtb_size" -gt 0 ] && [ "$dtb_size" -le 262144 ] || fail 'DTB store exceeds mtd7'
gunzip -t /payload/rootfs.tgz || fail 'Invalid compressed root filesystem archive'
gunzip -c /payload/rootfs.tgz | tar -tf - >/dev/null || fail 'Invalid root filesystem archive'
echo 'PREFLIGHT PASSED: Connector 2 geometry and firmware/client payload verified.'
[ "$action" = --install ] || exit 0

echo 'INSTALLING: replacing mtd6 (kernel), mtd7 (DTB), mtd8 (RootFS/Updates).'
echo 'The installer preserves mtd0-mtd5. Keep power connected until INSTALL COMPLETE.'
md5sum /dev/mtd0 /dev/mtd1 /dev/mtd2 /dev/mtd3 /dev/mtd4 /dev/mtd5 > /tmp/galdrun-protected.md5
mounted=0
led_pid=
led_set() {
    # Indicators are best effort; an absent LED must not abort installation.
    echo none > /sys/class/leds/raumfeld:1/trigger 2>/dev/null || :
    echo none > /sys/class/leds/raumfeld:2/trigger 2>/dev/null || :
    echo "$1" > /sys/class/leds/raumfeld:1/brightness 2>/dev/null || :
    echo "$2" > /sys/class/leds/raumfeld:2/brightness 2>/dev/null || :
}
led_pattern() {
    while [ -d /sys/class/leds/raumfeld:1 ] && [ -d /sys/class/leds/raumfeld:2 ]; do
        if [ "$1" = error ]; then
            led_set 1 1; sleep 0.2 || sleep 1
            led_set 1 0; sleep 0.2 || sleep 1
        else
            led_set 1 0; sleep 1
            led_set 0 1; sleep 1
        fi
    done
}
led_start() {
    (
        # Close inherited output permanently and leave firmware cleanup to its parent.
        trap - EXIT INT TERM
        exec </dev/null >/dev/null 2>&1
        led_pattern "$1"
    ) &
    led_pid=$!
}
cleanup() {
    result=$?
    if [ -n "$led_pid" ]; then
        kill "$led_pid" 2>/dev/null || :
        wait "$led_pid" 2>/dev/null || :
    fi
    if [ "$mounted" = 1 ]; then umount /mnt || true; fi
    rm -f /tmp/galdrun-nand-readback.bin
    if [ -d /sys/class/leds/raumfeld:1 ] && [ -d /sys/class/leds/raumfeld:2 ]; then
        if [ "$result" = 0 ]; then led_set 1 1
        else led_start error
        fi
    fi
}
trap cleanup EXIT
trap 'exit 1' INT TERM
if [ -d /sys/class/leds/raumfeld:1 ] && [ -d /sys/class/leds/raumfeld:2 ]; then
    led_start update
fi

# Match the vendor's subpage/VID geometry and let Linux handle bad blocks.
ubidetach /dev/ubi_ctrl -d 0 || true
ubiformat -q /dev/mtd8 -s 512 -O 2048 -y
ubiattach /dev/ubi_ctrl -d 0 -m 8 -O 2048
ubimkvol /dev/ubi0 -N RootFS -s 191MiB
ubimkvol /dev/ubi0 -N Updates -s 42MiB
mkdir -p /mnt
mount -t ubifs ubi0:RootFS /mnt
mounted=1
gunzip -c /payload/rootfs.tgz | tar -xf - -C /mnt
sync
(cd /mnt && md5sum -c /payload/rootfs-files.md5) || fail 'Installed root filesystem readback failed'

write_partition() {
    number=$1
    image=$2
    length=$(wc -c < "$image")
    flash_erase -q "/dev/mtd$number" 0 0
    nandwrite -q --pad "/dev/mtd$number" "$image"
    nanddump -q --omitoob --bb=skipbad -l "$length" -f /tmp/galdrun-nand-readback.bin "/dev/mtd$number"
    head -c "$length" /tmp/galdrun-nand-readback.bin | cmp - "$image" || fail "mtd$number readback failed"
}
write_partition 7 /payload/dts.cramfs
write_partition 6 /payload/kernel.uImage
rm /mnt/boot/uImage
sync
umount /mnt
mounted=0
md5sum -c /tmp/galdrun-protected.md5 || fail 'Protected partition checksum changed'
echo 'INSTALL COMPLETE: firmware and client installed; mtd0-mtd5 verified unchanged by this installer.'
echo 'Firmware ready for normal boot.'
