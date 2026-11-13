#include "app/SelfUpdate.h"

#include "util/FileUtil.h"

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__) && !defined(ESP_PLATFORM)
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <vector>
#endif

namespace multiroom {
namespace self_update {
namespace {

#if defined(_WIN32)
const char* const kScript = "galdrun-update.ps1";
const char* const kPackage = "package.zip";
#else
const char* const kScript = "galdrun-update.sh";
const char* const kPackage = "package.tar.gz";
#endif

#if !defined(ESP_PLATFORM)
std::string installDirectory()
{
#if defined(_WIN32)
    wchar_t path[MAX_PATH];
    const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    return fileutil::directoryOf(fileutil::fromWide(path));
#else
    char path[PATH_MAX];
    const ssize_t length = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0)
        return {};
    path[length] = '\0';
    return fileutil::directoryOf(path);
#endif
}

std::string workDirectory()
{
    return fileutil::joinPath(installDirectory(), ".update");
}

std::string trialPath()
{
    return fileutil::joinPath(workDirectory(), "trial");
}

std::string scriptPath()
{
    return fileutil::joinPath(installDirectory(), kScript);
}

// The --config the trial file named, kept for deferTrial.
std::string& trialOwner()
{
    static std::string owner;
    return owner;
}
#endif

#if defined(_WIN32)
std::string base64(const std::string& text)
{
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < text.size(); i += 3)
    {
        const size_t left = text.size() - i;
        const unsigned value = (static_cast<unsigned char>(text[i]) << 16) |
                               (left > 1 ? static_cast<unsigned char>(text[i + 1]) << 8 : 0) |
                               (left > 2 ? static_cast<unsigned char>(text[i + 2]) : 0);
        out += kAlphabet[(value >> 18) & 63];
        out += kAlphabet[(value >> 12) & 63];
        out += left > 1 ? kAlphabet[(value >> 6) & 63] : '=';
        out += left > 2 ? kAlphabet[value & 63] : '=';
    }
    return out;
}

// This process's command line after the program name, quoted as it came, so
// the updater can start the next client with exactly the same arguments.
std::string argumentTail()
{
    const wchar_t* line = ::GetCommandLineW();
    // The program name ends at the closing quote when quoted, else at a blank
    // (the rule CommandLineToArgvW applies to it).
    if (*line == L'"')
    {
        ++line;
        while (*line != L'\0' && *line != L'"')
            ++line;
        if (*line == L'"')
            ++line;
    }
    else
    {
        while (*line != L'\0' && *line != L' ' && *line != L'\t')
            ++line;
    }
    while (*line == L' ' || *line == L'\t')
        ++line;
    return fileutil::fromWide(line);
}
#endif

} // namespace

const char* releaseTarget()
{
#if defined(ESP_PLATFORM)
    return "";
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "windows-x64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x64";
#elif defined(__linux__) && defined(__i386__)
    return "linux-x86";
#elif defined(__linux__) && defined(__aarch64__) && defined(__ARM_ARCH) && __ARM_ARCH >= 9
    return "linux-armv9";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-armv8";
#elif defined(__linux__) && defined(__ARM_ARCH) && __ARM_ARCH == 7
    return "linux-armv7";
#elif defined(__linux__) && defined(__ARM_ARCH) && __ARM_ARCH == 6
    return "linux-armv6";
#else
    return "";
#endif
}

std::string unsupportedReason()
{
#if defined(ESP_PLATFORM)
    return "ESP32 boards are updated by flashing them";
#else
    if (*releaseTarget() == '\0')
        return "there is no release for this architecture";
    const std::string directory = installDirectory();
    if (directory.empty())
        return "the client's own folder is unknown";
    if (!fileutil::exists(scriptPath()))
        return std::string("this install has no updater (") + kScript + "); update it once by hand";
#if defined(_WIN32)
    if (!fileutil::createDirectories(workDirectory()))
        return "the client's folder is not writable";
    const std::string probe = fileutil::joinPath(workDirectory(), "write-test");
    std::FILE* file = fileutil::openFile(probe, "wb");
    if (file == nullptr)
        return "the client's folder is not writable";
    std::fclose(file);
    fileutil::removeFile(probe);
#else
    if (::access(directory.c_str(), W_OK) != 0)
        return "the client's folder is not writable";
    struct statfs info;
    // A RAM file system forgets the update at the next boot: such an image is
    // updated as a whole (Connector 3).
    if (::statfs(directory.c_str(), &info) == 0 &&
        (info.f_type == 0x01021994 /* tmpfs */ || info.f_type == static_cast<decltype(info.f_type)>(0x858458f6) /* ramfs */))
        return "the client runs from RAM; update the firmware image instead";
#endif
    return {};
#endif
}

std::string packagePath()
{
#if defined(ESP_PLATFORM)
    return {};
#else
    return fileutil::joinPath(workDirectory(), kPackage);
#endif
}

bool writePackage(const char* data, size_t size, std::string& error)
{
#if defined(ESP_PLATFORM)
    (void)data;
    (void)size;
    error = "unsupported";
    return false;
#else
    if (!fileutil::createDirectories(workDirectory()))
    {
        error = "cannot create " + workDirectory();
        return false;
    }
    const std::string target = packagePath();
    const std::string part = target + ".part";
    std::FILE* file = fileutil::openFile(part, "wb");
    if (file == nullptr)
    {
        error = "cannot write " + part;
        return false;
    }
    const bool written = std::fwrite(data, 1, size, file) == size;
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed || !fileutil::renameFile(part, target))
    {
        fileutil::removeFile(part);
        error = "cannot write " + target;
        return false;
    }
    return true;
#endif
}

Trial beginTrial(const std::string& configPath)
{
#if defined(ESP_PLATFORM)
    (void)configPath;
    return Trial::None;
#else
    // "pending <config>" or "running <config>"; no config names any client.
    std::string text;
    std::string error;
    if (!fileutil::readTextFile(trialPath(), text, error))
        return Trial::None;
    const size_t lineEnd = text.find_first_of("\r\n");
    const std::string line = text.substr(0, lineEnd);
    const size_t space = line.find(' ');
    const std::string state = line.substr(0, space);
    const std::string owner = space == std::string::npos ? std::string() : line.substr(space + 1);
    if (!owner.empty() && owner != configPath)
        return Trial::None; // another client in this folder is on trial
    trialOwner() = owner;
    if (state == "pending" &&
        fileutil::writeTextFileAtomic(trialPath(), "running " + owner + "\n", error))
        return Trial::Running;
    return Trial::Failed;
#endif
}

bool trialPending()
{
#if defined(ESP_PLATFORM)
    return false;
#else
    return fileutil::exists(trialPath());
#endif
}

void confirmTrial()
{
#if !defined(ESP_PLATFORM)
    fileutil::removeFile(trialPath());
#endif
}

void deferTrial()
{
#if !defined(ESP_PLATFORM)
    std::string error;
    fileutil::writeTextFileAtomic(trialPath(), "pending " + trialOwner() + "\n", error);
#endif
}

bool handOver(const std::string& action, const std::string& sha256,
              const std::string& configPath, int argc, char** argv, std::string& error)
{
#if defined(ESP_PLATFORM)
    (void)action;
    (void)sha256;
    (void)configPath;
    (void)argc;
    (void)argv;
    error = "unsupported";
    return false;
#elif defined(_WIN32)
    (void)argc;
    (void)argv;
    wchar_t systemRoot[MAX_PATH];
    const UINT rootLength = ::GetSystemDirectoryW(systemRoot, MAX_PATH);
    if (rootLength == 0 || rootLength >= MAX_PATH)
    {
        error = "cannot find the system folder";
        return false;
    }
    const std::string powershell =
        fileutil::fromWide(systemRoot) + "\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::string command = "\"" + powershell + "\" -NoProfile -NonInteractive -ExecutionPolicy Bypass"
                          " -WindowStyle Hidden -File \"" + scriptPath() + "\" -Action " + action +
                          " -ClientPid " + std::to_string(::GetCurrentProcessId());
    if (!sha256.empty())
        command += " -Sha256 " + sha256;
    // Base64: the updater writes these bytes back unchanged as the owner.
    if (!configPath.empty())
        command += " -Owner " + base64(configPath);
    const std::string tail = argumentTail();
    if (!tail.empty())
        command += " -Arguments " + base64(tail);

    std::wstring wide = fileutil::toWide(command);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(nullptr, &wide[0], nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr,
                          &startup, &process))
    {
        error = "cannot start the updater (error " + std::to_string(::GetLastError()) + ")";
        return false;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
#else
    const std::string script = scriptPath();
    std::vector<char*> args;
    args.push_back(const_cast<char*>("sh"));
    args.push_back(const_cast<char*>(script.c_str()));
    args.push_back(const_cast<char*>(action.c_str()));
    if (!sha256.empty())
        args.push_back(const_cast<char*>(sha256.c_str()));
    for (int i = 1; i < argc; ++i)
        args.push_back(argv[i]);
    args.push_back(nullptr);
    // The trial's owner, apart from the client arguments: the script does not
    // have to parse those (--config <path> and --config=<path>).
    ::setenv("GALDRUN_TRIAL_OWNER", configPath.c_str(), 1);
    ::execv("/bin/sh", args.data());
    ::unsetenv("GALDRUN_TRIAL_OWNER");
    error = std::string("cannot run the updater: ") + std::strerror(errno);
    return false;
#endif
}

} // namespace self_update
} // namespace multiroom
