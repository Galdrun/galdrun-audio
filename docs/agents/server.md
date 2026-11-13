# .NET server implementation map

Paths below are relative to `server/` unless stated otherwise. Under `src/`,
project names start with `Multiroom.Server`; abbreviations such as `Playback/`
below mean `src/Multiroom.Server.Playback/`. Commands: [development.md](development.md).

## Projects and entry points

| Project | Responsibility / first files |
| --- | --- |
| `Multiroom.Server` | Composition root: `Program.cs` -> `ServerHost.cs`; `Configuration/ConfigLoader.cs`, `ServerOptions.cs`; logging and service lifecycle |
| `Multiroom.Server.Abstractions` | Shared contracts: `Result.cs`, `Queue.cs`, `Providers.cs`, `ResolvedStream.cs`, `MediaPublisher.cs`, `ApiError.cs` |
| `Multiroom.Server.Library` | SQLite/indexer/catalog/stores, SQL migrations, local/SMB folders, playlists, users, likes/history/book positions |
| `Multiroom.Server.Providers` | `ProviderRegistry.cs`, `UnifiedSearch.cs`, `SongResolver.cs`; `Local/`, `WebRadio/`, `TuneIn/`, `Tidal/`, `Streaming/`, `Covers/`, `AddIns/` |
| `Multiroom.Server.Playback` | Queues/zone policy/persistence and the `Core/` TCP client |
| `Multiroom.Server.Web` | Independent `Api/ApiEndpoint`, `Media/MediaEndpoint`, and `Gui/GuiEndpoint` Kestrel listeners |
| `Multiroom.Server.Mcp` | `MultiroomMcpServer.cs`, `MultiroomTools.cs`, caller tiers and client store |
| `Multiroom.Server.Ai` | Provider registry/settings/login, `Agent/MultiroomAgent.cs`, `Suggestions/` |

`ServerHost.CreateBuilder` explicitly registers services and their order.
Hosted services start in registration order and stop in reverse. In particular,
library migration precedes providers; media publishing precedes `CoreLink`;
`PlaybackStore` restores before `CoreLink` connects and saves after it stops;
external endpoints start after their dependencies. Preserve these dependencies.

## Playback and persistence

| Concern | First files in `Playback/` |
| --- | --- |
| Queue API, event subscription, source/item IDs | `QueueEngine.cs` |
| Queue edits, staging, transport, shuffle/repeat, groups/chapters, playhead | `ZoneSession.cs` |
| One zone per room, joining, deleting empty zones | `ZoneKeeper.cs` |
| Actual + desired room/zone views and shared operations for REST/MCP | `House.cs` |
| Core request/reply/event shapes | `Core/CoreCommands.cs`, `CoreReplies.cs`, `CoreEvents.cs` |
| Reconnect, serialization, event routing/resync | `Core/CoreLink.cs` |
| Actual snapshot versus desired settings | `Core/CoreModel.cs`, `DesiredState.cs` |
| Selected core and beacon discovery | `Core/CoreDiscovery.cs` |
| Save/restore queues and desired state | `PlaybackStore.cs`, `PlaybackSettings.cs` |
| Playlist playback, AutoFill, history, book progress, room buttons | `PlaylistPlayer.cs`, `AutoFill.cs`, `PlayHistoryRecorder.cs`, `BookPositions.cs`, `InputActions.cs` |
| Timed plays (weekly playlist start/stop) | `TimedPlays.cs`; store `Library/TimedPlayStore.cs`; REST `Web/Api/ApiEndpoint.TimedPlays.cs`; MCP `*_timed_play(s)` tools |
| Sleep timer (per zone) | `SleepTimers.cs` (state, in memory), fired by `TimedPlays.cs`; REST `zones/{zone}/sleep-timer` in `Web/Api/ApiEndpoint.Zones.cs`; MCP `set_sleep_timer`; fade length is the playback setting `sleepFadeSeconds` |

`TimedPlays` ticks every 5 s on server local time. It starts a stretch of
blocks once, only in the timed play's rooms that play nothing (no
playing/starting zone with a current item (a queue played to its end is
idle), no Bluetooth local source), grouped into one zone, and
stops only a zone it started whose queue still holds an item it queued.
Touching blocks (24:00 / 00:00) are one stretch. Only the zone level is set
(fades ramp it; a level set by someone else ends the ramp), never a room's.
A fade in waits for the queue's `NowPlaying` (the rooms really started: the
transport reads stopped/playing before that); after a fade out the level comes
back 2 s after the stop, since the rooms play out what they hold first.
A zone's sleep timer overrides timed plays: while it runs the zone's rooms
count as busy and a block's end leaves the zone playing; when it is due,
`TimedPlays` fades the zone out over `sleepFadeSeconds` (default 30) and stops
it, ending a run there (its resume place kept). Timers live in memory only (a
restart ends them) and fire on the 5 s tick; `zone.sleepTimerChanged` reports
every change.
Its run and resume place are kept in `timed_play`, so a restart neither starts
the stretch again nor misses its stop; a missed start is made late within the
block. Nothing is decided while no core is connected. Every store write
(edits, run, resume) raises `TimedPlayStore.Changed`, published as the SSE
event `timedPlay.changed`, which the GUI page applies instead of polling.

`ZoneSession` serializes operations and core events with its own gate. It
resolves/publishes/stages one source ahead; `zone.sourceNeeded` continues that
loop. A `sourceId` identifies a staging, not a track; reused IDs are treated as
replays by the core. Audible `sourceStarted` events and playhead state differ
from the source currently being decoded. Groups contain parts; chapters of
one file may share one staged source and use seek for navigation.

A room's `localSource` (MATP; `bluetooth` while a phone plays on its device)
is a room fact, not a power state (`actual.localSource`, event
`room.localSource`). When it starts on a playing zone's **only** room,
`QueueEngine` pauses the zone (`ZoneSession.BluetoothAsync`) so the next track
does not fail with no room ready; when it ends, the zone plays on. A caller's
play, pause or stop in between cancels that resume. Zones with other rooms play on.

A room that reports `bluetoothOutput` is the second client on a device whose
buttons the device's own room reports too, bound on that room's page.
`InputActions` never runs the Bluetooth room's own input reports; with its
desired `inputsEnabled` (REST room patch, server-only, default off) it runs
the device room's press for it as well (same `actual.address`), a zone
action only once when both share the zone. Running both clients' reports
made one `togglePause` cancel itself.

When the source that played fails and nothing playable is left (every item
failed, e.g. a repeated single track), `ZoneSession.FailedAsync` stops the zone
and keeps the item current: the core would otherwise wait in `starting` for a
source that never comes, and a play would find the zone already starting.

`CoreLink` connects to the core's JSON-line API, subscribes, reads status and
reconciles `DesiredState` against `CoreModel`. The core can restart without
knowing any previous desired memberships/settings. Model changes happen in
wire order on the reader; event subscribers run through a separate bounded
dispatch queue. Never block the reader on downstream work. Lost events trigger
resynchronization; a stale event for replaced staging must not advance a queue.

Core replies are read strictly into records and enums (`CoreLink.Json`). A value
the server does not know -- e.g. a new MATP input action in a room's bindings --
fails the whole `status` read, and with it every room's live sync, buffer and
system figures (`SystemMonitor` reports `coreUnavailable`). Add new MATP enum
values (`InputAction` in `Core/CoreReplies.cs`) together with the client.

The core's `clients[].active` (default true for older cores) comes from client
discovery, including disabled clients that have not connected since a restart.
`House` hides inactive rooms and zones containing only inactive rooms, while
retaining their desired settings for reactivation. `discovery.clients` omits
inactive clients; an ordinary disconnected room remains visible.

Keep ownership straight when changing settings:

- `.NET DesiredState`: requested memberships, audio control, latency, EQ, room
  levels and zone mutes. A zone has no volume of its own (Raumfeld's model):
  it is its loudest member's, and setting it moves every member's position by
  the same amount, kept unclamped so the balance survives silence; positions
  are linear in dB, so this is the same dB shift. Effective volume clamps the
  position to 0..1 and applies the maximum level. A room is
  grouped exactly while its zone's wanted list names another room; the core
  decides when a room left alone changes plan (next track while playing).
- Client config: identity/name, device, buffer, Volume Control, drift actuator,
  inputs and hardware settings. Commands go through the core; returned client
  reports update the actual view. Remembered names are not replayed as intent.
- Listeners (`Library/UserStore.cs`): each room has one (`Playback/ZoneUsers.cs`,
  kept in `playback.json`); a zone's is its active rooms' shared listener,
  else the household. Plays, book positions and AutoFill's seed use the
  zone's. End-user behaviour: [docs/server/Listeners.md](../server/Listeners.md).
- `<dataDirectory>/playback.json`: queues/positions and desired state, written
  atomically by `PlaybackStore` on changes only; while a queue plays, only its
  place goes to `playback-positions.json` (matched by generation). SQLite
  `library.db`: library and store data (WAL, `synchronous=NORMAL`, one
  connection kept open by `LibraryIndexer` so closes do not checkpoint);
  `core.json`: selected core; `logging.json`: the one log level set through
  `settings/logging` (`Playback/LogSettings.cs`), applied to the server's
  `ServerLoggerProvider` and sent to the core (`log.configure`) on every
  connect. Inspect each store before changing persistence.

## Media, API and feature changes

- Providers return `ResolvedStream`, not decoded PCM. `KnownLength` must
  provide independent seekable readers with stable bytes/length per URL;
  `Live` provides nonseekable readers. Keep stream/cache release lifetimes.
- On-demand streaming providers (today Tidal) resolve through
  `Providers/Streaming/StreamWindow.cs`, one per provider: it holds only a
  window of pieces around each reader (memory or disk, top-level `streaming`
  config), renews expired signed URLs through the provider's manifest
  function, and frees a track on release. New streaming providers must use
  it instead of buffering or caching whole files themselves.
  `Web/Media/MediaHandles.cs` retires handles after the core is done.
- `Web/Media/MediaEndpoint.cs` serves `/media/{handle}` on its own listener:
  Range for seekable media, streaming/backpressure for live media. A paused
  core may hold an unread response open; do not restore Kestrel's minimum
  response-rate timeout. Match its token/origin with core `media` config.
- REST routes are in `Web/Api/ApiEndpoint.*.cs`; DTOs/validation in
  `ApiRequests.cs`, `ApiJson.cs`; SSE in `ApiEvents.cs` and `ApiEndpoint.cs`.
  Start with the suffix matching the feature. Add shared house operations
  where REST and MCP need the same behavior.
- The browser source is root `server-gui/www/`. `Multiroom.Server.csproj`
  copies it to output/publish `gui/`. After GUI or shared web asset changes,
  run `powershell -ExecutionPolicy Bypass -File scripts/copy-server-gui.ps1`
  from the repo root to update the existing Windows x64 release (or pass
  `-Target <target>`). Refresh the browser; no rebuild/restart is needed.
  `Web/Gui/GuiEndpoint.cs` serves it and proxies API/device requests. Its own
  text is by key through `tt()`/`data-i18n`, with English in
  `server-gui/www/app.en.js` and German in `app.de.js` (`shared/web/STYLE.md`,
  Languages); add the key to both when adding text.
- Provider work starts with `Abstractions/Providers.cs` and the nearest
  existing provider; register in `ServerHost.cs`. Library schema changes add
  the next numbered `Library/Migrations/*.sql`, embedded by the library
  project and applied transactionally by `LibraryDatabase.Migrate()`.
- Client updates: `Web/Updates/ClientUpdates.cs` behind
  `GET/POST /api/v1/rooms/{client}/update`, and `GET /api/v1/clients/updates`
  for the clients list (each client asked at most every 10 min, sooner when
  its beacon version changes; 5 s per client, silent ones left out). The server lists GitHub releases
  (cached 1 h) plus packages dropped into `<dataDirectory>/client-updates/`,
  downloads the asset for the client's `target`, checks GitHub's published
  SHA-256, and uploads it to the client's control port (`POST /api/update`,
  found through the core's `discovery.clients`), never through the core.
  Clients never reach the Internet; client side: [client.md](client.md),
  Self update. Version comparison uses the release tag against the client's
  CMake `project(VERSION)`, so bump that for every release. The release list,
  local packages and checked downloads are `Web/Updates/ReleasePackages.cs`,
  shared with the server update.
- Server updates: `Web/Updates/ServerUpdates.cs` behind
  `GET/POST /api/v1/server/update`. Server and core always update together
  from the release's `galdrun-server-bundle-<tag>-<target>` archive (local
  packages in `<dataDirectory>/server-updates/`). The core must be on this
  machine and report `executable`/`restartable` in `status`. The server
  unpacks the bundle into each folder's `.update/staged`, runs both new
  binaries with `--version`, then renames each package entry over the
  installed one (old entries go to `.update/previous`; a bundle folder with
  `start.*` gets its root files too). Next it sends `core.restart` and stops
  normally; `Program.cs`/`SelfRestart.cs` starts the server again.
  Invariant: the restart keeps the process where its supervisor can find it.
  Linux `execv()`s in place, so systemd, the Pi image's PID files and
  `start.sh` keep the same PID. Windows starts a new process on the same
  console; the bundle's `start.ps1` follows the programs by path. A Windows
  service, a core elsewhere or an unwritable folder reports `supported: false`
  with a reason, while `available` still says that a newer bundle exists.
  Renaming running executables and DLLs is allowed on Windows (verified),
  so no external updater script is needed.
- Configuration is JSON -> `GALDRUN_` environment -> CLI. Unknown keys fail;
  add keys to `ServerOptions` and resolve/validate in `ConfigLoader`. Explicit
  `--config` must exist; absent it, loader looks for executable-adjacent
  `appsettings.json`, then defaults. Relative paths resolve against the config.

## Tests

`tests/Multiroom.Server.Tests/` uses xUnit v3. Match the feature name:
`CoreLinkTests`/`CoreDiscoveryTests`, `QueueEngineTests*`, `ApiTests*`,
`MediaEndpointTests`, `ConfigLoaderTests`, `LibraryTests`, provider tests.
Reuse `FakeCore`, `FakeProvider`, temporary directories and port `0`; API tests
compose the real host with these doubles. A control-contract change also needs
the real core's parser/event tests, described in [server-core.md](server-core.md).

SDK comes from `global.json`; target/analyzer policy from `Directory.Build.props`
(.NET 11, nullable, warnings as errors). Keep secrets out of records' generated
`ToString()`, request logs and checked-in sample configs; existing token option
classes and credential stores show the pattern.
