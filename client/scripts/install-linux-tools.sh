#!/usr/bin/env bash
set -euo pipefail

# Run as the regular user, after installing the apt packages build-client.sh lists
# (build-client.sh does both).
tools="$HOME/.local/share/multiroom-linux"
mkdir -p "$tools/downloads" "$tools/x-tools" "$tools/include/linux"
cd "$tools/downloads"

download_toolchain() {
    local triplet="$1" checksum="$2" archive="x-tools-$1-gcc14.tar.xz"
    if [[ ! -x "$tools/x-tools/$triplet/bin/$triplet-g++" ]]; then
        curl --fail --location --retry 3 -o "$archive" \
            "https://github.com/tttapa/docker-arm-cross-toolchain/releases/download/1.2.1/$archive"
        printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check
        tar -xJf "$archive" -C "$tools"
    fi
}

download_toolchain armv6-rpi-linux-gnueabihf 0eca5ddfdba1fa7c36c1c23b5976b895184c81dcf59c2017bec66c08814c7132
download_toolchain aarch64-rpi3-linux-gnu f6fa1e871f6ba7182ebc1de6b3d3cef5a28977becc45214fc02890d242e1cdde

# Preserve GPIO v2 support even though the toolchain's baseline headers are Linux 5.4.
if [[ ! -f "$tools/include/linux/gpio.h" ]]; then
    curl --fail --location --retry 3 -o "$tools/include/linux/gpio.h" \
        https://raw.githubusercontent.com/torvalds/linux/v6.1/include/uapi/linux/gpio.h
fi

# Bullseye's ALSA version avoids depending on symbols absent from older Pi OS releases.
if [[ ! -d alsa-lib-1.2.4 ]]; then
    curl --fail --location --retry 3 -o alsa-lib-1.2.4.tar.bz2 \
        https://www.alsa-project.org/files/pub/lib/alsa-lib-1.2.4.tar.bz2
    tar -xjf alsa-lib-1.2.4.tar.bz2
fi

for target in x86 armv6 armv7 armv8 armv9; do
    # Already built: running this script again only fills in what is missing.
    [[ -f "$tools/alsa/$target/lib/libasound.so" ]] && continue
    flags=""
    case "$target" in
        x86) triplet=i686-linux-gnu; compiler="$triplet-gcc" ;;
        armv6|armv7)
            triplet=armv6-rpi-linux-gnueabihf
            compiler="$tools/x-tools/$triplet/bin/$triplet-gcc"
            flags="-march=armv6 -mfpu=vfp -mfloat-abi=hard"
            if [[ "$target" == armv7 ]]; then
                flags="-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard"
            fi ;;
        armv8|armv9)
            triplet=aarch64-rpi3-linux-gnu
            compiler="$tools/x-tools/$triplet/bin/$triplet-gcc"
            flags="-march=armv8-a"
            if [[ "$target" == armv9 ]]; then flags="-march=armv9-a"; fi ;;
    esac
    mkdir -p "$tools/build-alsa-$target"
    cd "$tools/build-alsa-$target"
    CC="$compiler" CFLAGS="-O2 $flags" "$tools/downloads/alsa-lib-1.2.4/configure" \
        --host="$triplet" --prefix="$tools/alsa/$target" \
        --with-configdir=/usr/share/alsa --with-plugindir=/usr/lib/alsa-lib \
        --disable-python --disable-static
    make -j"$(nproc)"
    # Only headers and the library are needed for linking; keep host ALSA configs intact.
    make -C include install
    make -C src install-libLTLIBRARIES
done

echo "Linux toolchains and target ALSA libraries installed in $tools"
