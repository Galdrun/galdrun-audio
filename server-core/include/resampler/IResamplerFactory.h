#pragma once

// How the registry makes one, without knowing what it is making.
//
// A factory rather than a create function because it also answers for its
// implementation before anything is built: OutputPlanner (phase 6) asks the
// registry what the configured drift resampler can do and refuses the
// configuration at startup if the answer is wrong, and it must be able to do
// that without constructing a converter for a client that may never connect.
//
// The factory is also where an implementation's build option stops: a
// MULTIROOM_WITH_SOXR that is off means its factory is never registered, and no
// caller above this header has to know the option exists.

#include "resampler/IAudioResampler.h"

#include <memory>

namespace multiroom {

class IResamplerFactory
{
public:
    virtual ~IResamplerFactory() = default;

    // Constant for the factory's lifetime. The id inside it is the registry
    // key, so it is fixed before registration and never changes after.
    [[nodiscard]] virtual const ResamplerCapabilities& capabilities() const noexcept = 0;

    // One per client output path. Unconfigured: the caller knows the rates.
    [[nodiscard]] virtual std::unique_ptr<IAudioResampler> create() const = 0;

protected:
    IResamplerFactory() = default;
    IResamplerFactory(const IResamplerFactory&) = delete;
    IResamplerFactory& operator=(const IResamplerFactory&) = delete;
};

} // namespace multiroom
