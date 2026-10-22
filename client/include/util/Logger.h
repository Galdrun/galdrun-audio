#pragma once

#include <cstdarg>
#include <string>

namespace multiroom {

enum class LogLevel
{
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3,
    Trace = 4
};

// Process-wide logger. Writes to stderr and, if configured, to a file.
//
// Not for use from the audio thread: it formats, locks a mutex and may block
// on I/O. Real-time code reports through atomic counters or a preallocated
// event buffer instead (see the audio engine, phase 3).
namespace log {

void setLevel(LogLevel level);
LogLevel currentLevel();

// Reading the level is a relaxed atomic load, so log macros can cheaply skip
// formatting for disabled levels.
bool enabled(LogLevel level);

// A file that reaches this size is moved to "<path>.1", replacing the previous
// one, and a new file is started: roughly twice this on disk, which matters on
// a board whose /tmp or /var/log is RAM.
// Rotation failure disables file output until setLogFile() is called again;
// stderr remains available. The size limit can be exceeded by one log line.
const long kMaxLogFileBytes = 1024 * 1024;

// Empty path disables file output. Returns false if the file cannot be opened;
// stderr output keeps working either way.
bool setLogFile(const std::string& path, long maxBytes = kMaxLogFileBytes);
void closeLogFile();

bool parseLevel(const std::string& text, LogLevel& out);
const char* levelName(LogLevel level);

void write(LogLevel level, const char* file, int line, const char* format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

} // namespace log
} // namespace multiroom

#define MULTIROOM_LOG(level, ...)                                                  \
    do                                                                         \
    {                                                                          \
        if (::multiroom::log::enabled(level))                                     \
            ::multiroom::log::write((level), __FILE__, __LINE__, __VA_ARGS__);    \
    } while (false)

#define MULTIROOM_ERROR(...) MULTIROOM_LOG(::multiroom::LogLevel::Error, __VA_ARGS__)
#define MULTIROOM_WARN(...) MULTIROOM_LOG(::multiroom::LogLevel::Warn, __VA_ARGS__)
#define MULTIROOM_INFO(...) MULTIROOM_LOG(::multiroom::LogLevel::Info, __VA_ARGS__)
#define MULTIROOM_DEBUG(...) MULTIROOM_LOG(::multiroom::LogLevel::Debug, __VA_ARGS__)
#define MULTIROOM_TRACE(...) MULTIROOM_LOG(::multiroom::LogLevel::Trace, __VA_ARGS__)
