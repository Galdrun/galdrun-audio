# Galdrun Server

The Galdrun server is the heart of a Galdrun multiroom system. It knows your
music, runs the queues of every zone and tells the [core](../server-core/README.md)
what to play; the core streams it in sync to the [clients](../client/README.md).
You control everything in the browser, through the API, or by asking an AI.

- **Music sources**: your own files (local folders and SMB shares, with a
  library, covers and metadata from MusicBrainz), Tidal (unofficial, see the [main README](../README.md#tidal)), TuneIn and
  web radio. One search across all of them.
- **Zones and rooms**: group rooms into zones, one queue per zone, playlists,
  favourites, likes, play history, audiobook positions and AutoFill when a queue
  runs out.
- **Per room**: volume, equalizer with profiles, and how the room is kept in sync.
- **Web interface** on port 4723 for everything above, including the client
  devices' settings.
- **API and MCP**: a REST API under `/api/v1` for apps, and an MCP server so AI
  assistants can control the house.
- **AI** (optional): song suggestions and an agent via OpenAI, Anthropic, Google
  Gemini, Mistral, Groq, OpenRouter or a local Ollama.
- **Users** and an announcement on the network (mDNS), so apps find the server.

## Supported systems

| Target | Runs on |
|---|---|
| `windows-x64` | Windows 10/11, 64-bit Intel/AMD |
| `windows-armv8` | Windows 11 on ARM |
| `linux-x64` | 64-bit Intel/AMD Linux |
| `linux-armv7` | Raspberry Pi 2 or newer with a 32-bit OS |
| `linux-armv8` | Raspberry Pi 3, 4, 5 with a 64-bit OS |
| `linux-armv9` | ARMv9-A hardware; the `linux-armv8` build, named to match the core |

Every release is self-contained: .NET is included, nothing has to be installed.
Linux needs the usual system libraries (glibc, libstdc++, OpenSSL, ICU, zlib);
the supported distributions are listed by
[.NET 11](https://github.com/dotnet/core/blob/main/release-notes/11.0/supported-os.md).

## Running it

Run the server and the core on the same machine.

```
cp server.example.json server.json               # first run only (Windows: copy)
./galdrun-server --config server.json            # Linux
.\galdrun-server.exe --config server.json        # Windows
```

Then open `http://127.0.0.1:4723/`. Without `--config` the server keeps its data
in `%LOCALAPPDATA%\Galdrun\Server` (Linux: `~/.local/share/Galdrun/Server`).

| Port | Use | Default bind |
|---|---|---|
| 4723 | Web interface | 127.0.0.1 (`gui.bind`, `gui.port`) |
| 4725 | REST API and MCP | 127.0.0.1 (`api.bind`, `api.port`) |
| 4714 | Media the core fetches | `media.port` |
| 4713 | The core's control API | `core.host`, `core.port` |

The web interface has no login of its own and forwards `api.token` to the API,
so bind it to a network address only on a trusted network. Every setting can
also come from an environment variable, `GALDRUN_<section>__<key>`, which is the
better place for API keys. Running it as a systemd or Windows service is
described in the release's README.

## Build

One script per host builds every target and installs the .NET SDK first when
it is missing. .NET builds all targets from either host. Run it from the
repository root.

```sh
scripts/build-server.sh                        # Linux: all targets
scripts/build-server.sh linux-armv7 windows-x64
```

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-server.ps1                    # Windows: all targets
powershell -ExecutionPolicy Bypass -File scripts\build-server.ps1 -Target linux-armv8
```

Targets: `all` (default), the groups `windows` and `linux`, and every name from
[Supported systems](#supported-systems). Results go to `releases/server/<target>`;
the web interface from [`../server-gui/www`](../server-gui/www) is packed into
each release's `gui/` folder.

| What | Where |
|---|---|
| .NET SDK, the version pinned in `server/global.json` | `%LOCALAPPDATA%\Microsoft\dotnet` / `~/.dotnet`, via Microsoft's dotnet-install |
| curl, CA certificates, ICU (Linux only) | via `apt` (`sudo`) |

## Scripts

| Script | Purpose |
|---|---|
| `scripts/build-server.sh`, `.ps1` | Publish all targets, see [Build](#build). |
| `server/scripts/verify-linux-release.py` | Checks the Linux releases' architectures and bundled runtime, then starts the x64 server: API, database and a clean shutdown. Run it on Linux or WSL. |

## Development

C# on .NET 11, projects in `server/src`, tests in `server/tests`.

```sh
cd server
dotnet build
dotnet test
```

Run it from the source tree with
`dotnet run --project src/Multiroom.Server -- --config <path>`. The web interface
is served from the copy of `server-gui/www` that every build puts into `gui/`
beside the executable, so rebuild after changing it. Use a config of its own:
without `--config` a development server shares its data folder with an
installed one.
