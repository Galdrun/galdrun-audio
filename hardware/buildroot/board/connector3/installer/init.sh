#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
umask 077
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t tmpfs tmpfs /run
mount -t tmpfs tmpfs /tmp
exec 0</dev/console 1>/dev/console 2>/dev/console
hostname connector3-installer
ip link set lo up
echo 'GALDRUN CONNECTOR 3 USB INSTALLER - a write image installs itself after the preflight'
# Front lights through the MCU (best effort, like Connector 2's installer
# LEDs): 4 starting/working, 10 done (just before the reboot), 22 failed.
lights() { /usr/sbin/galdrun-connector3-mcu --indication /dev/ttymxc2 "$1" >/dev/null 2>&1 || :; }
lights 4 &
(
    while :; do
        ip link set eth0 up
        udhcpc -f -n -q -t 5 -T 3 -i eth0 -x hostname:connector3-installer \
            -s /usr/share/udhcpc/default.script && exit
        sleep 5
    done
) >/run/dhcp.log 2>&1 &
mkdir -p /run/sshd
ssh-keygen -q -t ed25519 -N '' -f /etc/ssh/ssh_host_ed25519_key
/usr/sbin/sshd -E /run/sshd.log -o PidFile=/run/sshd.pid
(
    found=0
    for attempt in $(seq 1 30); do
        for device in /dev/sd[a-z][0-9] /dev/sd[a-z]; do
            [ -b "$device" ] || continue
            mount -t vfat -o ro "$device" /media 2>/dev/null || continue
            if [ -f /media/connect3.img ]; then found=1; break; fi
            umount /media
        done
        [ "$found" = 0 ] || break
        sleep 1
    done
    [ "$found" = 1 ] || { echo 'PREFLIGHT STOPPED: USB image not found'; exit 1; }
    # Authenticate the complete appended archive before extracting anything.
    dd if=/media/connect3.img of=/run/payload.tar bs=4096 skip=3072 2>/dev/null || exit 1
    sha256sum -c /etc/galdrun-payload.sha256 || exit 1
    tar -xf /run/payload.tar -C /payload || exit 1
    /usr/sbin/galdrun-install --check
) >/run/preflight.log 2>&1
# A write image installs by itself, like Connector 2: no backup, the install,
# then a reboot into the new firmware (without Reset held the bootloader
# starts the NAND). If a write fails it stays here; boot the stick again. The
# log ends up on the stick as galdrun-install.log. A check image stops after
# the preflight. SSH stays up throughout for inspection; `galdrun-install
# --backup` and `--install PIN` remain for a manual install with rollback.
if grep -q 'PREFLIGHT PASSED' /run/preflight.log && [ -f /etc/galdrun-install-writes-enabled ]; then
    (
        /usr/sbin/galdrun-install --install
    ) >/run/install.log 2>&1
    result=$?
    # The full log (progress lines, ~160 KB) goes to the stick; the 115200 baud
    # console gets the end, which a reset would otherwise cut off.
    tail -n 15 /run/install.log
    mount -o remount,rw /media && cp /run/install.log /media/galdrun-install.log && sync
    mount -o remount,ro /media
    if [ "$result" = 0 ]; then
        lights 10
        echo 'INSTALLED: rebooting into the new firmware'
        umount /media || :
        sync
        reboot -f
        # Only reached when that reset did not happen: force it.
        sleep 5
        echo 'INSTALLED: reboot did not happen; forcing it'
        echo b >/proc/sysrq-trigger
    fi
    lights 22
    echo "INSTALL FAILED ($result): see /run/install.log; SSH stays available"
elif [ -f /etc/galdrun-install-writes-enabled ]; then
    lights 22
fi
while :; do /bin/sh; sleep 1; done
