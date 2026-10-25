# Galdrun core for Windows

The Galdrun core audio server: it decodes the music, keeps the rooms in sync and
streams to every Galdrun client. Keep this whole folder together: the FFmpeg
DLLs and the Visual C++ runtime it needs are next to `galdrun-core.exe`, so
nothing has to be installed.

```powershell
copy server.example.json server.json     # first run only
.\galdrun-core.exe --config server.json
```

Without `--config` the core uses `%LOCALAPPDATA%\Galdrun\server.json`. The
example config uses the defaults; `.\galdrun-core.exe --help` lists the options.
Rebuilds keep your `server.json`.

Clients connect on TCP 4711 and 4712 and announce themselves on UDP 4721; allow
these in the Windows firewall when Windows asks. The control API for the Galdrun
server listens on `127.0.0.1:4713` only.

As a Windows service, register it with an absolute config path, for example:

```powershell
sc.exe create GaldrunCore binPath= "\"C:\Galdrun\core\galdrun-core.exe\" --service --config \"C:\Galdrun\core\server.json\"" start= auto
sc.exe start GaldrunCore
```

`windows-x64`: Windows 10/11, 64-bit Intel/AMD. `windows-armv8`: Windows 11 on
ARM64, built on x64 and not yet tested on an ARM64 device. Source and licences: FFmpeg 9.0.2 (LGPL, audio decoders
only, built by `server-core/ffmpeg/build-windows.ps1`), soxr and r8brain (in
`server-core/third_party`). All notices are in `licenses/`.
