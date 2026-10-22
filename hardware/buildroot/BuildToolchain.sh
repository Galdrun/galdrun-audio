#!/usr/bin/env bash
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work=${RAUMFELD_BUILD_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/raumfeld-buildroot}
source "$project/PrepareHost.sh"
mkdir -p "$work/connector2" "$project/logs"
make -C "$work/buildroot-2025.02.18" O="$work/connector2" BR2_EXTERNAL="$project" raumfeld_connector2_defconfig
make -C "$work/buildroot-2025.02.18" O="$work/connector2" BR2_EXTERNAL="$project" BR2_DL_DIR="$project/downloads" -j16 toolchain host-dtc host-uboot-tools host-util-linux 2>&1 | tee "$project/logs/toolchain.log"
