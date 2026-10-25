#!/usr/bin/env bash
#
# Decoder-focused FFmpeg build for a multiroom hi-fi audio system.
# Linux and Windows (MSYS2/MinGW) are supported; everything stays inside
# this folder.
#
# Codec scope (decoders only, no encoders, no video):
#   lossless: FLAC, ALAC, WavPack, APE, TAK, TTA, Shorten, WMA Lossless,
#             TrueHD, DST (compressed DSD)
#   DSD:      dsf/dff bitstream variants (dsd_lsbf/msbf + planar)
#   lossy:    MP1/MP2/MP3, AAC (+AAC-LATM), AC-3/E-AC-3, DTS, Vorbis, Opus
#   WAV/AIFF: the complete raw PCM family (8..64 bit, A-law/u-law) plus the
#             two legacy ADPCM WAV codecs for old files
#
# Containers/streams kept: wav, aiff, caf, flac, ogg, m4a/mov, asf, mka,
# wv, ape, tak, tta, dsf, dff(iff), raw MPEG audio/aac/ac3/dts/truehd/loas,
# concat, hls, mpegts, s337m and the raw pcm_* demuxers. Protocols: local
# files/pipes plus http(s)/tcp/udp for network streams. Filters are limited
# to the multiroom toolset: graph plumbing (aformat, asplit, adelay, atempo,
# ...), zone mixing/routing (amix, pan, channelmap, acrossover, afade,
# acrossfade), loudness measurement (ebur128, loudnorm, astats,
# volumedetect, silencedetect) and hi-fi DSP (afir room correction,
# firequalizer, hdcd, alimiter). Effects, denoisers and spatial gimmicks are
# dropped. Resampling is deliberately excluded (handled outside FFmpeg):
# the aresample filter is not built; libswresample stays enabled only
# because the native Opus decoder and the pan filter internally require it.
# avdevice/swscale/postproc and all video codecs are dropped.
#
# After install the script verifies that not a single video codec remains and
# runs a smoke test.
#
# Environment variables:
#   FFMPEG_VERSION    release to build                     (default: 9.0.2)
#   FFMPEG_SHA256     expected tarball checksum            (default: pinned for 9.0.2)
#   BUILD_SHARED=1    build shared libs instead of static  (default: static)
#   ENABLE_GPL=1      add --enable-gpl (not needed for the curated set)
#   AUTODETECT=1      let configure pick up system libs (zlib, TLS backends, ...)
#   JOBS              parallel make jobs                   (default: nproc)
#   EXTRA_DECODERS    extra decoder names, e.g. "musepack7 musepack8 oma"
#   EXTRA_ENCODERS    encoder names, e.g. "flac pcm_s16le" (default: none)
#   EXTRA_DEMUXERS    extra demuxer names
#   EXTRA_MUXERS      extra muxer names
#   EXTRA_PROTOCOLS   extra protocol names
#   EXTRA_FILTERS     extra filter names, e.g. "anequalizer anlmdn"
#   EXTRA_CONFIGURE   extra flags appended to configure
#   TARGET_ARCH       output tag for cross-builds (e.g. armv8); compiler flags go in EXTRA_CONFIGURE
#   FFMPEG_RUNNER     command prefix for cross-build verification (e.g. qemu-aarch64 -L <sysroot>)
#   NO_RUN_CHECKS=1   skip the checks that run ffmpeg (cross-builds the host cannot run at all)
#   LISTGEN_ONLY=1    only generate the component lists, then stop (debug)
#
set -euo pipefail

readonly DEFAULT_VERSION="9.0.2"
readonly DEFAULT_SHA256="8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e"

FFMPEG_VERSION="${FFMPEG_VERSION:-$DEFAULT_VERSION}"
FFMPEG_SHA256="${FFMPEG_SHA256:-}"
if [ "$FFMPEG_VERSION" = "$DEFAULT_VERSION" ] && [ -z "$FFMPEG_SHA256" ]; then
    FFMPEG_SHA256="$DEFAULT_SHA256"
fi
BUILD_SHARED="${BUILD_SHARED:-0}"
ENABLE_GPL="${ENABLE_GPL:-0}"
AUTODETECT="${AUTODETECT:-0}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
EXTRA_CONFIGURE="${EXTRA_CONFIGURE:-}"
EXTRA_DECODERS="${EXTRA_DECODERS:-}"
EXTRA_ENCODERS="${EXTRA_ENCODERS:-}"
EXTRA_DEMUXERS="${EXTRA_DEMUXERS:-}"
EXTRA_MUXERS="${EXTRA_MUXERS:-}"
EXTRA_PROTOCOLS="${EXTRA_PROTOCOLS:-}"
EXTRA_FILTERS="${EXTRA_FILTERS:-}"

log()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

[ -n "${BASH_VERSION:-}" ] || die "run this script with bash (e.g. ./build.sh)"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

SRC_DIR="$SCRIPT_DIR/src"
SRC="$SRC_DIR/ffmpeg-$FFMPEG_VERSION"
TARBALL="$SRC_DIR/ffmpeg-$FFMPEG_VERSION.tar.xz"
BASE_URL="https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"

# ------------------------------------------------------------------ profile --
# Decoders a multiroom hi-fi system needs. Names are intersected with
# `configure --list-decoders`, so unavailable ones are reported, not fatal.
readonly HIFI_DECODERS="
    flac alac wavpack ape tak tta shorten wmalossless truehd dst
    dsd_lsbf dsd_msbf dsd_lsbf_planar dsd_msbf_planar
    mp1 mp2 mp3 aac aac_latm ac3 eac3 dca vorbis opus
    adpcm_ima_wav adpcm_ms
"
readonly HIFI_DEMUXERS="
    wav aiff caf flac ogg mov asf matroska wv ape tak tta dsf iff
    mp3 aac ac3 eac3 dts truehd loas concat hls mpegts s337m
"
readonly HIFI_MUXERS="null"
readonly HIFI_PROTOCOLS="file pipe concat http httpproxy tcp udp tls https data fd subfile cache"
# multiroom filter set (graph plumbing, zone mixing/routing, loudness
# measurement, hi-fi DSP; NO resampling - that happens outside FFmpeg).
# abuffer/abuffersink are always built into libavfilter and need no
# explicit enable
readonly HIFI_FILTERS="
    aformat anull anullsrc anullsink asplit acopy asetpts asettb atrim aselect
    aloop apad adelay atempo
    volume afade acrossfade amix amerge join pan channelmap channelsplit acrossover
    astats ashowinfo volumedetect ebur128 ametadata silencedetect silenceremove loudnorm
    afir firequalizer hdcd alimiter
    sine amovie
"

# ---------------------------------------------------------------- toolchain --
for tool in make awk tar sort comm paste grep; do
    command -v "$tool" >/dev/null 2>&1 || die "'$tool' not found in PATH"
done
CC_BIN="$(command -v cc || command -v gcc || command -v clang || true)"
[ -n "$CC_BIN" ] || die "no C compiler found (install gcc or clang)"
log "using compiler: $CC_BIN"

X86ASM_OK=1
if ! command -v nasm >/dev/null 2>&1 && ! command -v yasm >/dev/null 2>&1; then
    X86ASM_OK=0
    printf '\033[1;33mNOTE:\033[0m nasm/yasm not found -> building with --disable-x86asm (slower).\n'
fi

# ------------------------------------------------------------------ platform --
case "$(uname -s)" in
    Linux*)          PLATFORM="linux" ;;
    Darwin*)         PLATFORM="macos" ;;
    *MINGW*|MSYS*|CYGWIN*)
                     PLATFORM="windows" ;;
    *) die "unsupported platform: $(uname -s)" ;;
esac
ARCH="${TARGET_ARCH:-$(uname -m)}"
TAG="$PLATFORM-$ARCH"
[ "$BUILD_SHARED" = "1" ] && TAG="$TAG-shared"

BUILD="$SCRIPT_DIR/build/$TAG"
PREFIX="$SCRIPT_DIR/dist/$TAG"
mkdir -p "$SRC_DIR" "$BUILD"

# ------------------------------------------------------------------- source --
if [ ! -f "$SRC/configure" ]; then
    if [ ! -f "$TARBALL" ]; then
        log "downloading ffmpeg-$FFMPEG_VERSION ..."
        if command -v curl >/dev/null 2>&1; then
            curl -fL --retry 3 -o "$TARBALL" "$BASE_URL"
        elif command -v wget >/dev/null 2>&1; then
            wget -O "$TARBALL" "$BASE_URL"
        else
            die "need curl or wget to download $BASE_URL"
        fi
    fi
    if [ -n "$FFMPEG_SHA256" ]; then
        log "verifying tarball checksum ..."
        if command -v sha256sum >/dev/null 2>&1; then
            echo "$FFMPEG_SHA256  $TARBALL" | sha256sum -c - || die "checksum mismatch for $TARBALL"
        else
            [ "$(shasum -a 256 "$TARBALL" | cut -d' ' -f1)" = "$FFMPEG_SHA256" ] \
                || die "checksum mismatch for $TARBALL"
        fi
    fi
    log "extracting source ..."
    tar -xJf "$TARBALL" -C "$SRC_DIR"
fi
[ -f "$SRC/configure" ] || die "source extraction failed: $SRC/configure missing"

# ------------------------------------------------------- generate lists --
# Curated codec/demuxer sets are intersected with what this platform's
# configure actually offers; unavailable names are reported, not fatal.
log "generating component lists ..."
LG="$BUILD/gen"
mkdir -p "$LG"

for what in decoders encoders filters demuxers muxers protocols; do
    ( cd "$SRC" && ./configure "--list-$what" ) | tr -s '[:space:]' '\n' | sed -n '/./p' | sort -u > "$LG/avail_${what%s}.txt"
done

# multiroom filter set (graph plumbing, zone mixing/routing, loudness
# measurement, hi-fi DSP); abuffer/abuffersink are always built into
# libavfilter and need no explicit enable
{
    printf '%s\n' $HIFI_FILTERS
    printf '%s\n' $EXTRA_FILTERS
} | tr -s '[:space:]' '\n' | sed -n '/./p' | sort -u | comm -12 - "$LG/avail_filter.txt" > "$LG/filt_final.txt"
[ -s "$LG/filt_final.txt" ] || die "filter selection produced an empty list"

# curated sets (+ the whole raw pcm_* family for decoders and demuxers)
# intersected with the platform's available components; platform-framework
# variants (e.g. pcm_mulaw_at) are filtered out
PLATFORM_SUFFIX_RE='_(at|mf|mediacodec|qsv|cuvid|nvenc|v4l2m2m|omx|mmal|rkmpp|oh|amf|d3d12va|vaapi|vdpau|videotoolbox)$'
{
    printf '%s\n' $HIFI_DECODERS
    grep '^pcm_' "$LG/avail_decoder.txt"
    printf '%s\n' $EXTRA_DECODERS
} | tr -s '[:space:]' '\n' | sed -n '/./p' | grep -vE "$PLATFORM_SUFFIX_RE" | sort -u | comm -12 - "$LG/avail_decoder.txt" > "$LG/dec_final.txt"

{
    printf '%s\n' $HIFI_DEMUXERS
    grep '^pcm_' "$LG/avail_demuxer.txt"
    printf '%s\n' $EXTRA_DEMUXERS
} | tr -s '[:space:]' '\n' | sed -n '/./p' | sort -u | comm -12 - "$LG/avail_demuxer.txt" > "$LG/demux_final.txt"

{
    printf '%s\n' $HIFI_MUXERS
    printf '%s\n' $EXTRA_MUXERS
} | tr -s '[:space:]' '\n' | sed -n '/./p' | sort -u | comm -12 - "$LG/avail_muxer.txt" > "$LG/mux_final.txt"

{
    printf '%s\n' $HIFI_PROTOCOLS
    printf '%s\n' $EXTRA_PROTOCOLS
} | tr -s '[:space:]' '\n' | sed -n '/./p' | sort -u | comm -12 - "$LG/avail_protocol.txt" > "$LG/proto_final.txt"

{
    printf '%s\n' $EXTRA_ENCODERS
} | tr -s '[:space:]' '\n' | sed -n '/./p' | sort -u | comm -12 - "$LG/avail_encoder.txt" > "$LG/enc_final.txt"

# report curated names that are unavailable on this platform
{
    printf '%s\n' $HIFI_DECODERS $HIFI_DEMUXERS $HIFI_MUXERS $HIFI_PROTOCOLS $HIFI_FILTERS
} | tr -s '[:space:]' '\n' | sed -n '/./p' | sort -u > "$LG/wanted.txt"
if [ -n "$(comm -23 "$LG/wanted.txt" <(cat "$LG/dec_final.txt" "$LG/demux_final.txt" "$LG/mux_final.txt" "$LG/proto_final.txt" "$LG/filt_final.txt" | sort -u))" ]; then
    printf '\033[1;33mNOTE:\033[0m unavailable on this platform (skipped): '
    comm -23 "$LG/wanted.txt" <(cat "$LG/dec_final.txt" "$LG/demux_final.txt" "$LG/mux_final.txt" "$LG/proto_final.txt" "$LG/filt_final.txt" | sort -u) | tr '\n' ' '
    printf '\n'
fi

[ -s "$LG/dec_final.txt" ] || die "decoder selection produced an empty list"
[ -s "$LG/demux_final.txt" ] || die "demuxer selection produced an empty list"
[ -s "$LG/mux_final.txt" ] || die "muxer selection produced an empty list"
for must in flac mp3 aac alac vorbis opus wavpack dca truehd pcm_s16le dsd_lsbf; do
    grep -qx "$must" "$LG/dec_final.txt" || die "expected decoder '$must' missing from generated list"
done
for must in aformat volume adelay amix ebur128; do
    grep -qx "$must" "$LG/filt_final.txt" || die "expected filter '$must' missing from generated list"
done

DEC_CSV=$(paste -sd, "$LG/dec_final.txt")
FILT_CSV=$(paste -sd, "$LG/filt_final.txt")
DEMUX_CSV=$(paste -sd, "$LG/demux_final.txt")
MUX_CSV=$(paste -sd, "$LG/mux_final.txt")
PROTO_CSV=$(paste -sd, "$LG/proto_final.txt")
log "selected: $(wc -l < "$LG/dec_final.txt") decoders, $(wc -l < "$LG/enc_final.txt") encoders, $(wc -l < "$LG/filt_final.txt") filters, $(wc -l < "$LG/demux_final.txt") demuxers, $(wc -l < "$LG/mux_final.txt") muxers, $(wc -l < "$LG/proto_final.txt") protocols"
log "generated lists kept in $LG"

if [ "${LISTGEN_ONLY:-0}" = "1" ]; then
    log "LISTGEN_ONLY=1 -> stopping before configure"
    exit 0
fi

# ---------------------------------------------------------------- configure --
ARGS=(
    --prefix="$PREFIX"
    --disable-everything
    --disable-avdevice
    --disable-swscale
    --disable-ffplay
    --disable-doc
    --enable-small
    --enable-pic
    --enable-avcodec --enable-avformat --enable-avfilter
    # libswresample stays enabled only as an internal dependency of the
    # native Opus decoder and the pan filter; the aresample filter is not
    # built (resampling is done outside FFmpeg)
    --enable-swresample
    --enable-decoder="$DEC_CSV"
    --enable-demuxer="$DEMUX_CSV"
    --enable-muxer="$MUX_CSV"
    --enable-protocol="$PROTO_CSV"
    --enable-parsers --enable-bsfs
    --enable-filter="$FILT_CSV"
)
if [ -s "$LG/enc_final.txt" ]; then
    ARGS+=("--enable-encoder=$(paste -sd, "$LG/enc_final.txt")")
fi
[ "$ENABLE_GPL" = "1" ] && ARGS+=(--enable-gpl)
[ "$AUTODETECT" = "1" ] || ARGS+=(--disable-autodetect)
# use native Windows threads instead of pthreads and link the MinGW runtime
# (libatomic/libgcc) statically: keeps ffmpeg.exe and the static libraries
# free of the libwinpthread-1.dll runtime dependency
if [ "$PLATFORM" = "windows" ]; then
    ARGS+=(--disable-pthreads --extra-ldflags=-static)
fi
[ "$X86ASM_OK" = "1" ] || ARGS+=(--disable-x86asm)
[ "$BUILD_SHARED" = "1" ] && ARGS+=(--enable-shared --disable-static)
if [ -n "$EXTRA_CONFIGURE" ]; then
    read -r -a EXTRA_ARR <<< "$EXTRA_CONFIGURE"
    ARGS+=("${EXTRA_ARR[@]}")
fi

log "configuring (out-of-tree build in $BUILD) ..."
cd "$BUILD"
printf '%s\n' "${ARGS[@]}" > configure-args.txt
"$SRC/configure" "${ARGS[@]}"

# -------------------------------------------------------------------- build --
log "building with $JOBS jobs (this takes a few minutes) ..."
make -j"$JOBS"

log "installing to $PREFIX ..."
make install

# ------------------------------------------------------------------- verify --
FFBIN="$PREFIX/bin/ffmpeg"
[ -x "$FFBIN" ] || FFBIN="$PREFIX/bin/ffmpeg.exe"
[ -x "$FFBIN" ] || die "ffmpeg binary not found in $PREFIX/bin"

if [ "${NO_RUN_CHECKS:-0}" = "1" ]; then
    printf '\033[1;33mNOTE:\033[0m NO_RUN_CHECKS=1 -> codec list and smoke test skipped; run them on the target.\n'
else
    log "verifying: no video codecs present ..."
    RUNNER=()
    if [ -n "${FFMPEG_RUNNER:-}" ]; then read -r -a RUNNER <<< "$FFMPEG_RUNNER"; fi
    out="$("${RUNNER[@]}" "$FFBIN" -hide_banner -decoders 2>/dev/null | grep -v '=')"
    video=$(printf '%s\n' "$out" | awk '$1 ~ /V/' | wc -l)
    audio=$(printf '%s\n' "$out" | awk '$1 ~ /A/' | wc -l)
    [ "$video" -eq 0 ] || { printf '%s\n' "$out" | awk '$1 ~ /V/' | head; die "decoder list contains $video video codecs"; }
    [ "$audio" -gt 60 ] || die "only $audio audio decoders found - decoder selection failed"
    log "  decoders: $audio audio codecs, 0 video codecs"

    out="$("${RUNNER[@]}" "$FFBIN" -hide_banner -encoders 2>/dev/null | grep -v '=')"
    video=$(printf '%s\n' "$out" | awk '$1 ~ /V/' | wc -l)
    audio=$(printf '%s\n' "$out" | awk '$1 ~ /A/' | wc -l)
    [ "$video" -eq 0 ] || die "encoder list contains $video video codecs"
    if [ "$audio" -gt 0 ]; then
        log "  encoders: $audio audio codecs, 0 video codecs"
    else
        log "  encoders: none (decode-only build)"
    fi

    log "running demux smoke test (raw pcm -> null) ..."
    SMOKE="$BUILD/smoke"
    mkdir -p "$SMOKE"
    dd if=/dev/zero of="$SMOKE/in.pcm" bs=1024 count=64 status=none 2>/dev/null || dd if=/dev/zero of="$SMOKE/in.pcm" bs=1024 count=64 2>/dev/null
    "${RUNNER[@]}" "$FFBIN" -hide_banner -loglevel error -y -f s16le -ar 44100 -ac 2 -i "$SMOKE/in.pcm" -c copy -f null -
    rm -rf "$SMOKE"
fi

log "done."
log "install prefix : $PREFIX"
log "  libraries    : $PREFIX/lib"
log "  headers      : $PREFIX/include"
log "  tools        : $PREFIX/bin (ffmpeg, ffprobe)"
log "pkg-config     : export PKG_CONFIG_PATH=\"$PREFIX/lib/pkgconfig\""
du -sh "$PREFIX" || true
