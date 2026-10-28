#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t tmpfs tmpfs /run
mount -t tmpfs tmpfs /tmp
exec 0</dev/console 1>/dev/console 2>/dev/console
hostname connector3-diag
echo 'GALDRUN CONNECTOR 3 RAM DIAGNOSTICS - no storage drivers or installer'
ip link set lo up

# Network startup must not delay SSH key generation. Retry DHCP until linked.
(
    while :; do
        for path in /sys/class/net/eth*; do
            [ -e "$path" ] || continue
            interface=${path##*/}
            ip link set "$interface" up
            udhcpc -f -n -q -t 5 -T 3 -i "$interface" \
                -x hostname:connector3-diag -s /usr/share/udhcpc/default.script && exit
        done
        sleep 5
    done
) >/run/dhcp.log 2>&1 &

mkdir -p /run/sshd
ssh-keygen -q -t ed25519 -N '' -f /etc/ssh/ssh_host_ed25519_key
/usr/sbin/sshd -E /run/sshd.log
while :; do
    /bin/sh
    sleep 1
done
