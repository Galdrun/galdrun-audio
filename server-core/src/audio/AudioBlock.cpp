#include "audio/AudioBlock.h"

#include <string>

namespace multiroom {

const char* errorName(BlockError error)
{
    switch (error)
    {
    case BlockError::NotCommitted:
        return "NotCommitted";
    case BlockError::AlreadyCommitted:
        return "AlreadyCommitted";
    case BlockError::InvalidFormat:
        return "InvalidFormat";
    case BlockError::TooLarge:
        return "TooLarge";
    }
    return "?";
}

uint32_t AudioBlock::capacityFrames(const AudioFormat& format) const noexcept
{
    if (!format.isValid())
        return 0;
    const uint32_t bytesPerFrame = format.bytesPerFrame();
    if (bytesPerFrame == 0)
        return 0;
    return static_cast<uint32_t>(storage_.size() / bytesPerFrame);
}

Status<BlockError> AudioBlock::commit(const AudioFormat& format, SourceFrame firstFrame,
                                      uint32_t frameCount)
{
    if (committed_)
        return fail(BlockError::AlreadyCommitted, "a block is written once");
    if (!format.isValid())
        return fail(BlockError::InvalidFormat, describeFormat(format));

    const size_t bytes = static_cast<size_t>(frameCount) * format.bytesPerFrame();
    if (bytes > storage_.size())
    {
        return fail(BlockError::TooLarge, std::to_string(bytes) + " bytes into a " +
                                              std::to_string(storage_.size()) + " byte block");
    }

    format_ = format;
    firstFrame_ = firstFrame;
    frameCount_ = frameCount;
    byteCount_ = bytes;
    committed_ = true;
    return {};
}

std::span<const std::byte> AudioBlock::bytes() const noexcept
{
    if (!committed_)
        return {};
    return storage_.subspan(0, byteCount_);
}

void AudioBlock::reset() noexcept
{
    format_ = AudioFormat{};
    firstFrame_ = SourceFrame{};
    frameCount_ = 0;
    byteCount_ = 0;
    committed_ = false;
}

} // namespace multiroom
