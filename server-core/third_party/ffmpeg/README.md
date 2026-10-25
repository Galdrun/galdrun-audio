# FFmpeg — a binary drop, not a vendored source tree

libsoxr and r8brain sit next to this directory as sources that our own
`third_party/CMakeLists.txt` compiles. FFmpeg does not, and cannot: it is three
million lines behind its own configure script, and reproducing that build in
CMake would be a second project rather than a dependency. So this is a prebuilt
SDK for one platform, built by `../../ffmpeg`, and every other platform is
expected to have the libraries installed.

## What is here

| | |
| --- | --- |
| Version | FFmpeg **9.0.2** (`libavformat` 63, `libavcodec` 63, `libavutil` 61) |
| Build | `server-core/ffmpeg/build.sh`, shared, MinGW-w64 UCRT64, taken 2026-09-24 (phase 32) |
| Licence | **LGPL-2.1-or-later**: `LICENSE.txt`, and see below |
| Platform | Windows x64 only |

```text
include/        libavcodec/, libavformat/, libavutil/: the three we use
win64/lib/      avcodec.lib, avformat.lib, avutil.lib: import libraries
win64/bin/      the four DLLs the above need at run time, about 3.7 MB
```

The DLLs import nothing but Windows' own libraries and the UCRT. The MinGW
runtime is linked into them (`-static` at link time), so no `libgcc` or
`libwinpthread` DLL has to travel with them.

`swresample-7.dll` is here because `avcodec-63.dll` imports it, not because
this server uses it. The roadmap's ban on `libswresample` is a ban on
resampling in the decode path, and nothing in `src/decode/` links or calls it.
Leaving its DLL out would simply stop `avcodec` from loading.

`libavfilter`, `ffmpeg.exe` and `ffprobe.exe` are built by `server-core/ffmpeg` but not
copied here: nothing links them. `libavdevice` and `libswscale` are not built
at all.

## What the curated build contains

It is `server-core/ffmpeg`'s audio-only set: no encoders, no video codecs, no hardware
acceleration. `build.sh` fails if the result reports a single video codec.

- **Decoders.** Lossless: `flac alac wavpack ape tak tta shorten wmalossless
  truehd dst`. DSD: `dsd_lsbf dsd_msbf dsd_lsbf_planar dsd_msbf_planar`.
  Lossy: `mp1 mp2 mp3 aac aac_latm ac3 eac3 dca vorbis opus`. The `pcm_*`
  family at every depth, and `adpcm_ima_wav adpcm_ms`.
- **Demuxers.** `wav aiff caf flac ogg mov asf matroska wv ape tak tta dsf
  iff mp3 aac ac3 eac3 dts truehd loas concat hls mpegts s337m` and the raw
  `pcm_*` ones.
- **Protocols.** `file pipe concat http httpproxy tcp udp tls https data fd
  subfile cache`. `https` and `tls` are there because the build enables
  Schannel, the OS's own TLS. Without that, `server-core/ffmpeg` prunes both, and phase
  20's https `media.baseUrl` would fail at open.

Parsers and bitstream filters are all enabled. `configure-args.txt` in
`server-core/ffmpeg/build/windows-x86_64-shared/` has the exact list.

## The licence, and why the build is shared

This is an **LGPL** build: it was configured without `--enable-gpl` and
without `--enable-version3`, so none of FFmpeg's GPL-only components are in it
and its licence is LGPL-2.1-or-later. The BtbN drop it replaced was
LGPL-3.0-or-later. A GPL build would make this server's binaries GPL, and that
is a decision for whoever ships the product, not one to inherit from a
convenient download.

The libraries are **shared** rather than static, and that is also deliberate:
the LGPL's relinking obligation is met by the DLLs sitting beside the
executable, where anyone can replace them with their own build of the same
version. That is why `multiroom_copy_ffmpeg_runtime()` in the root
`CMakeLists.txt` copies them next to every binary that links them. The copy is
part of meeting the licence, not a convenience.

`server-core/ffmpeg` can build static archives too, and they would drop four DLLs. That
is not the choice here, for two reasons. A static FFmpeg in a distributed
binary has to meet the relinking obligation some other way (object files, or
source for the whole product). And MinGW `.a` archives linked into an MSVC
executable need the MinGW runtime beside them. Switching is a licence decision
for whoever ships the product.

libsoxr is in the same position (LGPL-2.1-or-later) but is linked statically,
so a distributed binary that links it carries the obligation to make relinking
possible some other way.

## Rebuilding it

From `server-core/ffmpeg/`, in PowerShell:

```powershell
$env:BUILD_SHARED = '1'
$env:EXTRA_CONFIGURE = '--enable-schannel'
powershell -ExecutionPolicy Bypass -File .\build-windows.ps1
```

`build-windows.ps1` sets up a portable MSYS2 UCRT64 toolchain under
`server-core/ffmpeg/tools/` once, then runs `build.sh` in it. Inside an existing UCRT64
shell, `BUILD_SHARED=1 EXTRA_CONFIGURE=--enable-schannel ./build.sh` is the
same build. Do not add `AUTODETECT=1`: it would pull in whatever the build
machine happens to have.

Then copy from `server-core/ffmpeg/dist/windows-x86_64-shared/`:

| From | To |
| --- | --- |
| `include/libavcodec`, `include/libavformat`, `include/libavutil` | `include/` |
| `bin/avcodec.lib`, `bin/avformat.lib`, `bin/avutil.lib` | `win64/lib/` |
| `bin/avcodec-*.dll`, `bin/avformat-*.dll`, `bin/avutil-*.dll`, `bin/swresample-*.dll` | `win64/bin/` |

`server-core/ffmpeg` puts the import libraries in `bin/` beside the DLLs. The layout here
keeps them in `win64/lib/`, so the CMake lookup does not change. A major
version bump renames the DLLs, and the CMake glob picks up whatever is there.
Re-run CMake's configure step afterwards, because the list of DLLs to copy is
read when CMake configures.

## Building elsewhere

Two other ways to point the build at FFmpeg, both in
`third_party/CMakeLists.txt` and both preferred over editing this directory:

- `-DMULTIROOM_FFMPEG_ROOT=<path>`: any SDK laid out as `include/` and `lib/`.
  On Linux, `server-core/ffmpeg`'s own `dist/linux-x86_64` is one. A shared
  Windows build's `dist/` works too (import libraries in `bin/`): the
  `windows-armv8` preset uses `dist/windows-armv8-shared`.
- Nothing at all on Linux or macOS: the build falls through to `pkg-config`
  and uses the system `libavformat`, `libavcodec` and `libavutil`.

`-DMULTIROOM_WITH_FFMPEG=OFF` builds without a decoder. The media sources and
their tests still compile, but `FfmpegDecoder` and `HttpMediaSource` do not.
