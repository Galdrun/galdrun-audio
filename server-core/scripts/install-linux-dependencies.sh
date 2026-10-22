#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
tools="$HOME/.local/share/multiroom-linux"
mkdir -p "$tools/downloads"
archive="$tools/downloads/openssl-3.5.8.tar.gz"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 3 -o "$archive" \
    https://github.com/openssl/openssl/releases/download/openssl-3.5.8/openssl-3.5.8.tar.gz
fi
printf '%s  %s\n' a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2 "$archive" | sha256sum --check
if [[ ! -d "$tools/downloads/openssl-3.5.8" ]]; then tar -xzf "$archive" -C "$tools/downloads"; fi

if [[ $# == 0 ]]; then set -- x64 x86 armv7 armv8 armv9; fi
for target in "$@"; do
    runner=""; flags=""; cross=""; prefix=""; sysroot=""
    case "$target" in
        x64) cc=gcc; ar=ar; ranlib=ranlib; openssl_target=linux-x86_64 ;;
        x86)
            prefix=i686-linux-gnu-
            cc="${prefix}gcc"; ar="${prefix}ar"; ranlib="${prefix}ranlib"
            openssl_target=linux-x86
            cross="--enable-cross-compile --target-os=linux --arch=x86 --cross-prefix=$prefix"
            sysroot=/usr/i686-linux-gnu
            runner="qemu-i386 -L $sysroot" ;;
        armv7)
            prefix="$tools/x-tools/armv6-rpi-linux-gnueabihf/bin/armv6-rpi-linux-gnueabihf-"
            cc="${prefix}gcc"; ar="${prefix}ar"; ranlib="${prefix}ranlib"
            openssl_target=linux-armv4
            flags="-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard"
            sysroot="$($cc -print-sysroot)"
            # EXTRA_CONFIGURE is split on spaces: one --extra-cflags per flag.
            cross="--enable-cross-compile --target-os=linux --arch=arm --cpu=cortex-a7 --cross-prefix=$prefix"
            for flag in $flags; do cross+=" --extra-cflags=$flag"; done
            runner="qemu-arm -cpu cortex-a7 -L $sysroot" ;;
        armv8|armv9)
            prefix="$tools/x-tools/aarch64-rpi3-linux-gnu/bin/aarch64-rpi3-linux-gnu-"
            cc="${prefix}gcc"; ar="${prefix}ar"; ranlib="${prefix}ranlib"
            openssl_target=linux-aarch64
            flags="-march=armv8-a"
            cpu=cortex-a53
            if [[ "$target" == armv9 ]]; then flags="-march=armv9-a"; cpu=max; fi
            sysroot="$($cc -print-sysroot)"
            cross="--enable-cross-compile --target-os=linux --arch=aarch64 --cross-prefix=$prefix --extra-cflags=$flags"
            runner="qemu-aarch64 -cpu $cpu -L $sysroot" ;;
        *) echo "Unknown target: $target" >&2; exit 1 ;;
    esac
    ssl="$tools/openssl/$target"
    mkdir -p "$tools/build-openssl-$target"
    cd "$tools/build-openssl-$target"
    if [[ ! -f "$ssl/lib/libssl.so.3" ]]; then
        CC="$cc" AR="$ar" RANLIB="$ranlib" \
            "$tools/downloads/openssl-3.5.8/Configure" "$openssl_target" \
            --prefix="$ssl" --openssldir=/etc/ssl --libdir=lib \
            shared no-tests no-docs no-module no-legacy no-fips $flags
        make -j"${JOBS:-8}"
        make install_sw
    fi
    ffmpeg="$root/server-core/ffmpeg/dist/linux-$target-shared"
    if [[ ! -f "$ffmpeg/lib/libavformat.so" ]]; then
        export LD_LIBRARY_PATH="$ssl/lib:$ffmpeg/lib${sysroot:+:$sysroot/lib:$sysroot/usr/lib}"
        PKG_CONFIG_LIBDIR="$ssl/lib/pkgconfig" TARGET_ARCH="$target" \
            BUILD_SHARED=1 JOBS="${JOBS:-8}" FFMPEG_RUNNER="$runner" \
            EXTRA_CONFIGURE="$cross --enable-openssl --extra-cflags=-I$ssl/include --extra-ldflags=-L$ssl/lib" \
            bash "$root/server-core/ffmpeg/build.sh"
    fi
    # FFmpeg's only external TLS dependencies travel in the same SDK directory.
    cp "$ssl/lib/libssl.so.3" "$ssl/lib/libcrypto.so.3" "$ffmpeg/lib/"
done
