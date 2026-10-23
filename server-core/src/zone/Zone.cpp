#include "zone/Zone.h"

#include "util/Logger.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <utility>

namespace multiroom {
namespace {

// How long the pipeline waits before looking again when there was nothing to
// do. It is not a cadence: one pass decodes as much as the lookahead allows,
// which at the start of a zone is the whole prebuffer at once. What it decides
// is how quickly the decode notices that a room has drained and wants more,
// and five milliseconds is far finer than the four buffer statuses a second
// that move the answer.
constexpr auto kPollInterval = std::chrono::milliseconds(5);

// A source's length from where it was opened (phase 26): the container states
// the whole of it, and a source opened part-way has that much less to play.
std::optional<uint64_t> remainingFrames(const SourceStreamInfo& info, uint64_t startOffsetNs)
{
    if (!info.totalFrames)
        return std::nullopt;
    const uint64_t skipped =
        framesFromNanoseconds(startOffsetNs, info.format.sampleRate).value_or(*info.totalFrames);
    return *info.totalFrames - std::min(*info.totalFrames, skipped);
}

} // namespace

const char* errorName(ZoneError error)
{
    switch (error)
    {
    case ZoneError::InvalidSettings:
        return "InvalidSettings";
    case ZoneError::InvalidSegment:
        return "InvalidSegment";
    case ZoneError::NotPlaying:
        return "NotPlaying";
    case ZoneError::AlreadyPlaying:
        return "AlreadyPlaying";
    case ZoneError::NothingStaged:
        return "NothingStaged";
    case ZoneError::SourceFailed:
        return "SourceFailed";
    case ZoneError::StoreFailed:
        return "StoreFailed";
    case ZoneError::TimelineFailed:
        return "TimelineFailed";
    case ZoneError::PlanFailed:
        return "PlanFailed";
    case ZoneError::OutputFailed:
        return "OutputFailed";
    case ZoneError::AlreadyMember:
        return "AlreadyMember";
    case ZoneError::NotMember:
        return "NotMember";
    case ZoneError::AlreadyExists:
        return "AlreadyExists";
    case ZoneError::Full:
        return "Full";
    case ZoneError::NotStarted:
        return "NotStarted";
    case ZoneError::InvalidCommand:
        return "InvalidCommand";
    case ZoneError::NotSeekable:
        return "NotSeekable";
    case ZoneError::InvalidPosition:
        return "InvalidPosition";
    }
    return "?";
}

const char* zoneStateName(ZoneState state)
{
    switch (state)
    {
    case ZoneState::Idle:
        return "idle";
    case ZoneState::Playing:
        return "playing";
    case ZoneState::Ended:
        return "ended";
    case ZoneState::Failed:
        return "failed";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------

Zone::Zone(ZoneId id, AudioMemory& memory, const OutputPlanner& planner,
           const ResamplerRegistry& registry, IDecoderFactory& decoders, const IClock& clock,
           ZoneSettings settings, uint64_t lookaheadNs, uint64_t lateJoinLeadNs,
           std::unique_ptr<NativePcmStore> store)
    : id_(std::move(id)), memory_(memory), planner_(planner), registry_(registry),
      decoders_(decoders), clock_(clock), settings_(settings), lookaheadNs_(lookaheadNs),
      lateJoinLeadNs_(lateJoinLeadNs), pool_(BlockPool::create(memory)),
      store_(std::move(store)),
      scheduler_(settings.start)
{
}

Zone::~Zone()
{
    stop();
}

Result<std::unique_ptr<Zone>, ZoneError> Zone::create(ZoneId id, AudioMemory& memory,
                                                      const OutputPlanner& planner,
                                                      const ResamplerRegistry& registry,
                                                      IDecoderFactory& decoders,
                                                      const IClock& clock, ZoneSettings settings)
{
    if (id.empty())
        return fail(ZoneError::InvalidSettings, "a zone needs an id");
    if (settings.output.packetDurationMs == 0 || settings.output.targetBufferNs == 0)
        return fail(ZoneError::InvalidSettings,
                    "packetDurationMs and targetBufferNs must both be non-zero");
    if (settings.retentionNs == 0 || settings.maxMembers == 0)
        return fail(ZoneError::InvalidSettings,
                    "retentionNs and maxMembers must both be non-zero");
    if (!settings.sync.isValid())
        return fail(ZoneError::InvalidSettings,
                    "the sync controller's window, gains and limits are not usable");

    const uint64_t packetNs = static_cast<uint64_t>(settings.output.packetDurationMs) * 1000000ULL;
    const uint64_t lookaheadNs =
        settings.lookaheadNs != 0 ? settings.lookaheadNs : settings.output.targetBufferNs + packetNs;

    // A room reads the oldest block this zone still holds, so decoding further
    // ahead than the store retains would drop audio out from under a room that
    // has done nothing wrong. The two are settings rather than constants, and
    // this is the one relation between them.
    if (lookaheadNs >= settings.retentionNs)
    {
        return fail(ZoneError::InvalidSettings,
                    "a lookahead of " + std::to_string(lookaheadNs / 1000000ULL) +
                        " ms does not fit in a retention of " +
                        std::to_string(settings.retentionNs / 1000000ULL) + " ms");
    }

    // A zone that cannot start a room is a zone that cannot play, and the
    // barrier is five seconds too late to find that out.
    if (!settings.sendCommand)
        return fail(ZoneError::InvalidSettings, "a zone needs somewhere to send its messages");

    // Section 44's future position, and the second relation between the
    // retention and the lookahead. A latecomer's stream begins ahead of the
    // playhead, and the decode runs one lookahead past the room that is
    // furthest ahead -- which is that latecomer. So `lead + prebuffer +
    // lookahead` is how far the frontier runs past what is audible, and
    // anything the retention does not cover is a room that was playing being
    // evicted for somebody else's arrival.
    //
    // The prebuffer target is part of that sum and phase 11 left it out, which
    // the first two-room run found in under a second: a room that is playing
    // is allowed to hold anywhere between nothing and the full target, so the
    // position that must survive a latecomer's arrival is the **playhead**,
    // not the cursor of a room that happens to be full. Measuring from a full
    // room instead leaves exactly one packet of margin, and a room whose
    // buffer had dipped 140 ms below target was evicted for somebody else's
    // arrival -- which is the one thing the retention exists to prevent.
    const uint64_t reservedNs = lookaheadNs + settings.output.targetBufferNs + packetNs;
    const uint64_t lateJoinLeadNs =
        settings.lateJoinLeadNs != 0
            ? settings.lateJoinLeadNs
            : (settings.retentionNs > reservedNs ? settings.retentionNs - reservedNs : 0);
    if (lateJoinLeadNs == 0 ||
        lateJoinLeadNs + lookaheadNs + settings.output.targetBufferNs >= settings.retentionNs)
    {
        return fail(ZoneError::InvalidSettings,
                    "a late join lead of " + std::to_string(lateJoinLeadNs / 1000000ULL) +
                        " ms, a lookahead of " + std::to_string(lookaheadNs / 1000000ULL) +
                        " ms and a prebuffer of " +
                        std::to_string(settings.output.targetBufferNs / 1000000ULL) +
                        " ms do not fit in a retention of " +
                        std::to_string(settings.retentionNs / 1000000ULL) + " ms");
    }

    // The block ceiling follows this zone's own pool from its first source on
    // (phase 42).
    auto store = NativePcmStore::create(settings.retentionNs);
    if (!store)
        return fail(ZoneError::StoreFailed, describe(store.error()));

    return std::unique_ptr<Zone>(new Zone(std::move(id), memory, planner, registry, decoders, clock,
                                          settings, lookaheadNs, lateJoinLeadNs,
                                          std::move(*store)));
}

RoomAudioSettings Zone::settingsFor(const ClientId& client) const
{
    return settings_.roomPreference ? settings_.roomPreference(client)
                                    : RoomAudioSettings{settings_.audioControl};
}

// ---------------------------------------------------------------------------

Status<ZoneError> Zone::stage(SourceSegment segment)
{
    if (segment.sourceId.empty())
        return fail(ZoneError::InvalidSegment, "a staged source needs a sourceId");
    if (segment.mediaUri.empty())
        return fail(ZoneError::InvalidSegment,
                    "source " + segment.sourceId + " has no mediaUri");

    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == ZoneState::Failed)
        return fail(ZoneError::NotPlaying, lastError_);

    // A zone that had run out of staged sources is not finished with: the
    // high-end server may stage the next one at any time, and the timeline
    // carries on from where the last one ended rather than restarting.
    if (state_ == ZoneState::Ended)
    {
        state_ = ZoneState::Playing;
        publishEndLocked();
    }
    endAnnounced_ = false;

    staged_.push_back(std::move(segment));
    return {};
}

size_t Zone::stagedCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return staged_.size();
}

std::vector<SourceSegment> Zone::staged() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return {staged_.begin(), staged_.end()};
}

size_t Zone::clearStaged()
{
    std::vector<ZoneEvent> events;
    size_t cleared = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cleared = staged_.size();
        staged_.clear();

        // Phase 24: the list behind a source that is still playing has just
        // become empty, which is the question sourceNeeded asks -- asked
        // again, because the answer the high-end server gave has been taken
        // back.
        if (cleared != 0 && !sources_.empty() && !sources_.back().ended)
        {
            const StagedSource& current = sources_.back();
            ZoneEvent event = eventOf(ZoneEventKind::SourceNeeded);
            event.sourceId = current.segment.sourceId;
            if (current.totalFrames)
            {
                const uint64_t left =
                    *current.totalFrames - std::min(*current.totalFrames, current.frames);
                event.remainingNs =
                    nanosecondsFromFrames(left, current.format.sampleRate).value_or(0);
            }
            events.push_back(std::move(event));
        }
    }
    emit(events);
    return cleared;
}

void Zone::pruneSourcesLocked()
{
    // A long-running zone opens a source per track and per seek. The front of
    // the history is only ever read for the source a position falls in, and a
    // source whose successor the line has already reached is no position's
    // any more -- so beyond a short tail kept for status and replayed requests,
    // it goes.
    constexpr size_t kKeptSources = 32;
    size_t drop = 0;
    while (sources_.size() - drop > kKeptSources && startedThrough_ - drop >= 2)
        ++drop;
    if (drop == 0)
        return;
    sources_.erase(sources_.begin(), sources_.begin() + static_cast<std::ptrdiff_t>(drop));
    startedThrough_ -= drop;
}

std::vector<StagedSource> Zone::sources() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_;
}

// ---------------------------------------------------------------------------

Status<ZoneError> Zone::play()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == ZoneState::Playing)
            return fail(ZoneError::AlreadyPlaying, "zone " + id_.str() + " is already playing");
        if (state_ == ZoneState::Failed)
            return fail(ZoneError::SourceFailed, lastError_);
        if (staged_.empty())
            return fail(ZoneError::NothingStaged,
                        "zone " + id_.str() + " has no source to play");
        state_ = ZoneState::Playing;
    }

    // Opened here rather than on the pipeline thread: a URI that cannot be
    // played has to fail where the zone is being configured, and no room can
    // be planned until the source format is known.
    Result<bool, ZoneError> opened = false;
    {
        std::lock_guard<std::mutex> pipeline(pipelineMutex_);
        opened = openNext();
    }
    if (!opened)
    {
        // Phase 42: no memory for this source's format. The zone is where it
        // was, with the source still staged, and the caller hears why.
        if (opened.error().code == ZoneError::StoreFailed)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == ZoneState::Playing)
                state_ = ZoneState::Idle;
        }
        return std::unexpected(opened.error());
    }
    if (!*opened)
    {
        // Everything staged was skipped. openNext() called that the zone's
        // end, but a play() that opened nothing has not started anything to
        // end: the zone is where it was, and the caller hears why.
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = ZoneState::Idle;

        // Or the staged sources were cleared between the check above and the
        // open, and nothing was skipped at all.
        if (failed_.empty())
            return fail(ZoneError::NothingStaged,
                        "zone " + id_.str() + " has no source to play");
        const FailedSource& last = failed_.back();
        return fail(ZoneError::SourceFailed, last.sourceId + ": " + last.reason);
    }

    // A zone that was stopped rather than created: every room of it was parked
    // where section 38 left it, and the audio between there and here belongs
    // to a timeline nobody played. Each of them begins again at the first
    // position of the source that has just been opened.
    TimelineNs base{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sources_.empty())
            base = sources_.back().timelineStartNs;
    }
    restartParkedMembers(base);

    return {};
}

void Zone::restartParkedMembers(TimelineNs position)
{
    // One move at a time, and against a disconnect stopping a room this starts.
    std::lock_guard<std::mutex> moving(rejoinMutex_);

    std::vector<std::pair<ClientId, std::shared_ptr<ClientOutput>>> parked;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : members_)
        {
            if (entry.second.output->state() == OutputState::Parked)
                parked.emplace_back(entry.first, entry.second.output);
        }
    }

    // Outside the lock: rejoinAt() moves a store cursor and run() starts a
    // thread, and neither is work to do while holding a lock the two
    // listeners want.
    for (auto& entry : parked)
    {
        // Before the move, not after: the room goes on reporting the line the
        // stop abandoned until the STOP_AT comes round, and the barrier of this
        // play must not form on one of those reports.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            scheduler_.rejoin(entry.first, position);
        }
        if (const auto moved = entry.second->rejoinAt(position); !moved)
        {
            // That room's failure and not the zone's (the fault-isolation
            // table's second row): it stays parked and silent, and every other
            // room of this zone plays.
            MULTIROOM_WARN("zone {}: {} could not be moved to timeline {} ns: {}", id_.str(),
                     entry.first.str(), std::to_underlying(position), describe(moved.error()));
            continue;
        }
        if (const auto running = entry.second->run(); !running)
        {
            MULTIROOM_WARN("zone {}: {} could not be restarted: {}", id_.str(), entry.first.str(),
                     describe(running.error()));
        }
    }
}

Status<ZoneError> Zone::run()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != ZoneState::Playing)
            return fail(ZoneError::NotPlaying,
                        "zone " + id_.str() + " is " + zoneStateName(state_));
    }
    if (thread_.joinable())
        return {};

    thread_ = std::jthread([this](std::stop_token stop) {
        while (!stop.stop_requested())
        {
            pump();
            std::this_thread::sleep_for(kPollInterval);
        }
    });
    return {};
}

void Zone::stop()
{
    // The roadmap's order for a zone, and the reason it is an order: the rooms
    // read what the pipeline writes, and a room stopped first can never be
    // handed a block from a pipeline that has already gone.
    std::map<ClientId, Member> ending;
    {
        std::lock_guard<std::mutex> moving(rejoinMutex_); // a move in flight finishes first
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ending.swap(members_);
        }
        for (auto& entry : ending)
            entry.second.output->stop();
    }

    thread_.request_stop();
    if (thread_.joinable())
        thread_.join();

    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == ZoneState::Playing)
        state_ = ZoneState::Idle;
}

void Zone::dispatch(std::vector<RoomCommand> commands)
{
    if (commands.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (RoomCommand& command : commands)
        {
            // The one field of every one of these messages this zone owns. A
            // room whose session has been replaced since the decision is one
            // the sender will not find, which is the reconnect of section 43
            // and not a message to force onto a new session.
            const auto found = members_.find(command.client);
            if (found == members_.end())
                continue;

            const std::string session = found->second.session.toHex();
            switch (command.kind)
            {
            case RoomCommandKind::Start:
                command.start.sessionId = session;
                break;
            case RoomCommandKind::Pause:
            case RoomCommandKind::Stop:
            case RoomCommandKind::Flush:
                command.at.sessionId = session;
                break;
            case RoomCommandKind::Volume:
                command.volume.sessionId = session;
                break;
            case RoomCommandKind::TrackInfo:
                command.info.sessionId = session;
                break;
            case RoomCommandKind::TrackBoundary:
                command.boundary.sessionId = session;
                break;
            case RoomCommandKind::DriftFeedback:
                command.driftFeedback.sessionId = session;
                break;
            case RoomCommandKind::SetDriftCorrection: // the control API's
            case RoomCommandKind::Renegotiate:
            case RoomCommandKind::SetAudioDevice: // the control API's, never a zone's
            case RoomCommandKind::RescanAudio:
            case RoomCommandKind::SetBuffer: // the control API's too
            case RoomCommandKind::SetVolumeControl:
            case RoomCommandKind::SetInputBindings:
            case RoomCommandKind::SetRoomName:
            case RoomCommandKind::WakeStream: // the control API's, sent before a play
                break; // no payload: the session writes its own id
            }
        }
    }

    // Outside the lock: the sender reaches another thread's mailbox, and a
    // zone that held its own lock across that would be holding it across
    // whatever that thread decides to do about a room that has gone.
    for (const RoomCommand& command : commands)
    {
        if (!settings_.sendCommand(command))
        {
            MULTIROOM_WARN("zone {}: {} could not be sent a {}", id_.str(), command.client.str(),
                     roomCommandKindName(command.kind));
        }
    }
}

void Zone::schedule()
{
    // Before the barrier is evaluated, so that a line with nobody left on it
    // stops at the boundary in the same pass that noticed it got there.
    advanceTransitions();

    std::vector<RoomCommand> commands;
    std::vector<ClientId> wanted;
    std::vector<ZoneEvent> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t nowNs = clock_.monotonicNowNs();
        commands = scheduler_.evaluate(nowNs);
        wanted = scheduler_.takeRejoins();
        noticeLocked(events);

        // Phase 39: the line has moved on, and so has how far each room may
        // be built ahead of it -- and a room that is playing on it is paced on
        // it when its buffer reports stop arriving.
        const bool running = scheduler_.transport() == TransportState::Playing;
        for (const auto& entry : members_)
        {
            entry.second.output->setCeiling(ceilingLocked(entry.second.latencyNs, nowNs));
            entry.second.output->setOnLine(
                running && scheduler_.roomState(entry.first) == RoomStartState::Playing);
        }
        publishEndLocked();
    }
    dispatch(std::move(commands));
    emit(events);

    // The two triggers this loop is the one to notice (phase 16). Both end in
    // the same move and differ only in which end saw it first: the line has
    // run past everything one room still holds, or the decode has run past a
    // room that stopped draining long enough for the store to drop its audio.
    // Each waits out its room's back-off (phase 41). Outside the lock, because
    // a move stops that room's thread.
    std::vector<ZoneEvent> settled;
    const std::vector<std::pair<ClientId, RejoinTrigger>> due = dueMoves(wanted, settled);
    emit(settled);
    for (const auto& [client, trigger] : due)
        rejoinOrWarn(client, trigger);
}

// ---------------------------------------------------------------------------

Result<TimelineNs, ZoneError> Zone::pausePlayback()
{
    std::vector<RoomCommand> commands;
    std::vector<ZoneEvent> events;
    TimelineNs position{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (scheduler_.transport() != TransportState::Playing)
        {
            return fail(ZoneError::NotStarted,
                        "zone " + id_.str() + "'s timeline is " +
                            transportStateName(scheduler_.transport()));
        }
        commands = scheduler_.pause(clock_.monotonicNowNs(), position);
        noticeLocked(events);
    }

    // Nothing else moves, and that is the whole of section 37 from this end:
    // the rooms stop draining, so phase 9's pacing rule stops producing for
    // them, so this zone's demand-driven decode stops as well. A pause that
    // also touched a queue or a cursor would be throwing away the audio the
    // resume is made of.
    dispatch(std::move(commands));
    emit(events);
    return position;
}

Status<ZoneError> Zone::resumePlayback()
{
    std::vector<ZoneEvent> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (scheduler_.transport() != TransportState::Paused)
        {
            return fail(ZoneError::NotStarted, "zone " + id_.str() + "'s timeline is " +
                                                   transportStateName(scheduler_.transport()));
        }

        // Nothing is sent from here. A resume is a start, so it goes through
        // the barrier, and the barrier is resolved by the next pump().
        scheduler_.resume();
        noticeLocked(events);
    }
    emit(events);
    return {};
}

Result<TimelineNs, ZoneError> Zone::stopPlayback()
{
    std::vector<RoomCommand> commands;
    std::vector<ZoneEvent> events;
    TimelineNs position{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (scheduler_.transport() != TransportState::Stopped)
            commands = scheduler_.stop(clock_.monotonicNowNs(), position);
        else if (state_ != ZoneState::Playing && state_ != ZoneState::Ended)
            return fail(ZoneError::NotStarted,
                        "zone " + id_.str() + "'s timeline is not running");
        noticeLocked(events, position);
    }

    // The rooms are told first, and the pipeline stopped second: a STOP_AT
    // names a position a room is still about to reach, and a server that had
    // already torn its own side down would be naming one it could no longer
    // deliver.
    dispatch(std::move(commands));
    emit(events);

    // The pipeline thread is stopped before the decoder it owns is released,
    // which is the one thing here that cannot be done from two threads at
    // once. run() starts it again after the next play().
    thread_.request_stop();
    if (thread_.joinable())
        thread_.join();

    {
        // Against a disconnect, a release or a move on another thread, which
        // would otherwise stop or restart a room this is parking.
        std::lock_guard<std::mutex> moving(rejoinMutex_);

        std::vector<std::shared_ptr<ClientOutput>> outputs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& entry : members_)
                outputs.push_back(entry.second.output);
        }

        // Section 38's "drops it" on this side: each room's queue goes and its
        // stream stands still until play() names a position for it. Outside
        // mutex_, because parking a room joins its thread.
        for (const std::shared_ptr<ClientOutput>& output : outputs)
            output->park();
    }

    std::lock_guard<std::mutex> pipeline(pipelineMutex_);
    decoder_.reset();

    // Phase 42: a stopped zone holds no memory. What it decoded belongs to the
    // line that has just been abandoned, and the next play() sizes it afresh.
    releaseMemory();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sources_.empty())
            sources_.back().ended = true;
        if (state_ == ZoneState::Playing || state_ == ZoneState::Ended)
            state_ = ZoneState::Idle;

        // What was decoded for the line that has just been abandoned is never
        // going to be heard, so it is never going to be announced as started.
        startedThrough_ = sources_.size();
    }

    return position;
}

Status<ZoneError> Zone::setVolume(const ClientId& client, const matp::SetVolume& request)
{
    // The rule the wire library states and the client's parser enforces,
    // answered here rather than two threads later: a SET_VOLUME that carries
    // neither a value nor a mute asks for nothing.
    if (!request.hasValue && !request.hasMuted)
    {
        return fail(ZoneError::InvalidCommand,
                    "setVolume must carry a value, a muted flag, or both");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (members_.find(client) == members_.end())
            return fail(ZoneError::NotMember, client.str() + " is not in zone " + id_.str());
    }

    RoomCommand command;
    command.kind = RoomCommandKind::Volume;
    command.client = client;
    command.volume = request;

    // Forwarded and never applied: the gain is the client's, in its own render
    // path, which is what keeps this room's stream bitperfect (section 39).
    dispatch({std::move(command)});
    return {};
}

Result<SeekPlacement, ZoneError> Zone::seek(uint64_t positionNs)
{
    // What is audible, and whether this moment can take a seek at all. Asked
    // before the source is opened, so that a refusal costs no request to the
    // media endpoint.
    SourceSegment segment;
    AudioFormat format;
    size_t opened = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == ZoneState::Failed)
            return fail(ZoneError::NotPlaying, lastError_);

        // A position on a line, which a line that is stopped or still forming
        // its barrier does not have. A source that has not started yet takes
        // its offset where it is staged instead.
        const TransportState transport = scheduler_.transport();
        if (transport != TransportState::Playing && transport != TransportState::Paused)
        {
            return fail(ZoneError::NotStarted,
                        "zone " + id_.str() + "'s timeline is " + transportStateName(transport));
        }

        // One transition at a time: a room held at a format boundary is
        // standing still for its device, and moving its stream would start the
        // old converter on whatever comes after.
        for (const auto& entry : members_)
        {
            if (entry.second.transitionAt != TimelineNs{})
                return fail(ZoneError::NotSeekable,
                            entry.first.str() + " is held at a format boundary");
        }

        const uint64_t playhead =
            std::to_underlying(scheduler_.playheadAt(clock_.monotonicNowNs()));
        const StagedSource* audible = nullptr;
        for (const StagedSource& source : sources_)
        {
            if (std::to_underlying(source.timelineStartNs) <= playhead)
                audible = &source; // ordered oldest first, so the last one wins
        }
        if (audible == nullptr)
            return fail(ZoneError::NotStarted, "nothing is audible in zone " + id_.str() + " yet");

        // The next source has been opened and its boundary announced to every
        // room, so the line past the audible source is already somebody
        // else's. Seeking now would leave that boundary standing in the rooms
        // at a position that no longer means it. It lasts the final lookahead
        // of a source.
        if (audible != &sources_.back())
        {
            return fail(ZoneError::NotSeekable,
                        audible->segment.sourceId + " is ending: " +
                            sources_.back().segment.sourceId + " is already being decoded");
        }

        segment = audible->segment;
        segment.startOffsetNs = positionNs;
        format = audible->format;
        opened = sourcesOpened_;
    }

    // Open first, then cut. On the caller's thread and under no lock, because
    // an HTTP open may wait as long as a stall is allowed to -- and while it
    // does, the old decoder goes on feeding every room.
    Result<std::unique_ptr<IDecoder>, DecodeError> decoder =
        decoders_.open(segment.mediaUri, positionNs);
    if (!decoder)
    {
        const ZoneError code = decoder.error().code == DecodeError::OffsetOutOfRange
                                   ? ZoneError::InvalidPosition
                               : decoder.error().code == DecodeError::NotSeekable
                                   ? ZoneError::NotSeekable
                                   : ZoneError::SourceFailed;
        return fail(code, segment.sourceId + ": " + describe(decoder.error()));
    }
    const SourceStreamInfo info = (*decoder)->info();
    if (info.format != format)
    {
        // The same URI decoding into another format is another file, and the
        // rooms' converters were built for the one they are playing.
        return fail(ZoneError::SourceFailed, segment.sourceId + " now decodes as " +
                                                 describeFormat(info.format) + ", not " +
                                                 describeFormat(format));
    }

    // Order: the decoder's lock, then one move at a time, then the state.
    std::lock_guard<std::mutex> pipeline(pipelineMutex_);
    std::lock_guard<std::mutex> moving(rejoinMutex_);

    struct Room
    {
        ClientId client;
        std::shared_ptr<ClientOutput> output;
        bool onLine = false;
    };
    std::vector<Room> rooms;
    std::vector<RoomCommand> flushes;
    SeekPlacement placement;
    std::unique_ptr<IDecoder> replaced; // closed after mutex_ is let go: an HTTP close may wait
    placement.sourceId = segment.sourceId;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // The zone may have moved on while the source was being opened.
        const TransportState transport = scheduler_.transport();
        if (sourcesOpened_ != opened || sources_.back().segment.sourceId != segment.sourceId ||
            (transport != TransportState::Playing && transport != TransportState::Paused))
        {
            return fail(ZoneError::NotSeekable,
                        "zone " + id_.str() + " moved on while " + segment.sourceId +
                            " was being opened");
        }

        // A running line cuts one flush lead ahead of what is audible and is
        // heard again one fill lead after that. A paused one does both where
        // it stands, and the ordinary resume starts there.
        const uint64_t nowNs = clock_.monotonicNowNs();
        const uint64_t playhead = std::to_underlying(scheduler_.playheadAt(nowNs));
        const bool running = transport == TransportState::Playing;
        const uint64_t cut = running ? playhead + settings_.seekFlushLeadNs : playhead;
        const uint64_t resume = running ? cut + settings_.seekFillLeadNs : cut;
        placement.cutAt = TimelineNs{cut};
        placement.resumeAt = TimelineNs{resume};

        const SourceFrame firstFrame = store_->nextFrame();
        const Result<TimelineNs, TimelineError> base =
            timeline_.stageAt(firstFrame, info.format.sampleRate, placement.resumeAt);
        if (!base)
            return fail(ZoneError::TimelineFailed, describe(base.error()));

        StagedSource staged;
        staged.segment = segment;
        staged.timelineStartNs = *base;
        staged.firstFrame = firstFrame;
        staged.format = info.format;
        staged.totalFrames = remainingFrames(info, positionNs);

        // The old source is over where it was cut; nothing it decoded past the
        // cut is read by anybody, and the store ages it out like any other.
        sources_.back().ended = true;
        sources_.push_back(std::move(staged));
        ++sourcesOpened_;
        pruneSourcesLocked();
        state_ = ZoneState::Playing;
        publishEndLocked();
        endAnnounced_ = false;
        replaced = std::exchange(decoder_, std::move(*decoder));

        // The line's own instants: the cut, which the FLUSH carries, and when
        // the seeked audio is heard, which is when a room's reports mean
        // something again. A paused line has neither yet.
        const uint64_t anchor = std::to_underlying(scheduler_.startTimelineNs());
        const uint64_t anchoredAt = scheduler_.startServerTimeNs();
        const uint64_t cutServerNs = running ? anchoredAt + (cut - anchor) : nowNs;
        const uint64_t heardAtNs = running ? anchoredAt + (resume - anchor) : 0;

        for (auto& entry : members_)
        {
            const OutputState state = entry.second.output->state();
            if (state != OutputState::Streaming && state != OutputState::Stranded)
                continue;

            // Section 54 is for a room that has a line to keep. One that has
            // not been placed on it yet has no mapping for a flush to keep,
            // and is moved and placed the ordinary way.
            const RoomStartState start = scheduler_.roomState(entry.first);
            const bool onLine = start == RoomStartState::Playing || start == RoomStartState::Paused;
            rooms.push_back({entry.first, entry.second.output, onLine});
            if (!onLine)
                continue;

            entry.second.flushUntilNs = heardAtNs;
            RoomCommand command;
            command.kind = RoomCommandKind::Flush;
            command.client = entry.first;
            command.at.timelineNs = cut;
            command.at.serverTimeNs = cutServerNs;
            flushes.push_back(std::move(command));
        }
    }

    MULTIROOM_INFO("zone {}: {} seeks to {} ns: cut at timeline {} ns, heard again from {} ns",
             id_.str(), segment.sourceId, positionNs, std::to_underlying(placement.cutAt),
             std::to_underlying(placement.resumeAt));

    // The message before the audio it explains, though a room copes with the
    // two arriving in either order -- that is what `afterFlush` is for.
    dispatch(std::move(flushes));

    // Outside mutex_, because park() joins each room's thread. A room that
    // cannot be moved is that room's failure and the zone plays on.
    for (const Room& room : rooms)
    {
        room.output->park();
        const Status<OutputError> moved =
            room.onLine ? room.output->flushTo(placement.resumeAt)
                        : room.output->rejoinAt(placement.resumeAt);
        if (!moved)
        {
            MULTIROOM_WARN("zone {}: {} could not be moved to the seek at timeline {} ns: {}",
                     id_.str(), room.client.str(), std::to_underlying(placement.resumeAt),
                     describe(moved.error()));
            continue;
        }
        if (const auto running = room.output->run(); !running)
        {
            MULTIROOM_WARN("zone {}: {} could not be restarted after the seek: {}", id_.str(),
                     room.client.str(), describe(running.error()));
            continue;
        }
        if (room.onLine)
            continue;

        // Its stream now begins at the seeked audio, and what it reported
        // before described the audio it no longer has.
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_.rejoin(room.client, placement.resumeAt);
        if (const auto found = members_.find(room.client);
            found != members_.end() && found->second.sync != nullptr)
            found->second.sync->clearOffset();
    }

    return placement;
}

// ---------------------------------------------------------------------------

void Zone::announceBoundary(const StagedSource& staged)
{
    std::vector<RoomCommand> commands;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commands.reserve(members_.size());
        for (const auto& entry : members_)
        {
            RoomCommand command;
            command.kind = RoomCommandKind::TrackBoundary;
            command.client = entry.first;
            command.boundary.timelineNs = std::to_underlying(staged.timelineStartNs);
            command.boundary.trackId = staged.segment.trackId;
            command.boundary.title = staged.segment.title;
            commands.push_back(std::move(command));
        }
    }

    // Metadata and nothing else (section 23). Nothing about any room's stream
    // changes here, which is what makes a same-format boundary gapless: the
    // packets carry on across it because nothing asked them not to.
    dispatch(std::move(commands));
}

void Zone::announceTracksTo(const ClientId& client, TimelineNs position)
{
    std::vector<RoomCommand> commands;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        const uint64_t at = std::to_underlying(position);
        const StagedSource* current = nullptr;
        for (const StagedSource& source : sources_)
        {
            if (std::to_underlying(source.timelineStartNs) <= at)
                current = &source; // ordered oldest first, so the last one wins
        }

        // Section 44's case: this room's stream begins in the middle of a
        // track whose boundary was announced before it was here. A source that
        // names neither a track id nor a title has nothing to say about
        // itself, and TRACK_INFO is nothing but those two.
        if (current != nullptr &&
            (!current->segment.trackId.empty() || !current->segment.title.empty()))
        {
            RoomCommand command;
            command.kind = RoomCommandKind::TrackInfo;
            command.client = client;
            command.info.trackId = current->segment.trackId;
            command.info.title = current->segment.title;
            commands.push_back(std::move(command));
        }

        // And the boundaries it is about to reach, which were announced while
        // it was not a member either. Bounded by the sources this zone has
        // opened and not yet played out, which is one or two.
        for (const StagedSource& source : sources_)
        {
            if (std::to_underlying(source.timelineStartNs) <= at)
                continue;

            RoomCommand command;
            command.kind = RoomCommandKind::TrackBoundary;
            command.client = client;
            command.boundary.timelineNs = std::to_underlying(source.timelineStartNs);
            command.boundary.trackId = source.segment.trackId;
            command.boundary.title = source.segment.title;
            commands.push_back(std::move(command));
        }
    }

    dispatch(std::move(commands));
}

void Zone::pump()
{
    // One pass at a time against a seek, which replaces the decoder this pass
    // reads (phase 26).
    std::lock_guard<std::mutex> pipeline(pipelineMutex_);

    // Section 20 first, and whatever state the zone is in: a zone that has
    // decoded its last source still has rooms draining what it made, and a
    // room that becomes ready then still has a timeline to be put on.
    schedule();

    // Phase 42: the memory follows what is playing. A zone that has played
    // out gives all of it back, and a stage() that follows sizes it again.
    applyShrink();
    {
        bool playedOut = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            playedOut = state_ == ZoneState::Ended && endAnnounced_;
        }
        if (playedOut && budget_ != 0)
            releaseMemory();
    }

    for (;;)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != ZoneState::Playing)
                return;
        }

        if (decoder_ == nullptr)
        {
            // One boundary at a time. A room re-negotiating for the source
            // that has just been decoded is planned against that source's
            // format, and opening another on top of it would have that
            // format change under it. Bounded by the ready timeout, like the
            // wait itself.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (transitionPendingLocked(clock_.monotonicNowNs()))
                    return;
            }

            const Result<bool, ZoneError> opened = openNext();
            if (!opened && opened.error().code == ZoneError::StoreFailed)
            {
                // Phase 42: at a boundary, a format the memory limit cannot
                // hold fails the zone by name rather than stalling part-way
                // into the new source. The zones already playing are not
                // touched.
                failZone(ZoneError::StoreFailed, opened.error().detail);
                ZoneEvent event = eventOf(ZoneEventKind::SourceFailed);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!staged_.empty())
                        event.sourceId = staged_.front().sourceId;
                }
                event.reason = opened.error().detail;
                emit({event});
                return;
            }
            if (!opened || !*opened)
                return; // failed, or nothing left to open: both already recorded
        }

        if (!wantsAudio())
            return;

        bool sourceEnded = false;
        std::string sourceFailure;
        const bool published = decodeBlock(sourceEnded, sourceFailure);
        if (sourceEnded)
        {
            decoder_.reset();
            std::string sourceId;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!sources_.empty())
                {
                    sources_.back().ended = true;
                    sourceId = sources_.back().segment.sourceId;
                }
            }
            // Phase 22: the boundary is where it failed, and the next source
            // is opened there as if this one had simply ended.
            if (!sourceFailure.empty())
                skipSource(sourceId, std::move(sourceFailure));
            continue;
        }
        if (!published)
            return;
    }
}

Result<bool, ZoneError> Zone::openNext()
{
    SourceSegment segment;
    Result<std::unique_ptr<IDecoder>, DecodeError> decoder =
        fail(DecodeError::SourceFailed, "nothing opened");
    for (;;)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (staged_.empty())
            {
                // Out of staged sources: the core plays what it has and stops.
                // It never chooses the next one, so this is an end rather than
                // a fault, and stage() reopens it.
                if (state_ == ZoneState::Playing)
                {
                    state_ = ZoneState::Ended;
                    publishEndLocked();
                    MULTIROOM_INFO("zone {}: no source staged after {}", id_.str(),
                             sources_.empty() ? std::string("nothing")
                                              : sources_.back().segment.sourceId);
                }
                return false;
            }
            segment = std::move(staged_.front());
            staged_.pop_front();
        }

        // Phase 22: a source that will not open is skipped for the next one,
        // with nothing on the timeline for it.
        decoder = decoders_.open(segment.mediaUri, segment.startOffsetNs);
        if (!decoder)
        {
            skipSource(segment.sourceId, describe(decoder.error()));
            continue;
        }
        if (const AudioFormat format = (*decoder)->info().format; !format.isValid())
        {
            skipSource(segment.sourceId, "the decoder reported " + describeFormat(format));
            continue;
        }
        if (std::optional<std::string> refused = unplayable((*decoder)->info().format))
        {
            skipSource(segment.sourceId, std::move(*refused));
            continue;
        }
        break;
    }

    const SourceStreamInfo& info = (*decoder)->info();

    AudioFormat previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = sourceFormat_;
    }
    const bool transition = previous.isValid() && info.format != previous;

    const SourceFrame firstFrame = store_->nextFrame();

    // Phase 42, before anything of this source exists: the memory its format
    // needs. Refused, it is put back where it was staged, and nothing of it
    // reaches the timeline.
    if (const auto budgeted = budgetFor(info.format, firstFrame); !budgeted)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        staged_.push_front(std::move(segment));
        return std::unexpected(budgeted.error());
    }
    const Result<TimelineNs, TimelineError> base =
        timeline_.stage(firstFrame, info.format.sampleRate);
    if (!base)
    {
        failZone(ZoneError::TimelineFailed, segment.sourceId + ": " + describe(base.error()));
        return fail(ZoneError::TimelineFailed, describe(base.error()));
    }

    StagedSource staged;
    staged.segment = std::move(segment);
    staged.timelineStartNs = *base;
    staged.firstFrame = firstFrame;
    staged.format = info.format;
    staged.totalFrames = remainingFrames(info, staged.segment.startOffsetNs);

    bool nothingAfter = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sources_.empty())
            sources_.back().ended = true;
        if (!previous.isValid() || transition)
            formatSinceNs_ = *base;
        sourceFormat_ = info.format;
        sources_.push_back(staged);
        ++sourcesOpened_;
        pruneSourcesLocked();
        state_ = ZoneState::Playing;
        publishEndLocked();
        nothingAfter = staged_.empty();
    }

    decoder_ = std::move(*decoder);

    MULTIROOM_INFO("zone {}: {} ({} {}) from timeline {} ns, source frame {}", id_.str(),
             staged.segment.sourceId, info.containerName, info.codecName,
             std::to_underlying(*base), std::to_underlying(firstFrame));

    // Section 24, before a single block of the new source is published: a
    // room still reading the old one must be held at the boundary before
    // there is anything past it to read.
    if (transition)
    {
        MULTIROOM_INFO("zone {}: {} -> {} at timeline {} ns", id_.str(), describeFormat(previous),
                 describeFormat(info.format), std::to_underlying(*base));
        beginTransition(*base, info.format);
    }

    // Section 23, announced before any room reaches it and after the timeline
    // knows where it is. The first source of a zone is announced too: a room
    // that was already a member is owed the name of what it is about to hear,
    // and a boundary at a position every stream contains is one the client
    // promotes on its first playback report.
    announceBoundary(staged);

    // Phase 24: asked at the open rather than at the end of the decode, so
    // the high-end server has almost this whole source to answer in rather
    // than one lookahead. Nothing is decoded of it yet.
    if (nothingAfter)
    {
        ZoneEvent event = eventOf(ZoneEventKind::SourceNeeded);
        event.sourceId = staged.segment.sourceId;
        if (staged.totalFrames)
        {
            event.remainingNs =
                nanosecondsFromFrames(*staged.totalFrames, staged.format.sampleRate).value_or(0);
        }
        emit({event});
    }
    return true;
}

bool Zone::decodeBlock(bool& sourceEnded, std::string& sourceFailure)
{
    const AudioFormat format = decoder_->info().format;

    Result<BlockPool::Handle, BlockPoolError> handle = pool_->acquire();
    if (!handle)
    {
        // This zone's own blocks, all out (phase 42): a room is still holding
        // one the store has let go of, and gives it back with its next read.
        // Counted on the pool, and waited out rather than failed.
        return false;
    }

    const uint32_t capacity = (*handle)->capacityFrames(format);
    if (capacity == 0)
    {
        failZone(ZoneError::StoreFailed, "a pool block does not hold one frame of " +
                                             describeFormat(format));
        return false;
    }

    const uint32_t bytesPerFrame = format.bytesPerFrame();
    const std::span<std::byte> writable = (*handle)->writable();

    uint32_t frames = 0;
    while (frames < capacity)
    {
        const std::span<std::byte> rest =
            writable.subspan(static_cast<size_t>(frames) * bytesPerFrame,
                             static_cast<size_t>(capacity - frames) * bytesPerFrame);

        const Result<DecodedSpan, DecodeError> decoded = decoder_->decode(rest);
        if (!decoded)
        {
            // A failure ends the source and not the zone (phase 22). What was
            // decoded before it is still published below: it is real audio,
            // and the boundary after it is where the next source begins.
            if (decoded.error().code != DecodeError::EndOfStream)
                sourceFailure = describe(decoded.error());
            sourceEnded = true;
            break;
        }
        frames += decoded->frameCount;
    }

    if (frames == 0)
        return false; // the source ended exactly on a block boundary

    if (const auto committed = (*handle)->commit(format, store_->nextFrame(), frames);
        !committed)
    {
        failZone(ZoneError::StoreFailed, describe(committed.error()));
        return false;
    }

    if (const auto publishedBlock = store_->publish(std::move(*handle)); !publishedBlock)
    {
        failZone(ZoneError::StoreFailed, describe(publishedBlock.error()));
        return false;
    }

    published_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sources_.empty())
            sources_.back().frames += frames;
    }
    return true;
}

bool Zone::wantsAudio() const
{
    const Result<TimelineNs, TimelineError> frontier = timeline_.timelineAt(store_->nextFrame());
    if (!frontier)
        return false;

    // The room that is furthest ahead, and never the one that is furthest
    // behind: a room that has stopped draining stops asking for audio, and a
    // zone that waited for it would be a room blocking a zone. Zero is the
    // floor, so a zone with no members at all decodes one lookahead from its
    // start and then stops.
    uint64_t demandNs = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : members_)
        {
            if (entry.second.output->state() != OutputState::Streaming)
                continue;
            demandNs = std::max(demandNs, std::to_underlying(entry.second.output->producedThroughNs()));
        }

        // Phase 39: once there is a line, it bounds the demand and the rooms
        // do not. A barrier that is forming stands at its start position,
        // which plays the playhead's part until the line runs.
        if (scheduler_.started())
        {
            const uint64_t line = std::to_underlying(scheduler_.playheadAt(clock_.monotonicNowNs()));
            demandNs = std::min(demandNs, line + lateJoinLeadNs_ + settings_.output.targetBufferNs);
        }
    }

    if (demandNs > std::numeric_limits<uint64_t>::max() - lookaheadNs_)
        return false;
    return std::to_underlying(*frontier) < demandNs + lookaheadNs_;
}

TimelineNs Zone::ceilingLocked(uint64_t latencyNs, uint64_t nowNs) const
{
    // A room with an output latency is started that much earlier (phase 27),
    // so it plays that much ahead of the line.
    const uint64_t line =
        scheduler_.started() ? std::to_underlying(scheduler_.playheadAt(nowNs)) + latencyNs : 0;
    const uint64_t packetNs = static_cast<uint64_t>(settings_.output.packetDurationMs) * 1000000ULL;
    return TimelineNs{line + settings_.output.targetBufferNs + packetNs};
}

void Zone::publishEndLocked()
{
    const TimelineNs end = state_ == ZoneState::Ended ? frontierNs() : TimelineNs{};
    for (const auto& entry : members_)
        entry.second.output->setEnd(end);
}

Result<TimelineNs, ZoneError> Zone::joinPosition() const
{
    const uint64_t packetNs = static_cast<uint64_t>(settings_.output.packetDurationMs) * 1000000ULL;

    uint64_t wanted = 0;
    uint32_t rate = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t floor = std::to_underlying(formatSinceNs_);

        if (scheduler_.started())
        {
            // A line standing at a fixed position is a barrier forming there,
            // and a room that joins now can be part of it rather than a
            // latecomer to it: nothing is playing ahead of it. Section 44
            // otherwise: a zone that is already playing has a playhead, and a
            // room joining it has to begin somewhere that has not been heard
            // yet -- the audio it is about to prebuffer is audio nobody has had.
            wanted = scheduler_.transport() == TransportState::Starting
                         ? std::to_underlying(scheduler_.startTimelineNs())
                         : std::to_underlying(scheduler_.playheadAt(clock_.monotonicNowNs())) +
                               lateJoinLeadNs_;
            wanted = std::max(wanted, floor);
            rate = formatAtLocked(TimelineNs{wanted}).sampleRate;
        }
        else
        {
            wanted = floor;
        }
    }

    // Before the first START_AT: where this zone's audio still begins.
    if (rate == 0)
    {
        const Result<TimelineNs, TimelineError> oldest =
            timeline_.timelineAt(store_->oldestFrame());
        if (!oldest)
            return fail(ZoneError::TimelineFailed, describe(oldest.error()));
        return TimelineNs{std::max(std::to_underlying(*oldest), wanted)};
    }

    // Phase 40: on audio the store still holds, one packet clear of the block
    // the next eviction takes. Asked in frames, because after a seek the
    // oldest audio held can be an abandoned decode whose positions lie past
    // the seeked source's.
    const uint64_t clear = std::to_underlying(store_->oldestFrame()) +
                           framesFromNanoseconds(packetNs, rate).value_or(0);
    uint64_t position = wanted;
    if (const Result<SourceFrame, TimelineError> at = timeline_.frameAt(TimelineNs{wanted});
        !at || std::to_underlying(*at) < clear)
    {
        const Result<TimelineNs, TimelineError> held = timeline_.timelineAt(SourceFrame{clear});
        if (!held)
            return fail(ZoneError::TimelineFailed, describe(held.error()));
        position = std::max(wanted, std::to_underlying(*held));
    }

    const bool behind = position > wanted + packetNs;
    if (behind && !joinsBehindStore_.exchange(behind))
    {
        MULTIROOM_WARN("zone {}: joins are placed on the oldest audio the store holds, {} ms past "
                 "where they belong: the decode is ahead of the line",
                 id_.str(), (position - wanted) / 1000000ULL);
    }
    else if (!behind)
    {
        joinsBehindStore_.store(false);
    }
    return TimelineNs{position};
}

void Zone::skipSource(const std::string& sourceId, std::string reason)
{
    MULTIROOM_WARN("zone {}: {} skipped: {}", id_.str(), sourceId, reason);

    ZoneEvent event = eventOf(ZoneEventKind::SourceFailed);
    event.sourceId = sourceId;
    event.reason = reason;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_.push_back(FailedSource{sourceId, std::move(reason)});
        if (failed_.size() > kMaxFailedSources)
            failed_.pop_front();
    }
    emit({event});
}

std::optional<std::string> Zone::unplayable(const AudioFormat& format) const
{
    std::vector<std::pair<ClientId, ClientCapabilities>> rooms;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [client, member] : members_)
        {
            const OutputState state = member.output->state();
            if (state != OutputState::Failed && state != OutputState::Stopped)
                rooms.emplace_back(client, member.capabilities);
        }
    }

    // Outside the lock, as in beginTransition: the room's preference belongs
    // to another module.
    std::string why;
    for (const auto& [client, capabilities] : rooms)
    {
        const Result<OutputPlan, PlanError> plan =
            planner_.plan(format, settingsFor(client), capabilities);
        if (plan)
            return std::nullopt;
        why += (why.empty() ? "" : "; ") + client.str() + ": " + describe(plan.error());
    }
    if (rooms.empty())
        return std::nullopt;
    return "no room can play " + describeFormat(format) + " (" + why + ")";
}

Status<ZoneError> Zone::budgetFor(const AudioFormat& format, SourceFrame firstFrame)
{
    const size_t count = blocksFor(memory_.blockBytes(), format, settings_.retentionNs);
    if (count == 0)
        return fail(ZoneError::StoreFailed, "a block does not hold one frame of " +
                                                describeFormat(format));

    if (count > budget_)
    {
        // Grown before the first block of the source is decoded -- and at a
        // boundary, while the rooms are held at it anyway (section 24).
        if (const auto grown = pool_->resize(count); !grown)
        {
            constexpr double kMb = 1024.0 * 1024.0;
            const uint64_t limit = memory_.limitBytes();
            const uint64_t used = memory_.usedBytes();
            return fail(ZoneError::StoreFailed,
                        std::format("zone {} at {} needs {:.1f} MB and {:.1f} MB of the {:.0f} MB "
                                    "limit are free",
                                    id_.str(), describeFormat(format),
                                    static_cast<double>(count * memory_.blockBytes()) / kMb,
                                    static_cast<double>(limit - std::min(limit, used)) / kMb,
                                    static_cast<double>(limit) / kMb));
        }
        MULTIROOM_INFO("zone {}: {} blocks ({} kB) for {}", id_.str(), count,
                 count * memory_.blockBytes() / 1024, describeFormat(format));
        budget_ = count;
        pendingBudget_ = 0;
        store_->setMaxBlocks(count - 2);
    }
    else if (count < budget_)
    {
        pendingBudget_ = count;
        shrinkFrom_ = firstFrame;
    }
    else
    {
        pendingBudget_ = 0;
    }
    return {};
}

void Zone::applyShrink()
{
    if (pendingBudget_ == 0 ||
        std::to_underlying(store_->oldestFrame()) < std::to_underlying(shrinkFrom_))
        return;

    // The store holds nothing of the larger format any more. What is free
    // goes now; what a room still reads goes when it lets go.
    static_cast<void>(pool_->resize(pendingBudget_));
    MULTIROOM_INFO("zone {}: {} blocks ({} kB), down from {}", id_.str(), pendingBudget_,
             pendingBudget_ * memory_.blockBytes() / 1024, budget_);
    budget_ = pendingBudget_;
    pendingBudget_ = 0;
    store_->setMaxBlocks(budget_ - 2);
}

void Zone::releaseMemory()
{
    store_->clear();
    static_cast<void>(pool_->resize(0));
    if (budget_ != 0)
        MULTIROOM_INFO("zone {}: audio memory released", id_.str());
    budget_ = 0;
    pendingBudget_ = 0;
}

std::vector<FailedSource> Zone::failedSources() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return {failed_.begin(), failed_.end()};
}

void Zone::failZone(ZoneError code, std::string detail)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == ZoneState::Failed)
            return;
        state_ = ZoneState::Failed;
        lastError_ = std::string(errorName(code)) + ": " + detail;
    }

    // One zone stopping with a reason, which by the fault-isolation table is
    // every room of this zone and no other zone at all.
    MULTIROOM_ERROR("zone {}: {}: {}", id_.str(), errorName(code), detail);
}

// ---------------------------------------------------------------------------

ZoneState Zone::state() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string Zone::lastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

AudioFormat Zone::sourceFormat() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sourceFormat_;
}

TimelineNs Zone::frontierNs() const
{
    const Result<TimelineNs, TimelineError> frontier = timeline_.timelineAt(store_->nextFrame());
    return frontier ? *frontier : TimelineNs{};
}

TimelineNs Zone::oldestNs() const
{
    const Result<TimelineNs, TimelineError> oldest = timeline_.timelineAt(store_->oldestFrame());
    return oldest ? *oldest : TimelineNs{};
}

// ---------------------------------------------------------------------------

Result<OutputPlan, ZoneError> Zone::planFor(const ClientId& client,
                                            const ClientCapabilities& capabilities)
{
    AudioFormat source;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source = sourceFormat_;
    }
    if (!source.isValid())
        return fail(ZoneError::NotPlaying, "zone " + id_.str() + " has no source yet");

    const Result<OutputPlan, PlanError> planned =
        planner_.plan(source, settingsFor(client), capabilities);
    if (!planned)
        return fail(ZoneError::PlanFailed, describe(planned.error()));

    // Announced a format, and therefore waited for: section 20's barrier holds
    // the first start for a room whose device is still opening, and a room
    // that never answers holds it only until the ready timeout. A room this
    // zone could not plan is not one of them -- it has been told its own
    // ceiling instead and belongs to no zone.
    if (!client.empty())
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_.expect(client, clock_.monotonicNowNs());
    }
    return *planned;
}

Status<ZoneError> Zone::admit(const SessionId& session, const ClientId& client,
                              const ClientCapabilities& capabilities,
                              const AudioFormat& grantedFormat)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == ZoneState::Failed)
            return fail(ZoneError::NotPlaying, lastError_);
        if (members_.find(client) != members_.end())
            return fail(ZoneError::AlreadyMember, client.str() + " is already in this zone");
        if (members_.size() >= settings_.maxMembers)
            return fail(ZoneError::Full, "zone " + id_.str() + " holds " +
                                             std::to_string(settings_.maxMembers) + " rooms");
    }

    const Result<OutputPlan, ZoneError> requested = planFor(client, capabilities);
    if (!requested)
        return std::unexpected(requested.error());

    // Section 12: the device answers with what it opened, which is not always
    // what it was asked for, and the plan that room gets is the one made
    // against the answer.
    OutputPlan plan = *requested;
    if (grantedFormat.isValid())
    {
        const Result<OutputPlan, PlanError> granted =
            planner_.replanForGrantedFormat(*requested, settingsFor(client), grantedFormat,
                                            capabilities);
        if (!granted)
            return fail(ZoneError::PlanFailed, describe(granted.error()));
        plan = *granted;
    }

    Result<std::unique_ptr<IClientAudioProcessor>, ProcessorError> processor =
        makeClientAudioProcessor(plan, registry_, settings_.dither);
    if (!processor)
        return fail(ZoneError::PlanFailed, describe(processor.error()));

    alignLineToLoneRoom("a room joins it");

    const Result<TimelineNs, ZoneError> position = joinPosition();
    if (!position)
        return std::unexpected(position.error());

    Result<std::unique_ptr<ClientOutput>, OutputError> output =
        ClientOutput::create(session, client, *store_, timeline_, std::move(*processor), clock_,
                             settings_.output);
    if (!output)
        return fail(ZoneError::OutputFailed, describe(output.error()));

    if (settings_.roomEqualizer)
        (*output)->setEqualizer(settings_.roomEqualizer(client));

    if (const auto joined = (*output)->joinAt(*position); !joined)
        return fail(ZoneError::OutputFailed, describe(joined.error()));
    {
        // Bounded from its first packet rather than from the next pass.
        std::lock_guard<std::mutex> lock(mutex_);
        (*output)->setCeiling(ceilingLocked(0, clock_.monotonicNowNs()));
    }
    if (const auto running = (*output)->run(); !running)
        return fail(ZoneError::OutputFailed, describe(running.error()));

    std::shared_ptr<ClientOutput> shared(std::move(*output));

    // Section 26's nominal rate is this room's own stream rate, which is what
    // the plan above settled -- not the zone's source rate, and not a rate any
    // two rooms necessarily share.
    std::shared_ptr<ClientSyncController> sync;
    uint64_t rejoins = 0;
    bool returning = false;

    bool inserted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Section 43: the same room, back on a new connection. What its card
        // was measured to be doing is not a property of the socket that went
        // away, so the correction in force is carried rather than measured
        // again -- which is what stops every reconnect costing a room a fresh
        // convergence. Everything that *was* a property of the old connection
        // -- the window, the offset, the clock model behind both -- is
        // dropped by carryOver() itself.
        if (const auto kept = retained_.find(client); kept != retained_.end())
        {
            sync = kept->second.sync;
            rejoins = kept->second.rejoins;
            retained_.erase(kept);
            returning = sync != nullptr;
            if (returning)
                sync->carryOver(plan.outputFormat.sampleRate);
        }
        if (sync == nullptr)
            sync = std::make_shared<ClientSyncController>(plan.outputFormat.sampleRate,
                                                          settings_.sync);
        if (returning)
            ++rejoins;

        inserted = members_
                       .emplace(client, Member{session, shared, std::move(sync), rejoins,
                                               capabilities})
                       .second;
        if (inserted)
            scheduler_.join(client, *position, clock_.monotonicNowNs());
    }
    if (inserted && returning)
    {
        rejoins_.fetch_add(1);
        MULTIROOM_INFO("zone {}: {} is back ({}), move {}", id_.str(), client.str(),
                 rejoinTriggerName(RejoinTrigger::Reconnect), rejoins);
    }
    if (!inserted)
    {
        // Outside the lock, because stopping a room joins its thread. This is
        // a second admit() racing the first; whoever lost cleans up its own
        // output and the zone keeps the one that is in the map.
        shared->stop();
        return fail(ZoneError::AlreadyMember, client.str() + " is already in this zone");
    }

    MULTIROOM_INFO("zone {}: {} joined at timeline {} ns", id_.str(), client.str(),
             std::to_underlying(*position));

    // Section 44: this room's stream begins wherever the zone is, which is
    // rarely at a track boundary. What is playing there, and every boundary
    // this zone announced before this room was in it, go out now -- otherwise
    // it plays a track it cannot name.
    announceTracksTo(client, *position);
    return {};
}

const char* rejoinTriggerName(RejoinTrigger trigger)
{
    switch (trigger)
    {
    case RejoinTrigger::Reconnect:
        return "reconnect";
    case RejoinTrigger::Evicted:
        return "store eviction";
    case RejoinTrigger::DeviceRecovery:
        return "device recovery";
    case RejoinTrigger::LeftBehind:
        return "the line ran past it";
    case RejoinTrigger::HardResync:
        return "hard resync";
    }
    return "unknown";
}

Result<TimelineNs, ZoneError> Zone::rejoin(const ClientId& client, RejoinTrigger trigger,
                                           const PlanForSource& replan)
{
    // Why it fell behind, before the move replaces it: the strand's own text,
    // or the scheduler's (phase 41's `detail`).
    std::string detail;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto found = members_.find(client); found != members_.end())
        {
            detail = trigger == RejoinTrigger::LeftBehind ? scheduler_.roomDetail(client)
                                                          : found->second.output->lastError();
        }
    }

    TimelineNs position{};
    if (const auto moved = moveRoom(client, trigger, position, replan); !moved)
        return std::unexpected(moved.error());

    // After every lock of this zone has been let go, as every event is.
    if (std::optional<ZoneEvent> event = recordMove(client, trigger, position))
    {
        if (event->kind == ZoneEventKind::RoomUnstable)
        {
            event->reason = std::move(detail);
            MULTIROOM_WARN("zone {}: {} is unstable: {} moves inside {} s, the next after {} ms",
                     id_.str(), client.str(), event->rejoins, kSettledNs / kNanosecondsPerSecond,
                     event->backoffNs / 1000000ULL);
        }
        emit({*event});
    }
    return position;
}

std::optional<ZoneEvent> Zone::recordMove(const ClientId& client, RejoinTrigger trigger,
                                          TimelineNs position)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = members_.find(client);
    if (found == members_.end())
        return std::nullopt;

    Member& member = found->second;
    const uint64_t nowNs = clock_.monotonicNowNs();
    member.lastMoveNs = nowNs;
    member.leftBehind = false; // the move answers the scheduler's request too

    std::optional<ZoneEvent> event;
    if (trigger == RejoinTrigger::Evicted || trigger == RejoinTrigger::LeftBehind)
    {
        member.backoffNs = member.backoffNs == 0 ? kFirstBackoffNs
                                                 : std::min(member.backoffNs * 2, kMaxBackoffNs);
        member.nextMoveNs = nowNs + member.backoffNs;

        member.recentMoves.push_back(nowNs);
        while (nowNs - member.recentMoves.front() > kSettledNs)
            member.recentMoves.pop_front();

        if (!member.unstable && member.recentMoves.size() >= kUnstableMoves)
        {
            member.unstable = true;
            member.unstableSinceNs = nowNs;

            event = eventOf(ZoneEventKind::RoomUnstable);
            event->client = client;
            event->trigger = trigger;
            event->rejoins = member.recentMoves.size();
            event->backoffNs = member.backoffNs;
            member.episodeMoves = event->rejoins;
            return event;
        }
    }

    // While a room is unstable, its episode stands for every move it makes.
    if (member.unstable)
    {
        ++member.episodeMoves;
        return std::nullopt;
    }

    event = eventOf(ZoneEventKind::RoomFellBehind);
    event->client = client;
    event->trigger = trigger;
    event->timelineNs = position;
    return event;
}

std::vector<std::pair<ClientId, RejoinTrigger>>
Zone::dueMoves(const std::vector<ClientId>& leftBehind, std::vector<ZoneEvent>& events)
{
    std::vector<std::pair<ClientId, RejoinTrigger>> due;
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t nowNs = clock_.monotonicNowNs();

    for (const ClientId& client : leftBehind)
    {
        if (const auto found = members_.find(client); found != members_.end())
            found->second.leftBehind = true;
    }

    for (auto& [client, member] : members_)
    {
        const OutputState state = member.output->state();

        // Thirty seconds of streaming since its last move: the back-off
        // starts again from nothing, and an episode is over.
        if (member.lastMoveNs != 0 && nowNs - member.lastMoveNs >= kSettledNs &&
            state == OutputState::Streaming)
        {
            member.backoffNs = 0;
            member.nextMoveNs = 0;
            member.recentMoves.clear();
            if (member.unstable)
            {
                const uint64_t lastedNs = nowNs - member.unstableSinceNs;
                MULTIROOM_INFO("zone {}: {} is stable again after {} moves in {} s", id_.str(),
                         client.str(), member.episodeMoves, lastedNs / kNanosecondsPerSecond);

                ZoneEvent event = eventOf(ZoneEventKind::RoomStable);
                event.client = client;
                event.rejoins = member.episodeMoves;
                event.unstableForNs = lastedNs;
                events.push_back(std::move(event));
                member.unstable = false;
                member.unstableSinceNs = 0;
                member.episodeMoves = 0;
            }
        }

        // The room keeps trying for as long as the zone plays: a back-off
        // spaces its moves out and never ends them. While it waits, a stranded
        // room stays stranded, which is silence rather than stutter.
        if (nowNs < member.nextMoveNs)
            continue;
        if (state == OutputState::Stranded)
            due.emplace_back(client, RejoinTrigger::Evicted);
        else if (member.leftBehind)
            due.emplace_back(client, RejoinTrigger::LeftBehind);
    }
    return due;
}

RoomStability Zone::stability(const ClientId& client) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = members_.find(client);
    if (found == members_.end())
        return {};

    const Member& member = found->second;
    RoomStability out;
    out.unstable = member.unstable;
    out.rejoins = member.unstable ? member.episodeMoves : 0;
    out.backoffNs = member.backoffNs;
    out.unstableSinceNs = member.unstableSinceNs;
    return out;
}

void Zone::setTiming(const ZoneTiming& timing)
{
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.start.minimumStartBufferNs = timing.minimumStartBufferNs;
    settings_.start.readyTimeoutNs = timing.readyTimeoutNs;
    settings_.start.startDelayNs = timing.startDelayNs;
    settings_.seekFlushLeadNs = timing.seekFlushLeadNs;
    settings_.seekFillLeadNs = timing.seekFillLeadNs;
    scheduler_.setTiming(timing.minimumStartBufferNs, timing.readyTimeoutNs, timing.startDelayNs);
}

Result<bool, ZoneError> Zone::setOutputLatency(const ClientId& client, uint64_t latencyNs)
{
    bool playing = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = members_.find(client);
        if (found == members_.end())
            return fail(ZoneError::NotMember, client.str() + " is not in zone " + id_.str());

        // Applied whether or not it changed: a controller kept across a
        // disconnect still holds the target it had, and the admission that
        // sets this again is what brings the two back into line.
        const bool changed = found->second.latencyNs != latencyNs;
        found->second.latencyNs = latencyNs;
        if (found->second.sync != nullptr)
            found->second.sync->setTargetOffset(static_cast<int64_t>(latencyNs));
        scheduler_.setLatency(client, latencyNs);

        const RoomStartState start = scheduler_.roomState(client);
        playing =
            changed && (start == RoomStartState::Scheduled || start == RoomStartState::Playing);
        if (changed)
            MULTIROOM_INFO("zone {}: {} output latency {} us", id_.str(), client.str(),
                     latencyNs / 1000);
    }
    if (!playing)
        return false;

    // Outside the lock, because a move stops that room's thread. A room that
    // cannot be moved keeps playing on the start it had, and takes the figure
    // at its next one.
    if (const auto moved = rejoin(client, RejoinTrigger::HardResync); !moved)
    {
        MULTIROOM_WARN("zone {}: {} keeps its start until the next one: {}", id_.str(), client.str(),
                 describe(moved.error()));
        return false;
    }
    return true;
}

Replan Zone::replanInPlace(const ClientId& client)
{
    std::shared_ptr<ClientOutput> output;
    ClientCapabilities capabilities;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = members_.find(client);
        if (found == members_.end())
            return Replan::Renegotiate;
        if (found->second.transitionAt != TimelineNs{})
            return Replan::Unchanged;
        output = found->second.output;
        capabilities = found->second.capabilities;
    }

    // The device's clock and its trim range are the room's; its formats are
    // narrowed to the one its stream is in. A grouped room resampled that way
    // keeps its container -- a 16-bit one for a 16-bit source -- rather than
    // the widest one a renegotiated plan would choose, which is the price of
    // not reopening its device.
    const OutputPlan current = output->plan();
    ClientCapabilities same = capabilities;
    same.formats = {current.outputFormat};
    same.preferredSampleRate = current.outputFormat.sampleRate;

    const Result<OutputPlan, PlanError> fresh =
        planner_.plan(current.sourceFormat, settingsFor(client), same);
    if (!fresh)
        return Replan::Renegotiate;
    if (fresh->mode == current.mode && fresh->driftActuator == current.driftActuator &&
        fresh->resamplerId == current.resamplerId && fresh->outputFormat == current.outputFormat)
    {
        return Replan::Unchanged;
    }

    Result<std::unique_ptr<IClientAudioProcessor>, ProcessorError> processor =
        makeClientAudioProcessor(*fresh, registry_, settings_.dither);
    if (!processor)
        return Replan::Renegotiate;
    if (const auto replaced = output->replaceProcessor(std::move(*processor)); !replaced)
    {
        MULTIROOM_INFO("zone {}: {} cannot be replanned in place: {}", id_.str(), client.str(),
                 describe(replaced.error()));
        return Replan::Renegotiate;
    }

    MULTIROOM_INFO("zone {}: {} replanned in place: {}", id_.str(), client.str(),
             describePlan(*fresh));
    alignLineToLoneRoom("it was replanned");
    return Replan::InPlace;
}

void Zone::alignLineToLoneRoom(const char* why)
{
    constexpr uint64_t kSmallestMoveNs = 1000000ULL;

    std::lock_guard<std::mutex> lock(mutex_);
    if (scheduler_.transport() != TransportState::Playing)
        return;

    const Member* lone = nullptr;
    ClientId loneId;
    for (const auto& [id, member] : members_)
    {
        const RoomStartState start = scheduler_.roomState(id);
        if (start != RoomStartState::Scheduled && start != RoomStartState::Playing &&
            start != RoomStartState::Paused)
        {
            continue;
        }
        if (lone != nullptr)
            return; // two rooms on the line already: they are what it is kept for
        lone = &member;
        loneId = id;
    }
    if (lone == nullptr || lone->sync == nullptr || lone->transitionAt != TimelineNs{} ||
        lone->flushUntilNs != 0)
    {
        return;
    }

    const std::optional<int64_t> steady = lone->sync->steadyOffsetNs();
    if (!steady)
        return;

    // Measured from its own target (phase 27): a room heard through a
    // receiver's DSP is meant to be that far ahead.
    const int64_t moveNs = *steady - lone->sync->status().targetOffsetNs;
    const uint64_t magnitude =
        moveNs < 0 ? static_cast<uint64_t>(-moveNs) : static_cast<uint64_t>(moveNs);
    if (magnitude < kSmallestMoveNs || magnitude > settings_.output.targetBufferNs)
        return;
    if (!scheduler_.alignLineTo(loneId, moveNs))
        return;

    // Its offset was measured against the line that has just moved, so the
    // controller starts it again from the new one; the card's drift is kept.
    lone->sync->clearOffset();
    MULTIROOM_INFO("zone {}: line moved {} us onto {}, which plays it alone: {}", id_.str(),
             moveNs / 1000, loneId.str(), why);
}

Status<ZoneError> Zone::moveRoom(const ClientId& client, RejoinTrigger trigger,
                                 TimelineNs& movedTo, const PlanForSource& replan)
{
    // One move at a time: the pipeline thread notices an eviction, the
    // listener's thread notices a device that was reopened or an offset no
    // ratio can close, and two of them moving one room would be two answers to
    // where that room is.
    std::lock_guard<std::mutex> moving(rejoinMutex_);

    std::shared_ptr<ClientOutput> output;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = members_.find(client);
        if (found == members_.end())
            return fail(ZoneError::NotMember, client.str() + " is not in zone " + id_.str());

        // Section 24: its stream is standing at a boundary while its device
        // is reopened, and the device's answer is what places it again.
        // Moving it now would start the old converter on the new source.
        if (found->second.transitionAt != TimelineNs{})
        {
            return fail(ZoneError::OutputFailed,
                        client.str() + " is held at the format boundary at " +
                            std::to_string(std::to_underlying(found->second.transitionAt)) +
                            " ns");
        }
        output = found->second.output;
    }

    // A room a stop parked is waiting for a source rather than for a position
    // (section 38), and the next play() is what moves it -- moving it here
    // would start it into a timeline nobody is playing. A failed or stopped
    // stream has nothing to move at all.
    const OutputState was = output->state();

    // A room a stop parked still takes the converter for its reopened device,
    // so that the play() which moves it sends what that device was opened in.
    // Planned against the newest source, which is where play() starts it.
    if (replan && was == OutputState::Parked)
    {
        AudioFormat source;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            source = sourceFormat_;
        }
        Result<std::unique_ptr<IClientAudioProcessor>, ZoneError> processor = replan(source);
        if (!processor)
            return std::unexpected(processor.error());
        if (const auto reformatted = output->reformat(std::move(*processor)); !reformatted)
            return fail(ZoneError::OutputFailed, describe(reformatted.error()));
    }

    if (was == OutputState::Failed || was == OutputState::Stopped ||
        was == OutputState::Parked)
    {
        std::string detail = client.str() + "'s stream is " + outputStateName(was);
        if (const std::string why = output->lastError(); !why.empty())
            detail += ": " + why;
        return fail(ZoneError::OutputFailed, std::move(detail));
    }

    // Section 43 step 5, and the only position this server ever rejoins a room
    // at: far enough ahead that the audio for it has not been heard yet, which
    // is the same answer a late joiner gets and for the same reason. Old
    // packets are never replayed to catch up.
    const Result<TimelineNs, ZoneError> position = joinPosition();
    if (!position)
        return std::unexpected(position.error());

    // Phase 40: refused while the room is where it is, rather than seeked
    // there and stranded again on its first read. A stranded room stays
    // stranded and the next pass asks again.
    if (const Result<SourceFrame, TimelineError> at = timeline_.frameAt(*position);
        !at || std::to_underlying(*at) < std::to_underlying(store_->oldestFrame()))
    {
        return fail(ZoneError::OutputFailed,
                    "timeline " + std::to_string(std::to_underlying(*position)) +
                        " ns is behind the oldest audio the store holds");
    }

    // A reopened device's converter is for the audio at the position the room
    // lands on. Once a format boundary has been opened that is the new
    // source's (joinPosition() never places a room before it), not the one
    // the room's current converter was planned for.
    std::unique_ptr<IClientAudioProcessor> processor;
    AudioFormat landsIn;
    if (replan)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            landsIn = formatAtLocked(*position);
        }
        Result<std::unique_ptr<IClientAudioProcessor>, ZoneError> made = replan(landsIn);
        if (!made)
            return std::unexpected(made.error());
        processor = std::move(*made);
    }

    // Outside mutex_, because park() joins that room's thread. A stranded room
    // has already stopped producing and park() only joins its thread; a room
    // that is still streaming is stopped here, and in both cases what it had
    // built for the position it is leaving goes.
    output->park(trigger == RejoinTrigger::DeviceRecovery);
    if (processor != nullptr)
    {
        if (const auto reformatted = output->reformat(std::move(processor)); !reformatted)
            return fail(ZoneError::OutputFailed, describe(reformatted.error()));
    }
    if (const auto moved = output->rejoinAt(*position); !moved)
        return fail(ZoneError::OutputFailed, describe(moved.error()));

    // reformat() dropped any hold the room had, because it was planned for
    // the device that has gone. A boundary still ahead of the room -- opened
    // while this move was being made -- is planned again for the new one,
    // before the room produces anything that could reach it.
    if (replan)
        holdAtBoundaryAhead(client, *position, landsIn);

    if (const auto running = output->run(); !running)
        return fail(ZoneError::OutputFailed, describe(running.error()));

    uint64_t count = 0;
    bool logged = true;
    uint64_t unlogged = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // The line places it again from what it reports about the audio it
        // holds *now*: everything it said before described the stream that has
        // just been replaced.
        scheduler_.rejoin(client, *position);

        const auto found = members_.find(client);
        if (found != members_.end())
        {
            count = ++found->second.rejoins;
            logged = found->second.moveLog.allow(clock_.monotonicNowNs(), unlogged);
            if (found->second.sync != nullptr)
            {
                // The offset it was measured to have was measured against a
                // position it is no longer at. What its card is doing is not
                // touched: the oscillator did not move with the stream.
                found->second.sync->clearOffset();
            }
        }
    }
    rejoins_.fetch_add(1);

    if (logged)
        MULTIROOM_INFO("zone {}: {} rejoins at timeline {} ns ({}), move {} ({} more moves since the "
                 "last line)",
                 id_.str(), client.str(), std::to_underlying(*position),
                 rejoinTriggerName(trigger), count, unlogged);
    else
        MULTIROOM_DEBUG("zone {}: {} rejoins at timeline {} ns ({}), move {}", id_.str(), client.str(),
                  std::to_underlying(*position), rejoinTriggerName(trigger), count);
    movedTo = *position;
    return {};
}

void Zone::rejoinOrWarn(const ClientId& client, RejoinTrigger trigger)
{
    if (const auto moved = rejoin(client, trigger); !moved)
    {
        // That room's failure and not the zone's (the fault-isolation table's
        // second row): it stays where it is and every other room plays on.
        // Asked again every pass, so said once an interval (phase 40).
        bool logged = true;
        uint64_t unlogged = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (const auto found = members_.find(client); found != members_.end())
                logged = found->second.moveLog.allow(clock_.monotonicNowNs(), unlogged);
        }
        if (logged)
            MULTIROOM_WARN("zone {}: {} could not be rejoined after {}: {} ({} more since the last "
                     "line)",
                     id_.str(), client.str(), rejoinTriggerName(trigger),
                     describe(moved.error()), unlogged);
        else
            MULTIROOM_DEBUG("zone {}: {} could not be rejoined after {}: {}", id_.str(), client.str(),
                      rejoinTriggerName(trigger), describe(moved.error()));
    }
}

// ---------------------------------------------------------------------------

void Zone::beginTransition(TimelineNs at, const AudioFormat& format, const ClientId* only)
{
    struct Room
    {
        ClientId client;
        std::shared_ptr<ClientOutput> output;
        ClientCapabilities capabilities;
    };
    std::vector<Room> rooms;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : members_)
        {
            if (only != nullptr && entry.first != *only)
                continue;

            // Still reopening for an earlier boundary, past its timeout: it is
            // placed in whatever format is current when its device answers.
            if (entry.second.transitionAt != TimelineNs{})
                continue;
            rooms.push_back({entry.first, entry.second.output, entry.second.capabilities});
        }
    }

    // Outside the lock: the planner and the room's preference belong to other
    // modules, and building a converter is not work to do while the two
    // listeners wait.
    std::vector<std::pair<ClientId, TimelineNs>> held;
    for (const Room& room : rooms)
    {
        const OutputState state = room.output->state();
        if (state == OutputState::Failed || state == OutputState::Stopped)
            continue;

        // The boundary, or where this room's stream begins if it joined on
        // the far side of it and has produced nothing of the old source.
        const TimelineNs hold{std::max(std::to_underlying(at),
                                       std::to_underlying(room.output->producedThroughNs()))};
        if (const auto holding = room.output->holdAt(hold); !holding)
        {
            MULTIROOM_WARN("zone {}: {} could not be held at timeline {} ns: {}", id_.str(),
                     room.client.str(), std::to_underlying(hold), describe(holding.error()));
            continue;
        }

        // The table of section 24: a room whose device keeps its format is
        // sent the new source through a new converter and never stops -- the
        // "fixed rate, server resamples every source into it" row. Only a
        // room whose device would have to change is paused for it.
        const RoomAudioSettings settings = settingsFor(room.client);
        const Result<OutputPlan, PlanError> fresh =
            planner_.plan(format, settings, room.capabilities);
        if (fresh && fresh->outputFormat == room.output->plan().outputFormat)
        {
            Result<std::unique_ptr<IClientAudioProcessor>, ProcessorError> processor =
                makeClientAudioProcessor(*fresh, registry_, settings_.dither);
            if (processor)
            {
                if (const auto switched = room.output->switchTo(std::move(*processor), hold);
                    switched)
                {
                    MULTIROOM_INFO("zone {}: {} plays through the boundary: {}", id_.str(),
                             room.client.str(), describePlan(*fresh));
                    continue;
                }
            }
        }

        held.emplace_back(room.client, hold);
    }

    std::vector<RoomCommand> pauses;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [client, hold] : held)
        {
            const auto found = members_.find(client);
            if (found == members_.end())
                continue;
            found->second.transitionAt = hold;
            found->second.renegotiatedAtNs = 0;

            // Section 24 step 2: stopped cleanly on the boundary, on the
            // instant the line reaches it, with its device left open until
            // the old source has been played to its end.
            if (std::optional<RoomCommand> pause = scheduler_.pauseForTransition(client, at))
                pauses.push_back(std::move(*pause));
        }
    }
    dispatch(std::move(pauses));

    for (const auto& [client, hold] : held)
    {
        MULTIROOM_INFO("zone {}: {} is held at timeline {} ns until its device is reopened",
                 id_.str(), client.str(), std::to_underlying(hold));
    }
}

void Zone::advanceTransitions()
{
    std::vector<RoomCommand> commands;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t nowNs = clock_.monotonicNowNs();
        const bool started = scheduler_.started();
        const uint64_t playhead = std::to_underlying(scheduler_.playheadAt(nowNs));

        uint64_t reached = std::numeric_limits<uint64_t>::max();
        for (auto& entry : members_)
        {
            Member& member = entry.second;
            if (member.transitionAt == TimelineNs{} || member.renegotiatedAtNs != 0)
                continue;

            // Held while it was not on the line -- placed again after an
            // earlier boundary when this one was opened -- and started since:
            // paused on the boundary like the rooms that were playing then
            // (step 2). One that was paused already is not paused again.
            if (std::optional<RoomCommand> pause =
                    scheduler_.pauseForTransition(entry.first, member.transitionAt))
            {
                commands.push_back(std::move(*pause));
            }

            // Not before the line gets there: the room is still playing the
            // old source out of the device that is about to be reopened. With
            // no line at all there is nothing to play out, so it is now.
            const uint64_t boundary = std::to_underlying(member.transitionAt);
            if (started && playhead < boundary)
                continue;

            member.renegotiatedAtNs = std::max<uint64_t>(nowNs, 1);
            reached = std::min(reached, boundary);

            // Section 24 steps 3 and 4 in one message: the session asks this
            // zone for the room's format again, which is now the new source's,
            // and sends SESSION_CONFIG and PREPARE_STREAM together.
            RoomCommand command;
            command.kind = RoomCommandKind::Renegotiate;
            command.client = entry.first;
            commands.push_back(std::move(command));
        }

        // Nobody left playing through: the line waits at the boundary, which
        // is what lets the rooms that reopened resume on it (step 8) rather
        // than wherever it would have run on to while they did.
        if (started && reached != std::numeric_limits<uint64_t>::max())
            static_cast<void>(scheduler_.freezeForTransition(TimelineNs{reached}, nowNs));
    }
    dispatch(std::move(commands));
}

bool Zone::completeTransition(const ClientId& client, const AudioFormat& granted)
{
    std::shared_ptr<ClientOutput> output;
    ClientCapabilities capabilities;
    TimelineNs at{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = members_.find(client);
        if (found == members_.end() || found->second.transitionAt == TimelineNs{} ||
            found->second.renegotiatedAtNs == 0)
        {
            return false;
        }
        output = found->second.output;
        capabilities = found->second.capabilities;
        at = found->second.transitionAt;
    }

    // That room's failure and not the zone's: it stays held, silent, and says
    // why in its status as well as in the log, and every other room plays on.
    // A later STREAM_READY -- a device that changed -- tries again.
    const auto unserved = [&](const std::string& why) {
        const std::string detail = "cannot be served after the format boundary at " +
                                   std::to_string(std::to_underlying(at)) + " ns: " + why;
        MULTIROOM_WARN("zone {}: {} {}", id_.str(), client.str(), detail);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            scheduler_.explain(client, detail);
        }
        ZoneEvent event = eventOf(ZoneEventKind::RoomNotReady);
        event.client = client;
        event.reason = detail;
        emit({event});
        return true;
    };

    // Step 7: from the boundary, so that the room can be started exactly
    // there or anywhere after it the line has reached -- unless the store has
    // let go of it while this device took its time, in which case the room is
    // a late joiner like any other (step 10).
    TimelineNs position = at;
    if (std::to_underlying(at) < std::to_underlying(oldestNs()))
    {
        const Result<TimelineNs, ZoneError> later = joinPosition();
        if (!later)
            return unserved("no position to carry on from: " + describe(later.error()));
        position = *later;
    }

    AudioFormat format;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        format = formatAtLocked(position);
    }

    // Planned again against what the driver granted (section 12), because
    // that is now the only format this room can be sent -- exactly as admit()
    // does for a room arriving.
    const RoomAudioSettings settings = settingsFor(client);
    Result<OutputPlan, PlanError> plan = planner_.plan(format, settings, capabilities);
    if (plan && granted.isValid())
        plan = planner_.replanForGrantedFormat(*plan, settings, granted, capabilities);
    if (!plan)
        return unserved(describe(plan.error()));

    Result<std::unique_ptr<IClientAudioProcessor>, ProcessorError> processor =
        makeClientAudioProcessor(*plan, registry_, settings_.dither);
    if (!processor)
        return unserved(describe(processor.error()));
    if (const auto switched = output->switchTo(std::move(*processor), position); !switched)
        return unserved(describe(switched.error()));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = members_.find(client);
        if (found != members_.end())
        {
            found->second.transitionAt = TimelineNs{};
            found->second.renegotiatedAtNs = 0;

            // A new nominal rate and a device counter started from nothing
            // (section 25), and the same oscillator under both.
            if (found->second.sync != nullptr)
                found->second.sync->carryOver(plan->outputFormat.sampleRate);
        }

        // Steps 8 to 10 are the barrier's: a line waiting at the boundary
        // starts this room there, and a running one places it wherever it has
        // got to inside what this room holds -- or asks for it to be moved
        // once it has run past all of it.
        scheduler_.rejoin(client, position);
    }

    // A later boundary was opened while this room was held at this one, and
    // passed it by (beginTransition() leaves a held room to its device): it is
    // held there now, behind the converter just named.
    holdAtBoundaryAhead(client, position, plan->sourceFormat);

    MULTIROOM_INFO("zone {}: {} carries on from timeline {} ns: {}", id_.str(), client.str(),
             std::to_underlying(position), describePlan(*plan));
    return true;
}

void Zone::holdAtBoundaryAhead(const ClientId& client, TimelineNs position,
                               const AudioFormat& format)
{
    std::optional<std::pair<TimelineNs, AudioFormat>> ahead;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const StagedSource& source : sources_)
        {
            if (std::to_underlying(source.timelineStartNs) > std::to_underlying(position) &&
                source.format != format)
            {
                ahead.emplace(source.timelineStartNs, source.format);
                break; // ordered oldest first: the first is the room's next boundary
            }
        }
    }
    if (ahead)
        beginTransition(ahead->first, ahead->second, &client);
}

bool Zone::transitionPendingLocked(uint64_t nowNs) const
{
    for (const auto& entry : members_)
    {
        const Member& member = entry.second;
        if (member.transitionAt == TimelineNs{})
            continue;
        if (member.renegotiatedAtNs == 0 ||
            nowNs < member.renegotiatedAtNs + settings_.start.readyTimeoutNs)
        {
            return true;
        }
    }
    return false;
}

AudioFormat Zone::formatAtLocked(TimelineNs position) const
{
    AudioFormat format = sourceFormat_;
    for (const StagedSource& source : sources_)
    {
        if (std::to_underlying(source.timelineStartNs) <= std::to_underlying(position))
            format = source.format; // ordered oldest first, so the last one wins
    }
    return format;
}

uint64_t Zone::rejoins(const ClientId& client) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto found = members_.find(client); found != members_.end())
        return found->second.rejoins;
    if (const auto kept = retained_.find(client); kept != retained_.end())
        return kept->second.rejoins;
    return 0;
}

bool Zone::retains(const ClientId& client) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return retained_.contains(client);
}

// ---------------------------------------------------------------------------

void Zone::emit(const std::vector<ZoneEvent>& events) const
{
    if (!settings_.onEvent)
        return;
    for (const ZoneEvent& event : events)
        settings_.onEvent(event);
}

ZoneEvent Zone::eventOf(ZoneEventKind kind) const
{
    ZoneEvent event;
    event.kind = kind;
    event.zone = id_;
    return event;
}

void Zone::noticeLocked(std::vector<ZoneEvent>& events, std::optional<TimelineNs> stoppedAt)
{
    const uint64_t nowNs = clock_.monotonicNowNs();
    const TransportState transport = scheduler_.transport();

    // Observed rather than reported by each caller, so that a change nobody
    // asked for -- a line frozen at a format boundary, a barrier resolving on
    // the pipeline thread -- is announced exactly like one that was asked for.
    if (transport != announcedTransport_)
    {
        announcedTransport_ = transport;
        ZoneEvent event = eventOf(ZoneEventKind::Transport);
        event.transport = transport;
        event.timelineNs = stoppedAt ? *stoppedAt : scheduler_.playheadAt(nowNs);
        events.push_back(std::move(event));
    }

    // "Now playing" is the instant a source's first frame is audible on the
    // line, which is the instant its TRACK_BOUNDARY names -- not when it was
    // decoded, which is a lookahead earlier. Only a running line makes
    // anything audible.
    if (transport == TransportState::Playing)
    {
        const uint64_t anchor = std::to_underlying(scheduler_.startTimelineNs());
        const uint64_t anchoredAt = scheduler_.startServerTimeNs();
        for (; startedThrough_ < sources_.size(); ++startedThrough_)
        {
            const StagedSource& source = sources_[startedThrough_];
            const uint64_t begins = std::max(std::to_underlying(source.timelineStartNs), anchor);
            const uint64_t heardAt = anchoredAt + (begins - anchor);
            if (nowNs < heardAt)
                break;

            // Failed before its first frame: the next source begins where it
            // would have, and nothing of this one was ever heard.
            if (source.ended && source.frames == 0)
                continue;

            ZoneEvent event = eventOf(ZoneEventKind::SourceStarted);
            event.sourceId = source.segment.sourceId;
            event.trackId = source.segment.trackId;
            event.timelineNs = source.timelineStartNs;
            event.serverTimeNs = heardAt;
            event.startOffsetNs = source.segment.startOffsetNs;
            events.push_back(std::move(event));
        }
    }

    // Played out, not decoded out: the rooms are still sounding the last
    // lookahead of it when the decode ends. With no line and nobody to start
    // one there is nothing left to play out.
    if (state_ == ZoneState::Ended && !endAnnounced_ && !sources_.empty())
    {
        const StagedSource& last = sources_.back();
        const uint64_t endNs =
            std::to_underlying(last.timelineStartNs) +
            nanosecondsFromFrames(last.frames, last.format.sampleRate).value_or(0);
        const bool playedOut =
            transport == TransportState::Playing
                ? std::to_underlying(scheduler_.playheadAt(nowNs)) >= endNs
                : transport == TransportState::Stopped && members_.empty();
        if (playedOut)
        {
            endAnnounced_ = true;
            ZoneEvent event = eventOf(ZoneEventKind::Ended);
            event.sourceId = last.segment.sourceId;
            events.push_back(std::move(event));
        }
    }

    // Section 20's third strike, once per time a room reaches it.
    for (const auto& entry : members_)
    {
        if (scheduler_.roomState(entry.first) != RoomStartState::LeftBehind)
        {
            leftBehind_.erase(entry.first);
            continue;
        }
        if (!leftBehind_.insert(entry.first).second)
            continue;
        ZoneEvent event = eventOf(ZoneEventKind::RoomLeftBehind);
        event.client = entry.first;
        event.reason = scheduler_.roomDetail(entry.first);
        events.push_back(std::move(event));
    }
}

// ---------------------------------------------------------------------------

void Zone::retainLocked(const ClientId& client, Member& leaving)
{
    RetainedRoom keep;
    keep.sync = leaving.sync;
    keep.rejoins = leaving.rejoins;
    keep.sinceNs = clock_.monotonicNowNs();
    retained_[client] = std::move(keep);

    // Memory is bounded from this side too: a house of sixteen rooms cannot
    // cost more than sixteen rooms' worth of kept state however many clients
    // have ever connected to it, and the one that has been away longest is the
    // one least likely to come back.
    while (retained_.size() > settings_.maxMembers)
    {
        const auto oldest = std::min_element(
            retained_.begin(), retained_.end(), [](const auto& a, const auto& b) {
                return a.second.sinceNs < b.second.sinceNs;
            });
        MULTIROOM_DEBUG("zone {}: forgetting {}'s kept state", id_.str(), oldest->first.str());
        retained_.erase(oldest);
    }
}

bool Zone::disconnect(const ClientId& client, const std::optional<SessionId>& session)
{
    // Against a move in flight on the pipeline thread: a room being taken away
    // while it is being rejoined would be stopped and then started again.
    std::lock_guard<std::mutex> moving(rejoinMutex_);

    Member ending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = members_.find(client);
        if (found == members_.end())
            return true;
        if (session && found->second.session != *session)
            return false;
        ending = std::move(found->second);
        members_.erase(found);
        scheduler_.leave(client);
        leftBehind_.erase(client);
        retainLocked(client, ending);
    }

    // Outside the lock: stop() joins that room's thread, and nothing else
    // about this zone stops with it.
    ending.output->stop();
    MULTIROOM_INFO("zone {}: {} disconnected; its drift model and settings are kept for its return",
             id_.str(), client.str());
    return true;
}

Status<ZoneError> Zone::release(const ClientId& client)
{
    std::lock_guard<std::mutex> moving(rejoinMutex_); // as disconnect(), and for the same race

    Member ending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = members_.find(client);
        if (found == members_.end())
            return fail(ZoneError::NotMember, client.str() + " is not in zone " + id_.str());
        ending = std::move(found->second);
        members_.erase(found);
        scheduler_.leave(client);
        leftBehind_.erase(client);

        // A room taken out of a zone is not a room on its way back, so
        // nothing about it is kept -- and a room that disconnected and was
        // then removed keeps nothing either.
        retained_.erase(client);
    }

    // Outside the lock: stop() joins that room's thread, and nothing else
    // about this zone stops with it.
    ending.output->stop();
    MULTIROOM_INFO("zone {}: {} left", id_.str(), client.str());
    return {};
}

std::shared_ptr<ClientOutput> Zone::member(const ClientId& client) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = members_.find(client);
    return found == members_.end() ? nullptr : found->second.output;
}

std::vector<ClientId> Zone::memberIds() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ClientId> ids;
    ids.reserve(members_.size());
    for (const auto& entry : members_)
        ids.push_back(entry.first);
    return ids;
}

size_t Zone::memberCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return members_.size();
}

// ---------------------------------------------------------------------------

bool Zone::attach(const StreamAttachment& attachment, std::unique_ptr<IPacketSink> sink)
{
    std::shared_ptr<ClientOutput> output;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = members_.find(attachment.client);
        if (found == members_.end() || found->second.session != attachment.session)
            return false;
        output = found->second.output;
    }

    output->attach(std::move(sink));
    return true;
}

void Zone::releaseAudioConnection(const ClientId& client)
{
    const std::shared_ptr<ClientOutput> output = member(client);
    if (output == nullptr)
        return;

    // What is queued and where the room is on the timeline are kept, exactly
    // as for a connection that dropped: only the socket goes, before anything
    // more is written into one the client is about to close.
    output->detach();
    MULTIROOM_INFO("zone {}: {}'s audio connection let go: its device is being reopened", id_.str(),
             client.str());
}

bool Zone::holds(const StreamAttachment& attachment) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = members_.find(attachment.client);
    return found != members_.end() && found->second.session == attachment.session;
}

void Zone::onBufferStatus(const ClientId& client, const matp::BufferStatus& status,
                          uint64_t clockExchanges)
{
    std::shared_ptr<ClientOutput> output;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_.onBufferStatus(client, status, clockExchanges);

        const auto found = members_.find(client);
        if (found == members_.end())
            return;
        output = found->second.output;
    }

    output->onBufferStatus(status);
}

void Zone::onStreamReady(const ClientId& client, bool ready, const AudioFormat& granted,
                         const ClientCapabilities* offered)
{
    bool reopened = false;
    std::shared_ptr<ClientOutput> output;
    ClientCapabilities capabilities;
    OutputPlan current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_.onStreamReady(client, ready);

        // Section 12's re-negotiation, read the way section 25 asks it to be:
        // a device that has answered STREAM_READY again is a device that was
        // reopened, so its frame counter started from nothing and the window
        // this room's drift was fitted over describes a clock that no longer
        // exists. What that card was measured to be *doing* is carried across
        // it (phase 16), because reopening a device does not change its
        // oscillator. The rate is the one this room's plan settled -- a reopen
        // that changes it is section 24's transition, below, or a device
        // reopened in another format, replanned after this block.
        if (!ready)
            return;
        const auto found = members_.find(client);
        if (found == members_.end())
            return;

        if (found->second.transitionAt == TimelineNs{})
        {
            // What this room's device can do now: the device may be another
            // card than the one it was admitted with.
            if (offered != nullptr)
                found->second.capabilities = *offered;
            output = found->second.output;
            capabilities = found->second.capabilities;
            current = output->plan();
            if (found->second.sync != nullptr &&
                (!granted.isValid() || granted == current.outputFormat))
            {
                found->second.sync->carryOver(current.outputFormat.sampleRate);
            }
            reopened = true;
        }
    }

    // Section 24 step 5: this is the answer the transition asked for, and the
    // device was reopened in the new format on purpose. A room held at a
    // boundary that answers before it was asked -- its device went away on
    // its own -- stays held: the line reaching the boundary is what asks it.
    if (!reopened)
    {
        static_cast<void>(completeTransition(client, granted));
        return;
    }

    // A reopened device is an empty ring: everything this server already sent
    // this room went with the device, and the stream it was sending is ahead
    // of anything the room can still play. So it is moved rather than left to
    // be started at a position it does not hold -- outside the lock, because a
    // move stops that room's thread.
    const RoomAudioSettings settings = settingsFor(client);
    if (!granted.isValid() || granted == current.outputFormat)
    {
        // The same format: moved on the plan it has -- unless the device now
        // confirms or no longer confirms its clock trim (MATP section 61),
        // which changes the actuator and with it the converter's ratio mode,
        // and is replanned below like a changed format.
        const Result<OutputPlan, PlanError> again =
            planner_.plan(current.sourceFormat, settings, capabilities);
        if (!again || again->driftActuator == current.driftActuator)
        {
            rejoinOrWarn(client, RejoinTrigger::DeviceRecovery);
            return;
        }
    }
    const AudioFormat opened = granted.isValid() ? granted : current.outputFormat;

    // Reopened in another format, or for another actuator: planned again
    // against what the driver granted, exactly as admit() plans a room
    // arriving, and moved with that converter -- the old one would send the
    // old format's bytes to a device opened in the new one. Planned by the
    // move, for the source at the position it lands on: not the one this room
    // plays now when a format boundary has been opened.
    const PlanForSource replan = [&](const AudioFormat& source)
        -> Result<std::unique_ptr<IClientAudioProcessor>, ZoneError> {
        Result<OutputPlan, PlanError> plan = planner_.plan(source, settings, capabilities);
        if (plan)
            plan = planner_.replanForGrantedFormat(*plan, settings, opened, capabilities);
        if (!plan)
            return fail(ZoneError::PlanFailed, describe(plan.error()));
        Result<std::unique_ptr<IClientAudioProcessor>, ProcessorError> made =
            makeClientAudioProcessor(*plan, registry_, settings_.dither);
        if (!made)
            return fail(ZoneError::PlanFailed, describe(made.error()));

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = members_.find(client);
            if (found != members_.end() && found->second.sync != nullptr)
                found->second.sync->carryOver(plan->outputFormat.sampleRate);
        }
        MULTIROOM_INFO("zone {}: {} reopened in {}: {}", id_.str(), client.str(),
                 describeFormat(opened), describePlan(*plan));
        return std::move(*made);
    };

    const auto moved = rejoin(client, RejoinTrigger::DeviceRecovery, replan);
    if (moved)
        return;
    if (moved.error().code != ZoneError::PlanFailed)
    {
        MULTIROOM_WARN("zone {}: {} could not be rejoined after {}: {}", id_.str(), client.str(),
                 rejoinTriggerName(RejoinTrigger::DeviceRecovery), describe(moved.error()));
        return;
    }

    // That room's failure and not the zone's, as at a format boundary.
    const std::string detail = "cannot be served in the reopened device's " +
                               describeFormat(opened) + ": " + moved.error().detail;
    MULTIROOM_WARN("zone {}: {} {}", id_.str(), client.str(), detail);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_.explain(client, detail);
    }
    ZoneEvent event = eventOf(ZoneEventKind::RoomNotReady);
    event.client = client;
    event.reason = detail;
    emit({event});
}

void Zone::onPlaybackReport(const ClientId& client, const matp::PlaybackReport& report,
                            const ClientClockModel& clock)
{
    SyncSample outcome = SyncSample::Accepted;
    ClientSyncStatus status;
    uint64_t disagreementsBefore = 0;
    std::shared_ptr<ClientOutput> output;
    bool feedback = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduler_.onPlaybackReport(client, report);

        const auto found = members_.find(client);
        if (found == members_.end() || found->second.sync == nullptr)
            return;

        // A room held at a format boundary is standing still on purpose while
        // its device is reopened, so what it reports is neither a drift nor an
        // offset: the device counting is the one being closed, and the line
        // runs on without it. Its series starts again with the new stream.
        if (found->second.transitionAt != TimelineNs{})
            return;
        output = found->second.output;
        feedback = found->second.capabilities.driftFeedback;

        // The actuator is the plan's, which a format boundary can change; the
        // controller is told on every report rather than at every replan.
        const matp::HardwareClockCapability& range = found->second.capabilities.hardwareClock;
        found->second.sync->setHardwareClock(
            output != nullptr && output->driftActuator() == DriftActuator::ClientHardwareClock,
            range.minPpm, range.maxPpm);

        // Section 54: between the cut and the seeked audio the room plays
        // silence and reports the cut as audible, which is a position that is
        // not playing. Its reports feed nothing until the seeked audio is
        // heard, and the model is kept, because the mapping did not move.
        if (found->second.flushUntilNs != 0)
        {
            if (!clock.valid() ||
                clock.toServerTimeNs(report.localMonotonicTimeNs) < found->second.flushUntilNs)
            {
                outcome = SyncSample::Stepped;
            }
            else
            {
                found->second.flushUntilNs = 0;
            }
        }

        // What the zone's line says should have been audible at the instant
        // this report names. Absent before the first START_AT: there is no
        // line yet, so there is no offset -- which leaves the drift
        // regression, and a card's rate can be measured long before anything
        // is audible.
        std::optional<TimelineNs> expected;
        if (scheduler_.started() && clock.valid())
            expected = scheduler_.playheadAt(clock.toServerTimeNs(report.localMonotonicTimeNs));

        disagreementsBefore = found->second.sync->status().disagreements;
        if (outcome == SyncSample::Accepted)
            outcome = found->second.sync->onPlaybackReport(report, clock, expected);
        status = found->second.sync->status();
    }

    // Section 26: two independent derivations of one number, and where they
    // disagree it is the clock model between them that is suspect rather than
    // the sound card. Logged, and deliberately not averaged into anything.
    if (status.disagreements != disagreementsBefore)
    {
        MULTIROOM_WARN("zone {}: {} drift disagrees: server {:.2f} ppm (confidence {:.2f}), client "
                 "{:.2f} ppm on its own clock, {:.2f} ppm on this server's (confidence {:.2f}) "
                 "-- suspect the clock model, not the card",
                 id_.str(), client.str(), status.driftPpm, status.confidence,
                 status.clientDriftPpm, status.driftPpm - status.lastDisagreementPpm,
                 status.clientConfidence);
    }

    if (outcome != SyncSample::Accepted)
    {
        MULTIROOM_DEBUG("zone {}: {} playback report {}", id_.str(), client.str(),
                  syncSampleName(outcome));
        return;
    }

    // Phase 13: the controller's answer, applied. Outside the lock, because
    // this reaches another room's own object -- and only ever offered to a
    // room whose plan was made for a correction, so that the decision an
    // operator reads in the log is the decision the room got. ClientOutput
    // refuses one anyway; a bitperfect room being left alone is a rule that
    // holds at both ends or it is not a rule.
    const bool corrected = output != nullptr && output->driftCorrected();
    const bool hardware =
        output != nullptr && output->driftActuator() == DriftActuator::ClientHardwareClock;

    // The measurement goes back to every client that reads it, and with it
    // the command where its own clock is the actuator. A room the server
    // resamples is never sent a target, so it never moves its clock.
    if (feedback)
    {
        RoomCommand command;
        command.kind = RoomCommandKind::DriftFeedback;
        command.client = client;
        command.driftFeedback.hasDrift = status.hasDrift;
        command.driftFeedback.measuredDriftPpm = status.driftPpm;
        command.driftFeedback.driftConfidence = status.confidence;
        command.driftFeedback.hasHardwareTarget = hardware;
        command.driftFeedback.hardwareTargetPpm = status.hardwareTargetPpm;
        dispatch({std::move(command)});
    }

    // Section 32, and phase 13's line through it. An offset this large in a
    // room the controller is holding near zero did not accumulate -- it is a
    // step, and no ratio inside section 31's limits is going to close it
    // before the room is audibly out. A room that is *not* corrected is left
    // alone: it has no threshold at all, because drift is what the operator
    // chose for it and a reposition every quarter of an hour is the worse
    // answer. Its steps still reach it, through the triggers that are events
    // rather than measurements.
    if (corrected && status.resyncWanted)
    {
        MULTIROOM_WARN("zone {}: {} is {} us off the timeline, which no ratio closes", id_.str(),
                 client.str(), status.offsetNs / 1000);
        rejoinOrWarn(client, RejoinTrigger::HardResync);
        return;
    }

    if (corrected && !hardware)
        output->setDriftTrim(status.ratio);

    // The room's whole sync state in one line: what its card is doing, how far
    // out it is, what the controller asked for, and what it was actually given
    // -- the last two differing by the confidence, which is what stops a room
    // that cannot be measured from being pulled about by its own noise.
    if (hardware)
    {
        MULTIROOM_DEBUG("zone {}: {} residual drift {:.2f} +/- {:.2f} ppm (confidence {:.2f}), "
                  "offset {} us, hardware clock {:.2f} ppm, programmed {} ppm",
                  id_.str(), client.str(), status.driftPpm, status.standardErrorPpm,
                  status.confidence, status.hasOffset ? status.offsetNs / 1000 : 0,
                  status.hardwareTargetPpm, status.clientClockPpm);
        return;
    }
    if (corrected)
    {
        MULTIROOM_DEBUG("zone {}: {} drift {:.2f} +/- {:.2f} ppm (confidence {:.2f}, {} samples), "
                  "offset {} us, correction {:.2f} ppm, applied {:.2f} ppm",
                  id_.str(), client.str(), status.driftPpm, status.standardErrorPpm,
                  status.confidence, status.samples,
                  status.hasOffset ? status.offsetNs / 1000 : 0, status.correctionPpm,
                  status.appliedPpm);
        return;
    }

    MULTIROOM_DEBUG("zone {}: {} drift {:.2f} +/- {:.2f} ppm (confidence {:.2f}, {} samples), offset "
              "{} us, correction {:.2f} ppm (not applied: {})",
              id_.str(), client.str(), status.driftPpm, status.standardErrorPpm,
              status.confidence, status.samples, status.hasOffset ? status.offsetNs / 1000 : 0,
              status.correctionPpm,
              output == nullptr ? "this room has no output"
                                : driftDisabledText(output->plan().driftDisabledBy));
}

// ---------------------------------------------------------------------------

bool Zone::started() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return scheduler_.started();
}

TimelineNs Zone::playheadNs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return scheduler_.playheadAt(clock_.monotonicNowNs());
}

TimelineNs Zone::startTimelineNs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return scheduler_.startTimelineNs();
}

uint64_t Zone::startServerTimeNs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return scheduler_.startServerTimeNs();
}

TransportState Zone::transport() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return scheduler_.transport();
}

RoomStartState Zone::roomStart(const ClientId& client) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return scheduler_.roomState(client);
}

std::string Zone::roomStartDetail(const ClientId& client) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return scheduler_.roomDetail(client);
}

ClientSyncStatus Zone::syncStatus(const ClientId& client) const
{
    std::shared_ptr<ClientSyncController> sync;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = members_.find(client);
        if (found == members_.end())
            return {};
        sync = found->second.sync;
    }
    return sync == nullptr ? ClientSyncStatus{} : sync->status();
}

uint64_t Zone::startsSent() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return scheduler_.startsSent();
}

uint64_t Zone::leftBehindCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return scheduler_.leftBehindCount();
}

} // namespace multiroom
