#!/bin/sh
# PID 1 of the USB installer. Finds connect2.img on the USB stick, unpacks the
# payload behind its first 0x840000 bytes and installs it. With
# galdrun.install=check on the command line it only runs the preflight checks.
# Any failure ends in a shell on the UART; PID 1 must not exit.
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t tmpfs tmpfs /tmp
exec 0</dev/console 1>/dev/console 2>/dev/console

shell() {
    echo "INSTALLER STOPPED: $*"
    exec /bin/sh
}

echo 'GALDRUN CONNECTOR 2 USB INSTALLER'
mode=--install
grep -q 'galdrun.install=check' /proc/cmdline && mode=--check

image=
tries=0
while [ -z "$image" ] && [ "$tries" -lt 30 ]; do
    for device in /dev/sd[a-z][0-9] /dev/sd[a-z]; do
        [ -b "$device" ] || continue
        mount -t vfat -o ro "$device" /media 2>/dev/null || continue
        if [ -f /media/connect2.img ]; then image=/media/connect2.img; break; fi
        umount /media
    done
    [ -n "$image" ] || { sleep 1; tries=$((tries + 1)); }
done
[ -n "$image" ] || shell 'no USB stick with connect2.img found'
echo "Payload: $image"

# 0x840000 / 4096: the payload tar follows the kernel and the DTB store.
dd if="$image" bs=4096 skip=2112 2>/dev/null | tar -xf - -C /payload || shell 'the payload cannot be unpacked'
[ -f /payload/payload.md5 ] || shell 'connect2.img holds no Galdrun payload'

if [ "$mode" = --check ]; then
    /usr/sbin/galdrun-install --check
    shell 'check mode: nothing was written'
fi
/usr/sbin/galdrun-start || shell 'installation failed'
shell 'unexpected return from the installer'
