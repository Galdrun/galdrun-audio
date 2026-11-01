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
echo 'GALDRUN CONNECTOR 3 USB PREFLIGHT - writes require an explicit command'
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
) >/run/preflight.log 2>&1 &
while :; do /bin/sh; sleep 1; done
