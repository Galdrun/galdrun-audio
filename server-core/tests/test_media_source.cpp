#include "TestSupport.h"

#include "source/FileMediaSource.h"
#include "source/MemoryMediaSource.h"

#if MULTIROOM_WITH_FFMPEG
#include "source/http/HttpMediaSource.h"
#include "util/Clock.h"
#endif

#include <fstream>
#include <numeric>
#include <string>
#include <vector>

using namespace multiroom;

namespace {

std::vector<std::byte> countingBytes(size_t count)
{
    std::vector<std::byte> bytes(count);
    for (size_t index = 0; index < count; ++index)
        bytes[index] = static_cast<std::byte>(index & 0xFF);
    return bytes;
}

void writeFile(const std::string& path, const std::vector<std::byte>& bytes)
{
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

} // namespace

MULTIROOM_TEST(memory_source_reads_reports_and_ends)
{
    MemoryMediaSource source(countingBytes(1000), "fixture");

    CHECK(source.seekable());
    CHECK(source.sizeBytes().has_value());
    CHECK_EQ(*source.sizeBytes(), 1000u);
    CHECK_EQ(source.describe(), std::string("fixture"));

    std::vector<std::byte> buffer(300);
    auto first = source.read(buffer);
    CHECK_OK(first);
    CHECK_EQ(*first, 300u);
    CHECK_EQ(source.position(), 300u);
    CHECK_EQ(static_cast<int>(buffer[0]), 0);
    CHECK_EQ(static_cast<int>(buffer[299]), 299 & 0xFF);

    // A short read is the tail of the source, not a failure.
    CHECK_OK(source.seek(900));
    auto tail = source.read(buffer);
    CHECK_OK(tail);
    CHECK_EQ(*tail, 100u);

    // Zero, and only zero, is the end.
    auto end = source.read(buffer);
    CHECK_OK(end);
    CHECK_EQ(*end, 0u);
}

MULTIROOM_TEST(memory_source_seeks_to_the_end_but_not_past_it)
{
    MemoryMediaSource source(countingBytes(64), "fixture");

    // avformat asks for exactly the end to find out whether anything is left.
    CHECK_OK(source.seek(64));
    CHECK_EQ(source.position(), 64u);

    auto result = source.seek(65);
    CHECK_ERR(result, MediaSourceError::OutOfRange);
    CHECK(result.error().detail.find("fixture") != std::string::npos);
}

MULTIROOM_TEST(file_source_reads_seeks_and_sizes)
{
    multiroomtest::TempDirectory temp("mediasource");
    const std::string path = temp.file("bytes.bin");
    writeFile(path, countingBytes(5000));

    auto opened = FileMediaSource::open(path);
    CHECK_OK(opened);
    auto& source = **opened;

    CHECK(source.seekable());
    CHECK_EQ(*source.sizeBytes(), 5000u);

    std::vector<std::byte> buffer(1024);
    auto first = source.read(buffer);
    CHECK_OK(first);
    CHECK_EQ(*first, 1024u);
    CHECK_EQ(source.position(), 1024u);

    CHECK_OK(source.seek(4990));
    auto tail = source.read(buffer);
    CHECK_OK(tail);
    CHECK_EQ(*tail, 10u);
    CHECK_EQ(static_cast<int>(buffer[0]), 4990 & 0xFF);

    // The end has to be readable again after it has been reached once: a
    // stream left with eofbit set silently refuses every later seek.
    auto end = source.read(buffer);
    CHECK_OK(end);
    CHECK_EQ(*end, 0u);

    CHECK_OK(source.seek(0));
    auto again = source.read(buffer);
    CHECK_OK(again);
    CHECK_EQ(*again, 1024u);
}

MULTIROOM_TEST(file_source_reports_what_went_wrong_by_name)
{
    multiroomtest::TempDirectory temp("mediasource");

    auto missing = FileMediaSource::open(temp.file("absent.flac"));
    CHECK_ERR(missing, MediaSourceError::NotFound);
    CHECK(missing.error().detail.find("absent.flac") != std::string::npos);

    auto directory = FileMediaSource::open(temp.path());
    CHECK_ERR(directory, MediaSourceError::OpenFailed);

    auto empty = FileMediaSource::open(std::filesystem::path{});
    CHECK_ERR(empty, MediaSourceError::InvalidArgument);
}

MULTIROOM_TEST(file_source_refuses_a_seek_past_the_end)
{
    multiroomtest::TempDirectory temp("mediasource");
    const std::string path = temp.file("short.bin");
    writeFile(path, countingBytes(16));

    auto opened = FileMediaSource::open(path);
    CHECK_OK(opened);

    CHECK_OK((*opened)->seek(16));
    CHECK_ERR((*opened)->seek(17), MediaSourceError::OutOfRange);
}

#if MULTIROOM_WITH_FFMPEG

// The rest of HttpMediaSource needs a server on the other end, which this
// phase has no way to start -- the listeners arrive in phase 8, and a test
// that reached the real internet would break the rule that tests need no
// network. What can be pinned here is the guard that keeps a URI from the
// control API off this server's filesystem, and it is the part worth pinning.
MULTIROOM_TEST(http_source_takes_only_http_and_https)
{
    SystemClock clock;

    for (const char* uri : {"file:///etc/passwd", "concat:a|b", "/tmp/song.flac", ""})
    {
        auto result = HttpMediaSource::open(uri, clock);
        CHECK_ERR(result, MediaSourceError::InvalidArgument);
    }
}

#endif
