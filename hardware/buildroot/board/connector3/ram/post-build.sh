#!/usr/bin/env bash
set -euo pipefail
target=$1
board=$(cd -- "$(dirname -- "$0")" && pwd)
project=${GALDRUN_PROJECT_DIR:?Run build-connector3-ram.sh}
release="$project/../../releases/client/linux-armv7"
[[ -s "$release/galdrun-client" ]] || { echo 'Build linux-armv7 first.' >&2; exit 1; }

# Reuse the diagnostic's validated public-key-only SSH policy.
bash "$board/../diagnostic/post-build.sh" "$target"
install -m 0755 "$board/init.sh" "$target/init"
mkdir -p "$target/opt/galdrun-client/lib" "$target/opt/galdrun-client/licenses"
install -m 0755 "$release/galdrun-client" "$target/opt/galdrun-client/galdrun-client"
for library in "$release/lib/"*; do
    [[ ! -f $library ]] || install -m 0644 "$library" "$target/opt/galdrun-client/lib/"
done
install -m 0644 "$release/licenses/"* "$target/opt/galdrun-client/licenses/"
install -m 0644 "$release/build-info.txt" "$target/opt/galdrun-client/build-info.txt"
install -m 0644 "$board/client-default.json" "$target/opt/galdrun-client/client-default.json"
install -m 0644 "$board/client-board.json" "$target/etc/galdrun-board.json"
common="$project/board/common/overlay"
install -m 0755 "$common/usr/sbin/raumfeld-network" "$common/usr/sbin/raumfeld-wifi-scan" "$target/usr/sbin/"
install -D -m 0755 "$common/etc/init.d/S45raumfeld-network" "$target/etc/init.d/S45raumfeld-network"
install -m 0600 "$common/etc/galdrun-network.conf" "$target/etc/galdrun-network.conf"
install -m 0600 "$board/network.conf" "$target/etc/raumfeld-network.conf"
# Only the network supervisor starts these services.
rm -f "$target/etc/init.d/S50hostapd" "$target/etc/init.d/S80dnsmasq" "$target/etc/init.d/S50wpa_supplicant"
"$HOST_DIR/bin/arm-linux-g++" -std=c++17 -Os -Wall -Wextra -Werror \
    -I"$STAGING_DIR/usr/include/dbus-1.0" -I"$STAGING_DIR/usr/lib/dbus-1.0/include" \
    "$board/buttons.cpp" -ldbus-1 -o "$target/usr/sbin/galdrun-connector3-buttons"
"$HOST_DIR/bin/arm-linux-g++" -std=c++17 -Os -Wall -Wextra -Werror \
    "$board/mcu-buttons.cpp" -o "$target/usr/sbin/galdrun-connector3-mcu"
# The existing network service launches this helper for setup AP mode.
"$HOST_DIR/bin/arm-linux-g++" -std=c++17 -Os -Wall -Wextra -Werror \
    "$project/board/common/captive-portal.cpp" -o "$target/usr/sbin/raumfeld-captive-portal"
firmware="$BUILD_DIR/linux-firmware-20250211"
# Install explicitly on every build so changing the local radio input cannot
# leak a previous build's firmware into the generic image through the cache.
install -D -m 0644 "${GALDRUN_CONNECTOR3_WIFI_FIRMWARE:-$firmware/mrvl/pcie8897_uapsta.bin}" \
    "$target/lib/firmware/mrvl/pcie8897_uapsta.bin"
install -D -m 0644 "$firmware/imx/sdma/sdma-imx7d.bin" \
    "$target/lib/firmware/imx/sdma/sdma-imx7d.bin"
install -D -m 0644 "$firmware/LICENSE.sdma_firmware" \
    "$target/usr/share/licenses/sdma/LICENSE.sdma_firmware"
if [[ -d "$BASE_DIR/legal-info/licenses" ]]; then
    mkdir -p "$target/usr/share/licenses"
    cp -a "$BASE_DIR/legal-info/licenses/." "$target/usr/share/licenses/"
    install -m 0644 "$BASE_DIR/legal-info/manifest.csv" "$target/usr/share/licenses/manifest.csv"
fi
# Home Wi-Fi credentials are supplied only in a private deployment overlay.
rm -f "$target/etc/wpa_supplicant.conf"
