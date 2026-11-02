#!/bin/sh
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
umask 077
mount -t proc proc /proc
grep -q ' /dev devtmpfs ' /proc/mounts || mount -t devtmpfs devtmpfs /dev
mount -t sysfs sysfs /sys
mount -t tmpfs tmpfs /run
mount --bind /run /var/run
mount -t tmpfs tmpfs /tmp
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts
exec 0</dev/console 1>/dev/console 2>/dev/console

failed() {
    echo "Persistent startup stopped: $*"
    while :; do /bin/sh; sleep 1; done
}

# The kernel must attach only mtd5; never format or attach factory settings.
test "$(cat /sys/class/ubi/ubi0/mtd_num)" = 5 || failed 'unexpected UBI device'
grep -Eq '^ubi0:RootFS / ubifs ro(,| )' /proc/mounts || failed 'rootfs must be read-only'
mount -t ubifs ubi0:GaldrunSettings /settings || failed 'settings mount failed'
# ponytail: keep all /etc for the first-install candidate; migrate selected
# settings when the persistent firmware update format is implemented.
# Seed atomically so a power loss cannot promote a half-copied /etc tree.
if [ ! -d /settings/etc ]; then
    rm -rf /settings/etc.new
    cp -a /etc /settings/etc.new || failed 'settings seed failed'
    sync
    mv /settings/etc.new /settings/etc || failed 'settings seed commit failed'
    sync
fi
if [ ! -s /settings/client.json ]; then
    cp /opt/galdrun-client/client-default.json /settings/client.json.new || failed 'client seed failed'
    mv /settings/client.json.new /settings/client.json || failed 'client seed commit failed'
    sync
fi
mount --bind /settings/etc /etc || failed 'settings bind failed'

hostname connector3
ip link set lo up
syslogd -O /run/system.log
/etc/init.d/S45raumfeld-network start
# Waits for the radio firmware the network start loads.
/usr/sbin/galdrun-bluetooth start >/run/bluetooth-start.log 2>&1 &
mkdir -p /run/sshd
/etc/init.d/S50sshd start || failed 'SSH startup failed'
sync
/usr/sbin/galdrun-connector3-mcu /dev/ttymxc2 >/run/mcu-buttons.log 2>&1 &
echo $! >/run/mcu-buttons.pid
for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    [ -S /run/galdrun-connector3-mcu.sock ] && break
    sleep 1
done
[ -S /run/galdrun-connector3-mcu.sock ] || failed 'MCU audio initialization failed; client not started'
export LD_LIBRARY_PATH=/opt/galdrun-client/lib
/usr/sbin/galdrun-status-leds /settings/client.json >/run/status-leds.log 2>&1 &
/opt/galdrun-client/galdrun-client --config /settings/client.json \
    --audio-cache /settings/audio_devices.json --log-file /run/galdrun-client.log \
    >/run/galdrun-client-console.log 2>&1 &
while :; do /bin/sh; sleep 1; done
