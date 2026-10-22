#include "util/FileUtil.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace multiroom {

const char* errorName(FileError error)
{
    switch (error)
    {
    case FileError::NotFound:
        return "NotFound";
    case FileError::ReadFailed:
        return "ReadFailed";
    case FileError::WriteFailed:
        return "WriteFailed";
    case FileError::CreateDirectoryFailed:
        return "CreateDirectoryFailed";
    }
    return "?";
}

namespace fileutil {
namespace {

std::string pathText(const std::filesystem::path& path)
{
    // u8string() would be the exact spelling but returns char8_t; the log and
    // the error detail want something printable, and a path this server names
    // is one the config or the command line supplied.
    return path.string();
}

} // namespace

Result<std::string, FileError> readTextFile(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
        return fail(FileError::NotFound, pathText(path));

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return fail(FileError::ReadFailed, pathText(path));

    std::ostringstream contents;
    contents << stream.rdbuf();
    if (stream.bad())
        return fail(FileError::ReadFailed, pathText(path));

    return contents.str();
}

Status<FileError> createDirectories(const std::filesystem::path& path)
{
    if (path.empty())
        return {};

    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec && !std::filesystem::is_directory(path))
        return fail(FileError::CreateDirectoryFailed, pathText(path) + ": " + ec.message());

    return {};
}

Status<FileError> writeTextFileAtomic(const std::filesystem::path& path,
                                      std::string_view contents)
{
    const std::filesystem::path parent = path.parent_path();
    if (auto created = createDirectories(parent); !created)
        return created;

    std::filesystem::path temporary = path;
    temporary += ".tmp";

    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
            return fail(FileError::WriteFailed, pathText(temporary));
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        stream.flush();
        if (!stream)
            return fail(FileError::WriteFailed, pathText(temporary));
    }

    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return fail(FileError::WriteFailed, pathText(path) + ": " + ec.message());
    }

    return {};
}

std::filesystem::path userConfigDirectory()
{
#if defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0')
        return std::filesystem::path(local) / MULTIROOM_BRAND_NAME;
    return std::filesystem::current_path();
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0')
        return std::filesystem::path(xdg) / MULTIROOM_BRAND_SLUG;
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
        return std::filesystem::path(home) / ".config" / MULTIROOM_BRAND_SLUG;
    return std::filesystem::current_path();
#endif
}

} // namespace fileutil
} // namespace multiroom
