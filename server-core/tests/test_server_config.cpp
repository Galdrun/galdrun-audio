#include "TestSupport.h"

#include "config/ServerConfig.h"
#include "util/FileUtil.h"

#include <filesystem>

using namespace multiroom;

MULTIROOM_TEST(server_config_round_trips_through_json)
{
    ServerConfig config;
    config.logLevel = LogLevel::Debug;
    config.logFile = "/var/log/galdrun/server.log";
    config.audio.maxSampleRate = 96000;
    config.audio.maxChannels = 6;
    config.audio.packetDurationMs = 40;
    config.audio.memoryLimitMb = 96;
    config.audio.targetBufferMs = 2000;
    config.network.controlPort = 5000;
    config.network.audioPort = 5001;

    const auto parsed = server_config::deserialize(server_config::serialize(config));
    CHECK_OK(parsed);
    CHECK_EQ(parsed->audio.targetBufferMs, 2000u);
    CHECK_EQ(parsed->network.controlPort, uint16_t{5000});
    CHECK_EQ(parsed->network.audioPort, uint16_t{5001});

    CHECK(parsed->logLevel == LogLevel::Debug);
    CHECK_EQ(parsed->logFile, config.logFile);
    CHECK_EQ(parsed->audio.maxSampleRate, 96000u);
    CHECK_EQ(parsed->audio.maxChannels, uint16_t{6});
    CHECK_EQ(parsed->audio.packetDurationMs, 40u);
    CHECK_EQ(parsed->audio.memoryLimitMb, 96u);

    // And serializing what came back produces the same document, so a config
    // the server rewrites is byte-stable.
    CHECK_EQ(server_config::serialize(*parsed), server_config::serialize(config));
}

MULTIROOM_TEST(server_config_defaults_every_missing_key)
{
    const ServerConfig defaults;

    const auto empty = server_config::deserialize("{}");
    CHECK_OK(empty);
    CHECK(empty->logLevel == defaults.logLevel);
    CHECK_EQ(empty->logFile, defaults.logFile);
    CHECK_EQ(empty->audio.maxSampleRate, defaults.audio.maxSampleRate);
    CHECK_EQ(empty->audio.maxChannels, defaults.audio.maxChannels);
    CHECK_EQ(empty->audio.packetDurationMs, defaults.audio.packetDurationMs);
    CHECK_EQ(empty->audio.memoryLimitMb, defaults.audio.memoryLimitMb);
    CHECK_EQ(empty->audio.targetBufferMs, defaults.audio.targetBufferMs);
    CHECK_EQ(empty->network.controlPort, defaults.network.controlPort);
    CHECK_EQ(empty->network.audioPort, defaults.network.audioPort);
    // Section 3's example pair, and the one the client's "control port plus
    // one" fallback assumes.
    CHECK_EQ(defaults.network.audioPort, uint16_t{4712});

    // A partial audio object defaults the rest of its own keys rather than the
    // whole section.
    const auto partial = server_config::deserialize(R"({"audio":{"maxChannels":8}})");
    CHECK_OK(partial);
    CHECK_EQ(partial->audio.maxChannels, uint16_t{8});
    CHECK_EQ(partial->audio.maxSampleRate, defaults.audio.maxSampleRate);
    CHECK_EQ(partial->audio.memoryLimitMb, defaults.audio.memoryLimitMb);
}

MULTIROOM_TEST(server_config_rejects_an_unknown_key)
{
    // A server that silently ignores a typo runs on settings nobody chose.
    CHECK_ERR(server_config::deserialize(R"({"logLevl":"debug"})"), ConfigError::UnknownKey);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"maxSampleRte":48000}})"),
              ConfigError::UnknownKey);
    CHECK_ERR(server_config::deserialize(R"({"controlPort":4720})"), ConfigError::UnknownKey);

    // The detail names the key, because that is the whole point of reporting it.
    const auto rejected = server_config::deserialize(R"({"audio":{"blockCont":8}})");
    CHECK(!rejected.has_value());
    CHECK_EQ(describe(rejected.error()), std::string("UnknownKey: audio.blockCont"));
}

MULTIROOM_TEST(server_config_rejects_a_key_of_the_wrong_type)
{
    CHECK_ERR(server_config::deserialize(R"({"logLevel":3})"), ConfigError::WrongType);
    CHECK_ERR(server_config::deserialize(R"({"logFile":42})"), ConfigError::WrongType);
    CHECK_ERR(server_config::deserialize(R"({"audio":7})"), ConfigError::WrongType);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"memoryLimitMb":"64"}})"),
              ConfigError::WrongType);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"memoryLimitMb":12.5}})"),
              ConfigError::WrongType);
}

MULTIROOM_TEST(server_config_rejects_a_value_outside_its_range)
{
    CHECK_ERR(server_config::deserialize(R"({"logLevel":"verbose"})"), ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"maxSampleRate":7999}})"),
              ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"maxSampleRate":768001}})"),
              ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"maxChannels":0}})"),
              ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"maxChannels":9}})"),
              ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"packetDurationMs":0}})"),
              ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"packetDurationMs":201}})"),
              ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"memoryLimitMb":0}})"),
              ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"memoryLimitMb":-1}})"),
              ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"targetBufferMs":0}})"),
              ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"targetBufferMs":30001}})"),
              ConfigError::OutOfRange);

    // A prebuffer shorter than one packet would pace every room to never send
    // anything at all.
    CHECK_ERR(server_config::deserialize(
                  R"({"audio":{"packetDurationMs":200,"targetBufferMs":100}})"),
              ConfigError::OutOfRange);

    // Two listeners cannot share one port, and the bind failure that would
    // otherwise report it names neither field. Zero is exempt: it means
    // "whichever port is free", so two zeros are two different ports.
    CHECK_ERR(server_config::deserialize(R"({"network":{"controlPort":4711,"audioPort":4711}})"),
              ConfigError::OutOfRange);
    CHECK_OK(server_config::deserialize(R"({"network":{"controlPort":0,"audioPort":0}})"));
}

MULTIROOM_TEST(server_config_rejects_a_memory_limit_that_cannot_hold_one_zone_at_the_ceiling)
{
    // Phase 42. Every field is individually in range; together they describe
    // a zone that needs more than the limit, which is why the check is not
    // per field. 7.7 MB is one zone at the default 192 kHz int32 stereo
    // ceiling, so 8 MB is the least the defaults take.
    ServerConfig config;
    CHECK_EQ(server_config::ceilingZoneMb(config.audio), 8u);
    config.audio.memoryLimitMb = 7;
    CHECK_ERR(server_config::validate(config), ConfigError::OutOfRange);
    config.audio.memoryLimitMb = 8;
    CHECK_OK(server_config::validate(config));

    config.audio.maxSampleRate = 768000;
    config.audio.maxChannels = 8;
    config.audio.memoryLimitMb = 64;
    const auto refused = server_config::validate(config);
    CHECK_ERR(refused, ConfigError::OutOfRange);
    if (!refused)
        CHECK(refused.error().detail.find("memoryLimitMb") != std::string::npos);
}

MULTIROOM_TEST(server_config_refuses_block_count_and_names_what_replaced_it)
{
    // An old config gets a clear error rather than a key silently ignored.
    const auto old = server_config::deserialize(R"({"audio":{"blockCount":128}})");
    CHECK_ERR(old, ConfigError::UnknownKey);
    if (!old)
        CHECK(old.error().detail.find("memoryLimitMb") != std::string::npos);
}

MULTIROOM_TEST(server_config_rejects_a_document_that_is_not_an_object)
{
    CHECK_ERR(server_config::deserialize(""), ConfigError::Malformed);
    CHECK_ERR(server_config::deserialize("not json"), ConfigError::Malformed);
    CHECK_ERR(server_config::deserialize("[]"), ConfigError::Malformed);
    CHECK_ERR(server_config::deserialize(R"({"logFile":"x",})"), ConfigError::Malformed);
}

MULTIROOM_TEST(server_config_load_reports_a_missing_file_as_its_own_case)
{
    const multiroomtest::TempDirectory temp("config");

    // Not an error the caller has to treat as fatal: a first start has no file
    // and the defaults are a working server.
    CHECK_ERR(server_config::load(temp.file("server.json")), ConfigError::Missing);
}

MULTIROOM_TEST(server_config_saves_and_loads_a_file)
{
    const multiroomtest::TempDirectory temp("config");
    const std::filesystem::path path =
        std::filesystem::path(temp.path()) / "nested" / "server.json";

    ServerConfig config;
    config.logLevel = LogLevel::Trace;
    config.audio.memoryLimitMb = 200;

    CHECK_OK(server_config::save(path, config));
    CHECK(std::filesystem::exists(path));

    // The temporary the atomic write used is gone.
    CHECK(!std::filesystem::exists(std::filesystem::path(path.string() + ".tmp")));

    const auto loaded = server_config::load(path);
    CHECK_OK(loaded);
    CHECK(loaded->logLevel == LogLevel::Trace);
    CHECK_EQ(loaded->audio.memoryLimitMb, 200u);
}

MULTIROOM_TEST(server_config_load_reports_a_bad_file_rather_than_defaulting)
{
    const multiroomtest::TempDirectory temp("config");
    const std::filesystem::path path = std::filesystem::path(temp.path()) / "server.json";

    CHECK_OK(fileutil::writeTextFileAtomic(path, R"({"audio":{"blockCont":8}})"));
    CHECK_ERR(server_config::load(path), ConfigError::UnknownKey);
}

MULTIROOM_TEST(server_config_refuses_to_save_a_config_it_would_not_load)
{
    const multiroomtest::TempDirectory temp("config");
    const std::filesystem::path path = std::filesystem::path(temp.path()) / "server.json";

    ServerConfig config;
    config.audio.maxChannels = 99;

    CHECK_ERR(server_config::save(path, config), ConfigError::OutOfRange);
    CHECK(!std::filesystem::exists(path));
}

MULTIROOM_TEST(server_config_largest_format_is_what_sizes_a_block)
{
    ServerConfig config;
    config.audio.maxSampleRate = 96000;
    config.audio.maxChannels = 2;

    const AudioFormat largest = config.audio.largestFormat();
    CHECK_EQ(largest.sampleRate, 96000u);
    CHECK_EQ(largest.channels, uint16_t{2});
    CHECK(largest.sampleType == kWidestSampleType);
    CHECK_EQ(largest.bytesPerFrame(), 8u);
}

MULTIROOM_TEST(server_config_default_path_sits_under_the_user_config_directory)
{
    const std::filesystem::path path = server_config::defaultPath();
    CHECK_EQ(path.filename().string(), std::string("server.json"));
    CHECK_EQ(path.parent_path(), fileutil::userConfigDirectory());
}

MULTIROOM_TEST(server_config_carries_the_control_api_and_the_audio_path_settings)
{
    const ServerConfig defaults;

    // Empty means "whichever resampler this build has", which is the honest
    // default for a build whose two resampler options are both off.
    CHECK_EQ(defaults.audio.resampler, std::string());
    CHECK(defaults.audio.audioControl == AudioControl::Auto);

    // Loopback by default: the API has no authentication.
    CHECK_EQ(defaults.control.bindAddress, std::string("127.0.0.1"));
    CHECK_EQ(defaults.control.port, uint16_t(4713));

    ServerConfig config;
    config.audio.resampler = "soxr";
    config.audio.audioControl = AudioControl::BitPerfect;
    config.control.bindAddress = "0.0.0.0";
    config.control.port = 5000;

    const auto parsed = server_config::deserialize(server_config::serialize(config));
    CHECK_OK(parsed);
    CHECK_EQ(parsed->audio.resampler, std::string("soxr"));
    CHECK(parsed->audio.audioControl == AudioControl::BitPerfect);
    CHECK_EQ(parsed->control.bindAddress, std::string("0.0.0.0"));
    CHECK_EQ(parsed->control.port, uint16_t(5000));
    CHECK_EQ(server_config::serialize(*parsed), server_config::serialize(config));

    // "auto" is *no preference* and not "never bitperfect" -- the one value of
    // the three that is misread.
    const auto none = server_config::deserialize(R"({"audio":{"audioControl":"auto"}})");
    CHECK_OK(none);
    CHECK(none->audio.audioControl == AudioControl::Auto);

    // The names from before section 59 are refused, key and values alike.
    CHECK(!server_config::deserialize(R"({"audio":{"bitperfect":"no"}})"));
    CHECK(!server_config::deserialize(R"({"audio":{"audioControl":"force"}})"));
}

MULTIROOM_TEST(server_config_carries_the_house_dither_and_refuses_an_unknown_one_by_name)
{
    // Off until a listening test says otherwise (phase 31).
    CHECK(ServerConfig{}.audio.dither == DitherMode::No);

    ServerConfig config;
    config.audio.dither = DitherMode::TriangularPdf;
    const std::string text = server_config::serialize(config);
    CHECK(text.find("\"tpdf\"") != std::string::npos);
    const auto parsed = server_config::deserialize(text);
    CHECK_OK(parsed);
    CHECK(parsed->audio.dither == DitherMode::TriangularPdf);

    const auto off = server_config::deserialize(R"({"audio":{"dither":"off"}})");
    CHECK_OK(off);
    CHECK(off->audio.dither == DitherMode::No);

    const auto unknown = server_config::deserialize(R"({"audio":{"dither":"shaped"}})");
    CHECK_ERR(unknown, ConfigError::OutOfRange);
    CHECK(unknown.error().detail.find("shaped") != std::string::npos);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"dither":true}})"), ConfigError::WrongType);
}

MULTIROOM_TEST(server_config_rejects_a_control_api_it_could_not_place)
{
    // Phase 17 deleted the one-zone stand-in, so a config that still names one
    // is told so rather than silently playing nothing.
    CHECK_ERR(server_config::deserialize(R"({"zone":{"id":"living"}})"), ConfigError::UnknownKey);
    CHECK_ERR(server_config::deserialize(R"({"control":{"address":"127.0.0.1"}})"),
              ConfigError::UnknownKey);
    CHECK_ERR(server_config::deserialize(R"({"control":{"port":"4713"}})"),
              ConfigError::WrongType);
    // A file from before the merge still loads: the drift key decides
    // nothing, and bitPerfectFallback is the old name for auto.
    const auto old = server_config::deserialize(
        R"({"audio":{"audioControl":"bitPerfectFallback","driftCorrection":true}})");
    CHECK_OK(old);
    CHECK(old->audio.audioControl == AudioControl::Auto);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"audioControl":"sometimes"}})"),
              ConfigError::OutOfRange);

    // An empty address would have to mean "everywhere", on an API with no
    // authentication.
    CHECK_ERR(server_config::deserialize(R"({"control":{"bindAddress":""}})"),
              ConfigError::OutOfRange);

    // Three listeners, three ports; zero is exempt because it means "any".
    CHECK_ERR(server_config::deserialize(R"({"control":{"port":4711}})"), ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"control":{"port":4712}})"), ConfigError::OutOfRange);
    CHECK_OK(server_config::deserialize(
        R"({"control":{"port":0},"network":{"controlPort":0,"audioPort":0}})"));
}

MULTIROOM_TEST(server_config_names_one_media_origin_and_refuses_one_it_cannot_hold_uris_against)
{
    // The high-end server on this machine, over plain HTTP.
    const ServerConfig defaults;
    CHECK_EQ(defaults.media.baseUrl, std::string("http://127.0.0.1:4714/"));
    CHECK_OK(server_config::validate(defaults));

    ServerConfig config;
    config.media.baseUrl = "https://media.lan:8443/galdrun/";
    const auto parsed = server_config::deserialize(server_config::serialize(config));
    CHECK_OK(parsed);
    CHECK_EQ(parsed->media.baseUrl, config.media.baseUrl);

    CHECK_ERR(server_config::deserialize(R"({"media":{"base":"http://h/"}})"),
              ConfigError::UnknownKey);
    CHECK_ERR(server_config::deserialize(R"({"media":{"baseUrl":4714}})"), ConfigError::WrongType);
    CHECK_ERR(server_config::deserialize(R"({"media":"http://h/"})"), ConfigError::WrongType);

    for (const char* base : {"", "http://", "http:///media/", "ftp://127.0.0.1/", "/srv/music",
                             "file:///srv/music", "http://127.0.0.1:0/", "http://h/?a=b"})
    {
        ServerConfig bad;
        bad.media.baseUrl = base;
        CHECK_ERR(server_config::validate(bad), ConfigError::OutOfRange);
    }

    // The phase 17 rule, extended: no two of the four may share a port, and
    // an absent port is the scheme's own.
    for (const char* base : {"http://127.0.0.1:4711/", "http://127.0.0.1:4712/",
                             "http://127.0.0.1:4713/"})
    {
        ServerConfig clash;
        clash.media.baseUrl = base;
        CHECK_ERR(server_config::validate(clash), ConfigError::OutOfRange);
    }
    ServerConfig https;
    https.media.baseUrl = "https://media.lan/";
    https.control.port = 443;
    CHECK_ERR(server_config::validate(https), ConfigError::OutOfRange);
}

MULTIROOM_TEST(server_config_carries_the_media_stall_timeout_and_retries)
{
    const ServerConfig defaults;
    CHECK_EQ(defaults.media.stallTimeoutMs, 2000u);
    CHECK_EQ(defaults.media.retries, 3u);

    ServerConfig config;
    config.media.stallTimeoutMs = 750;
    config.media.retries = 0;
    const auto parsed = server_config::deserialize(server_config::serialize(config));
    CHECK_OK(parsed);
    CHECK_EQ(parsed->media.stallTimeoutMs, 750u);
    CHECK_EQ(parsed->media.retries, 0u);

    CHECK_ERR(server_config::deserialize(R"({"media":{"retries":"3"}})"), ConfigError::WrongType);

    // What a playing room holds ahead of what is audible: its 1500 ms target
    // and the 1520 ms the zone decodes past it. A stall that long has already
    // been heard, so the timeout must be shorter, and the message says which
    // fields it was held against.
    ServerConfig stall;
    stall.media.stallTimeoutMs = 3019;
    CHECK_OK(server_config::validate(stall));
    stall.media.stallTimeoutMs = 3020;
    const auto refused = server_config::validate(stall);
    CHECK_ERR(refused, ConfigError::OutOfRange);
    if (!refused)
    {
        CHECK(refused.error().detail.find("media.stallTimeoutMs") != std::string::npos);
        CHECK(refused.error().detail.find("audio.targetBufferMs") != std::string::npos);
    }
    stall.audio.targetBufferMs = 3000;
    CHECK_OK(server_config::validate(stall));

    stall.media.stallTimeoutMs = 99;
    CHECK_ERR(server_config::validate(stall), ConfigError::OutOfRange);

    ServerConfig retries;
    retries.media.retries = 11;
    CHECK_ERR(server_config::validate(retries), ConfigError::OutOfRange);
}

MULTIROOM_TEST(server_config_carries_the_media_token_and_never_names_it_in_a_refusal)
{
    // None by default, and a base on another machine without one is still
    // a config that loads: a closed LAN is a legitimate deployment.
    ServerConfig config;
    CHECK(config.media.token.empty());
    config.media.baseUrl = "http://media.lan:4714/";
    CHECK_OK(server_config::validate(config));

    config.media.token = "Sh4red.s3cret~token/+=";
    const auto parsed = server_config::deserialize(server_config::serialize(config));
    CHECK_OK(parsed);
    CHECK_EQ(parsed->media.token, config.media.token);

    CHECK_ERR(server_config::deserialize(R"({"media":{"token":42}})"), ConfigError::WrongType);

    // Anything that would end the header line or is not a bearer token's
    // character, refused by position and never by value.
    for (const char* bad : {"secret\r\nX-Other: 1", "two words", "tab\there", "caf\xc3\xa9"})
    {
        ServerConfig refused;
        refused.media.token = bad;
        const auto result = server_config::validate(refused);
        CHECK_ERR(result, ConfigError::OutOfRange);
        if (!result)
        {
            CHECK(result.error().detail.find("media.token") != std::string::npos);
            CHECK(result.error().detail.find(bad) == std::string::npos);
        }
    }
}

MULTIROOM_TEST(server_config_carries_section_twenty_s_start_timings)
{
    const ServerConfig defaults;
    CHECK_EQ(defaults.audio.minimumStartBufferMs, 1000u);
    CHECK_EQ(defaults.audio.clientReadyTimeoutMs, 5000u);
    CHECK_EQ(defaults.audio.startDelayMs, 1000u);

    ServerConfig config;
    config.audio.minimumStartBufferMs = 800;
    config.audio.clientReadyTimeoutMs = 3000;
    config.audio.startDelayMs = 2000;

    const auto parsed = server_config::deserialize(server_config::serialize(config));
    CHECK_OK(parsed);
    CHECK_EQ(parsed->audio.minimumStartBufferMs, 800u);
    CHECK_EQ(parsed->audio.clientReadyTimeoutMs, 3000u);
    CHECK_EQ(parsed->audio.startDelayMs, 2000u);

    // A barrier that waits for more audio than a room's pacing is allowed to
    // produce is a house that never plays and says nothing about why.
    CHECK_ERR(server_config::deserialize(
                  R"({"audio":{"targetBufferMs":1500,"minimumStartBufferMs":1501}})"),
              ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(
                  R"({"audio":{"packetDurationMs":20,"minimumStartBufferMs":19}})"),
              ConfigError::OutOfRange);

    // A first start that waits for nobody, and one whose instant has passed by
    // the time the message arrives.
    CHECK_ERR(server_config::deserialize(R"({"audio":{"clientReadyTimeoutMs":0}})"),
              ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"clientReadyTimeoutMs":60001}})"),
              ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"startDelayMs":0}})"),
              ConfigError::OutOfRange);

    // The client refuses a start more than sixty seconds away.
    CHECK_ERR(server_config::deserialize(R"({"audio":{"startDelayMs":30001}})"),
              ConfigError::OutOfRange);
}

MULTIROOM_TEST(server_config_carries_phase_26_s_seek_leads)
{
    const ServerConfig defaults;
    CHECK_EQ(defaults.audio.seekFlushLeadMs, 100u);
    CHECK_EQ(defaults.audio.seekFillLeadMs, 150u);

    ServerConfig config;
    config.audio.seekFlushLeadMs = 60;
    config.audio.seekFillLeadMs = 0;
    const auto parsed = server_config::deserialize(server_config::serialize(config));
    CHECK_OK(parsed);
    CHECK_EQ(parsed->audio.seekFlushLeadMs, 60u);
    CHECK_EQ(parsed->audio.seekFillLeadMs, 0u);

    // A cut no room hears in time, and leads long enough to be a slow stop.
    CHECK_ERR(server_config::deserialize(R"({"audio":{"seekFlushLeadMs":9}})"),
              ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"seekFlushLeadMs":5001}})"),
              ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"seekFillLeadMs":5001}})"),
              ConfigError::OutOfRange);
}

MULTIROOM_TEST(server_config_carries_the_sync_controllers_window_and_limits)
{
    const ServerConfig defaults;
    CHECK_EQ(defaults.audio.driftWindowMs, 20000u);
    CHECK_EQ(defaults.audio.correctionLimitPpm, 100u);
    CHECK_EQ(defaults.audio.convergenceLimitPpm, 500u);

    ServerConfig config;
    config.audio.driftWindowMs = 30000;
    config.audio.correctionLimitPpm = 60;
    config.audio.convergenceLimitPpm = 250;

    const auto parsed = server_config::deserialize(server_config::serialize(config));
    CHECK_OK(parsed);
    CHECK_EQ(parsed->audio.driftWindowMs, 30000u);
    CHECK_EQ(parsed->audio.correctionLimitPpm, 60u);
    CHECK_EQ(parsed->audio.convergenceLimitPpm, 250u);

    // Section 26 asks for ten to thirty seconds of window; below a second the
    // slope uncertainty swamps the tens of ppm being looked for.
    CHECK_ERR(server_config::deserialize(R"({"audio":{"driftWindowMs":999}})"),
              ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"audio":{"driftWindowMs":300001}})"),
              ConfigError::OutOfRange);

    // A correction that can never be applied, and a "temporary widening" that
    // is really a narrowing -- a controller that pulls harder once it is in
    // sync than before it was.
    CHECK_ERR(server_config::deserialize(R"({"audio":{"correctionLimitPpm":0}})"),
              ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(
                  R"({"audio":{"correctionLimitPpm":100,"convergenceLimitPpm":99}})"),
              ConfigError::OutOfRange);
}

// Phase 29.
MULTIROOM_TEST(server_config_log_max_bytes_defaults_to_ten_mib_and_is_zero_or_at_least_64_kib)
{
    const auto empty = server_config::deserialize("{}");
    CHECK_OK(empty);
    CHECK_EQ(empty->logMaxBytes, 10u * 1024 * 1024);

    const auto off = server_config::deserialize(R"({"logMaxBytes":0})");
    CHECK_OK(off);
    CHECK_EQ(off->logMaxBytes, 0u);

    const auto smallest = server_config::deserialize(R"({"logMaxBytes":65536})");
    CHECK_OK(smallest);
    CHECK_EQ(smallest->logMaxBytes, 65536u);
    const auto again = server_config::deserialize(server_config::serialize(*smallest));
    CHECK_OK(again);
    CHECK_EQ(again->logMaxBytes, 65536u);

    const auto tiny = server_config::deserialize(R"({"logMaxBytes":65535})");
    CHECK_ERR(tiny, ConfigError::OutOfRange);
    if (!tiny)
        CHECK(tiny.error().detail.find("logMaxBytes") != std::string::npos);
    CHECK_ERR(server_config::deserialize(R"({"logMaxBytes":"10MB"})"), ConfigError::WrongType);
}

MULTIROOM_TEST(server_config_resolves_a_relative_log_file_against_its_own_directory)
{
    const std::filesystem::path configPath =
        std::filesystem::path("C:/ProgramData/Galdrun") / "server.json";

    ServerConfig config;
    CHECK(server_config::logFilePath(config, configPath).empty());

    config.logFile = "logs/server.log";
    CHECK(server_config::logFilePath(config, configPath) ==
          std::filesystem::path("C:/ProgramData/Galdrun") / "logs/server.log");

    const std::filesystem::path absolute = std::filesystem::absolute("elsewhere.log");
    config.logFile = absolute.string();
    CHECK(server_config::logFilePath(config, configPath) == absolute);
}

MULTIROOM_TEST(server_config_keeps_the_core_id_and_refuses_an_unusable_one)
{
    ServerConfig config;
    config.coreId = "galdrun-core-nas-1a2b3c4d";
    const auto parsed = server_config::deserialize(server_config::serialize(config));
    CHECK(parsed.has_value());
    CHECK_EQ(parsed->coreId, config.coreId);

    // Empty is a first start, which generates one; blanks and control
    // characters are an id the high-end server could not show or match.
    CHECK(server_config::deserialize(R"({"coreId":""})").has_value());
    CHECK_ERR(server_config::deserialize(R"({"coreId":" core"})"), ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"coreId":"a\tb"})"), ConfigError::OutOfRange);
    CHECK_ERR(server_config::deserialize(R"({"coreId":7})"), ConfigError::WrongType);
}
