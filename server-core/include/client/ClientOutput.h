#pragma once

// One room's audio path, from the zone's decoded PCM to that room's socket,
// on that room's own thread.
//
// This is where the specification's central rule stops being a principle and
// becomes a thread: **one client can never block another, and no client can
// block a zone.** A room that stops reading its socket fills its own bounded
// queue, on its own thread, and is failed by its own timeout. Nothing it does
// reaches the decoder, the store or any other room -- the store's writer
// evicts a slow cursor rather than waiting for it, and every buffer between
// here and the socket has a limit.
//
// The path, in order:
//
//     NativePcmStore cursor          the zone's decode, read at this room's pace
//         -> IClientAudioProcessor   this room's plan, executed (phase 7)
//         -> OutputClock             how many frames the timeline has asked for
//         -> OutputPacketizer        the 72-byte header and this room's PCM
//         -> a bounded queue         what a socket that is not draining costs
//         -> IPacketSink             the audio connection
//
// Three decisions the protocol does not make and this class cannot avoid:
//
//   * **Pacing is against `bufferedDurationNs`, never against the wall clock.**
//     A room that has been paused stops draining its buffer; a sender paced by
//     its own clock keeps producing, fills the socket, blocks inside the send,
//     and never gets to the resume -- which looks from the outside exactly
//     like a client ignoring commands, and cost the client's own bring-up an
//     hour. The rule is to produce while
//     `bufferedDurationNs + producedSince(that report) < target` and to stop
//     otherwise. It follows that this class reads the injected clock for one
//     thing only: how long a full queue has been full.
//
//   * **The send path never blocks.** The sink is non-blocking and the queue
//     is bounded. Leaning on TCP's own backpressure with a blocking send would
//     isolate the rooms just as well -- it is what the client does on its
//     receiving side -- but it leaves the server unable to tell a room that is
//     merely full from a room that has gone away, and those need different
//     answers.
//
//   * **A packet's timeline span is the grid's, and its frame count is the
//     clock's.** The span is `origin + n * packetDuration`, computed from the
//     packet's index rather than added to the last one, so a stream of any
//     length is exactly contiguous and carries no accumulated rounding
//     (section 15). The frame count is what OutputClock's running target asks
//     for, so a corrected room's extra frame lands where a resampler would
//     have put it. The two fields move independently, which is the entire
//     reason section 14 carries both.
//
// Phase 13 adds a fourth, and it is where the drift correction of sections 28
// and 30 stops being a number in a log line: **the trim moves this room's
// clock and its converter together, on this room's own thread, or it moves
// neither.** The clock decides how many frames a span of timeline asks for and
// the converter decides how much source audio those frames are made of, so a
// trim that reaches one of them and not the other is a room being sent the
// right number of the wrong frames. setDriftTrim() therefore records a wish
// from the listener's thread and pump() is what grants it -- and grants it
// only to a room whose plan says it may be corrected at all. A bitperfect room
// is never given a ratio under any circumstance (the roadmap's phase 13,
// against MATP section 32's recommendation), and this is the class that
// refuses one.
//
// Phase 15 adds a fifth, and it is the only place a room's stream changes
// shape without ending: **the source format changes at one position on the
// timeline, and the packet that would have crossed it is cut short.** The grid
// cannot straddle a boundary whose two sides are different bytes, so holdAt()
// produces a short final packet that ends exactly on it -- the old source is
// played to its end, nothing is repeated, and section 24's "sample-accurate on
// both sides of the gap" is arithmetic rather than an intention. switchTo()
// then names the converter for what follows and where this stream carries on,
// and the packetizer's sequence runs on through both: the client anchors on
// the first packet of a connection and reads a gap in the numbering as a
// server that skipped ahead, whatever its device did in between.
//
// Phase 16 adds a sixth, and it is the one that turns three different
// accidents into one mechanism: **a room that has been put somewhere other
// than where the timeline says it is gets a new position, and nothing else.**
// rejoinAt() is the whole of it -- the store cursor, the grid, the clock and
// the converter move, the packetizer's numbering does not -- and sections 32
// and 43 both arrive on the wire as exactly that: a discontinuity in
// `timelineStartNs` followed by a START_AT. There is no HARD_RESYNC message
// because there does not need to be one.
//
// What this class decides is when it cannot go on where it is, and it decides
// that once: the store has dropped the audio its cursor named. That is
// **Stranded** rather than a failure -- the room is intact, the connection is
// intact, and the only thing missing is where it is now, which is the one
// thing this class cannot work out for itself. Everything else phase 16 adds
// here is a *subtraction*: a dead audio connection and a connection that has
// stopped taking bytes stop ending the room, because the session is on the
// control connection (section 13) and losing a socket is losing a socket. The
// stream keeps its position and its queue, and the next attach() resumes on
// it -- which costs nothing at all when the connection comes back before the
// decode runs past this room, and is a rejoin when it does not.
//
// The numbering is the one thing that cannot be dropped along with the audio.
// Packets that were built, numbered and then thrown away have to give their
// numbers back (OutputPacketizer::rewind), because the client anchors the
// sequence on the first packet of a connection and reads a gap in it as a
// server that skipped ahead -- and drops the connection, not the packet.
//
// **Threading.** `pump()` and everything it reaches belong to this room's own
// thread. `attach()`, `detach()` and `onBufferStatus()` are called from the
// two listeners' threads and take a mutex that is held for a pointer swap or
// three integers. The counters are atomics the output thread writes and status
// reads. Either run() the thread or drive pump() yourself -- a caller that
// does both is two threads in one loop.

#include "audio/NativePcmStore.h"
#include "client/IPacketSink.h"
#include "client/OutputClock.h"
#include "client/OutputPacketizer.h"
#include "core/Error.h"
#include "core/Ids.h"
#include "core/Units.h"
#include "matp/Protocol.h"
#include "processing/IClientAudioProcessor.h"
#include "timeline/SourceTimeline.h"
#include "util/Clock.h"
#include "util/Logger.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace multiroom {

enum class OutputError
{
    InvalidSettings,  // a packet duration or a target of zero
    InvalidSession,   // section 13's all-zero id, which is "no session"
    InvalidPlan,      // no processor, or one whose output format is not a format
    NotJoined,        // pump() or run() before joinAt() named a position
    AlreadyJoined,    // joinAt() twice; a second position needs rejoinAt()
    NotParked,        // rejoinAt() on a stream that is neither parked nor stranded
    NotHeld,          // switchTo() on a stream holdAt() has not stopped at a boundary
    NotStreaming,     // replaceProcessor() on a stream that is not producing
    SwitchPending,    // replaceProcessor() while another converter waits to be installed
    TimelineFailed,   // nothing staged, or a position the segment table no longer holds
    ClockFailed,      // the running target ran past what 64 bits hold
    StoreFailed,      // the cursor fell behind the decode and was evicted
    ProcessorFailed,  // this room's converter refused a block
    PacketFailed,     // a packet this server would not accept from a peer
    SinkFailed,       // the audio connection is gone
    QueueStalled      // the queue stayed full past the timeout: the room is not reading
};

const char* errorName(OutputError error);

enum class OutputState
{
    Idle,      // created; no position, no thread
    Streaming, // joined, and producing whenever pacing allows
    Stopped,   // stop() was called
    Parked,    // section 38's stop: nothing is produced until a new position
    Stranded,  // the decode ran past this room's cursor; only a rejoin follows
    Failed     // lastError() says which room and why
};

const char* outputStateName(OutputState state);

struct ClientOutputSettings
{
    // MATP section 46. A duration and never a frame count: 20 ms is 882 frames
    // at 44.1 kHz, 960 at 48 kHz and 1920 at 96 kHz, and one value covers
    // every room in the house.
    uint32_t packetDurationMs = 20;

    // Section 46's normal prebuffer, and the only thing pacing compares
    // against. The client's own ring is its target plus a second of headroom,
    // so a server that paces to this never fills it -- which is what keeps the
    // client's `bufferFullWaits` in the low single digits instead of the
    // hundreds.
    uint64_t targetBufferNs = 1500ULL * 1000000ULL;

    // Memory is bounded, and this is the bound. Sixty-four packets is 1.28 s
    // of audio at the default duration and about a megabyte in the widest
    // format -- far more than a socket that is draining ever holds, and far
    // less than a room that has gone away could otherwise cost.
    size_t maxQueuedPackets = 64;

    // How long a full queue is tolerated once there is a connection to drain
    // it. A room whose client is merely holding a full prebuffer is not here
    // at all: pacing stopped producing long before the queue filled. Reaching
    // this means the socket has stopped taking bytes while the room still owes
    // its buffer audio, which is a room that has gone away without its TCP
    // connection noticing.
    uint64_t queueStallTimeoutNs = 10ULL * 1000000000ULL;
};

class ClientOutput
{
public:
    // The store and the timeline are the zone's and outlive every output of
    // it; the processor is this room's own and is given up here, because it is
    // touched by this room's thread and by nothing else.
    [[nodiscard]] static Result<std::unique_ptr<ClientOutput>, OutputError>
    create(SessionId session, ClientId client, NativePcmStore& store,
           const SourceTimeline& timeline, std::unique_ptr<IClientAudioProcessor> processor,
           const IClock& clock, ClientOutputSettings settings = {});

    ~ClientOutput();

    ClientOutput(const ClientOutput&) = delete;
    ClientOutput& operator=(const ClientOutput&) = delete;

    // Where this room's stream begins on the zone's timeline. Section 44's
    // late join is this called with a position the zone is already past, and
    // there is no other kind: a room that starts with the zone joins at the
    // position the zone is at.
    [[nodiscard]] Status<OutputError> joinAt(TimelineNs position);

    // Section 38's stop, from this side: the thread goes, and everything built
    // for a timeline nobody is going to play goes with it. Whole packets that
    // have not been started are dropped; a packet the socket is half-way
    // through is written out first, because half a header is not a packet and
    // the receiver on the other end would refuse the connection rather than
    // the packet.
    //
    // The audio connection is left alone, exactly as the client leaves its
    // device alone (section 38): what stopped is the timeline, not the room.
    // Device recovery passes disconnect=true to drop the old partial packet too.
    void park(bool disconnect = false);

    // A position for a stream that is not producing, which is what a zone that
    // was stopped and played again gives every room of it, and what every
    // trigger of phase 16 gives the one room it moved. The packet grid, the
    // clock's anchor, the converter and the store cursor all move to it; the
    // packetizer's sequence numbering does not, because the client reads a
    // sequence gap on one connection as a server that skipped ahead.
    //
    // The client sees one timeline discontinuity here and rejoins on it --
    // sections 43 and 32 are the same thing on the wire, and there is no
    // HARD_RESYNC message because there does not need to be. That is honest:
    // the audio between where this room stopped and where it begins again is
    // audio nobody played.
    //
    // Accepted on a stream that is parked (section 38) or stranded (the decode
    // ran past its cursor) and refused on one that is still producing:
    // park() is what stops a running one. A stranded room's thread, which
    // never stopped, is stopped here; run() starts the thread again either way.
    [[nodiscard]] Status<OutputError> rejoinAt(TimelineNs position);

    // Section 54's seek (phase 26): the same move, announced. The first packet
    // built from `position` carries `afterFlush`, which is what tells the
    // client that the jump is the one its FLUSH explains -- so it keeps its
    // start mapping and plays `position` where that mapping puts it, instead
    // of rejoining. Everything else is rejoinAt(), preconditions included.
    [[nodiscard]] Status<OutputError> flushTo(TimelineNs position);

    // Section 12's re-negotiation outside any boundary: this room's own device
    // was reopened in another format (the room was moved to another card), so
    // its converter is replaced while the stream is parked or stranded, and
    // rejoinAt() builds the grid, the clock and the store cursor for it. The
    // packetizer carries on in the new format; its numbering does not restart.
    [[nodiscard]] Status<OutputError> reformat(std::unique_ptr<IClientAudioProcessor> processor);

    // --- section 24's format transition --------------------------------------

    // Nothing past `position` is produced until switchTo() names the converter
    // for what follows it.
    //
    // The packet that would have crossed the boundary is cut short so that the
    // last one ends exactly on it: the source before it is played to its end
    // and no media time is lost or repeated, which is the whole of section
    // 24's "sample-accurate on both sides of the gap" from this end. A short
    // packet is what that costs, and the client's own reader takes one -- it
    // reads a packet's span from the packet and never from its frame count.
    //
    // A converter reads past what it emits, so the last packets before the
    // boundary are finished from silence rather than from the source that
    // follows it: the old source has ended there, and the block after it may
    // be one this room's converter cannot read at all.
    //
    // Recorded here and granted by pump() on this room's own thread, which is
    // the shape setDriftTrim() has and for the same reason. A second hold at
    // or before the first replaces it, and one past it is queued behind it --
    // switchTo() then names the converter for that later one. A hold at a
    // position this stream has already produced past is refused, because
    // there is no packet left to cut. A hold exactly where the stream has got
    // to is a stream that produces nothing more, which is what a room that
    // joined past the boundary needs.
    [[nodiscard]] Status<OutputError> holdAt(TimelineNs position);

    // The converter for what follows the hold, and where this room's stream
    // carries on.
    //
    // `position` is the hold itself for a room whose zone held its line at the
    // boundary, and a future position for a room the line ran past while its
    // device was reopening -- section 44's late join, reached through section
    // 24's door. The grid, the clock, the store cursor and the converter all
    // move to it, or to wherever this stream has been moved since if that is
    // further on; the packetizer's sequence numbering does not.
    //
    // Whole queued packets are dropped where the **output** format changed
    // with the converter, because the device that was going to play them has
    // been reopened in another one and pacing has to start again from what
    // this room says it holds. Where it did not change -- a room resampling
    // the new source into the rate it was already on -- they are kept and that
    // room's stream never stops.
    //
    // Refused on a stream that is not held: a converter installed anywhere but
    // on a boundary is the wrong bytes read as the right ones.
    [[nodiscard]] Status<OutputError> switchTo(std::unique_ptr<IClientAudioProcessor> processor,
                                               TimelineNs position);

    // --- a room grouped while it plays ---

    // A new converter for the same output, from the next packet this stream
    // builds, with nothing queued dropped and nothing held: the client is sent
    // the packets it was going to get, read on from exactly the source frame
    // it had reached. That is what lets a room that is joined in its zone be
    // drift corrected from then on without leaving the line.
    //
    // A resampled converter is primed with the source audio just before that
    // frame and what it makes of it discarded, so its first frame is the
    // source's and not a filter starting from silence, which in the middle of
    // a track is a click. Where the store no longer holds that audio, or the
    // decode is not far enough ahead to finish the priming, it starts cold.
    //
    // Refused on a stream that is not streaming, while a section 24 switch is
    // waiting, and for a converter whose output format is not this stream's:
    // a device that would have to be reopened is a re-negotiation. A holdAt()
    // before it is installed drops it, because the boundary plans the room
    // again anyway.
    [[nodiscard]] Status<OutputError>
    replaceProcessor(std::unique_ptr<IClientAudioProcessor> processor);

    // Phase 39: nothing is produced past `ceiling`, which the zone moves on
    // with its line -- one prebuffer and one packet past what this room is
    // playing. A room that has reached it waits, exactly like one whose buffer
    // is full.
    //
    // It is what bounds a room on a slow link. Pacing counts what this process
    // still holds, never what is already in the kernel, on the air or in the
    // client's receive window, so a room whose socket keeps taking bytes while
    // its buffer report stays low would otherwise be built further and further
    // ahead -- and the zone's decode, which follows the room furthest ahead,
    // would evict audio the other rooms have not played yet.
    //
    // Never below one prebuffer and one packet past where this stream began,
    // so a room placed ahead of the line can still fill before the line gets
    // there. Zero, the default, is no ceiling: a stream with no zone behind it.
    void setCeiling(TimelineNs ceiling) noexcept { ceilingNs_.store(std::to_underlying(ceiling)); }

    // Where the zone's decode has ended with nothing staged after it, which
    // the zone moves with its state beside the ceiling. The packet that would
    // cross it is cut short to end on it, as at a hold, and a converter is
    // finished from silence -- so the last frames of the last source are sent
    // instead of waiting at the frontier for a next source that may never
    // come. Moved on or cleared (zero) when the zone has more: the stream then
    // carries on from exactly where it stopped, its converter started afresh
    // there. Zero, the default, is no end.
    void setEnd(TimelineNs end) noexcept { endNs_.store(std::to_underlying(end)); }

    // Whether this room is playing on a running line: started, confirmed by a
    // PLAYBACK_REPORT, and its zone's transport playing. The zone says so
    // each pass, beside the ceiling.
    //
    // A room that is playing drains its buffer whether or not its reports get
    // through, so once they have stopped arriving for kReportGapNs it is paced
    // on the line instead: built up to the ceiling, which is one prebuffer and
    // one packet past what it is playing. Without that, a gap in the reports
    // is a room sent nothing while it plays its buffer empty, and then a room
    // the store has evicted. A paused or unstarted room is never paced this
    // way -- it is not draining, which is what section 18's rule is for.
    void setOnLine(bool playing) noexcept { onLine_.store(playing); }

    // How long a playing room's BUFFER_STATUS may be missing before pacing
    // stops trusting the last one. Four reports at section 46's 250 ms.
    static constexpr uint64_t kReportGapNs = 1000ULL * 1000000ULL;

    // Starts this room's thread. It calls pump() until stop(), the object is
    // destroyed, or the room fails.
    [[nodiscard]] Status<OutputError> run();

    // Stops the thread and waits for it. Idempotent.
    void stop();

    // One pass: take a connection that has attached, drain what is queued,
    // produce while pacing allows, and notice a queue that has been full too
    // long. The thread calls it; a test calls it instead of sleeping.
    void pump();

    // --- the two listeners' threads -----------------------------------------

    // Binds an audio connection to this room (section 43's reconnect is the
    // same call). Whatever was attached is closed, and the packet the old
    // socket was half-way through is written again from its first byte -- the
    // new socket did not get the prefix, and half a header is not a packet.
    void attach(std::unique_ptr<IPacketSink> sink);

    void detach();

    // Section 19, and pacing's input for every room that is not playing on a
    // line whose reports have stopped (setOnLine()). Producing is measured
    // from the moment of a report, so what is still queued here is counted as
    // audio this room is going to have rather than audio it already has.
    void onBufferStatus(const matp::BufferStatus& status);

    // Sections 28 and 30's drift correction, as the ratio the zone's
    // ClientSyncController arrived at: 1.000030 for a room being pulled
    // +30 ppm, and the project's one convention everywhere else.
    //
    // It is a wish rather than an instruction, for the reason the header
    // gives: it is granted on this room's own thread, at the next packet
    // boundary, to the clock and the converter together. A trim arriving for a
    // room that was not planned for a correction is dropped here and nowhere
    // else -- that rule has to hold however many callers there ever are, so it
    // lives with the converter rather than with whoever found the number. Only
    // the newest wish survives: a correction is a position to hold, not a
    // queue to work through.
    void setDriftTrim(double trim);

    // This room's equalizer. Like a trim, a
    // wish granted on this room's own thread at the next packet, newest wins,
    // and it outlives every converter this room is given. Only a resampled
    // plan runs it; a bitperfect room keeps its bytes whatever this says.
    void setEqualizer(const EqualizerSettings& settings);

    // --- what status reports -------------------------------------------------

    [[nodiscard]] OutputState state() const;
    [[nodiscard]] std::string lastError() const;

    // Why this room's audio connection was last let go of, and how often one
    // has been. Neither is a failure: an audio connection that dies or stops
    // taking bytes is the room's socket and not the room (section 43 -- the
    // session is on the control connection), so the stream keeps its position
    // and its queue and resumes on the next one. What it costs is nothing at
    // all when the connection comes back before the decode runs past this
    // room's cursor, and a rejoin when it does not.
    [[nodiscard]] std::string lastDrop() const;
    [[nodiscard]] uint64_t connectionDrops() const noexcept { return connectionDrops_.load(); }

    [[nodiscard]] const SessionId& sessionId() const noexcept { return session_; }
    [[nodiscard]] const ClientId& clientId() const noexcept { return client_; }

    // By value, because section 24's transition replaces it on this room's own
    // thread while the zone reads it on another: a reference into a converter
    // that is being swapped out is a reference to whichever half won.
    [[nodiscard]] OutputPlan plan() const;

    // Whether this room's PCM reaches the packetizer without passing through a
    // converter at all: a bitperfect plan whose two formats are the same
    // layout. The block's bytes are then the payload, and phase 7's direct
    // path -- which for that pair is a byte-for-byte copy -- is not built into
    // the loop at all.
    [[nodiscard]] bool passThrough() const noexcept { return passThrough_.load(); }

    // How far this room's stream has been built on the *zone's* timeline: the
    // end of the last packet produced, or the join position before the first.
    //
    // It is the zone's pipeline that reads it (phase 10), and reading it is
    // what makes a zone's decode demand-driven: the pipeline decodes a
    // lookahead past the room that is furthest ahead, so a room that has
    // stopped draining cannot slow the decode down and a room that has gone
    // away cannot stop it. In the timeline's axis rather than this room's
    // frames, because that is the one axis every room of a zone shares.
    [[nodiscard]] TimelineNs producedThroughNs() const noexcept
    {
        return TimelineNs{producedThroughNs_.load()};
    }

    // Whether a correction may reach this room at all: the plan's flag, which
    // is false for every bitperfect room, for every room whose resampler
    // cannot vary a ratio, and for every room in a house that did not ask for
    // correction. plan().driftDisabledBy says which of those it was.
    // Corrected by either actuator, which is what section 32's hard resync
    // reads; only ServerResampling takes setDriftTrim().
    [[nodiscard]] bool driftCorrected() const noexcept
    {
        return driftActuator_.load() != DriftActuator::None;
    }
    [[nodiscard]] DriftActuator driftActuator() const noexcept { return driftActuator_.load(); }

    // The trim in force: 1.0 for an uncorrected room, and for a corrected one
    // until its first correction arrives.
    [[nodiscard]] double driftTrim() const;

    // Corrections applied, and corrections the converter refused. The second
    // is a configured limit wider than the converter's own trim window rather
    // than a room at fault, which is why it is counted and the room plays on.
    [[nodiscard]] uint64_t corrections() const noexcept { return corrections_.load(); }
    [[nodiscard]] uint64_t correctionsRefused() const noexcept
    {
        return correctionsRefused_.load();
    }

    // Whether the equalizer is changing this room's samples: switched on with
    // a filter or a preamp in it, and a resampled plan to run it on.
    [[nodiscard]] bool equalizing() const noexcept { return equalizing_.load(); }

    // Samples past full scale after the equalizer.
    [[nodiscard]] uint64_t equalizerClippedSamples() const noexcept
    {
        return equalizer_.clippedSamples();
    }

    // Converters section 24's transition, a reopened device or
    // replaceProcessor() has installed on this room's stream.
    [[nodiscard]] uint64_t formatSwitches() const noexcept { return formatSwitches_.load(); }

    [[nodiscard]] uint64_t packets() const noexcept { return packets_.load(); }
    [[nodiscard]] uint64_t frames() const noexcept { return frames_.load(); }
    [[nodiscard]] uint64_t bytes() const noexcept { return bytes_.load(); }

    // Packets built and not yet handed to the socket in full.
    [[nodiscard]] size_t queuedPackets() const;

    // How often the queue reached its limit, and how often the loop had
    // nothing to send because the decoder had not got there yet. Neither is a
    // fault on its own; both rising together is a room that is behind.
    [[nodiscard]] uint64_t queueFullCount() const noexcept { return queueFull_.load(); }
    [[nodiscard]] uint64_t starvedCount() const noexcept { return starved_.load(); }

    // The last figure this room reported, and what has been produced for it
    // since -- the two halves of the pacing rule, so that a room that is not
    // being sent audio can be explained rather than guessed at.
    [[nodiscard]] uint64_t bufferedNs() const;
    [[nodiscard]] uint64_t producedSinceReportNs() const;

    // How long ago the last BUFFER_STATUS arrived, absent before the first;
    // the longest gap between two; and how many gaps a playing room was paced
    // on the line through. A report gap is the first sign of a link going
    // bad, and is visible here long before it becomes an eviction.
    [[nodiscard]] std::optional<uint64_t> bufferReportAgeNs() const;
    [[nodiscard]] uint64_t longestReportGapNs() const;
    [[nodiscard]] uint64_t reportGaps() const noexcept { return reportGaps_.load(); }
    [[nodiscard]] bool pacedOnLine() const;

private:
    ClientOutput(SessionId session, ClientId client, NativePcmStore& store,
                 const SourceTimeline& timeline,
                 std::unique_ptr<IClientAudioProcessor> processor, const IClock& clock,
                 ClientOutputSettings settings);

    struct Packet
    {
        std::string bytes;
        uint64_t durationNs = 0;
        uint32_t frames = 0;
    };

    // One grid step, or false when there is not enough audio for it yet.
    bool produce();

    // Where the next packet begins on the timeline, or false where the grid
    // has run past what 64 bits hold. The correction and the packet it lands
    // in front of ask the same question, and a stream that has run out of
    // timeline is not a place to install a new ratio.
    [[nodiscard]] bool nextPacketStart(uint64_t& startNs) const;

    // A trim recorded by setDriftTrim(), given to the converter and then to
    // the clock. That order is deliberate: the converter is the one that can
    // refuse, and a refusal has to leave both where they were.
    void applyDriftTrim();

    // Settings recorded by setEqualizer(), given to the equalizer.
    void applyEqualizer();

    // Everything about this room's loop that follows from its plan: whether it
    // copies or converts, whether it may be corrected, its nominal ratio, its
    // two frame sizes and the carry ceiling. Read once here rather than out of
    // the plan in the loop, and read again whenever the converter is replaced.
    void adoptPlan();

    // A converter recorded by switchTo(), installed once this room's stream
    // has been built up to the boundary holdAt() named -- the tail of the old
    // source goes out before the first packet of the new one is built.
    void applyFormatSwitch();

    // The grid, the store cursor and the converter moved on to where the last
    // packet was cut by setEnd(), once the end has moved past it. The clock is
    // left alone: it is continuous across the cut, and framesSent_ is its
    // target there.
    void resumeAfterEnd();

    // replaceProcessor()'s priming, on a stream just moved to `at` and with
    // the new converter in place.
    void preRoll(SourceFrame at);

    // `wanted` frames of this room's own PCM, or an empty span when the decode
    // has not reached that far. The span is into the current block where this
    // room is pass-through and the block holds the whole packet, and into
    // packetBuffer_ otherwise; it stays valid until the next call.
    std::span<const std::byte> assemble(uint32_t wanted);

    // Fills packetBuffer_ towards `wanted` frames from the store, through the
    // processor unless this room is pass-through.
    bool fill(uint32_t wanted);

    // Puts the next block behind the frames a converter would not take, for
    // the one case where the store's block boundary is not the converter's:
    // interpolating needs the frame *after* the one it reads, and the last
    // frame of a block does not have one until the next block is behind it.
    // `shown` is how many frames it was given, which is what tells a converter
    // that is merely short of input from one that is broken.
    bool extendCarry(uint32_t shown);

    // The packet grid and this converter's clock begun again at `position`:
    // what joinAt(), rejoinAt() and a format switch start a stream from. Not
    // resumeAfterEnd(), whose clock runs on across the cut.
    void anchorAt(TimelineNs position);

    // Drops the block being read, the carry and the packet being assembled:
    // what a stream moved to another position or converter must not read on
    // from.
    void clearAssembly();

    // Copies `frames` from the current block onto the end of the carry.
    void appendToCarry(uint32_t frames);

    // One packet's worth of silent source frames onto the end of the carry,
    // for a converter that needs input past a boundary the old source ended
    // on (section 24).
    void appendSilenceToCarry();

    // Whether the frontier the last nextBlock() met is a boundary or an end
    // the packet being built was sized for, so silence may finish it. One
    // sized before that hold or end was published would run past it.
    bool silenceMayFinishPacket() const;

    // The block the cursor is in, or false at the frontier. Failures are
    // recorded and end the room.
    bool nextBlock();

    void drain();

    // A queue that has been full too long with a connection to drain it.
    void checkStall();

    // The audio this room's cursor named has been dropped by the store, so
    // there is nothing to produce from and no position to produce it at. Not a
    // failure and not a park: the room is intact and is waiting to be told
    // where it is now (phase 16). Its whole queue goes, for the reason park()
    // drops one -- a packet the sink is half-way through stays.
    //
    // Called from this room's own thread, which is why it cannot join it: the
    // thread keeps running and pump() returns at its first check until
    // rejoinAt() names a position.
    void strandRoom(std::string detail);

    // The audio connection is gone, or has stopped taking bytes for long
    // enough that it may as well be. The socket is closed -- which is what
    // makes the client re-attach -- and this room keeps everything else,
    // because the stream is not on that connection.
    void dropConnection(OutputError code, std::string detail);

    // Whole queued packets, the pacing state behind them and the trim waiting
    // to be installed -- everything this room built for a timeline position it
    // is not going to be at. Called under mutex_, by park() and by
    // strandRoom().
    void dropBuiltAudio();

    void failRoom(OutputError code, std::string detail);

    // --- fixed for the life of the output ------------------------------------

    SessionId session_;
    ClientId client_;
    NativePcmStore& store_;
    const SourceTimeline& timeline_;
    std::unique_ptr<IClientAudioProcessor> processor_;
    const IClock& clock_;
    ClientOutputSettings settings_;

    uint64_t packetDurationNs_ = 0;

    // --- this room's plan, and everything that follows from it ---------------
    //
    // Fixed until section 24's transition replaces the converter, which is
    // done on this room's own thread. The two that another thread reads are
    // atomics for exactly that reason; the rest are this thread's alone.

    std::atomic<bool> passThrough_{false};

    // The plan's drift flag, and the ratio this room's converter runs at
    // before any correction -- outputRate / sourceRate, which is 1.0 for a
    // room whose rate matches the zone's. A trim multiplies it; the converter
    // is never told the trim on its own, because its ratio is the whole
    // conversion (IAudioResampler's one convention).
    std::atomic<bool> driftCorrected_{false}; // by this output's converter
    std::atomic<DriftActuator> driftActuator_{DriftActuator::None};
    double nominalRatio_ = 1.0;

    uint32_t outputBytesPerFrame_ = 0;
    uint32_t sourceBytesPerFrame_ = 0;

    // Twice the source frames one packet spans. A packet is a fixed span of
    // the zone's timeline, so the input it needs is that span at the *source*
    // rate whatever this room's own rate is; twice it is far more than any
    // filter's lookahead and still small enough that a converter which never
    // moves is caught rather than fed forever.
    uint32_t maxCarryFrames_ = 0;

    // --- this room's thread only ---------------------------------------------

    OutputPacketizer packetizer_;
    OutputClock outputClock_{0, TimelineNs{}};

    bool joined_ = false;
    NativePcmStore::ReaderId reader_ = 0;
    TimelineNs originNs_{};
    uint64_t packetIndex_ = 0;
    OutputFrame framesSent_{};

    // The block being consumed and how far into it the cursor is. The store
    // hands over a shared, immutable block and advances its own cursor past
    // it, so what is left of it lives here.
    NativePcmStore::SharedBlock block_;
    uint32_t blockOffsetFrames_ = 0;

    // Source frames carried across a block boundary for a converter that
    // needs more than the tail of one block, and how many of them have been
    // taken. Empty for every pass-through room and for every packet that does
    // not land on a boundary, which is almost all of them.
    std::vector<std::byte> carry_;
    uint32_t carryFrames_ = 0;
    uint32_t carryOffsetFrames_ = 0;

    // Outlives the converters: each one is handed a pointer to it in
    // adoptPlan(), and only a resampled one runs it. Its clip counter is the
    // one member status reads.
    Equalizer equalizer_;
    std::atomic<bool> equalizing_{false};

    // The cursor has reached the boundary a hold names, and was put back on
    // it: what the converter still wants before it is silence (section 24).
    bool atBoundary_ = false;

    // The last read found the frontier at the zone's end (setEnd()), so what
    // a converter still wants is silence. Set by every nextBlock().
    bool atEnd_ = false;

    // The last packet was cut short on the zone's end, and the grid has to be
    // moved to it before anything more is built (resumeAfterEnd()).
    bool cutAtEnd_ = false;

    // The zone's end and the hold as produce() read them when it sized the
    // packet being built. Either can be published between that read and the
    // read that reaches it, and a packet sized before it runs past it: fill()
    // leaves that one to be sized again rather than finishing it from
    // silence.
    uint64_t packetEndNs_ = 0;
    uint64_t packetHoldNs_ = 0;

    // The hold the last nextBlock() put a block back on, beside atBoundary_.
    uint64_t boundaryNs_ = 0;

    // The packet being assembled. Exactly one packet's worth: the loop asks
    // the processor for what is still missing rather than for whatever fits,
    // so nothing is ever produced that this packet cannot carry.
    std::vector<std::byte> packetBuffer_;
    uint32_t packetFrames_ = 0;

    // --- shared ---------------------------------------------------------------

    mutable std::mutex mutex_;
    std::unique_ptr<IPacketSink> sink_;         // guarded by mutex_
    std::deque<Packet> queue_;                  // guarded by mutex_
    size_t writtenOfFront_ = 0;                 // guarded by mutex_
    uint64_t queuedNs_ = 0;                     // guarded by mutex_
    uint64_t bufferedNs_ = 0;                   // guarded by mutex_
    uint64_t producedSinceReportNs_ = 0;        // guarded by mutex_
    std::optional<uint64_t> lastReportNs_;      // guarded by mutex_
    uint64_t longestReportGapNs_ = 0;           // guarded by mutex_
    bool inReportGap_ = false;                  // guarded by mutex_; warned once per gap
    uint64_t queueFullSinceNs_ = 0;             // guarded by mutex_; 0 is "not full"
    double pendingTrim_ = 0.0;                  // guarded by mutex_; 0 is "nothing to apply"
    double trim_ = 1.0;                         // guarded by mutex_; what is in force
    std::optional<EqualizerSettings> pendingEqualizer_; // guarded by mutex_
    OutputState state_ = OutputState::Idle;     // guarded by mutex_
    std::string lastError_;                     // guarded by mutex_
    std::string lastDrop_;                      // guarded by mutex_

    // Phase 40: a room on a bad link is stranded and moved again and again,
    // and each of those is a line.
    log::RateLimit strandLog_;                  // guarded by mutex_
    log::RateLimit moveLog_;                    // guarded by mutex_
    OutputPlan plan_;                           // guarded by mutex_; the converter's, copied

    // Section 24. `holdNs_` is where this stream stops producing and zero is
    // "no boundary ahead" -- a hold at timeline zero would be a stream with
    // nothing in it, and the first source of a zone has no format before it to
    // change from. `pendingProcessor_` is what follows the boundary and where.
    uint64_t holdNs_ = 0;                                          // guarded by mutex_
    std::unique_ptr<IClientAudioProcessor> pendingProcessor_;      // guarded by mutex_
    TimelineNs pendingPosition_{};                                 // guarded by mutex_
    // The pending converter is replaceProcessor()'s: no boundary, the next
    // packet, primed.
    bool pendingInPlace_ = false;                                  // guarded by mutex_
    // Boundaries past `holdNs_`, oldest first, each with what switchTo() named
    // for it. The front becomes `holdNs_` when the switch before it installs.
    struct LaterHold
    {
        uint64_t holdNs = 0;
        std::unique_ptr<IClientAudioProcessor> processor;
        TimelineNs position{};
    };
    std::deque<LaterHold> laterHolds_;                             // guarded by mutex_

    // Written by this room's thread after every packet and read by the zone's
    // pipeline, which is why it is an atomic rather than the packetIndex_
    // above it.
    std::atomic<uint64_t> producedThroughNs_{0};

    // Phase 39's ceiling, written by the zone's pipeline and read here.
    std::atomic<uint64_t> ceilingNs_{0};

    // setEnd()'s, written by the zone and read here.
    std::atomic<uint64_t> endNs_{0};

    // Written by the zone's pipeline beside the ceiling.
    std::atomic<bool> onLine_{false};

    std::atomic<uint64_t> packets_{0};
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> bytes_{0};
    std::atomic<uint64_t> queueFull_{0};
    std::atomic<uint64_t> starved_{0};
    std::atomic<uint64_t> corrections_{0};
    std::atomic<uint64_t> correctionsRefused_{0};
    std::atomic<uint64_t> formatSwitches_{0};
    std::atomic<uint64_t> connectionDrops_{0};
    std::atomic<uint64_t> reportGaps_{0};

    // Last, so that it is stopped before anything it touches is destroyed.
    std::jthread thread_;
};

} // namespace multiroom
