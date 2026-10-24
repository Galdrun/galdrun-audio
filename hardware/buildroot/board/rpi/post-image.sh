#!/usr/bin/env bash
set -euo pipefail
images=$1
project=${RAUMFELD_PROJECT_DIR:?}
board=${RAUMFELD_BOARD:?}
export PATH="$HOST_DIR/bin:$HOST_DIR/sbin:$PATH"
kernel=$(find "$BUILD_DIR" -maxdepth 1 -type d -name 'linux-custom')
[[ -n $kernel ]] || { echo 'Missing pinned Pi kernel build' >&2; exit 1; }
[[ $(make -s -C "$kernel" ARCH=arm kernelversion) == 6.12.112 ]] || {
    echo 'Raspberry Pi kernel baseline must be 6.12.112' >&2; exit 1;
}
install -m 0644 "$kernel/.config" "$images/linux.config"
install -m 0644 "$BASE_DIR/.config" "$images/buildroot.config"
boot="$images/rpi-boot"
rm -rf "$boot"
mkdir -p "$boot/overlays"
install -m 0644 "$images/rpi-firmware/"{bootcode.bin,start.elf,fixup.dat} "$boot/"
install -m 0644 "$project/board/$board/config.txt" "$boot/config.txt"
install -m 0644 "$project/board/rpi/cmdline.txt" "$boot/cmdline.txt"
install -m 0644 "$images/zImage" "$images/"*.dtb "$boot/"
install -m 0644 "$images/"*.dtbo "$boot/overlays/"
printf '# Galdrun audio profile: usb\n' > "$boot/galdrun-audio.txt"
# Keep the root's /boot a mountpoint; boot assets live on FAT, not in ext4.
python3 - "$project/board/rpi/genimage.cfg" "$images/genimage.cfg" "$boot" <<'PY'
from pathlib import Path
import sys
template, output, boot = map(Path, sys.argv[1:])
dtbs = '\n'.join(f'file {p.name} {{ image = "rpi-boot/{p.name}" }}' for p in sorted(boot.glob('*.dtb')))
output.write_text(template.read_text().replace('#DTBS#', dtbs))
PY
rootpath=$(mktemp -d)
trap 'rm -rf "$rootpath"' EXIT
genimage --rootpath "$rootpath" --tmppath "$BUILD_DIR/genimage.tmp" \
    --inputpath "$images" --outputpath "$images" --config "$images/genimage.cfg"
python3 "$project/scripts/verify-rpi-images.py" "$images" "$board" "$TARGET_DIR"
mkdir -p "$images/licenses"
cp -a "$BASE_DIR/legal-info/licenses/." "$images/licenses/"
install -m 0644 "$BASE_DIR/legal-info/manifest.csv" "$images/licenses/manifest.csv"
cp -a "$TARGET_DIR/opt/galdrun-client/licenses" "$images/licenses/galdrun-client"
cp -a "$TARGET_DIR/usr/share/licenses/toolchain-external-bootlin" "$images/licenses/"
gzip -n -c "$images/sdcard.img" > "$images/galdrun-$board-sdcard.img.gz"
cd "$images"
sha256sum "galdrun-$board-sdcard.img.gz" firmware-manifest.json > SHA256SUMS
