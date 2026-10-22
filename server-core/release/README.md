# Galdrun core for Linux

The Galdrun core audio server: it decodes the music, keeps the rooms in sync and
streams to every Galdrun client. Copy this whole folder to the Linux machine
matching its platform.

```bash
chmod +x run.sh galdrun-core               # if copying through Windows lost the permissions
cp server.example.json server.json         # first run only
./run.sh --config server.json
```

Keep `galdrun-core`, `run.sh` and `lib/` together: FFmpeg, OpenSSL, soxr and the
GCC runtimes it needs are in `lib/`. The system provides glibc and the CA
certificates for HTTPS streams (`sudo apt install ca-certificates`). The example
config uses the defaults; `./run.sh --help` lists the options. Rebuilds keep
your `server.json`.

| Folder | Runs on |
|---|---|
| `linux-armv7` | Raspberry Pi 2 or newer with a 32-bit OS; ARMv7 with NEON |
| `linux-armv8` | Raspberry Pi 3, 4, 5 with a 64-bit OS |
| `linux-armv9` | ARMv9-A hardware only (a Pi 5 is ARMv8.2) |
| `linux-x64` | 64-bit Intel/AMD |
| `linux-x86` | 32-bit Intel/AMD |

ARM builds need glibc 2.31 or newer (Raspberry Pi OS / DietPi Bullseye or newer).
The Intel builds need glibc 2.43 or newer (Ubuntu 26.04 or newer).

Source and licences: FFmpeg 9.0.2 (LGPL, audio decoders only, built by
`server-core/ffmpeg/build.sh`), OpenSSL 3.5.8, soxr and r8brain (in
`server-core/third_party`). All notices are in `licenses/`.
