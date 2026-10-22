#pragma once

// When a zone's rooms are told to make a media position audible, to stop
// making one audible, and which rooms are told at all.
//
// This is MATP section 20 as a state machine: bytes in, decisions out. It
// holds no socket, no thread, no clock and no zone -- every input is handed to
// it and the time is a parameter -- which is what lets the whole of the
// barrier, the readiness timeout, the acknowledgement and the late join be
// tested in process against an injected clock, without sleeping through a five
// second timeout and without a room that has to exist.
//
// Phase 14 puts sections 37 and 38 here rather than beside it, because a pause
// and a stop are the same line this class already owns, seen from the other
// end: a pause freezes the playhead, a resume is a new first start at exactly
// the position the pause named, and a stop abandons the line so that the next
// start picks its own position again. Two owners of one playhead would be two
// answers to "what is audible now".
//
// Four decisions the protocol states and one it does not.
//
//   * **Two conditions, not one** (section 20). A room is schedulable when its
//     device has granted a stream *and* it holds the minimum start buffer. A
//     client can hold a full buffer while its device is still opening, and a
//     client whose device will never open holds a full buffer forever. A third
//     is added here because the protocol gives the server no way to hear about
//     it: the client refuses a START_AT it has no clock mapping for and says
//     so only to itself, so a room is not scheduled until it has reported
//     enough completed clock exchanges (section 8) to have one.
//
//   * **The barrier applies only before the first START_AT.** Once the anchor
//     exists nothing waits for anybody: a room that becomes ready afterwards
//     is placed on the line that is already running (section 44), and the
//     rooms that are playing are not held for it.
//
//   * **The anchor is the zone's playhead, and every later START_AT is derived
//     from it.** The first start picks a timeline position and an instant; the
//     timeline then advances at the rate of the server's monotonic clock,
//     which is the one fact section 20's mapping rests on. A room joining
//     later is not negotiated with -- the instant its own first buffered
//     position becomes audible is arithmetic. (An addition within one axis,
//     not a conversion between two: the frame axes are still SourceTimeline's
//     and OutputClock's alone.)
//
//   * **A room that misses its window is left behind deliberately.** One room
//     with a slow driver must never keep the others silent. A room that has
//     answered nothing at all after three starts is LeftBehind with a reason
//     rather than retried forever; a room the line has simply run *past* is
//     not the same case and is not treated as one -- there is no position a
//     START_AT could name, so it asks for that room's stream to be moved
//     (Rejoining, and takeRejoins() below) and places it again from where the
//     move put it. Moving a stream is the caller's, which is why this class
//     asks rather than acts.
//
//   * **The acknowledgement is a PLAYBACK_REPORT that names an audible
//     timeline position**, because the protocol has no other. The client that
//     cannot act on a START_AT logs it and answers nothing, so a start that
//     was not acknowledged by the instant it named is re-sent at a fresh
//     position on the same line -- not the same pair, which is now in the
//     past.
//
// And two more that phase 14 could not avoid.
//
//   * **A pause and a stop name one position for the whole house**, a start
//     delay ahead of what is audible now, because section 37 asks every room
//     to stop at the same logical position and the message has the same LAN to
//     cross that a START_AT has. A room that is not on the line is sent
//     neither: there is nothing there to pause.
//
//   * **A resume is a start, and every rule a start has applies to it.** It
//     goes back through the barrier rather than straight onto the wire, so a
//     room whose device has closed in the meantime holds it exactly as it
//     would have held the first one -- the single difference being that the
//     position is fixed at what the pause named instead of being picked from
//     what the rooms happen to hold. That is what makes a pause and a resume
//     land on the same frame.
//
// And one from phase 27: **a room's output latency moves its START_AT and
// nothing else.** Its instant is earlier by the latency for the same position,
// so the room is heard on the line although its card plays ahead of it. A
// pause, a stop and a flush need no such change: the client places each of
// them by timeline position on the mapping its START_AT gave it, and logs the
// instant without using it.

#include "core/Ids.h"
#include "core/Units.h"
#include "matp/Protocol.h"
#include "matp/RoomCommand.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace multiroom {

// Where one room is in the start sequence. Everything except Playing is a
// room that is not yet making sound.
enum class RoomStartState
{
    Expecting,  // a format was announced to it; its device has not answered
    Waiting,    // its device answered; it is not schedulable yet
    Ready,      // both of section 20's conditions are met; holding the barrier
    Scheduled,  // START_AT is out, and the report that confirms it is not in
    Playing,    // a playback report has named an audible timeline position
    Paused,     // PAUSE_AT is out; its buffer is kept for the resume
    Rejoining,  // the line ran past its buffer; its stream is being moved
    LeftBehind  // no position on the line is still inside its buffer
};

const char* roomStartStateName(RoomStartState state);

// What the zone's timeline itself is doing, which is a different question from
// what any one room is doing.
enum class TransportState
{
    // Nothing is on the line. The next start picks its own position from what
    // the rooms hold, which is what the first start of a zone does and what
    // the first start after a stop does again.
    Stopped,

    // A position is fixed and the barrier is forming around it: a resume,
    // waiting for the rooms that are still there.
    Starting,

    // The line is running, and the playhead with it.
    Playing,

    // Frozen at the position a PAUSE_AT named. The rooms keep what they hold.
    Paused
};

const char* transportStateName(TransportState state);

struct StartSchedulerSettings
{
    // Section 20's second condition, and section 46's value for it. Less than
    // the normal prebuffer on purpose: what the barrier needs is enough audio
    // that the start cannot underrun, not a full buffer.
    uint64_t minimumStartBufferNs = 1000ULL * 1000000ULL;

    // Section 46's client ready timeout: how long the first START_AT waits for
    // a room that has been announced a format. A room that has not reported
    // both conditions by then is left out of it and joins later.
    uint64_t readyTimeoutNs = 5ULL * kNanosecondsPerSecond;

    // How far ahead of now a START_AT is placed. Section 20's example is two
    // seconds; one is kept here because the message has only a LAN to cross
    // and the client aligns it to a device period rather than to a wakeup
    // (section 21), and because it is also the lead every *late* join is given
    // on a zone that is already playing.
    uint64_t startDelayNs = 1ULL * kNanosecondsPerSecond;

    // How long past the instant a START_AT named this waits for the
    // PLAYBACK_REPORT that confirms it before trying again.
    uint64_t ackTimeoutNs = 1ULL * kNanosecondsPerSecond;

    // How many START_ATs one room is sent before it is left behind. Three is
    // two repairs: a client that has answered none of them is not a client
    // that will answer the fourth.
    uint32_t startAttempts = 3;

    // Completed clock exchanges (CLOCK_SYNC_RESULT) before a room may be
    // scheduled. The client's own model needs four samples before it will map
    // a server instant at all, and it sends twenty in a burst on connect
    // (section 46), so this is reached in a fifth of a second and is never the
    // reason a start is late.
    uint32_t minClockExchanges = 4;
};

class StartScheduler
{
public:
    explicit StartScheduler(StartSchedulerSettings settings = {});

    // --- membership ---------------------------------------------------------

    // A format has been announced to this room (section 10's SESSION_CONFIG),
    // so it is a room this zone's first start waits for -- but only until the
    // ready timeout, and only while it is still connected. This is what makes
    // the barrier a barrier: a room that has not yet opened its device is not
    // a member yet, and a start that only counted members would go without it.
    void expect(const ClientId& client, uint64_t nowNs);

    // Its device granted a stream and its output has joined the timeline at
    // `origin`. The deadline is not restarted: a room that has been holding
    // the barrier for four seconds has one left, not six.
    void join(const ClientId& client, TimelineNs origin, uint64_t nowNs);

    // The room is gone, with its output. Nothing here outlives it.
    void leave(const ClientId& client);

    // Its stream has been moved to `origin` (sections 32 and 43), so
    // everything this room said about the audio it held describes a position
    // it is no longer at. It goes back to waiting and is placed on the line
    // again by the next evaluate(), from what it reports about the audio it
    // holds now -- which is the late join of section 44 reached by a room that
    // was already playing.
    //
    // Only the caller can move a stream, so this class asks rather than acts:
    // takeRejoins() below is the asking, and this is the answer.
    void rejoin(const ClientId& client, TimelineNs origin);

    // Phase 27: this room is heard `latencyNs` after its card plays, so every
    // START_AT it is sent names an instant that much earlier for the same
    // position, and it plays that far ahead of the line out of audio it
    // already holds. Nothing else about the room changes; a room that is
    // already on the line keeps the start it was given until it is started
    // again. Ignored for a room this class does not know.
    void setLatency(const ClientId& client, uint64_t latencyNs);

    // The control API's audio.configure: the barrier's minimum, the first
    // start's wait and the lead of every START_AT, PAUSE_AT and STOP_AT from
    // now on. A room already waiting keeps the deadline it was given.
    void setTiming(uint64_t minimumStartBufferNs, uint64_t readyTimeoutNs, uint64_t startDelayNs);

    // --- what the control connection reports --------------------------------

    // Section 12, both answers. `ready` false is not a fault and does not end
    // anything: section 20's table drops that room from scheduling and keeps
    // its state, so it is scheduled again when it reports ready once more.
    void onStreamReady(const ClientId& client, bool ready);

    // Section 19, four times a second, and the completed clock exchanges of
    // section 8 counted at the same moment -- the two halves of "can this room
    // be scheduled?" arrive on one connection and are read together.
    void onBufferStatus(const ClientId& client, const matp::BufferStatus& status,
                        uint64_t clockExchanges);

    // Section 25. Only one field of it matters here: a report that names an
    // audible timeline position is this room confirming that its start took,
    // and it is the only acknowledgement the protocol has.
    void onPlaybackReport(const ClientId& client, const matp::PlaybackReport& report);

    // --- the decision --------------------------------------------------------

    // Everything the caller is to send now: the first start once the barrier
    // resolves, one late join per room that became schedulable afterwards, and
    // one re-send per room whose start was not acknowledged. Calling it more
    // often than the inputs move costs a walk of the member map.
    //
    // Nothing is decided while the transport is paused or stopped: a room that
    // has just become schedulable then is a room with nowhere to be put.
    [[nodiscard]] std::vector<RoomCommand> evaluate(uint64_t nowNs);

    // The rooms the line has run past since this was last called: every
    // position they still hold is behind what is audible now, so no START_AT
    // can place them and only moving their streams will (section 32). Drained
    // by the call, because the caller is what moves a stream and a room that
    // has been asked for twice would be moved twice.
    //
    // A room stays Rejoining until rejoin() says where it went, so nothing
    // here asks again while the caller is still working on it -- and a room
    // whose stream could not be moved stays visible as rejoining rather than
    // quietly becoming a room that is simply never scheduled.
    [[nodiscard]] std::vector<ClientId> takeRejoins();

    // --- the transport --------------------------------------------------------

    // Section 37. One position for the whole house, a start delay ahead of
    // what is audible now, and a PAUSE_AT for every room that is on the line;
    // the playhead freezes there and nothing is scheduled until resume().
    // Empty, and nothing changes, on a transport that is not playing.
    //
    // `position` is where it froze, which is not a thing the commands can be
    // asked for: a zone with no room on the line still pauses, and still has
    // to be able to say where.
    [[nodiscard]] std::vector<RoomCommand> pause(uint64_t nowNs, TimelineNs& position);

    // Section 37's resume: the barrier forms again at exactly the position the
    // pause named, and the next evaluate() places the rooms on it. Nothing is
    // sent from here, because a resume is a start and a start goes through the
    // barrier. Ignored on a transport that is not paused.
    //
    // It takes no instant, which is the one place this class departs from "the
    // time is a parameter": nothing about a resume happens now. It puts the
    // barrier back, and the barrier is resolved by evaluate() against the
    // instant evaluate() is given.
    void resume();

    // Section 38. The same two numbers as a pause and a different consequence:
    // what each room holds is dropped rather than kept, the line is abandoned,
    // and every room goes back to waiting -- so the next start is a first
    // start again and picks its own position. Empty on a transport that is
    // already stopped, and `position` is where it stopped.
    [[nodiscard]] std::vector<RoomCommand> stop(uint64_t nowNs, TimelineNs& position);

    // --- section 24's format transition ---------------------------------------

    // One room, not the house: a room whose device has to be reopened for the
    // source that begins at `at` is paused exactly there, on the instant the
    // running line reaches it, so the old source is played to its end. The
    // line keeps running for every room that plays straight through the same
    // boundary. Empty for a room that is not on a running line -- there is
    // nothing there to pause.
    //
    // The room stays Paused until rejoin() says where its new stream begins,
    // so nothing schedules it on the old one in the meantime.
    [[nodiscard]] std::optional<RoomCommand> pauseForTransition(const ClientId& client,
                                                                TimelineNs at);

    // The house, when nobody is left on the line to play through: the line
    // stops at `at` and forms the barrier there again, exactly as a resume
    // does, so the rooms that reopened their devices are started at `at`
    // rather than wherever the line would have run on to while they did
    // (section 24 steps 6 and 8). Every room the transition paused is given
    // the ready timeout afresh from `nowNs`, which is what bounds the wait.
    //
    // False, and nothing changes, while any room is still playing or
    // scheduled: that room is the line, and the rooms that reopened join it
    // wherever it has got to.
    bool freezeForTransition(TimelineNs at, uint64_t nowNs);

    // --- a second room on a line one room has had to itself -------------------

    // `client` is alone on the running line and heard `offsetNs` off it
    // (positive: ahead), which is what a card nobody corrects walks to while
    // it plays alone. The line is moved by exactly that, so a room joining now
    // is placed where that room is heard instead of where the line had
    // drifted to -- and the room on it is not moved at all: it is sent
    // nothing, and every later position derived from the line agrees with
    // what it plays.
    //
    // False, and nothing changes, unless the transport is playing, `client`
    // is Playing and no other room is on the line: a second room on it is
    // what the line is kept for.
    bool alignLineTo(const ClientId& client, int64_t offsetNs);

    // --- what status reports --------------------------------------------------

    [[nodiscard]] TransportState transport() const noexcept { return transport_; }

    // There is a position on the line: a zone that is playing, paused, or
    // forming the barrier for a resume. False before the first START_AT of a
    // zone and after a stop, which are the two states in which a room joining
    // has no playhead to be placed ahead of.
    [[nodiscard]] bool started() const noexcept
    {
        return transport_ != TransportState::Stopped;
    }

    // Where the line stands, and the instant that position became audible.
    // Together they are what every later start is derived from; the first is
    // also what a pause froze the line at, because a paused line stands still
    // rather than ceasing to exist.
    [[nodiscard]] TimelineNs startTimelineNs() const noexcept { return startTimelineNs_; }
    [[nodiscard]] uint64_t startServerTimeNs() const noexcept { return startServerTimeNs_; }

    // What is audible now, on the zone's own axis. It advances only while the
    // transport is playing: a paused zone's playhead is the position the pause
    // named, and a stopped zone's is zero, which is not a position on the line
    // but the absence of one -- `started()` is what distinguishes them.
    [[nodiscard]] TimelineNs playheadAt(uint64_t nowNs) const;

    [[nodiscard]] RoomStartState roomState(const ClientId& client) const;

    // Why a room was left behind, for the status the operator reads. Empty
    // while it was not.
    [[nodiscard]] std::string roomDetail(const ClientId& client) const;

    // Why a room is not playing when the reason is not this class's own -- a
    // room held at a format boundary that cannot be served after it (section
    // 24). Kept until the room's state next changes here, like every other
    // detail.
    void explain(const ClientId& client, std::string detail);

    [[nodiscard]] size_t roomCount() const noexcept { return rooms_.size(); }
    [[nodiscard]] uint64_t startsSent() const noexcept { return startsSent_; }
    [[nodiscard]] uint64_t pausesSent() const noexcept { return pausesSent_; }
    [[nodiscard]] uint64_t stopsSent() const noexcept { return stopsSent_; }
    [[nodiscard]] uint64_t leftBehindCount() const noexcept { return leftBehind_; }

    // Rooms this class has asked to have moved. It counts the asking, not the
    // moving: Zone::rejoinCount() is what the triggers of phase 16 add up to,
    // and this is the one of them the line itself noticed.
    [[nodiscard]] uint64_t rejoinsAsked() const noexcept { return rejoinsAsked_; }

    // Times the line was moved to a room alone on it instead of that room
    // being asked to rejoin.
    [[nodiscard]] uint64_t linesRestarted() const noexcept { return linesRestarted_; }

private:
    struct Room
    {
        bool member = false;
        TimelineNs origin{};

        // When this room stops holding the first start back. Set once, when
        // the room is first heard of.
        uint64_t deadlineNs = 0;

        bool streamReady = false;
        matp::BufferStatus buffer;
        uint64_t clockExchanges = 0;

        RoomStartState state = RoomStartState::Expecting;
        std::string detail;

        // The instant the last START_AT named, and how many have been sent.
        uint64_t startServerTimeNs = 0;
        uint32_t attempts = 0;

        // How much earlier than the line this room's START_AT names (phase
        // 27). Zero for a room played through its card alone.
        uint64_t latencyNs = 0;
    };

    // Section 20's conditions, all three of them, for one room.
    [[nodiscard]] bool schedulable(const Room& room) const;

    // The first position on the running line that this room could still be
    // started at: far enough ahead to be in the future, and inside the audio
    // this room says it holds. False when there is none, which is a room the
    // line has run past. `serverTimeNs` is this room's own instant for it,
    // its latency already taken off.
    [[nodiscard]] bool placeOnLine(const Room& room, uint64_t nowNs, uint64_t& timelineNs,
                                   uint64_t& serverTimeNs) const;

    // When the line has run past `room` and no other room is on it: the line
    // starts again at the oldest position the room holds, instead of the room
    // being asked to rejoin. False, and nothing changes, when another room is
    // on the line -- that room is what the line is kept for.
    [[nodiscard]] bool restartLineAt(const ClientId& client, Room& room, uint64_t nowNs,
                                     uint64_t& timelineNs, uint64_t& serverTimeNs);

    // The barrier, once. Returns false while it is still waiting.
    [[nodiscard]] bool resolveBarrier(uint64_t nowNs, std::vector<RoomCommand>& commands);

    // A barrier at a fixed position that no room is ready for: each room past
    // its window that holds audio there, but less than the start minimum, is
    // asked to rejoin (section 24 step 10) -- the line's ceiling would never
    // send it the rest.
    void rejoinShortRooms(uint64_t nowNs);

    // Whether this room is on the line and therefore has something to pause or
    // to stop. A room still waiting for the barrier has not been told to play
    // anything, so telling it to stop would be a message about nothing.
    [[nodiscard]] static bool onTheLine(const Room& room);

    void leaveBehind(Room& room, std::string detail);

    static RoomCommand startFor(const ClientId& client, uint64_t timelineNs,
                                uint64_t serverTimeNs);

    StartSchedulerSettings settings_;

    std::map<ClientId, Room> rooms_;

    TransportState transport_ = TransportState::Stopped;

    // Where the line stands, and when that position was or will be audible.
    // The second is meaningless while the transport is not playing.
    TimelineNs startTimelineNs_{};
    uint64_t startServerTimeNs_ = 0;

    uint64_t startsSent_ = 0;
    uint64_t pausesSent_ = 0;
    uint64_t stopsSent_ = 0;
    uint64_t leftBehind_ = 0;
    uint64_t rejoinsAsked_ = 0;
    uint64_t linesRestarted_ = 0;

    // Rooms whose streams the caller has been asked to move and has not yet
    // said it moved. Bounded by the member map above it.
    std::vector<ClientId> rejoinWanted_;
};

} // namespace multiroom
