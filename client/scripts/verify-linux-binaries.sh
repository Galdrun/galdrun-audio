#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
tools="$HOME/.local/share/multiroom-linux"

./build/linux-x64/galdrun-client --version
../releases/client/linux-x64/run.sh --version
for target in x64 x86 armv6 armv7 armv8 armv9; do
    cmp "build/linux-$target/galdrun-client" "../releases/client/linux-$target/galdrun-client"
    test -f "../releases/client/linux-$target/client.example.json"
    test -f "../releases/client/linux-$target/licenses/GPL-3.0.txt"
done
for target in x86 armv6 armv7 armv8 armv9; do
    case "$target" in
        x86) emulator=qemu-i386; cpu=max; sysroot=/usr/i686-linux-gnu ;;
        armv6|armv7)
            emulator=qemu-arm
            cpu=arm1176
            if [[ "$target" == armv7 ]]; then cpu=cortex-a7; fi
            sysroot="$tools/x-tools/armv6-rpi-linux-gnueabihf/armv6-rpi-linux-gnueabihf/sysroot" ;;
        armv8|armv9)
            emulator=qemu-aarch64
            cpu=cortex-a53
            if [[ "$target" == armv9 ]]; then cpu=max; fi
            sysroot="$tools/x-tools/aarch64-rpi3-linux-gnu/aarch64-rpi3-linux-gnu/sysroot" ;;
    esac
    printf '%s: ' "$target"
    "$emulator" -cpu "$cpu" -L "$sysroot" \
        -E "LD_LIBRARY_PATH=$tools/alsa/$target/lib:$sysroot/lib:$sysroot/usr/lib" \
        "build/linux-$target/galdrun-client" --version
    printf '%s release: ' "$target"
    "$emulator" -cpu "$cpu" -L "$sysroot" \
        -E "LD_LIBRARY_PATH=$PWD/../releases/client/linux-$target/lib:$tools/alsa/$target/lib:$sysroot/lib:$sysroot/usr/lib" \
        "../releases/client/linux-$target/galdrun-client" --version
done
