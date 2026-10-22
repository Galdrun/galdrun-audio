# Galdrun Linux client

Copy this entire folder to the Linux device matching its platform. The folder
includes the client, any dynamically linked GCC runtimes in `lib/`, their
license notices, a launcher, an example config, and `build-info.txt` recording
the version and Debug/Release configuration. It contains no bench settings
or device cache. Later builds update these files; your `client.json` is kept.

Install the system audio library on Debian, DietPi or Ubuntu:

```bash
sudo apt update
sudo apt install libasound2
```

On releases with the renamed package, install `libasound2t64` instead.
Use the target OS's ALSA plugins and `/usr/share/alsa` configuration; neither
ALSA nor glibc is bundled. A PulseAudio/WSLg output also needs
`libasound2-plugins` for the executable's architecture. The 32-bit x86 client
on a 64-bit OS needs its 32-bit glibc and ALSA packages (architecture `i386`).

ARM builds require glibc 2.31 or newer and Linux 5.4 or newer; DietPi based on
Bullseye or newer is the intended target. Intel x64/x86 builds use Ubuntu
26.04's toolchain/runtime and should be deployed to Ubuntu 26.04 or a
compatible newer environment. A different CPU architecture, an older glibc,
or musl-based Linux such as Alpine requires a matching build.

```bash
chmod +x galdrun-client run.sh
cp client.example.json client.json  # first run only
./run.sh --config ./client.json --server 192.168.1.10:4711
```

Replace the address with your server. The empty client ID is generated and
saved on first run. Edit `client.json` to select a device; defaults use ALSA's
system default. Without `--config`, the client uses its normal XDG config
directory. Command-line overrides are not saved to the config.

```bash
./run.sh --version
./run.sh --help
```

The launcher uses the adjacent runtime libraries. Keep `lib/` beside the
executable and use `run.sh` rather than relying on the system's GCC runtime.
WSLg's shared output exercises Linux playback but does not expose a dedicated
Linux hardware audio clock. Stop the client with Ctrl+C.
