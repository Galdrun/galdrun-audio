#!/bin/sh
# Runs on Galdrun Buildroot appliances (BusyBox sh). Connector uploaders copy it
# together with /tmp/galdrun-client.tar, the matching ARM release. Not meant to be run by hand.
# Replaces the client only; settings, room ID and the firmware stay as they are.
set -eu
dir=/opt/galdrun-client
next=$dir/.next
archive=/tmp/galdrun-client.tar
trap 'rm -rf "$next" "$archive" "$0"' EXIT
grep -Eq '"(raumfeld-connector2|raspberrypi-rpi1|raspberrypi-rpi3|raspberrypi-rpi4|raspberrypi-rpi5)"' /etc/galdrun-board.json 2>/dev/null ||
    { echo 'This device is not running a supported Galdrun Buildroot image.' >&2; exit 1; }
if [ "${1:-}" = --stdin ]; then
    # Generic Linux uploaders stream gzip; BusyBox images have neither bash nor systemd.
    sed '1s/^\xef\xbb\xbf//' | gzip -dc > "$archive"
fi

rm -rf "$next"
mkdir "$next"
tar -xf "$archive" -C "$next"
# Archives made on Windows carry no executable bit.
chmod 755 "$next/galdrun-client"
# Refuse a binary that does not start here before touching the running client.
version=$(LD_LIBRARY_PATH=$next/lib "$next/galdrun-client" --version)
echo "New client: $version"

/etc/init.d/S95galdrun-client stop
cp -p "$dir/galdrun-client" "$dir/galdrun-client.previous"
if ! { cp "$next/galdrun-client" "$dir/galdrun-client" && cp -R "$next/lib/." "$dir/lib/" &&
        cp "$next/licenses/"* "$dir/licenses/" && cp "$next/build-info.txt" "$dir/build-info.txt"; }; then
    cp -p "$dir/galdrun-client.previous" "$dir/galdrun-client"
    /etc/init.d/S95galdrun-client start
    echo 'Copying failed; the previous client was restored and started.' >&2
    exit 1
fi
/etc/init.d/S95galdrun-client start
sleep 2
/etc/init.d/S95galdrun-client status || { echo 'The new client did not stay running; see /run/log/galdrun-client.log.' >&2; exit 1; }
echo "Client updated and running. Previous binary: $dir/galdrun-client.previous"
