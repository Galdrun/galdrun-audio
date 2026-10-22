#pragma once

// One decoded span of a zone's source audio.
//
// A block is written once and immutable from the moment it is published. It is
// then handed to every client output of the zone as
// std::shared_ptr<const AudioBlock>, which is what makes "one decode per zone"
// (the roadmap's criterion 15) a property of the type rather than a discipline:
// there is no copy to make and nothing for a room to modify.
//
// A block does **not** carry a timeline position. The source-to-timeline
// mapping can be re-anchored while the audio cannot, so the block knows only
// where it sits in its own source, and the zone owns the mapping.
//
// Storage belongs to the BlockPool that handed the block out, and is never
// moved or resized while the block exists; see BlockPool.h for why.

#include "audio/AudioFormat.h"
#include "core/Error.h"
#include "core/Units.h"

#include <cstddef>
#include <span>

namespace multiroom {

enum class BlockError
{
    NotCommitted,  // read before the producer published it
    AlreadyCommitted,
    InvalidFormat, // zero rate or zero channels
    TooLarge       // more frames than the pool's fixed block size holds
};

const char* errorName(BlockError error);

class AudioBlock
{
public:
    AudioBlock(const AudioBlock&) = delete;
    AudioBlock& operator=(const AudioBlock&) = delete;

    // --- write side, before the block is published ---------------------------

    // The whole fixed-size buffer. The producer decodes into it and then says
    // how much of it is audio.
    std::span<std::byte> writable() noexcept { return storage_; }

    size_t capacityBytes() const noexcept { return storage_.size(); }

    // How many frames of this format fit. Zero for an invalid format.
    uint32_t capacityFrames(const AudioFormat& format) const noexcept;

    // Fixes the format, the position and the length, after which the block is
    // read-only. Refused rather than truncated if the frames do not fit: a
    // short block would be silently wrong audio.
    Status<BlockError> commit(const AudioFormat& format, SourceFrame firstFrame,
                              uint32_t frameCount);

    bool committed() const noexcept { return committed_; }

    // --- read side -----------------------------------------------------------

    const AudioFormat& format() const noexcept { return format_; }
    SourceFrame firstFrame() const noexcept { return firstFrame_; }
    uint32_t frameCount() const noexcept { return frameCount_; }

    // The audio, and only the audio: the committed length, not the capacity.
    std::span<const std::byte> bytes() const noexcept;

private:
    friend class BlockPool;

    explicit AudioBlock(std::span<std::byte> storage) noexcept : storage_(storage) {}

    // Called by the pool when the last reference goes away, so the next
    // producer sees a block that cannot be mistaken for a published one.
    void reset() noexcept;

    std::span<std::byte> storage_;
    AudioFormat format_;
    SourceFrame firstFrame_{};
    uint32_t frameCount_ = 0;
    size_t byteCount_ = 0;
    bool committed_ = false;
};

} // namespace multiroom
