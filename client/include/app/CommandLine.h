#pragma once

#include "config/ClientConfig.h"

#include <string>

namespace multiroom {

// Parsed command line. Values here override the config file in memory; they
// are not written back to disk.
struct CommandLineOptions
{
    bool helpRequested = false;
    bool versionRequested = false;

    bool rescanAudio = false;

    std::string configPath;     // --config, empty = default location
    std::string audioCachePath; // --audio-cache, empty = default location

    bool hasServer = false;
    std::string server;

    bool hasAudioDevice = false;
    std::string audioDevice;

    bool hasAudioAccessMode = false;
    AudioAccessPreference audioAccessMode = AudioAccessPreference::PreferExclusive;

    bool hasControlPort = false;
    uint16_t controlPort = 0;

    bool hasLogLevel = false;
    LogLevel logLevel = LogLevel::Info;

    // Where to append a copy of the log, beside stderr. Not a config field:
    // the two things that run this client without a terminal -- a systemd unit
    // and a Windows service -- are both a command line, and a path persisted
    // in client.json would keep writing to a file nobody asked for after the
    // run that wanted it.
    bool hasLogFile = false;
    std::string logFile;

    // Seconds of test tone to play before exiting; 0 means no playback. The
    // bring-up path until the server delivers audio.
    uint32_t testToneSeconds = 0;

    // Produce that tone on a second thread and let the audio thread read it
    // through the ring buffer -- the same path the server's audio takes.
    // Without it the tone is generated in the render callback itself.
    bool testToneBuffered = false;
};

// Accepts both "--option value" and "--option=value". On failure, error names
// the offending argument.
bool parseCommandLine(int argc, const char* const argv[], CommandLineOptions& out,
                      std::string& error);

void applyCommandLine(const CommandLineOptions& options, ClientConfig& config);

// The config as client.json should get it: a field still at its command-line
// value is written as `stored` (the file before the overrides) had it. A field
// changed since is the user's, so it is written and its override ends.
ClientConfig configToSave(CommandLineOptions& options, const ClientConfig& stored,
                          ClientConfig config);

const char* usageText();

} // namespace multiroom
