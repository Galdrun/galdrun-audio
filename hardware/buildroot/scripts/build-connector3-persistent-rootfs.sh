#!/usr/bin/env bash
# Offline persistent filesystem assembly; no hardware access.
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")/.." && pwd)
[[ $# == 0 || ( $# == 1 && $1 == --release ) ]] || { echo "Usage: $0 [--release]" >&2; exit 2; }
profile=developer
[[ $# == 0 ]] || profile=release
source "$project/PrepareHost.sh"
exec 9>"$work/build.lock"
flock -n 9 || { echo 'Another firmware build is running.' >&2; exit 1; }
ram="$project/private/connector3/ram-local"
export GALDRUN_CONNECTOR3_WIFI_FIRMWARE="$project/private/connector3/pcie8897-working-reference.bin"
python3 "$project/scripts/verify-connector3-ram.py" "$work/connector3-ram" "$project"
(cd "$ram" && sha256sum -c SHA256SUMS)
tools="$work/connector3-ram/host"
if [[ ! -x "$tools/sbin/mkfs.ubifs" || ! -x "$tools/sbin/ubinize" ]]; then
    mkdir -p "$project/logs"
    make -C "$work/buildroot-2025.02.18" "O=$work/connector3-ram" \
        "BR2_EXTERNAL=$work/external" "BR2_DL_DIR=$project/downloads" \
        -j"${JOBS:-16}" host-mtd >"$project/logs/connector3-persistent-host-mtd.log" 2>&1
fi
destination="$project/private/connector3/persistent-rootfs"
[[ $profile != release ]] || destination="$project/output/connector3-persistent-rootfs"
mkdir -p "$destination"
stage=$(mktemp -d "$work/connector3-persistent-rootfs.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
mkdir -p "$stage/rootfs" "$stage/settings"
chmod 0700 "$stage/settings"
tar -xzf "$ram/rootfs.tar.gz" -C "$stage/rootfs"
install -m 0755 "$project/board/connector3/persistent/init.sh" "$stage/rootfs/init"
install -m 0755 "$project/board/connector3/raumfeld-suspend" "$stage/rootfs/usr/sbin/raumfeld-suspend"
# Rebuild board userspace from the current source, even when reusing a RAM archive.
"$tools/bin/arm-linux-g++" -std=c++17 -Os -Wall -Wextra -Werror -pthread \
    "$project/board/connector3/ram/mcu-buttons.cpp" -o "$stage/rootfs/usr/sbin/galdrun-connector3-mcu"
"$tools/bin/arm-linux-g++" -std=c++17 -Os -Wall -Wextra -Werror -DCONNECTOR3_STATUS_LEDS \
    -I"$project/../../shared/json/include" -I"$project/../../client/src/audio/linux" \
    "$project/board/connector2/status-leds.cpp" "$project/../../shared/json/src/Json.cpp" \
    "$project/../../client/src/audio/linux/Connector3Mcu.cpp" -o "$stage/rootfs/usr/sbin/galdrun-status-leds"
common="$project/board/common/overlay"
install -m 0755 "$common/usr/sbin/raumfeld-ssh" "$stage/rootfs/usr/sbin/raumfeld-ssh"
install -m 0755 "$common/etc/init.d/S50sshd" "$stage/rootfs/etc/init.d/S50sshd"
if [[ $profile == release ]]; then
    rm -f "$stage/rootfs/root/.ssh/authorized_keys" "$stage/rootfs/etc/galdrun-ssh.conf"
    install -m 0644 "$common/etc/ssh/sshd_config" "$stage/rootfs/etc/ssh/sshd_config"
    printf 'connector3\n' >"$stage/rootfs/etc/galdrun-release-defaults"
else
    printf 'SSH_ENABLED=1\n' >"$stage/rootfs/etc/galdrun-ssh.conf"
fi
# The RAM profile omits the password applets used by the standard SSH switch.
# Reuse its pinned BusyBox source/toolchain in a separate build directory.
busybox="$work/connector3-persistent-busybox"
if [[ ! -f "$busybox/Makefile" ]]; then
    cp -a --reflink=auto "$work/connector3-ram/build/busybox-1.37.0" "$busybox"
fi
cp "$work/connector3-ram/build/busybox-1.37.0/.config" "$busybox/.config"
python3 - "$busybox/.config" "$project/board/connector3/persistent/busybox.fragment" <<'PY'
from pathlib import Path
import re,sys
config,fragment=map(Path,sys.argv[1:])
text=config.read_text()
for line in fragment.read_text().splitlines():
    name=line.split('=')[0]
    text,count=re.subn(r'(?m)^(?:'+re.escape(name)+r'=.*|# '+re.escape(name)+r' is not set)$',line,text)
    if not count: text+='\n'+line+'\n'
config.write_text(text)
PY
make -C "$busybox" "CROSS_COMPILE=$tools/bin/arm-linux-" oldconfig </dev/null \
    >"$project/logs/connector3-persistent-busybox.log" 2>&1
make -C "$busybox" -j"${JOBS:-16}" "CROSS_COMPILE=$tools/bin/arm-linux-" \
    >>"$project/logs/connector3-persistent-busybox.log" 2>&1
install -m 0755 "$busybox/busybox" "$stage/rootfs/bin/busybox"
ln -sfn ../../bin/busybox "$stage/rootfs/usr/bin/cryptpw"
ln -sfn ../../bin/busybox "$stage/rootfs/usr/sbin/chpasswd"
install -m 0644 "$busybox/.config" "$stage/rootfs/usr/share/licenses/busybox.config"
toolchain_licenses="$stage/rootfs/usr/share/licenses/toolchain-external-bootlin"
mkdir -p "$toolchain_licenses"
install -m 0644 "$work/connector3-ram/build/linux-6.12.112/LICENSES/preferred/LGPL-2.1" \
    "$toolchain_licenses/glibc-LGPL-2.1.txt"
install -m 0644 "$project/../../releases/client/linux-armv7/licenses/"* "$toolchain_licenses/"
install -m 0644 "$tools/opt/ext-toolchain/summary.csv" "$toolchain_licenses/summary.csv"
mkdir -p "$stage/rootfs/settings" "$stage/rootfs/dev/pts"
python3 - "$stage/rootfs" <<'PY'
from pathlib import Path
import json,sys
root=Path(sys.argv[1])
defaults=root/'opt/galdrun-client/client-default.json'
data=json.loads(defaults.read_text())
assert not data.get('clientId') and not data.get('server')
data['name']='Connector 3'
defaults.write_text(json.dumps(data,indent=2)+'\n')
board=root/'etc/galdrun-board.json'
data=json.loads(board.read_text());data['board']='connector3';data['sshSettings']=True
data['statusLeds']=True
board.write_text(json.dumps(data,indent=2)+'\n')
PY
cp "$project/board/connector3/persistent/ubinize.cfg" "$stage/ubinize.cfg"
cat >"$stage/make-filesystems.sh" <<'SH'
set -eu
cd "$1"
chown -h -R 0:0 rootfs settings
mknod -m 0622 rootfs/dev/console c 5 1
"$2/sbin/mkfs.ubifs" -r rootfs -m 4096 -e 253952 -c 768 -x zlib -o rootfs.ubifs
"$2/sbin/mkfs.ubifs" -r settings -m 4096 -e 253952 -c 128 -x zlib -o settings.ubifs
"$2/sbin/ubinize" -m 4096 -p 262144 -s 4096 -O 4096 -o rootfs.ubi ubinize.cfg
SH
"$tools/bin/fakeroot" -- sh "$stage/make-filesystems.sh" "$stage" "$tools"
python3 "$project/scripts/verify-connector3-persistent-rootfs.py" "$stage" "$work/connector3-ram/legal-info/manifest.csv"
for name in rootfs.ubifs settings.ubifs rootfs.ubi persistent-rootfs-verification.json; do
    install -m 0644 "$stage/$name" "$destination/$name"
done
(cd "$destination" && sha256sum rootfs.ubifs settings.ubifs rootfs.ubi >SHA256SUMS)
printf 'Persistent %s filesystem candidate: %s\n' "$profile" "$destination"
