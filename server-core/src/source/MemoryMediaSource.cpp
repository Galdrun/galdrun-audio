#include "source/MemoryMediaSource.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace multiroom {

MemoryMediaSource::MemoryMediaSource(std::vector<std::byte> bytes, std::string name)
    : bytes_(std::move(bytes)), name_(std::move(name))
{
}

Result<size_t, MediaSourceError> MemoryMediaSource::read(std::span<std::byte> buffer)
{
    const uint64_t remaining = bytes_.size() - position_;
    const size_t count = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
    if (count > 0)
        std::memcpy(buffer.data(), bytes_.data() + position_, count);
    position_ += count;
    return count;
}

Status<MediaSourceError> MemoryMediaSource::seek(uint64_t offset)
{
    // Seeking to exactly the end is how avformat asks "is there anything
    // left"; only past it is wrong.
    if (offset > bytes_.size())
        return fail(MediaSourceError::OutOfRange,
                    name_ + " offset " + std::to_string(offset) + " of " +
                        std::to_string(bytes_.size()));

    position_ = offset;
    return {};
}

} // namespace multiroom
