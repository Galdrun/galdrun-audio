# Galdrun App

> **Early work.** This app has only just been started and is far from finished.
> Expect missing features, rough edges and changes without notice. To control
> Galdrun today, use the [server](../server/README.md)'s web interface.

A remote control for a Galdrun multiroom system, for phones, tablets and
desktops. It talks to the Galdrun server through its API, like any other app
could.

What already works, more or less:

- **Find servers** on the network (mDNS) or enter an address; an optional API
  token is kept for the session only, never saved.
- **Rooms and zones**: see what plays where, switch zones by swiping.
- **Now playing, transport and queue**: play, pause, skip, play next, add to queue.
- **Browse and search** the server's sources; **collections**: playlists,
  favourites, recommendations.

Built with [Avalonia](https://avaloniaui.net/) 12 on .NET, one shared UI for
desktop, Android and iOS.

## Build

From the repository root; installs the .NET SDK first when it is missing (the
version in `mobile-gui/global.json`, via Microsoft's dotnet-install). Only the
desktop version is built so far.

```sh
scripts/build-mobile-gui.sh                    # Linux: all targets
```

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-mobile-gui.ps1                    # Windows: all targets
powershell -ExecutionPolicy Bypass -File scripts\build-mobile-gui.ps1 -Target windows-x64
```

Targets: `all` (default), `windows` (`windows-x64`, `windows-armv8`), `linux`
(`linux-x64`, `linux-armv7`, `linux-armv8`). Results go to
`releases/mobile-gui/<target>`, self-contained, started with `galdrun.exe` or
`./galdrun`.

Android and iOS have their projects (`src/Multiroom.App.Android`,
`src/Multiroom.App.iOS`) but are not built by the scripts: Android needs the .NET
`android` workload (`dotnet workload install android`), iOS a Mac with Xcode.

## Development

```sh
cd mobile-gui
dotnet build
dotnet test
```

| Project | Content |
|---|---|
| `src/Multiroom.App.Api` | The server's API: client, contracts, event stream, discovery |
| `src/Multiroom.App` | Views and view models, shared by every platform |
| `src/Multiroom.App.Desktop`, `.Android`, `.iOS` | Platform entry points |
| `tests/Multiroom.App.Tests` | Unit and headless UI tests |

The libraries target .NET 10 and the desktop entry point .NET 11; the pinned
.NET 11 SDK builds both. Running the tests also needs the .NET 10 runtime.
