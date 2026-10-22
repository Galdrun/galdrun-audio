#pragma once

// Process-wide logger. Writes to stderr and, if configured, to a file.
//
// Formatting is std::format rather than the client's printf-style vararg call.
// The client has no choice -- C++17 and an ARMv6 toolchain -- and pays for it
// with a format attribute that only GCC honours, so on MSVC a mismatched
// argument is a runtime crash. This server must not terminate, whatever a
// module gets wrong, so the check happens at compile time.
//
// The log line is formatted only if its level is enabled, because the macros
// test first and the formatting lives inside them.
//
// This is a control-path facility. Nothing on a per-client output thread's
// inner loop logs per packet: it locks a mutex and writes to a file, and a
// blocked disk would become a blocked room.

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace multiroom {

enum class LogLevel
{
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3,
    Trace = 4
};

namespace log {

void setLevel(LogLevel level);
LogLevel currentLevel();

// A relaxed atomic load, so the macros below skip disabled levels cheaply.
bool enabled(LogLevel level);

// An empty path disables file output. Returns false if the file cannot be
// opened; stderr output keeps working either way.
bool setLogFile(const std::string& path);
void closeLogFile();

// Phase 29's rotation. A line that would take the file past `bytes` first
// renames it to `<path>.1`, replacing an older one, and opens a new file, so
// the disk holds at most twice the limit and each file stays under it (a
// single line longer than the limit is written whole, alone in its file).
// Done under the lock every line takes, so no line is split between two
// files. Zero, the default, never rotates; stderr never rotates.
void setMaxFileBytes(uint64_t bytes);

bool parseLevel(std::string_view text, LogLevel& out);
const char* levelName(LogLevel level);

// The already-formatted line. Called by writeFormatted and by tests.
void write(LogLevel level, const char* file, int line, std::string_view message);

// One recurring condition logged at most once per interval (phase 40): a room
// on a bad link is stranded and moved every few hundred milliseconds, and one
// line for each buried everything else in the log. Not synchronised; the
// caller's lock guards it with the state it describes.
class RateLimit
{
public:
    static constexpr uint64_t kIntervalNs = 10ULL * 1000000000ULL;

    // True when this occurrence is the one to log, and then `suppressed` is
    // how many were not since the last one that was.
    bool allow(uint64_t nowNs, uint64_t& suppressed)
    {
        if (logged_ && nowNs - lastNs_ < kIntervalNs)
        {
            ++suppressed_;
            return false;
        }
        logged_ = true;
        lastNs_ = nowNs;
        suppressed = std::exchange(suppressed_, 0);
        return true;
    }

private:
    uint64_t lastNs_ = 0;
    uint64_t suppressed_ = 0;
    bool logged_ = false;
};

template <typename... Args>
void writeFormatted(LogLevel level, const char* file, int line,
                    std::format_string<Args...> format, Args&&... args)
{
    // A format that throws is a bug in the call site, not a reason to lose the
    // line or take the process down with it.
    try
    {
        write(level, file, line, std::format(format, std::forward<Args>(args)...));
    }
    catch (const std::exception& e)
    {
        write(LogLevel::Error, file, line, std::string("<format error: ") + e.what() + ">");
    }
}

} // namespace log
} // namespace multiroom

#define MULTIROOM_LOG(level, ...)                                                                   \
    do                                                                                        \
    {                                                                                         \
        if (::multiroom::log::enabled(level))                                                    \
            ::multiroom::log::writeFormatted((level), __FILE__, __LINE__, __VA_ARGS__);          \
    } while (false)

#define MULTIROOM_ERROR(...) MULTIROOM_LOG(::multiroom::LogLevel::Error, __VA_ARGS__)
#define MULTIROOM_WARN(...) MULTIROOM_LOG(::multiroom::LogLevel::Warn, __VA_ARGS__)
#define MULTIROOM_INFO(...) MULTIROOM_LOG(::multiroom::LogLevel::Info, __VA_ARGS__)
#define MULTIROOM_DEBUG(...) MULTIROOM_LOG(::multiroom::LogLevel::Debug, __VA_ARGS__)
#define MULTIROOM_TRACE(...) MULTIROOM_LOG(::multiroom::LogLevel::Trace, __VA_ARGS__)
