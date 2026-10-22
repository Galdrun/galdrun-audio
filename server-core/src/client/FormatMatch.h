#pragma once

// Ranking one client's offered formats against a source format.
//
// Two searches, both returning a pointer into the list they were given, or
// null. They are the *ordered rules* of phase 6 -- what "can play exactly"
// means and what to send when nothing can -- separated from OutputPlanner's
// ten rows because the two answer different questions: this file decides which
// format, and the planner decides whether anything may be converted at all.
//
// It lives beside its source rather than in include/ because nothing above the
// planner has any business choosing a room's format; the planner's OutputPlan
// is the only answer the rest of the server sees.

#include "audio/AudioFormat.h"
#include "client/OutputPlan.h"

#include <vector>

namespace multiroom::format_match {

// The best layout this client accepts that leaves the source's sample values
// alone, at the source's own rate and channel count -- "can play exactly" in
// the table OutputPlanner.h states. Null when there is none.
//
// Ranked by the bits a repack adds (none is best: the source's own depth is
// what the client asked to hear), then by whether the layout is identical
// rather than merely equivalent, then by the bytes it puts on the wire. So a
// device offering both s24le3 and s24le takes a packed 24-bit source
// unrepacked, one offering only s24le takes the repack, and a 16-bit source
// reaches a device offering int24 and int32 as int24 -- the extra sixteen bits
// carry nothing and cost half the bandwidth again.
[[nodiscard]] const AudioFormat* bestValuePreserving(const AudioFormat& source,
                                                     const std::vector<AudioFormat>& offered);

// What to send a client that cannot play the source, by the roadmap's ordered
// rules. Null when the client offers nothing at the source's channel count,
// which is the one case with no answer rather than a worse one.
//
//  1. The source's channel count, and only that count. Nothing in this server
//     mixes or reorders channels -- IAudioResampler converts one count to
//     itself -- so a plan naming a different one would be a plan the pipeline
//     cannot execute.
//  2. A rate the source rate divides into or that divides the source rate,
//     then preferredSampleRate, then the highest offered. An integer relation
//     is first because it is a quality argument and not only a cheap one: both
//     vendored libraries take a different and better path for 44.1 -> 88.2 kHz.
//     A multiple beats a divisor, which would throw bandwidth away, and the
//     smallest multiple wins, because past the first there is nothing further
//     to preserve.
//  3. The most bits at or above the source's effective depth, reducing depth
//     only where the client offers nothing else. Bits above the source's depth
//     are not waste here: this format is for a room being *resampled*, so the
//     samples are the converter's rather than the source's, and the rounding
//     at the output end is a real loss. Integer beats float at the same width
//     -- a float32 mantissa carries 24 bits where an int32 carries 32 -- and at
//     the same depth the narrower container wins, being equally exact and
//     smaller on the wire.
//
// Every comparison is strict, so equal candidates leave the one the client
// listed first: the only tie-break a capability list offers.
[[nodiscard]] const AudioFormat* bestFallback(const AudioFormat& source,
                                              const ClientCapabilities& capabilities);

} // namespace multiroom::format_match
