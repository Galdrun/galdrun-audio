#!/bin/sh
# Explicit SSH diagnostic: stops playback, sleeps, and arms a 30-second RTC
# fallback. Send an Ethernet magic packet from another machine to test WOL.
set -eu
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
grep -q 'fsl,imx7d-teufel-streamer' /proc/device-tree/compatible
grep -q '\[deep\]' /sys/power/mem_sleep
grep -q '\[none\]' /sys/power/pm_test
test "$(cat /sys/class/net/eth0/carrier)" = 1
test -z "$(cat /sys/class/rtc/rtc0/wakealarm)"
old_wol=$(ethtool eth0 | awk '/^[[:space:]]*Wake-on:/ {print $2}')
case "$old_wol" in d|g) ;; *) echo 'Unsupported wake policy' >&2; exit 1 ;; esac
pid=$(pidof galdrun-client || :)
mkdir /run/connector3-suspend-test.lock
restore() {
    result=$?
    trap - EXIT
    echo 0 >/sys/class/rtc/rtc0/wakealarm || result=1
    ethtool -s eth0 wol "$old_wol" || result=1
    if [ -n "$pid" ]; then
        export LD_LIBRARY_PATH=/opt/galdrun-client/lib
        nohup /opt/galdrun-client/galdrun-client --config /run/client.json \
            --audio-cache /run/audio_devices.json --log-file /run/galdrun-client.log \
            >>/run/galdrun-client-console.log 2>&1 </dev/null &
    fi
    rmdir /run/connector3-suspend-test.lock || result=1
    exit "$result"
}
trap restore EXIT
trap 'exit 1' HUP INT TERM
if [ -n "$pid" ]; then
    kill -TERM "$pid"
    for attempt in 1 2 3 4 5; do
        kill -0 "$pid" 2>/dev/null || break
        sleep 1
    done
    if kill -0 "$pid" 2>/dev/null; then pid=; echo 'Client did not stop' >&2; exit 1; fi
fi
ethtool -s eth0 wol g
echo +30 >/sys/class/rtc/rtc0/wakealarm
sync
echo mem >/sys/power/state
echo "Wake IRQ: $(cat /sys/power/pm_wakeup_irq)"
dmesg | tail -25
