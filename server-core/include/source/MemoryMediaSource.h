#pragma once

// A source over bytes that are already in memory.
//
// It is not a convenience for tests that happens to exist: it is what makes
// the decoder testable at all. Every phase-4 decoder test hands avformat a
// fixture this class holds, so the tests need no file on disk, no network and
// no fixture checked in, and a decode test is as fast as a unit test rather
// than as slow as a filesystem.
//
// It owns its bytes. A span into a caller's buffer would be cheaper and would
// hand the pipeline a dangling source the first time a staged segment outlived
// the vector it came from.

#include "source/IMediaSource.h"

#include <vector>

namespace multiroom {

class MemoryMediaSource final : public IMediaSource
{
public:
    // `name` is what describe() reports, so a failure names this fixture
    // rather than "memory".
    MemoryMediaSource(std::vector<std::byte> bytes, std::string name);

    const std::string& describe() const noexcept override { return name_; }
    bool seekable() const noexcept override { return true; }
    std::optional<uint64_t> sizeBytes() const noexcept override { return bytes_.size(); }
    uint64_t position() const noexcept override { return position_; }

    Result<size_t, MediaSourceError> read(std::span<std::byte> buffer) override;
    Status<MediaSourceError> seek(uint64_t offset) override;

private:
    std::vector<std::byte> bytes_;
    std::string name_;
    uint64_t position_ = 0;
};

} // namespace multiroom
