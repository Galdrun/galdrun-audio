#include "TestSupport.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <system_error>
#include <vector>

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

int g_counter = 0;

} // namespace

void registerTest(const char* name, TestFunction function)
{
    registry().push_back(RegisteredTest{name, function});
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
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::temp_directory_path(ec);
    const std::filesystem::path base = ec ? std::filesystem::path(".") : root;

    const std::string name = std::string("multiroom_server_test_") + tag + "_" + toText(++g_counter);
    const std::filesystem::path directory = base / name;

    std::filesystem::remove_all(directory, ec);
    std::filesystem::create_directories(directory, ec);
    path_ = directory.string();
}

TempDirectory::~TempDirectory()
{
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    if (ec)
        std::fprintf(stderr, "warning: could not remove %s\n", path_.c_str());
}

std::string TempDirectory::file(const std::string& name) const
{
    return (std::filesystem::path(path_) / name).string();
}

} // namespace multiroomtest
