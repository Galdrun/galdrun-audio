#include "TestSupport.h"

#include "config/BoardConfig.h"
#include "config/ClientConfig.h"
#include "discovery/NodeId.h"
#include "util/FileUtil.h"

using namespace multiroom;

MULTIROOM_TEST(config_rpi_board_wiring_uses_valid_client_inputs_and_bindings)
{
    const std::string source = __FILE__;
    const std::string path = source.substr(0, source.find_last_of("/\\") + 1) +
                             "../../hardware/buildroot/board/rpi/inputs.json";
    ClientConfig config;
    std::string error;
    CHECK(client_config::load(path, config, error) == client_config::LoadResult::Ok);
    CHECK_EQ(config.inputs.size(), size_t(8));
    CHECK_EQ(config.inputGpioChip, std::string("/dev/gpiochip0"));
    CHECK_EQ(config.inputs[0].pinA, 7);
    CHECK_EQ(config.inputs[0].pinB, 8);
    CHECK(config.inputs[1].control.kind == matp::InputKind::Encoder);
    CHECK(config.inputs.back().control.bindings[static_cast<size_t>(matp::InputGesture::Click)].action ==
          matp::InputAction::ToggleStandby);
    ClientConfig restored;
    CHECK(client_config::deserialize(client_config::serialize(config), restored, error));
    CHECK_EQ(restored.inputs.size(), config.inputs.size());
}

MULTIROOM_TEST(config_standby_timeout_is_optional_bounded_and_persists)
{
    ClientConfig config;
    std::string error;
    CHECK(client_config::deserialize("{}", config, error));
    CHECK_EQ(config.standbyAfterSeconds, uint32_t(0));
    for (const auto seconds : {0u, 900u, 86400u})
    {
        config.standbyAfterSeconds = seconds;
        const auto saved = client_config::serialize(config);
        config.standbyAfterSeconds = 42;
        CHECK(client_config::deserialize(saved, config, error));
        CHECK_EQ(config.standbyAfterSeconds, seconds);
    }
    for (const char* value : {"-1", "86401", "1.5", "\"900\"", "true", "null"})
        CHECK(!client_config::deserialize(std::string("{\"standbyAfterSeconds\":") + value + "}", config, error));
}

MULTIROOM_TEST(config_deep_sleep_delay_is_off_or_a_minute_to_a_day)
{
    ClientConfig config;
    std::string error;
    CHECK(client_config::deserialize("{}", config, error));
    CHECK_EQ(config.deepSleepAfterSeconds, uint32_t(0));
    for (const auto seconds : {0u, 60u, 3600u, 86400u})
    {
        config.deepSleepAfterSeconds = seconds;
        const auto saved = client_config::serialize(config);
        config.deepSleepAfterSeconds = 42;
        CHECK(client_config::deserialize(saved, config, error));
        CHECK_EQ(config.deepSleepAfterSeconds, seconds);
    }
    // Under a minute: deep sleep that soon after boot does not reach its state.
    for (const char* value : {"-1", "1", "59", "86401", "1.5", "\"3600\"", "null"})
        CHECK(!client_config::deserialize(std::string("{\"deepSleepAfterSeconds\":") + value + "}", config, error));
}

MULTIROOM_TEST(config_parses_server_addresses)
{
    std::string host;
    uint16_t port = 0;

    CHECK(parseServerAddress("192.168.1.10:4711", host, port));
    CHECK_EQ(host, std::string("192.168.1.10"));
    CHECK_EQ(port, static_cast<uint16_t>(4711));

    CHECK(parseServerAddress("server.local:1", host, port));
    CHECK_EQ(port, static_cast<uint16_t>(1));

    CHECK(parseServerAddress("[::1]:4711", host, port));
    CHECK_EQ(host, std::string("::1"));
    CHECK_EQ(port, static_cast<uint16_t>(4711));

    CHECK(!parseServerAddress("", host, port));
    CHECK(!parseServerAddress("192.168.1.10", host, port));
    CHECK(!parseServerAddress(":4711", host, port));
    CHECK(!parseServerAddress("host:", host, port));
    CHECK(!parseServerAddress("host:0", host, port));
    CHECK(!parseServerAddress("host:65536", host, port));
    CHECK(!parseServerAddress("host:abc", host, port));
    CHECK(!parseServerAddress("host:-1", host, port));
    CHECK(!parseServerAddress("::1:4711", host, port)); // unbracketed IPv6 is ambiguous
    CHECK(!parseServerAddress("[::1]4711", host, port));
}

MULTIROOM_TEST(config_defaults_are_valid)
{
    ClientConfig config;
    std::string error;
    CHECK(client_config::validate(config, error));
    CHECK_EQ(config.preferredSampleRate, static_cast<uint32_t>(48000));
    CHECK_EQ(config.controlPort, static_cast<uint16_t>(4720));
    // Every interface, so a server that heard the beacon can attach it.
    CHECK_EQ(config.controlBindAddress, std::string("0.0.0.0"));
    CHECK(config.server.empty());
    // An empty clientId is valid in the file: startup generates one.
    CHECK(config.clientId.empty());
}

MULTIROOM_TEST(config_led_off_is_optional_boolean_and_persists)
{
    ClientConfig config;
    std::string error;
    CHECK(client_config::deserialize("{}", config, error));
    CHECK(!config.ledsOff);
    CHECK(!client_config::deserialize("{\"ledsOff\":\"true\"}", config, error));
    CHECK(client_config::deserialize("{\"ledsOff\":true}", config, error));
    CHECK(config.ledsOff);
    const auto saved = client_config::serialize(config);
    config.ledsOff = false;
    CHECK(client_config::deserialize(saved, config, error));
    CHECK(config.ledsOff);
}

MULTIROOM_TEST(config_active_defaults_on_and_persists)
{
    ClientConfig config;
    std::string error;
    CHECK(client_config::deserialize("{}", config, error));
    CHECK(config.active);
    CHECK(!client_config::deserialize("{\"active\":\"false\"}", config, error));
    CHECK(client_config::deserialize("{\"active\":false}", config, error));
    CHECK(!config.active);
    const auto saved = client_config::serialize(config);
    config.active = true;
    CHECK(client_config::deserialize(saved, config, error));
    CHECK(!config.active);
}

MULTIROOM_TEST(config_accepts_only_usable_client_ids)
{
    CHECK(discovery::isValidNodeId("livingroom"));
    CHECK(discovery::isValidNodeId("Living Room"));
    CHECK(discovery::isValidNodeId("K\xC3\xBC" "che")); // UTF-8 is a name, not an error
    CHECK(discovery::isValidNodeId(std::string(64, 'a')));

    CHECK(!discovery::isValidNodeId(""));
    CHECK(!discovery::isValidNodeId(std::string(65, 'a')));
    CHECK(!discovery::isValidNodeId(" livingroom"));
    CHECK(!discovery::isValidNodeId("livingroom "));
    CHECK(!discovery::isValidNodeId("living\nroom"));
    CHECK(!discovery::isValidNodeId("living\x7Froom"));

    ClientConfig config;
    config.clientId = "bad\tid";
    std::string error;
    CHECK(!client_config::validate(config, error));
}

MULTIROOM_TEST(config_generates_distinct_valid_client_ids)
{
    const std::string first = discovery::generateNodeId("galdrun", "STUDY-PC");
    const std::string second = discovery::generateNodeId("galdrun", "STUDY-PC");

    CHECK(discovery::isValidNodeId(first));
    CHECK_EQ(first.size(), std::string("galdrun-study-pc-00000000").size());
    CHECK_EQ(first.compare(0, 17, "galdrun-study-pc-"), 0);
    CHECK(first != second);

    // No host name: the random part alone.
    const std::string anonymous = discovery::generateNodeId("galdrun", "");
    CHECK_EQ(anonymous.size(), std::string("galdrun-00000000").size());
    CHECK_EQ(anonymous.compare(0, 8, "galdrun-"), 0);
    CHECK(discovery::isValidNodeId(anonymous));
}

MULTIROOM_TEST(config_generated_client_id_survives_an_unusual_host_name)
{
    // A dotted or overlong name is folded and cut, never turned into an id
    // the client would itself refuse.
    const std::string dotted = discovery::generateNodeId("galdrun", "pi.local");
    CHECK_EQ(dotted.compare(0, 17, "galdrun-pi-local-"), 0);

    const std::string longest = discovery::generateNodeId("galdrun", std::string(200, 'x'));
    CHECK(discovery::isValidNodeId(longest));
    CHECK_EQ(longest.size(), static_cast<size_t>(64));
}

MULTIROOM_TEST(config_reads_all_fields)
{
    const std::string text =
        "{\n"
        "  \"server\": \"192.168.1.10:4711\",\n"
        "  \"clientId\": \"livingroom\",\n"
        "  \"audioDeviceId\": \"hw:CARD=USB,DEV=0\",\n"
        "  \"preferredSampleRate\": 96000,\n"
        "  \"bufferMs\": 1800,\n"
        "  \"controlPort\": 5000,\n"
        "  \"controlBindAddress\": \"0.0.0.0\",\n"
        "  \"logLevel\": \"debug\"\n"
        "}\n";

    ClientConfig config;
    std::string error;
    CHECK(client_config::deserialize(text, config, error));
    CHECK_EQ(config.server, std::string("192.168.1.10:4711"));
    CHECK_EQ(config.clientId, std::string("livingroom"));
    CHECK_EQ(config.audioDeviceId, std::string("hw:CARD=USB,DEV=0"));
    CHECK_EQ(config.preferredSampleRate, static_cast<uint32_t>(96000));
    CHECK_EQ(config.bufferMs, static_cast<uint32_t>(1800));
    CHECK_EQ(config.controlPort, static_cast<uint16_t>(5000));
    CHECK_EQ(config.controlBindAddress, std::string("0.0.0.0"));
    CHECK(config.logLevel == LogLevel::Debug);
}

MULTIROOM_TEST(config_writes_lowercase_log_level)
{
    ClientConfig config;
    config.logLevel = LogLevel::Warn;
    const std::string text = client_config::serialize(config);
    CHECK(text.find("\"logLevel\": \"warn\"") != std::string::npos);

    ClientConfig reloaded;
    std::string error;
    CHECK(client_config::deserialize(text, reloaded, error));
    CHECK(reloaded.logLevel == LogLevel::Warn);
}

MULTIROOM_TEST(config_missing_keys_keep_defaults)
{
    ClientConfig config;
    std::string error;
    CHECK(client_config::deserialize("{\"server\":\"host:1234\"}", config, error));
    CHECK_EQ(config.server, std::string("host:1234"));
    CHECK_EQ(config.controlPort, static_cast<uint16_t>(4720));
    CHECK_EQ(config.preferredSampleRate, static_cast<uint32_t>(48000));
    CHECK_EQ(config.bufferMs, static_cast<uint32_t>(2500));
}

MULTIROOM_TEST(config_rejects_invalid_documents)
{
    ClientConfig config;
    std::string error;

    CHECK(!client_config::deserialize("not json", config, error));
    CHECK(!client_config::deserialize("[]", config, error));
    CHECK(!client_config::deserialize("{\"controlPort\":\"4712\"}", config, error));
    CHECK(!client_config::deserialize("{\"controlPort\":0}", config, error));
    CHECK(!client_config::deserialize("{\"controlPort\":70000}", config, error));
    CHECK(!client_config::deserialize("{\"preferredSampleRate\":-1}", config, error));
    // The buffer is configurable, not unbounded: it sizes
    // the ring buffer, and the target board has 512 MB in total.
    CHECK(!client_config::deserialize("{\"bufferMs\":0}", config, error));
    CHECK(!client_config::deserialize("{\"bufferMs\":500}", config, error));
    CHECK(!client_config::deserialize("{\"bufferMs\":600000}", config, error));
    CHECK(!client_config::deserialize("{\"bufferMs\":\"1500\"}", config, error));
    CHECK(!client_config::deserialize("{\"server\":\"no-port\"}", config, error));
    CHECK(!client_config::deserialize("{\"server\":12}", config, error));
    CHECK(!client_config::deserialize("{\"clientId\":42}", config, error));
    CHECK(!client_config::deserialize("{\"logLevel\":\"verbose\"}", config, error));
    CHECK(!error.empty());
}

MULTIROOM_TEST(config_reads_and_checks_the_external_dac_formats)
{
    ClientConfig config;
    std::string error;
    // The default is the list the external DAC was offered with before it
    // could be configured, and it survives a round trip.
    CHECK_EQ(config.i2s.externalFormats.size(), static_cast<size_t>(12));
    ClientConfig reloaded;
    CHECK(client_config::deserialize(client_config::serialize(config), reloaded, error));
    CHECK(reloaded.i2s.externalFormats == config.i2s.externalFormats);

    CHECK(client_config::deserialize(
        "{\"i2sExternalFormats\":[{\"sampleRate\":48000,\"sampleType\":\"int16\"},"
        "{\"sampleRate\":96000,\"sampleType\":\"int32\"}]}",
        config, error));
    CHECK_EQ(config.i2s.externalFormats.size(), static_cast<size_t>(2));
    CHECK_EQ(config.i2s.externalFormats[1].sampleRate, static_cast<uint32_t>(96000));
    CHECK(config.i2s.externalFormats[1].sampleType == AudioFormat::SampleType::Int32);
    CHECK_EQ(config.i2s.externalFormats[1].channels, static_cast<uint16_t>(2));

    // Nothing declared, a rate the I2S output does not clock, a layout it does
    // not put on the bus, the same pair twice, and a malformed entry.
    CHECK(!client_config::deserialize("{\"i2sExternalFormats\":[]}", config, error));
    CHECK(!client_config::deserialize(
        "{\"i2sExternalFormats\":[{\"sampleRate\":47000,\"sampleType\":\"int16\"}]}", config,
        error));
    CHECK(!client_config::deserialize(
        "{\"i2sExternalFormats\":[{\"sampleRate\":48000,\"sampleType\":\"int24\"}]}", config,
        error));
    CHECK(!client_config::deserialize(
        "{\"i2sExternalFormats\":[{\"sampleRate\":48000,\"sampleType\":\"int16\"},"
        "{\"sampleRate\":48000,\"sampleType\":\"int16\"}]}",
        config, error));
    CHECK(!client_config::deserialize("{\"i2sExternalFormats\":[48000]}", config, error));
    CHECK(!client_config::deserialize("{\"i2sExternalFormats\":{}}", config, error));
}

// A file written before `bufferMs` names the old `prebufferMs`, which the
// ring buffer held a second of headroom on top of: it is read as that buffer,
// so an update neither grows nor shrinks anybody's memory.
MULTIROOM_TEST(config_reads_the_old_prebuffer_as_the_buffer_it_gave)
{
    ClientConfig config;
    std::string error;
    CHECK(client_config::deserialize("{\"prebufferMs\":1500}", config, error));
    CHECK_EQ(config.bufferMs, static_cast<uint32_t>(2500));
    CHECK(client_config::deserialize("{\"prebufferMs\":1500,\"bufferMs\":3000}", config, error));
    CHECK_EQ(config.bufferMs, static_cast<uint32_t>(3000));
    CHECK(client_config::serialize(config).find("prebufferMs") == std::string::npos);
}

MULTIROOM_TEST(config_round_trips_through_disk)
{
    multiroomtest::TempDirectory temp("config");
    const std::string path = temp.file("client.json");

    ClientConfig original;
    original.server = "10.0.0.5:4711";
    original.clientId = "kitchen";
    original.audioDeviceId = "{0.0.0.00000000}.{guid}";
    original.preferredSampleRate = 192000;
    original.controlPort = 4713;
    original.controlBindAddress = "127.0.0.1";
    original.logLevel = LogLevel::Trace;

    std::string error;
    CHECK(client_config::save(path, original, error));
    CHECK(fileutil::exists(path));
    // The temporary file must not survive a successful write.
    CHECK(!fileutil::exists(path + ".tmp"));

    ClientConfig loaded;
    CHECK(client_config::load(path, loaded, error) == client_config::LoadResult::Ok);
    CHECK_EQ(loaded.server, original.server);
    CHECK_EQ(loaded.clientId, original.clientId);
    CHECK_EQ(loaded.audioDeviceId, original.audioDeviceId);
    CHECK_EQ(loaded.preferredSampleRate, original.preferredSampleRate);
    CHECK_EQ(loaded.controlPort, original.controlPort);
    CHECK_EQ(loaded.controlBindAddress, original.controlBindAddress);
    CHECK(loaded.logLevel == original.logLevel);
}

MULTIROOM_TEST(config_load_reports_missing_and_invalid)
{
    multiroomtest::TempDirectory temp("config_missing");
    ClientConfig config;
    std::string error;

    CHECK(client_config::load(temp.file("absent.json"), config, error) ==
          client_config::LoadResult::Missing);

    const std::string broken = temp.file("broken.json");
    CHECK(fileutil::writeTextFileAtomic(broken, "{ this is not json", error));
    CHECK(client_config::load(broken, config, error) == client_config::LoadResult::Invalid);
}

MULTIROOM_TEST(config_save_rejects_invalid_config)
{
    multiroomtest::TempDirectory temp("config_invalid");
    const std::string path = temp.file("client.json");

    ClientConfig config;
    config.server = "missing-port";

    std::string error;
    CHECK(!client_config::save(path, config, error));
    // Nothing may be written when validation fails.
    CHECK(!fileutil::exists(path));
}

MULTIROOM_TEST(config_overwrites_existing_file_atomically)
{
    multiroomtest::TempDirectory temp("config_overwrite");
    const std::string path = temp.file("client.json");

    ClientConfig first;
    first.server = "a.local:1000";
    std::string error;
    CHECK(client_config::save(path, first, error));

    ClientConfig second;
    second.server = "b.local:2000";
    CHECK(client_config::save(path, second, error));

    ClientConfig loaded;
    CHECK(client_config::load(path, loaded, error) == client_config::LoadResult::Ok);
    CHECK_EQ(loaded.server, std::string("b.local:2000"));
}

MULTIROOM_TEST(config_reads_audio_access_mode)
{
    ClientConfig config;
    std::string error;

    // The default is exclusive: bit-perfect, and the clock we measure is the
    // sound card's rather than the system mixer's.
    CHECK(config.audioAccessMode == AudioAccessPreference::PreferExclusive);

    CHECK(client_config::deserialize("{\"audioAccessMode\":\"exclusive-only\"}", config, error));
    CHECK(config.audioAccessMode == AudioAccessPreference::ExclusiveOnly);

    CHECK(client_config::deserialize("{\"audioAccessMode\":\"shared\"}", config, error));
    CHECK(config.audioAccessMode == AudioAccessPreference::SharedOnly);

    CHECK(!client_config::deserialize("{\"audioAccessMode\":\"bitperfect\"}", config, error));
    CHECK(!client_config::deserialize("{\"audioAccessMode\":1}", config, error));

    ClientConfig written;
    written.audioAccessMode = AudioAccessPreference::ExclusiveOnly;
    const std::string text = client_config::serialize(written);
    CHECK(text.find("\"audioAccessMode\": \"exclusive-only\"") != std::string::npos);

    ClientConfig reloaded;
    CHECK(client_config::deserialize(text, reloaded, error));
    CHECK(reloaded.audioAccessMode == AudioAccessPreference::ExclusiveOnly);
}

MULTIROOM_TEST(config_reads_i2s_clock_settings)
{
    ClientConfig config;
    std::string error;
    CHECK(client_config::deserialize("{}", config, error));
    CHECK(config.i2s.clockMode == I2sClockMode::Master);
    CHECK_EQ(config.i2s.slaveSampleRate, 48000u);

    CHECK(client_config::deserialize(
        "{\"i2sClockMode\":\"slave\",\"i2sSlaveSampleRate\":44100}", config, error));
    CHECK(config.i2s.clockMode == I2sClockMode::Slave);
    CHECK_EQ(config.i2s.slaveSampleRate, 44100u);

    CHECK(!client_config::deserialize("{\"i2sClockMode\":\"external\"}", config, error));
    CHECK(!client_config::deserialize("{\"i2sClockMode\":1}", config, error));
    CHECK(!client_config::deserialize("{\"i2sSlaveSampleRate\":0}", config, error));
    CHECK(!client_config::deserialize("{\"i2sSlaveSampleRate\":\"48000\"}", config, error));
    CHECK(!client_config::deserialize("{\"i2sSlaveSampleRate\":768000}", config, error));

    ClientConfig written;
    written.i2s.clockMode = I2sClockMode::Slave;
    written.i2s.slaveSampleRate = 96000;
    const std::string text = client_config::serialize(written);
    CHECK(text.find("\"i2sClockMode\": \"slave\"") != std::string::npos);

    ClientConfig reloaded;
    CHECK(client_config::deserialize(text, reloaded, error));
    CHECK(reloaded.i2s.clockMode == I2sClockMode::Slave);
    CHECK_EQ(reloaded.i2s.slaveSampleRate, 96000u);
}

MULTIROOM_TEST(config_load_accepts_a_utf8_byte_order_mark)
{
    // What Notepad writes. The JSON parser still refuses it on its own.
    multiroomtest::TempDirectory temp("config_bom");
    const std::string path = temp.file("client.json");
    std::string error;
    CHECK(fileutil::writeTextFileAtomic(path, "\xEF\xBB\xBF{\"clientId\":\"kitchen\"}", error));

    ClientConfig loaded;
    CHECK(client_config::load(path, loaded, error) == client_config::LoadResult::Ok);
    CHECK_EQ(loaded.clientId, std::string("kitchen"));

    // Only one: a second is not a byte order mark any more.
    CHECK(fileutil::writeTextFileAtomic(path, "\xEF\xBB\xBF\xEF\xBB\xBF{}", error));
    CHECK(client_config::load(path, loaded, error) == client_config::LoadResult::Invalid);
}

MULTIROOM_TEST(config_random_ids_are_sixteen_hex_digits_and_differ)
{
    const std::string first = discovery::generateRandomId();
    const std::string second = discovery::generateRandomId();
    CHECK_EQ(first.size(), static_cast<size_t>(16));
    CHECK(first.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(discovery::isValidNodeId(first));
    CHECK(first != second);
}

MULTIROOM_TEST(config_completes_a_fresh_identity_with_the_host_as_the_name)
{
    ClientConfig config;
    CHECK(client_config::completeIdentity(config, "RPI3-DietPi-01"));
    CHECK_EQ(config.clientId.size(), static_cast<size_t>(16));
    CHECK_EQ(config.name, std::string("RPI3-DietPi-01"));

    // Nothing left to do: a second start writes nothing.
    const ClientConfig before = config;
    CHECK(!client_config::completeIdentity(config, "other-host"));
    CHECK_EQ(config.clientId, before.clientId);
    CHECK_EQ(config.name, before.name);

    // A host the system does not name, or one that is no name: the id.
    ClientConfig anonymous;
    CHECK(client_config::completeIdentity(anonymous, ""));
    CHECK_EQ(anonymous.name, anonymous.clientId);
    ClientConfig overlong;
    CHECK(client_config::completeIdentity(overlong, std::string(80, 'x')));
    CHECK_EQ(overlong.name, overlong.clientId);
}

MULTIROOM_TEST(config_migrates_an_id_that_was_the_name_once)
{
    // Written before phase 26: the id was the name people saw.
    ClientConfig config;
    config.clientId = "Arbeitsraum";
    CHECK(client_config::completeIdentity(config, "esp32-a1b2c3"));
    CHECK_EQ(config.name, std::string("Arbeitsraum"));
    CHECK(config.clientId != std::string("Arbeitsraum"));
    CHECK_EQ(config.clientId.size(), static_cast<size_t>(16));
}

MULTIROOM_TEST(config_keeps_the_name_and_refuses_a_bad_one)
{
    ClientConfig config;
    config.clientId = "3f9c2a71e04b8d15";
    config.name = "K\xC3\xBC" "che";
    ClientConfig parsed;
    std::string error;
    CHECK(client_config::deserialize(client_config::serialize(config), parsed, error));
    CHECK_EQ(parsed.name, config.name);

    for (const char* bad : {" Kitchen", "Kitchen ", "Kit\nchen"})
    {
        config.name = bad;
        CHECK(!client_config::validate(config, error));
    }
    config.name = std::string(65, 'a');
    CHECK(!client_config::validate(config, error));
    CHECK(!client_config::deserialize("{\"name\": 5}", parsed, error));
}

MULTIROOM_TEST(board_config_reads_opt_ins_and_tolerates_a_missing_file)
{
    multiroomtest::TempDirectory temp("board_config");
    const std::string path = temp.file("board.json");
    std::string error;

    // Every board that is not a firmware image: no file, nothing enabled.
    CHECK(board_config::read(path).isObject());
    CHECK(!board_config::enabled(board_config::read(path), "statusLeds"));

    CHECK(fileutil::writeTextFileAtomic(path, "not json", error));
    CHECK(board_config::read(path).isObject());
    CHECK(!board_config::enabled(board_config::read(path), "statusLeds"));

    CHECK(fileutil::writeTextFileAtomic(
        path, R"({"statusLeds":true,"dacControls":false,"cpuFrequency":"true","maxVolumeDb":-9})", error));
    const json::Value board = board_config::read(path);
    CHECK(board_config::enabled(board, "statusLeds"));
    CHECK(!board_config::enabled(board, "dacControls"));
    CHECK(!board_config::enabled(board, "cpuFrequency")); // a string is not an opt-in
    CHECK(!board_config::enabled(board, "networkSettings"));
    double db = 0.0;
    CHECK(json::getDouble(board, "maxVolumeDb", db));
    CHECK_NEAR(db, -9.0, 0.0);

    // The cached copy is one object, whatever this machine has at kPath.
    CHECK(&board_config::current() == &board_config::current());
    CHECK(board_config::current().isObject());
}
