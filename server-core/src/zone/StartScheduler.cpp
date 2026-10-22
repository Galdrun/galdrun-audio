#include "zone/StartScheduler.h"

#include "util/Logger.h"

#include <algorithm>
#include <utility>

namespace multiroom {

const char* roomStartStateName(RoomStartState state)
{
    switch (state)
    {
    case RoomStartState::Expecting:
        return "expecting";
    case RoomStartState::Waiting:
        return "waiting";
    case RoomStartState::Ready:
        return "ready";
    case RoomStartState::Scheduled:
        return "scheduled";
    case RoomStartState::Playing:
        return "playing";
    case RoomStartState::Paused:
        return "paused";
    case RoomStartState::Rejoining:
        return "rejoining";
    case RoomStartState::LeftBehind:
        return "leftBehind";
    }
    return "unknown";
}

const char* transportStateName(TransportState state)
{
    switch (state)
    {
    case TransportState::Stopped:
        return "stopped";
    case TransportState::Starting:
        return "starting";
    case TransportState::Playing:
        return "playing";
    case TransportState::Paused:
        return "paused";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------

StartScheduler::StartScheduler(StartSchedulerSettings settings) : settings_(settings)
{
    // A zero here would be a barrier that never waits or a start always in the
    // past, and neither has a caller that means it. The zone validates its own
    // settings; these are the floors that keep the arithmetic below honest.
    if (settings_.startDelayNs == 0)
        settings_.startDelayNs = 1;
    if (settings_.startAttempts == 0)
        settings_.startAttempts = 1;
}

// ---------------------------------------------------------------------------

void StartScheduler::expect(const ClientId& client, uint64_t nowNs)
{
    const auto found = rooms_.find(client);
    if (found != rooms_.end())
        return; // already known; the deadline is not restarted by a re-negotiation

    Room room;
    room.deadlineNs = nowNs + settings_.readyTimeoutNs;
    rooms_.emplace(client, room);
}

void StartScheduler::join(const ClientId& client, TimelineNs origin, uint64_t nowNs)
{
    Room& room = rooms_[client];
    if (room.deadlineNs == 0)
        room.deadlineNs = nowNs + settings_.readyTimeoutNs;
    room.member = true;
    room.origin = origin;

    // admit() only happens on a device that granted a stream, so membership
    // carries section 12's first condition with it.
    room.streamReady = true;
    if (room.state == RoomStartState::Expecting)
        room.state = RoomStartState::Waiting;
}

void StartScheduler::leave(const ClientId& client)
{
    rooms_.erase(client);
    std::erase(rejoinWanted_, client);
}

void StartScheduler::rejoin(const ClientId& client, TimelineNs origin)
{
    std::erase(rejoinWanted_, client);

    const auto found = rooms_.find(client);
    if (found == rooms_.end())
        return;

    Room& room = found->second;
    room.origin = origin;

    // Everything this room told this class describes the stream it had. The
    // buffer report is the one that matters: it names the positions the room
    // holds, and those are the positions the move has just replaced -- so it
    // is dropped rather than trusted, and the next BUFFER_STATUS is what makes
    // this room schedulable again (section 43 step 5, from this end).
    room.buffer = matp::BufferStatus();
    room.attempts = 0;
    room.startServerTimeNs = 0;
    room.detail.clear();
    room.state = room.member ? RoomStartState::Waiting : RoomStartState::Expecting;
}

void StartScheduler::setLatency(const ClientId& client, uint64_t latencyNs)
{
    if (const auto found = rooms_.find(client); found != rooms_.end())
        found->second.latencyNs = latencyNs;
}

void StartScheduler::setTiming(uint64_t minimumStartBufferNs, uint64_t readyTimeoutNs,
                               uint64_t startDelayNs)
{
    settings_.minimumStartBufferNs = minimumStartBufferNs;
    settings_.readyTimeoutNs = readyTimeoutNs;
    // The constructor's floor, for the constructor's reason.
    settings_.startDelayNs = std::max<uint64_t>(startDelayNs, 1);
}

std::vector<ClientId> StartScheduler::takeRejoins()
{
    std::vector<ClientId> wanted;
    wanted.swap(rejoinWanted_);
    return wanted;
}

// ---------------------------------------------------------------------------

void StartScheduler::onStreamReady(const ClientId& client, bool ready)
{
    const auto found = rooms_.find(client);
    if (found == rooms_.end())
        return;

    Room& room = found->second;
    room.streamReady = ready;
    if (ready)
    {
        // Section 20's table: a room that reports ready again is scheduled
        // again, from whatever state dropping it left it in. Its buffer is a
        // new device's, so what it said about the old one is not an answer --
        // and neither is a pause, which named a position on a frame axis that
        // has just been replaced.
        room.buffer = matp::BufferStatus();
        if (room.state == RoomStartState::LeftBehind || room.state == RoomStartState::Playing ||
            room.state == RoomStartState::Paused || room.state == RoomStartState::Scheduled)
        {
            room.state = RoomStartState::Waiting;
            room.detail.clear();
            room.attempts = 0;
            room.startServerTimeNs = 0;
        }
        return;
    }

    // Dropped from scheduling, and nothing else: its output, its plan and its
    // place in the zone are kept, because the device this room is about to
    // reopen is the same room.
    room.buffer = matp::BufferStatus();
    room.attempts = 0;
    room.startServerTimeNs = 0;
    room.state = room.member ? RoomStartState::Waiting : RoomStartState::Expecting;
    room.detail.clear();
}

void StartScheduler::onBufferStatus(const ClientId& client, const matp::BufferStatus& status,
                                    uint64_t clockExchanges)
{
    const auto found = rooms_.find(client);
    if (found == rooms_.end())
        return;

    found->second.clockExchanges = clockExchanges;

    // A room moved to `origin` still has reports in flight about the stream it
    // had: it plays that one until the STOP_AT or the move reaches it. Its new
    // stream holds nothing before `origin`, so a report naming a position there
    // describes audio it no longer has -- and a barrier formed on it would
    // start the room where it has nothing to play.
    if (status.hasContent && status.playheadTimelineNs < std::to_underlying(found->second.origin))
        return;

    found->second.buffer = status;
}

void StartScheduler::onPlaybackReport(const ClientId& client, const matp::PlaybackReport& report)
{
    const auto found = rooms_.find(client);
    if (found == rooms_.end())
        return;

    // The only acknowledgement the protocol has (section 25). A report that
    // names no audible position is a device that has not reached the scheduled
    // frame yet, which is exactly what a start that has not happened looks
    // like -- so it is not one.
    if (!report.hasTimelineNsAudible)
        return;
    if (found->second.state != RoomStartState::Scheduled)
        return;

    found->second.state = RoomStartState::Playing;
    found->second.detail.clear();
}

// ---------------------------------------------------------------------------

bool StartScheduler::schedulable(const Room& room) const
{
    if (!room.member || !room.streamReady)
        return false;
    if (room.clockExchanges < settings_.minClockExchanges)
        return false;

    // Buffered audio is not enough on its own, and neither is a duration with
    // no positions behind it: the client omits both until its first packet has
    // arrived, and a start it cannot place in its own ring is refused there
    // rather than here.
    if (!room.buffer.hasContent)
        return false;
    return room.buffer.bufferedDurationNs >= settings_.minimumStartBufferNs;
}

TimelineNs StartScheduler::playheadAt(uint64_t nowNs) const
{
    // A line that is not running stands where it stands: the position a pause
    // froze it at, the position a resume is forming its barrier around, or --
    // stopped -- the zero that means there is no line at all.
    if (transport_ != TransportState::Playing || nowNs <= startServerTimeNs_)
        return startTimelineNs_;

    // The zone's timeline advances at the rate of the server's own monotonic
    // clock. That is section 20's whole mapping from this end: the instant a
    // position is audible in every room is an instant on this clock.
    return TimelineNs{std::to_underlying(startTimelineNs_) + (nowNs - startServerTimeNs_)};
}

bool StartScheduler::placeOnLine(const Room& room, uint64_t nowNs, uint64_t& timelineNs,
                                 uint64_t& serverTimeNs) const
{
    if (!room.buffer.hasContent)
        return false;

    // Far enough ahead that the message has a chance to arrive, and never
    // earlier than the oldest position this room still holds -- a start it
    // has already dropped is one it will refuse. A room with an output latency
    // is started that much earlier than the line (phase 27), so the position
    // is that much further on for the start to be as far in the future.
    uint64_t wanted =
        std::to_underlying(playheadAt(nowNs)) + settings_.startDelayNs + room.latencyNs;
    wanted = std::max(wanted, room.buffer.playheadTimelineNs);

    if (wanted > room.buffer.bufferedUntilTimelineNs)
        return false; // the line has run past everything this room holds

    const uint64_t anchor = std::to_underlying(startTimelineNs_);
    if (wanted < anchor)
        return false; // before the zone started; there is no instant for it

    const uint64_t lineNs = startServerTimeNs_ + (wanted - anchor);
    if (lineNs <= room.latencyNs)
        return false;

    timelineNs = wanted;
    serverTimeNs = lineNs - room.latencyNs;
    return serverTimeNs > nowNs;
}

bool StartScheduler::restartLineAt(const ClientId& client, Room& room, uint64_t nowNs,
                                   uint64_t& timelineNs, uint64_t& serverTimeNs)
{
    // A room alone on the line is the line: nobody is heard against it, so
    // when the line has run past it -- a start it confirmed too late, after a
    // resume, on a card whose clock nothing corrects -- the line is moved to
    // the room rather than the room's stream to the line. Moving the stream
    // is a cut and a refill; this is a START_AT at the audio it already holds.
    for (const auto& entry : rooms_)
    {
        if (entry.first != client && onTheLine(entry.second))
            return false;
    }
    if (!room.buffer.hasContent)
        return false;

    timelineNs = room.buffer.playheadTimelineNs;
    serverTimeNs = nowNs + settings_.startDelayNs;
    startTimelineNs_ = TimelineNs{timelineNs};
    startServerTimeNs_ = serverTimeNs + room.latencyNs;

    // A new line, so a new first start: the attempts made on the old one say
    // nothing about this one, and counting them would leave the zone's only
    // room behind -- silence, where the rejoin this replaces would have
    // played on.
    room.attempts = 0;
    ++linesRestarted_;

    MULTIROOM_INFO("line restarted at timeline {} ns at server time {} ns: {} is alone on it and "
             "the line had run past it",
             timelineNs, startServerTimeNs_, client.str());
    return true;
}

RoomCommand StartScheduler::startFor(const ClientId& client, uint64_t timelineNs,
                                     uint64_t serverTimeNs)
{
    RoomCommand command;
    command.kind = RoomCommandKind::Start;
    command.client = client;
    command.start.timelineNs = timelineNs;
    command.start.serverTimeNs = serverTimeNs;
    return command;
}

bool StartScheduler::onTheLine(const Room& room)
{
    return room.state == RoomStartState::Scheduled || room.state == RoomStartState::Playing ||
           room.state == RoomStartState::Paused;
}

void StartScheduler::leaveBehind(Room& room, std::string detail)
{
    if (room.state == RoomStartState::LeftBehind)
        return;
    room.state = RoomStartState::LeftBehind;
    room.detail = std::move(detail);
    ++leftBehind_;
}

// ---------------------------------------------------------------------------

bool StartScheduler::resolveBarrier(uint64_t nowNs, std::vector<RoomCommand>& commands)
{
    // A resume fixed its position before the barrier formed (section 37), and
    // a first start picks one from what the rooms hold. That is the whole
    // difference between the two, and it is what makes a resume land on the
    // frame the pause named rather than near it.
    const bool fixed = transport_ == TransportState::Starting;

    std::vector<const ClientId*> ready;
    bool pending = false;

    for (auto& entry : rooms_)
    {
        Room& room = entry.second;
        if (schedulable(room))
        {
            room.state = RoomStartState::Ready;
            ready.push_back(&entry.first);
            continue;
        }

        // A room still inside its window holds the barrier; one past it does
        // not, and is neither failed nor forgotten -- it joins the line once
        // it becomes schedulable, which is what the late join below is.
        if (nowNs < room.deadlineNs)
            pending = true;
    }

    if (ready.empty())
    {
        if (fixed)
            rejoinShortRooms(nowNs);
        return false;
    }
    if (pending)
        return false;

    // The earliest position every ready room holds. Each of them reports the
    // oldest position still in its own ring, and those differ: a room admitted
    // later began its stream later. Taking the greatest is the one position
    // all of them can make audible.
    uint64_t position = std::to_underlying(startTimelineNs_);
    if (!fixed)
    {
        position = 0;
        for (const ClientId* client : ready)
            position = std::max(position, rooms_[*client].buffer.playheadTimelineNs);
    }

    // The room with the most output latency is started earliest (phase 27),
    // so the line's instant is put that much later: every START_AT of this
    // barrier is still a whole start delay in the future.
    uint64_t latencyNs = 0;
    for (const ClientId* client : ready)
        latencyNs = std::max(latencyNs, rooms_[*client].latencyNs);

    transport_ = TransportState::Playing;
    startTimelineNs_ = TimelineNs{position};
    startServerTimeNs_ = nowNs + settings_.startDelayNs + latencyNs;

    for (const ClientId* client : ready)
    {
        Room& room = rooms_[*client];
        if (room.buffer.bufferedUntilTimelineNs < position ||
            room.buffer.playheadTimelineNs > position)
        {
            // It met both conditions but not at this position: it is behind
            // the rooms that set it, or -- after a resume, where the position
            // is not derived from the rooms at all -- ahead of it. Either way
            // it is joined as a latecomer instead.
            room.state = RoomStartState::Waiting;
            continue;
        }

        const uint64_t serverTimeNs = startServerTimeNs_ - room.latencyNs;
        commands.push_back(startFor(*client, position, serverTimeNs));
        room.state = RoomStartState::Scheduled;
        room.startServerTimeNs = serverTimeNs;
        room.attempts = 1;
        ++startsSent_;
    }

    MULTIROOM_INFO("start barrier resolved: timeline {} ns at server time {} ns, {} room(s)", position,
             startServerTimeNs_, commands.size());
    return true;
}

void StartScheduler::rejoinShortRooms(uint64_t nowNs)
{
    // Section 24 step 10, for the barrier nobody is ready for. A line waiting
    // at a fixed position builds each room no further than a target past it
    // (phase 39), so a room that lost the front of that -- packets written
    // into an audio connection its client had already let go -- holds less
    // than the minimum and is never sent more: it would wait here for good,
    // and the zone with it. Past its window, it is moved instead, which sends
    // it the whole of what the line allows again from the position it waits at.
    for (auto& entry : rooms_)
    {
        Room& room = entry.second;
        if (room.state != RoomStartState::Waiting || nowNs < room.deadlineNs)
            continue;
        if (!room.member || !room.streamReady || !room.buffer.hasContent ||
            room.clockExchanges < settings_.minClockExchanges ||
            room.buffer.bufferedDurationNs >= settings_.minimumStartBufferNs)
        {
            continue;
        }

        room.state = RoomStartState::Rejoining;
        room.detail = "holds " + std::to_string(room.buffer.bufferedDurationNs / 1000000ULL) +
                      " ms at the position the line waits at, short of the start minimum";
        // A fresh window for the stream the move sends: until it is over, a
        // room still filling is not taken for one that stopped short.
        room.deadlineNs = nowNs + settings_.readyTimeoutNs;
        rejoinWanted_.push_back(entry.first);
        ++rejoinsAsked_;
    }
}

std::vector<RoomCommand> StartScheduler::evaluate(uint64_t nowNs)
{
    std::vector<RoomCommand> commands;

    // A paused zone decides nothing. A room that becomes schedulable now has
    // nowhere to be put: the line it would be put on is standing still, and
    // starting it alone is the one thing section 37's "the same logical
    // position" rules out.
    if (transport_ == TransportState::Paused)
        return commands;

    // A room that was announced a format and never came back costs an entry
    // until its window closes, and nothing after it. Bounded, like every other
    // container here.
    for (auto it = rooms_.begin(); it != rooms_.end();)
    {
        if (!it->second.member && nowNs >= it->second.deadlineNs)
            it = rooms_.erase(it);
        else
            ++it;
    }

    if (transport_ != TransportState::Playing)
    {
        static_cast<void>(resolveBarrier(nowNs, commands));
        return commands;
    }

    for (auto& entry : rooms_)
    {
        Room& room = entry.second;

        if (room.state == RoomStartState::Scheduled)
        {
            // Section 20 gives the server no way to hear that a START_AT was
            // refused: the client says "no clock mapping yet" to itself. What
            // it does say is a PLAYBACK_REPORT with an audible position once
            // the start took, so silence past the instant that was named is
            // the signal to try again -- at a fresh position, because the pair
            // that was sent is now in the past.
            if (nowNs < room.startServerTimeNs + settings_.ackTimeoutNs)
                continue;

            if (room.attempts >= settings_.startAttempts)
            {
                leaveBehind(room, "no playback report after " +
                                      std::to_string(room.attempts) + " start(s)");
                continue;
            }
            room.state = RoomStartState::Waiting;
        }

        if (room.state != RoomStartState::Waiting && room.state != RoomStartState::Ready)
            continue;
        if (!schedulable(room))
            continue;

        uint64_t timelineNs = 0;
        uint64_t serverTimeNs = 0;
        if (!placeOnLine(room, nowNs, timelineNs, serverTimeNs) &&
            !restartLineAt(entry.first, room, nowNs, timelineNs, serverTimeNs))
        {
            // Everything this room holds is behind what is audible now, so
            // there is no position a START_AT could name: it has to be moved
            // before it can be placed, which is section 32's hard resync and
            // section 43's rejoin arriving as the same thing. This class moves
            // nothing -- it asks, and stops considering the room until it is
            // told where the move put it.
            room.state = RoomStartState::Rejoining;
            room.detail = "the timeline has run past the audio this room holds";
            rejoinWanted_.push_back(entry.first);
            ++rejoinsAsked_;
            continue;
        }

        commands.push_back(startFor(entry.first, timelineNs, serverTimeNs));
        room.state = RoomStartState::Scheduled;
        room.startServerTimeNs = serverTimeNs;
        ++room.attempts;
        ++startsSent_;
    }

    return commands;
}

// ---------------------------------------------------------------------------

std::vector<RoomCommand> StartScheduler::pause(uint64_t nowNs, TimelineNs& position)
{
    std::vector<RoomCommand> commands;
    position = startTimelineNs_;
    if (transport_ != TransportState::Playing)
        return commands;

    // One position for the whole house (section 37), a start delay ahead of
    // what is audible now for the same reason a START_AT is: the message has a
    // LAN to cross and every room has to place it on its own frame axis before
    // it comes round.
    const uint64_t frozen = std::to_underlying(playheadAt(nowNs)) + settings_.startDelayNs;

    for (auto& entry : rooms_)
    {
        Room& room = entry.second;
        if (!onTheLine(room))
            continue;

        RoomCommand command;
        command.kind = RoomCommandKind::Pause;
        command.client = entry.first;
        command.at.timelineNs = frozen;
        command.at.serverTimeNs = nowNs + settings_.startDelayNs;
        commands.push_back(command);

        room.state = RoomStartState::Paused;
        room.detail.clear();
        room.attempts = 0;
        room.startServerTimeNs = 0;
        ++pausesSent_;
    }

    transport_ = TransportState::Paused;
    startTimelineNs_ = TimelineNs{frozen};
    position = startTimelineNs_;

    MULTIROOM_INFO("paused at timeline {} ns, {} room(s)", frozen, commands.size());
    return commands;
}

void StartScheduler::resume()
{
    if (transport_ != TransportState::Paused)
        return;

    // The barrier again, around the position the pause named rather than
    // around whatever the rooms happen to hold. Every room that was paused is
    // put back in it; one whose device closed in the meantime holds the resume
    // exactly as it would have held the first start, and one that never
    // started is unaffected.
    for (auto& entry : rooms_)
    {
        Room& room = entry.second;
        if (room.state != RoomStartState::Paused)
            continue;
        room.state = RoomStartState::Waiting;
        room.detail.clear();
        room.attempts = 0;
        room.startServerTimeNs = 0;
    }

    transport_ = TransportState::Starting;
    startServerTimeNs_ = 0;

    MULTIROOM_INFO("resuming from timeline {} ns", std::to_underlying(startTimelineNs_));
}

std::vector<RoomCommand> StartScheduler::stop(uint64_t nowNs, TimelineNs& position)
{
    std::vector<RoomCommand> commands;
    position = startTimelineNs_;
    if (transport_ == TransportState::Stopped)
        return commands;

    // A playing line is stopped a start delay ahead of what is audible, for
    // the reason a pause is; a line that is already standing still is stopped
    // where it stands, because there is nothing left to play out first.
    const bool playing = transport_ == TransportState::Playing;
    const uint64_t at =
        std::to_underlying(playheadAt(nowNs)) + (playing ? settings_.startDelayNs : 0);
    position = TimelineNs{at};

    for (auto& entry : rooms_)
    {
        Room& room = entry.second;
        if (onTheLine(room))
        {
            RoomCommand command;
            command.kind = RoomCommandKind::Stop;
            command.client = entry.first;
            command.at.timelineNs = at;
            command.at.serverTimeNs = nowNs + (playing ? settings_.startDelayNs : 0);
            commands.push_back(command);
            ++stopsSent_;
        }

        // Section 38 drops what every room holds, so what every room last said
        // about its buffer describes a timeline nobody is playing. A room is
        // not scheduled again until it has reported a new one, which is the
        // same rule a reopened device gets.
        room.buffer = matp::BufferStatus();
        room.state = room.member ? RoomStartState::Waiting : RoomStartState::Expecting;
        room.detail.clear();
        room.attempts = 0;
        room.startServerTimeNs = 0;
    }

    transport_ = TransportState::Stopped;
    startTimelineNs_ = TimelineNs{};
    startServerTimeNs_ = 0;

    MULTIROOM_INFO("stopped at timeline {} ns, {} room(s)", at, commands.size());
    return commands;
}

// ---------------------------------------------------------------------------

std::optional<RoomCommand> StartScheduler::pauseForTransition(const ClientId& client,
                                                              TimelineNs at)
{
    const auto found = rooms_.find(client);
    if (found == rooms_.end() || transport_ != TransportState::Playing)
        return std::nullopt;

    Room& room = found->second;
    if (room.state != RoomStartState::Scheduled && room.state != RoomStartState::Playing)
        return std::nullopt;

    // The instant the line reaches the boundary, which is arithmetic on the
    // anchor like every other instant here. A boundary the line has already
    // passed is paused where it is: the room has nothing past it to play.
    const uint64_t position = std::to_underlying(at);
    const uint64_t anchor = std::to_underlying(startTimelineNs_);

    RoomCommand command;
    command.kind = RoomCommandKind::Pause;
    command.client = client;
    command.at.timelineNs = position;
    command.at.serverTimeNs =
        startServerTimeNs_ + (position > anchor ? position - anchor : 0);

    room.state = RoomStartState::Paused;
    room.detail = "format transition at " + std::to_string(position) + " ns";
    room.attempts = 0;
    room.startServerTimeNs = 0;
    ++pausesSent_;
    return command;
}

bool StartScheduler::freezeForTransition(TimelineNs at, uint64_t nowNs)
{
    if (transport_ != TransportState::Playing)
        return false;
    for (const auto& entry : rooms_)
    {
        if (entry.second.state == RoomStartState::Scheduled ||
            entry.second.state == RoomStartState::Playing)
        {
            return false;
        }
    }

    // A resume's barrier, at a position nobody paused the house for: every
    // room still on the line stopped at `at` of its own accord.
    for (auto& entry : rooms_)
    {
        Room& room = entry.second;
        if (room.state != RoomStartState::Paused)
            continue;

        // What it last said about its buffer was about the device it is
        // reopening, and its window is the ready timeout from now -- not from
        // whenever it first connected.
        room.state = RoomStartState::Waiting;
        room.buffer = matp::BufferStatus();
        room.deadlineNs = nowNs + settings_.readyTimeoutNs;
        room.attempts = 0;
        room.startServerTimeNs = 0;
    }

    transport_ = TransportState::Starting;
    startTimelineNs_ = at;
    startServerTimeNs_ = 0;

    MULTIROOM_INFO("format transition: the line waits at timeline {} ns", std::to_underlying(at));
    return true;
}

bool StartScheduler::alignLineTo(const ClientId& client, int64_t offsetNs)
{
    if (transport_ != TransportState::Playing)
        return false;
    const auto found = rooms_.find(client);
    if (found == rooms_.end() || found->second.state != RoomStartState::Playing)
        return false;
    for (const auto& entry : rooms_)
    {
        if (entry.first != client && onTheLine(entry.second))
            return false;
    }

    // The anchor's position and not its instant: an instant moved into the
    // future would stand the line still until it got there.
    const uint64_t anchor = std::to_underlying(startTimelineNs_);
    const uint64_t magnitude = offsetNs < 0 ? static_cast<uint64_t>(-offsetNs)
                                            : static_cast<uint64_t>(offsetNs);
    if (offsetNs < 0 && anchor < magnitude)
        return false;
    startTimelineNs_ = TimelineNs{offsetNs < 0 ? anchor - magnitude : anchor + magnitude};

    MULTIROOM_INFO("line moved {} us to {}, which is alone on it", offsetNs / 1000, client.str());
    return true;
}

// ---------------------------------------------------------------------------

RoomStartState StartScheduler::roomState(const ClientId& client) const
{
    const auto found = rooms_.find(client);
    return found == rooms_.end() ? RoomStartState::Expecting : found->second.state;
}

std::string StartScheduler::roomDetail(const ClientId& client) const
{
    const auto found = rooms_.find(client);
    return found == rooms_.end() ? std::string() : found->second.detail;
}

void StartScheduler::explain(const ClientId& client, std::string detail)
{
    if (const auto found = rooms_.find(client); found != rooms_.end())
        found->second.detail = std::move(detail);
}

} // namespace multiroom
