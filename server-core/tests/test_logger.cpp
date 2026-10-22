#include "TestSupport.h"

#include "util/FileUtil.h"
#include "util/Logger.h"

#include <filesystem>
#include <string>

using namespace multiroom;

namespace {

// The log level is process-wide, so a test that changes it puts it back.
class LevelGuard
{
public:
    explicit LevelGuard(LogLevel level) : previous_(log::currentLevel()) { log::setLevel(level); }
    ~LevelGuard() { log::setLevel(previous_); }

    LevelGuard(const LevelGuard&) = delete;
    LevelGuard& operator=(const LevelGuard&) = delete;

private:
    LogLevel previous_;
};

} // namespace

MULTIROOM_TEST(logger_levels_round_trip_through_their_names)
{
    const LogLevel all[] = {LogLevel::Error, LogLevel::Warn, LogLevel::Info, LogLevel::Debug,
                            LogLevel::Trace};
    for (const LogLevel level : all)
    {
        LogLevel parsed = LogLevel::Info;
        CHECK(log::parseLevel(log::levelName(level), parsed));
        CHECK(parsed == level);
    }

    LogLevel lower = LogLevel::Info;
    CHECK(log::parseLevel("debug", lower));
    CHECK(lower == LogLevel::Debug);

    LogLevel unused = LogLevel::Info;
    CHECK(!log::parseLevel("verbose", unused));
    CHECK(!log::parseLevel("", unused));
    CHECK(!log::parseLevel("Info", unused)); // only the two spellings, exactly
}

MULTIROOM_TEST(logger_enables_everything_at_or_below_the_current_level)
{
    const LevelGuard guard(LogLevel::Warn);

    CHECK(log::currentLevel() == LogLevel::Warn);
    CHECK(log::enabled(LogLevel::Error));
    CHECK(log::enabled(LogLevel::Warn));
    CHECK(!log::enabled(LogLevel::Info));
    CHECK(!log::enabled(LogLevel::Debug));
    CHECK(!log::enabled(LogLevel::Trace));

    log::setLevel(LogLevel::Trace);
    CHECK(log::enabled(LogLevel::Trace));

    log::setLevel(LogLevel::Error);
    CHECK(log::enabled(LogLevel::Error));
    CHECK(!log::enabled(LogLevel::Warn));
}

MULTIROOM_TEST(logger_writes_the_enabled_levels_to_its_file)
{
    const multiroomtest::TempDirectory temp("logger");
    const std::string path = temp.file("server.log");

    const LevelGuard guard(LogLevel::Info);
    CHECK(log::setLogFile(path));

    MULTIROOM_ERROR("an error about {}", "kitchen");
    MULTIROOM_INFO("{} blocks of {} bytes", 128, 7688);
    MULTIROOM_DEBUG("this one is below the level");

    log::closeLogFile();

    const auto contents = fileutil::readTextFile(path);
    CHECK_OK(contents);

    CHECK(contents->find("an error about kitchen") != std::string::npos);
    CHECK(contents->find("128 blocks of 7688 bytes") != std::string::npos);
    CHECK(contents->find("below the level") == std::string::npos);

    // The file always carries the source location, whatever the level.
    CHECK(contents->find("[test_logger.cpp:") != std::string::npos);
    CHECK(contents->find("ERROR") != std::string::npos);
    CHECK(contents->find("INFO") != std::string::npos);
}

MULTIROOM_TEST(logger_appends_rather_than_truncating)
{
    const multiroomtest::TempDirectory temp("logger");
    const std::string path = temp.file("server.log");
    const LevelGuard guard(LogLevel::Info);

    CHECK(log::setLogFile(path));
    MULTIROOM_INFO("first run");
    log::closeLogFile();

    CHECK(log::setLogFile(path));
    MULTIROOM_INFO("second run");
    log::closeLogFile();

    const auto contents = fileutil::readTextFile(path);
    CHECK_OK(contents);
    CHECK(contents->find("first run") != std::string::npos);
    CHECK(contents->find("second run") != std::string::npos);
}

MULTIROOM_TEST(logger_reports_a_file_it_cannot_open_and_keeps_working)
{
    const multiroomtest::TempDirectory temp("logger");

    // A directory that does not exist: the logger does not create one, because
    // a log path that is wrong should be visible rather than materialized.
    CHECK(!log::setLogFile(temp.file("missing/server.log")));

    // stderr output keeps working either way, which is what the false means.
    MULTIROOM_ERROR("still logging");
    log::closeLogFile();
}

// Phase 29: written well past the limit, the log is the current file and one
// previous one, never a third, and neither holds a partial line or more than
// the limit.
MULTIROOM_TEST(logger_rotates_to_one_previous_file_at_its_size_limit)
{
    const multiroomtest::TempDirectory temp("logger");
    const std::string path = temp.file("server.log");
    const LevelGuard guard(LogLevel::Info);

    constexpr uint64_t kLimit = 4096;
    CHECK(log::setLogFile(path));
    log::setMaxFileBytes(kLimit);
    for (int index = 0; index < 400; ++index)
        MULTIROOM_INFO("line {:04} of a log that is rotated by size", index);
    log::setMaxFileBytes(0);
    log::closeLogFile();

    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(temp.path()))
    {
        (void)entry;
        ++files;
    }
    CHECK_EQ(files, 2u);

    const auto current = fileutil::readTextFile(path);
    const auto previous = fileutil::readTextFile(path + ".1");
    CHECK_OK(current);
    CHECK_OK(previous);
    if (!current || !previous)
        return;

    for (const std::string* contents : {&*current, &*previous})
    {
        CHECK(!contents->empty());
        CHECK(contents->size() <= kLimit);
        CHECK(contents->back() == '\n'); // no line cut at the end
        CHECK(contents->find("rotated by size") < contents->find('\n')); // nor at the start
    }

    // The newest line is in the current file, and the previous file is what
    // came just before it: nothing between the two was lost.
    CHECK(current->find("line 0399") != std::string::npos);
    const size_t firstCurrent = std::stoul(current->substr(current->find("line ") + 5, 4));
    const size_t lastPrevious = std::stoul(previous->substr(previous->rfind("line ") + 5, 4));
    CHECK_EQ(firstCurrent, lastPrevious + 1);
}
