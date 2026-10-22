#pragma once

// What the high-end server hands this one: a URI and where it begins.
//
// This is the only playlist-shaped concept in the whole server, and it is not
// a playlist. The core never chooses the next segment, never reorders, never
// repeats and never looks one up -- it plays what it was given, in the order
// it was given, and stops when it runs out. Everything a playlist implies --
// providers, accounts, shuffle, "what comes next" -- belongs to the server
// above this one, and the line is drawn here on purpose.
//
// Several may be staged ahead of the one playing, because that is what gapless
// playback needs: the pipeline opens the next source while the current one is
// still being decoded, and the timeline carries on rather than restarting
// (MATP sections 22 and 23).
//
// `timelineStartNs` is not an input. Where a source begins follows from where
// the previous one ended, and the previous one's length is not known until it
// has been decoded -- so the zone fills it in when it opens the segment, and
// `StagedSource` below is what it reports back.

#include "audio/AudioFormat.h"
#include "core/Units.h"

#include <cstdint>
#include <optional>
#include <string>

namespace multiroom {

struct SourceSegment
{
    // The high-end server's handle for this staged source. Required: every
    // failure and every status line has to name one source rather than "the
    // zone".
    std::string sourceId;

    // What to play. A local path, or http:// and https:// -- the decoder
    // factory is the only thing that knows which is which.
    std::string mediaUri;

    // Optional, and carried rather than used: TRACK_BOUNDARY (section 23)
    // carries them to the rooms in phase 14, and status reports them meanwhile.
    // The core attaches no meaning to either.
    std::string trackId;
    std::string title;

    // Phase 26: where in the source to begin, zero being its first frame --
    // a track resumed where it was left, and a seek, which re-opens the
    // source that is audible at the offset asked for. It moves nothing on
    // the timeline: the source is heard from wherever the zone places it,
    // beginning with the frame this offset falls in.
    uint64_t startOffsetNs = 0;
};

// A segment the zone has opened, with the three things that were not known
// until it did.
struct StagedSource
{
    SourceSegment segment;

    // Where this source's first frame is heard. The first source of a zone is
    // heard at zero (section 5) and every later one continues from where its
    // predecessor ended.
    TimelineNs timelineStartNs{};

    // Where it begins in the store's own frame numbering, which is continuous
    // across sources: the store holds one contiguous run of frames and the
    // segment table beside it is what says which source each belongs to.
    SourceFrame firstFrame{};

    // As the decoder found it. Never converted on the way in -- phase 4's
    // native-PCM contract -- so this is the format every room of this zone is
    // planned against.
    AudioFormat format;

    // Decoded so far. Final once `ended` is true, which is also when the
    // source's length on the timeline stops moving.
    uint64_t frames = 0;
    bool ended = false;

    // Its length from `segment.startOffsetNs` on, as the container states the
    // whole of it, absent for a live stream or a container that does not say.
    // What phase 24's `remainingNs` is measured from, which since phase 26 is
    // what is left to play rather than what the file holds.
    std::optional<uint64_t> totalFrames;
};

} // namespace multiroom
