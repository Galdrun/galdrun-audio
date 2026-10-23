#!/usr/bin/env bash
# build-server-core.sh - Build the Galdrun core audio server for the selected Linux
# targets, installing whatever they need first. Running it again skips what is already
# there. On Windows, scripts\build-server-core.ps1 builds windows-x64 itself and runs this in WSL.
#
# Usage:
#   scripts/build-server-core.sh                       # all targets below
#   scripts/build-server-core.sh linux-armv7
#   scripts/build-server-core.sh --packages-only ...   # only install missing apt packages
#
# Targets:
#   all, linux (default)  linux-x86 linux-x64 linux-armv7 linux-armv8 linux-armv9
# windows-x64 needs Windows: use scripts\build-server-core.ps1 there.
#
# Installs when missing (Ubuntu/Debian on x86-64):
#   apt packages (via sudo)   system-wide
#   cross toolchains          ~/.local/share/multiroom-linux (shared with the client)
#   OpenSSL 3.5.8 per target  ~/.local/share/multiroom-linux/openssl/<arch>
#   FFmpeg 9.0.2 per target   server-core/ffmpeg/dist/linux-<arch>-shared (source in server-core/ffmpeg/src)
# Results go to releases/server-core/<target>. Stops at the first failure.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/setup/linux.sh"
linux_targets=(linux-x86 linux-x64 linux-armv7 linux-armv8 linux-armv9)

packages_only=false
if [[ ${1:-} == --packages-only ]]; then packages_only=true; shift; fi
[[ $# -gt 0 ]] || set -- all
targets=()
for name in "$@"; do
    case $name in
        all|linux) targets+=("${linux_targets[@]}") ;;
        linux-x86|linux-x64|linux-armv[7-9]) targets+=("$name") ;;
        windows|windows-x64) die "$name needs Windows: run scripts\build-server-core.ps1 there." ;;
        *) die "Unknown target: $name (see the top of $0)" ;;
    esac
done
mapfile -t targets < <(printf '%s\n' "${targets[@]}" | awk '!seen[$0]++')

# perl builds OpenSSL; nasm and pkgconf build FFmpeg; qemu-user runs the cross-built FFmpeg's self-test.
install_packages build-essential cmake ninja-build curl xz-utils bzip2 perl nasm pkgconf qemu-user \
    gcc-i686-linux-gnu g++-i686-linux-gnu
$packages_only && exit 0
require_build_user

bash "$root/client/scripts/install-linux-tools.sh"
jobs=$(nproc)
for target in "${targets[@]}"; do
    echo "== $target"
    JOBS=$jobs bash "$root/server-core/scripts/install-linux-dependencies.sh" "${target#linux-}"
    (cd "$root/server-core" &&
        cmake --preset "$target" &&
        cmake --build --preset "$target" --target export-server --parallel "$jobs")
done
echo "Built: ${targets[*]}. Results are in $root/releases/server-core"
