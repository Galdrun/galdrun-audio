#!/usr/bin/env bash
# build-server.sh - Build the Galdrun server (.NET) for the selected targets, installing
# the .NET SDK first when it is missing. Running it again skips what is already there.
# .NET publishes every target from any host, so this also builds the Windows targets.
#
# Usage:
#   ./build-server.sh                     # all targets below
#   ./build-server.sh linux-armv7
#   ./build-server.sh linux windows-x64
#
# Targets (all self-contained: nothing has to be installed on the device):
#   all (default)  windows and linux
#   windows        windows-x64 windows-armv8
#   linux          linux-x64 linux-armv7 linux-armv8
#
# Installs when missing:
#   apt packages (via sudo)   curl, ca-certificates, ICU (Ubuntu/Debian)
#   .NET SDK                  the version in server/global.json, into ~/.dotnet (dotnet-install)
# Results go to releases/server/<target>. Stops at the first failure.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$root/scripts/setup/linux.sh"

declare -A rids=([windows-x64]=win-x64 [windows-armv8]=win-arm64
                 [linux-x64]=linux-x64 [linux-armv7]=linux-arm [linux-armv8]=linux-arm64)
targets=()
[[ $# -gt 0 ]] || set -- all
for name in "$@"; do
    case $name in
        all) targets+=(windows-x64 windows-armv8 linux-x64 linux-armv7 linux-armv8) ;;
        windows) targets+=(windows-x64 windows-armv8) ;;
        linux) targets+=(linux-x64 linux-armv7 linux-armv8) ;;
        *) [[ -n ${rids[$name]:-} ]] || die "Unknown target: $name (see the top of $0)"; targets+=("$name") ;;
    esac
done
mapfile -t targets < <(printf '%s\n' "${targets[@]}" | awk '!seen[$0]++')

install_packages curl ca-certificates libicu-dev
[[ $EUID -ne 0 ]] || die 'Run as your normal user, not root; the SDK is installed into your home directory.'

cd "$root/server"
ensure_dotnet

for target in "${targets[@]}"; do
    echo "== $target"
    dotnet publish src/Multiroom.Server/Multiroom.Server.csproj -c Release -r "${rids[$target]}" \
        --self-contained true -o "$root/releases/server/$target"
done
echo "Built: ${targets[*]}. Results are in $root/releases/server"
