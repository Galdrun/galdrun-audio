# Notes for AI agents

Use this table to find the relevant code without scanning the whole repository.
Read only the guides for the task, then the named source files and tests. Paths
and commands are case-sensitive on Linux. Existing `Multiroom`/`multiroom`
identifiers remain in code; the product and executable names are Galdrun.

Repository build, local start/stop, setup, and device upload scripts live under
`scripts/` (build scripts have moved out of the repository root). Their purposes
and commands are listed in [docs/agents/development.md](docs/agents/development.md).

| Working on | Read first |
| --- | --- |
| New session, component ownership, ports, or a change crossing components | [docs/agents/overview.md](docs/agents/overview.md) |
| Client playback, device backends, settings, buttons, or ESP32 | [docs/agents/client.md](docs/agents/client.md) |
| Core decoding, zones, scheduling, streaming, resampling, or synchronization | [docs/agents/server-core.md](docs/agents/server-core.md) |
| .NET server, queues, library, providers, REST, MCP, AI, or server GUI integration | [docs/agents/server.md](docs/agents/server.md) |
| Building, testing, running locally, or a locked executable/DLL | [docs/agents/development.md](docs/agents/development.md) |
| The wire protocol between core and clients (`shared/matp/`, `client/src/matp/`, `server-core/src/matp/`, `server-core/src/client/`) | [docs/protocol/MATP-AudioTransferProtocol.md](docs/protocol/MATP-AudioTransferProtocol.md) |
| Porting the client firmware to a Raumfeld device (`hardware/buildroot/`) | [docs/hardware/raumfeld/connector2.md](docs/hardware/raumfeld/connector2.md) |

## Local processes and builds

Stop running local Galdrun servers, cores, and clients before building. On
Windows, running executables and loaded DLLs are locked: linking, copying, or
publishing over them will fail. C++ builds also export to `releases/`, so a
running release can break a development build.

**Agents are explicitly allowed to close running Galdrun servers, cores, and
clients on this machine without asking for confirmation.** Prefer a graceful
stop so the server saves playback state. Start with
`powershell -ExecutionPolicy Bypass -File scripts/stop-local.ps1` from the repo
root; it only matches this checkout's release executables. For other local
instances, identify the Galdrun executable/service or `dotnet` command line
before stopping it. See the development guide for scope and restart commands.

## Change rules

The protocol's message numbers, field names and packet layout exist in one copy,
`shared/matp/`, used by both the client and the core. Change them there, and
update the protocol document in the same change.

- Keep `shared/` compatible with C++17; the core itself uses C++23.
- The .NET-to-core control API is a separate line-based JSON contract. Update
  both `server-core/src/control/ControlApi.cpp` and the matching records/parsers
  in `server/src/Multiroom.Server.Playback/Core/` when changing it.
- Inspect `git status` before editing; preserve existing staged and unstaged work.
  Prefer the smallest correct change and existing platform APIs/dependencies.
- After changing `server-gui/www/` or its shared assets (`shared/web/theme.css`,
  `shared/web/usage-charts.js`), run
  `powershell -ExecutionPolicy Bypass -File scripts/copy-server-gui.ps1`
  before browser verification. This updates the Windows x64 server release;
  use `-Target <target>` for another release. Refresh the browser; GUI-only
  changes do not require a server rebuild or restart.
- Verify with the relevant commands in the development guide. Report what ran
  and any limitations; do not treat an old test binary as verification of new code.
- Keep the matching agent guide current when moving entry points or changing
  ownership, build commands, or cross-component behavior. Link to authoritative
  code/protocol definitions instead of copying large inventories into these docs.
