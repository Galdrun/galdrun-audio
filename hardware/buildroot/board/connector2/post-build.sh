#!/usr/bin/env bash
set -euo pipefail
target=$1
# procps-ng supplies vmstat, which BusyBox does not implement. Keep the
# existing BusyBox versions of its other process tools.
for tool in free kill pgrep pidof pkill pmap ps pwdx slabtop sysctl tload top uptime w watch hugetop; do
    rm -f "$target/bin/$tool"
done
while IFS= read -r applet; do
    case ${applet##*/} in
        free|kill|pgrep|pidof|pkill|ps|pwdx|sysctl|top|uptime|watch)
            ln -sfn /bin/busybox "$target$applet" ;;
    esac
done < "$BUILD_DIR/busybox-1.37.0/busybox.links"
# Target Python/GDB is not shipped; discard toolchain debugger helpers and host paths.
rm -f "$target"/usr/lib/libstdc++*-gdb.py
project=${RAUMFELD_PROJECT_DIR:?Run build.sh to supply project location}
board=${RAUMFELD_BOARD:?}
"$HOST_DIR/bin/arm-linux-g++" -std=c++17 -Os -Wall -Wextra -Werror \
    -static-libstdc++ -static-libgcc "$project/board/common/captive-portal.cpp" \
    -o "$target/usr/sbin/raumfeld-captive-portal"
"$HOST_DIR/bin/arm-linux-g++" -std=c++17 -Os -Wall -Wextra -Werror \
    -static-libstdc++ -static-libgcc \
    -I"$project/../../shared/json/include" \
    "$project/board/connector2/status-leds.cpp" "$project/../../shared/json/src/Json.cpp" \
    -o "$target/usr/sbin/raumfeld-status-leds"
release="$project/../../releases/client/linux-armv6"
[[ -s "$release/galdrun-client" && -s "$release/lib/libatomic.so.1" ]] || {
    echo 'Build/export the Linux ARMv6 client before building firmware.' >&2; exit 1;
}
mkdir -p "$target/opt/galdrun-client/lib" "$target/opt/galdrun-client/licenses" "$target/etc/galdrun"
install -m 0755 "$release/galdrun-client" "$target/opt/galdrun-client/galdrun-client"
install -m 0644 "$release/lib/"* "$target/opt/galdrun-client/lib/"
install -m 0644 "$release/licenses/"* "$target/opt/galdrun-client/licenses/"
install -m 0644 "$release/build-info.txt" "$target/opt/galdrun-client/build-info.txt"
install -m 0644 "$project/board/$board/client-default.json" "$target/opt/galdrun-client/client-default.json"
chmod 0700 "$target/etc/galdrun"
mkdir -p "$target/root/.ssh" "$target/lib/firmware"
# No key and no root password ship, and SSH stays off until the settings page
# sets a password and turns it on. A developer build can add its own key with
# GALDRUN_SSH_PUBKEY (it still needs SSH turned on).
rm -f "$target/root/.ssh/authorized_keys"
if [[ -n ${GALDRUN_SSH_PUBKEY:-} ]]; then
    install -m 0600 "$GALDRUN_SSH_PUBKEY" "$target/root/.ssh/authorized_keys"
fi
chmod 0700 "$target/root/.ssh"
chmod 0700 "$target/root"
install -m 0644 "$project/board/connector2/firmware/am335x-pm-firmware.elf" "$target/lib/firmware/am335x-pm-firmware.elf"
find "$target/etc/init.d" -type f -name 'S*' -exec chmod 0755 {} +
chmod 0755 "$target/usr/sbin/raumfeld-network" "$target/usr/sbin/raumfeld-hardware-check"
chmod 0755 "$target/usr/sbin/raumfeld-wifi-scan" "$target/usr/sbin/raumfeld-ssh" "$target/etc/init.d/S50sshd"
# The setting lives on the device; a fresh install starts with SSH off.
rm -f "$target/etc/galdrun-ssh.conf"
install -m 0755 "$project/board/connector2/raumfeld-suspend" "$target/usr/sbin/raumfeld-suspend"
chmod 0600 "$target/etc/raumfeld-network.conf"
chmod 0600 "$target/etc/galdrun-network.conf"
ln -sfn ../run/log "$target/var/log"
install -m 0644 "$project/board/$board/client-board.json" "$target/etc/galdrun-board.json"
printf 'CPU_MHZ=300\n' > "$target/etc/galdrun-cpu.conf"
# rsync is used over SSH on demand, never as a listening daemon.
rm -f "$target/etc/init.d/S50rsync" "$target/etc/init.d/S80rsync"
# The network supervisor exclusively owns hostapd/dnsmasq startup.
rm -f "$target/etc/init.d/S50hostapd" "$target/etc/init.d/S80dnsmasq" "$target/etc/init.d/S50wpa_supplicant"
# Let Buildroot's CPIO hook install its standard /init devtmpfs wrapper.
rm -f "$target/init"
# License texts of everything in the image: Buildroot's legal-info (build.sh
# runs it first) plus the TI firmware installed above.
if [[ -d "$BASE_DIR/legal-info/licenses" ]]; then
    rm -rf "$target/usr/share/licenses"
    mkdir -p "$target/usr/share/licenses"
    cp -r "$BASE_DIR/legal-info/licenses/." "$target/usr/share/licenses/"
    install -m 0644 "$BASE_DIR/legal-info/manifest.csv" "$target/usr/share/licenses/manifest.csv"
fi
install -D -m 0644 "$project/board/connector2/firmware/TI-LICENSE.txt" \
    "$target/usr/share/licenses/am335x-pm-firmware/TI-LICENSE.txt"
# legal-info lists the Bootlin toolchain as "unknown"; its glibc (LGPL-2.1) and
# GCC runtime (GPL-3.0 with the runtime exception) libraries ship in the image.
toolchain="$target/usr/share/licenses/toolchain-external-bootlin"
install -D -m 0644 "$BUILD_DIR/linux-6.12.112/LICENSES/preferred/LGPL-2.1" "$toolchain/glibc-LGPL-2.1.txt"
install -m 0644 "$release/licenses/GPL-3.0.txt" "$release/licenses/GCC-RUNTIME-EXCEPTION.txt" "$toolchain/"
install -m 0644 "$HOST_DIR/opt/ext-toolchain/summary.csv" "$toolchain/summary.csv"
