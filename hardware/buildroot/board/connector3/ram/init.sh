#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t tmpfs tmpfs /run
mount -t tmpfs tmpfs /tmp
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts
exec 0</dev/console 1>/dev/console 2>/dev/console
hostname connector3-ram
echo 'GALDRUN CONNECTOR 3 RAM TEST - no persistent storage drivers' >/dev/kmsg
ip link set lo up

# There is no usable UART yet. Recover the installed beta if networking fails.
(
    sleep 120
    if ! ip -4 -o addr show scope global | grep -q ' inet ' || ! pidof sshd >/dev/null; then
        echo 'RAM test has no network address or SSH service; returning to installed firmware.' >/dev/kmsg
        for log in /run/system.log /run/sshd.log; do
            [ -r "$log" ] || continue
            while IFS= read -r line; do
                printf '%s: %s\n' "$log" "$line" >/dev/kmsg
            done <"$log"
        done
        reboot -f
    fi
) &

syslogd -O /run/system.log
/etc/init.d/S45raumfeld-network start

# Retain clock/reset diagnostics for a warm-reboot recovery without a UART.
mount -t debugfs debugfs /sys/kernel/debug
(
    sleep 15
    {
        echo 'RAM clock/reset snapshot'
        grep -E 'enet2|enet_axi|pll_enet|pcie|clko2' /sys/kernel/debug/clk/clk_summary
        cat /sys/kernel/debug/gpio
        for address in 0x30340004 0x30340030 0x3038a800 0x3038a700 0x303300a4 0x30330570 0x30230000 0x30230004 0x30240000 0x30240004; do
            echo "$address $(devmem "$address")"
        done
    } 2>&1 | while IFS= read -r line; do
        printf '%s\n' "$line" >/dev/kmsg
    done
) &

mkdir -p /run/sshd
[ -s /etc/ssh/ssh_host_ed25519_key ] ||
    ssh-keygen -q -t ed25519 -N '' -f /etc/ssh/ssh_host_ed25519_key </dev/null
/usr/sbin/sshd -E /run/sshd.log

cp /opt/galdrun-client/client-default.json /run/client.json
/usr/sbin/galdrun-connector3-mcu /dev/ttymxc2 >/run/mcu-buttons.log 2>&1 &
echo $! >/run/mcu-buttons.pid
for attempt in 1 2 3 4 5; do
    [ -S /run/galdrun-connector3-mcu.sock ] && break
    sleep 1
done
export LD_LIBRARY_PATH=/opt/galdrun-client/lib
/opt/galdrun-client/galdrun-client --config /run/client.json \
    --audio-cache /run/audio_devices.json --log-file /run/galdrun-client.log \
    >/run/galdrun-client-console.log 2>&1 &

while :; do
    /bin/sh
    sleep 1
done
