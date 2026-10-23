#!/usr/bin/env bash
# build-client.sh - Build the Galdrun client for the selected targets on Linux,
# installing whatever they need first. Running it again skips what is already there.
# On Windows, scripts\build-client.ps1 builds windows-x64 itself and runs this script in WSL.
#
# Usage:
#   scripts/build-client.sh                        # all targets below
#   scripts/build-client.sh connector2
#   scripts/build-client.sh linux esp32-a1s
#   scripts/build-client.sh --packages-only ...    # only install missing apt packages
#
# Targets:
#   all (default)  linux, esp32 and connector2
#   linux          linux-x86 linux-x64 linux-armv6 linux-armv7 linux-armv8 linux-armv9
#   esp32          esp32-a1s esp32-s3-devkitc
#   connector2     Raumfeld Connector 2 firmware with the linux-armv6 client
# windows-x64 needs Windows: use scripts\build-client.ps1 there.
#
# Installs when missing (Ubuntu/Debian on x86-64):
#   apt packages (via sudo)            system-wide
#   cross toolchains and target ALSA   ~/.local/share/multiroom-linux
#   ESP-IDF v5.5.1                     ~/esp/esp-idf-v5.5.1, its tools in ~/.espressif
#   Buildroot and package sources      hardware/buildroot/downloads, build tree in ~/.cache/raumfeld-buildroot
# Results go to releases/client/<target>, the Connector 2 USB image to
# releases/client/raumfeld/connector2. Stops at the first failure.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/setup/linux.sh"
idf_version=v5.5.1
idf="$HOME/esp/esp-idf-$idf_version"
linux_targets=(linux-x86 linux-x64 linux-armv6 linux-armv7 linux-armv8 linux-armv9)
esp32_targets=(esp32-a1s esp32-s3-devkitc)

packages_only=false
if [[ ${1:-} == --packages-only ]]; then packages_only=true; shift; fi
[[ $# -gt 0 ]] || set -- all
targets=()
for name in "$@"; do
    case $name in
        all) targets+=("${linux_targets[@]}" "${esp32_targets[@]}" connector2) ;;
        linux) targets+=("${linux_targets[@]}") ;;
        esp32) targets+=("${esp32_targets[@]}") ;;
        linux-x86|linux-x64|linux-armv[6-9]|esp32-a1s|esp32-s3-devkitc|connector2) targets+=("$name") ;;
        windows|windows-x64) die "$name needs Windows: run scripts\build-client.ps1 there." ;;
        *) die "Unknown target: $name (see the top of $0)" ;;
    esac
done
mapfile -t targets < <(printf '%s\n' "${targets[@]}" | awk '!seen[$0]++')

# True when a selected target matches the pattern.
has() { local target; for target in "${targets[@]}"; do [[ $target == $1 ]] && return 0; done; return 1; }

# --- apt packages
packages=()
if has 'linux-*' || has connector2; then
    packages+=(build-essential cmake ninja-build curl xz-utils bzip2 libasound2-dev gcc-i686-linux-gnu g++-i686-linux-gnu)
fi
if has 'esp32-*'; then
    packages+=(git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev
        libssl-dev dfu-util libusb-1.0-0)
fi
if has connector2; then
    packages+=(bison flex cpio libssl-dev libncurses-dev patch unzip file wget git rsync python3 bc e2fsprogs)
fi
install_packages "${packages[@]}"
$packages_only && exit 0
require_build_user

# --- tools
if has 'linux-*' || has connector2; then
    bash "$root/client/scripts/install-linux-tools.sh"
fi
if has 'esp32-*' && [[ ! -f $idf/.installed ]]; then
    if [[ ! -d $idf ]]; then
        rm -rf "$idf.part"
        git clone --branch "$idf_version" --depth 1 --recursive --shallow-submodules \
            https://github.com/espressif/esp-idf.git "$idf.part"
        mv "$idf.part" "$idf"
    fi
    "$idf/install.sh" esp32,esp32s3
    touch "$idf/.installed"
fi

# --- build
jobs=$(nproc)

build_linux() {
    (cd "$root/client" &&
        cmake --preset "$1" -DCMAKE_BUILD_TYPE=Release &&
        cmake --build --preset "$1" --target export-client --parallel "$jobs")
}

build_esp32() {
    (
        set +eu
        export IDF_PATH=$idf
        . "$idf/export.sh" >/dev/null
        set -eu
        idf.py -C "$root/client/esp32" -B "$root/client/esp32/build-$1" "-DMULTIROOM_BOARD=$1" build
    )
}

build_connector2() {
    # The firmware bundles releases/client/linux-armv6; an up-to-date build only re-exports.
    build_linux linux-armv6
    bash "$root/hardware/buildroot/build.sh" connector2
    local release="$root/releases/client/raumfeld/connector2"
    mkdir -p "$release"
    # README.md in the release folder is maintained by hand and left alone.
    cp "$root/hardware/buildroot/output/connector2/usb-install/"{connect2.img,installer-manifest.json} "$release/"
    # The image's license texts and source note travel with it.
    rm -rf "$release/licenses"
    cp -r "$root/hardware/buildroot/output/connector2/usb-install/licenses" "$release/"
}

for target in "${targets[@]}"; do
    echo "== $target"
    case $target in
        linux-*) build_linux "$target" ;;
        esp32-*) build_esp32 "$target" ;;
        connector2) build_connector2 ;;
    esac
done
echo "Built: ${targets[*]}. Results are in $root/releases/client"
