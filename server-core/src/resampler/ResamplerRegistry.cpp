#include "resampler/ResamplerRegistry.h"

namespace multiroom {

Status<ResamplerError> ResamplerRegistry::add(std::unique_ptr<IResamplerFactory> factory)
{
    if (factory == nullptr)
        return fail(ResamplerError::UnknownId, "null factory");

    std::string id = factory->capabilities().id;
    if (id.empty())
        return fail(ResamplerError::UnknownId, "a factory with no id");
    if (factories_.find(id) != factories_.end())
        return fail(ResamplerError::DuplicateId, id);

    factories_.emplace(std::move(id), std::move(factory));
    return {};
}

bool ResamplerRegistry::contains(std::string_view id) const
{
    return factories_.find(id) != factories_.end();
}

std::vector<std::string> ResamplerRegistry::ids() const
{
    std::vector<std::string> out;
    out.reserve(factories_.size());
    for (const auto& entry : factories_)
        out.push_back(entry.first);
    return out;
}

Result<ResamplerCapabilities, ResamplerError>
ResamplerRegistry::capabilities(std::string_view id) const
{
    const auto entry = factories_.find(id);
    if (entry == factories_.end())
        return fail(ResamplerError::UnknownId, std::string(id) + "; known: " + knownIds());
    return entry->second->capabilities();
}

Result<std::unique_ptr<IAudioResampler>, ResamplerError>
ResamplerRegistry::create(std::string_view id) const
{
    const auto entry = factories_.find(id);
    if (entry == factories_.end())
        return fail(ResamplerError::UnknownId, std::string(id) + "; known: " + knownIds());
    return entry->second->create();
}

std::string ResamplerRegistry::knownIds() const
{
    if (factories_.empty())
        return "none";

    std::string out;
    for (const auto& entry : factories_)
    {
        if (!out.empty())
            out += ", ";
        out += entry.first;
    }
    return out;
}

} // namespace multiroom
