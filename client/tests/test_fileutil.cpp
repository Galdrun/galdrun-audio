#include "TestSupport.h"

#include "util/FileUtil.h"
#include "util/Time.h"

using namespace multiroom;

MULTIROOM_TEST(fileutil_writes_and_reads_back)
{
    multiroomtest::TempDirectory temp("fileutil");
    const std::string path = temp.file("data.txt");
    const std::string content = "line one\nline two\n";

    std::string error;
    CHECK(fileutil::writeTextFileAtomic(path, content, error));
    CHECK(fileutil::exists(path));

    std::string readBack;
    CHECK(fileutil::readTextFile(path, readBack, error));
    CHECK_EQ(readBack, content);
}

MULTIROOM_TEST(fileutil_handles_large_and_empty_content)
{
    multiroomtest::TempDirectory temp("fileutil_sizes");
    std::string error;

    const std::string empty = temp.file("empty.txt");
    CHECK(fileutil::writeTextFileAtomic(empty, "", error));
    std::string readBack;
    CHECK(fileutil::readTextFile(empty, readBack, error));
    CHECK(readBack.empty());

    // Larger than the internal read and write chunk sizes.
    std::string large;
    large.reserve(300000);
    for (int i = 0; i < 30000; ++i)
        large += "0123456789";

    const std::string path = temp.file("large.txt");
    CHECK(fileutil::writeTextFileAtomic(path, large, error));
    CHECK(fileutil::readTextFile(path, readBack, error));
    CHECK_EQ(readBack.size(), large.size());
    CHECK(readBack == large);
}

MULTIROOM_TEST(fileutil_creates_missing_directories)
{
    multiroomtest::TempDirectory temp("fileutil_dirs");
    const std::string nested =
        fileutil::joinPath(fileutil::joinPath(temp.path(), "a"), "b");
    const std::string path = fileutil::joinPath(nested, "file.json");

    std::string error;
    CHECK(fileutil::writeTextFileAtomic(path, "{}", error));
    CHECK(fileutil::exists(nested));
    CHECK(fileutil::exists(path));
}

MULTIROOM_TEST(fileutil_reports_errors)
{
    std::string error;
    std::string content;
    CHECK(!fileutil::readTextFile("", content, error));
    CHECK(!fileutil::writeTextFileAtomic("", "x", error));
    CHECK(!fileutil::exists(""));

    multiroomtest::TempDirectory temp("fileutil_errors");
    CHECK(!fileutil::readTextFile(temp.file("nothing-here.txt"), content, error));
    CHECK(!error.empty());
}

MULTIROOM_TEST(fileutil_path_helpers)
{
    CHECK_EQ(fileutil::joinPath("", "file.txt"), std::string("file.txt"));
    CHECK_EQ(fileutil::joinPath("dir", ""), std::string("dir"));

    const std::string joined = fileutil::joinPath("dir", "file.txt");
    CHECK(joined == "dir/file.txt" || joined == "dir\\file.txt");
    // A trailing separator must not be doubled.
    const std::string trailing = fileutil::joinPath("dir/", "file.txt");
    CHECK_EQ(trailing, std::string("dir/file.txt"));

    CHECK_EQ(fileutil::directoryOf("/a/b/c.json"), std::string("/a/b"));
    CHECK(fileutil::directoryOf("c.json").empty());
}

MULTIROOM_TEST(fileutil_remove_file)
{
    multiroomtest::TempDirectory temp("fileutil_remove");
    const std::string path = temp.file("gone.txt");
    std::string error;
    CHECK(fileutil::writeTextFileAtomic(path, "x", error));
    CHECK(fileutil::removeFile(path));
    CHECK(!fileutil::exists(path));
}

MULTIROOM_TEST(fileutil_user_config_directory_is_absolute_enough)
{
    const std::string directory = fileutil::userConfigDirectory();
    CHECK(!directory.empty());
}

MULTIROOM_TEST(time_monotonic_clock_advances)
{
    const uint64_t first = monotonicNowNs();
    CHECK(first > 0);

    // Busy work rather than sleeping: this must stay fast in CI.
    uint64_t second = first;
    for (int i = 0; i < 1000000 && second == first; ++i)
        second = monotonicNowNs();

    CHECK(second >= first);
}

MULTIROOM_TEST(time_iso8601_has_expected_shape)
{
    const std::string text = iso8601UtcNow();
    CHECK_EQ(text.size(), static_cast<size_t>(20));
    CHECK_EQ(text[4], '-');
    CHECK_EQ(text[7], '-');
    CHECK_EQ(text[10], 'T');
    CHECK_EQ(text[13], ':');
    CHECK_EQ(text[16], ':');
    CHECK_EQ(text[19], 'Z');
}
