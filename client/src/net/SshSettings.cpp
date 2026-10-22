#include "net/SshSettings.h"
#include "config/BoardConfig.h"
#include "util/FileUtil.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace multiroom::ssh_settings {
namespace {
constexpr std::size_t kMinPassword = 8;
constexpr std::size_t kMaxPassword = 128;
// A wrong current password locks the next attempt out this long: the page has
// no login of its own, so it must not be a fast way to guess the password.
constexpr auto kRetryDelay = std::chrono::seconds(3);
std::chrono::steady_clock::time_point retryAfter;

bool passwordSet(const std::string& shadow, std::string& error)
{
    std::string text;
    if (!fileutil::readTextFile(shadow, text, error)) return false;
    std::size_t line = 0;
    while (line < text.size())
    {
        const auto end = text.find('\n', line);
        const auto entry = text.substr(line, end == std::string::npos ? std::string::npos : end - line);
        if (entry.compare(0, 5, "root:") == 0)
        {
            const auto hash = entry.substr(5, entry.find(':', 5) - 5);
            return !hash.empty() && hash[0] != '!' && hash[0] != '*';
        }
        if (end == std::string::npos) break;
        line = end + 1;
    }
    error = "no root entry in " + shadow;
    return false;
}

bool enabled(const std::string& config)
{
    std::string text, error;
    return fileutil::exists(config) && fileutil::readTextFile(config, text, error) &&
           text.find("SSH_ENABLED=1\n") != std::string::npos;
}

bool running(const std::string& pidFile)
{
    std::string text, error;
    if (!fileutil::exists(pidFile) || !fileutil::readTextFile(pidFile, text, error)) return false;
    const long pid = std::strtol(text.c_str(), nullptr, 10);
    return pid > 0 && fileutil::exists("/proc/" + std::to_string(pid));
}

bool validPassword(const json::Value* value, std::size_t minimum)
{
    if (value == nullptr) return true;
    if (!value->isString()) return false;
    const auto& text = value->asString();
    return text.size() >= minimum && text.size() <= kMaxPassword &&
           text.find_first_of(std::string("\r\n\0", 3)) == std::string::npos;
}

// Runs the helper with `input` on stdin; its stderr line is the error.
bool runHelper(const std::string& helper, const std::string& input, std::string& error)
{
    int in[2], out[2];
    if (::pipe2(in, O_CLOEXEC) != 0) { error = std::string("pipe: ") + std::strerror(errno); return false; }
    if (::pipe2(out, O_CLOEXEC) != 0)
    {
        error = std::string("pipe: ") + std::strerror(errno);
        ::close(in[0]); ::close(in[1]);
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_adddup2(&actions, out[1], 2);
    pid_t child = 0;
    char* const arguments[] = {const_cast<char*>(helper.c_str()), nullptr};
    const int spawned = ::posix_spawn(&child, helper.c_str(), &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(in[0]);
    ::close(out[1]);
    if (spawned != 0)
    {
        ::close(in[1]); ::close(out[0]);
        error = "cannot start " + helper + ": " + std::strerror(spawned);
        return false;
    }
    // Three short lines fit the pipe buffer, so writing first cannot block.
    const bool written = ::write(in[1], input.data(), input.size()) == static_cast<ssize_t>(input.size());
    ::close(in[1]);
    std::string output;
    char buffer[256];
    for (ssize_t n; (n = ::read(out[0], buffer, sizeof buffer)) != 0;)
    {
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) break;
        output.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(out[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (written && WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
    while (!output.empty() && (output.back() == '\n' || output.back() == ' ')) output.pop_back();
    error = output.empty() ? "the SSH setting could not be changed" : output;
    return false;
}
} // namespace

bool supported()
{
    return board_config::enabled(board_config::current(), "sshSettings");
}

bool supported(const std::string& boardPath)
{
    return board_config::enabled(board_config::read(boardPath), "sshSettings");
}

bool load(json::Value& settings, std::string& error, const std::string& shadow,
          const std::string& config, const std::string& pidFile)
{
    error.clear();
    const bool hasPassword = passwordSet(shadow, error);
    if (!error.empty()) return false;
    settings = json::Value::makeObject();
    settings["user"] = json::Value("root");
    settings["enabled"] = json::Value(enabled(config));
    settings["passwordSet"] = json::Value(hasPassword);
    settings["running"] = json::Value(running(pidFile));
    return true;
}

bool validate(const json::Value& changes, std::string& error)
{
    const auto* on = changes.isObject() ? changes.find("enabled") : nullptr;
    if (on == nullptr || !on->isBool())
    {
        error = "SSH setting needs enabled: true or false";
        return false;
    }
    for (const auto& member : changes.members())
    {
        if (member.first != "enabled" && member.first != "currentPassword" && member.first != "password")
        {
            error = "unknown SSH setting: " + member.first;
            return false;
        }
    }
    if (!validPassword(changes.find("currentPassword"), 0))
    {
        error = "invalid current password";
        return false;
    }
    if (!validPassword(changes.find("password"), kMinPassword))
    {
        error = "the new password needs 8 to 128 characters and no line breaks";
        return false;
    }
    return true;
}

bool save(const json::Value& changes, json::Value& settings, std::string& error,
          const std::string& helper, const std::string& shadow,
          const std::string& config, const std::string& pidFile)
{
    error.clear();
    if (!validate(changes, error)) return false;
    if (std::chrono::steady_clock::now() < retryAfter)
    {
        error = "wait a few seconds before trying again";
        return false;
    }
    const auto text = [&](const char* name) {
        const auto* value = changes.find(name);
        return value == nullptr ? std::string() : value->asString();
    };
    const std::string input = std::string(changes.find("enabled")->asBool() ? "1" : "0") + "\n" +
                              text("currentPassword") + "\n" + text("password") + "\n";
    if (!runHelper(helper, input, error))
    {
        if (error.find("password is wrong") != std::string::npos)
            retryAfter = std::chrono::steady_clock::now() + kRetryDelay;
        return false;
    }
    return load(settings, error, shadow, config, pidFile);
}

} // namespace multiroom::ssh_settings
