#include "TestSupport.h"

#include "app/CommandLine.h"

#include <initializer_list>
#include <string>
#include <vector>

using namespace multiroom;

namespace {

Result<CommandLineOptions, CommandLineError> parse(std::initializer_list<const char*> arguments)
{
    std::vector<const char*> argv;
    argv.push_back("galdrun-core");
    for (const char* argument : arguments)
        argv.push_back(argument);
    return parseCommandLine(static_cast<int>(argv.size()), argv.data());
}

} // namespace

MULTIROOM_TEST(command_line_defaults_to_nothing_set)
{
    const auto options = parse({});
    CHECK_OK(options);

    CHECK(options->configPath.empty()); // meaning server_config::defaultPath()
    CHECK(!options->logLevel.has_value());
    CHECK(!options->logFile.has_value());
    CHECK(!options->showHelp);
    CHECK(!options->showVersion);
}

MULTIROOM_TEST(command_line_reads_every_option)
{
    const auto options = parse({"--config", "C:/tmp/server.json", "--log-level", "trace",
                                "--log-file", "C:/tmp/server.log"});
    CHECK_OK(options);

    CHECK_EQ(options->configPath.string(), std::string("C:/tmp/server.json"));
    CHECK(options->logLevel.has_value());
    CHECK(*options->logLevel == LogLevel::Trace);
    CHECK(options->logFile.has_value());
    CHECK_EQ(*options->logFile, std::string("C:/tmp/server.log"));
}

MULTIROOM_TEST(command_line_help_and_version)
{
    const auto help = parse({"--help"});
    CHECK_OK(help);
    CHECK(help->showHelp);

    const auto shortHelp = parse({"-h"});
    CHECK_OK(shortHelp);
    CHECK(shortHelp->showHelp);

    const auto version = parse({"--version"});
    CHECK_OK(version);
    CHECK(version->showVersion);

    CHECK(commandLineUsage().find("--config") != std::string::npos);
    CHECK(commandLineUsage().find("--log-level") != std::string::npos);
}

MULTIROOM_TEST(command_line_rejects_what_it_does_not_understand)
{
    CHECK_ERR(parse({"--zone", "kitchen"}), CommandLineError::UnknownOption);
    CHECK_ERR(parse({"server.json"}), CommandLineError::UnknownOption);
    CHECK_ERR(parse({"--Config", "x"}), CommandLineError::UnknownOption);
}

MULTIROOM_TEST(command_line_rejects_an_option_with_no_value)
{
    // Almost always a quoting mistake, and an empty value would be worse than
    // a refusal: it would point the config at nothing in particular.
    CHECK_ERR(parse({"--config"}), CommandLineError::MissingValue);
    CHECK_ERR(parse({"--log-level"}), CommandLineError::MissingValue);
    CHECK_ERR(parse({"--log-file"}), CommandLineError::MissingValue);
    CHECK_ERR(parse({"--config", "x", "--log-level"}), CommandLineError::MissingValue);
}

MULTIROOM_TEST(command_line_rejects_a_log_level_that_is_not_one)
{
    const auto rejected = parse({"--log-level", "verbose"});
    CHECK_ERR(rejected, CommandLineError::InvalidValue);
    CHECK(describe(rejected.error()).find("verbose") != std::string::npos);
}

// Phase 29.
#if defined(_WIN32)
MULTIROOM_TEST(command_line_service_needs_a_config_and_says_so_by_name)
{
    const auto refused = parse({"--service"});
    CHECK_ERR(refused, CommandLineError::MissingOption);
    if (!refused)
    {
        CHECK(describe(refused.error()).find("--service") != std::string::npos);
        CHECK(describe(refused.error()).find("--config") != std::string::npos);
    }

    const auto service = parse({"--service", "--config", "C:/ProgramData/Galdrun/server.json"});
    CHECK_OK(service);
    CHECK(service->service);
    CHECK(!parse({})->service);
    CHECK(commandLineUsage().find("--service") != std::string::npos);
}
#else
MULTIROOM_TEST(command_line_has_no_service_option_off_windows)
{
    // systemd runs the server as it is; there is nothing for a flag to do.
    CHECK_ERR(parse({"--service", "--config", "/etc/galdrun/server.json"}),
              CommandLineError::UnknownOption);
}
#endif
