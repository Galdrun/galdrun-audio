#!/bin/sh
# Run on the Connector 3 RAM image over SSH. Plays silence and restores its client.
set -eu
grep -q 'fsl,imx7d-teufel-streamer' /proc/device-tree/compatible
grep -q Connector3 /proc/asound/cards
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
runner= watchdog= restart_client=0
cleanup() {
    result=$?
    trap - EXIT INT TERM
    [ -z "$runner" ] || kill "$runner" 2>/dev/null || :
    [ -z "$watchdog" ] || kill "$watchdog" 2>/dev/null || :
    if [ "$result" != 0 ]; then cat /run/aplay-native.log >&2; fi
    if [ "$restart_client" = 1 ]; then
        export LD_LIBRARY_PATH=/opt/galdrun-client/lib
        nohup /opt/galdrun-client/galdrun-client --config /run/client.json \
            --audio-cache /run/audio_devices.json --log-file /run/galdrun-client.log \
            >>/run/galdrun-client-console.log 2>&1 </dev/null &
    fi
}
trap cleanup EXIT INT TERM
pid=$(pidof galdrun-client || :)
if [ -n "$pid" ]; then
    restart_client=1
    kill -TERM "$pid"
    sleep 3
fi
for rate in 44100 48000 44100 22050 32000 88200 96000 176400 192000; do
    echo "Testing native $rate Hz stereo S16_LE (silence)"
    aplay -D hw:CARD=Connector3,DEV=0 -t raw -f S16_LE -c 2 -r "$rate" -d 3 \
        /dev/zero >/run/aplay-native.log 2>&1 &
    runner=$!
    (sleep 8; kill "$runner" 2>/dev/null || :) &
    watchdog=$!
    sleep 1
    grep -q "^rate: $rate " /proc/asound/card0/pcm0p/sub0/hw_params
    grep -q '^state: RUNNING' /proc/asound/card0/pcm0p/sub0/status
    before=$(awk '/hw_ptr/ {print $3}' /proc/asound/card0/pcm0p/sub0/status)
    sleep 1
    after=$(awk '/hw_ptr/ {print $3}' /proc/asound/card0/pcm0p/sub0/status)
    test "$after" -gt "$before"
    echo "hw_ptr $before -> $after"
    grep -E 'pll_audio_main |audio_mclk_post_div|sai1_post_div' /sys/kernel/debug/clk/clk_summary
    wait "$runner"
    runner=
    kill "$watchdog" 2>/dev/null || :
    wait "$watchdog" 2>/dev/null || :
    watchdog=
    echo "PASS $rate Hz"
done
echo 'All advertised rates and repeated 44.1/48 family switching passed.'
