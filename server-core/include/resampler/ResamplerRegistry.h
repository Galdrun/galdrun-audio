#pragma once

// Every resampler implementation the process has, looked up by its id.
//
// This is the whole of what makes a further implementation cost nothing
// architecturally: it is registered here, it is named by a string in the
// config, and no other file changes.
//
// Threading: registration happens once, during startup, on the thread that
// owns the configuration. Lookup and create() happen later from a zone's or a
// client's thread. There is no mutex because there is no interleaving -- add()
// after the server is running is a defect, not a supported operation.

#include "resampler/IResamplerFactory.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace multiroom {

class ResamplerRegistry
{
public:
    ResamplerRegistry() = default;
    ResamplerRegistry(const ResamplerRegistry&) = delete;
    ResamplerRegistry& operator=(const ResamplerRegistry&) = delete;

    // Startup only. A second factory under an id already present is refused
    // rather than allowed to replace the first: which of the two a room got
    // would otherwise depend on registration order.
    Status<ResamplerError> add(std::unique_ptr<IResamplerFactory> factory);

    [[nodiscard]] bool contains(std::string_view id) const;

    // Sorted, because it is printed: in a startup log line, in status, and in
    // the detail of an UnknownId failure so the message names the alternatives.
    [[nodiscard]] std::vector<std::string> ids() const;

    [[nodiscard]] size_t size() const noexcept { return factories_.size(); }

    // What the planner asks before it commits a client to an implementation.
    [[nodiscard]] Result<ResamplerCapabilities, ResamplerError>
    capabilities(std::string_view id) const;

    [[nodiscard]] Result<std::unique_ptr<IAudioResampler>, ResamplerError>
    create(std::string_view id) const;

private:
    [[nodiscard]] std::string knownIds() const;

    // Ordered, so ids() needs no sort and iteration is reproducible between
    // runs -- a status response that reorders itself is a diff nobody wants.
    std::map<std::string, std::unique_ptr<IResamplerFactory>, std::less<>> factories_;
};

} // namespace multiroom
