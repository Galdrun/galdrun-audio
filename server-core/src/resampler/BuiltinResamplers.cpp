#include "resampler/BuiltinResamplers.h"

#if MULTIROOM_WITH_SOXR
#include "resampler/soxr/SoxrResampler.h"
#endif
#if MULTIROOM_WITH_R8BRAIN
#include "resampler/r8brain/R8BrainResampler.h"
#endif

#include <memory>

namespace multiroom {

Status<ResamplerError> registerBuiltinResamplers([[maybe_unused]] ResamplerRegistry& registry)
{
#if MULTIROOM_WITH_SOXR
    if (auto added = registry.add(std::make_unique<SoxrResamplerFactory>()); !added)
        return added;
#endif
#if MULTIROOM_WITH_R8BRAIN
    if (auto added = registry.add(std::make_unique<R8BrainResamplerFactory>()); !added)
        return added;
#endif

    // A build with both options off registers nothing and succeeds. That is
    // not a broken server -- it is one that can only serve rooms whose format
    // matches their zone's, and the phase 6 planner is where that becomes a
    // refused configuration rather than a silent resample.
    return {};
}

} // namespace multiroom
