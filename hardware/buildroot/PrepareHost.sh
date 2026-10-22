#!/usr/bin/env bash
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
project=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work=${RAUMFELD_BUILD_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/raumfeld-buildroot}
mkdir -p "$work" "$project/downloads"
mkdir -p "$work/host-tools"
if [[ -x /usr/bin/gnuinstall ]]; then
    ln -sf /usr/bin/gnuinstall "$work/host-tools/install"
fi
export PATH="$work/host-tools:$PATH"
archive="$project/downloads/buildroot-2025.02.18.tar.xz"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 3 https://buildroot.org/downloads/buildroot-2025.02.18.tar.xz -o "$archive.part"
    mv -- "$archive.part" "$archive"
fi
printf '%s  %s\n' e38ad1df6ea0479fff6419a87a64535d02131674d463be154a763ef725b55321 "$archive" | sha256sum -c -
if [[ ! -d "$work/buildroot-2025.02.18" ]]; then
    tar -xJf "$archive" -C "$work"
fi
printf 'Buildroot source: %s\n' "$work/buildroot-2025.02.18"
