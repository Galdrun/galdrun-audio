#pragma once

// A source over a local file.
//
// std::ifstream rather than the platform's own handle, for the reason
// util/FileUtil gives: the standard library is the platform API the project
// rule asks for first, and this server has none of the client's toolchain
// constraints. The streams throw, so this module catches at its own edge and
// no exception leaves it.
//
// It bounds no wait, and says so rather than pretending: a blocking read of a
// local file has nothing this process can interrupt, and a source that claimed
// a deadline it cannot enforce would be worse than one that does not claim it.
// A network filesystem behind a local path is the case this does not cover,
// and the honest answer there is a URI and HttpMediaSource.

#include "source/IMediaSource.h"

#include <filesystem>
#include <fstream>

namespace multiroom {

class FileMediaSource final : public IMediaSource
{
public:
    // Opens and stats the file up front, so a path that is wrong fails where
    // the zone is being configured rather than at the first read.
    static Result<std::unique_ptr<FileMediaSource>, MediaSourceError>
    open(const std::filesystem::path& path);

    const std::string& describe() const noexcept override { return name_; }
    bool seekable() const noexcept override { return true; }
    std::optional<uint64_t> sizeBytes() const noexcept override { return size_; }
    uint64_t position() const noexcept override { return position_; }

    Result<size_t, MediaSourceError> read(std::span<std::byte> buffer) override;
    Status<MediaSourceError> seek(uint64_t offset) override;

private:
    FileMediaSource(std::ifstream stream, std::string name, uint64_t size);

    std::ifstream stream_;
    std::string name_;
    uint64_t size_ = 0;
    uint64_t position_ = 0;
};

} // namespace multiroom
