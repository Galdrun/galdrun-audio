#include "source/FileMediaSource.h"

#include <system_error>
#include <utility>

namespace multiroom {
namespace {

std::string pathText(const std::filesystem::path& path)
{
    return path.string();
}

} // namespace

FileMediaSource::FileMediaSource(std::ifstream stream, std::string name, uint64_t size)
    : stream_(std::move(stream)), name_(std::move(name)), size_(size)
{
}

Result<std::unique_ptr<FileMediaSource>, MediaSourceError>
FileMediaSource::open(const std::filesystem::path& path)
{
    if (path.empty())
        return fail(MediaSourceError::InvalidArgument, "empty path");

    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(status))
        return fail(MediaSourceError::NotFound, pathText(path));
    if (std::filesystem::is_directory(status))
        return fail(MediaSourceError::OpenFailed, pathText(path) + " is a directory");

    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec)
        return fail(MediaSourceError::OpenFailed, pathText(path) + ": " + ec.message());

    std::ifstream stream;
    try
    {
        stream.open(path, std::ios::binary);
    }
    catch (const std::exception& e)
    {
        return fail(MediaSourceError::OpenFailed, pathText(path) + ": " + e.what());
    }

    // The stream classes report a permission problem the same way they report
    // everything else, so the distinction has to come from the filesystem.
    if (!stream)
    {
        const std::filesystem::perms permissions = status.permissions();
        const bool readable = (permissions & (std::filesystem::perms::owner_read |
                                              std::filesystem::perms::group_read |
                                              std::filesystem::perms::others_read)) !=
                              std::filesystem::perms::none;
        return fail(readable ? MediaSourceError::OpenFailed : MediaSourceError::AccessDenied,
                    pathText(path));
    }

    return std::unique_ptr<FileMediaSource>(
        new FileMediaSource(std::move(stream), pathText(path), static_cast<uint64_t>(size)));
}

Result<size_t, MediaSourceError> FileMediaSource::read(std::span<std::byte> buffer)
{
    if (buffer.empty())
        return size_t{0};

    try
    {
        stream_.read(reinterpret_cast<char*>(buffer.data()),
                     static_cast<std::streamsize>(buffer.size()));
    }
    catch (const std::exception& e)
    {
        return fail(MediaSourceError::ReadFailed, name_ + ": " + e.what());
    }

    // eofbit with a short count is the end of the file, not a fault; badbit is
    // the only condition that means the read itself went wrong.
    if (stream_.bad())
        return fail(MediaSourceError::ReadFailed, name_);

    const auto count = static_cast<size_t>(stream_.gcount());
    if (stream_.eof())
        stream_.clear();

    position_ += count;
    return count;
}

Status<MediaSourceError> FileMediaSource::seek(uint64_t offset)
{
    if (offset > size_)
        return fail(MediaSourceError::OutOfRange,
                    name_ + " offset " + std::to_string(offset) + " of " +
                        std::to_string(size_));

    try
    {
        // A previous read that hit the end leaves eofbit set, and seekg on a
        // stream in a failed state is a no-op on some implementations.
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    }
    catch (const std::exception& e)
    {
        return fail(MediaSourceError::SeekFailed, name_ + ": " + e.what());
    }

    if (!stream_)
        return fail(MediaSourceError::SeekFailed,
                    name_ + " offset " + std::to_string(offset));

    position_ = offset;
    return {};
}

} // namespace multiroom
