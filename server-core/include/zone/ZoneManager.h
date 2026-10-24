#pragma once

// Every zone this server has, and which zone each room is in.
//
// Two maps and the rule that keeps them honest. `zones` is the obvious one;
// the second exists because the two listeners route by room and know nothing
// about zones: `STREAM_ATTACH` carries a client id, `BUFFER_STATUS` arrives on
// one room's session, and a session closing is one room going away. Scanning
// every zone for each of those would make a room's cost depend on how many
// zones the house has, and would put the same lookup in three call sites.
//
// **A room is in at most one zone**, which is what makes the index a map
// rather than a multimap: one room is one output stream, and a room in two
// zones would be two streams on one connection. `assign()` is the only thing
// that writes it, and it writes both maps or neither.
//
// **An idle zone costs an entry in a map.** No members and no staged source
// means no decoder, no store past its lookahead and no thread -- a zone is
// created when the high-end server names one, not when it starts playing.
//
// **Threading.** The control listener's thread calls `assign()` and
// `release()`, the audio listener's calls `zoneOf()`, and whatever owns the
// server creates and removes zones. The mutex here guards the two maps for a
// lookup or a shared_ptr copy and nothing else; every call that stops a zone
// or a room does it outside the lock, because those join threads.

#include "core/Error.h"
#include "core/Ids.h"
#include "zone/Zone.h"

#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace multiroom {

class ZoneManager
{
public:
    // Memory is bounded, and this is the ceiling. Sixteen zones against the
    // ten rooms the roadmap's threading section sizes for is far more than a
    // house needs, and each idle one is a map entry.
    static constexpr size_t kDefaultMaxZones = 16;

    // Everything here outlives the manager and every zone in it: `memory` is
    // the process's one audio ceiling, which every zone's own blocks are
    // counted against (phase 42), the planner is the server's one, which
    // resampler.configure may replace, and the factory is what turns a staged
    // URI into a decoder.
    ZoneManager(AudioMemory& memory, const SharedPlanner& planner, const ResamplerRegistry& registry,
                IDecoderFactory& decoders, const IClock& clock, ZoneSettings defaults = {},
                size_t maxZones = kDefaultMaxZones);

    // Stops every zone, which stops every room in it.
    ~ZoneManager();

    ZoneManager(const ZoneManager&) = delete;
    ZoneManager& operator=(const ZoneManager&) = delete;

    // Creates an idle zone under `id`. An id that already exists is refused
    // rather than replaced: the rooms in the first one would otherwise be
    // playing a timeline nothing holds any more.
    [[nodiscard]] Result<std::shared_ptr<Zone>, ZoneError> create(const ZoneId& id);

    [[nodiscard]] std::shared_ptr<Zone> find(const ZoneId& id) const;

    // Stops the zone and every room in it, and forgets both maps' entries for
    // it. False when there is no such zone.
    bool remove(const ZoneId& id);

    [[nodiscard]] std::vector<ZoneId> ids() const;
    [[nodiscard]] size_t size() const;

    // What every zone is created with. Fixed for the manager's life, so it
    // needs no lock -- which is what lets the control API bound a room's
    // setting by it before the room is in any zone.
    [[nodiscard]] const ZoneSettings& defaults() const noexcept { return defaults_; }

    // The control API's audio.configure: every zone now and every zone made
    // from here on. Kept beside the defaults rather than in them, so the
    // promise above -- fixed, no lock -- still holds for the rest.
    void setTiming(const ZoneTiming& timing);
    [[nodiscard]] ZoneTiming timing() const;

    // --- membership ----------------------------------------------------------

    // Puts a room in a zone and starts its output. A room already in another
    // zone is released from it first, because membership is exclusive and a
    // move between zones is the case section 44's late join is written for --
    // so an admit that then fails leaves the room in no zone at all, which is
    // the honest outcome: it is no longer being sent the stream it left.
    [[nodiscard]] Status<ZoneError> assign(const ZoneId& zone, const SessionId& session,
                                           const ClientId& client,
                                           const ClientCapabilities& capabilities,
                                           const AudioFormat& grantedFormat);

    // Takes a room out of whatever zone it is in, for good. Silent when it is
    // in none. This is the high-end server changing a zone's membership, not a
    // room whose connection went away.
    void release(const ClientId& client);

    // Its control connection went away (section 43). The room leaves the
    // membership index exactly as release() leaves it -- so its reconnect is
    // an ordinary assign() and an ordinary admit(), which is the path section
    // 43 describes -- but the zone keeps what belongs to the room rather than
    // to the socket, and restores it when that admit() comes. Silent when the
    // room is in no zone, which is the normal case for a session that closed
    // before it was ever assigned one. With `session`, a room another session
    // now holds is left where it is.
    void disconnect(const ClientId& client,
                    const std::optional<SessionId>& session = std::nullopt);

    // The zone a room is in, or null. This is what the audio listener follows
    // to find the room a STREAM_ATTACH belongs to.
    [[nodiscard]] std::shared_ptr<Zone> zoneOf(const ClientId& client) const;

    // Stops every zone without forgetting it, for the shutdown order in
    // main.cpp: zones stop before the listeners that feed them.
    void stop();

    // The ceiling every zone's blocks are counted against: what `status`
    // reports and `audio.configure` moves (phase 42).
    [[nodiscard]] AudioMemory& memory() const noexcept { return memory_; }

private:
    AudioMemory& memory_;
    const SharedPlanner& planner_;
    const ResamplerRegistry& registry_;
    IDecoderFactory& decoders_;
    const IClock& clock_;
    ZoneSettings defaults_;
    size_t maxZones_;

    mutable std::mutex mutex_;
    std::map<ZoneId, std::shared_ptr<Zone>> zones_; // guarded by mutex_
    ZoneTiming timing_;                              // guarded by mutex_
    std::map<ClientId, ZoneId> membership_;         // guarded by mutex_
};

} // namespace multiroom
