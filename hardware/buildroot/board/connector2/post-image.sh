#!/usr/bin/env bash
set -euo pipefail
images=$1
board_dir=$(cd -- "$(dirname -- "$0")" && pwd)
export PATH="$HOST_DIR/bin:$HOST_DIR/sbin:$PATH"
install -m 0644 "$BUILD_DIR/linux-6.12.112/.config" "$images/linux.config"
# Catch legacy two-cell pin tuples against this kernel's three-cell binding.
pinmux=/ocp/interconnect@44c00000/segment@200000/target-module@10000/scm@0/pinmux@800
cells=$(fdtget -t i "$images/am335x-raumfeld-connector2.dtb" "$pinmux" '#pinctrl-cells')
[[ $cells == 2 ]] || { echo 'Unexpected AM335x pinctrl binding' >&2; exit 1; }
for group in $(fdtget -l "$images/am335x-raumfeld-connector2.dtb" "$pinmux"); do
    read -ra pins <<<"$(fdtget -t x "$images/am335x-raumfeld-connector2.dtb" "$pinmux/$group" pinctrl-single,pins)"
    (( ${#pins[@]} > 0 && ${#pins[@]} % 3 == 0 )) || { echo "Invalid pin tuple count: $group" >&2; exit 1; }
    for ((index=0; index<${#pins[@]}; index+=3)); do
        (( 16#${pins[index]} < 0x238 && 16#${pins[index]} % 4 == 0 && 16#${pins[index+2]} <= 7 )) || {
            echo "Invalid pin offset/mux: $group" >&2; exit 1;
        }
    done
done
gzip -dc "$images/rootfs.cpio.gz" | cpio -i --to-stdout init > "$images/ram-init.sh"
grep -q 'mount -t devtmpfs' "$images/ram-init.sh" || { echo 'RAM init does not mount devtmpfs' >&2; exit 1; }
mkdir -p "$images/dtb-store"
for name in am33xx-raumfeld-connector-0-0 am33xx-raumfeld-connector-0-1 am33xx-raumfeld-connector-2-0 am33xx-raumfeld-connector-2-1; do
    install -m 0644 "$images/am335x-raumfeld-connector2.dtb" "$images/dtb-store/$name.dtb"
done
mkfs.cramfs "$images/dtb-store" "$images/dts.cramfs"
mkimage -A arm -O linux -T ramdisk -C gzip -a 0 -e 0 -n 'Custom Connector2 RAM root' \
    -d "$images/rootfs.cpio.gz" "$images/uInitrd"
python3 "$board_dir/../../scripts/verify-images.py" "$images"
tar -czf "$images/raumfeld-connector2-firmware.tar.gz" -C "$images" \
    uImage am335x-raumfeld-connector2.dtb dts.cramfs rootfs.ubi linux.config firmware-manifest.json
