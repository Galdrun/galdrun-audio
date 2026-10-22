# FFmpeg for Galdrun (multiroom hi-fi, decode-only)

Builds a pinned stable FFmpeg release with **only the audio decoders a
multiroom hi-fi system needs** — no video codecs, no encoders — for Linux and
Windows. Everything (download, build, install) happens inside this folder;
nothing outside `server-core/ffmpeg/` is touched.

- Current pinned release: **FFmpeg 9.0.2 "Lei"** (latest stable, 2026-09-18), verified by SHA256.
- Libraries built: `libavcodec`, `libavformat`, `libavfilter`, `libswresample`, `libavutil`.
  Dropped: `libavdevice`, `libswscale`, `libpostproc`, `ffplay`.
- Licensing: LGPL (the curated codec set needs nothing GPL).

## Codec scope

| Group | Decoders |
|---|---|
| Lossless | `flac`, `alac`, `wavpack`, `ape`, `tak`, `tta`, `shorten`, `wmalossless`, `truehd`, `dst` (compressed DSD) |
| DSD | `dsd_lsbf`, `dsd_msbf`, `dsd_lsbf_planar`, `dsd_msbf_planar` (via `.dsf` / `.dff`) |
| Lossy / streaming | `mp1`, `mp2`, `mp3`, `aac`, `aac_latm`, `ac3`, `eac3`, `dca` (incl. DTS-HD MA), `vorbis`, `opus` |
| WAV / AIFF at any depth | full `pcm_*` family (u8/s8/u16/s16/u24/s24/s32/s64/f32/f64, A-law, u-law, planar) + `adpcm_ima_wav`, `adpcm_ms` for legacy files |

Containers/streams: `wav aiff caf flac ogg mov(asf/m4a) asf matroska(mka) wv
ape tak tta dsf iff(dff) mp3 aac ac3 eac3 dts truehd loas concat hls mpegts
s337m` + raw `pcm_*` demuxers. Protocols: `file pipe concat http httpproxy
tcp udp tls https data fd subfile cache`. Parsers and bitstream filters stay
fully enabled (tiny, keeps all containers working).

Filters are curated down to the multiroom toolset:

- graph plumbing: `aformat anull anullsrc anullsink asplit acopy asetpts
  asettb atrim aselect aloop apad adelay atempo`
- zone mixing / routing: `volume afade acrossfade amix amerge join pan
  channelmap channelsplit acrossover`
- loudness / measurement: `astats ashowinfo volumedetect ebur128 ametadata
  silencedetect silenceremove loudnorm`
- hi-fi DSP: `afir` (room-correction convolution), `firequalizer`, `hdcd`,
  `alimiter`
- sources/testing: `sine amovie` (+ always-built `abuffer`/`abuffersink`)

**Resampling is intentionally not built** — it is handled outside FFmpeg, so
the `aresample` filter is excluded. `libswresample` is still compiled in, but
only as an internal dependency of the native Opus decoder and the `pan`
filter; the application is not supposed to call it.

Effects (echo/chorus/exciter/stereo widening), denoisers and spatial
filters are excluded; add any filter back with `EXTRA_FILTERS`.

The script fails the build if the resulting `ffmpeg` reports any video codec.

## Requirements

### Linux (Debian/Ubuntu)
```sh
sudo apt install build-essential nasm curl xz-utils ca-certificates
```
### Linux (Fedora)
```sh
sudo dnf install gcc make nasm tar xz curl
```
### Windows
FFmpeg's supported native toolchain on Windows is MSYS2 + MinGW-w64:

1. Install [MSYS2](https://www.msys2.org) (default location `C:\msys64`).
2. Open the **UCRT64** shell (Start menu → "MSYS2 UCRT64").
3. Install the toolchain:
   ```sh
   pacman -Syu                      # restart the shell if it asks
   pacman -S --needed make nasm tar xz curl mingw-w64-ucrt-x86_64-toolchain
   ```
   (In a CLANG64 shell use `mingw-w64-clang-x86_64-toolchain` instead.)

## Build

```sh
cd /path/to/server-core/ffmpeg
./build.sh
```

Artifacts land in:

```
server-core/ffmpeg/
  src/ffmpeg-<version>/     downloaded + extracted source (cached)
  build/<os>-<arch>/        out-of-tree build dir (gen/ holds the generated lists)
  dist/<os>-<arch>/         install prefix: include/ lib/ bin/
```

## Options (environment variables)

| Variable          | Default | Meaning                                                        |
|-------------------|---------|----------------------------------------------------------------|
| `FFMPEG_VERSION`  | `9.0.2` | FFmpeg release to build (checksum only pinned for the default) |
| `FFMPEG_SHA256`   | pinned  | Expected tarball SHA256                                        |
| `BUILD_SHARED=1`  | off     | Build shared libraries/DLLs instead of static archives         |
| `ENABLE_GPL=1`    | off     | Add `--enable-gpl` (not needed for the curated set)            |
| `AUTODETECT=1`    | off     | Let configure use system libs (zlib, TLS for https, ...)       |
| `JOBS`            | `nproc` | Parallel make jobs                                             |
| `EXTRA_DECODERS`  | empty   | Extra decoders, e.g. `"musepack7 musepack8 atrac3"`            |
| `EXTRA_ENCODERS`  | empty   | Add encoders, e.g. `"flac pcm_s16le"` if you ever need output  |
| `EXTRA_DEMUXERS`  | empty   | Extra demuxers, e.g. `"oma rm aax"`                            |
| `EXTRA_MUXERS`    | empty   | Extra muxers                                                   |
| `EXTRA_PROTOCOLS` | empty   | Extra protocols                                                |
| `EXTRA_FILTERS`   | empty   | Extra filters, e.g. `"anequalizer anlmdn"`                     |
| `EXTRA_CONFIGURE` | empty   | Extra flags appended to `configure`                            |
| `LISTGEN_ONLY=1`  | off     | Only generate the component lists, then stop (debugging)       |

Examples:

```sh
BUILD_SHARED=1 ./build.sh                              # shared libs (ffmpeg.dll / .so)
EXTRA_DECODERS="musepack7 musepack8" ./build.sh        # add Musepack
EXTRA_ENCODERS="flac pcm_s16le" ./build.sh             # allow (test-)encoding again
FFMPEG_VERSION=8.1.2 ./build.sh                        # older stable branch
```

## Using the result

Static libraries + headers install to `dist/<os>-<arch>`. Compile against them
with pkg-config:

```sh
export PKG_CONFIG_PATH="$PWD/dist/linux-x86_64/lib/pkgconfig"
g++ app.cpp $(pkg-config --cflags --libs libavcodec libavformat libavfilter libswresample libavutil)
```

On Windows (UCRT64) the same works inside MSYS2; for MSVC projects prefer
`BUILD_SHARED=1` and generate an import library from the DLL with
`lib /def:ffmpeg.def /out:avcodec.lib` (or point CMake imported targets at
`dist/windows-x86_64-shared`).

## Notes

- **No encoders by default.** This is a pure playback/decode build. Add
  encoders with `EXTRA_ENCODERS` if you ever need to write files or
  re-encode for zone transport.
- **MP3 encoding** in particular requires the external `libmp3lame`
  (`EXTRA_CONFIGURE="--enable-libmp3lame"` + `AUTODETECT=1` + the dev package).
- **https:// streams:** TLS is not bundled; the `tls`/`https` protocols are
  selected but get pruned unless you build with `AUTODETECT=1` (uses schannel
  on Windows, gnutls/openssl on Linux). Plain `http`, `tcp`, `udp`, `file`,
  `pipe` work out of the box.
- **Smoke test** runs in stream-copy mode (`-c copy -f null -`) because a
  decode-only build has no encoders; full codec correctness is covered by
  FFmpeg's own FATE suite.
- **No assembler?** If `nasm`/`yasm` is missing the script falls back to
  `--disable-x86asm` automatically (correct but slower).
- The curated decoder set intersects `configure --list-decoders` per platform,
  so unavailable names are reported and skipped instead of failing the build.
