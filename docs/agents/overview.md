# Project map for agents

Use [development.md](development.md) for commands, then one component guide:
[client](client.md), [server-core](server-core.md), [server](server.md).
These are navigation maps of the implementation, not a second protocol spec.

## Ownership

| Component | Owns | Start reading |
| --- | --- | --- |
| `server/` (.NET) | Music catalog/providers, queues, persistent desired state, users, REST/MCP/AI | `server/src/Multiroom.Server/ServerHost.cs` |
| `server-core/` (C++23) | Decode once per zone, media timeline, per-room format/DSP/sync/output | `server-core/src/main.cpp`, `include/zone/Zone.h` |
| `client/` (C++17) | Room identity/settings, device capabilities, PCM buffering/rendering, hardware clock measurements | `client/src/main.cpp`, `include/app/StreamPlayback.h` |
| `shared/` (C++17 and assets) | MATP, audio format/clock types, JSON, time, discovery, system sampling, branding/theme | `shared/matp/include/matp/Protocol.h` |

An audio **room** is a client keyed by stable `clientId`; a TCP connection or
MATP `sessionId` is temporary. A **zone** has one queue in .NET and one shared
media timeline in the core, with a separate output stream for each room.
The .NET `ZoneKeeper` maintains one zone per room initially, moves memberships,
and removes zones left empty. The core itself exposes lower-level zone commands.

## Connections (default ports)

| Initiator -> listener | Transport | Purpose |
| --- | --- | --- |
| .NET server -> core `4713` | TCP, newline-delimited JSON | Commands/replies and subscribed events; **not MATP** |
| Core -> .NET server `4714` | HTTP `/media/{handle}` | Fetch original media bytes for FFmpeg |
| Client -> core `4711` | TCP, framed MATP control | Handshake, scheduling, telemetry, room controls |
| Client -> core `4712` | TCP | `STREAM_ATTACH`, then binary PCM packets from core |
| Client -> core `4721` | UDP broadcast | Client discovery beacons |
| Core -> .NET server `4722` | UDP broadcast | Core discovery beacons |
| Browser -> .NET server `4720` | HTTP | GUI; proxies API requests |
| API/MCP caller -> .NET server `4725` | HTTP | REST `/api/v1`, events, MCP |
| Browser/core -> client `4720` | HTTP or line-based TCP JSON | Device settings/control, separate from MATP |

Server GUI and client settings both default to `4720`; when colocated, use a
different client `controlPort`. `scripts/start-local.ps1` uses `4723` for it.
Core control and .NET listeners default to loopback. Follow the actual config
before diagnosing reachability; token settings for API and media are separate.

## Playback path

1. REST/MCP or room input reaches the .NET playback services. `QueueEngine` /
   `ZoneSession` resolve a queue entry through `ProviderRegistry`.
2. A provider returns a `ResolvedStream` of original media. `MediaEndpoint`
   publishes a handle; `source.stage` sends its URL and a unique staging
   `sourceId` to the core through `CoreLink`.
3. The core's `Zone` opens/decodes it into `NativePcmStore`; `SourceTimeline`
   maps source frames to the zone's nanosecond timeline.
4. Each `ClientOutput` reads that decode with its own cursor and plan, converts
   only as required, packetizes, and sends on that room's audio connection.
5. Client `AudioStream` fills `AudioRingBuffer`. `StreamPlayback` schedules
   `TimelineSource` on the device frame axis after `START_AT`; arrival time
   never decides when audio becomes audible.
6. Client telemetry feeds core synchronization. Core events drive staging and
   audible now-playing state back in .NET.

## Find the right layer

| Symptom/change | First files or guide |
| --- | --- |
| Queue order, next/previous, chapters, repeat/shuffle, resume | [server.md](server.md): `ZoneSession.cs`, `QueueEngine.cs`, `PlaybackStore.cs` |
| File/radio/service resolution or HTTP Range behavior | [server.md](server.md): provider, `ResolvedStream.cs`, `MediaEndpoint.cs`; [server-core.md](server-core.md): `HttpMediaSource.cpp` |
| Wrong room format, unexpected resampling/EQ | [server-core.md](server-core.md): `OutputPlanner.cpp`, `OutputPlan.h`; .NET `DesiredState.cs` for grouping |
| Silence, late start, drift, repeated rejoins | Core `StartScheduler`, `ClientSyncController`, `ClientOutput`; client `StreamPlayback`, `TimelineSource`, backend snapshots |
| Room settings change does not stick | Check its owner in [server.md](server.md); client `ClientCommandHandler.cpp` / `ClientConfig.cpp` |
| Discovery/attach | Shared `ClientBeacon.h` / `CoreBeacon.h`; core `discovery/`; .NET `CoreDiscovery.cs` |
| Add a room command end to end | .NET endpoint/`House` -> `CoreCommands` -> core `ControlApi`/`RoomCommand` -> shared MATP -> client `SessionHooks`/handler |
| Browser UI | `server-gui/www/` for server; client `src/control/` and ESP32 `main/WebUi.cpp` for device UI; `shared/web/STYLE.md` for shared theme |

For MATP, read the relevant sections of
[the protocol](../protocol/MATP-AudioTransferProtocol.md): 12 negotiation,
14-16 packets/framing, 18-25 buffering/start/telemetry, 33 axes, 37-38
pause/stop, 43-44 reconnect/join, 54 seek/flush, 56-61 room controls,
62 concrete wire reference. Do not infer wire details from older design prose.

Use scoped searches such as `rg -n 'sourceNeeded' server-core/src server/src/Multiroom.Server.Playback`
or `rg --files client/src client/include client/tests`. Exclude build outputs,
`third_party`, `bin`, `obj`, and `releases` when searching implementation.
Root `BugFindings*.md`, `TODOS.md`, and `client/docs/Info.md` are background;
check the current implementation/tests before treating a finding as open.
