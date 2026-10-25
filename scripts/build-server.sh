#!/usr/bin/env bash
# build-server.sh - Build the Galdrun server (.NET) for the selected targets, installing
# the .NET SDK first when it is missing. Running it again skips what is already there.
# .NET publishes every target from any host, so this also builds the Windows targets.
#
# Usage:
#   scripts/build-server.sh                     # all targets below
#   scripts/build-server.sh linux-armv7
#   scripts/build-server.sh linux windows-x64
#   scripts/build-server.sh rpi4                # Raspberry Pi server SD image
#
# Targets (all self-contained: nothing has to be installed on the device):
#   all (default)  windows and linux
#   windows        windows-x64 windows-armv8
#   linux          linux-x64 linux-armv7 linux-armv8 linux-armv9
#   raspberrypi    rpi3 rpi4 rpi5: SD images with server, core and client (Pi 3 or newer),
#                  to releases/server/raspberrypi/<board>. Each also builds its core
#                  (scripts/build-server-core.sh) and its client image (scripts/build-client.sh);
#                  not part of all, since a client image is a Buildroot build.
#
# linux-armv9 is the linux-arm64 build under its own name (.NET has no ARMv9 RID).
#
# Installs when missing:
#   apt packages (via sudo)   curl, ca-certificates, ICU (Ubuntu/Debian)
#   .NET SDK                  the version in server/global.json, into ~/.dotnet (dotnet-install)
# Results go to releases/server/<target>. Stops at the first failure.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/setup/linux.sh"

declare -A rids=([windows-x64]=win-x64 [windows-armv8]=win-arm64
                 [linux-x64]=linux-x64 [linux-armv7]=linux-arm [linux-armv8]=linux-arm64
                 [linux-armv9]=linux-arm64)
# The server, core and client architecture of each Pi image (hardware/buildroot/board/rpi*).
declare -A boards=([rpi3]=linux-armv7 [rpi4]=linux-armv8 [rpi5]=linux-armv8)
targets=() images=()
[[ $# -gt 0 ]] || set -- all
for name in "$@"; do
    case $name in
        all) targets+=(windows-x64 windows-armv8 linux-x64 linux-armv7 linux-armv8 linux-armv9) ;;
        windows) targets+=(windows-x64 windows-armv8) ;;
        linux) targets+=(linux-x64 linux-armv7 linux-armv8 linux-armv9) ;;
        raspberrypi) images+=(rpi3 rpi4 rpi5) ;;
        rpi3|rpi4|rpi5) images+=("$name") ;;
        *) [[ -n ${rids[$name]:-} ]] || die "Unknown target: $name (see the top of $0)"; targets+=("$name") ;;
    esac
done
for board in "${images[@]}"; do targets+=("${boards[$board]}"); done
mapfile -t targets < <(printf '%s\n' "${targets[@]}" | awk '!seen[$0]++')
mapfile -t images < <(printf '%s\n' "${images[@]}" | awk 'NF && !seen[$0]++')

install_packages curl ca-certificates libicu-dev
[[ $EUID -ne 0 ]] || die 'Run as your normal user, not root; the SDK is installed into your home directory.'

cd "$root/server"
ensure_dotnet

for target in "${targets[@]}"; do
    echo "== $target"
    dotnet publish src/Multiroom.Server/Multiroom.Server.csproj -c Release -r "${rids[$target]}" \
        --self-contained true -o "$root/releases/server/$target"
done
for board in "${images[@]}"; do
    echo "== $board"
    bash "$root/scripts/build-server-core.sh" "${boards[$board]}"
    bash "$root/scripts/build-client.sh" "$board"
    bash "$root/hardware/buildroot/scripts/build-server-image.sh" "$board" "$root/releases/server/raspberrypi/$board"
done
echo "Built: ${targets[*]} ${images[*]}. Results are in $root/releases/server"
