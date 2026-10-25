#!/usr/bin/env bash
set -euo pipefail
[[ -z ${GALDRUN_SSH_PUBKEY:-} ]] || { echo 'Pi release images must not contain developer keys' >&2; exit 1; }
case ${RAUMFELD_BOARD:?} in
    rpi1) export GALDRUN_CLIENT_TARGET=linux-armv6; cpu=arm1176; qemu=qemu-arm ;;
    rpi3) export GALDRUN_CLIENT_TARGET=linux-armv7; cpu=cortex-a7; qemu=qemu-arm ;;
    rpi4) export GALDRUN_CLIENT_TARGET=linux-armv8; cpu=cortex-a72; qemu=qemu-aarch64 ;;
    rpi5) export GALDRUN_CLIENT_TARGET=linux-armv8; cpu=cortex-a76; qemu=qemu-aarch64 ;;
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
python3 - "$target/opt/galdrun-client" "$project/board/rpi/inputs.json" "$target/etc/galdrun-board.json" "$target/usr/share/galdrun-inputs.json" <<'PY'
import json, sys
from pathlib import Path
client = Path(sys.argv[1])
board = json.loads(Path(sys.argv[2]).read_text())
flags = json.loads(Path(sys.argv[3]).read_text())
board['inputGpioChip'] = flags.get('inputGpioChip', board['inputGpioChip'])
Path(sys.argv[4]).write_text(json.dumps(board, indent=2) + '\n')
defaults = json.loads((client / 'client-default.json').read_text())
defaults['inputGpioChip'] = board['inputGpioChip']
for header in (26, 40):
    defaults['inputs'] = [i for i in board['inputs']
                          if header == 40 or i['id'] in board['header26InputIds']]
    (client / f'client-default-{header}pin.json').write_text(json.dumps(defaults, indent=2) + '\n')
PY
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
command -v "$qemu" >/dev/null || { echo 'Install qemu-user for the client ABI check' >&2; exit 1; }
"$qemu" -cpu "$cpu" -L "$target" -E LD_BIND_NOW=1 -E "LD_LIBRARY_PATH=$target/opt/galdrun-client/lib" \
    "$target/opt/galdrun-client/galdrun-client" --version
