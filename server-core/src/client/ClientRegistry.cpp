#include "client/ClientRegistry.h"

#include "util/Logger.h"

#include <algorithm>

namespace multiroom {

const char* errorName(RegistryError error)
{
    switch (error)
    {
    case RegistryError::Full:
        return "Full";
    case RegistryError::Unnamed:
        return "Unnamed";
    case RegistryError::NotPresent:
        return "NotPresent";
    }
    return "Unknown";
}

ClientRegistry::ClientRegistry(size_t maxClients)
    : maxClients_(maxClients == 0 ? kDefaultMaxClients : maxClients)
{
}

Result<std::shared_ptr<ClientSession>, RegistryError>
ClientRegistry::add(std::shared_ptr<ClientSession> session)
{
    if (!session || session->clientId().empty())
    {
        return fail(RegistryError::Unnamed,
                    "a session is keyed on the clientId its hello carried");
    }

    const ClientId id = session->clientId();
    const auto existing = sessions_.find(id);

    if (existing == sessions_.end() && sessions_.size() >= maxClients_)
    {
        ++refused_;
        return fail(RegistryError::Full, "already serving " + std::to_string(maxClients_) +
                                             " clients, refusing " + id.str());
    }

    std::shared_ptr<ClientSession> displaced;
    if (existing != sessions_.end())
    {
        // Section 43: the same room reconnecting. The old connection is gone
        // whether or not its socket has noticed.
        displaced = existing->second;
        existing->second = std::move(session);
        MULTIROOM_INFO("client {} reconnected; the previous session is replaced", id.str());
    }
    else
    {
        sessions_.emplace(id, std::move(session));
        MULTIROOM_INFO("client {} connected ({} of {})", id.str(), sessions_.size(), maxClients_);
    }

    return displaced;
}

bool ClientRegistry::remove(const ClientId& id, const ClientSession* session)
{
    const auto found = sessions_.find(id);
    if (found == sessions_.end())
        return false;
    // Only if it is still the one being removed. A connection that was
    // replaced tidies up after itself later, and must not take the live
    // session with it.
    if (session != nullptr && found->second.get() != session)
        return false;

    sessions_.erase(found);
    MULTIROOM_INFO("client {} disconnected ({} remaining)", id.str(), sessions_.size());
    return true;
}

std::shared_ptr<ClientSession> ClientRegistry::find(const ClientId& id) const
{
    const auto found = sessions_.find(id);
    return found == sessions_.end() ? nullptr : found->second;
}

std::vector<ClientId> ClientRegistry::ids() const
{
    std::vector<ClientId> result;
    result.reserve(sessions_.size());
    for (const auto& entry : sessions_)
        result.push_back(entry.first);
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace multiroom
