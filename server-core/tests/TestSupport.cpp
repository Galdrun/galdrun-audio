#include "TestSupport.h"

#include "net/Socket.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string_view>
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

int runAll(const std::vector<std::string>& filters)
{
    const std::vector<RegisteredTest>& tests = registry();
    int failed = 0;
    size_t ran = 0;

    for (size_t i = 0; i < tests.size(); ++i)
    {
        const std::string_view name = tests[i].name;
        if (!filters.empty() &&
            std::none_of(filters.begin(), filters.end(),
                         [&](const std::string& filter) { return name.contains(filter); }))
        {
            continue;
        }
        ++ran;
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

    std::printf("\n%lu test(s), %d failure(s)\n", static_cast<unsigned long>(ran), failed);
    return failed == 0 ? 0 : 1;
}

bool closeListenerOn(uint16_t port, bool datagram)
{
    // Socket numbers are small: descriptors on POSIX, and handles on Windows,
    // which are multiples of four.
#if defined(_WIN32)
    constexpr uintptr_t kFirst = 4;
    constexpr uintptr_t kStep = 4;
    constexpr uintptr_t kLast = uintptr_t{1} << 20;
    using Length = int;
#else
    constexpr uintptr_t kFirst = 0;
    constexpr uintptr_t kStep = 1;
    constexpr uintptr_t kLast = FD_SETSIZE;
    using Length = socklen_t;
#endif
    for (uintptr_t value = kFirst; value < kLast; value += kStep)
    {
        const auto socket = static_cast<multiroom::net::Socket>(value);
        // An accepted TCP connection shares its listener's port, so a stream
        // socket has to be the one accepting; a datagram socket is the only
        // one on its port.
        int answer = 0;
        Length length = sizeof(answer);
        const int option = datagram ? SO_TYPE : SO_ACCEPTCONN;
        if (::getsockopt(socket, SOL_SOCKET, option, reinterpret_cast<char*>(&answer), &length) !=
                0 ||
            (datagram ? answer != SOCK_DGRAM : answer == 0))
        {
            continue;
        }
        uint16_t bound = 0;
        if (!multiroom::net::boundPort(socket, bound) || bound != port)
            continue;
        multiroom::net::closeSocket(socket);
        return true;
    }
    return false;
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
