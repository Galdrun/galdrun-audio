#!/usr/bin/env bash
set -euo pipefail
target=$1
board=$(cd -- "$(dirname -- "$0")" && pwd)
bash "$board/../diagnostic/post-build.sh" "$target"
install -m 0755 "$board/init.sh" "$target/init"
install -m 0755 "$board/install-usb.sh" "$target/usr/sbin/galdrun-install"
install -m 0755 "$board/update-fit.sh" "$target/usr/sbin/galdrun-update-fit"
mkdir -p "$target/media" "$target/payload"
install -m 0644 "${GALDRUN_CONNECTOR3_INSTALL_INPUTS:?}/payload.tar.sha256" "$target/etc/galdrun-payload.sha256"
install -m 0644 "$GALDRUN_CONNECTOR3_INSTALL_INPUTS/payload.sha256" "$target/etc/galdrun-install.sha256"
install -m 0644 "$GALDRUN_CONNECTOR3_INSTALL_INPUTS/volume-readback" "$target/etc/galdrun-volume-readback"
rm -f "$target/etc/galdrun-install-writes-enabled"
if [[ ${GALDRUN_CONNECTOR3_INSTALL_WRITE:-} == 1 ]]; then
    touch "$target/etc/galdrun-install-writes-enabled"
fi
mkdir -p "$target/usr/share/licenses"
cp -a "$BASE_DIR/legal-info/licenses/." "$target/usr/share/licenses/"
install -m 0644 "$BASE_DIR/legal-info/manifest.csv" "$target/usr/share/licenses/manifest.csv"
