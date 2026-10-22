#include "config/ServerConfig.h"

#include "audio/BlockPool.h"
#include "audio/NativePcmStore.h"
#include "discovery/NodeId.h"
#include "util/FileUtil.h"
#include "util/Json.h"

#include <array>
#include <limits>
#include <string>

namespace multiroom {

const char* errorName(ConfigError error)
{
    switch (error)
    {
    case ConfigError::Missing:
        return "Missing";
    case ConfigError::Unreadable:
        return "Unreadable";
    case ConfigError::Malformed:
        return "Malformed";
    case ConfigError::UnknownKey:
        return "UnknownKey";
    case ConfigError::WrongType:
        return "WrongType";
    case ConfigError::OutOfRange:
        return "OutOfRange";
    case ConfigError::WriteFailed:
        return "WriteFailed";
    }
    return "?";
}

AudioFormat AudioConfig::largestFormat() const
{
    AudioFormat format;
    format.sampleRate = maxSampleRate;
    format.channels = maxChannels;
    format.sampleType = kWidestSampleType;
    return format;
}

namespace server_config {
namespace {

// Serialization is explicit field by field, both directions. A missing key
// keeps the default, a present key of the wrong type is an error, and a key
// nobody recognises is an error. Nothing here is driven by reflection or a
// table, because the validation is per field anyway.

constexpr std::array<std::string_view, 8> kRootKeys{"coreId",  "logLevel", "logFile", "logMaxBytes",
                                                    "audio",   "network",  "control", "media"};
constexpr std::array<std::string_view, 17> kAudioKeys{
    "maxSampleRate",      "maxChannels",          "packetDurationMs",    "memoryLimitMb",
    "targetBufferMs",     "minimumStartBufferMs", "clientReadyTimeoutMs", "startDelayMs",
    "seekFlushLeadMs",    "seekFillLeadMs",
    "driftWindowMs",      "correctionLimitPpm",   "convergenceLimitPpm",  "resampler",
    "audioControl",       "driftCorrection",      "dither"};
constexpr std::array<std::string_view, 6> kNetworkKeys{
    "controlPort",    "audioPort",        "maxConnections",
    "pingIntervalMs", "silenceTimeoutMs", "discoveryPort"};
constexpr std::array<std::string_view, 2> kControlKeys{"bindAddress", "port"};
constexpr std::array<std::string_view, 4> kMediaKeys{"baseUrl", "stallTimeoutMs", "retries",
                                                     "token"};

template <size_t N>
Status<ConfigError> rejectUnknownKeys(const json::Value& object,
                                      const std::array<std::string_view, N>& known,
                                      std::string_view prefix)
{
    for (const auto& member : object.members())
    {
        bool recognised = false;
        for (const std::string_view key : known)
        {
            if (member.first == key)
            {
                recognised = true;
                break;
            }
        }
        if (!recognised)
            return fail(ConfigError::UnknownKey, std::string(prefix) + member.first);
    }
    return {};
}

// A missing key and a key of the wrong type must not look alike, so presence
// is tested first: a blockCount given as a string has to be an error rather
// than a silent default.
Status<ConfigError> readUint(const json::Value& object, const char* key, std::string_view prefix,
                             uint64_t maximum, uint64_t& out)
{
    const json::Value* member = object.find(key);
    if (member == nullptr)
        return {};

    const std::string where = std::string(prefix) + key;
    if (!member->isNumber() || !member->isIntegral())
        return fail(ConfigError::WrongType, where + " must be a whole number");

    const int64_t value = member->asInt();
    if (value < 0 || static_cast<uint64_t>(value) > maximum)
    {
        return fail(ConfigError::OutOfRange, where + " is " + std::to_string(value) +
                                                 ", expected 0.." + std::to_string(maximum));
    }

    out = static_cast<uint64_t>(value);
    return {};
}

Status<ConfigError> readString(const json::Value& object, const char* key,
                               std::string_view prefix, std::string& out)
{
    const json::Value* member = object.find(key);
    if (member == nullptr)
        return {};
    if (!member->isString())
        return fail(ConfigError::WrongType, std::string(prefix) + key + " must be a string");
    out = member->asString();
    return {};
}

Status<ConfigError> readBool(const json::Value& object, const char* key,
                             std::string_view prefix, bool& out)
{
    const json::Value* member = object.find(key);
    if (member == nullptr)
        return {};
    if (!member->isBool())
        return fail(ConfigError::WrongType, std::string(prefix) + key + " must be true or false");
    out = member->asBool();
    return {};
}

Status<ConfigError> readAudio(const json::Value& audio, AudioConfig& out)
{
    // Phase 42 took the shared pool away, and its size with it. An old config
    // is told what replaced it rather than having the key ignored.
    if (audio.find("blockCount") != nullptr)
        return fail(ConfigError::UnknownKey,
                    "audio.blockCount: each zone sizes its own memory now; set "
                    "audio.memoryLimitMb instead");
    if (auto known = rejectUnknownKeys(audio, kAudioKeys, "audio."); !known)
        return known;

    // The bounds here are only what the field can hold; validate() owns the
    // question of whether a representable value is a sensible one, because
    // that answer depends on all four fields at once.
    uint64_t value = out.maxSampleRate;
    if (auto taken = readUint(audio, "maxSampleRate", "audio.",
                              std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.maxSampleRate = static_cast<uint32_t>(value);

    value = out.maxChannels;
    if (auto taken = readUint(audio, "maxChannels", "audio.",
                              std::numeric_limits<uint16_t>::max(), value);
        !taken)
        return taken;
    out.maxChannels = static_cast<uint16_t>(value);

    value = out.packetDurationMs;
    if (auto taken = readUint(audio, "packetDurationMs", "audio.",
                              std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.packetDurationMs = static_cast<uint32_t>(value);

    value = out.memoryLimitMb;
    if (auto taken = readUint(audio, "memoryLimitMb", "audio.",
                              std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.memoryLimitMb = static_cast<uint32_t>(value);

    value = out.targetBufferMs;
    if (auto taken = readUint(audio, "targetBufferMs", "audio.",
                              std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.targetBufferMs = static_cast<uint32_t>(value);

    value = out.minimumStartBufferMs;
    if (auto taken = readUint(audio, "minimumStartBufferMs", "audio.",
                              std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.minimumStartBufferMs = static_cast<uint32_t>(value);

    value = out.clientReadyTimeoutMs;
    if (auto taken = readUint(audio, "clientReadyTimeoutMs", "audio.",
                              std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.clientReadyTimeoutMs = static_cast<uint32_t>(value);

    value = out.startDelayMs;
    if (auto taken = readUint(audio, "startDelayMs", "audio.",
                              std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.startDelayMs = static_cast<uint32_t>(value);

    value = out.seekFlushLeadMs;
    if (auto taken = readUint(audio, "seekFlushLeadMs", "audio.",
                              std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.seekFlushLeadMs = static_cast<uint32_t>(value);

    value = out.seekFillLeadMs;
    if (auto taken = readUint(audio, "seekFillLeadMs", "audio.",
                              std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.seekFillLeadMs = static_cast<uint32_t>(value);

    value = out.driftWindowMs;
    if (auto taken = readUint(audio, "driftWindowMs", "audio.",
                              std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.driftWindowMs = static_cast<uint32_t>(value);

    value = out.correctionLimitPpm;
    if (auto taken = readUint(audio, "correctionLimitPpm", "audio.",
                              std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.correctionLimitPpm = static_cast<uint32_t>(value);

    value = out.convergenceLimitPpm;
    if (auto taken = readUint(audio, "convergenceLimitPpm", "audio.",
                              std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.convergenceLimitPpm = static_cast<uint32_t>(value);

    if (auto taken = readString(audio, "resampler", "audio.", out.resampler); !taken)
        return taken;

    if (const json::Value* mode = audio.find("audioControl"); mode != nullptr)
    {
        if (!mode->isString())
            return fail(ConfigError::WrongType, "audio.audioControl must be a string");
        if (!parseAudioControl(mode->asString(), out.audioControl))
        {
            return fail(ConfigError::OutOfRange,
                        "audio.audioControl is " + mode->asString() +
                            ", expected bitPerfect|auto");
        }
    }

    // driftCorrection is still a known key so that a file written while
    // there was a drift setting loads; it decides nothing
    //.

    if (const json::Value* mode = audio.find("dither"); mode != nullptr)
    {
        if (!mode->isString())
            return fail(ConfigError::WrongType, "audio.dither must be a string");
        if (!parseDitherMode(mode->asString(), out.dither))
        {
            return fail(ConfigError::OutOfRange,
                        "audio.dither is " + mode->asString() + ", expected off|tpdf");
        }
    }

    return {};
}

Status<ConfigError> readControl(const json::Value& control, ControlConfig& out)
{
    if (auto known = rejectUnknownKeys(control, kControlKeys, "control."); !known)
        return known;

    if (auto taken = readString(control, "bindAddress", "control.", out.bindAddress); !taken)
        return taken;

    uint64_t value = out.port;
    if (auto taken = readUint(control, "port", "control.", std::numeric_limits<uint16_t>::max(),
                              value);
        !taken)
        return taken;
    out.port = static_cast<uint16_t>(value);

    return {};
}

Status<ConfigError> readMedia(const json::Value& media, MediaConfig& out)
{
    if (auto known = rejectUnknownKeys(media, kMediaKeys, "media."); !known)
        return known;
    if (auto taken = readString(media, "baseUrl", "media.", out.baseUrl); !taken)
        return taken;

    uint64_t value = out.stallTimeoutMs;
    if (auto taken = readUint(media, "stallTimeoutMs", "media.",
                              std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.stallTimeoutMs = static_cast<uint32_t>(value);

    value = out.retries;
    if (auto taken =
            readUint(media, "retries", "media.", std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.retries = static_cast<uint32_t>(value);

    if (auto taken = readString(media, "token", "media.", out.token); !taken)
        return taken;

    return {};
}

Status<ConfigError> readNetwork(const json::Value& network, NetworkConfig& out)
{
    if (auto known = rejectUnknownKeys(network, kNetworkKeys, "network."); !known)
        return known;

    uint64_t value = out.controlPort;
    if (auto taken = readUint(network, "controlPort", "network.",
                              std::numeric_limits<uint16_t>::max(), value);
        !taken)
        return taken;
    out.controlPort = static_cast<uint16_t>(value);

    value = out.audioPort;
    if (auto taken = readUint(network, "audioPort", "network.",
                              std::numeric_limits<uint16_t>::max(), value);
        !taken)
        return taken;
    out.audioPort = static_cast<uint16_t>(value);

    value = out.maxConnections;
    if (auto taken = readUint(network, "maxConnections", "network.",
                              std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.maxConnections = static_cast<uint32_t>(value);

    value = out.pingIntervalMs;
    if (auto taken = readUint(network, "pingIntervalMs", "network.",
                              std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.pingIntervalMs = static_cast<uint32_t>(value);

    value = out.silenceTimeoutMs;
    if (auto taken = readUint(network, "silenceTimeoutMs", "network.",
                              std::numeric_limits<uint32_t>::max(), value);
        !taken)
        return taken;
    out.silenceTimeoutMs = static_cast<uint32_t>(value);

    value = out.discoveryPort;
    if (auto taken = readUint(network, "discoveryPort", "network.",
                              std::numeric_limits<uint16_t>::max(), value);
        !taken)
        return taken;
    out.discoveryPort = static_cast<uint16_t>(value);

    return {};
}

} // namespace

uint32_t ceilingZoneMb(const AudioConfig& audio)
{
    const size_t blockBytes =
        blockBytesFor(audio.maxSampleRate, audio.maxChannels, audio.packetDurationMs);
    const uint64_t bytes = static_cast<uint64_t>(blocksFor(blockBytes, audio.largestFormat(),
                                                           NativePcmStore::kDefaultRetentionNs)) *
                           blockBytes;
    constexpr uint64_t kMb = 1024ULL * 1024ULL;
    return static_cast<uint32_t>((bytes + kMb - 1) / kMb);
}

json::Value audioToJson(const AudioConfig& config)
{
    json::Value audio = json::Value::makeObject();
    audio["maxSampleRate"] = json::Value(config.maxSampleRate);
    audio["maxChannels"] = json::Value(static_cast<uint32_t>(config.maxChannels));
    audio["packetDurationMs"] = json::Value(config.packetDurationMs);
    audio["memoryLimitMb"] = json::Value(config.memoryLimitMb);
    audio["targetBufferMs"] = json::Value(config.targetBufferMs);
    audio["minimumStartBufferMs"] = json::Value(config.minimumStartBufferMs);
    audio["clientReadyTimeoutMs"] = json::Value(config.clientReadyTimeoutMs);
    audio["startDelayMs"] = json::Value(config.startDelayMs);
    audio["seekFlushLeadMs"] = json::Value(config.seekFlushLeadMs);
    audio["seekFillLeadMs"] = json::Value(config.seekFillLeadMs);
    audio["driftWindowMs"] = json::Value(config.driftWindowMs);
    audio["correctionLimitPpm"] = json::Value(config.correctionLimitPpm);
    audio["convergenceLimitPpm"] = json::Value(config.convergenceLimitPpm);
    audio["resampler"] = json::Value(config.resampler);
    audio["audioControl"] = json::Value(audioControlName(config.audioControl));
    audio["dither"] = json::Value(ditherModeName(config.dither));
    return audio;
}

std::string serialize(const ServerConfig& config)
{
    const json::Value audio = audioToJson(config.audio);

    json::Value control = json::Value::makeObject();
    control["bindAddress"] = json::Value(config.control.bindAddress);
    control["port"] = json::Value(static_cast<uint32_t>(config.control.port));

    json::Value media = json::Value::makeObject();
    media["baseUrl"] = json::Value(config.media.baseUrl);
    media["stallTimeoutMs"] = json::Value(config.media.stallTimeoutMs);
    media["retries"] = json::Value(config.media.retries);
    media["token"] = json::Value(config.media.token);

    json::Value network = json::Value::makeObject();
    network["controlPort"] = json::Value(static_cast<uint32_t>(config.network.controlPort));
    network["audioPort"] = json::Value(static_cast<uint32_t>(config.network.audioPort));
    network["maxConnections"] = json::Value(config.network.maxConnections);
    network["pingIntervalMs"] = json::Value(config.network.pingIntervalMs);
    network["silenceTimeoutMs"] = json::Value(config.network.silenceTimeoutMs);
    network["discoveryPort"] = json::Value(static_cast<uint32_t>(config.network.discoveryPort));

    json::Value root = json::Value::makeObject();
    root["coreId"] = json::Value(config.coreId);
    root["logLevel"] = json::Value(log::levelName(config.logLevel));
    root["logFile"] = json::Value(config.logFile);
    root["logMaxBytes"] = json::Value(config.logMaxBytes);
    root["audio"] = std::move(audio);
    root["network"] = std::move(network);
    root["control"] = std::move(control);
    root["media"] = std::move(media);

    return json::serialize(root, true);
}

Result<ServerConfig, ConfigError> deserialize(std::string_view text)
{
    json::Value root;
    std::string parseError;
    if (!json::parse(std::string(text), root, parseError))
        return fail(ConfigError::Malformed, parseError);
    if (!root.isObject())
        return fail(ConfigError::Malformed, "the document is not a JSON object");

    if (auto known = rejectUnknownKeys(root, kRootKeys, ""); !known)
        return std::unexpected(known.error());

    ServerConfig config;

    if (const json::Value* level = root.find("logLevel"); level != nullptr)
    {
        if (!level->isString())
            return fail(ConfigError::WrongType, "logLevel must be a string");
        if (!log::parseLevel(level->asString(), config.logLevel))
        {
            return fail(ConfigError::OutOfRange,
                        "logLevel is " + level->asString() +
                            ", expected error|warn|info|debug|trace");
        }
    }

    if (auto taken = readString(root, "coreId", "", config.coreId); !taken)
        return std::unexpected(taken.error());

    if (auto taken = readString(root, "logFile", "", config.logFile); !taken)
        return std::unexpected(taken.error());

    uint64_t logMaxBytes = config.logMaxBytes;
    if (auto taken =
            readUint(root, "logMaxBytes", "", std::numeric_limits<uint32_t>::max(), logMaxBytes);
        !taken)
        return std::unexpected(taken.error());
    config.logMaxBytes = static_cast<uint32_t>(logMaxBytes);

    if (const json::Value* audio = root.find("audio"); audio != nullptr)
    {
        if (!audio->isObject())
            return fail(ConfigError::WrongType, "audio must be an object");
        if (auto taken = readAudio(*audio, config.audio); !taken)
            return std::unexpected(taken.error());
    }

    if (const json::Value* network = root.find("network"); network != nullptr)
    {
        if (!network->isObject())
            return fail(ConfigError::WrongType, "network must be an object");
        if (auto taken = readNetwork(*network, config.network); !taken)
            return std::unexpected(taken.error());
    }

    if (const json::Value* control = root.find("control"); control != nullptr)
    {
        if (!control->isObject())
            return fail(ConfigError::WrongType, "control must be an object");
        if (auto taken = readControl(*control, config.control); !taken)
            return std::unexpected(taken.error());
    }

    if (const json::Value* media = root.find("media"); media != nullptr)
    {
        if (!media->isObject())
            return fail(ConfigError::WrongType, "media must be an object");
        if (auto taken = readMedia(*media, config.media); !taken)
            return std::unexpected(taken.error());
    }

    if (auto valid = validate(config); !valid)
        return std::unexpected(valid.error());

    return config;
}

Status<ConfigError> validate(const ServerConfig& config)
{
    if (!config.coreId.empty() && !discovery::isValidNodeId(config.coreId))
        return fail(ConfigError::OutOfRange,
                    "coreId must be 1 to 64 bytes without control characters or surrounding blanks");

    // Below 64 KiB a rotation every few lines would leave too little of the
    // log behind to diagnose anything with.
    if (config.logMaxBytes != 0 && config.logMaxBytes < kMinLogMaxBytes)
    {
        return fail(ConfigError::OutOfRange,
                    "logMaxBytes is " + std::to_string(config.logMaxBytes) + ", expected 0 or " +
                        std::to_string(kMinLogMaxBytes) + " and more");
    }

    // 8 kHz to 768 kHz spans every rate a sound card offers. The point of the
    // bound is not the rate itself but the block size derived from it.
    if (config.audio.maxSampleRate < 8000 || config.audio.maxSampleRate > 768000)
    {
        return fail(ConfigError::OutOfRange,
                    "audio.maxSampleRate is " + std::to_string(config.audio.maxSampleRate) +
                        ", expected 8000..768000");
    }
    if (config.audio.maxChannels < 1 || config.audio.maxChannels > 8)
    {
        return fail(ConfigError::OutOfRange,
                    "audio.maxChannels is " + std::to_string(config.audio.maxChannels) +
                        ", expected 1..8");
    }
    if (config.audio.packetDurationMs < 1 || config.audio.packetDurationMs > 200)
    {
        return fail(ConfigError::OutOfRange, "audio.packetDurationMs is " +
                                                 std::to_string(config.audio.packetDurationMs) +
                                                 ", expected 1..200");
    }

    // Section 18 recommends 500-3000 ms and section 35 a high watermark of
    // 2500. The bound is wider than either, because what it exists to catch is
    // a target of zero -- which would pace every room to never send anything --
    // and one so large that a room's ring buffer could never reach it.
    if (config.audio.targetBufferMs < 100 || config.audio.targetBufferMs > 30000)
    {
        return fail(ConfigError::OutOfRange, "audio.targetBufferMs is " +
                                                 std::to_string(config.audio.targetBufferMs) +
                                                 ", expected 100..30000");
    }
    if (config.audio.targetBufferMs < config.audio.packetDurationMs)
    {
        return fail(ConfigError::OutOfRange,
                    "audio.targetBufferMs is " + std::to_string(config.audio.targetBufferMs) +
                        ", which is less than one packet (" +
                        std::to_string(config.audio.packetDurationMs) + " ms)");
    }

    // Section 20's barrier waits for audio a room's pacing is allowed to
    // produce. A minimum above the target is a barrier that can never open,
    // which is a house that never plays and says nothing about why.
    if (config.audio.minimumStartBufferMs < config.audio.packetDurationMs ||
        config.audio.minimumStartBufferMs > config.audio.targetBufferMs)
    {
        return fail(ConfigError::OutOfRange,
                    "audio.minimumStartBufferMs is " +
                        std::to_string(config.audio.minimumStartBufferMs) + ", expected " +
                        std::to_string(config.audio.packetDurationMs) + ".." +
                        std::to_string(config.audio.targetBufferMs) +
                        " (one packet to the prebuffer target)");
    }

    // A zero timeout is a first start that waits for nobody, and a zero delay
    // is an instant that has passed by the time the message arrives. Both are
    // a house that starts out of step rather than one that does not start.
    if (config.audio.clientReadyTimeoutMs < 100 || config.audio.clientReadyTimeoutMs > 60000)
    {
        return fail(ConfigError::OutOfRange,
                    "audio.clientReadyTimeoutMs is " +
                        std::to_string(config.audio.clientReadyTimeoutMs) +
                        ", expected 100..60000");
    }

    // The client refuses a start more than 60 s away (its own kMaxStartLeadNs)
    // and one that has already passed, so the useful range is well inside
    // both ends of that.
    if (config.audio.startDelayMs < 20 || config.audio.startDelayMs > 30000)
    {
        return fail(ConfigError::OutOfRange, "audio.startDelayMs is " +
                                                 std::to_string(config.audio.startDelayMs) +
                                                 ", expected 20..30000");
    }

    // Phase 26. A cut with no lead is one no room hears in time, and a lead
    // of seconds is not a seek any more but a slow stop. The fill lead may be
    // zero: the client skips whatever of the seeked audio arrives late.
    if (config.audio.seekFlushLeadMs < 10 || config.audio.seekFlushLeadMs > 5000)
    {
        return fail(ConfigError::OutOfRange, "audio.seekFlushLeadMs is " +
                                                 std::to_string(config.audio.seekFlushLeadMs) +
                                                 ", expected 10..5000");
    }
    if (config.audio.seekFillLeadMs > 5000)
    {
        return fail(ConfigError::OutOfRange, "audio.seekFillLeadMs is " +
                                                 std::to_string(config.audio.seekFillLeadMs) +
                                                 ", expected 0..5000");
    }

    // Section 26 asks for "10-30 seconds" of regression window and section 46
    // names twenty. Below a second there is no lever arm at all -- the slope
    // uncertainty swamps the tens of ppm being looked for -- and past a few
    // minutes the window describes an oscillator that has since warmed up.
    if (config.audio.driftWindowMs < 1000 || config.audio.driftWindowMs > 300000)
    {
        return fail(ConfigError::OutOfRange, "audio.driftWindowMs is " +
                                                 std::to_string(config.audio.driftWindowMs) +
                                                 ", expected 1000..300000");
    }

    // Section 31's two limits, and the one relation between them: the
    // convergence limit is the temporary widening of the normal one, so a
    // deployment that set it lower would have written a narrowing and got a
    // controller that pulls harder once it is in sync than before.
    if (config.audio.correctionLimitPpm == 0 || config.audio.correctionLimitPpm > 10000)
    {
        return fail(ConfigError::OutOfRange, "audio.correctionLimitPpm is " +
                                                 std::to_string(config.audio.correctionLimitPpm) +
                                                 ", expected 1..10000");
    }
    if (config.audio.convergenceLimitPpm < config.audio.correctionLimitPpm ||
        config.audio.convergenceLimitPpm > 10000)
    {
        return fail(ConfigError::OutOfRange,
                    "audio.convergenceLimitPpm is " +
                        std::to_string(config.audio.convergenceLimitPpm) + ", expected " +
                        std::to_string(config.audio.correctionLimitPpm) + "..10000");
    }

    // The block size depends on three fields together, and the memory limit
    // on the block size, so both are checked once here rather than
    // approximated field by field. A limit that cannot hold one zone at the
    // ceiling is a core that refuses the formats it says it plays.
    const size_t blockBytes = blockBytesFor(config.audio.maxSampleRate, config.audio.maxChannels,
                                            config.audio.packetDurationMs);
    if (blockBytes == 0)
        return fail(ConfigError::OutOfRange, "the audio ceiling describes a zero-sized block");
    if (const uint32_t least = ceilingZoneMb(config.audio); config.audio.memoryLimitMb < least)
    {
        return fail(ConfigError::OutOfRange,
                    "audio.memoryLimitMb is " + std::to_string(config.audio.memoryLimitMb) +
                        ", expected at least " + std::to_string(least) + " (one zone at " +
                        describeFormat(config.audio.largestFormat()) + ")");
    }

    // Two listeners cannot share one port, and a deployment that set them the
    // same would get a bind failure from whichever started second and no clue
    // which field caused it. Zero is exempt: it means "whichever port is
    // free", so two zeros are two different ports.
    if (config.network.controlPort != 0 &&
        config.network.controlPort == config.network.audioPort)
    {
        return fail(ConfigError::OutOfRange,
                    "network.audioPort is " + std::to_string(config.network.audioPort) +
                        ", which is also network.controlPort");
    }

    // select() watches a fixed number of descriptors, and the listener refuses
    // to start above it; catching that here means a bad config is a startup
    // message about the file rather than one about a socket API.
    if (config.network.maxConnections < 1 || config.network.maxConnections > 60)
    {
        return fail(ConfigError::OutOfRange,
                    "network.maxConnections is " +
                        std::to_string(config.network.maxConnections) + ", expected 1..60");
    }
    // A ping that is not answered before the silence timeout expires never
    // gets the chance to prove the room is there, which turns a liveness check
    // into a disconnect on a timer.
    if (config.network.pingIntervalMs < 100)
    {
        return fail(ConfigError::OutOfRange, "network.pingIntervalMs is " +
                                                 std::to_string(config.network.pingIntervalMs) +
                                                 ", expected at least 100");
    }
    if (config.network.silenceTimeoutMs <= config.network.pingIntervalMs)
    {
        return fail(ConfigError::OutOfRange,
                    "network.silenceTimeoutMs is " +
                        std::to_string(config.network.silenceTimeoutMs) +
                        ", which is not longer than network.pingIntervalMs (" +
                        std::to_string(config.network.pingIntervalMs) + ")");
    }

    // The control API has no authentication, so where it listens is the whole
    // of its access control -- an empty address would have to be read as
    // "everywhere", which is the one reading this field must never get.
    if (config.control.bindAddress.empty())
        return fail(ConfigError::OutOfRange, "control.bindAddress is empty");

    // Three listeners, and the same argument as the pair above: a clash would
    // be a bind failure naming no field.
    if (config.control.port != 0 && (config.control.port == config.network.controlPort ||
                                      config.control.port == config.network.audioPort))
    {
        return fail(ConfigError::OutOfRange,
                    "control.port is " + std::to_string(config.control.port) +
                        ", which is also a MATP port in network");
    }

    // An empty base, one with no host, or one that is not http(s) would leave
    // source.stage with nothing to hold a URI against. And a base on one of
    // this server's own ports points the fetch back at a listener that speaks
    // MATP or the control API -- better a startup message about the file than
    // a zone that fails on its first track.
    const auto origin = MediaOrigin::parse(config.media.baseUrl);
    if (!origin)
        return fail(ConfigError::OutOfRange, "media.baseUrl: " + origin.error().detail);
    for (const uint16_t own :
         {config.network.controlPort, config.network.audioPort, config.control.port})
    {
        if (own != 0 && origin->port() == own)
        {
            return fail(ConfigError::OutOfRange,
                        "media.baseUrl is on port " + std::to_string(own) +
                            ", which is one of this server's own listeners");
        }
    }

    // A playing room holds its prebuffer target, and the zone has decoded one
    // lookahead -- the target plus a packet -- past the room furthest ahead.
    // That is how long the far end can go quiet before anybody hears it, and
    // a stuck connection has to be given up on and replaced inside it. Below
    // 100 ms a slow first byte on a LAN is already a stall.
    const uint64_t heldAheadMs = 2ULL * config.audio.targetBufferMs + config.audio.packetDurationMs;
    if (config.media.stallTimeoutMs < 100 || config.media.stallTimeoutMs >= heldAheadMs)
    {
        return fail(ConfigError::OutOfRange,
                    "media.stallTimeoutMs is " + std::to_string(config.media.stallTimeoutMs) +
                        ", expected 100.." + std::to_string(heldAheadMs - 1) +
                        " (less than twice audio.targetBufferMs plus audio.packetDurationMs, "
                        "which is what a playing room holds ahead of what is audible)");
    }

    // Each retry waits a little longer than the last, and a zone that is
    // retrying is a zone whose rooms are draining: past ten the backoff alone
    // is longer than any buffer.
    if (config.media.retries > 10)
    {
        return fail(ConfigError::OutOfRange,
                    "media.retries is " + std::to_string(config.media.retries) +
                        ", expected 0..10");
    }

    // The token goes into a header line as it stands, so a line break would
    // end the header and start another, and a space is not in a bearer
    // token's grammar. The message says where, never what: this is the one
    // field whose value must not reach a log.
    for (size_t index = 0; index < config.media.token.size(); ++index)
    {
        const auto byte = static_cast<unsigned char>(config.media.token[index]);
        if (byte <= 0x20 || byte >= 0x7f)
        {
            return fail(ConfigError::OutOfRange,
                        "media.token has a space, a control character or a non-ASCII byte at "
                        "position " +
                            std::to_string(index) + ", expected printable ASCII only");
        }
    }

    return {};
}

Result<ServerConfig, ConfigError> load(const std::filesystem::path& path)
{
    auto text = fileutil::readTextFile(path);
    if (!text)
    {
        if (text.error().code == FileError::NotFound)
            return fail(ConfigError::Missing, text.error().detail);
        return fail(ConfigError::Unreadable, describe(text.error()));
    }
    return deserialize(*text);
}

Status<ConfigError> save(const std::filesystem::path& path, const ServerConfig& config)
{
    if (auto valid = validate(config); !valid)
        return valid;

    if (auto written = fileutil::writeTextFileAtomic(path, serialize(config)); !written)
        return fail(ConfigError::WriteFailed, describe(written.error()));

    return {};
}

std::filesystem::path defaultPath()
{
    return fileutil::userConfigDirectory() / "server.json";
}

std::filesystem::path logFilePath(const ServerConfig& config,
                                  const std::filesystem::path& configPath)
{
    if (config.logFile.empty())
        return {};
    const std::filesystem::path logFile(config.logFile);
    return logFile.is_absolute() ? logFile : configPath.parent_path() / logFile;
}

} // namespace server_config
} // namespace multiroom
