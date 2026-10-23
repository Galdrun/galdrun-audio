#!/usr/bin/env bash
# esp32.sh - Flash an ESP32 client release to a board over USB serial (esptool).
# The ESP32 has no SSH or network update; this needs the USB cable. Windows version: esp32.ps1.
# Under WSL, attach the USB serial device first (usbipd-win) or use esp32.ps1 on Windows.
#
# Usage:
#   scripts/upload/esp32.sh
#   scripts/upload/esp32.sh --port /dev/ttyUSB0 --baud 115200
#   scripts/upload/esp32.sh --board esp32-s3-devkitc --port /dev/ttyACM0
#
# Options:
#   --port   serial port of the board; may be left out when exactly one is present.
#   --board  board name under releases/client (default esp32-a1s); must be built first.
#   --baud   flash baud rate (default 460800; drop to 115200 when a cable is unreliable).
#
# What it does: checks the port and the release images, downloads Espressif's standalone
# esptool once (checksum-verified, into ~/.cache/galdrun), probes the chip first (flash_id)
# so a wrong port or dead wiring fails before anything is written, flashes bootloader,
# partition table and app at the offsets in flash_args, then hard-resets the board. The
# storage partition is not flashed; the client formats it on first start. Opening the port
# needs membership in the dialout group (sudo usermod -aG dialout $USER, then log in again).
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
port='' board=esp32-a1s baud=460800
while (( $# )); do
    case $1 in
        --port) port=${2:?}; shift 2 ;;
        --board) board=${2:?}; shift 2 ;;
        --baud) baud=${2:?}; shift 2 ;;
        *) echo "Unknown option: $1 (see the top of $0)" >&2; exit 2 ;;
    esac
done
release="$root/releases/client/$board"
[[ -f $release/flash_args ]] || { echo "No release for '$board' under releases/client - build it first: scripts/build-client.sh $board" >&2; exit 1; }
[[ $baud =~ ^[0-9]+$ ]] && (( baud >= 9600 && baud <= 921600 )) || { echo 'Baud must be between 9600 and 921600.' >&2; exit 2; }
shopt -s nullglob
ports=(/dev/ttyUSB* /dev/ttyACM*)
[[ -n $port || ${#ports[@]} -ne 1 ]] || port=${ports[0]}
[[ -n $port && -e $port ]] || { echo "Give --port. Available: ${ports[*]:-none}" >&2; exit 2; }
chip=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["extra_esptool_args"]["chip"])' "$release/flasher_args.json")

# Standalone esptool, no Python packages needed. v4 matches the flash_args ESP-IDF 5.5 writes.
version=v4.12.0
dir="$HOME/.cache/galdrun/esptool-$version"
esptool="$dir/esptool-linux-amd64/esptool"
if [[ ! -x $esptool ]]; then
    [[ $(uname -m) == x86_64 ]] || { echo 'Install esptool yourself (pip install esptool) on this architecture.' >&2; exit 1; }
    archive=$(mktemp)
    trap 'rm -f "$archive"' EXIT
    echo "Downloading esptool $version ..."
    curl --fail --location --retry 3 -o "$archive" \
        "https://github.com/espressif/esptool/releases/download/$version/esptool-$version-linux-amd64.tar.gz"
    echo "fd92bfde850baa20c4090457b317c767df22878600a567237b092f9b151a5161  $archive" | sha256sum -c --quiet -
    mkdir -p "$dir"
    tar -xzf "$archive" -C "$dir"
fi

echo "Probing $chip on $port ..."
"$esptool" --chip "$chip" -p "$port" --before default_reset --after hard_reset flash_id ||
    { echo "No $chip answered on $port - check port, cable and permissions." >&2; exit 1; }
echo "Flashing $board to $port at $baud baud ..."
(cd "$release" && "$esptool" --chip "$chip" -p "$port" -b "$baud" --before default_reset --after hard_reset write_flash @flash_args)
echo 'Done. The board was reset and now runs the client; see client/esp32/README.md for Wi-Fi setup.'
