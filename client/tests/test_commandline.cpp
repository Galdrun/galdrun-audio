#include "TestSupport.h"

#include "app/CommandLine.h"

#include <vector>

using namespace multiroom;

namespace {

bool parseArgs(const std::vector<const char*>& args, CommandLineOptions& out,
               std::string& error)
{
    std::vector<const char*> argv;
    argv.push_back("galdrun-client");
    for (size_t i = 0; i < args.size(); ++i)
        argv.push_back(args[i]);
    return parseCommandLine(static_cast<int>(argv.size()), argv.data(), out, error);
}

} // namespace

MULTIROOM_TEST(commandline_accepts_no_arguments)
{
    CommandLineOptions options;
    std::string error;
    CHECK(parseArgs({}, options, error));
    CHECK_EQ(options.rescanAudio, false);
    CHECK_EQ(options.hasServer, false);
    CHECK_EQ(options.helpRequested, false);
    CHECK(options.configPath.empty());
}

MULTIROOM_TEST(commandline_parses_separate_and_inline_values)
{
    CommandLineOptions separate;
    std::string error;
    CHECK(parseArgs({"--server", "192.168.1.20:4711", "--audio-device", "dev-1"}, separate,
                    error));
    CHECK_EQ(separate.hasServer, true);
    CHECK_EQ(separate.server, std::string("192.168.1.20:4711"));
    CHECK_EQ(separate.hasAudioDevice, true);
    CHECK_EQ(separate.audioDevice, std::string("dev-1"));

    CommandLineOptions inlineForm;
    CHECK(parseArgs({"--server=192.168.1.20:4711", "--audio-device=dev-1"}, inlineForm, error));
    CHECK_EQ(inlineForm.server, separate.server);
    CHECK_EQ(inlineForm.audioDevice, separate.audioDevice);
}

MULTIROOM_TEST(commandline_parses_flags_and_paths)
{
    CommandLineOptions options;
    std::string error;
    CHECK(parseArgs({"--rescan-audio", "--audio-cache", "/tmp/devices.json", "--config",
                     "/tmp/client.json", "--log-level", "trace"},
                    options, error));
    CHECK_EQ(options.rescanAudio, true);
    CHECK_EQ(options.audioCachePath, std::string("/tmp/devices.json"));
    CHECK_EQ(options.configPath, std::string("/tmp/client.json"));
    CHECK_EQ(options.hasLogLevel, true);
    CHECK(options.logLevel == LogLevel::Trace);
}

MULTIROOM_TEST(commandline_parses_help_and_version)
{
    CommandLineOptions options;
    std::string error;
    CHECK(parseArgs({"--help"}, options, error));
    CHECK_EQ(options.helpRequested, true);

    CHECK(parseArgs({"--version"}, options, error));
    CHECK_EQ(options.versionRequested, true);
}

MULTIROOM_TEST(commandline_rejects_bad_input)
{
    CommandLineOptions options;
    std::string error;

    CHECK(!parseArgs({"--unknown"}, options, error));
    CHECK(!error.empty());
    CHECK(!parseArgs({"--server"}, options, error));
    CHECK(!parseArgs({"--server="}, options, error));
    CHECK(!parseArgs({"--server", "no-port"}, options, error));
    CHECK(!parseArgs({"--log-level", "verbose"}, options, error));
    CHECK(!parseArgs({"--rescan-audio=yes"}, options, error));
    CHECK(!parseArgs({"positional"}, options, error));
}

MULTIROOM_TEST(commandline_overrides_config_without_touching_other_fields)
{
    ClientConfig config;
    config.server = "from-file:1000";
    config.audioDeviceId = "file-device";
    config.controlPort = 5555;
    config.preferredSampleRate = 44100;

    CommandLineOptions options;
    std::string error;
    CHECK(parseArgs({"--server", "cli:2000"}, options, error));
    applyCommandLine(options, config);

    CHECK_EQ(config.server, std::string("cli:2000"));
    // Untouched by the command line.
    CHECK_EQ(config.audioDeviceId, std::string("file-device"));
    CHECK_EQ(config.controlPort, static_cast<uint16_t>(5555));
    CHECK_EQ(config.preferredSampleRate, static_cast<uint32_t>(44100));
}

MULTIROOM_TEST(commandline_overrides_are_not_saved_with_other_settings)
{
    ClientConfig stored;
    stored.server = "original:4711";
    stored.controlPort = 5555;

    CommandLineOptions options;
    std::string error;
    CHECK(parseArgs({"--server", "temporary:4711", "--control-port", "6000"}, options, error));
    ClientConfig config = stored;
    applyCommandLine(options, config);

    // A rename: the overrides stay this run's.
    config.name = "Kitchen";
    ClientConfig saved = configToSave(options, stored, config);
    CHECK_EQ(saved.server, std::string("original:4711"));
    CHECK_EQ(saved.controlPort, static_cast<uint16_t>(5555));
    CHECK_EQ(saved.name, std::string("Kitchen"));

    // A server set explicitly is saved, and ends that override only.
    config.server = "chosen:4711";
    saved = configToSave(options, stored, config);
    CHECK_EQ(saved.server, std::string("chosen:4711"));
    CHECK(!options.hasServer);
    CHECK(options.hasControlPort);

    config.server = "temporary:4711";
    saved = configToSave(options, stored, config);
    CHECK_EQ(saved.server, std::string("temporary:4711"));
}

MULTIROOM_TEST(commandline_usage_lists_every_option)
{
    const std::string usage = usageText();
    CHECK(usage.find("--server") != std::string::npos);
    CHECK(usage.find("--audio-device") != std::string::npos);
    CHECK(usage.find("--rescan-audio") != std::string::npos);
    CHECK(usage.find("--audio-cache") != std::string::npos);
    CHECK(usage.find("--control-port") != std::string::npos);
    CHECK(usage.find("--log-file") != std::string::npos);
    CHECK(usage.find("--help") != std::string::npos);
}

MULTIROOM_TEST(commandline_parses_audio_mode)
{
    CommandLineOptions options;
    std::string error;

    CHECK(parseArgs({"--audio-mode", "exclusive-only"}, options, error));
    CHECK_EQ(options.hasAudioAccessMode, true);
    CHECK(options.audioAccessMode == AudioAccessPreference::ExclusiveOnly);

    CHECK(parseArgs({"--audio-mode=shared"}, options, error));
    CHECK(options.audioAccessMode == AudioAccessPreference::SharedOnly);

    CHECK(!parseArgs({"--audio-mode", "bitperfect"}, options, error));
    CHECK(!parseArgs({"--audio-mode"}, options, error));

    // The override must not touch anything else in the config.
    ClientConfig config;
    config.audioDeviceId = "file-device";
    CHECK(parseArgs({"--audio-mode", "shared"}, options, error));
    applyCommandLine(options, config);
    CHECK(config.audioAccessMode == AudioAccessPreference::SharedOnly);
    CHECK_EQ(config.audioDeviceId, std::string("file-device"));
}

MULTIROOM_TEST(commandline_parses_the_control_port_and_the_log_file)
{
    CommandLineOptions options;
    std::string error;

    CHECK(parseArgs({"--control-port", "4721", "--log-file", "/var/log/galdrun.log"}, options,
                    error));
    CHECK_EQ(options.hasControlPort, true);
    CHECK_EQ(options.controlPort, static_cast<uint16_t>(4721));
    CHECK_EQ(options.hasLogFile, true);
    CHECK_EQ(options.logFile, std::string("/var/log/galdrun.log"));

    ClientConfig config;
    config.controlPort = 4720;
    applyCommandLine(options, config);
    CHECK_EQ(config.controlPort, static_cast<uint16_t>(4721));

    // 0 is "let the OS choose" everywhere else here, and a control API on a
    // port nobody can predict is one nobody can reach.
    CHECK(!parseArgs({"--control-port", "0"}, options, error));
    CHECK(!parseArgs({"--control-port", "65536"}, options, error));
    CHECK(!parseArgs({"--control-port", "4721x"}, options, error));
    CHECK(!parseArgs({"--control-port"}, options, error));
    CHECK(!parseArgs({"--log-file"}, options, error));
}
