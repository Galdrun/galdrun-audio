#!/usr/bin/env bash
set -euo pipefail
[[ -z ${GALDRUN_SSH_PUBKEY:-} ]] || { echo 'Pi release images must not contain developer keys' >&2; exit 1; }
case ${RAUMFELD_BOARD:?} in
    rpi1) export GALDRUN_CLIENT_TARGET=linux-armv6; cpu=arm1176 ;;
    rpi3) export GALDRUN_CLIENT_TARGET=linux-armv7; cpu=cortex-a7 ;;
    *) echo 'Unknown Raspberry Pi board' >&2; exit 1 ;;
esac
source "${RAUMFELD_PROJECT_DIR:?}/board/common/post-build.sh"
# Device modaliases, not the Connector's fixed module list, load Pi drivers.
rm -f "$target/etc/init.d/S02raumfeld-led-boot" "$target/etc/init.d/S40raumfeld-hardware" \
    "$target/etc/init.d/S96raumfeld-status-leds" "$target/usr/sbin/raumfeld-hardware-check" \
    "$target/etc/asound.conf" "$target/usr/sbin/raumfeld-suspend" "$target/etc/galdrun-cpu.conf"
mkdir -p "$target/boot"
install -m 0755 "$project/board/rpi/galdrun-audio-profile" "$target/usr/sbin/galdrun-audio-profile"
install -m 0644 "$project/board/rpi/audio-profiles.json" "$target/usr/share/galdrun-audio-profiles.json"
python3 - "$project/board/rpi/audio-profiles.json" "$target/usr/share/galdrun-audio-profiles.tsv" <<'PY'
import json, sys
from pathlib import Path
profiles = json.loads(Path(sys.argv[1]).read_text())
Path(sys.argv[2]).write_text(''.join(p['id'] + '\t' + p['overlay'] + '\n' for p in profiles))
PY
toolchain="$target/usr/share/licenses/toolchain-external-bootlin"
install -D -m 0644 "$BUILD_DIR/linux-custom/LICENSES/preferred/LGPL-2.1" "$toolchain/glibc-LGPL-2.1.txt"
install -m 0644 "$release/licenses/GPL-3.0.txt" "$release/licenses/GCC-RUNTIME-EXCEPTION.txt" "$toolchain/"
install -m 0644 "$HOST_DIR/opt/ext-toolchain/summary.csv" "$toolchain/summary.csv"
# Check the actual client, its ARM ISA, loader, glibc, ALSA and libatomic together.
# The Pi 1 check runs on an ARM1176, so an accidental ARMv7 library fails here.
command -v qemu-arm >/dev/null || { echo 'Install qemu-user for the client ABI check' >&2; exit 1; }
qemu-arm -cpu "$cpu" -L "$target" -E LD_BIND_NOW=1 -E "LD_LIBRARY_PATH=$target/opt/galdrun-client/lib" \
    "$target/opt/galdrun-client/galdrun-client" --version
