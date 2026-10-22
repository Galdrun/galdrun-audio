#pragma once

// One media timeline, decoded once, sent to every room attached to it.
//
// This is the component the roadmap's criterion 15 is about. A zone owns the
// source list, the decoder reading the current source, the store that holds
// what came out of it, the segment table that says when each frame of it is
// heard, and one ClientOutput per member. Two rooms in different formats are
// two cursors and two processors over **one** decode, and a test proves that
// with a counter rather than with an argument.
//
//     staged segments -> IDecoder -> BlockPool -> NativePcmStore
//                                        |            |      |
//                              SourceTimeline      cursor  cursor
//                                                     |      |
//                                              ClientOutput  ClientOutput
//
// Five decisions carry this class.
//
//   * **The decode is demand-driven, and the demand is the room that is
//     furthest ahead.** The pipeline decodes while the store's frontier is
//     less than one lookahead past the greatest producedThroughNs of the rooms
//     that are streaming, and stops otherwise. Pacing a decoder by a wall
//     clock is the same trap as pacing a sender by one: it keeps producing for
//     rooms that are not consuming. Taking the *fastest* room rather than the
//     slowest is what keeps the isolation rule intact -- a room that has
//     stopped draining, or gone away, can neither slow this zone down nor stop
//     it, and the store evicts it rather than waiting for it.
//
//   * **A zone with no members still decodes its lookahead, once.** A room
//     that joins should find audio rather than a frontier, and the bound is
//     the same lookahead, so an idle zone costs one lookahead of store and
//     then nothing at all.
//
//   * **The timeline starts at zero and is never reset** (section 5). A track
//     boundary adds an entry to the segment table and re-anchors nothing,
//     which is what makes gapless playback a property of the arithmetic.
//     Membership does not touch it either: a room joining or leaving is one
//     cursor appearing or disappearing.
//
//   * **A room is planned twice, and the second answer is the one it gets.**
//     planFor() is what SESSION_CONFIG announces, from this zone's source
//     format and that device's whole capability list; admit() plans again
//     against the format the driver actually granted (section 12), because
//     that is now the only format the room can be sent. A room whose plan
//     cannot be made is that room's failure, and the zone keeps playing.
//
//   * **A source in another format is a transition, and it is per room.**
//     Every room's converter was built for one source format, so a source in
//     another is re-planned room by room before a block of it is published
//     (section 24, phase 15). A room whose device keeps its format -- a room
//     resampling into a fixed rate -- gets the new converter at the boundary
//     and plays straight through it. A room whose device has to be reopened
//     is held at the boundary, paused there, asked to re-negotiate when the
//     line reaches it, and placed on the line again from the boundary once its
//     device answers. The line waits for those rooms only when no room is left
//     playing through: two rooms on one timeline, one silent while its device
//     reopens and the other not, is the correct outcome and not a bug.
//
// Phase 11 adds a sixth, and it is the one that makes a zone audible: **the
// zone decides when, and StartScheduler decides who.** Decoding, sending and
// buffering are all this class already did; what section 20 adds is a barrier
// before the first START_AT and a running line afterwards. The scheduler holds
// that state and no zone at all, so every rule of it is testable without a
// decoder; this class is what feeds it, what turns its answers into messages
// through `ZoneSettings::sendCommand`, and what places a late joiner's stream
// far enough ahead of the playhead to have somewhere to join (section 44).
//
// Phase 14 adds a seventh, and it is the one that costs the least code:
// **a pause is the absence of an instruction.** Sections 37 and 38 name a
// position and the rooms stop draining at it; phase 9's pacing rule then stops
// producing for them, and this class's demand-driven decode stops behind that.
// So `pausePlayback()` sends a message and freezes a playhead, and touches no
// queue, no cursor and no decoder -- the audio the resume is made of is the
// audio that was already there. A **stop** is the one that costs something:
// what every room holds is dropped on both sides, so each room's stream is
// parked and begins again at the first position of whatever `play()` opens
// next. The timeline is still never reset (section 5), which is why the
// restart is a rejoin for the rooms rather than a new zone.
//
// Track metadata is the other half of the phase and changes nothing at all
// about playback: a boundary is announced when a source is opened, and a room
// that joined mid-track is told what is playing when it is admitted. That
// *is* gapless playback -- the packets carry on across a same-format boundary
// because nothing here asks them not to.
//
// Phase 16 adds an eighth, and it is the one that makes a room's bad luck
// survivable: **a connection is not a room, and every step is one move.** A
// control connection that goes takes the stream, the session and the place in
// the barrier with it -- and what belongs to the room rather than to the
// socket is kept against its return, so a reconnect is the ordinary admit()
// path with a measured card put back under it instead of a fresh convergence.
// Everything that moves a room -- a store eviction, a line that has run past
// it, a device that was reopened, an offset no ratio closes, and a caller that
// asks -- ends in one rejoin(): a future position, the stream moved there, and
// the barrier placing it again from what it reports about the audio it holds
// now. No other room is touched, and the log line says which trigger it was,
// because "it resynced" and "it was drifting" are two different conversations
// with an operator.
//
// **Threading.** The pipeline is one thread calling pump(); a test calls
// pump() itself and never sleeps. stage(), planFor(), admit(), release(),
// attach() and onBufferStatus() come from the two listeners' threads and from
// whatever owns the zone, and hold the mutex for a map lookup or a shared_ptr
// copy. Every call that stops a room's thread does it outside the mutex,
// because joining a thread while holding a lock that thread's owner may want
// is how a shutdown deadlocks.

#include "audio/BlockPool.h"
#include "audio/NativePcmStore.h"
#include "client/ClientOutput.h"
#include "client/ClientSyncController.h"
#include "client/OutputPlan.h"
#include "client/OutputPlanner.h"
#include "core/Error.h"
#include "core/Ids.h"
#include "core/Units.h"
#include "decode/DecoderFactory.h"
#include "matp/AudioServer.h"
#include "matp/Protocol.h"
#include "matp/RoomCommand.h"
#include "processing/IClientAudioProcessor.h"
#include "resampler/ResamplerRegistry.h"
#include "timeline/SourceTimeline.h"
#include "util/Clock.h"
#include "util/Logger.h"
#include "zone/SourceSegment.h"
#include "zone/StartScheduler.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace multiroom {

enum class ZoneError
{
    InvalidSettings, // a zero duration, or a lookahead the retention cannot cover
    InvalidSegment,  // a segment with no sourceId or no mediaUri
    NotPlaying,      // a call that needs a source, on a zone that has none
    AlreadyPlaying,
    NothingStaged, // play() with an empty source list

    SourceFailed,  // the decoder or its source refused; the detail is theirs
    StoreFailed,   // the store refused a block, or the memory limit a source's
                   // format needs (phase 42); the detail names both
    TimelineFailed,

    PlanFailed,   // this room's plan; the zone keeps playing for the others
    OutputFailed, // this room's output could not be created or started

    AlreadyMember,
    NotMember,
    AlreadyExists, // a zone id the manager already holds
    Full,          // the zone or the member ceiling; the detail says which

    NotStarted,      // a pause, a resume or a stop on a zone that is not there yet
    InvalidCommand,  // a message that would ask a room for nothing

    // Phase 26: a seek this source or this moment cannot take -- a source the
    // media endpoint contract calls non-seekable, one whose successor is
    // already being decoded, or a room held at a format boundary -- and a
    // position that is not in the source at all.
    NotSeekable,
    InvalidPosition
};

const char* errorName(ZoneError error);

enum class ZoneState
{
    Idle,    // created, or stopped; no decoder and no thread
    Playing, // a source is open and the pipeline is decoding it
    Ended,   // the last staged source was decoded to its end; nothing follows
    Failed   // the store or the timeline refused; lastError() says why. A
             // source that fails is skipped instead (phase 22)
};

const char* zoneStateName(ZoneState state);

// Why one room's stream was moved to another timeline position (phase 16).
//
// Every one of them is a **step**: something put this room somewhere other
// than where the timeline says it is, and put it there at once. Accumulated
// drift is deliberately not among them and never reaches this enum -- a room
// that is allowed to drift is a room nobody is correcting, and phase 13 leaves
// it alone rather than interrupting it every quarter of an hour.
//
// The name is here so that a log line says which one it was: "it resynced" and
// "it was drifting" are two different conversations with an operator, and a
// bitperfect room that rejoins after a cut must never be mistaken for one that
// was resynced for drift, because that one never is.
enum class RejoinTrigger
{
    Reconnect,      // its control connection came back under a new session
    Evicted,        // the decode ran past its cursor while it was not draining
    DeviceRecovery, // STREAM_READY again: its device was reopened and its ring is empty
    LeftBehind,     // the line has run past every position this room still holds
    HardResync      // section 32: an offset no ratio is going to close, or one asked for
};

const char* rejoinTriggerName(RejoinTrigger trigger);

// What a playing room's plan needed when a setting it is planned from changed
//: nothing, a new converter on the stream
// it already has, or a re-negotiation -- the device would have to change, or
// the stream is not running to take a converter.
enum class Replan
{
    Unchanged,
    InPlace,
    Renegotiate
};

// A staged source this zone could not play (phase 22): it could not be
// opened, or it failed part-way through after its own retries. **A broken
// fetch is not a broken zone** -- the source ends where it failed, the next
// staged one is opened at that boundary exactly as if it had ended there, and
// the zone ends normally if nothing is staged. What is kept is the high-end
// server's name for it and the reason, which is the HTTP status, `Timeout` or
// the decoder's error, because "skip it" and "fix the endpoint" are two
// different answers to one failure.
struct FailedSource
{
    std::string sourceId;
    std::string reason;
};

// Something the high-end server would otherwise have to poll `status` for
// (phase 24). The zone **announces and never asks**: `SourceNeeded` says the
// staged list is empty behind the source that has just been opened, and what
// comes of it is an ordinary `stage()` -- or nothing, and the zone ends when
// its sources do.
//
// A kind beside the fields every kind reads, rather than a variant, for the
// reason RoomCommand is one: several kinds carry the same two or three
// fields. Which fields a kind fills is stated beside each one; the rest keep
// their defaults. The wire spelling belongs to the control API.
enum class ZoneEventKind
{
    SourceNeeded,  // a source was opened and nothing is staged after it: `sourceId`
                   // is that source, `remainingNs` its length not yet decoded
                   // (absent for a stream of unknown length). Again when
                   // clearStaged() empties the list behind a source still playing
    SourceStarted, // `sourceId`'s first frame is audible on the line: `timelineNs`
                   // is where it begins, `serverTimeNs` the instant it was heard,
                   // `startOffsetNs` how far into the source that frame is. Again
                   // for a source that was seeked (phase 26)
    SourceFailed,  // `sourceId` was skipped (phase 22) for `reason`
    Ended,         // everything after `sourceId` has been played out and nothing
                   // was staged in time
    Transport,     // the line is now `transport`, standing at or starting from
                   // `timelineNs` -- including a change nobody asked for
    RoomNotReady,  // `client` is a member and silent: it could not be served
                   // after a format boundary, for `reason`
    RoomFellBehind, // `client` was moved to `timelineNs` by `trigger`; not while
                    // it is unstable, when RoomUnstable stands for every move
    RoomLeftBehind, // `client` answered none of its starts; `reason` is the
                    // scheduler's own detail
    RoomUnstable,   // `client`'s third `Evicted` or `LeftBehind` move inside 30 s
                    // (phase 41): `trigger` is the latest one's, `rejoins` the
                    // moves of this episode, `backoffNs` the wait before the next,
                    // `reason` the last strand's text
    RoomStable      // `client` has streamed 30 s since its last move: `rejoins`
                    // the moves of the episode, `unstableForNs` how long it lasted
};

struct ZoneEvent
{
    ZoneEventKind kind = ZoneEventKind::SourceNeeded;
    ZoneId zone;
    ClientId client;
    std::string sourceId;
    std::string trackId;
    std::string reason;
    std::optional<uint64_t> remainingNs;
    TimelineNs timelineNs{};
    uint64_t serverTimeNs = 0;
    TransportState transport = TransportState::Stopped;
    RejoinTrigger trigger = RejoinTrigger::HardResync;
    uint64_t startOffsetNs = 0;
    uint64_t rejoins = 0;
    uint64_t backoffNs = 0;
    uint64_t unstableForNs = 0;
};

// Phase 41: whether one room keeps falling behind, for `status`. A high-end
// server that reconnects reads the episode from here rather than from the
// events it missed.
struct RoomStability
{
    bool unstable = false;
    uint64_t rejoins = 0;         // this episode's moves; zero when stable
    uint64_t backoffNs = 0;       // the wait before its next move may be made
    uint64_t unstableSinceNs = 0; // zero when stable
};

struct ZoneSettings
{
    // What every room of this zone is sent. One packet duration and one
    // prebuffer target for the whole house -- a duration and never a frame
    // count, because 20 ms is 882 frames at 44.1 kHz and 1920 at 96 kHz.
    ClientOutputSettings output;

    // The store's retention. It has to cover the lookahead below with room
    // to spare: a room reading the oldest block this zone holds must not have
    // it dropped out from under it, and section 46's five seconds against a
    // 1500 ms prebuffer is that margin. It is also what this zone's memory is
    // sized by (phase 42): its retention in blocks of the format it plays.
    uint64_t retentionNs = NativePcmStore::kDefaultRetentionNs;

    // How far past the room that is furthest ahead this zone decodes. Zero
    // means the prebuffer target plus one packet, worked out from `output`: a
    // room is allowed to produce until it holds the target, so anything less
    // would starve a room that is doing exactly what it was told to.
    uint64_t lookaheadNs = 0;

    // Bounded memory, from the membership side. Sixteen rooms is well past the
    // ten the roadmap's threading section sizes for, and each one is a thread
    // and a bounded queue.
    size_t maxMembers = 16;

    // The Audio Control of every room of this zone that has none of its own
    // (see `roomPreference` below).
    AudioControl audioControl = AudioControl::Auto;

    // Phase 17: the settings of *one* room, which the control API stores per
    // client and which follow that client from zone to zone. Empty means every
    // room gets the Audio Control above, ungrouped. Asked each time a room is
    // planned, so a room whose settings changed is planned with the new ones
    // -- and never under this zone's lock, because the answer comes from
    // another module's.
    std::function<RoomAudioSettings(const ClientId&)> roomPreference;

    // One room's equalizer, asked when the room is admitted so that it is in
    // place before the room's first packet. Empty means no room has one. A
    // change afterwards goes to the room's ClientOutput directly.
    std::function<EqualizerSettings(const ClientId&)> roomEqualizer;

    // Phase 7's narrowing dither: off unless a deployment asks, and applying
    // to the resampled path only.
    DitherMode dither = DitherMode::No;

    // Section 20's barrier, timeout and start lead. What they are and why
    // each one exists is in StartScheduler.h.
    StartSchedulerSettings start;

    // Sections 25 to 31, per room: the drift regression window, the offset
    // controller and the two correction limits. What they are and why is in
    // ClientSyncController.h. Phase 12 measures with them and corrects with
    // nothing.
    ClientSyncSettings sync;

    // How far ahead of what is audible a room joining a zone that is *already
    // playing* has its stream begin (section 44). Zero derives it, and the
    // derivation is the one relation it has to the settings above it: the
    // decode runs one lookahead past the room that is furthest ahead, a
    // latecomer is by construction that room, and it produces its whole
    // prebuffer target before its pacing stops it. So `lead + prebuffer +
    // lookahead` has to fit inside the retention, measured from what is
    // audible rather than from a room that happens to be full -- a playing
    // room is allowed to hold anywhere between nothing and the target, and a
    // derivation that assumed the target was the first thing the two-room
    // milestone disproved. It is therefore also the window a slow device has
    // to open in, and a deployment that wants a longer one raises
    // `retentionNs`.
    uint64_t lateJoinLeadNs = 0;

    // Phase 26's seek on a running line: the cut is `seekFlushLeadNs` past
    // what is audible, which covers a FLUSH reaching every room, and the
    // seeked audio is heard `seekFillLeadNs` after the cut, which covers its
    // first packets arriving before they are due. The two are the silence a
    // seek costs, and both are starting values the milestone is to replace.
    uint64_t seekFlushLeadNs = 100ULL * 1000000ULL;
    uint64_t seekFillLeadNs = 150ULL * 1000000ULL;

    // Where a message for one room goes: section 20's start, sections 37 and
    // 38's pause and stop, section 39's volume, and the two metadata messages
    // of sections 23 and 44. This zone knows which room, which position and
    // which instant; putting the message on that room's control connection
    // belongs to whatever owns the sessions, which is ControlServer and its
    // one thread. Required: a zone that cannot start a room is a zone that
    // cannot play, and finding that out at the barrier is five seconds too
    // late.
    //
    // **Called from whichever thread decided.** That is the pipeline thread
    // for a start and a track boundary, and the caller's for a pause, a stop,
    // a volume or a late joiner's metadata -- never under this zone's own
    // lock, because the other end of it is another thread's mailbox. False is
    // a room that could not be reached, which for a start leaves it unstarted
    // until the scheduler offers it another.
    std::function<bool(const RoomCommand&)> sendCommand;

    // Where this zone's events go (phase 24). Optional: empty is nobody
    // listening. Called from whichever thread noticed -- the pipeline's for
    // what the line and the decode do, the caller's for a transport change or
    // a cleared list -- and **never under this zone's locks**, so the other
    // end must hand the event on and return, as a mailbox does. A sink that
    // blocked would be a zone waiting on whoever reads its events.
    std::function<void(const ZoneEvent&)> onEvent;
};

// The part of ZoneSettings the control API changes on a running zone
// (audio.configure): the barrier, the leads of every transport message and of
// a seek. Nothing here sizes memory, which is why it can change live.
struct ZoneTiming
{
    uint64_t minimumStartBufferNs = 0;
    uint64_t readyTimeoutNs = 0;
    uint64_t startDelayNs = 0;
    uint64_t seekFlushLeadNs = 0;
    uint64_t seekFillLeadNs = 0;
};

// Where a seek put the line (phase 26): the source that was re-opened, the cut
// every room on the line was told to flush at, and where the seeked audio is
// heard. The two positions are equal on a paused line.
struct SeekPlacement
{
    std::string sourceId;
    TimelineNs cutAt{};
    TimelineNs resumeAt{};
};

class Zone
{
public:
    // `memory` is the process's one audio ceiling, which this zone's blocks
    // are counted against; it, the planner, the registry, the decoder factory
    // and the clock all outlive this. What the zone owns alone -- its blocks,
    // the store, the segment table, the decoder and its members -- is created
    // here, and creating it allocates nothing (phase 42).
    [[nodiscard]] static Result<std::unique_ptr<Zone>, ZoneError>
    create(ZoneId id, AudioMemory& memory, const OutputPlanner& planner,
           const ResamplerRegistry& registry, IDecoderFactory& decoders, const IClock& clock,
           ZoneSettings settings = {});

    // Stops every room's thread, then the pipeline's, then releases the
    // decoder: the roadmap's shutdown order for a zone, in one place.
    ~Zone();

    Zone(const Zone&) = delete;
    Zone& operator=(const Zone&) = delete;

    [[nodiscard]] const ZoneId& id() const noexcept { return id_; }

    // --- the source list ----------------------------------------------------

    // Appends a segment to play after everything already staged. Staging
    // several ahead is what gapless playback needs; staging none and calling
    // play() is NothingStaged rather than a zone that silently does nothing.
    [[nodiscard]] Status<ZoneError> stage(SourceSegment segment);

    [[nodiscard]] size_t stagedCount() const;

    // What is staged and not yet opened, in the order it will be. The source
    // being decoded is not among them: it is `sources().back()`.
    [[nodiscard]] std::vector<SourceSegment> staged() const;

    // Forgets everything staged and not yet opened, and says how much that
    // was. The source being decoded plays on to its end; what follows it is
    // the high-end server's to stage again.
    size_t clearStaged();

    // What this zone has opened so far, oldest first. The source being decoded
    // is the last entry and its `ended` is false while it is.
    [[nodiscard]] std::vector<StagedSource> sources() const;

    // --- playing ------------------------------------------------------------

    // Opens the first staged source. Synchronous and on the caller's thread on
    // purpose: a URI that cannot be played is a failure where the zone is
    // being configured, and the source format has to be known before any room
    // can be planned against it.
    //
    // A staged source that cannot be opened is skipped for the next one, as
    // it would be at any boundary (phase 22). Only when none of them opens is
    // it SourceFailed, naming the last, and the zone is left Idle -- stage
    // another and play again.
    [[nodiscard]] Status<ZoneError> play();

    // Starts the pipeline thread, which calls pump() until stop(). Either
    // run() it or drive pump() yourself -- a caller that does both is two
    // threads in one decode loop. ClientOutput draws the same line for the
    // same reason.
    [[nodiscard]] Status<ZoneError> run();

    // Stops the pipeline thread and every room's output. Idempotent, and the
    // zone stays where it stopped: nothing here rewinds. This is the zone's
    // *lifetime* ending, not the transport's -- pausePlayback() and
    // stopPlayback() below are sections 37 and 38.
    void stop();

    // --- the transport (sections 37 to 39) -----------------------------------

    // Section 37. Every room on the line is told to pause at one position, a
    // start delay ahead of what is audible now, and the playhead freezes
    // there. What each room holds it keeps, which is what makes the resume
    // seamless and is why nothing here touches a store cursor or a queue: a
    // room that has stopped draining stops being sent audio by phase 9's
    // pacing rule and by nothing else.
    //
    // The position it chose is the answer, so that a caller can say where the
    // zone stopped rather than ask afterwards.
    [[nodiscard]] Result<TimelineNs, ZoneError> pausePlayback();

    // Section 37's resume: a new START_AT at exactly the position the pause
    // named. It goes through the barrier like any other start, so a room whose
    // device closed while the zone was paused holds it exactly as it would
    // have held the first one.
    [[nodiscard]] Status<ZoneError> resumePlayback();

    // Section 38. Every room on the line is told to stop, and this time what
    // it holds is dropped on both sides: each room's own queue goes with it
    // and its stream is parked, because audio built for a timeline nobody is
    // going to play is audio to throw away rather than to send.
    //
    // The zone is left where play() can restart it: the decoder is released,
    // the source that was playing is marked ended, and the timeline is *not*
    // reset (section 5) -- the next source continues after everything this one
    // decoded, and every parked room begins again at its first position.
    //
    // **It stops the pipeline thread**, because the decoder it releases is
    // that thread's, so it must not be called from that thread -- as with
    // stop(), and for the same reason. run() starts it again after the next
    // play().
    //
    // A zone that is decoding but whose line never started -- no room has
    // been placed on it yet -- is stopped the same way with nothing sent,
    // because there is no room on a line to tell. Only a zone with neither a
    // line nor a source is NotStarted.
    [[nodiscard]] Result<TimelineNs, ZoneError> stopPlayback();

    // Section 39, forwarded to one room and never applied here. The server
    // does not touch a sample for volume: the gain belongs to that client's
    // own render path, and applying it in the stream would end bitperfect
    // transport for a room that only wanted to be quieter.
    [[nodiscard]] Status<ZoneError> setVolume(const ClientId& client,
                                              const matp::SetVolume& request);

    // Section 54 (phase 26): the source that is audible now, heard again from
    // `positionNs` into it, without stopping the line.
    //
    // **Open first, then cut.** The source is re-opened at the offset before
    // anything is sent, on the caller's thread, so a source that will not
    // open, cannot seek or has no such position is an answer and the zone
    // plays on as though nothing was asked. Only then is every room on the
    // line sent a FLUSH at the cut and its stream moved to the resume
    // position, its first packet there carrying `afterFlush`: no START_AT, no
    // barrier, and the line never stops. A room that is not on the line yet
    // is moved there the ordinary way and placed by the barrier.
    //
    // The timeline only moves forward: the seeked source is a new segment
    // heard from the resume position, and what the old decode had made past
    // the cut is read by nobody and ages out of the store. Whatever is staged
    // behind the source stays staged and follows it.
    //
    // On a paused line the cut and the resume position are the pause position,
    // and the ordinary resume starts there.
    [[nodiscard]] Result<SeekPlacement, ZoneError> seek(uint64_t positionNs);

    // One pass of the pipeline: resolve whatever the start barrier can decide,
    // then open whatever has to be opened and decode while the lookahead is
    // not covered. The thread calls it; a test calls it instead of sleeping.
    //
    // The scheduling half runs whatever state the zone is in -- a zone that
    // has decoded its last source still has rooms draining what it made, and
    // a room that becomes ready then still has a timeline to be put on.
    void pump();

    // --- what status reports ------------------------------------------------

    [[nodiscard]] ZoneState state() const;
    [[nodiscard]] std::string lastError() const;

    // The sources this zone skipped, oldest first. Bounded: a zone skipping
    // source after source keeps the most recent, because the high-end server
    // reads this by polling status and an unbounded list is a leak with a
    // reason attached.
    static constexpr size_t kMaxFailedSources = 16;
    [[nodiscard]] std::vector<FailedSource> failedSources() const;

    // The format every room of this zone is planned against. Valid once a
    // source has been opened.
    [[nodiscard]] AudioFormat sourceFormat() const;

    // How far the decode has reached on the timeline, and the oldest position
    // the store still holds: the two ends of what a room can be sent.
    [[nodiscard]] TimelineNs frontierNs() const;
    [[nodiscard]] TimelineNs oldestNs() const;

    // Blocks published. One decode serving two rooms is this number not moving
    // faster when a second room is admitted, which is how criterion 15 is
    // proved rather than argued.
    [[nodiscard]] uint64_t publishedBlocks() const noexcept { return published_.load(); }

    // Phase 42: the blocks this zone has allocated, and their bytes. Zero for
    // a zone that is not playing; a paused zone keeps its own, because the
    // resume plays them.
    [[nodiscard]] size_t blocks() const { return pool_->blockCount(); }
    [[nodiscard]] uint64_t memoryBytes() const { return pool_->totalBytes(); }

    // How often the decode found every one of this zone's blocks out: a room
    // still holding one the store had let go of. Waited out, and counted so
    // that status reports it instead of a guess.
    [[nodiscard]] uint64_t blocksRefused() const { return pool_->exhaustedCount(); }

    // Section 20's start, from the outside.
    //
    // `started()` is the first START_AT of this session having gone out, which
    // is when the barrier stops applying and the timeline starts running;
    // `playheadNs()` is what is audible now, and is zero-and-meaningless until
    // it does. `startTimelineNs()` / `startServerTimeNs()` are the pair the
    // first start named, which is the line every later one is derived from.
    [[nodiscard]] bool started() const;
    [[nodiscard]] TimelineNs playheadNs() const;
    [[nodiscard]] TimelineNs startTimelineNs() const;
    [[nodiscard]] uint64_t startServerTimeNs() const;

    // What the line itself is doing, which is a different question from what
    // `state()` above answers: a zone can be Playing -- decoding and sending
    // -- with its transport Paused, and that is exactly what a pause is.
    [[nodiscard]] TransportState transport() const;

    // Where one room is in that sequence, and why it is there. A room that was
    // left behind says so, with the reason an operator needs.
    [[nodiscard]] RoomStartState roomStart(const ClientId& client) const;
    [[nodiscard]] std::string roomStartDetail(const ClientId& client) const;

    // Sections 26 to 31, for one room: its measured drift, its offset from
    // this zone's timeline, and the correction phase 12 computes and does not
    // apply. A default-constructed status is a room this zone does not hold.
    [[nodiscard]] ClientSyncStatus syncStatus(const ClientId& client) const;

    // START_ATs this zone handed to the sender, and rooms it gave up on.
    [[nodiscard]] uint64_t startsSent() const;
    [[nodiscard]] uint64_t leftBehindCount() const;

    // --- membership ---------------------------------------------------------

    // What SESSION_CONFIG announces for this room: the planner's answer from
    // this zone's source format and that device's whole capability list.
    //
    // Not const, and phase 11 is why: a room this zone has announced a format
    // to is a room its first start waits for (section 20), so the answer is
    // remembered as an expectation with the ready timeout on it. Nothing else
    // is created -- a room that never completes its handshake costs an entry
    // in a map until that timeout, and then nothing.
    [[nodiscard]] Result<OutputPlan, ZoneError> planFor(const ClientId& client,
                                                        const ClientCapabilities& capabilities);

    // Adds a room and starts its output.
    //
    // Where that output begins depends on whether this zone is already
    // playing. Before the first START_AT there is no playhead, so it begins
    // where this zone's audio still begins; afterwards it begins one
    // `lateJoinLeadNs` past what is audible now, which is section 44's "a
    // future timeline position" made into a number -- the room prebuffers
    // audio that has not been heard yet and is started the instant the
    // timeline reaches it, without the rooms that are playing being held.
    //
    // `grantedFormat` is what the device opened (section 12), which is not
    // always what was asked for; the plan is made again against it, because
    // that is now the only format this room can be sent.
    //
    // Membership never restarts the zone: this is one cursor appearing.
    [[nodiscard]] Status<ZoneError> admit(const SessionId& session, const ClientId& client,
                                          const ClientCapabilities& capabilities,
                                          const AudioFormat& grantedFormat);

    // Stops that room's output and nothing else. This is a room being taken
    // out of this zone -- it is not coming back, and nothing about it is kept.
    [[nodiscard]] Status<ZoneError> release(const ClientId& client);

    // Its control connection went away (section 43). The stream stops and the
    // room leaves the membership, because there is nothing to send it and no
    // session to send it under -- but what belongs to the *room* rather than
    // to the socket is kept against its return: its sync controller, with what
    // its card was measured to be doing and the correction that was in force,
    // and how often it has been moved. A reconnect is then the ordinary
    // admit() path with that state restored, which is what stops every
    // reconnect costing a fresh convergence.
    //
    // Bounded like everything else here: at most `maxMembers` rooms are kept,
    // and the room that has been away longest is forgotten first.
    //
    // With `session`, only that session's membership ends: a displaced
    // connection closing after its successor was admitted is not the room
    // going away. False when another session holds the room.
    bool disconnect(const ClientId& client,
                    const std::optional<SessionId>& session = std::nullopt);

    // Sections 32 and 43: move one room's stream to a position the timeline
    // has not reached yet, and have it placed on the line again from there.
    // What the client sees is a discontinuity in `timelineStartNs` followed by
    // a START_AT, which is the only mechanism either section needs and the
    // reason there is no HARD_RESYNC message.
    //
    // Public because a caller may force one -- phase 25's `client.rejoin` --
    // and because it is the one path every trigger takes: an eviction, a room
    // the line ran past, a device that was reopened and an offset no ratio can
    // close all end here. The answer is the position the room was moved to.
    //
    // No other room of this zone is touched, and this zone's decode is not
    // either. A room that cannot be moved is that room's failure and the zone
    // plays on. A room held at a format boundary is not moved: its stream is
    // already standing still, and the device answering section 24's
    // re-negotiation is what places it again.
    // A `processor` replaces the room's converter while it is parked for the
    // move: a device reopened in another format.
    [[nodiscard]] Result<TimelineNs, ZoneError> rejoin(
        const ClientId& client, RejoinTrigger trigger,
        std::unique_ptr<IClientAudioProcessor> processor = nullptr);

    // Phase 27: this room is heard `latencyNs` after its card plays, through
    // a receiver or speakers with their own DSP. Its START_AT is moved that
    // much earlier and its sync controller holds its offset there instead of
    // at zero, so it plays ahead of the line by exactly what the room adds
    // back. No sample changes, so a bitperfect room stays bitperfect.
    //
    // A room that is scheduled or playing is moved once with `HardResync` and
    // placed again with its new start, because a bitperfect room cannot slew
    // and tens of milliseconds is a step; any other room takes the figure at
    // its next start. True when the room was moved. The ceiling is the
    // caller's: the room plays that far ahead out of the audio it holds, so
    // the control API bounds it by the prebuffer.
    [[nodiscard]] Result<bool, ZoneError> setOutputLatency(const ClientId& client,
                                                           uint64_t latencyNs);

    // This room's settings changed -- it was grouped, or left alone -- while
    // it is a member. It is planned again against the one format its stream
    // is already in, because that is what lets its device and its place on
    // the line stay as they are, and where that plan serves it differently
    // the new converter is installed on the running stream
    // (ClientOutput::replaceProcessor). A room held at a format boundary is
    // Unchanged: the device it is reopening is planned from the new settings
    // when it answers. Renegotiate is the caller's to do. A room replanned in
    // place while it plays the line alone has the line moved onto it.
    [[nodiscard]] Replan replanInPlace(const ClientId& client);

    // The control API's audio.configure, for every room from now on.
    void setTiming(const ZoneTiming& timing);

    // How often one room has been moved, and how often any room of this zone
    // has. The per-room figure survives a disconnect, because a room that has
    // been rejoined four times is telling an operator something whether or not
    // its socket is up.
    [[nodiscard]] uint64_t rejoins(const ClientId& client) const;
    [[nodiscard]] uint64_t rejoinCount() const noexcept { return rejoins_.load(); }

    // Phase 41: a room this zone does not hold is stable.
    [[nodiscard]] RoomStability stability(const ClientId& client) const;

    // Phase 41's back-off: the first `Evicted` or `LeftBehind` move is made at
    // once, the next after 1 s, then 2 s and 4 s, and 8 s from then on while
    // they keep coming. Thirty seconds of streaming without a move ends both
    // the back-off and an unstable episode; three such moves inside thirty
    // seconds begin one.
    static constexpr uint64_t kFirstBackoffNs = 1ULL * kNanosecondsPerSecond;
    static constexpr uint64_t kMaxBackoffNs = 8ULL * kNanosecondsPerSecond;
    static constexpr uint64_t kSettledNs = 30ULL * kNanosecondsPerSecond;
    static constexpr size_t kUnstableMoves = 3;

    // Whether this zone still holds what a room left behind when its
    // connection went (phase 16) -- which is what makes its next connection a
    // return rather than a stranger arriving (phase 24's `reconnect`).
    [[nodiscard]] bool retains(const ClientId& client) const;

    // One room's output, for status and for the tests. A shared_ptr, because a
    // caller holding a raw one would be racing release().
    [[nodiscard]] std::shared_ptr<ClientOutput> member(const ClientId& client) const;

    [[nodiscard]] std::vector<ClientId> memberIds() const;
    [[nodiscard]] size_t memberCount() const;

    // --- the two listeners' threads -----------------------------------------

    // The audio connection of section 3, once STREAM_ATTACH has named a room.
    // False when this zone does not hold that room, or holds it under another
    // session -- an attachment for a session that has already been torn down
    // is a socket to close, not a room to feed.
    bool attach(const StreamAttachment& attachment, std::unique_ptr<IPacketSink> sink);

    // What attach() would answer, without taking anything: the audio
    // listener asks before it hands a connection over (phase 35).
    [[nodiscard]] bool holds(const StreamAttachment& attachment) const;

    // The room is about to be sent PREPARE_STREAM, which has its client close
    // the audio connection and open another: the old one is let go now, so
    // nothing built for the reopened device is written into it and lost. The
    // stream resumes on the next attach(). No room of this zone, nothing.
    void releaseAudioConnection(const ClientId& client);

    // Section 19, four times a second per room. It is the only input this
    // room's pacing has, and half of what decides whether it can be
    // scheduled -- so the completed clock exchanges of section 8 are read at
    // the same moment and from the same connection, because the client
    // refuses a START_AT it has no mapping for and tells nobody.
    void onBufferStatus(const ClientId& client, const matp::BufferStatus& status,
                        uint64_t clockExchanges);

    // Section 12, both answers, for a room that is already a member: a device
    // that stopped being ready is dropped from scheduling and keeps
    // everything else (section 20's table), and one that reports ready again
    // is scheduled again.
    //
    // For a room this zone asked to re-negotiate at a format boundary, ready
    // is section 24 step 5: the room is planned against what its driver
    // `granted` and its stream carries on from the boundary in that format.
    // An invalid `granted` is the format this zone asked for.
    //
    // For any other room, a device reopened in a format its plan does not
    // output -- the room was moved to another card -- is planned again
    // against `offered` (what that device can do; null keeps what the room
    // was admitted with) and `granted`, and moved with the new converter.
    void onStreamReady(const ClientId& client, bool ready, const AudioFormat& granted = {},
                       const ClientCapabilities* offered = nullptr);

    // Section 25, twice a second per room, and the one thing the report
    // cannot be read without.
    //
    // Phase 11 reads a single field of it -- an audible timeline position is
    // this room confirming that its START_AT took, and it is the only
    // acknowledgement the protocol has. Phase 12 reads the rest: the device's
    // frame position against server time is that card's drift, and what was
    // audible against what this zone's line says should have been audible then
    // is that room's offset. Neither is computable without `clock`, which is
    // why the session's model arrives with the report rather than being
    // fetched -- the model belongs to the listener's thread and is valid for
    // the duration of this call.
    void onPlaybackReport(const ClientId& client, const matp::PlaybackReport& report,
                          const ClientClockModel& clock);

private:
    Zone(ZoneId id, AudioMemory& memory, const OutputPlanner& planner,
         const ResamplerRegistry& registry, IDecoderFactory& decoders, const IClock& clock,
         ZoneSettings settings, uint64_t lookaheadNs,
         uint64_t lateJoinLeadNs, std::unique_ptr<NativePcmStore> store);

    // One room's two settings, resolved: its own through
    // `ZoneSettings::roomPreference` where there is one, this zone's
    // otherwise.
    [[nodiscard]] RoomAudioSettings settingsFor(const ClientId& client) const;

    // One room plays this zone's line alone and is about to stop being alone
    // -- it was grouped, or a room is joining: the line is moved to where that
    // room is heard (StartScheduler::alignLineTo). The room is not moved, a
    // room joining is placed with it, and a room that has just become drift
    // corrected starts from no offset rather than from the one its card
    // walked to, which past section 32's threshold would be a hard resync on
    // its next report. Nothing happens unless that offset is steady and the
    // move is between a millisecond and a prebuffer. `why` is for the log.
    void alignLineToLoneRoom(const char* why);

    struct Member
    {
        SessionId session;
        std::shared_ptr<ClientOutput> output;

        // One per room, built against the rate that room's device actually
        // granted: section 26's drift is measured against *this client's*
        // stream rate and against no rate the zone shares. A shared_ptr for
        // the same reason the output is one -- a caller reading a room's sync
        // status must not be racing release().
        std::shared_ptr<ClientSyncController> sync;

        // Times this room's stream has been moved (phase 16). It is carried
        // across a disconnect with the rest of the room's state, so it counts
        // the room rather than the connection.
        uint64_t rejoins = 0;

        // What its device offered when it was admitted, which is what a
        // format boundary re-plans it against (section 24).
        ClientCapabilities capabilities;

        // Section 24: the boundary this room's stream is held at while its
        // device is reopened, and zero when it is not. `renegotiatedAtNs` is
        // when it was asked to, and zero until the line reached the boundary
        // -- the ready timeout runs from there.
        TimelineNs transitionAt{};
        uint64_t renegotiatedAtNs = 0;

        // Phase 26: the server instant the seeked audio becomes audible in
        // this room, and zero when no flush is in progress. Until then the
        // room plays silence at the cut and reports the cut as audible, which
        // is a position that is not playing -- so its reports feed nothing.
        uint64_t flushUntilNs = 0;

        // Phase 27's output latency, as last set; not kept across a
        // disconnect, because the control API sets it again on admission.
        uint64_t latencyNs = 0;

        // Phase 40: this room's moves, and the moves it could not make, one
        // line per interval however often they come.
        log::RateLimit moveLog{};

        // Phase 41. `nextMoveNs` is the earliest instant the next `Evicted` or
        // `LeftBehind` move may be made, and `backoffNs` the wait after it.
        // `leftBehind` holds the scheduler's request while that wait runs,
        // because takeRejoins() hands it over only once; a stranded room asks
        // again on every pass by itself. `recentMoves` are those moves' instants
        // inside the last 30 s, and `lastMoveNs` any move's.
        uint64_t backoffNs = 0;
        uint64_t nextMoveNs = 0;
        bool leftBehind = false;
        std::deque<uint64_t> recentMoves{};
        uint64_t lastMoveNs = 0;
        bool unstable = false;
        uint64_t unstableSinceNs = 0;
        uint64_t episodeMoves = 0;
    };

    // What a room leaves behind when its connection goes, against its return
    // (section 43). Not a member: there is no stream, no session and no place
    // in the start barrier -- only the two things a reconnect would otherwise
    // have to measure again from nothing.
    struct RetainedRoom
    {
        std::shared_ptr<ClientSyncController> sync;
        uint64_t rejoins = 0;
        uint64_t sinceNs = 0; // when it went away; the eviction order
    };

    // --- the pipeline thread only -------------------------------------------

    // Opens the next staged source, stages it on the timeline and records it.
    // A source that cannot be opened is skipped for the one after it. False
    // when there is none left, which is the zone ending rather than failing:
    // the core never chooses what comes next, so running out is a state and
    // not a fault.
    [[nodiscard]] Result<bool, ZoneError> openNext();

    // One block: filled from the decoder, committed and published. False when
    // there is nothing more to do this pass; `sourceEnded` distinguishes a
    // source that finished from a pool that was empty or a zone that failed.
    // A source that failed part-way has ended too, with what it decoded
    // before the failure published and the reason in `sourceFailure`.
    bool decodeBlock(bool& sourceEnded, std::string& sourceFailure);

    // Records a source this zone could not play and says so in the log.
    void skipSource(const std::string& sourceId, std::string reason);

    // Why no room of this zone can be planned for `format`, or nothing when
    // one can -- or when there is no room to ask. A source no room can play
    // is skipped rather than holding the zone at its boundary for good.
    [[nodiscard]] std::optional<std::string> unplayable(const AudioFormat& format) const;

    // --- phase 42: this zone's memory, on the pipeline's side of its mutex --

    // The blocks a source in `format` beginning at `firstFrame` needs, taken
    // before a block of it is decoded. More than this zone holds is allocated
    // now, and refused by name where the memory limit would not hold it.
    // Fewer is a shrink that waits until the store holds nothing of the
    // previous format, because until then the rooms are still playing it.
    [[nodiscard]] Status<ZoneError> budgetFor(const AudioFormat& format, SourceFrame firstFrame);

    // A shrink budgetFor() deferred, made once the store has let go of the
    // format it was for. Every pass of pump().
    void applyShrink();

    // Everything this zone holds goes back: the store's blocks, and the pool's
    // as the rooms let go of theirs. A stopped zone and one that has played
    // out cost nothing.
    void releaseMemory();

    // Whether the frontier is still inside one lookahead of the room that is
    // furthest ahead -- and, once the line exists, of `playhead + lateJoinLead
    // + targetBuffer`, which is the budget create() checked against the
    // retention (phase 39). A room's demand is what it *built*, not what it
    // was delivered, so it is bounded here as well as at the room.
    [[nodiscard]] bool wantsAudio() const;

    // Phase 39: how far a room with `latencyNs` of output latency may be built,
    // one prebuffer and one packet past what it plays now. With no line yet
    // that is measured from zero, and the room's own floor -- where its stream
    // began -- is what applies. Called under mutex_.
    [[nodiscard]] TimelineNs ceilingLocked(uint64_t latencyNs, uint64_t nowNs) const;

    // The position a room joining now starts at.
    //
    // Before the first START_AT there is no playhead for a join to be ahead
    // of, so it is where this zone's audio still begins -- the only answer
    // that is both defensible and contiguous. Once the timeline is running it
    // is one lateJoinLead past what is audible now (section 44), which is a
    // position the decode has not necessarily reached: the store reports the
    // cursor as being at the frontier rather than as a fault, and the zone's
    // own demand-driven decode is what closes the gap.
    //
    // A line that is standing at a fixed position waiting for its barrier --
    // a resume, or a format transition nobody played through -- has no
    // playhead running ahead of anybody, so a room joins it at that position.
    // And never before the source whose format every new plan is made
    // against (section 24).
    //
    // Nor on audio the store has let go of (phase 40): once there is a line,
    // never before the oldest position the store holds plus one packet. A
    // frontier far enough ahead of the line puts the oldest audio past
    // `playhead + lead`, and a room moved there would be stranded again on its
    // first read and moved again on the next pass.
    [[nodiscard]] Result<TimelineNs, ZoneError> joinPosition() const;

    // Everything section 20 can decide this pass, and the messages it decided
    // on, sent. Called by pump() before anything is decoded, and by nothing
    // else: every input the scheduler has arrives on another thread and only
    // records.
    void schedule();

    // The same move rejoin() makes, for a caller that is a loop and cannot do
    // anything useful with a failure but say so. Every trigger that is noticed
    // rather than asked for comes through here.
    void rejoinOrWarn(const ClientId& client, RejoinTrigger trigger);

    // The `Evicted` and `LeftBehind` moves this pass may make (phase 41):
    // rooms the store ran past and rooms the line ran past, each once its
    // back-off has run out. `leftBehind` is what the scheduler asked for this
    // pass. Also ends the episodes of rooms that have settled, into `events`.
    [[nodiscard]] std::vector<std::pair<ClientId, RejoinTrigger>>
    dueMoves(const std::vector<ClientId>& leftBehind, std::vector<ZoneEvent>& events);

    // A move that was made, counted against the room's back-off and episode
    // (phase 41). Answers the event that announces it, if any: RoomFellBehind,
    // RoomUnstable when this move begins an episode, nothing while one lasts.
    [[nodiscard]] std::optional<ZoneEvent> recordMove(const ClientId& client,
                                                      RejoinTrigger trigger, TimelineNs position);

    // --- section 24 -----------------------------------------------------------

    // Every room of this zone, re-planned for a source in `format` that
    // begins at `at`, before a block of it is published. A room whose device
    // keeps its format gets its new converter at the boundary; one that has to
    // be reopened is held there and paused there.
    void beginTransition(TimelineNs at, const AudioFormat& format);

    // The rooms held at a boundary the line has now reached, asked to
    // re-negotiate (section 24 steps 3 and 4) -- and the line itself stopped
    // there if no room is left playing through it. Called by schedule().
    void advanceTransitions();

    // A room held at a boundary answered STREAM_READY: planned against what
    // its driver granted, given the converter, and placed on the line again
    // from the boundary. False when this room is not in a transition.
    bool completeTransition(const ClientId& client, const AudioFormat& granted);

    // A transition that is still inside its ready timeout, which is what
    // keeps the next source from being opened on top of it: one boundary at a
    // time is what lets a room's re-negotiated format be the format at its
    // boundary. Called under mutex_.
    [[nodiscard]] bool transitionPendingLocked(uint64_t nowNs) const;

    // The format of the source that covers `position`. Called under mutex_.
    [[nodiscard]] AudioFormat formatAtLocked(TimelineNs position) const;

    // Keeps what belongs to a room rather than to its connection, and forgets
    // the room that has been away longest once the ceiling is reached. Called
    // under mutex_.
    void retainLocked(const ClientId& client, Member& leaving);

    // Drops the front of `sources_` the line has left behind, past a short tail.
    void pruneSourcesLocked();

    // Moves every room a stop parked onto `position` and starts it again.
    // A room that cannot be moved is that room's failure and stays parked,
    // because one slow or broken room has never been a reason for a zone to
    // stay silent.
    void restartParkedMembers(TimelineNs position);

    // Fills in each message's one remaining field -- the session of the room
    // it is for -- and hands it to the sender. Always called with the lock
    // released: the other end of the sender is another thread's mailbox.
    void dispatch(std::vector<RoomCommand> commands);

    // Section 23, to every room of this zone: where the source that has just
    // been opened begins. Announced before any room reaches it, which is what
    // lets a client change what it is displaying exactly when the position
    // becomes audible.
    void announceBoundary(const StagedSource& staged);

    // Sections 23 and 44, to one room that has just joined: what is playing at
    // the position its stream begins at, and every boundary after it. A room
    // that joined mid-track never received the boundary that named the track,
    // and one that joined past a boundary this zone has already announced
    // never received that one either.
    void announceTracksTo(const ClientId& client, TimelineNs position);

    void failZone(ZoneError code, std::string detail);

    // --- phase 24 --------------------------------------------------------------

    // Hands events to `ZoneSettings::onEvent`. Always called with every lock
    // of this zone released.
    void emit(const std::vector<ZoneEvent>& events) const;

    // A ZoneEvent of `kind` with this zone's id in it.
    [[nodiscard]] ZoneEvent eventOf(ZoneEventKind kind) const;

    // What the line and the decode did since the last look: a transport that
    // changed, sources that became audible, a zone that has played out and
    // rooms left behind. Called under mutex_; `stoppedAt` is where a stop
    // left the line, which the scheduler forgets as it stops.
    void noticeLocked(std::vector<ZoneEvent>& events,
                      std::optional<TimelineNs> stoppedAt = std::nullopt);

    // The move rejoin() makes, and where it put the room. rejoin() is this
    // and its event.
    [[nodiscard]] Status<ZoneError> moveRoom(const ClientId& client, RejoinTrigger trigger,
                                             TimelineNs& position,
                                             std::unique_ptr<IClientAudioProcessor> processor);

    // --- fixed for the life of the zone -------------------------------------

    ZoneId id_;
    AudioMemory& memory_;
    const OutputPlanner& planner_;
    const ResamplerRegistry& registry_;
    IDecoderFactory& decoders_;
    const IClock& clock_;
    ZoneSettings settings_;
    uint64_t lookaheadNs_ = 0;
    uint64_t lateJoinLeadNs_ = 0;

    // Before the store, so that it outlives every block the store and the
    // rooms hold (phase 42).
    std::unique_ptr<BlockPool> pool_;

    std::unique_ptr<NativePcmStore> store_;
    SourceTimeline timeline_;

    // --- the pipeline thread only -------------------------------------------

    std::unique_ptr<IDecoder> decoder_;

    // Phase 42, under pipelineMutex_: the blocks this zone's format needs,
    // and a smaller number waiting for the store to hold nothing before
    // `shrinkFrom_`. Zero is nothing held and nothing waiting.
    size_t budget_ = 0;
    size_t pendingBudget_ = 0;
    SourceFrame shrinkFrom_{};

    // --- shared --------------------------------------------------------------

    mutable std::mutex mutex_;
    std::deque<SourceSegment> staged_;   // guarded by mutex_
    std::vector<StagedSource> sources_;  // guarded by mutex_

    // Every source ever pushed onto `sources_`, which pruneSourcesLocked()
    // shortens: what a seek compares to notice the zone moved on meanwhile.
    uint64_t sourcesOpened_ = 0;         // guarded by mutex_
    std::map<ClientId, Member> members_;          // guarded by mutex_
    std::map<ClientId, RetainedRoom> retained_;   // guarded by mutex_
    AudioFormat sourceFormat_;           // guarded by mutex_

    // Where the source that brought `sourceFormat_` begins. Nothing this zone
    // plans against that format may begin before it, which is what keeps a
    // room joining between a boundary's decode and its playback out of the
    // old source's blocks.
    TimelineNs formatSinceNs_{};         // guarded by mutex_
    ZoneState state_ = ZoneState::Idle;  // guarded by mutex_
    std::string lastError_;              // guarded by mutex_
    std::deque<FailedSource> failed_;    // guarded by mutex_

    // Phase 24: what has been announced already, so that each event goes out
    // once. `startedThrough_` counts the sources at the front of `sources_`
    // the line has already reached; `leftBehind_` is bounded by the members.
    size_t startedThrough_ = 0;                                   // guarded by mutex_
    bool endAnnounced_ = false;                                   // guarded by mutex_
    TransportState announcedTransport_ = TransportState::Stopped; // guarded by mutex_
    std::set<ClientId> leftBehind_;                               // guarded by mutex_


    // Section 20's state machine. It has no lock of its own: every call into
    // it is made under mutex_, from the listeners' threads that record and
    // from the pipeline thread that decides.
    StartScheduler scheduler_; // guarded by mutex_

    // Phase 40: joins are being placed on the store's oldest audio rather
    // than at `playhead + lead`, which is said once per episode.
    mutable std::atomic<bool> joinsBehindStore_{false};

    std::atomic<uint64_t> published_{0};
    std::atomic<uint64_t> rejoins_{0};

    // One move at a time, whichever thread noticed. A rejoin stops a room's
    // thread, moves its cursor and starts it again, and two of those racing on
    // one room would be two answers to where that room is. It is never taken
    // while mutex_ is held, and mutex_ is taken inside it.
    std::mutex rejoinMutex_;

    // The decoder's, and one pass of pump() at a time against a seek that
    // replaces it (phase 26). Taken before rejoinMutex_ and mutex_, and never
    // while either is held.
    std::mutex pipelineMutex_;

    // Last, so that it is stopped before anything it touches is destroyed.
    std::jthread thread_;
};

} // namespace multiroom
