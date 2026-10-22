#pragma once

// Where a build option stops being visible.
//
// MULTIROOM_WITH_SOXR and MULTIROOM_WITH_R8BRAIN decide which implementations this
// binary contains, and this function is the only place above the adapters that
// knows either name exists. Whoever owns a configuration calls it once at
// startup and then works entirely in terms of registry ids -- an unknown id
// already fails with the ids that *are* in the build, which is the commonest
// way a config naming a resampler left out by a build option is met.
//
// FakeResampler is deliberately not among them. A room served by it would be
// an audible mistake, and the way to make that impossible is for production
// code never to put it in a registry; tests that want it add it themselves.

#include "resampler/ResamplerRegistry.h"

namespace multiroom {

// Adds every resampler this build contains. Fails only on DuplicateId, which
// means the registry already held one of these ids -- a caller registering
// twice, not a configuration problem.
Status<ResamplerError> registerBuiltinResamplers(ResamplerRegistry& registry);

} // namespace multiroom
