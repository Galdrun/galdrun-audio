#pragma once

// Minimal test harness: registration, assertions, one executable.
//
// The same one the client uses. A full framework would be a dependency for
// something that fits in seventy lines, and CTest already provides discovery
// and reporting.

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace multiroomtest {

using TestFunction = void (*)();

void registerTest(const char* name, TestFunction function);
// Every test, or with `filters` only those whose name contains one of them.
int runAll(const std::vector<std::string>& filters = {});

// Thrown by a failing assertion; caught by the runner so the remaining tests
// still execute.
struct Failure
{
    std::string message;
};

void failAt(const std::string& message, const char* file, int line);

// Closes the listening socket this process has bound to `port` -- a TCP one
// that accepts, or with `datagram` a UDP one -- under the listener that owns
// it: the next select() that listener makes fails as a broken socket
// library's would, which nothing else here can cause. False when no such
// socket was found. The owner closes the number again on its way out, so
// nothing may open a socket between this and its destruction.
bool closeListenerOn(uint16_t port, bool datagram = false);

template <typename T>
std::string toText(const T& value)
{
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

inline std::string toText(bool value)
{
    return value ? "true" : "false";
}

struct Registrar
{
    Registrar(const char* name, TestFunction function) { registerTest(name, function); }
};

// Creates a scratch directory under the system temp dir, removed on scope exit.
class TempDirectory
{
public:
    explicit TempDirectory(const char* tag);
    ~TempDirectory();

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    const std::string& path() const { return path_; }
    std::string file(const std::string& name) const;

private:
    std::string path_;
};

} // namespace multiroomtest

#define MULTIROOM_TEST(name)                                                                        \
    static void name();                                                                       \
    static ::multiroomtest::Registrar registrar_##name(#name, &name);                               \
    static void name()

#define CHECK(condition)                                                                      \
    do                                                                                        \
    {                                                                                         \
        if (!(condition))                                                                     \
            ::multiroomtest::failAt("CHECK failed: " #condition, __FILE__, __LINE__);               \
    } while (false)

#define CHECK_EQ(actual, expected)                                                            \
    do                                                                                        \
    {                                                                                         \
        const auto check_actual = (actual);                                                  \
        const auto check_expected = (expected);                                              \
        if (!(check_actual == check_expected))                                                \
        {                                                                                     \
            ::multiroomtest::failAt("CHECK_EQ failed: " #actual " == " #expected                    \
                              "\n    actual:   " +                                            \
                                  ::multiroomtest::toText(check_actual) +                           \
                                  "\n    expected: " +                                        \
                                  ::multiroomtest::toText(check_expected),                          \
                              __FILE__, __LINE__);                                            \
        }                                                                                     \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                               \
    do                                                                                        \
    {                                                                                         \
        const double check_actual = static_cast<double>(actual);                              \
        const double check_expected = static_cast<double>(expected);                          \
        const double check_delta = check_actual - check_expected;                             \
        if (!((check_delta < 0 ? -check_delta : check_delta) <=                               \
              static_cast<double>(tolerance)))                                                \
        {                                                                                     \
            ::multiroomtest::failAt("CHECK_NEAR failed: " #actual " ~= " #expected                  \
                              "\n    actual:   " +                                            \
                                  ::multiroomtest::toText(check_actual) +                           \
                                  "\n    expected: " +                                        \
                                  ::multiroomtest::toText(check_expected),                          \
                              __FILE__, __LINE__);                                            \
        }                                                                                     \
    } while (false)

// std::expected is the server's error model, so the harness has to be able to
// say why a call that should have worked did not -- CHECK(result.has_value())
// alone throws away the one string that explains it.
#define CHECK_OK(result)                                                                      \
    do                                                                                        \
    {                                                                                         \
        const auto& check_result = (result);                                                  \
        if (!check_result.has_value())                                                        \
        {                                                                                     \
            ::multiroomtest::failAt("CHECK_OK failed: " #result "\n    error:    " +                \
                                  ::multiroom::describe(check_result.error()),                   \
                              __FILE__, __LINE__);                                            \
        }                                                                                     \
    } while (false)

#define CHECK_ERR(result, expected_code)                                                      \
    do                                                                                        \
    {                                                                                         \
        const auto& check_result = (result);                                                  \
        if (check_result.has_value())                                                         \
        {                                                                                     \
            ::multiroomtest::failAt("CHECK_ERR failed: " #result " succeeded, expected " #expected_code, \
                              __FILE__, __LINE__);                                            \
        }                                                                                     \
        else if (!(check_result.error().code == (expected_code)))                             \
        {                                                                                     \
            ::multiroomtest::failAt("CHECK_ERR failed: " #result "\n    actual:   " +               \
                                  ::multiroom::describe(check_result.error()) +                  \
                                  "\n    expected: " #expected_code,                          \
                              __FILE__, __LINE__);                                            \
        }                                                                                     \
    } while (false)
