#pragma once

// Persistent server settings (server.json).
//
// It holds what this phase actually uses and nothing else. Listeners, zones,
// resampler selection and the timing constants of MATP section 46 arrive in
// the phases that consume them; a field added ahead of its phase is a field
// nobody validates.
//
// Unknown keys are an error rather than a shrug. A server that silently
// ignores "maxSampleRte" is a server that plays at the wrong ceiling and says
// nothing, and a typo in a deployment's config is exactly the failure this
// rule exists to catch.

#include "audio/AudioFormat.h"
#include "client/OutputPlan.h"
#include "core/Error.h"
#include "matp/ClientBeacon.h"
#include "processing/IClientAudioProcessor.h"
#include "source/MediaOrigin.h"
#include "util/Json.h"
#include "util/Logger.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace multiroom {

// The ceiling the decoded-audio memory is sized from. Every zone decodes into
// blocks of one fixed size, so this is what turns the audio memory into a
// number printed at startup.
struct AudioConfig
{
    uint32_t maxSampleRate = 192000;
    uint16_t maxChannels = 2;

    // MATP section 46's recommended audio packet duration. A duration, never a
    // frame count: 20 ms is 882 frames at 44.1 kHz and 1920 at 96 kHz, and one
    // value covers every room.
    uint32_t packetDurationMs = 20;

    // Phase 42: the ceiling on every zone's decoded audio together. Each zone
    // allocates its own blocks, sized by what it plays and only while it
    // plays -- 1 MB for 44.1 kHz int16 and 7.7 MB at the 192 kHz int32
    // ceiling -- and a zone whose format would go past this is refused by
    // name. It replaced `blockCount`, the size of one pool every zone shared.
    // audio.configure can change it while the core runs.
    uint32_t memoryLimitMb = 128;

    // MATP section 46's normal prebuffer, and the only thing a room's pacing
    // compares against (section 18). A duration for the same reason the packet
    // is one: 1500 ms is 66,150 frames at 44.1 kHz and 144,000 at 96 kHz, and
    // none of those numbers is comparable with another room's.
    uint32_t targetBufferMs = 1500;

    // Section 20's barrier, and section 46's values for it. A duration again,
    // and for the same reason: what every room's answer is compared in.
    //
    // `minimumStartBufferMs` is deliberately below the target -- what a start
    // needs is enough audio that it cannot underrun, not a full buffer, and
    // waiting for the full one would make the slowest network decide when the
    // house begins. `clientReadyTimeoutMs` is how long the first start waits
    // for a room whose device is still opening; one room with a slow driver
    // must never keep the others silent, so a room that misses it is started
    // later instead. `startDelayMs` is how far ahead of now a START_AT is
    // placed, and is also the lead a room joining a playing zone is given.
    uint32_t minimumStartBufferMs = 1000;
    uint32_t clientReadyTimeoutMs = 5000;
    uint32_t startDelayMs = 1000;

    // Phase 26's seek, and the two leads it places its positions with. The cut
    // is `seekFlushLeadMs` past what is audible, which is the time a FLUSH has
    // to reach every room; the seeked audio is heard `seekFillLeadMs` after
    // the cut, which is the time its first packets have to arrive before they
    // are due. Starting values waiting for the milestone's measurement.
    uint32_t seekFlushLeadMs = 100;
    uint32_t seekFillLeadMs = 150;

    // Sections 26 and 31, and section 46's values for them: how long a window
    // this server's own drift regression runs over, and how far the correction
    // it computes from it may reach.
    //
    // The window is a duration for the same reason every other one here is --
    // it is compared against server nanoseconds, never against a frame count
    // that means nothing in another room. The two limits are whole ppm because
    // that is the resolution section 31 states them in; the wider one is for
    // the initial convergence of a room only, and `ClientSyncController.h`
    // says when it stops applying.
    //
    // **Phase 12 measures with all three and corrects with none of them.**
    uint32_t driftWindowMs = 20000;
    uint32_t correctionLimitPpm = 100;
    uint32_t convergenceLimitPpm = 500;

    // The one resampler every resampled room in every zone is converted by
    // (phase 6: never a mix, because two rooms of one house converted by two
    // filters is a difference everybody can hear between them).
    //
    // Empty means "whichever this build has", which is the honest default for
    // a build whose two resampler options are both off: it registers none, and
    // a server with none can still serve every room whose format matches its
    // zone's. A name this build does not have is refused at startup, with the
    // ids that *are* in it named.
    std::string resampler;

    // Every room's default Audio Control, which the control API's
    // client.setAudioControl overrides per room (phase 17). There is no drift
    // setting: correction follows from this, the room's zone and its device.
    AudioControl audioControl = AudioControl::Auto;

    // Phase 31: phase 7's narrowing dither, for the whole house ("off" or
    // "tpdf"). Zone-wide like the resampler, never per room, and reaching only
    // a resampled room below 24 bits. Off until a blind listening test says
    // otherwise, because off is what keeps the bytes predictable.
    DitherMode dither = DitherMode::No;

    // The widest format a block must hold, which is what sizes one.
    AudioFormat largestFormat() const;
};

// The line-based JSON API the high-end server drives (phase 17). It replaced
// the one-zone `zone` section phases 10 to 16 stood in with: zones, sources
// and memberships are the high-end server's to name, and a config file that
// could name them too would be a second answer to "what should be playing".
//
// **Loopback by default, and nothing here widens it.** There is no
// authentication, so the bind address is the whole of the access control, and
// a deployment that exposes the API to its network does so by editing this
// field on purpose.
struct ControlConfig
{
    std::string bindAddress = "127.0.0.1";

    // The next port after section 3's MATP pair. Zero binds whichever port is
    // free and is what the tests use.
    uint16_t port = 4713;
};

// Where every source comes from (phase 20): the high-end server, over HTTP.
// `source.stage` refuses a mediaUri that is not under this base -- same
// scheme, host and port, and a path beneath its path -- so the core never
// opens a file it was named and never fetches from a host it was not told
// about. `source/MediaOrigin.h` says how the comparison is made.
//
// **Loopback by default**, like the control API: the high-end server on this
// machine. A LAN deployment names the other machine here, and one that wants
// TLS names an https base -- an https URI is accepted only under one.
//
// `stallTimeoutMs` and `retries` are phase 22's, and `MediaFetchSettings` in
// the same header says what each one bounds. The timeout has to be shorter
// than the audio a playing room holds ahead of what is audible -- its
// prebuffer target, and the lookahead the zone decodes past it -- or a
// connection that is stuck but still inside its timeout has already been
// heard; validation names both fields.
//
// `token` is phase 23's shared secret with the high-end server, sent as
// `Authorization: Bearer <token>` on every media GET when it is not empty.
// **Empty by default**, because the default base is loopback. A base on
// another machine with no token is warned about at startup and not refused:
// a closed LAN is a legitimate deployment. The token is never logged and
// never reported by `status`; it is only ever in this file.
struct MediaConfig
{
    std::string baseUrl = std::string(MediaOrigin::kDefaultBaseUrl);
    uint32_t stallTimeoutMs = MediaFetchSettings{}.stallTimeoutMs;
    uint32_t retries = MediaFetchSettings{}.retries;
    std::string token;
};

// The two MATP listeners and the sessions on them. Only the fields something
// actually reads: a field added ahead of its phase is a field nobody
// validates.
struct NetworkConfig
{
    // MATP section 3's example pair is 4711 control / 4712 audio, and the
    // client's fallback for an unannounced audio port is "control plus one" --
    // so keeping this default makes that fallback right by construction.
    //
    // Zero binds whichever port the system has free and is for tests, which
    // may not take a fixed one.
    uint16_t controlPort = 4711;

    // The second connection of section 3, announced in PREPARE_STREAM. The
    // default keeps the pair the client's fallback assumes, and the port is
    // announced anyway: a fallback that happens to be right is how a
    // non-default port becomes a silent failure.
    //
    // Zero binds whichever port the system has free, like controlPort, and is
    // what the tests use.
    uint16_t audioPort = 4712;

    // Sockets the listener watches at once, identified or not. Bounded memory
    // is a rule, and a listener is the one place a stranger gets to decide how
    // much of it this process uses.
    uint32_t maxConnections = 32;

    // Liveness (section 46's order of magnitude, and the client's own 20 s
    // silence timeout). The ping is the nudge; the timeout is the decision.
    uint32_t pingIntervalMs = 5000;
    uint32_t silenceTimeoutMs = 20000;

    // The UDP port the clients' discovery beacons arrive on (phase 33),
    // listened to on every interface. The client sends to 4721 and has no
    // setting for it, so another value here only makes sense for a test,
    // which uses zero for whichever port is free. UDP, so it may share a
    // number with the TCP ports above.
    uint16_t discoveryPort = matp::kDiscoveryPort;
};

// The smallest non-zero `logMaxBytes` (phase 29).
inline constexpr uint32_t kMinLogMaxBytes = 64 * 1024;

struct ServerConfig
{
    // What this core announces itself as in its beacon (`matp/CoreBeacon.h`),
    // and what the high-end server stores to find it again at a new address.
    // Empty only in a file no core has started on yet: startup generates one
    // ("galdrun-core-<host>-<8 hex>", discovery::generateNodeId) and writes it
    // back.
    std::string coreId;

    LogLevel logLevel = LogLevel::Info;

    // Empty: stderr only. A relative path is the config file's neighbour, not
    // the working directory's -- which for a service is System32 (phase 29).
    std::string logFile;

    // Phase 29: past this the log file becomes `<logFile>.1` and a new one is
    // started, so the disk holds at most twice it. Zero never rotates.
    uint32_t logMaxBytes = 10 * 1024 * 1024;

    AudioConfig audio;
    NetworkConfig network;
    ControlConfig control;
    MediaConfig media;
};

enum class ConfigError
{
    Missing,     // no file yet; the caller may keep the defaults
    Unreadable,
    Malformed,   // not JSON, or not an object
    UnknownKey,
    WrongType,
    OutOfRange,
    WriteFailed
};

const char* errorName(ConfigError error);

namespace server_config
{

Result<ServerConfig, ConfigError> load(const std::filesystem::path& path);
Status<ConfigError> save(const std::filesystem::path& path, const ServerConfig& config);

std::string serialize(const ServerConfig& config);

// The `audio` section as serialize() writes it, for the control API's
// audio.settings.
json::Value audioToJson(const AudioConfig& audio);

// What one zone playing the ceiling format -- `maxSampleRate`, `maxChannels`
// and the widest sample type -- needs at the default retention, in whole MB
// rounded up: the least `memoryLimitMb` may be (phase 42).
uint32_t ceilingZoneMb(const AudioConfig& audio);

Result<ServerConfig, ConfigError> deserialize(std::string_view text);

// Field-level sanity check, run after loading and before saving.
Status<ConfigError> validate(const ServerConfig& config);

std::filesystem::path defaultPath();

// `logFile` as the server opens it: resolved against the directory of the
// config file it came from when relative, and empty when it is.
std::filesystem::path logFilePath(const ServerConfig& config,
                                  const std::filesystem::path& configPath);

} // namespace server_config
} // namespace multiroom
