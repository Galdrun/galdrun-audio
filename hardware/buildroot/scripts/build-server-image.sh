#!/usr/bin/env bash
# build-server-image.sh - Make the Galdrun server SD image for a Raspberry Pi board from
# that board's finished client image: the same appliance (network, SSH and client
# settings on port 4720) plus the server (web interface on 4723) and the core, both
# started at boot. The client filesystem is extended with the releases and ICU;
# Buildroot builds only ICU and its dependencies in a separate package cache.
#
# Usage (Linux/WSL, after hardware/buildroot/build.sh <board>):
#   hardware/buildroot/scripts/build-server-image.sh rpi3|rpi4|rpi5 <output dir>
# Needs releases/server/<arch> and releases/server-core/<arch> (rpi3: linux-armv7,
# rpi4/rpi5: linux-armv8). scripts/build-server.sh <board> builds all of it.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
board=${1:?board}
out=${2:?output directory}
project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
repo=$(cd -- "$project/../.." && pwd)
source "$project/PrepareHost.sh"
case $board in
    rpi3) arch=linux-armv7 ;;
    rpi4|rpi5) arch=linux-armv8 ;;
    *) echo "No server image for $board: the server needs a Pi 3 or newer (rpi3, rpi4, rpi5)" >&2; exit 2 ;;
esac
images="$work/$board/images"
host="$work/$board/host"
server="$repo/releases/server/$arch"
core="$repo/releases/server-core/$arch"
for file in "$images/rootfs.tar" "$images/boot.vfat" "$host/bin/fakeroot" "$host/sbin/mkfs.ext4" \
    "$host/bin/genimage" "$server/galdrun-server" "$core/galdrun-core"; do
    [[ -e $file ]] || { echo "Missing $file: build the $board client image and the $arch server and core first" >&2; exit 1; }
done
# Keep the client's Buildroot configuration and filesystem untouched. ICU is
# needed for the server's accent-insensitive browsing and alphabet indexes.
deps="$work/$board-server-deps"
buildroot="$work/buildroot-2025.02.18"
make -C "$buildroot" O="$deps" BR2_EXTERNAL="$work/external" "raumfeld_${board}_defconfig"
"$buildroot/utils/config" --file "$deps/.config" --enable BR2_PACKAGE_ICU
make -C "$buildroot" O="$deps" BR2_EXTERNAL="$work/external" olddefconfig
grep -q '^BR2_PACKAGE_ICU=y$' "$deps/.config"
make -C "$buildroot" O="$deps" BR2_EXTERNAL="$work/external" \
    BR2_DL_DIR="$project/downloads" -j"${JOBS:-16}" icu icu-legal-info
# The client image's root is 512 MiB; the server's library and caches need room to grow.
rootfs_size=3G

tmp=$(mktemp -d "$work/server-image-$board.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
# One fakeroot session: the ownership tar restores is what mkfs.ext4 then writes.
"$host/bin/fakeroot" -- bash -euo pipefail -s "$tmp" "$images/rootfs.tar" "$server" "$core" \
    "$project/board/rpi/server" "$board" "$host/sbin/mkfs.ext4" "$rootfs_size" "$deps/target" <<'ASSEMBLE'
tmp=$1 tar=$2 server=$3 core=$4 files=$5 board=$6 mkfs=$7 size=$8
deps=$9
root="$tmp/rootfs"
mkdir "$root"
tar -xpf "$tar" -C "$root"
mkdir -p "$root/opt/galdrun-server" "$root/opt/galdrun-core" "$root/var/lib/galdrun/server"
# The .NET debug symbols are not needed on the device.
cp -a "$server/." "$root/opt/galdrun-server/"
find "$root/opt/galdrun-server" -name '*.pdb' -delete
cp -a "$core/." "$root/opt/galdrun-core/"
cp -a --no-preserve=ownership "$deps/usr/lib/"libicu*.so* "$root/usr/lib/"
rm -f "$root/opt/galdrun-core/run.sh" "$root/opt/galdrun-core/galdrun-core.service" \
    "$root/opt/galdrun-server/galdrun-server.service"
install -m 0644 "$files/server-default.json" "$root/opt/galdrun-server/server-default.json"
install -m 0644 "$files/core-default.json" "$root/opt/galdrun-core/core-default.json"
install -m 0755 "$files/S90galdrun-core" "$root/etc/init.d/S90galdrun-core"
install -m 0755 "$files/S91galdrun-server" "$root/etc/init.d/S91galdrun-server"
chown -R 0:0 "$root/opt/galdrun-server" "$root/opt/galdrun-core" "$root/var/lib/galdrun"
chmod 0755 "$root/opt/galdrun-server/galdrun-server" "$root/opt/galdrun-core/galdrun-core"
chmod 0700 "$root/var/lib/galdrun"
echo "galdrun-server-$board" > "$root/etc/hostname"
# Three services log to RAM now, each rotating at 1 MiB.
sed -i 's/size=4m,/size=16m,/' "$root/etc/init.d/S00logram"
grep -q 'size=16m,' "$root/etc/init.d/S00logram"
# Buildroot's own ext4 options (fs/ext2): root-owned, no 64bit feature for older tools.
"$mkfs" -d "$root" -r 1 -N 0 -m 5 -L galdrun-root -O ^64bit -F -q "$tmp/rootfs.ext4" "$size"
ASSEMBLE

python3 "$project/scripts/test-server-runtime.py" "$tmp/rootfs" "$board"

ln -s "$images/boot.vfat" "$tmp/boot.vfat"
cat > "$tmp/genimage.cfg" <<'CFG'
image sdcard.img {
    hdimage { }
    partition boot {
        partition-type = 0xC
        bootable = true
        offset = 1M
        image = "boot.vfat"
    }
    partition rootfs {
        partition-type = 0x83
        image = "rootfs.ext4"
    }
}
CFG
mkdir "$tmp/genroot" "$tmp/gentmp"
"$host/bin/genimage" --rootpath "$tmp/genroot" --tmppath "$tmp/gentmp" \
    --inputpath "$tmp" --outputpath "$tmp" --config "$tmp/genimage.cfg"

# Check what was written, not what was meant: the files in the final ext4.
for path in /opt/galdrun-server/galdrun-server /opt/galdrun-core/galdrun-core /opt/galdrun-client/galdrun-client \
    /etc/init.d/S90galdrun-core /etc/init.d/S91galdrun-server /etc/init.d/S95galdrun-client; do
    "$host/sbin/debugfs" -R "stat $path" "$tmp/rootfs.ext4" 2>/dev/null | grep -q 'Mode:  0[0-7]*7[0-7][0-7]' ||
        { echo "Server image check failed: $path missing or not executable" >&2; exit 1; }
done
[[ $("$host/sbin/debugfs" -R 'cat /etc/hostname' "$tmp/rootfs.ext4" 2>/dev/null) == "galdrun-server-$board" ]] ||
    { echo 'Server image check failed: hostname' >&2; exit 1; }
for file in "$tmp/rootfs/usr/lib/"libicu*.so*; do
    "$host/sbin/debugfs" -R "stat /usr/lib/${file##*/}" "$tmp/rootfs.ext4" 2>/dev/null |
        grep -Eq 'User:[[:space:]]+0[[:space:]]+Group:[[:space:]]+0' ||
        { echo 'Server image check failed: ICU library must be root-owned' >&2; exit 1; }
done

mkdir -p "$out"
rm -rf "$out/licenses"
gzip -n -c "$tmp/sdcard.img" > "$out/galdrun-server-$board-sdcard.img.gz"
python3 "$project/scripts/server-image-manifest.py" "$images/firmware-manifest.json" \
    "$tmp" "$server" "$core" "$arch" "$board" "$out/firmware-manifest.json"
cp -a "$images/licenses" "$out/licenses"
cp -a "$server/licenses" "$out/licenses/galdrun-server"
cp -a "$core/licenses" "$out/licenses/galdrun-core"
mkdir -p "$out/licenses/icu"
cp -a "$deps/legal-info/licenses/"icu-*/. "$out/licenses/icu/"
cp "$project/board/rpi/server/README.md" "$out/README.md"
(cd "$out" && sha256sum "galdrun-server-$board-sdcard.img.gz" firmware-manifest.json > SHA256SUMS)
printf 'Server image exported: %s\n' "$out"
