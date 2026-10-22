#include "util/Logger.h"

#include "util/FileUtil.h"
#include "util/Time.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace multiroom {
namespace log {
namespace {

std::atomic<int> g_level(static_cast<int>(LogLevel::Info));

std::mutex& sinkMutex()
{
    static std::mutex m;
    return m;
}

// Guarded by sinkMutex().
std::FILE* g_file = nullptr;
std::string g_path;
long g_maxBytes = kMaxLogFileBytes;
long g_bytes = 0;

// Under sinkMutex(). Opens g_path for appending and counts what it holds.
bool openLogFile()
{
    g_file = fileutil::openFile(g_path, "a");
    if (g_file == nullptr)
        return false;
    std::fseek(g_file, 0, SEEK_END);
    g_bytes = std::ftell(g_file);
    if (g_bytes < 0)
        g_bytes = 0;
    return true;
}

const char* shortFileName(const char* path)
{
    if (path == nullptr)
        return "";
    const char* last = path;
    for (const char* p = path; *p != '\0'; ++p)
    {
        if (*p == '/' || *p == '\\')
            last = p + 1;
    }
    return last;
}

} // namespace

void setLevel(LogLevel level)
{
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel currentLevel()
{
    return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed));
}

bool enabled(LogLevel level)
{
    return static_cast<int>(level) <= g_level.load(std::memory_order_relaxed);
}

bool setLogFile(const std::string& path, long maxBytes)
{
    std::lock_guard<std::mutex> lock(sinkMutex());
    if (g_file != nullptr)
    {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_path = path;
    g_maxBytes = maxBytes;
    if (path.empty())
        return true;
    return openLogFile();
}

void closeLogFile()
{
    setLogFile(std::string());
}

bool parseLevel(const std::string& text, LogLevel& out)
{
    if (text == "error" || text == "ERROR")
        out = LogLevel::Error;
    else if (text == "warn" || text == "WARN")
        out = LogLevel::Warn;
    else if (text == "info" || text == "INFO")
        out = LogLevel::Info;
    else if (text == "debug" || text == "DEBUG")
        out = LogLevel::Debug;
    else if (text == "trace" || text == "TRACE")
        out = LogLevel::Trace;
    else
        return false;
    return true;
}

const char* levelName(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Trace:
        return "TRACE";
    }
    return "?";
}

void write(LogLevel level, const char* file, int line, const char* format, ...)
{
    // Fixed buffer: no heap traffic per log line, and a bounded worst case on
    // small targets. Long messages are truncated rather than reallocated.
    char message[1024];
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (written < 0)
        std::strcpy(message, "<format error>");

    const std::string timestamp = localTimeOfDay();
    const char* const name = levelName(level);

    std::lock_guard<std::mutex> lock(sinkMutex());
    if (level <= LogLevel::Debug)
    {
        std::fprintf(stderr, "%s %-5s %s\n", timestamp.c_str(), name, message);
    }
    else
    {
        std::fprintf(stderr, "%s %-5s [%s:%d] %s\n", timestamp.c_str(), name,
                     shortFileName(file), line, message);
    }
    std::fflush(stderr);

    if (g_file != nullptr)
    {
        const int bytes = std::fprintf(g_file, "%s %-5s [%s:%d] %s\n", timestamp.c_str(),
                                       name, shortFileName(file), line, message);
        std::fflush(g_file);
        if (bytes > 0)
            g_bytes += bytes;
        if (g_bytes >= g_maxBytes)
        {
            std::fclose(g_file);
            g_file = nullptr;
            // A failed rotation must not turn a bounded log into an unbounded
            // one, especially on RAM filesystems. stderr remains available.
            if (!fileutil::renameFile(g_path, g_path + ".1"))
                std::fprintf(stderr, "%s ERROR cannot rotate the log file %s; file logging disabled\n",
                             timestamp.c_str(), g_path.c_str());
            else if (!openLogFile())
                std::fprintf(stderr, "%s ERROR cannot reopen the log file %s\n",
                             timestamp.c_str(), g_path.c_str());
        }
    }
}

} // namespace log
} // namespace multiroom
