#!/usr/bin/env bash
set -euo pipefail
target=$1
board_dir=$(cd -- "$(dirname -- "$0")/.." && pwd)
install -m 0755 "$board_dir/installer/init.sh" "$target/init"
install -m 0755 "$board_dir/install-usb.sh" "$target/usr/sbin/galdrun-install"
install -m 0755 "$board_dir/start-usb.sh" "$target/usr/sbin/galdrun-start"
mkdir -p "$target/media" "$target/payload"
# The external toolchain copies its C++ and OpenMP runtimes; nothing here uses them.
rm -f "$target"/usr/lib/libstdc++.so* "$target"/usr/lib/libgomp.so*
