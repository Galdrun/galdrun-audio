#pragma once

// The five options the server accepts at startup, and a sixth on Windows.
//
// They exist so that a test run never touches the machine's real config, and
// so that a logging problem can be diagnosed before the config that would
// configure logging has been read. Everything else the server does is
// configured through server.json or, from phase 17, through the control API --
// a command line that grows a flag per feature is a second configuration
// system.

#include "core/Error.h"
#include "util/Logger.h"

#include <filesystem>
#include <optional>
#include <string>

namespace multiroom {

struct CommandLineOptions
{
    // Empty means server_config::defaultPath().
    std::filesystem::path configPath;

    // Set only if given; both override the config file in memory and are never
    // written back.
    std::optional<LogLevel> logLevel;
    std::optional<std::string> logFile;

    bool showHelp = false;
    bool showVersion = false;

#if defined(_WIN32)
    // Phase 29: run under the Service Control Manager. Windows only, because
    // systemd needs nothing of the process -- it sends SIGTERM and reads
    // stderr like any console. Refused without --config: the default path is
    // the service account's profile, not the installer's.
    bool service = false;
#endif
};

enum class CommandLineError
{
    UnknownOption,
    MissingValue,
    InvalidValue,
    MissingOption
};

const char* errorName(CommandLineError error);

Result<CommandLineOptions, CommandLineError> parseCommandLine(int argc, const char* const* argv);

std::string commandLineUsage();

} // namespace multiroom
