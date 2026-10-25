# Server-core implementation map

Paths below are relative to `server-core/` unless stated otherwise. Build/test
commands: [development.md](development.md). Client wire behavior:
[MATP](../protocol/MATP-AudioTransferProtocol.md).

## Entry points and ownership

`src/main.cpp` composes the memory budget, decoder/resampler registries,
planner, rooms, zones, listeners and discovery. Its startup/shutdown comments
are authoritative for lifecycle order. Audio listener starts before MATP
control can advertise it. Fatal listener failure ends the process for the
service manager to restart it.

| Area | Files to inspect first |
| --- | --- |
| CLI/config/defaults | `src/app/CommandLine.cpp`, `include/config/ServerConfig.h`, `src/config/ServerConfig.cpp` |
| .NET control commands and status | `include/control/ControlApi.h`, `src/control/ControlApi.cpp` |
| JSON-line transport/events | `include/control/ControlListener.h`, `src/control/ControlListener.cpp` |
| Room desired settings and last reported facts | `include/control/RoomDirectory.h`, `src/control/RoomDirectory.cpp` |
| Client discovery/attach/device control | `src/discovery/DiscoveryListener.cpp`, `DiscoveredClients.cpp`, `ClientControl.cpp` |
| MATP control listener/handshake | `src/matp/ControlServer.cpp`, `src/client/ClientSession.cpp`, `ClientRegistry.cpp` |
| Audio socket attachment and writes | `src/matp/AudioServer.cpp`, `SocketPacketSink.cpp` |
| Zone membership and lifetime | `src/zone/ZoneManager.cpp` |
| Decode pipeline, source transitions, seek/rejoin | `include/zone/Zone.h`, `src/zone/Zone.cpp` |
| Start barrier, common playhead, late join | `include/zone/StartScheduler.h`, `src/zone/StartScheduler.cpp` |
| Immutable PCM blocks, memory budget, reader eviction | `include/audio/BlockPool.h`, `NativePcmStore.h`, implementations in `src/audio/` |
| Source frames <-> timeline | `include/core/Units.h`, `src/timeline/SourceTimeline.cpp` |
| Per-room plan/format and why it was selected | `src/client/OutputPlanner.cpp`, `FormatMatch.cpp`, `include/client/OutputPlan.h` |
| Per-room pacing/conversion/queue | `include/client/ClientOutput.h`, `src/client/ClientOutput.cpp` |
| Frame totals/packet grid and serialization | `src/client/OutputClock.cpp`, `OutputPacketizer.cpp`; wire layout in root `shared/matp/` |
| Device drift/offset versus server time | `src/client/ClientClockModel.cpp`, `ClientSyncController.cpp` |
| PCM conversion/EQ and resamplers | `src/processing/`, `src/resampler/`, `include/resampler/` |
| Decode/media access/URL restrictions | `src/decode/ffmpeg/FfmpegDecoder.cpp`, `src/source/MediaOrigin.cpp`, `src/source/http/HttpMediaSource.cpp` |

```text
staged SourceSegment -> IMediaSource -> IDecoder -> BlockPool -> NativePcmStore
                                                SourceTimeline      |
                                                     one cursor per room
                                                                    |
ClientOutput -> processor -> OutputClock/OutputPacketizer -> bounded queue -> IPacketSink
```

## Invariants that affect changes

- Decode once per zone; rooms share immutable source-native PCM, with their
  own cursors, formats, processors and sockets. Slow readers are evicted and
  rejoined, never allowed to stall another room or grow buffers without bound.
- Decode demand follows the furthest-ahead streaming room plus lookahead.
  Output pacing follows `bufferedDurationNs` plus frames produced since its
  report; wall-clock-paced sending breaks pause/backpressure.
- `TimelineNs`, `SourceFrame`, `OutputFrame` are different axes (`core/Units.h`).
  Preserve absolute-origin/grid arithmetic and defined rounding. Packet frame
  count and timeline duration are independent when correction is active.
- `OutputPlanner.cpp` decides format and correction actuator. Plan again
  against the format the client actually opened. Room incompatibility fails
  that room, not the whole zone. Inspect the implementation when changing the
  policy: older header/design tables omit later EQ/hardware-clock cases.
- One planner (one resampler) serves the whole server, held in a
  `SharedPlanner` (`include/client/OutputPlanner.h`). Zones take one
  snapshot per planning decision. `resampler.configure` persists
  `audio.resampler`, swaps the planner while zones play, and re-negotiates
  every zoned room whose plan changes (`Zone::planOutdated`); untouched
  bitperfect rooms keep playing. A fixed-ratio resampler (`r8brain`) corrects
  no drift except through a client's own clock trim.
- BitPerfect forbids sample-changing conversion. Verified client clock trim
  can preserve samples while correcting a grouped room; otherwise correction
  uses a resampled path where permitted. EQ runs on the resampled path.
- The zone timeline continues across tracks and membership changes. A format
  boundary is handled per room, with renegotiation only where its device
  needs reopening. Same-format gapless playback must keep flowing.
- Grouping a room (`client.setGrouped true`) replans it on the stream it has
  (`Zone::replanInPlace`). A room left alone gets its solo plan
  (`Zone::replanAlone`): at once while the line is not playing, otherwise at
  its next track boundary, through the same converter switch or device reopen
  as a format change. Status reports the wait as `output.replanPending`;
  `zone.pause`/`zone.stop` apply it.
- Rejoin is one coordinated move of cursor, packet grid, output clock and
  converter followed by scheduling. Keep packet sequence accounting correct
  when dropping queued packets. Losing an audio socket differs from losing
  the MATP control session.

## Thread boundaries and the .NET contract

MATP control, audio attach, .NET control, and discovery have their own listener
threads. Zones have pipeline threads; each room has an output thread.
`ControlServer::send` carries room commands through the listener mailbox;
session state belongs to that listener. `RoomDirectory` holds copied facts.
Never join an output/pipeline thread while holding a mutex it may need.
Tests can drive `pump()` with `ManualClock`; do not also run the worker thread.

Port `4713` is newline-delimited JSON: `{command, params, id}` requests,
`{ok, command, id, ...}` replies, and `{event, ...}` events. Unknown request
fields are rejected. `zone.setClients` replaces the complete membership.
Subscribers with overflowing event queues receive `events.lost` and must read
`status` again. The control API is unauthenticated and defaults to loopback.

Discovery's optional `active` flag defaults to true. Disabled clients keep
announcing `active: false`; `discovery.clients` hides them, while internal
`clients`/`status` reports their flag so .NET can hide remembered rooms.
`DiscoveryListener` publishes changes through `ControlApi::clientActiveChanged`
as `room.active`; see the shared beacon definition and protocol wire reference.

For contract changes, update the .NET side under
`server/src/Multiroom.Server.Playback/Core/`: `CoreCommands.cs`,
`CoreReplies.cs`, `CoreEvents.cs`, `CoreModel.cs`, `CoreLink.cs`, and
`DesiredState.cs` as applicable. Its `FakeCore` is a test double, so passing
.NET tests alone does not prove agreement with the real C++ parser.

## Tests and build seams

Use `tests/test_control_api.cpp` and `test_control_server.cpp` for the two
control contracts; `test_audio_server.cpp` for attachment;
`test_output_planner.cpp`, `test_processor.cpp`, `test_real_resamplers.cpp` for
audio plans; `test_zone.cpp`, `test_start_scheduler.cpp`, `test_client_sync.cpp`
for timing; `test_matp_roundtrip.cpp` / `test_integration.cpp` for joined paths;
`test_media_fetch.cpp` for HTTP behavior. Helpers include `ManualClock.h`,
`AudioFixtures.*`, `HttpTestServer.*` and `TestSupport.*`.

Source/test lists are explicit in `CMakeLists.txt` and `tests/CMakeLists.txt`.
Presets enable FFmpeg, soxr and r8brain; a bare configure defaults the resamplers
off. `MULTIROOM_WITH_FFMPEG=OFF` is a reduced build that cannot validate actual
decoding/HTTP playback. Keep `shared/` C++17 despite this component's C++23.
