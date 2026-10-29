# Build, test and local runtime

Repository-level scripts live in `scripts/`; run them from the repository root
using that prefix. Commands under a component heading run from that component
directory. Reuse configured build trees where possible; inspect `CMakeCache.txt`
for the source directory, generator and options.

## Script directory map

| Script (relative to repository root) | Purpose / when to use |
| --- | --- |
| `scripts/build-client.ps1`, `scripts/build-client.sh` | Build desktop/Linux clients, ESP32, Connector 2 firmware or `rpi1`/`rpi3`/`rpi4`/`rpi5` SD images; bootstrap tools; export to `releases/client/` |
| `scripts/build-server-core.ps1`, `scripts/build-server-core.sh` | Build/export the C++ audio engine and its runtime libraries to `releases/server-core/` (`windows-x64`, `windows-armv8`, `linux-x64`, `linux-armv7`/`armv8`/`armv9`; no `linux-x86`) |
| `scripts/build-server.ps1`, `scripts/build-server.sh` | Publish self-contained .NET server releases, including the browser GUI, to `releases/server/` (same targets as the core; `linux-armv9` is the `linux-arm64` build under its own name). `rpi3`/`rpi4`/`rpi5` (`raspberrypi`) build Pi server SD images into `releases/server/raspberrypi/`: core and client image first, then `hardware/buildroot/scripts/build-server-image.sh` |
| `scripts/release/package.py` (gitignored, local) | Pack `releases/` into release archives, plus one `galdrun-server-bundle-*` per target with both server and core, using `scripts/release/bundle/` (README, `start.sh`, `start.cmd`/`start.ps1`) |
| `scripts/copy-server-gui.ps1` | Copy `server-gui/www/` and shared web assets into an existing server release's `gui/`; defaults to `windows-x64`, accepts `-Target <target>` |
| `scripts/build-mobile-gui.ps1`, `scripts/build-mobile-gui.sh` | Publish the Avalonia desktop app; these scripts do not build Android/iOS |
| `scripts/start-mobile-gui.ps1` | Start the Avalonia desktop app from the `windows-x64` release at phone size (`-Desktop`: desktop size); does not build |
| `scripts/setup/windows.ps1`, `scripts/setup/linux.sh` | Helpers sourced by build scripts for tool/dependency setup; not standalone entry points |
| `scripts/start-local.ps1` | Start core, server and one client from Windows x64 releases with isolated configs/logs under `scripts/.run/` |
| `scripts/stop-local.ps1` | Graceful stop, then timeout/kill, for this checkout's running release processes |
| `scripts/restart-local.ps1` | Stop then start the local stack; does not rebuild |
| `scripts/upload/linux.ps1`, `scripts/upload/linux.sh` | Detect architecture and Galdrun BusyBox/systemd images over SSH; update the matching client and preserve config |
| `scripts/upload/connector2.ps1`, `scripts/upload/connector2.sh` | SSH client-only update for existing Connector 2 firmware; test the binary first, preserve settings/identity, retain the previous binary |
| `scripts/upload/esp32.ps1`, `scripts/upload/esp32.sh` | Flash an already-built board release over USB serial with esptool; preserve the storage partition |
| `scripts/upload/linux-remote.sh`, `scripts/upload/connector2-remote.sh` | Device-side installers invoked by the corresponding upload script |

Read each script's header and parameters before invoking it. Build scripts take
target groups or individual targets (`-Target` in PowerShell, positional in
Bash); omitted targets generally mean all. `.ps1` and `.sh` are host variants,
with Windows C++ cross-builds delegated to WSL. Upload scripts affect actual
devices; the local-process permission below does not authorize remote updates.
Component-specific dependency/verification tools remain in `client/scripts/`,
`server-core/scripts/`, `server/scripts/`, `server-core/ffmpeg/`, and
`hardware/buildroot/`; see the relevant component README for those workflows.

## Stop local processes before building

**Agents may close running Galdrun servers, cores and clients on this machine
without asking again.** Stop them before a build: Windows locks running `.exe`
files and loaded DLLs, so link/export/publish steps fail when replacing them.
Client/core CMake builds copy binaries to `releases/` even for development
builds; stopping only an instance from `build/` may leave the release locked.
Running instances can also occupy test/manual-check ports or the audio device.

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/stop-local.ps1
```

The script matches `galdrun-server`, `galdrun-core`, `galdrun-client` executable
paths under this checkout's `releases/`. It sends Ctrl+C first (allowing state
to save), waits up to `-TimeoutSeconds` (default 10), then kills survivors.
It does **not** cover every instance on the machine. If a file is still locked,
inspect processes/services for the exact Galdrun path being rebuilt; a source-run
server may be hosted by `dotnet`. Stop those identified local Galdrun instances
too, gracefully where possible, then force only a survivor that prevents the
build. A service may need stopping through its service manager to avoid restart.
Do not stop unrelated `dotnet` processes or remote devices under this permission.

## C++ client

From `client/`, Windows with Visual Studio 2022 C++ tools and CMake:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
ctest --preset windows-x64
```

From `client/`, native Linux with CMake, Ninja, C++17 and ALSA development headers:

```sh
cmake --preset linux-x64
cmake --build --preset linux-x64
ctest --preset linux-x64
```

Presets build into `client/build/<preset>/`. Some checkouts already use the
older `client/build/` tree; for that tree, from the repo root use
`cmake --build client/build --config Release` and
`ctest --test-dir client/build -C Release --output-on-failure`.
Do not mix generators within a build directory.

## C++ core

From `server-core/`, Windows:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
ctest --preset windows-x64
```

Linux uses the same three commands with `linux-x64`. Presets enable FFmpeg,
soxr and r8brain. Windows FFmpeg is bundled in `third_party/ffmpeg/`; Linux's
preset expects `ffmpeg/dist/linux-x64-shared`. Use the dependency/build scripts
in [the core README](../../server-core/README.md) if absent. `windows-armv8`
cross-compiles: it needs the Visual Studio component "MSVC ARM64 build tools"
and `ffmpeg/dist/windows-armv8-shared` (`ffmpeg/build-windows.ps1 -Target armv8`,
llvm-mingw), has no tests, and cannot run on the x64 host. Bare CMake defaults
differ from presets: resamplers are off, FFmpeg on. A deliberately reduced
`-DMULTIROOM_WITH_FFMPEG=OFF` build is not a playback verification.

Both C++ projects enable `MULTIROOM_BUILD_TESTS` by default; cross-target
presets disable them. CTest registers aggregate executables (`multiroom_tests`
for client, `multiroom_server_tests` for core), not each `MULTIROOM_TEST`.
Core's executable accepts substring filters, e.g. from the repo root:

```powershell
& ./server-core/build/windows-x64/tests/Release/multiroom_server_tests.exe output_planner
```

Client's main runner runs all registered cases. Linux has additional client
backend test executables. Rebuild the relevant tests before running; a passing
stale executable does not verify changes. Investigate timing-sensitive failures
with a targeted rerun; report failures that persist.

## .NET server

From `server/`:

```sh
dotnet build
dotnet test
dotnet run --project src/Multiroom.Server -- --config <existing-config-path>
```

`global.json` pins the SDK and selects Microsoft.Testing.Platform; tests use
xUnit v3 executables. A focused run from `server/` is:

```sh
dotnet test --project tests/Multiroom.Server.Tests/Multiroom.Server.Tests.csproj --filter-class Multiroom.Server.Tests.CoreLinkTests
```

Use `--filter-class` / `--filter-method`, as reported by `dotnet test --help`,
rather than VSTest's `--filter`. `Directory.Build.props` enables warnings as errors.
There is no `package.json`/npm build for these three components.

## Release builds and smoke checks

From the repository root, build only the needed target on Windows:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-client.ps1 -Target windows-x64
powershell -ExecutionPolicy Bypass -File scripts\build-server-core.ps1 -Target windows-x64
powershell -ExecutionPolicy Bypass -File scripts\build-server.ps1 -Target windows-x64
```

Linux equivalents are `scripts/build-client.sh linux-x64`,
`scripts/build-server-core.sh linux-x64`, `scripts/build-server.sh linux-x64`. These scripts
bootstrap missing tools/dependencies and produce `releases/<component>/<target>/`;
avoid a default all-target build for a small edit. Windows uses WSL for C++
Linux/firmware targets. For release/architecture checks inspect
`client/scripts/verify-linux-binaries.sh`,
`server-core/scripts/verify-linux-binaries.sh`, and
`server/scripts/verify-linux-release.py` (Linux/WSL). The server and the core
take their version from `shared/branding/server-version.txt`; the server logs a
warning when the core it reaches reports another one.

After rebuilding, start a local stack when needed with
`powershell -ExecutionPolicy Bypass -File scripts/start-local.ps1`.
`restart-local.ps1` stops then starts; it does not build. These scripts run the
Windows x64 releases. Configs are `scripts/.run/config/{server,core,client}.json`;
logs/data are under `scripts/.run/{server,core,client}/` and remain untracked.
The GUI is `http://127.0.0.1:4723/`; client settings are on `4720` (configs created
before the port change may still set them the other way round).
Preserve existing configs/data instead of resetting them to debug a build.
Starting scripts explicitly opens consoles; automated helper processes launched
by agents should use hidden windows unless an interactive console is requested.

For manual runs, set separate config/data/cache paths so experiments do not
reuse installed state. Useful observations: core JSON `status`, REST
`/api/v1/status` and `/api/v1/system`, client `status`/`diagnostics`, and the
component logs. The server GUI defaults to `4723`, every client's settings page to `4720`.

## Pick verification by changed surface

Connector 3 has an experimental SSH/kexec RAM build containing the ARMv7 client;
see [the Connector 3 guide](../hardware/connector3.md#custom-ram-build).
The earlier USB diagnostic's FIT signing inputs and host checks remain in
[the investigation guide](../hardware/raumfeld/connector3.md). Neither is a
supported persistent client firmware target. Host verification does not establish
physical boot, SSH reachability or playback. Separately, the 6.12.112 RAM kernel
has passed a live kexec boot, Ethernet/SSH/client-API checks and audio format
negotiation. Its hash-bound `hardware-verification.json` records those checks;
The native MCU UART/uinput driver has packet-parser self-checks and a physical
UART capture of all nine controls; the real client API exposes all nine bindings.
WLAN initialization, DHCP and Wi-Fi-only SSH/traffic have since been verified
under 6.12.112 with this device's local 15.87.19.p14 radio firmware. The generic
upstream radio firmware still fails. The optional second argument to
`hardware/buildroot/scripts/build-connector3-ram.sh` selects that hash-checked
local file and writes private artifacts under `private/connector3/ram-local/`.
See the hardware guide's [session handoff](../hardware/connector3.md#wlan-fix-and-session-handoff-2026-10-29)
for evidence and the final build's hash-bound hardware verification. Audible playback
and cold boot remain unqualified. Firmware selection/cache isolation is checked
by `python3 hardware/buildroot/scripts/test-connector3-wifi-firmware.py`.
`python3 hardware/buildroot/scripts/test-connector3-ram-init.py` checks missing/
existing SSH host keys and the address/SSH recovery guard without hardware.
The guard requires an SSH process as well as an IPv4 address; it cannot recover
a kernel hang before userspace. See the hardware guide's dated retest status.
`hardware/buildroot/scripts/prepare-connector3-vendor-ram.py` prepares the built
`rootfs.tar.gz` under the beta's `/run`, with private network settings supplied
separately. Preparation leaves the current network running; the printed start
command hands networking to the RAM service. The corrected helper has been
verified on the vendor kernel: nine inputs, distinct ALSA names, live Wi-Fi
settings and scanning. Its first handover failed because the SSH PATH omitted
service directories and an enabled WPA helper was missing from a cached build.
Read the hardware guide for the preflight, recovery limits and current runtime.

Pi image builds are `scripts/build-client.sh rpi1 rpi3 rpi4 rpi5` (Windows: the equivalent
PowerShell targets). See [the Pi guide](../../hardware/buildroot/board/rpi/README.md).
Firmware host checks live in `hardware/buildroot/scripts/test-*.py`; in particular
`test-server-image.py` checks final server-image metadata. Server image assembly
builds ICU and its dependencies in `<board>-server-deps` beside the client build
cache; it preserves the client filesystem and config. The assembled image's
manifest records its own partition sizes and hashes, plus server/core provenance.
Image assembly also runs `test-server-runtime.py <rootfs> <board>`: core ABI,
enabled client defaults, .NET API, ICU browsing, SQLite and clean shutdown.
ARM32 QEMU validation alone sets `DOTNET_EnableWriteXorExecute=0`: the default
dual-mapped JIT memory crashed QEMU 10.2.1 in CoreCLR `MethodTable::CanCastTo`.
The device service retains .NET's production W^X default. An emulated pass does
not qualify physical boot or playback.
For shared appliance changes,
run network/init/scan, client-image, SSH, build-cache, `test-rpi-profile.py`,
`test-rpi-inputs.py` and
`test-appliance-update.py` for shared-service changes. `verify-rpi-images.py` is
invoked by each image build. `test-appliance.py http://DEVICE:4720` exercises a
running appliance using reads and rejected invalid writes. It requires an
explicit test device; it does not flash an SD card.

| Change | Relevant checks |
| --- | --- |
| Shared MATP/framing/sample formats | Client + core suites; keep protocol doc in sync |
| Core control command/status/event | Core `test_control_api.cpp` and listener tests + .NET `CoreLinkTests`/queue tests |
| Client device/render/timing | Client suite; backend-specific Linux/Windows/ESP32 validation as applicable |
| .NET endpoint/queue/provider/storage | .NET build/tests for the feature; core checks if its contract changes |
| Server GUI/shared web assets | Run `powershell -ExecutionPolicy Bypass -File scripts/copy-server-gui.ps1` (or add `-Target <target>`), refresh the browser, and exercise the affected page; no server rebuild/restart for GUI-only changes |
| Device web assets | Rebuild their owning executable/copy and exercise the affected page |
| Documentation only | Check relative links, source paths, command/preset definitions and `git diff --check` |

Browser helpers under `client/tests/*.cjs` are manual scripts outside CTest;
inspect their launch paths and target URLs first. Some retain pre-rename paths
or target an actual device by default. Do not run them blindly as unit tests.
