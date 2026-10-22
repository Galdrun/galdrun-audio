#include "app/CommandLine.h"

#include <string_view>

namespace multiroom {

const char* errorName(CommandLineError error)
{
    switch (error)
    {
    case CommandLineError::UnknownOption:
        return "UnknownOption";
    case CommandLineError::MissingValue:
        return "MissingValue";
    case CommandLineError::InvalidValue:
        return "InvalidValue";
    case CommandLineError::MissingOption:
        return "MissingOption";
    }
    return "?";
}

Result<CommandLineOptions, CommandLineError> parseCommandLine(int argc, const char* const* argv)
{
    CommandLineOptions options;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument(argv[i] != nullptr ? argv[i] : "");

        // A value-taking option consumes the next argument, and running off
        // the end is an error rather than an empty value -- "--config" with
        // nothing after it almost always means a quoting mistake.
        const auto nextValue = [&](std::string& out) -> Status<CommandLineError> {
            if (i + 1 >= argc)
                return fail(CommandLineError::MissingValue, std::string(argument));
            out = argv[++i];
            return {};
        };

        if (argument == "--help" || argument == "-h")
        {
            options.showHelp = true;
        }
        else if (argument == "--version")
        {
            options.showVersion = true;
        }
        else if (argument == "--config")
        {
            std::string value;
            if (auto taken = nextValue(value); !taken)
                return std::unexpected(taken.error());
            options.configPath = value;
        }
        else if (argument == "--log-level")
        {
            std::string value;
            if (auto taken = nextValue(value); !taken)
                return std::unexpected(taken.error());
            LogLevel level = LogLevel::Info;
            if (!log::parseLevel(value, level))
            {
                return fail(CommandLineError::InvalidValue,
                            "--log-level " + value + ", expected error|warn|info|debug|trace");
            }
            options.logLevel = level;
        }
        else if (argument == "--log-file")
        {
            std::string value;
            if (auto taken = nextValue(value); !taken)
                return std::unexpected(taken.error());
            options.logFile = value;
        }
#if defined(_WIN32)
        else if (argument == "--service")
        {
            options.service = true;
        }
#endif
        else
        {
            return fail(CommandLineError::UnknownOption, std::string(argument));
        }
    }

#if defined(_WIN32)
    if (options.service && options.configPath.empty())
        return fail(CommandLineError::MissingOption,
                    "--service needs --config <path>: a service's default config directory "
                    "is its account's, not the installer's");
#endif

    return options;
}

std::string commandLineUsage()
{
    return "Usage: " MULTIROOM_BRAND_SLUG "-core [options]\n"
           "\n"
           "  --config <path>       Configuration file (default: the user config directory)\n"
           "  --log-level <level>   error | warn | info | debug | trace\n"
           "  --log-file <path>     Append log output to this file as well as stderr\n"
#if defined(_WIN32)
           "  --service             Run under the Service Control Manager (needs --config)\n"
#endif
           "  --version             Print the version and exit\n"
           "  -h, --help            Print this help and exit\n";
}

} // namespace multiroom
