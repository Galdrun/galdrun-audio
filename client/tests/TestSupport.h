#pragma once

// Minimal test harness: registration, assertions, one executable.
//
// A full framework would be a dependency for something that fits in seventy
// lines, and CTest already provides discovery and reporting.

#include <sstream>
#include <string>

namespace multiroomtest {

using TestFunction = void (*)();

void registerTest(const char* name, TestFunction function);
int runAll();

// Thrown by a failing assertion; caught by the runner so the remaining tests
// still execute.
struct Failure
{
    std::string message;
};

void failAt(const std::string& message, const char* file, int line);

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

#define MULTIROOM_TEST(name)                                                         \
    static void name();                                                        \
    static ::multiroomtest::Registrar registrar_##name(#name, &name);                \
    static void name()

#define CHECK(condition)                                                       \
    do                                                                         \
    {                                                                          \
        if (!(condition))                                                      \
            ::multiroomtest::failAt("CHECK failed: " #condition, __FILE__, __LINE__); \
    } while (false)

#define CHECK_EQ(actual, expected)                                             \
    do                                                                         \
    {                                                                          \
        const auto& check_actual = (actual);                                   \
        const auto& check_expected = (expected);                               \
        if (!(check_actual == check_expected))                                 \
        {                                                                      \
            ::multiroomtest::failAt("CHECK_EQ failed: " #actual " == " #expected     \
                              "\n    actual:   " +                             \
                                  ::multiroomtest::toText(check_actual) +            \
                                  "\n    expected: " +                         \
                                  ::multiroomtest::toText(check_expected),           \
                              __FILE__, __LINE__);                             \
        }                                                                      \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                \
    do                                                                         \
    {                                                                          \
        const double check_actual = static_cast<double>(actual);               \
        const double check_expected = static_cast<double>(expected);           \
        const double check_delta = check_actual - check_expected;              \
        if (!((check_delta < 0 ? -check_delta : check_delta) <=                \
              static_cast<double>(tolerance)))                                 \
        {                                                                      \
            ::multiroomtest::failAt("CHECK_NEAR failed: " #actual " ~= " #expected   \
                              "\n    actual:   " +                             \
                                  ::multiroomtest::toText(check_actual) +            \
                                  "\n    expected: " +                         \
                                  ::multiroomtest::toText(check_expected),           \
                              __FILE__, __LINE__);                             \
        }                                                                      \
    } while (false)
