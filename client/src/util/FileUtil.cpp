#include "util/FileUtil.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace multiroom {
namespace fileutil {

#if defined(_WIN32)

std::wstring toWide(const std::string& utf8)
{
    if (utf8.empty())
        return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0)
        return std::wstring();
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                        &wide[0], needed);
    return wide;
}

std::string fromWide(const wchar_t* wide)
{
    if (wide == nullptr || *wide == L'\0')
        return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
        return std::string();
    std::string utf8(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &utf8[0], needed, nullptr, nullptr);
    return utf8;
}

#endif

namespace {

#if defined(_WIN32)

std::string lastErrorText()
{
    const DWORD code = GetLastError();
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "windows error %lu", static_cast<unsigned long>(code));
    return std::string(buffer);
}

#else

std::string errnoText()
{
    return std::string(std::strerror(errno));
}

#endif

bool isSeparator(char c)
{
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

std::string environmentValue(const char* name)
{
#if defined(_WIN32)
    // _wgetenv keeps non-ASCII profile paths intact.
    const std::wstring wideName = toWide(name);
    const wchar_t* value = _wgetenv(wideName.c_str());
    return value != nullptr ? fromWide(value) : std::string();
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

bool createSingleDirectory(const std::string& directory)
{
#if defined(_WIN32)
    const std::wstring wide = toWide(directory);
    if (CreateDirectoryW(wide.c_str(), nullptr))
        return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    if (::mkdir(directory.c_str(), 0755) == 0)
        return true;
    return errno == EEXIST;
#endif
}

#if !defined(_WIN32)
// Renaming is only durable once the directory entry itself has been flushed.
void syncDirectory(const std::string& directory)
{
    if (directory.empty())
        return;
    const int fd = ::open(directory.c_str(), O_RDONLY);
    if (fd < 0)
        return;
    ::fsync(fd);
    ::close(fd);
}
#endif

} // namespace

bool exists(const std::string& path)
{
    if (path.empty())
        return false;
#if defined(_WIN32)
    const std::wstring wide = toWide(path);
    return GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
#elif defined(ESP_PLATFORM)
    // newlib's access() does not reach the VFS, so every littlefs path would
    // "not exist"; stat() does go through it.
    struct stat info = {};
    return ::stat(path.c_str(), &info) == 0;
#else
    // No metadata is needed; stat can overflow a 32-bit inode on WSL mounts.
    return ::access(path.c_str(), F_OK) == 0;
#endif
}

std::FILE* openFile(const std::string& path, const char* mode)
{
#if defined(_WIN32)
    return _wfopen(toWide(path).c_str(), toWide(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool readTextFile(const std::string& path, std::string& out, std::string& error)
{
    if (path.empty())
    {
        error = "empty path";
        return false;
    }

    std::FILE* file = openFile(path, "rb");
    if (file == nullptr)
    {
        error = "cannot open file for reading";
        return false;
    }

    std::string content;
    char buffer[4096];
    for (;;)
    {
        const size_t bytesRead = std::fread(buffer, 1, sizeof(buffer), file);
        if (bytesRead > 0)
            content.append(buffer, bytesRead);
        if (bytesRead < sizeof(buffer))
            break;
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed)
    {
        error = "read error";
        return false;
    }
    out = std::move(content);
    return true;
}

bool writeTextFileAtomic(const std::string& path, const std::string& content,
                         std::string& error)
{
    if (path.empty())
    {
        error = "empty path";
        return false;
    }

    const std::string directory = directoryOf(path);
    if (!directory.empty() && !createDirectories(directory))
    {
        error = "cannot create directory " + directory;
        return false;
    }

    // Two writers of the same file would share this temporary name: one could
    // truncate it under the other, or rename it while the other still writes.
    // The ESP32 web UI saves client.json from its HTTP task while the client
    // saves it from its main thread, so every write here goes one at a time.
    static std::mutex writeMutex;
    std::lock_guard<std::mutex> lock(writeMutex);

    const std::string temporary = path + ".tmp";

#if defined(_WIN32)
    const std::wstring wideTemporary = toWide(temporary);
    HANDLE handle = CreateFileW(wideTemporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        error = "cannot create " + temporary + ": " + lastErrorText();
        return false;
    }

    size_t offset = 0;
    while (offset < content.size())
    {
        const DWORD chunk = static_cast<DWORD>(
            content.size() - offset > 0x10000 ? 0x10000 : content.size() - offset);
        DWORD written = 0;
        if (!WriteFile(handle, content.data() + offset, chunk, &written, nullptr) ||
            written == 0)
        {
            error = "write failed: " + lastErrorText();
            CloseHandle(handle);
            DeleteFileW(wideTemporary.c_str());
            return false;
        }
        offset += written;
    }

    if (!FlushFileBuffers(handle))
    {
        error = "flush failed: " + lastErrorText();
        CloseHandle(handle);
        DeleteFileW(wideTemporary.c_str());
        return false;
    }
    CloseHandle(handle);

    const std::wstring widePath = toWide(path);
    if (!MoveFileExW(wideTemporary.c_str(), widePath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error = "rename failed: " + lastErrorText();
        DeleteFileW(wideTemporary.c_str());
        return false;
    }
    return true;
#else
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        error = "cannot create " + temporary + ": " + errnoText();
        return false;
    }

    size_t offset = 0;
    while (offset < content.size())
    {
        const ssize_t written = ::write(fd, content.data() + offset, content.size() - offset);
        if (written < 0)
        {
            if (errno == EINTR)
                continue;
            error = "write failed: " + errnoText();
            ::close(fd);
            ::unlink(temporary.c_str());
            return false;
        }
        offset += static_cast<size_t>(written);
    }

    if (::fsync(fd) != 0)
    {
        error = "fsync failed: " + errnoText();
        ::close(fd);
        ::unlink(temporary.c_str());
        return false;
    }
    ::close(fd);

    if (::rename(temporary.c_str(), path.c_str()) != 0)
    {
        error = "rename failed: " + errnoText();
        ::unlink(temporary.c_str());
        return false;
    }
    syncDirectory(directory);
    return true;
#endif
}

bool createDirectories(const std::string& directory)
{
    if (directory.empty())
        return false;
    if (exists(directory))
        return true;

    // Walk forward and create each prefix in turn.
    for (size_t i = 1; i < directory.size(); ++i)
    {
        if (!isSeparator(directory[i]))
            continue;
        const std::string prefix = directory.substr(0, i);
        if (prefix.empty() || exists(prefix))
            continue;
#if defined(_WIN32)
        // Skip bare drive prefixes such as "C:".
        if (prefix.size() == 2 && prefix[1] == ':')
            continue;
#endif
        createSingleDirectory(prefix);
    }
    return createSingleDirectory(directory) || exists(directory);
}

bool removeFile(const std::string& path)
{
    if (path.empty())
        return false;
#if defined(_WIN32)
    const std::wstring wide = toWide(path);
    return DeleteFileW(wide.c_str()) != 0;
#else
    return ::unlink(path.c_str()) == 0;
#endif
}

bool renameFile(const std::string& from, const std::string& to)
{
    if (from.empty() || to.empty())
        return false;
#if defined(_WIN32)
    return MoveFileExW(toWide(from).c_str(), toWide(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return ::rename(from.c_str(), to.c_str()) == 0;
#endif
}

std::string directoryOf(const std::string& path)
{
    for (size_t i = path.size(); i > 0; --i)
    {
        if (isSeparator(path[i - 1]))
            return path.substr(0, i - 1);
    }
    return std::string();
}

std::string joinPath(const std::string& directory, const std::string& name)
{
    if (directory.empty())
        return name;
    if (name.empty())
        return directory;
    if (isSeparator(directory[directory.size() - 1]))
        return directory + name;
#if defined(_WIN32)
    return directory + "\\" + name;
#else
    return directory + "/" + name;
#endif
}

std::string userConfigDirectory()
{
#if defined(_WIN32)
    std::string base = environmentValue("LOCALAPPDATA");
    if (base.empty())
        base = environmentValue("APPDATA");
    if (base.empty())
        return std::string(".");
    return joinPath(base, MULTIROOM_BRAND_NAME);
#else
    const std::string xdg = environmentValue("XDG_CONFIG_HOME");
    if (!xdg.empty())
        return joinPath(xdg, MULTIROOM_BRAND_SLUG);
    const std::string home = environmentValue("HOME");
    if (!home.empty())
        return joinPath(joinPath(home, ".config"), MULTIROOM_BRAND_SLUG);
    return std::string(".");
#endif
}

} // namespace fileutil
} // namespace multiroom
