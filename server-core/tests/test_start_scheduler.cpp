// Section 20, without a zone, a socket or a second passing on a real clock.
//
// The scheduler holds no decoder, no output and no connection -- every input
// is a call and the time is a parameter -- so the barrier, the five second
// readiness timeout, the acknowledgement and the late join are all driven
// here by naming instants. Nothing in this file sleeps, and nothing in it
// needs a room to exist.

#include "TestSupport.h"

#include "zone/StartScheduler.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace multiroom;

namespace {

constexpr uint64_t kMs = 1000000ULL;
constexpr uint64_t kSecond = kNanosecondsPerSecond;

// Far enough into a monotonic clock that nothing here works by accident from
// an origin of zero.
constexpr uint64_t kT0 = 100ULL * kSecond;

StartSchedulerSettings defaults()
{
    StartSchedulerSettings settings;
    settings.minimumStartBufferNs = 1000 * kMs;
    settings.readyTimeoutNs = 5 * kSecond;
    settings.startDelayNs = 1 * kSecond;
    settings.ackTimeoutNs = 1 * kSecond;
    settings.startAttempts = 3;
    settings.minClockExchanges = 4;
    return settings;
}

// What a room reports once it holds `bufferedNs` beginning at `fromNs`.
matp::BufferStatus buffered(uint64_t fromNs, uint64_t bufferedNs)
{
    matp::BufferStatus status;
    status.hasContent = true;
    status.playheadTimelineNs = fromNs;
    status.bufferedUntilTimelineNs = fromNs + bufferedNs;
    status.bufferedDurationNs = bufferedNs;
    return status;
}

matp::PlaybackReport audibleAt(uint64_t timelineNs)
{
    matp::PlaybackReport report;
    report.clockQuality = AudioClockQuality::Hardware;
    report.hasTimelineNsAudible = true;
    report.timelineNsAudible = timelineNs;
    return report;
}

// A room that has done everything section 20 asks of it, in one call.
void becomeReady(StartScheduler& scheduler, const ClientId& client, TimelineNs origin,
                 uint64_t nowNs, uint64_t bufferedNs = 1500 * kMs, uint64_t exchanges = 20)
{
    scheduler.expect(client, nowNs);
    scheduler.join(client, origin, nowNs);
    scheduler.onBufferStatus(client, buffered(std::to_underlying(origin), bufferedNs), exchanges);
}

const RoomCommand* commandFor(const std::vector<RoomCommand>& commands, const char* client)
{
    const auto found = std::find_if(commands.begin(), commands.end(),
                                    [client](const RoomCommand& command)
                                    { return command.client.str() == client; });
    return found == commands.end() ? nullptr : &*found;
}

} // namespace

// --- the barrier -------------------------------------------------------------

MULTIROOM_TEST(every_ready_room_is_started_at_the_same_position_and_instant)
{
    StartScheduler scheduler(defaults());

    const ClientId kitchen("kitchen");
    const ClientId living("living");

    becomeReady(scheduler, kitchen, TimelineNs{0}, kT0);
    becomeReady(scheduler, living, TimelineNs{0}, kT0);

    const std::vector<RoomCommand> commands = scheduler.evaluate(kT0);
    CHECK_EQ(commands.size(), size_t{2});

    // Section 20's whole point: one position, one instant, whatever rate or
    // format each room is playing it at.
    const RoomCommand* first = commandFor(commands, "kitchen");
    const RoomCommand* second = commandFor(commands, "living");
    CHECK(first != nullptr);
    CHECK(second != nullptr);
    CHECK_EQ(first->start.timelineNs, second->start.timelineNs);
    CHECK_EQ(first->start.serverTimeNs, second->start.serverTimeNs);
    CHECK_EQ(first->start.serverTimeNs, kT0 + 1 * kSecond);
    CHECK_EQ(first->start.timelineNs, uint64_t{0});

    CHECK(scheduler.started());
    CHECK_EQ(std::to_underlying(scheduler.startTimelineNs()), uint64_t{0});
    CHECK_EQ(scheduler.startServerTimeNs(), kT0 + 1 * kSecond);
    CHECK_EQ(scheduler.startsSent(), uint64_t{2});
}

MULTIROOM_TEST(a_room_that_was_announced_a_format_holds_the_barrier_until_the_timeout)
{
    StartScheduler scheduler(defaults());

    const ClientId ready("ready");
    const ClientId slow("slow");

    becomeReady(scheduler, ready, TimelineNs{0}, kT0);

    // Its device is still opening: SESSION_CONFIG named a format for it and
    // nothing has come back.
    scheduler.expect(slow, kT0);

    CHECK(scheduler.evaluate(kT0).empty());
    CHECK(scheduler.evaluate(kT0 + 4 * kSecond).empty());
    CHECK(!scheduler.started());
    CHECK(scheduler.roomState(ready) == RoomStartState::Ready);
    CHECK(scheduler.roomState(slow) == RoomStartState::Expecting);

    // One room with a slow driver must never keep the others silent.
    const std::vector<RoomCommand> commands = scheduler.evaluate(kT0 + 5 * kSecond);
    CHECK_EQ(commands.size(), size_t{1});
    CHECK_EQ(commands.front().client.str(), std::string("ready"));
    CHECK(scheduler.started());

    // And the room that never answered is not remembered at all: its window
    // closed, and nothing here grows without a bound.
    CHECK_EQ(scheduler.roomCount(), size_t{1});
}

MULTIROOM_TEST(a_room_that_never_becomes_ready_never_delays_anything)
{
    StartScheduler scheduler(defaults());

    const ClientId ready("ready");
    const ClientId deaf("deaf");

    becomeReady(scheduler, ready, TimelineNs{0}, kT0);

    // A device that granted a stream and then reported a buffer that never
    // fills: a room that is there and will not be ready.
    scheduler.expect(deaf, kT0);
    scheduler.join(deaf, TimelineNs{0}, kT0);
    scheduler.onBufferStatus(deaf, buffered(0, 10 * kMs), 20);

    CHECK(scheduler.evaluate(kT0 + 1 * kSecond).empty());

    const std::vector<RoomCommand> commands = scheduler.evaluate(kT0 + 5 * kSecond);
    CHECK_EQ(commands.size(), size_t{1});
    CHECK_EQ(commands.front().client.str(), std::string("ready"));
    CHECK(scheduler.roomState(deaf) == RoomStartState::Waiting);
}

MULTIROOM_TEST(a_full_buffer_is_not_enough_without_a_device)
{
    StartScheduler scheduler(defaults());

    const ClientId room("room");
    scheduler.expect(room, kT0);
    scheduler.join(room, TimelineNs{0}, kT0);

    // Section 20's first condition, taken away: the client holds a full ring
    // and its device is not open. A server that only looked at the buffer
    // would start it.
    scheduler.onStreamReady(room, false);
    scheduler.onBufferStatus(room, buffered(0, 3 * kSecond), 20);

    CHECK(scheduler.evaluate(kT0 + 10 * kSecond).empty());
    CHECK(!scheduler.started());

    scheduler.onStreamReady(room, true);
    scheduler.onBufferStatus(room, buffered(0, 3 * kSecond), 20);
    CHECK_EQ(scheduler.evaluate(kT0 + 10 * kSecond).size(), size_t{1});
}

MULTIROOM_TEST(a_room_with_no_clock_mapping_is_not_started)
{
    StartScheduler scheduler(defaults());

    const ClientId room("room");
    scheduler.expect(room, kT0);
    scheduler.join(room, TimelineNs{0}, kT0);

    // Three completed exchanges is one short of what the client's own model
    // needs before it will map a server instant at all -- and it answers a
    // START_AT it cannot map by logging to itself, so the server would never
    // learn that the start did not take.
    scheduler.onBufferStatus(room, buffered(0, 1500 * kMs), 3);
    CHECK(scheduler.evaluate(kT0 + 10 * kSecond).empty());

    scheduler.onBufferStatus(room, buffered(0, 1500 * kMs), 4);
    CHECK_EQ(scheduler.evaluate(kT0 + 10 * kSecond).size(), size_t{1});
}

MULTIROOM_TEST(an_empty_buffer_status_is_not_a_position)
{
    StartScheduler scheduler(defaults());

    const ClientId room("room");
    scheduler.expect(room, kT0);
    scheduler.join(room, TimelineNs{0}, kT0);

    // The client omits both positions until its first packet has arrived, so
    // a duration with no positions behind it is a room with no audio rather
    // than a room holding some at timeline zero.
    matp::BufferStatus empty;
    empty.bufferedDurationNs = 2 * kSecond;
    scheduler.onBufferStatus(room, empty, 20);

    CHECK(scheduler.evaluate(kT0 + 10 * kSecond).empty());
}

MULTIROOM_TEST(the_start_position_is_the_earliest_one_every_room_holds)
{
    StartScheduler scheduler(defaults());

    const ClientId early("early");
    const ClientId late("late");

    // Two rooms whose streams begin at different positions, because the second
    // one was admitted 400 ms of timeline later. Only the later of the two
    // beginnings is a position both of them can make audible.
    becomeReady(scheduler, early, TimelineNs{0}, kT0, 2 * kSecond);
    becomeReady(scheduler, late, TimelineNs{400 * kMs}, kT0, 1500 * kMs);

    const std::vector<RoomCommand> commands = scheduler.evaluate(kT0);
    CHECK_EQ(commands.size(), size_t{2});
    CHECK_EQ(commands.front().start.timelineNs, uint64_t{400 * kMs});
    CHECK_EQ(commands.back().start.timelineNs, uint64_t{400 * kMs});
}

// --- the acknowledgement ------------------------------------------------------

MULTIROOM_TEST(a_start_that_was_not_acknowledged_is_sent_again_at_a_fresh_position)
{
    StartScheduler scheduler(defaults());

    const ClientId room("room");
    becomeReady(scheduler, room, TimelineNs{0}, kT0, 3 * kSecond);

    const std::vector<RoomCommand> first = scheduler.evaluate(kT0);
    CHECK_EQ(first.size(), size_t{1});
    CHECK(scheduler.roomState(room) == RoomStartState::Scheduled);

    // Nothing is due until the instant it named has come and the
    // acknowledgement window after it has passed.
    CHECK(scheduler.evaluate(kT0 + 1500 * kMs).empty());

    const uint64_t retryAt = kT0 + 2100 * kMs;
    scheduler.onBufferStatus(room, buffered(0, 3 * kSecond), 20);
    const std::vector<RoomCommand> second = scheduler.evaluate(retryAt);
    CHECK_EQ(second.size(), size_t{1});

    // The pair that was sent is in the past now, so the same one again would
    // be a start nobody could act on: it is a later position on the same line.
    CHECK(second.front().start.timelineNs > first.front().start.timelineNs);
    CHECK(second.front().start.serverTimeNs > retryAt);

    // And the line itself did not move.
    CHECK_EQ(second.front().start.serverTimeNs - scheduler.startServerTimeNs(),
             second.front().start.timelineNs - std::to_underlying(scheduler.startTimelineNs()));
}

MULTIROOM_TEST(a_playback_report_naming_an_audible_position_ends_the_retries)
{
    StartScheduler scheduler(defaults());

    const ClientId room("room");
    becomeReady(scheduler, room, TimelineNs{0}, kT0, 3 * kSecond);
    CHECK_EQ(scheduler.evaluate(kT0).size(), size_t{1});

    // A report that names no audible position is a device that has not reached
    // the scheduled frame yet, which is what a start that has not happened
    // looks like -- so it is not an acknowledgement.
    matp::PlaybackReport silent;
    silent.clockQuality = AudioClockQuality::Hardware;
    scheduler.onPlaybackReport(room, silent);
    CHECK(scheduler.roomState(room) == RoomStartState::Scheduled);

    scheduler.onPlaybackReport(room, audibleAt(std::to_underlying(scheduler.startTimelineNs())));
    CHECK(scheduler.roomState(room) == RoomStartState::Playing);

    // Playing rooms are never re-offered a start; section 32's resync is what
    // moves one that is already running, and it is phase 16's.
    scheduler.onBufferStatus(room, buffered(0, 3 * kSecond), 20);
    CHECK(scheduler.evaluate(kT0 + 10 * kSecond).empty());
    CHECK_EQ(scheduler.startsSent(), uint64_t{1});
}

MULTIROOM_TEST(a_room_that_answers_no_start_is_left_behind_rather_than_retried_forever)
{
    StartScheduler scheduler(defaults());

    const ClientId room("room");
    becomeReady(scheduler, room, TimelineNs{0}, kT0, 30 * kSecond);

    uint64_t nowNs = kT0;
    for (uint32_t attempt = 0; attempt < 3; ++attempt)
    {
        scheduler.onBufferStatus(room, buffered(0, 30 * kSecond), 20);
        CHECK_EQ(scheduler.evaluate(nowNs).size(), size_t{1});
        nowNs += 3 * kSecond;
    }

    CHECK_EQ(scheduler.startsSent(), uint64_t{3});
    scheduler.onBufferStatus(room, buffered(0, 30 * kSecond), 20);
    CHECK(scheduler.evaluate(nowNs).empty());
    CHECK(scheduler.roomState(room) == RoomStartState::LeftBehind);
    CHECK_EQ(scheduler.leftBehindCount(), uint64_t{1});
    CHECK(!scheduler.roomDetail(room).empty());
}

// --- the line, once it is running ---------------------------------------------

MULTIROOM_TEST(the_playhead_runs_at_the_rate_of_the_servers_own_clock)
{
    StartScheduler scheduler(defaults());

    const ClientId room("room");
    becomeReady(scheduler, room, TimelineNs{5 * kSecond}, kT0, 3 * kSecond);
    CHECK_EQ(scheduler.evaluate(kT0).size(), size_t{1});

    // Before the instant it named, what is audible is still where the start
    // begins -- nothing has been heard yet.
    CHECK_EQ(std::to_underlying(scheduler.playheadAt(kT0)), uint64_t{5 * kSecond});
    CHECK_EQ(std::to_underlying(scheduler.playheadAt(kT0 + 500 * kMs)), uint64_t{5 * kSecond});

    // And afterwards it is the start plus the time since it.
    CHECK_EQ(std::to_underlying(scheduler.playheadAt(kT0 + 1 * kSecond)), uint64_t{5 * kSecond});
    CHECK_EQ(std::to_underlying(scheduler.playheadAt(kT0 + 3 * kSecond)), uint64_t{7 * kSecond});
    CHECK_EQ(std::to_underlying(scheduler.playheadAt(kT0 + 3601 * kSecond)),
             uint64_t{3605 * kSecond});
}

MULTIROOM_TEST(a_late_joiner_is_placed_on_the_line_without_the_playing_rooms_being_held)
{
    StartScheduler scheduler(defaults());

    const ClientId first("first");
    becomeReady(scheduler, first, TimelineNs{0}, kT0, 3 * kSecond);
    CHECK_EQ(scheduler.evaluate(kT0).size(), size_t{1});
    scheduler.onPlaybackReport(first, audibleAt(0));

    // Ten seconds later a second room arrives and prebuffers audio that has
    // not been heard yet: its stream begins two seconds ahead of the playhead.
    const uint64_t joinAt = kT0 + 11 * kSecond;
    const uint64_t playhead = std::to_underlying(scheduler.playheadAt(joinAt));
    CHECK_EQ(playhead, uint64_t{10 * kSecond});

    const ClientId second("second");
    becomeReady(scheduler, second, TimelineNs{playhead + 2 * kSecond}, joinAt, 1500 * kMs);

    const std::vector<RoomCommand> commands = scheduler.evaluate(joinAt);
    CHECK_EQ(commands.size(), size_t{1});
    CHECK_EQ(commands.front().client.str(), std::string("second"));

    // It is started at its own first buffered position, at the instant the
    // line says that position becomes audible -- which is arithmetic, not a
    // negotiation, and is why the room that is playing is not touched.
    CHECK_EQ(commands.front().start.timelineNs, playhead + 2 * kSecond);
    CHECK_EQ(commands.front().start.serverTimeNs, joinAt + 2 * kSecond);
    CHECK(scheduler.roomState(first) == RoomStartState::Playing);
}

MULTIROOM_TEST(a_room_the_timeline_has_run_past_is_asked_to_rejoin)
{
    StartScheduler scheduler(defaults());

    const ClientId first("first");
    becomeReady(scheduler, first, TimelineNs{0}, kT0, 3 * kSecond);
    CHECK_EQ(scheduler.evaluate(kT0).size(), size_t{1});
    scheduler.onPlaybackReport(first, audibleAt(0));

    // A room that was admitted before the start, missed the barrier, and holds
    // only the first second and a half of a timeline that is now a minute in.
    const ClientId stranded("stranded");
    scheduler.expect(stranded, kT0);
    scheduler.join(stranded, TimelineNs{0}, kT0);
    scheduler.onBufferStatus(stranded, buffered(0, 1500 * kMs), 20);

    // There is no position a START_AT could name, so this class asks for the
    // one thing that would make one exist -- the stream moved (phase 16) --
    // and sends nothing at all in the meantime.
    const uint64_t muchLater = kT0 + 61 * kSecond;
    CHECK(scheduler.evaluate(muchLater).empty());
    CHECK(scheduler.roomState(stranded) == RoomStartState::Rejoining);
    CHECK_EQ(scheduler.roomDetail(stranded),
             std::string("the timeline has run past the audio this room holds"));
    CHECK_EQ(scheduler.rejoinsAsked(), 1ULL);

    const std::vector<ClientId> wanted = scheduler.takeRejoins();
    CHECK_EQ(wanted.size(), size_t{1});
    CHECK_EQ(wanted.front().str(), std::string("stranded"));

    // Drained, and not asked for again while the caller is working on it: a
    // room asked for twice would be moved twice.
    CHECK(scheduler.takeRejoins().empty());
    CHECK(scheduler.evaluate(muchLater + kSecond).empty());
    CHECK(scheduler.takeRejoins().empty());
    CHECK_EQ(scheduler.rejoinsAsked(), 1ULL);

    // Moved, and placed on the line from what it reports about the audio it
    // holds now -- which is the late join of section 44, reached by a room
    // that had already been admitted.
    const uint64_t moved = muchLater + 2 * kSecond;
    const uint64_t origin = std::to_underlying(scheduler.playheadAt(moved)) + 2 * kSecond;
    scheduler.rejoin(stranded, TimelineNs{origin});
    CHECK(scheduler.roomState(stranded) == RoomStartState::Waiting);
    CHECK_EQ(scheduler.roomDetail(stranded), std::string());

    // What it said about the stream it used to have is not an answer about
    // the one it has now, so nothing is scheduled until it reports again.
    CHECK(scheduler.evaluate(moved).empty());
    scheduler.onBufferStatus(stranded, buffered(origin, origin + 1500 * kMs), 20);

    const std::vector<RoomCommand> commands = scheduler.evaluate(moved);
    CHECK_EQ(commands.size(), size_t{1});
    CHECK_EQ(commands.front().client.str(), std::string("stranded"));
    CHECK_EQ(commands.front().start.timelineNs, origin);

    // And the room that is playing heard nothing about any of it.
    CHECK(scheduler.roomState(first) == RoomStartState::Playing);
    CHECK_EQ(scheduler.leftBehindCount(), 0ULL);
}

MULTIROOM_TEST(a_room_left_short_at_a_format_transition_is_asked_to_rejoin_after_its_window)
{
    StartScheduler scheduler(defaults());

    // The zone's only room, playing.
    const ClientId alone("alone");
    becomeReady(scheduler, alone, TimelineNs{0}, kT0, 3 * kSecond);
    CHECK_EQ(scheduler.evaluate(kT0).size(), size_t{1});
    scheduler.onPlaybackReport(alone, audibleAt(0));

    // Section 24 at ten seconds: paused on the boundary, the line frozen
    // there, and the reopened device carried on from it.
    const uint64_t boundary = 10 * kSecond;
    const uint64_t frozenAt = kT0 + 11 * kSecond;
    CHECK(scheduler.pauseForTransition(alone, TimelineNs{boundary}).has_value());
    CHECK(scheduler.freezeForTransition(TimelineNs{boundary}, frozenAt));
    scheduler.rejoin(alone, TimelineNs{boundary});

    // The first 540 ms of the new stream went into the connection the client
    // had already let go, so it holds 980 ms from just after the boundary --
    // short of the minimum, and the line's ceiling sends it nothing more.
    scheduler.onBufferStatus(alone, buffered(boundary + 540 * kMs, 980 * kMs), 20);

    // Inside its window it is a room still filling, and nothing happens.
    CHECK(scheduler.evaluate(frozenAt + 4 * kSecond).empty());
    CHECK(scheduler.takeRejoins().empty());
    CHECK(scheduler.roomState(alone) == RoomStartState::Waiting);

    // Past it, the zone would wait for good: so the room is moved instead.
    const uint64_t late = frozenAt + 6 * kSecond;
    CHECK(scheduler.evaluate(late).empty());
    CHECK(scheduler.roomState(alone) == RoomStartState::Rejoining);
    CHECK_EQ(scheduler.rejoinsAsked(), 1ULL);
    const std::vector<ClientId> wanted = scheduler.takeRejoins();
    CHECK_EQ(wanted.size(), size_t{1});
    CHECK_EQ(wanted.front().str(), std::string("alone"));

    // Asked once, however many passes the move takes.
    CHECK(scheduler.evaluate(late + 1 * kSecond).empty());
    CHECK(scheduler.takeRejoins().empty());

    // Moved back to the boundary, it is sent the whole of the line's allowance
    // again, and the barrier starts it exactly there.
    scheduler.rejoin(alone, TimelineNs{boundary});
    scheduler.onBufferStatus(alone, buffered(boundary, 1520 * kMs), 20);
    const std::vector<RoomCommand> commands = scheduler.evaluate(late + 2 * kSecond);
    CHECK_EQ(commands.size(), size_t{1});
    CHECK_EQ(commands.front().start.timelineNs, boundary);
    CHECK(scheduler.transport() == TransportState::Playing);
}

MULTIROOM_TEST(a_room_alone_on_the_line_moves_the_line_rather_than_being_asked_to_rejoin)
{
    StartScheduler scheduler(defaults());

    // The zone's only room, started, unconfirmed, and holding 1.5 s from 0 --
    // a room whose confirmation comes later than the acknowledgement window
    // after a resume does exactly this.
    const ClientId alone("alone");
    becomeReady(scheduler, alone, TimelineNs{0}, kT0);
    CHECK_EQ(scheduler.evaluate(kT0).size(), size_t{1});

    // Its retry finds the line run past everything it holds. With nobody else
    // to be heard against, the line starts again at the audio the room has,
    // and no stream is moved.
    const uint64_t later = kT0 + 5 * kSecond;
    scheduler.onBufferStatus(alone, buffered(0, 1500 * kMs), 20);
    const std::vector<RoomCommand> commands = scheduler.evaluate(later);
    CHECK_EQ(commands.size(), size_t{1});
    CHECK_EQ(commands.front().start.timelineNs, 0ULL);
    CHECK_EQ(commands.front().start.serverTimeNs, later + 1 * kSecond);
    CHECK(scheduler.roomState(alone) == RoomStartState::Scheduled);
    CHECK(scheduler.takeRejoins().empty());
    CHECK_EQ(scheduler.rejoinsAsked(), 0ULL);
    CHECK_EQ(scheduler.linesRestarted(), 1ULL);

    // The line is the room's now: what it plays is what the zone says is
    // audible.
    CHECK_EQ(std::to_underlying(scheduler.startTimelineNs()), 0ULL);
    CHECK_EQ(scheduler.startServerTimeNs(), later + 1 * kSecond);
    CHECK_EQ(std::to_underlying(scheduler.playheadAt(later + 1500 * kMs)), 500 * kMs);

    // A new line is a new first start: the attempts on the old one are not
    // counted against it, so the zone's only room is not left behind.
    for (int retry = 0; retry < 3; ++retry)
    {
        const uint64_t again = later + static_cast<uint64_t>(retry + 1) * 5 * kSecond;
        scheduler.onBufferStatus(alone, buffered(0, 1500 * kMs), 20);
        CHECK_EQ(scheduler.evaluate(again).size(), size_t{1});
    }
    CHECK(scheduler.roomState(alone) == RoomStartState::Scheduled);
    CHECK_EQ(scheduler.leftBehindCount(), 0ULL);
}

MULTIROOM_TEST(a_device_that_stops_being_ready_is_dropped_from_scheduling_and_kept)
{
    StartScheduler scheduler(defaults());

    const ClientId room("room");
    becomeReady(scheduler, room, TimelineNs{0}, kT0, 3 * kSecond);
    CHECK_EQ(scheduler.evaluate(kT0).size(), size_t{1});
    scheduler.onPlaybackReport(room, audibleAt(0));
    CHECK(scheduler.roomState(room) == RoomStartState::Playing);

    // Section 20's table: dropped from scheduling, its state kept, and
    // rejoined when it reports ready again.
    scheduler.onStreamReady(room, false);
    CHECK(scheduler.roomState(room) == RoomStartState::Waiting);
    CHECK(scheduler.evaluate(kT0 + 5 * kSecond).empty());
    CHECK_EQ(scheduler.roomCount(), size_t{1});

    const uint64_t backAt = kT0 + 6 * kSecond;
    const uint64_t playhead = std::to_underlying(scheduler.playheadAt(backAt));
    scheduler.onStreamReady(room, true);
    scheduler.onBufferStatus(room, buffered(playhead + 500 * kMs, 1500 * kMs), 20);

    const std::vector<RoomCommand> commands = scheduler.evaluate(backAt);
    CHECK_EQ(commands.size(), size_t{1});
    CHECK_EQ(commands.front().start.timelineNs, playhead + 1 * kSecond);
    CHECK_EQ(commands.front().start.serverTimeNs, backAt + 1 * kSecond);
}

MULTIROOM_TEST(a_room_that_leaves_takes_nothing_with_it)
{
    StartScheduler scheduler(defaults());

    const ClientId staying("staying");
    const ClientId leaving("leaving");

    becomeReady(scheduler, staying, TimelineNs{0}, kT0, 3 * kSecond);
    scheduler.expect(leaving, kT0);

    // The barrier is held by the room that has not answered...
    CHECK(scheduler.evaluate(kT0).empty());

    // ...and released the moment that room's connection goes, rather than five
    // seconds later.
    scheduler.leave(leaving);
    CHECK_EQ(scheduler.evaluate(kT0).size(), size_t{1});
    CHECK_EQ(scheduler.roomCount(), size_t{1});
}

// --- phase 14: the transport, sections 37 and 38 -----------------------------

namespace {

// Two rooms, ready at position zero, started and acknowledged. Every transport
// test below begins from here, which is the only state a pause or a stop has
// anything to act on.
uint64_t startTwoRooms(StartScheduler& scheduler, const ClientId& kitchen,
                       const ClientId& living)
{
    becomeReady(scheduler, kitchen, TimelineNs{0}, kT0);
    becomeReady(scheduler, living, TimelineNs{0}, kT0);

    const std::vector<RoomCommand> started = scheduler.evaluate(kT0);
    CHECK_EQ(started.size(), size_t{2});

    scheduler.onPlaybackReport(kitchen, audibleAt(0));
    scheduler.onPlaybackReport(living, audibleAt(0));
    return scheduler.startServerTimeNs();
}

} // namespace

MULTIROOM_TEST(a_pause_names_one_position_for_the_house_and_freezes_the_playhead)
{
    StartScheduler scheduler(defaults());

    const ClientId kitchen("kitchen");
    const ClientId living("living");
    const uint64_t startedAt = startTwoRooms(scheduler, kitchen, living);

    // Three seconds of timeline have been heard.
    const uint64_t pauseAt = startedAt + 3 * kSecond;
    CHECK_EQ(std::to_underlying(scheduler.playheadAt(pauseAt)), 3 * kSecond);

    TimelineNs position{};
    const std::vector<RoomCommand> paused = scheduler.pause(pauseAt, position);

    // Section 37: the same logical position in every room, a start delay
    // ahead of what is audible, because the message has the same LAN to cross
    // that a START_AT has.
    CHECK_EQ(paused.size(), size_t{2});
    CHECK(paused.front().kind == RoomCommandKind::Pause);
    CHECK_EQ(std::to_underlying(position), 4 * kSecond);
    CHECK_EQ(paused.front().at.timelineNs, 4 * kSecond);
    CHECK_EQ(paused.back().at.timelineNs, 4 * kSecond);
    CHECK_EQ(paused.front().at.serverTimeNs, paused.back().at.serverTimeNs);
    CHECK_EQ(scheduler.pausesSent(), uint64_t{2});

    // The line stands still and stays a line: a paused zone still has a
    // position for a room joining it to be placed ahead of.
    CHECK(scheduler.transport() == TransportState::Paused);
    CHECK(scheduler.started());
    CHECK(scheduler.roomState(kitchen) == RoomStartState::Paused);
    CHECK_EQ(std::to_underlying(scheduler.playheadAt(pauseAt + 10 * kSecond)), 4 * kSecond);
}

MULTIROOM_TEST(a_resume_starts_at_exactly_the_position_the_pause_named)
{
    StartScheduler scheduler(defaults());

    const ClientId kitchen("kitchen");
    const ClientId living("living");
    const uint64_t startedAt = startTwoRooms(scheduler, kitchen, living);

    const uint64_t pauseAt = startedAt + 3 * kSecond;
    TimelineNs position{};
    CHECK_EQ(scheduler.pause(pauseAt, position).size(), size_t{2});

    // A paused zone decides nothing, however ready a room says it is: the
    // line it would be put on is standing still, and starting one room alone
    // is what "the same logical position" rules out.
    const uint64_t held = std::to_underlying(position);
    scheduler.onBufferStatus(kitchen, buffered(held, 1500 * kMs), 20);
    scheduler.onBufferStatus(living, buffered(held, 1500 * kMs), 20);
    CHECK(scheduler.evaluate(pauseAt + 1 * kSecond).empty());

    scheduler.resume();
    CHECK(scheduler.transport() == TransportState::Starting);

    const uint64_t resumeAt = pauseAt + 2 * kSecond;
    const std::vector<RoomCommand> again = scheduler.evaluate(resumeAt);

    // The whole point of putting a resume through the barrier: it lands on the
    // frame the pause named rather than near it, and both rooms land on the
    // same one at the same instant.
    CHECK_EQ(again.size(), size_t{2});
    CHECK(again.front().kind == RoomCommandKind::Start);
    CHECK_EQ(again.front().start.timelineNs, held);
    CHECK_EQ(again.back().start.timelineNs, held);
    CHECK_EQ(again.front().start.serverTimeNs, resumeAt + 1 * kSecond);
    CHECK_EQ(again.back().start.serverTimeNs, resumeAt + 1 * kSecond);

    // And the line runs again from there.
    CHECK(scheduler.transport() == TransportState::Playing);
    CHECK_EQ(std::to_underlying(scheduler.playheadAt(resumeAt + 1500 * kMs)),
             held + 500 * kMs);
}

MULTIROOM_TEST(a_stop_abandons_the_line_and_the_next_start_picks_its_own_position)
{
    StartScheduler scheduler(defaults());

    const ClientId kitchen("kitchen");
    const ClientId living("living");
    const uint64_t startedAt = startTwoRooms(scheduler, kitchen, living);

    const uint64_t stopAt = startedAt + 2 * kSecond;
    TimelineNs position{};
    const std::vector<RoomCommand> stopped = scheduler.stop(stopAt, position);

    CHECK_EQ(stopped.size(), size_t{2});
    CHECK(stopped.front().kind == RoomCommandKind::Stop);
    CHECK_EQ(std::to_underlying(position), 3 * kSecond);
    CHECK_EQ(stopped.front().at.timelineNs, 3 * kSecond);
    CHECK_EQ(scheduler.stopsSent(), uint64_t{2});

    // There is no line any more, which is the difference from a pause.
    CHECK(!scheduler.started());
    CHECK(scheduler.transport() == TransportState::Stopped);
    CHECK(scheduler.roomState(kitchen) == RoomStartState::Waiting);

    // Section 38 dropped what every room held, so what every room last said
    // about its buffer describes a timeline nobody is playing: nothing is
    // scheduled until each of them has reported a new one.
    CHECK(scheduler.evaluate(stopAt + 1 * kSecond).empty());

    // And the start that follows picks its own position, with nothing left of
    // the old one in it.
    const uint64_t restart = 30 * kSecond;
    scheduler.onBufferStatus(kitchen, buffered(restart, 1500 * kMs), 20);
    scheduler.onBufferStatus(living, buffered(restart, 1500 * kMs), 20);

    const std::vector<RoomCommand> again = scheduler.evaluate(stopAt + 2 * kSecond);
    CHECK_EQ(again.size(), size_t{2});
    CHECK_EQ(again.front().start.timelineNs, restart);
    CHECK(scheduler.transport() == TransportState::Playing);
}

MULTIROOM_TEST(a_report_about_the_stream_a_room_had_before_it_was_moved_is_not_a_position)
{
    StartScheduler scheduler(defaults());

    const ClientId kitchen("kitchen");
    const ClientId living("living");
    const uint64_t startedAt = startTwoRooms(scheduler, kitchen, living);

    // Stop, then play from a source that begins further on: the zone moves
    // both rooms there before the STOP_AT has come round in either of them.
    TimelineNs position{};
    CHECK_EQ(scheduler.stop(startedAt + 2 * kSecond, position).size(), size_t{2});
    const uint64_t restart = 5 * kSecond;
    scheduler.rejoin(kitchen, TimelineNs{restart});
    scheduler.rejoin(living, TimelineNs{restart});

    // The kitchen is still playing the abandoned line and says so. That is not
    // a position the new line can start it at -- evaluated past the ready
    // timeout, so the silent living room holds nothing back and the kitchen
    // alone would form the barrier.
    scheduler.onBufferStatus(kitchen, buffered(2 * kSecond, 1500 * kMs), 20);
    CHECK(scheduler.evaluate(startedAt + 10 * kSecond).empty());
    CHECK(!scheduler.started());

    // Its first report from the new stream is.
    scheduler.onBufferStatus(kitchen, buffered(restart, 1500 * kMs), 20);
    scheduler.onBufferStatus(living, buffered(restart, 1500 * kMs), 20);
    const std::vector<RoomCommand> again = scheduler.evaluate(startedAt + 11 * kSecond);
    CHECK_EQ(again.size(), size_t{2});
    CHECK_EQ(again.front().start.timelineNs, restart);
    CHECK_EQ(again.back().start.timelineNs, restart);
}

MULTIROOM_TEST(a_room_that_is_not_on_the_line_is_told_neither_to_pause_nor_to_stop)
{
    StartScheduler scheduler(defaults());

    const ClientId playing("playing");
    const ClientId waiting("waiting");

    becomeReady(scheduler, playing, TimelineNs{0}, kT0);
    CHECK_EQ(scheduler.evaluate(kT0).size(), size_t{1});
    scheduler.onPlaybackReport(playing, audibleAt(0));

    // Its device answered and its buffer has not: there is nothing in this
    // room to pause, and a PAUSE_AT naming a position it was never told to
    // play is a message about nothing.
    scheduler.expect(waiting, kT0 + 1 * kSecond);
    scheduler.join(waiting, TimelineNs{0}, kT0 + 1 * kSecond);

    TimelineNs position{};
    const std::vector<RoomCommand> paused = scheduler.pause(kT0 + 2 * kSecond, position);
    CHECK_EQ(paused.size(), size_t{1});
    CHECK_EQ(paused.front().client.str(), std::string("playing"));

    scheduler.resume();
    CHECK_EQ(scheduler.evaluate(kT0 + 3 * kSecond).size(), size_t{0});

    TimelineNs where{};
    const std::vector<RoomCommand> stopped = scheduler.stop(kT0 + 4 * kSecond, where);
    CHECK_EQ(stopped.size(), size_t{0});

    // Nothing was on the line to stop: the resume put the one room that was
    // back into the barrier, and the barrier had not resolved.
    CHECK(scheduler.transport() == TransportState::Stopped);
}

// --- phase 27: a room's own output latency -------------------------------------

MULTIROOM_TEST(a_room_with_an_output_latency_is_started_that_much_earlier_for_the_same_position)
{
    StartScheduler scheduler(defaults());

    const ClientId plain("plain");
    const ClientId receiver("receiver");
    const ClientId soundbar("soundbar");

    becomeReady(scheduler, plain, TimelineNs{0}, kT0);
    becomeReady(scheduler, receiver, TimelineNs{0}, kT0);
    becomeReady(scheduler, soundbar, TimelineNs{0}, kT0);
    scheduler.setLatency(receiver, 80 * kMs);
    scheduler.setLatency(soundbar, 120 * kMs);

    const std::vector<RoomCommand> commands = scheduler.evaluate(kT0);
    CHECK_EQ(commands.size(), size_t{3});
    const RoomCommand* toPlain = commandFor(commands, "plain");
    const RoomCommand* toReceiver = commandFor(commands, "receiver");
    const RoomCommand* toSoundbar = commandFor(commands, "soundbar");
    CHECK(toPlain != nullptr && toReceiver != nullptr && toSoundbar != nullptr);

    // One position, and instants apart by exactly each room's latency. The
    // line is put back by the largest of them, so the earliest start is still
    // a whole start delay away.
    CHECK_EQ(toReceiver->start.timelineNs, toPlain->start.timelineNs);
    CHECK_EQ(toSoundbar->start.timelineNs, toPlain->start.timelineNs);
    CHECK_EQ(toPlain->start.serverTimeNs - toReceiver->start.serverTimeNs, 80 * kMs);
    CHECK_EQ(toPlain->start.serverTimeNs - toSoundbar->start.serverTimeNs, 120 * kMs);
    CHECK_EQ(toSoundbar->start.serverTimeNs, kT0 + 1 * kSecond);
    CHECK_EQ(scheduler.startServerTimeNs(), toPlain->start.serverTimeNs);

    // The acknowledgement waits from the instant that room was given.
    scheduler.onPlaybackReport(plain, audibleAt(0));
    scheduler.onPlaybackReport(soundbar, audibleAt(0));
    CHECK(scheduler.evaluate(toReceiver->start.serverTimeNs + 1 * kSecond - 1).empty());
    CHECK(scheduler.roomState(receiver) == RoomStartState::Scheduled);
    static_cast<void>(scheduler.evaluate(toReceiver->start.serverTimeNs + 1 * kSecond));
    CHECK(scheduler.roomState(receiver) != RoomStartState::Scheduled);
}

MULTIROOM_TEST(a_late_joiner_with_an_output_latency_is_still_started_a_start_delay_ahead)
{
    StartScheduler scheduler(defaults());

    const ClientId playing("playing");
    becomeReady(scheduler, playing, TimelineNs{0}, kT0);
    const std::vector<RoomCommand> first = scheduler.evaluate(kT0);
    CHECK_EQ(first.size(), size_t{1});
    scheduler.onPlaybackReport(playing, audibleAt(0));

    // Two seconds into the line a room joins with 300 ms of latency. Its
    // position is a start delay and its latency past what is audible, so the
    // instant it is given -- the line's for that position, less the latency --
    // is still a whole start delay from now.
    const uint64_t now = first.front().start.serverTimeNs + 2 * kSecond;
    const ClientId late("late");
    becomeReady(scheduler, late, TimelineNs{2500 * kMs}, now);
    scheduler.setLatency(late, 300 * kMs);

    const std::vector<RoomCommand> commands = scheduler.evaluate(now);
    CHECK_EQ(commands.size(), size_t{1});
    CHECK_EQ(commands.front().start.timelineNs, 3300 * kMs);
    CHECK_EQ(commands.front().start.serverTimeNs, now + 1 * kSecond);
    CHECK_EQ(scheduler.startServerTimeNs() + 3300 * kMs - commands.front().start.serverTimeNs,
             300 * kMs);

    // A latency that pushes the position past everything a room holds is a
    // room the line has run past, the same as any other.
    const ClientId cramped("cramped");
    becomeReady(scheduler, cramped, TimelineNs{2500 * kMs}, now, 1100 * kMs);
    scheduler.setLatency(cramped, 1200 * kMs);
    CHECK(scheduler.evaluate(now).empty());
    CHECK(scheduler.roomState(cramped) == RoomStartState::Rejoining);
}
