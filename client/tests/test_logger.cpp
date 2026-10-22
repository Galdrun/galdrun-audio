#include "TestSupport.h"

#include "util/FileUtil.h"
#include "util/Logger.h"

#include <string>

using namespace multiroom;

MULTIROOM_TEST(logger_appends_to_the_file_it_was_given)
{
    multiroomtest::TempDirectory temp("logger");
    const std::string path = temp.file("galdrun-client.log");

    CHECK(log::setLogFile(path));
    MULTIROOM_ERROR("a message worth keeping: %d", 42);
    log::closeLogFile();

    std::string contents;
    std::string error;
    CHECK(fileutil::readTextFile(path, contents, error));
    CHECK(contents.find("a message worth keeping: 42") != std::string::npos);
    CHECK(contents.find("ERROR") != std::string::npos);

    // Appends rather than truncates: a service that is restarted must not take
    // the reason it was restarted with it.
    CHECK(log::setLogFile(path));
    MULTIROOM_ERROR("and a second one");
    log::closeLogFile();

    CHECK(fileutil::readTextFile(path, contents, error));
    CHECK(contents.find("a message worth keeping: 42") != std::string::npos);
    CHECK(contents.find("and a second one") != std::string::npos);
}

MULTIROOM_TEST(logger_keeps_writing_to_stderr_when_the_file_will_not_open)
{
    multiroomtest::TempDirectory temp("logger-bad");

    // A directory that does not exist. The caller warns and carries on, which
    // is why this reports rather than aborts.
    CHECK(!log::setLogFile(temp.file("missing/galdrun-client.log")));
    MULTIROOM_ERROR("still reported");

    // An empty path is how the file is turned off, and turning off a sink that
    // never opened is not a failure.
    CHECK(log::setLogFile(std::string()));
}

MULTIROOM_TEST(logger_opens_a_path_outside_the_ansi_code_page)
{
    multiroomtest::TempDirectory temp("logger-utf8");
    // "Ω" is in no Western ANSI code page, so plain fopen on Windows cannot
    // name it.
    const std::string path = temp.file("galdrun-client-\xCE\xA9.log");

    CHECK(log::setLogFile(path));
    MULTIROOM_ERROR("written under a UTF-8 name");
    log::closeLogFile();

    std::string contents;
    std::string error;
    CHECK(fileutil::readTextFile(path, contents, error));
    CHECK(contents.find("written under a UTF-8 name") != std::string::npos);
}

MULTIROOM_TEST(logger_moves_a_full_file_aside_and_starts_a_new_one)
{
    multiroomtest::TempDirectory temp("logger-rotate");
    const std::string path = temp.file("galdrun-client.log");

    // A small cap so a few lines fill it; the default is a megabyte.
    CHECK(log::setLogFile(path, 200));
    for (int i = 0; i < 20; ++i)
        MULTIROOM_ERROR("line %d of a client that will not stop complaining", i);
    log::closeLogFile();

    std::string current;
    std::string previous;
    std::string error;
    CHECK(fileutil::readTextFile(path, current, error));
    CHECK(fileutil::readTextFile(path + ".1", previous, error));
    CHECK(current.size() < 400);
    CHECK(previous.size() < 400);
    CHECK(current.find("line 19 of") != std::string::npos);
    CHECK(!fileutil::exists(path + ".2"));
}

MULTIROOM_TEST(logger_stops_file_growth_when_rotation_fails)
{
    multiroomtest::TempDirectory temp("logger-rotate-failure");
    const std::string path = temp.file("galdrun-client.log");
    // A directory at the backup path prevents replacement on every platform.
    CHECK(fileutil::createDirectories(path + ".1"));
    CHECK(log::setLogFile(path, 200));
    for (int i = 0; i < 50; ++i)
        MULTIROOM_ERROR("line %d despite a failed rotation", i);
    log::closeLogFile();

    std::string contents;
    std::string error;
    CHECK(fileutil::readTextFile(path, contents, error));
    CHECK(contents.size() < 400);
    CHECK(contents.find("line 0 despite") != std::string::npos);
    CHECK(contents.find("line 49 despite") == std::string::npos);

    const std::string recovered = temp.file("recovered.log");
    CHECK(log::setLogFile(recovered));
    MULTIROOM_ERROR("file logging explicitly enabled again");
    log::closeLogFile();
    CHECK(fileutil::readTextFile(recovered, contents, error));
    CHECK(contents.find("explicitly enabled again") != std::string::npos);
}
