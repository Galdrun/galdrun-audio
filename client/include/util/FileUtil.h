#pragma once

#include <cstdio>
#include <string>

// Small filesystem helpers built on native APIs.
//
// <filesystem> is deliberately avoided: it needs -lstdc++fs on GCC 8 and is
// incomplete or missing in several toolchains still shipped for ARMv6 targets.
// All paths are UTF-8; on Windows they are converted to UTF-16 internally so
// non-ASCII paths work.

namespace multiroom {
namespace fileutil {

bool exists(const std::string& path);

// std::fopen for a UTF-8 path. Plain fopen on Windows reads the path in the
// ANSI code page, so a non-ASCII path fails to open.
std::FILE* openFile(const std::string& path, const char* mode);

bool readTextFile(const std::string& path, std::string& out, std::string& error);

// Writes to "<path>.tmp", flushes it to disk, then renames over the target, so
// a crash or power loss never leaves a truncated file behind. The containing
// directory is created if needed.
bool writeTextFileAtomic(const std::string& path, const std::string& content,
                         std::string& error);

// Creates the directory and any missing parents. True if it exists afterwards.
bool createDirectories(const std::string& directory);

bool removeFile(const std::string& path);

// Moves the file, replacing one already at the target.
bool renameFile(const std::string& from, const std::string& to);

// "/a/b/c.json" -> "/a/b". Empty if there is no separator.
std::string directoryOf(const std::string& path);

std::string joinPath(const std::string& directory, const std::string& name);

// Windows: %LOCALAPPDATA%\Galdrun
// Linux:   $XDG_CONFIG_HOME/galdrun, else $HOME/.config/galdrun
// Falls back to the current directory if neither is available.
std::string userConfigDirectory();

#if defined(_WIN32)
// UTF-8 <-> UTF-16 for the Windows API. Empty on empty or invalid input.
std::wstring toWide(const std::string& utf8);
std::string fromWide(const wchar_t* wide);
#endif

} // namespace fileutil
} // namespace multiroom
