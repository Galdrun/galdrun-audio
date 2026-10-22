#!/usr/bin/env bash
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ ${1:-} == --list ]]; then
    for config in "$project"/configs/raumfeld_*_defconfig; do
        board=${config##*/raumfeld_}; printf '%s\n' "${board%_defconfig}"
    done
    exit
fi
[[ $# -gt 0 ]] || set -- connector2
for board in "$@"; do
    [[ $board =~ ^[a-z0-9][a-z0-9_-]*$ && -f "$project/configs/raumfeld_${board}_defconfig" ]] || {
        printf 'Unknown board: %s (use --list)\n' "$board" >&2; exit 2;
    }
done
source "$project/PrepareHost.sh"
jobs=${JOBS:-16}
[[ $jobs =~ ^[0-9]+$ && $jobs -ge 1 && $jobs -le 32 ]] || { echo 'JOBS must be 1..32' >&2; exit 2; }
mkdir -p "$project/logs" "$project/output" "$work/external"
exec 9>"$work/build.lock"
flock -n 9 || { echo 'Another Custom build is running.' >&2; exit 1; }
# Compile on Linux storage; keep maintained sources and exported images in this project.
rsync -a --delete --exclude downloads --exclude logs --exclude output --exclude reference --exclude private \
    "$project/" "$work/external/"
find "$work/external/board" -type f -name '*.sh' -exec chmod 755 {} +
mkdir -p "$project/downloads/linux"
kernel_archive=linux-6.12.112.tar.xz
if [[ -f "$project/downloads/$kernel_archive" ]]; then
    cp -u "$project/downloads/$kernel_archive" "$project/downloads/linux/$kernel_archive"
fi
if [[ ! -f "$project/downloads/linux/$kernel_archive" ]]; then
    curl --fail --location --retry 3 "https://cdn.kernel.org/pub/linux/kernel/v6.x/$kernel_archive" -o "$project/downloads/linux/$kernel_archive.part"
    mv -- "$project/downloads/linux/$kernel_archive.part" "$project/downloads/linux/$kernel_archive"
fi
printf '%s  %s\n' 164dc9d1f6c93c61a15e1f071c48379b467f2b17c469cce7223471968208ed03 \
    "$project/downloads/linux/$kernel_archive" | sha256sum -c -
export RAUMFELD_PROJECT_DIR="$project"
for board in "$@"; do
    export RAUMFELD_BOARD="$board"
    output="$work/$board"
    make -C "$work/buildroot-2025.02.18" O="$output" BR2_EXTERNAL="$work/external" "raumfeld_${board}_defconfig"
    openssh_inputs=$({ for patches in "$project/board/common/patches/openssh" "$project/board/$board/patches/openssh"; do
        if [[ -d "$patches" ]]; then find "$patches" -type f -print0; fi
    done; } | sort -z | xargs -0 -r sha256sum | sha256sum | cut -d ' ' -f 1)
    if compgen -G "$output/build/openssh-*" >/dev/null && [[ $(cat "$output/.raumfeld-openssh-inputs" 2>/dev/null || true) != "$openssh_inputs" ]]; then
        make -C "$work/buildroot-2025.02.18" O="$output" BR2_EXTERNAL="$work/external" openssh-dirclean
    fi
    busybox_inputs=$({ if [[ -f "$project/board/$board/busybox.config" ]]; then cat "$project/board/$board/busybox.config"; fi; } | sha256sum | cut -d ' ' -f 1)
    if [[ -d "$output/build/busybox-1.37.0" && $(cat "$output/.raumfeld-busybox-inputs" 2>/dev/null || true) != "$busybox_inputs" ]]; then
        make -C "$work/buildroot-2025.02.18" O="$output" BR2_EXTERNAL="$work/external" busybox-dirclean
    fi
    kernel_inputs=$({ find "$project/board/$board" -type f \( -name linux.config -o -name '*.c' -o -name '*.h' -o -name '*.dts' -o -name '*.dtsi' -o -name '*.patch' -o -name '*.elf' \) -print0; printf '%s\0' "$project/configs/raumfeld_${board}_defconfig" "$project/external.mk"; } | sort -z | xargs -0 sha256sum | sha256sum | cut -d ' ' -f 1)
    if [[ -d "$output/build/linux-6.12.112" && $(cat "$output/.raumfeld-kernel-inputs" 2>/dev/null || true) != "$kernel_inputs" ]]; then
        make -C "$work/buildroot-2025.02.18" O="$output" BR2_EXTERNAL="$work/external" linux-dirclean
    fi
    make -C "$work/buildroot-2025.02.18" O="$output" BR2_EXTERNAL="$work/external" \
        BR2_DL_DIR="$project/downloads" -j"$jobs" linux 2>&1 | tee "$project/logs/$board.log"
    printf '%s\n' "$kernel_inputs" >"$output/.raumfeld-kernel-inputs"
    # License texts first: post-build copies them into the image.
    make -C "$work/buildroot-2025.02.18" O="$output" BR2_EXTERNAL="$work/external" \
        BR2_DL_DIR="$project/downloads" legal-info 2>&1 | tee -a "$project/logs/$board.log"
    make -C "$work/buildroot-2025.02.18" O="$output" BR2_EXTERNAL="$work/external" \
        BR2_DL_DIR="$project/downloads" -j"$jobs" 2>&1 | tee -a "$project/logs/$board.log"
    printf '%s\n' "$busybox_inputs" >"$output/.raumfeld-busybox-inputs"
    printf '%s\n' "$openssh_inputs" >"$output/.raumfeld-openssh-inputs"
    mkdir -p "$project/output/$board"
    cp -a "$output/images/." "$project/output/$board/"
    if [[ $board == connector2 ]]; then
        python3 "$project/scripts/build-usb-installer.py" "$work" "$jobs"
    fi
    printf 'Firmware exported: %s/output/%s\n' "$project" "$board"
done
