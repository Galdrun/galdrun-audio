#!/usr/bin/env bash
# Build a source-only kernel and RAM filesystem; no installer or hardware writes.
set -euo pipefail
project=$(cd -- "$(dirname -- "$0")/.." && pwd)
[[ $# == 1 ]] || { echo "Usage: $0 <SSH public key>" >&2; exit 2; }
export GALDRUN_SSH_PUBKEY GALDRUN_PROJECT_DIR="$project"
GALDRUN_SSH_PUBKEY=$(realpath -- "$1")
ssh-keygen -l -f "$GALDRUN_SSH_PUBKEY" >/dev/null
if grep -q 'PRIVATE KEY' "$GALDRUN_SSH_PUBKEY"; then
    echo 'Supply only a public SSH key.' >&2; exit 2
fi
bash "$project/../../scripts/build-client.sh" linux-armv7
source "$project/PrepareHost.sh"
jobs=${JOBS:-16}
[[ $jobs =~ ^[0-9]+$ && $jobs -ge 1 && $jobs -le 32 ]] || exit 2
exec 9>"$work/build.lock"
flock -n 9 || { echo 'Another firmware build is running.' >&2; exit 1; }
mkdir -p "$work/external" "$project/logs" "$project/output/connector3-ram"
rsync -a --delete --exclude downloads --exclude logs --exclude output --exclude reference --exclude private \
    "$project/" "$work/external/"
board="$work/external/board/connector3/ram"
chmod 755 "$board/init.sh" "$board/post-build.sh"
output="$work/connector3-ram"
# Static kexec can run on the old vendor libc. Other packages stay dynamic.
args=(-C "$work/buildroot-2025.02.18" "O=$output" "BR2_EXTERNAL=$work/external"
      "BR2_DL_DIR=$project/downloads" 'KEXEC_CONF_ENV=LDFLAGS=-static')
inputs=$(find "$board" -type f \( -name '*.dts' -o -name '*.fragment' -o -name defconfig \) \
    -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -d ' ' -f 1)
patches=$(find "$board/patches" -type f -name '*.patch' -print0 | sort -z | \
    xargs -0 sha256sum | sha256sum | cut -d ' ' -f 1)
make "${args[@]}" "BR2_DEFCONFIG=$board/defconfig" defconfig
if [[ -d "$output/build/linux-6.12.112" ]]; then
    if [[ $(cat "$output/.kernel-patches" 2>/dev/null || true) != "$patches" ]]; then
        # Patches run when the source is unpacked, not during reconfiguration.
        make "${args[@]}" linux-dirclean
    elif [[ $(cat "$output/.kernel-inputs" 2>/dev/null || true) != "$inputs" ]]; then
        make "${args[@]}" linux-reconfigure
    fi
fi
make "${args[@]}" legal-info >"$project/logs/connector3-ram-licenses.log" 2>&1
make "${args[@]}" -j"$jobs" 2>&1 | tee "$project/logs/connector3-ram.log"
printf '%s\n' "$inputs" >"$output/.kernel-inputs"
printf '%s\n' "$patches" >"$output/.kernel-patches"
python3 "$project/scripts/verify-connector3-ram.py" "$output" "$project"
destination="$project/output/connector3-ram"
install -m 0644 "$output/images/zImage" "$destination/zImage"
install -m 0644 "$output/images/rootfs.cpio.gz" "$destination/rootfs.cpio.gz"
install -m 0644 "$output/images/rootfs.tar.gz" "$destination/rootfs.tar.gz"
install -m 0644 "$output/images/imx7d-galdrun-connector3-ram.dtb" "$destination/connector3.dtb"
install -m 0755 "$output/target/usr/sbin/kexec" "$destination/kexec"
install -m 0644 "$output/images/ram-verification.json" "$destination/ram-verification.json"
cd "$destination"
sha256sum zImage rootfs.cpio.gz rootfs.tar.gz connector3.dtb kexec >SHA256SUMS
printf 'Connector 3 RAM test files: %s\n' "$destination"
