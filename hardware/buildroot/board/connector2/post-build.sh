#!/usr/bin/env bash
set -euo pipefail
source "${RAUMFELD_PROJECT_DIR:?}/board/common/post-build.sh"
"$HOST_DIR/bin/arm-linux-g++" -std=c++17 -Os -Wall -Wextra -Werror \
    -static-libstdc++ -static-libgcc \
    -I"$project/../../shared/json/include" \
    "$project/board/connector2/status-leds.cpp" "$project/../../shared/json/src/Json.cpp" \
    -o "$target/usr/sbin/raumfeld-status-leds"
install -m 0644 "$project/board/connector2/firmware/am335x-pm-firmware.elf" "$target/lib/firmware/am335x-pm-firmware.elf"
install -m 0755 "$project/board/connector2/raumfeld-suspend" "$target/usr/sbin/raumfeld-suspend"
printf 'CPU_MHZ=300\n' > "$target/etc/galdrun-cpu.conf"
install -D -m 0644 "$project/board/connector2/firmware/TI-LICENSE.txt" \
    "$target/usr/share/licenses/am335x-pm-firmware/TI-LICENSE.txt"
# legal-info lists the Bootlin toolchain as "unknown"; its glibc (LGPL-2.1) and
# GCC runtime (GPL-3.0 with the runtime exception) libraries ship in the image.
toolchain="$target/usr/share/licenses/toolchain-external-bootlin"
install -D -m 0644 "$BUILD_DIR/linux-6.12.112/LICENSES/preferred/LGPL-2.1" "$toolchain/glibc-LGPL-2.1.txt"
install -m 0644 "$release/licenses/GPL-3.0.txt" "$release/licenses/GCC-RUNTIME-EXCEPTION.txt" "$toolchain/"
install -m 0644 "$HOST_DIR/opt/ext-toolchain/summary.csv" "$toolchain/summary.csv"
