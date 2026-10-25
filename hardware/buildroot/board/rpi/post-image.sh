#!/usr/bin/env bash
set -euo pipefail
images=$1
project=${RAUMFELD_PROJECT_DIR:?}
board=${RAUMFELD_BOARD:?}
arch=arm; image=zImage
case "$board" in rpi4|rpi5) arch=arm64; image=Image ;; esac
export PATH="$HOST_DIR/bin:$HOST_DIR/sbin:$PATH"
kernel=$(find "$BUILD_DIR" -maxdepth 1 -type d -name 'linux-custom')
[[ -n $kernel ]] || { echo 'Missing pinned Pi kernel build' >&2; exit 1; }
[[ $(make -s -C "$kernel" ARCH="$arch" kernelversion) == 6.12.112 ]] || {
    echo 'Raspberry Pi kernel baseline must be 6.12.112' >&2; exit 1;
}
install -m 0644 "$kernel/.config" "$images/linux.config"
install -m 0644 "$BASE_DIR/.config" "$images/buildroot.config"
boot="$images/rpi-boot"
rm -rf "$boot"
mkdir -p "$boot/overlays"
firmware=(bootcode.bin start.elf fixup.dat)
case "$board" in rpi4) firmware=(start4.elf fixup4.dat) ;; rpi5) firmware=() ;; esac
for file in "${firmware[@]}"; do install -m 0644 "$images/rpi-firmware/$file" "$boot/"; done
install -m 0644 "$project/board/$board/config.txt" "$boot/config.txt"
cmdline="$project/board/$board/cmdline.txt"
[[ -f $cmdline ]] || cmdline="$project/board/rpi/cmdline.txt"
install -m 0644 "$cmdline" "$boot/cmdline.txt"
install -m 0644 "$images/$image" "$images/"*.dtb "$boot/"
install -m 0644 "$images/"*.dtbo "$boot/overlays/"
printf '# Galdrun audio profile: usb\n' > "$boot/galdrun-audio.txt"
# Keep the root's /boot a mountpoint; boot assets live on FAT, not in ext4.
python3 - "$project/board/rpi/genimage.cfg" "$images/genimage.cfg" "$boot" "$image" "${firmware[@]}" <<'PY'
from pathlib import Path
import sys
template, output, boot = map(Path, sys.argv[1:4])
dtbs = '\n'.join(f'file {p.name} {{ image = "rpi-boot/{p.name}" }}' for p in sorted(boot.glob('*.dtb')))
firmware = '\n'.join(f'file {name} {{ image = "rpi-boot/{name}" }}' for name in sys.argv[5:])
output.write_text(template.read_text().replace('#DTBS#', dtbs).replace('#KERNEL#', sys.argv[4]).replace('#FIRMWARE#', firmware))
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
