#!/bin/sh
# Explicit diagnostic using the image's qualified helper, with RTC recovery.
# Send an Ethernet magic packet or tap a top touch control to test early wake.
set -eu
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
grep -q 'fsl,imx7d-teufel-streamer' /proc/device-tree/compatible
grep -q '\[deep\]' /sys/power/mem_sleep
grep -q '\[none\]' /sys/power/pm_test
test -x /usr/sbin/raumfeld-suspend
test -z "$(cat /sys/class/rtc/rtc0/wakealarm)"
mkdir /run/connector3-suspend-test.lock
restore() {
    result=$?
    trap - EXIT
    echo 0 >/sys/class/rtc/rtc0/wakealarm || result=1
    rmdir /run/connector3-suspend-test.lock || result=1
    exit "$result"
}
trap restore EXIT
trap 'exit 1' HUP INT TERM
echo +45 >/sys/class/rtc/rtc0/wakealarm
before=$(cat /sys/class/rtc/rtc0/since_epoch)
result=0
/usr/sbin/raumfeld-suspend || result=$?
after=$(cat /sys/class/rtc/rtc0/since_epoch)
echo "Sleep helper result: $result; RTC elapsed seconds: $((after - before))"
echo "Wake IRQ: $(cat /sys/power/pm_wakeup_irq)"
dmesg | tail -25
exit "$result"
