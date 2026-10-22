#include "TestSupport.h"

#include "util/FileUtil.h"

#include <fstream>

using namespace multiroom;

MULTIROOM_TEST(fileutil_reads_back_what_it_wrote)
{
    const multiroomtest::TempDirectory temp("file");
    const std::filesystem::path path = std::filesystem::path(temp.path()) / "server.json";

    const std::string contents = "{\n  \"logLevel\": \"info\"\n}\n";
    CHECK_OK(fileutil::writeTextFileAtomic(path, contents));

    const auto loaded = fileutil::readTextFile(path);
    CHECK_OK(loaded);
    CHECK_EQ(*loaded, contents);
}

MULTIROOM_TEST(fileutil_reports_a_missing_file_as_its_own_error)
{
    const multiroomtest::TempDirectory temp("file");
    const auto loaded = fileutil::readTextFile(std::filesystem::path(temp.path()) / "absent.json");

    // The caller has to be able to tell "no config yet" from "the config is
    // unreadable", so these are not the same code.
    CHECK_ERR(loaded, FileError::NotFound);
    CHECK(describe(loaded.error()).find("absent.json") != std::string::npos);
}

MULTIROOM_TEST(fileutil_creates_the_parent_directory_and_leaves_no_temporary)
{
    const multiroomtest::TempDirectory temp("file");
    const std::filesystem::path path =
        std::filesystem::path(temp.path()) / "a" / "b" / "server.json";

    CHECK_OK(fileutil::writeTextFileAtomic(path, "{}"));
    CHECK(std::filesystem::exists(path));
    CHECK(!std::filesystem::exists(std::filesystem::path(path.string() + ".tmp")));
}

MULTIROOM_TEST(fileutil_replaces_an_existing_file_atomically)
{
    const multiroomtest::TempDirectory temp("file");
    const std::filesystem::path path = std::filesystem::path(temp.path()) / "server.json";

    CHECK_OK(fileutil::writeTextFileAtomic(path, "first"));
    CHECK_OK(fileutil::writeTextFileAtomic(path, "second"));

    const auto loaded = fileutil::readTextFile(path);
    CHECK_OK(loaded);
    CHECK_EQ(*loaded, std::string("second"));
}

MULTIROOM_TEST(fileutil_reports_a_path_it_cannot_write)
{
    const multiroomtest::TempDirectory temp("file");
    const std::filesystem::path file = std::filesystem::path(temp.path()) / "occupied";
    CHECK_OK(fileutil::writeTextFileAtomic(file, "x"));

    // A file where a directory would have to be: no exception leaves here.
    const auto written = fileutil::writeTextFileAtomic(file / "child.json", "x");
    CHECK(!written.has_value());
    CHECK(written.error().code == FileError::CreateDirectoryFailed ||
          written.error().code == FileError::WriteFailed);
}

MULTIROOM_TEST(fileutil_create_directories_is_idempotent)
{
    const multiroomtest::TempDirectory temp("file");
    const std::filesystem::path directory = std::filesystem::path(temp.path()) / "x" / "y";

    CHECK_OK(fileutil::createDirectories(directory));
    CHECK_OK(fileutil::createDirectories(directory));
    CHECK_OK(fileutil::createDirectories(""));
    CHECK(std::filesystem::is_directory(directory));
}

MULTIROOM_TEST(fileutil_user_config_directory_is_absolute)
{
    const std::filesystem::path directory = fileutil::userConfigDirectory();
    CHECK(!directory.empty());
    CHECK(directory.is_absolute());
}
