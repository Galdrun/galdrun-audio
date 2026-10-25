# Galdrun server

The Galdrun server runs the multiroom system: music library and streaming
services, queues, zones and rooms, the web interface and the API for apps. It
plays through the Galdrun core (`galdrun-core`), which it controls on
`127.0.0.1:4713`; run both on the same machine.

This folder is self-contained: .NET is included, nothing has to be installed.
Keep all files together. Linux needs the usual system libraries (glibc,
libstdc++, OpenSSL, ICU, zlib) and `ca-certificates` for HTTPS.

```
cp server.example.json server.json               # first run only (Windows: copy)
./galdrun-server --config server.json            # Linux; chmod +x galdrun-server if needed
.\galdrun-server.exe --config server.json        # Windows
```

Then open the web interface at `http://127.0.0.1:4723/`. Ctrl+C stops the server.
The library, playback state and caches go to `data/` beside `server.json` (the
example's `dataDirectory`). Rebuilds keep `server.json` and `data/`.

| Port | Use | Listens on |
|---|---|---|
| 4723 | Web interface | 127.0.0.1 (`gui.bind`, `gui.port`) |
| 4725 | API for apps and MCP | 127.0.0.1 (`api.bind`, `api.port`) |
| 4714 | Media for the core | the core fetches from here (`media.port`) |
| 4713 | The core's control API | `core.host`, `core.port` |

The web interface has no login of its own and forwards `api.token` to the API.
Bind it to a network address only on a trusted network.

**As a service.** Linux: copy `galdrun-server.service` to `/etc/systemd/system/`,
adjust `ExecStart` and the config path (defaults: `/opt/galdrun/server`,
`/etc/galdrun/galdrun-server.json`), then `systemctl enable --now galdrun-server`.
Windows: `sc.exe create GaldrunServer binPath= "\"C:\Galdrun\server\galdrun-server.exe\" --config \"C:\Galdrun\server\server.json\"" start= auto`.

| Folder | Runs on |
|---|---|
| `windows-x64`, `windows-armv8` | Windows 10/11, 64-bit Intel/AMD or ARM |
| `linux-x64` | 64-bit Intel/AMD Linux |
| `linux-armv7` | Raspberry Pi 2 or newer with a 32-bit OS |
| `linux-armv8` | Raspberry Pi 3, 4, 5 with a 64-bit OS |
| `linux-armv9` | ARMv9-A hardware; the same build as `linux-armv8`, named to match the core |

Built on .NET 11; the supported Linux distributions are listed at
https://github.com/dotnet/core/blob/main/release-notes/11.0/supported-os.md.
The .NET runtime's licences are in `licenses/`.
