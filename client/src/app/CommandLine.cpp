#include "app/CommandLine.h"

#include <cstdlib>

namespace multiroom {
namespace {

// Matches "--name" exactly, or "--name=value" and returns the value part.
bool matchOption(const std::string& argument, const char* name, bool& hasInlineValue,
                 std::string& inlineValue)
{
    const std::string prefix = std::string("--") + name;
    if (argument == prefix)
    {
        hasInlineValue = false;
        inlineValue.clear();
        return true;
    }
    if (argument.size() > prefix.size() && argument.compare(0, prefix.size(), prefix) == 0 &&
        argument[prefix.size()] == '=')
    {
        hasInlineValue = true;
        inlineValue = argument.substr(prefix.size() + 1);
        return true;
    }
    return false;
}

// A TCP port the client may bind: 0 is "let the OS choose" everywhere else in
// this code base, and a control API on a port nobody can predict is one nobody
// can reach.
bool parsePort(const std::string& value, uint16_t& out)
{
    char* end = nullptr;
    const long port = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || port <= 0 || port > 65535)
        return false;
    out = static_cast<uint16_t>(port);
    return true;
}

bool parseTestToneSeconds(const std::string& value, uint32_t& out)
{
    const long seconds = std::strtol(value.c_str(), nullptr, 10);
    if (seconds <= 0 || seconds > 86400)
        return false;
    out = static_cast<uint32_t>(seconds);
    return true;
}

} // namespace

bool parseCommandLine(
int argc, const char* const argv[], CommandLineOptions& out,
                      std::string& error)
{
    CommandLineOptions options;

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i] != nullptr ? argv[i] : "";
        bool hasInlineValue = false;
        std::string inlineValue;

        // Takes the value from "--name=value" or from the following argument.
        auto readValue = [&](std::string& value) -> bool {
            if (hasInlineValue)
            {
                if (inlineValue.empty())
                {
                    error = argument + " needs a value";
                    return false;
                }
                value = inlineValue;
                return true;
            }
            if (i + 1 >= argc || argv[i + 1] == nullptr)
            {
                error = argument + " needs a value";
                return false;
            }
            value = argv[++i];
            return true;
        };

        if (argument == "--help" || argument == "-h")
        {
            options.helpRequested = true;
        }
        else if (argument == "--version" || argument == "-v")
        {
            options.versionRequested = true;
        }
        else if (matchOption(argument, "rescan-audio", hasInlineValue, inlineValue))
        {
            if (hasInlineValue)
            {
                error = "--rescan-audio takes no value";
                return false;
            }
            options.rescanAudio = true;
        }
        else if (matchOption(argument, "server", hasInlineValue, inlineValue))
        {
            std::string value;
            if (!readValue(value))
                return false;
            std::string host;
            uint16_t port = 0;
            if (!parseServerAddress(value, host, port))
            {
                error = "invalid server address: " + value;
                return false;
            }
            options.hasServer = true;
            options.server = value;
        }
        else if (matchOption(argument, "audio-device", hasInlineValue, inlineValue))
        {
            std::string value;
            if (!readValue(value))
                return false;
            options.hasAudioDevice = true;
            options.audioDevice = value;
        }
        else if (matchOption(argument, "audio-mode", hasInlineValue, inlineValue))
        {
            std::string value;
            if (!readValue(value))
                return false;
            if (!parseAudioAccessPreference(value, options.audioAccessMode))
            {
                error = "invalid audio mode: " + value +
                        " (expected exclusive, exclusive-only or shared)";
                return false;
            }
            options.hasAudioAccessMode = true;
        }
        else if (matchOption(argument, "test-tone", hasInlineValue, inlineValue))
        {
            std::string value;
            if (!readValue(value))
                return false;
            if (!parseTestToneSeconds(value, options.testToneSeconds))
            {
                error = "invalid test tone duration: " + value;
                return false;
            }
            options.testToneBuffered = false;
        }
        else if (matchOption(argument, "test-tone-buffered", hasInlineValue, inlineValue))
        {
            std::string value;
            if (!readValue(value))
                return false;
            if (!parseTestToneSeconds(value, options.testToneSeconds))
            {
                error = "invalid test tone duration: " + value;
                return false;
            }
            options.testToneBuffered = true;
        }
        else if (matchOption(argument, "audio-cache", hasInlineValue, inlineValue))
        {
            if (!readValue(options.audioCachePath))
                return false;
        }
        else if (matchOption(argument, "config", hasInlineValue, inlineValue))
        {
            if (!readValue(options.configPath))
                return false;
        }
        else if (matchOption(argument, "control-port", hasInlineValue, inlineValue))
        {
            std::string value;
            if (!readValue(value))
                return false;
            if (!parsePort(value, options.controlPort))
            {
                error = "invalid control port: " + value;
                return false;
            }
            options.hasControlPort = true;
        }
        else if (matchOption(argument, "log-file", hasInlineValue, inlineValue))
        {
            if (!readValue(options.logFile))
                return false;
            options.hasLogFile = true;
        }
        else if (matchOption(argument, "log-level", hasInlineValue, inlineValue))
        {
            std::string value;
            if (!readValue(value))
                return false;
            if (!log::parseLevel(value, options.logLevel))
            {
                error = "invalid log level: " + value;
                return false;
            }
            options.hasLogLevel = true;
        }
        else
        {
            error = "unknown option: " + argument;
            return false;
        }
    }

    out = options;
    return true;
}

void applyCommandLine(const CommandLineOptions& options, ClientConfig& config)
{
    if (options.hasServer)
        config.server = options.server;
    if (options.hasAudioDevice)
        config.audioDeviceId = options.audioDevice;
    if (options.hasAudioAccessMode)
        config.audioAccessMode = options.audioAccessMode;
    if (options.hasControlPort)
        config.controlPort = options.controlPort;
    if (options.hasLogLevel)
        config.logLevel = options.logLevel;
}

namespace {

template <typename T>
void keepStored(bool& overridden, T& field, const T& override, const T& stored)
{
    if (!overridden)
        return;
    if (field == override)
        field = stored;
    else
        overridden = false;
}

} // namespace

ClientConfig configToSave(CommandLineOptions& options, const ClientConfig& stored,
                          ClientConfig config)
{
    keepStored(options.hasServer, config.server, options.server, stored.server);
    keepStored(options.hasAudioDevice, config.audioDeviceId, options.audioDevice,
               stored.audioDeviceId);
    keepStored(options.hasAudioAccessMode, config.audioAccessMode, options.audioAccessMode,
               stored.audioAccessMode);
    keepStored(options.hasControlPort, config.controlPort, options.controlPort,
               stored.controlPort);
    keepStored(options.hasLogLevel, config.logLevel, options.logLevel, stored.logLevel);
    return config;
}

const char* usageText()
{
    return "usage: " MULTIROOM_BRAND_SLUG "-client [options]\n"
           "\n"
           "  --server <host:port>     audio server to connect to\n"
           "  --audio-device <id>      playback device id to use\n"
           "  --audio-mode <mode>      exclusive (default) | exclusive-only | shared\n"
           "  --rescan-audio           rescan audio hardware instead of using the cache\n"
           "  --test-tone <seconds>    play a test tone on the selected device\n"
           "  --test-tone-buffered <s> the same tone, produced by a second thread\n"
           "                           and played through the ring buffer\n"
           "  --audio-cache <path>     path to audio_devices.json\n"
           "  --config <path>          path to client.json\n"
           "  --control-port <port>    port the control API listens on\n"
           "  --log-level <level>      error | warn | info | debug | trace\n"
           "  --log-file <path>        append the log to this file as well as stderr\n"
           "  --version                print version and exit\n"
           "  --help                   print this help and exit\n"
           "\n"
           "Command-line values override client.json for this run only.\n";
}

} // namespace multiroom
