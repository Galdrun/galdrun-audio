#pragma once

// The few file operations this server needs, on std::filesystem.
//
// The client hand-wrote these because <filesystem> is not dependable on the
// ARMv6 toolchain it targets; this server has no such constraint, and the
// standard library is the platform API the project rule asks for first.
//
// This is the module that catches: std::filesystem and the stream classes
// throw, and no exception leaves these functions.

#include "core/Error.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace multiroom {

enum class FileError
{
    NotFound,
    ReadFailed,
    WriteFailed,
    CreateDirectoryFailed
};

const char* errorName(FileError error);

namespace fileutil {

Result<std::string, FileError> readTextFile(const std::filesystem::path& path);

// Writes to a sibling temporary file, flushes it and renames over the target,
// so a crash or a full disk leaves the previous contents intact rather than a
// half-written config. Creates the parent directory if it is missing.
Status<FileError> writeTextFileAtomic(const std::filesystem::path& path,
                                      std::string_view contents);

Status<FileError> createDirectories(const std::filesystem::path& path);

// %LOCALAPPDATA%\Galdrun on Windows, $XDG_CONFIG_HOME/galdrun (or
// ~/.config/galdrun) elsewhere.
std::filesystem::path userConfigDirectory();

} // namespace fileutil
} // namespace multiroom
