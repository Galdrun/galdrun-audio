#include "util/Logger.h"

#include "util/Time.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <system_error>

namespace multiroom {
namespace log {
namespace {

std::atomic<int> g_level{static_cast<int>(LogLevel::Warn)};

std::mutex& sinkMutex()
{
    static std::mutex m;
    return m;
}

// Guarded by sinkMutex(). The file is opened in binary mode so that the bytes
// counted are the bytes on disk: in text mode Windows would add a CR per line
// behind the count's back.
std::FILE* g_file = nullptr;
std::string g_path;
uint64_t g_fileBytes = 0;
uint64_t g_maxBytes = 0;

// The file as it stands becomes `<path>.1` and a new one is started. If the
// rename fails -- something holds the old `.1` open -- the file is started
// again in place instead, because a limit that holds only when nothing goes
// wrong is not a limit.
void rotateLocked()
{
    std::fclose(g_file);

    std::filesystem::path previous(g_path);
    previous += ".1";
    std::error_code error;
    // Replaces an existing `.1` on both platforms: POSIX rename() does, and so
    // does MSVC's, which is MoveFileEx with MOVEFILE_REPLACE_EXISTING.
    std::filesystem::rename(g_path, previous, error);

    g_file = std::fopen(g_path.c_str(), error ? "wb" : "ab");
    g_fileBytes = 0;
    if (error)
        std::fprintf(stderr, "log rotation could not rename %s (%s); started it again\n",
                     g_path.c_str(), error.message().c_str());
}

std::string_view shortFileName(const char* path)
{
    if (path == nullptr)
        return {};
    std::string_view view(path);
    const size_t slash = view.find_last_of("/\\");
    return slash == std::string_view::npos ? view : view.substr(slash + 1);
}

// A short, stable tag per thread. The server runs a pipeline thread per zone
// and an output thread per client, so a line without one is unattributable,
// and the platform thread id is too long to read in a column.
int threadTag()
{
    static std::atomic<int> next{0};
    static thread_local const int tag = next.fetch_add(1, std::memory_order_relaxed);
    return tag;
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

bool setLogFile(const std::string& path)
{
    const std::lock_guard<std::mutex> lock(sinkMutex());
    if (g_file != nullptr)
    {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_path = path;
    g_fileBytes = 0;
    if (path.empty())
        return true;
    g_file = std::fopen(path.c_str(), "ab");
    if (g_file == nullptr)
        return false;

    // Appended to, so it already counts against the limit.
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(path, error);
    g_fileBytes = error ? 0 : static_cast<uint64_t>(size);
    return true;
}

void setMaxFileBytes(uint64_t bytes)
{
    const std::lock_guard<std::mutex> lock(sinkMutex());
    g_maxBytes = bytes;
}

void closeLogFile()
{
    setLogFile(std::string());
}

bool parseLevel(std::string_view text, LogLevel& out)
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

void write(LogLevel level, const char* file, int line, std::string_view message)
{
    const std::string timestamp = localTimeOfDay();
    const char* const name = levelName(level);
    const std::string_view source = shortFileName(file);
    const int thread = threadTag();

    const std::lock_guard<std::mutex> lock(sinkMutex());

    // Error, Warn and Info read as a narrative and carry no source location;
    // Debug and Trace are read next to the code that produced them.
    if (level <= LogLevel::Info)
    {
        std::fprintf(stderr, "%s %-5s t%-2d %.*s\n", timestamp.c_str(), name, thread,
                     static_cast<int>(message.size()), message.data());
    }
    else
    {
        std::fprintf(stderr, "%s %-5s t%-2d [%.*s:%d] %.*s\n", timestamp.c_str(), name, thread,
                     static_cast<int>(source.size()), source.data(), line,
                     static_cast<int>(message.size()), message.data());
    }
    std::fflush(stderr);

    if (g_file != nullptr)
    {
        const std::string text =
            std::format("{} {:<5} t{:<2} [{}:{}] {}\n", timestamp, name, thread, source, line,
                        message);
        if (g_maxBytes > 0 && g_fileBytes > 0 && g_fileBytes + text.size() > g_maxBytes)
            rotateLocked();
        if (g_file != nullptr)
        {
            // What reached the file, so that a full disk does not move where
            // rotation happens. The line still went to stderr above.
            g_fileBytes += std::fwrite(text.data(), 1, text.size(), g_file);
            std::fflush(g_file);
        }
    }
}

} // namespace log
} // namespace multiroom
