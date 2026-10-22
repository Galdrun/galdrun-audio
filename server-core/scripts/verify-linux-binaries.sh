#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
arm="$HOME/.local/share/multiroom-linux/x-tools/aarch64-rpi3-linux-gnu"
arm_sysroot="$($arm/bin/aarch64-rpi3-linux-gnu-gcc -print-sysroot)"
arm32="$HOME/.local/share/multiroom-linux/x-tools/armv6-rpi-linux-gnueabihf"
arm32_sysroot="$($arm32/bin/armv6-rpi-linux-gnueabihf-gcc -print-sysroot)"
for target in x64 x86 armv7 armv8 armv9; do
    directory="$root/releases/server-core/linux-$target"
    runner=()
    case "$target" in
        x86) runner=(qemu-i386 -L /usr/i686-linux-gnu) ;;
        armv7) runner=(qemu-arm -cpu cortex-a7 -L "$arm32_sysroot") ;;
        armv8) runner=(qemu-aarch64 -cpu cortex-a53 -L "$arm_sysroot") ;;
        armv9) runner=(qemu-aarch64 -cpu max -L "$arm_sysroot") ;;
    esac
    echo "Checking $target release"
    LD_LIBRARY_PATH="$directory/lib" "${runner[@]}" "$directory/galdrun-core" --version
    LD_LIBRARY_PATH="$directory/lib" "${runner[@]}" "$directory/galdrun-core" --help >/dev/null
    # Start real listeners on free ports, then require clean SIGTERM shutdown.
    config="$(mktemp)"
    printf '%s\n' '{"network":{"controlPort":0,"audioPort":0},"control":{"bindAddress":"127.0.0.1","port":0}}' > "$config"
    LD_LIBRARY_PATH="$directory/lib" "${runner[@]}" "$directory/galdrun-core" --config "$config" &
    pid=$!
    sleep 2
    if ! kill -0 "$pid" 2>/dev/null; then rm -f "$config"; wait "$pid"; exit 1; fi
    kill -TERM "$pid"
    wait "$pid"
    rm -f "$config"
done
