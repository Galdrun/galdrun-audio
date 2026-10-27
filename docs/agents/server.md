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
| `Multiroom.Server.Providers` | `ProviderRegistry.cs`, `UnifiedSearch.cs`, `SongResolver.cs`; `Local/`, `WebRadio/`, `TuneIn/`, `Tidal/`, `Covers/`, `AddIns/` |
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

`ZoneSession` serializes operations and core events with its own gate. It
resolves/publishes/stages one source ahead; `zone.sourceNeeded` continues that
loop. A `sourceId` identifies a staging, not a track; reused IDs are treated as
replays by the core. Audible `sourceStarted` events and playhead state differ
from the source currently being decoded. Groups contain parts; chapters of
one file may share one staged source and use seek for navigation.

`CoreLink` connects to the core's JSON-line API, subscribes, reads status and
reconciles `DesiredState` against `CoreModel`. The core can restart without
knowing any previous desired memberships/settings. Model changes happen in
wire order on the reader; event subscribers run through a separate bounded
dispatch queue. Never block the reader on downstream work. Lost events trigger
resynchronization; a stale event for replaced staging must not advance a queue.

The core's `clients[].active` (default true for older cores) comes from client
discovery, including disabled clients that have not connected since a restart.
`House` hides inactive rooms and zones containing only inactive rooms, while
retaining their desired settings for reactivation. `discovery.clients` omits
inactive clients; an ordinary disconnected room remains visible.

Keep ownership straight when changing settings:

- `.NET DesiredState`: requested memberships, audio control, latency, EQ, room
  levels and zone levels. Effective volume incorporates zone and maximum level
  in decibels; preserve the room's own slider value separately. A room is
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
