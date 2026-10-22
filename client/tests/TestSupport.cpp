#include "TestSupport.h"

#include "util/FileUtil.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace multiroomtest {
namespace {

struct RegisteredTest
{
    const char* name;
    TestFunction function;
};

// Function-local to avoid a static initialization order problem with the
// registrars in each translation unit.
std::vector<RegisteredTest>& registry()
{
    static std::vector<RegisteredTest> tests;
    return tests;
}

unsigned long processId()
{
#if defined(_WIN32)
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

std::string systemTempDirectory()
{
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH + 1];
    const DWORD length = GetTempPathW(MAX_PATH + 1, buffer);
    if (length == 0)
        return std::string(".");
    char utf8[MAX_PATH * 4];
    const int written = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length),
                                            utf8, sizeof(utf8), nullptr, nullptr);
    if (written <= 0)
        return std::string(".");
    return std::string(utf8, static_cast<size_t>(written));
#else
    const char* tmp = std::getenv("TMPDIR");
    if (tmp != nullptr && *tmp != '\0')
        return std::string(tmp);
    return std::string("/tmp");
#endif
}

void removeDirectoryRecursively(const std::string& path)
{
#if defined(_WIN32)
    const std::string pattern = path + "\\*";
    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(pattern.c_str(), &data);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::string name = data.cFileName;
            if (name == "." || name == "..")
                continue;
            const std::string child = path + "\\" + name;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                removeDirectoryRecursively(child);
            else
                DeleteFileA(child.c_str());
        } while (FindNextFileA(find, &data) != 0);
        FindClose(find);
    }
    RemoveDirectoryA(path.c_str());
#else
    // No <dirent.h> walk needed: tests only create flat directories, and any
    // leftover is confined to the system temp dir.
    const std::string command = "rm -rf '" + path + "'";
    if (std::system(command.c_str()) != 0)
        std::fprintf(stderr, "warning: could not remove %s\n", path.c_str());
#endif
}

int g_counter = 0;

} // namespace

void registerTest(const char* name, TestFunction function)
{
    RegisteredTest test;
    test.name = name;
    test.function = function;
    registry().push_back(test);
}

void failAt(const std::string& message, const char* file, int line)
{
    Failure failure;
    failure.message = std::string(file) + ":" + toText(line) + "\n  " + message;
    throw failure;
}

int runAll()
{
    const std::vector<RegisteredTest>& tests = registry();
    int failed = 0;

    for (size_t i = 0; i < tests.size(); ++i)
    {
        try
        {
            tests[i].function();
            std::printf("[ ok ] %s\n", tests[i].name);
        }
        catch (const Failure& failure)
        {
            std::printf("[fail] %s\n%s\n", tests[i].name, failure.message.c_str());
            ++failed;
        }
        catch (const std::exception& e)
        {
            std::printf("[fail] %s\n  unexpected exception: %s\n", tests[i].name, e.what());
            ++failed;
        }
    }

    std::printf("\n%lu test(s), %d failure(s)\n",
                static_cast<unsigned long>(tests.size()), failed);
    return failed == 0 ? 0 : 1;
}

TempDirectory::TempDirectory(const char* tag)
{
    const std::string name = std::string("multiroom_test_") + tag + "_" + toText(processId()) +
                             "_" + toText(++g_counter);
    path_ = multiroom::fileutil::joinPath(systemTempDirectory(), name);
    multiroom::fileutil::createDirectories(path_);
}

TempDirectory::~TempDirectory()
{
    removeDirectoryRecursively(path_);
}

std::string TempDirectory::file(const std::string& name) const
{
    return multiroom::fileutil::joinPath(path_, name);
}

} // namespace multiroomtest
