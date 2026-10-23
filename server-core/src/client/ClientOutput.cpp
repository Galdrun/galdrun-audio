#include "client/ClientOutput.h"

#include "matp/AudioPacket.h"
#include "util/Logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace multiroom {
namespace {

// How long this room's thread waits before looking again when there was
// nothing to do. It is not a protocol timeout and it is not the packet
// cadence: one pass produces as much as pacing allows, which during a
// prebuffer is the whole 1500 ms at once, so this only decides how quickly a
// room notices a report or a socket that started draining again. Five
// milliseconds is far finer than the four buffer statuses a second it
// services and costs two hundred wake-ups a second per room.
constexpr auto kPollInterval = std::chrono::milliseconds(5);

// How much source a converter installed mid-stream is primed with. More than
// either real converter's filter reaches back (r8brain's is the longer, about
// 40 ms at 44.1 kHz), and far less than the store holds behind a stream that
// is a prebuffer ahead of what is audible.
constexpr uint64_t kPreRollNs = 100ULL * 1000000ULL;

} // namespace

const char* errorName(OutputError error)
{
    switch (error)
    {
    case OutputError::InvalidSettings:
        return "InvalidSettings";
    case OutputError::InvalidSession:
        return "InvalidSession";
    case OutputError::InvalidPlan:
        return "InvalidPlan";
    case OutputError::NotJoined:
        return "NotJoined";
    case OutputError::AlreadyJoined:
        return "AlreadyJoined";
    case OutputError::NotParked:
        return "NotParked";
    case OutputError::NotHeld:
        return "NotHeld";
    case OutputError::NotStreaming:
        return "NotStreaming";
    case OutputError::SwitchPending:
        return "SwitchPending";
    case OutputError::TimelineFailed:
        return "TimelineFailed";
    case OutputError::ClockFailed:
        return "ClockFailed";
    case OutputError::StoreFailed:
        return "StoreFailed";
    case OutputError::ProcessorFailed:
        return "ProcessorFailed";
    case OutputError::PacketFailed:
        return "PacketFailed";
    case OutputError::SinkFailed:
        return "SinkFailed";
    case OutputError::QueueStalled:
        return "QueueStalled";
    }
    return "Unknown";
}

const char* outputStateName(OutputState state)
{
    switch (state)
    {
    case OutputState::Idle:
        return "idle";
    case OutputState::Streaming:
        return "streaming";
    case OutputState::Stopped:
        return "stopped";
    case OutputState::Parked:
        return "parked";
    case OutputState::Stranded:
        return "stranded";
    case OutputState::Failed:
        return "failed";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------

ClientOutput::ClientOutput(SessionId session, ClientId client, NativePcmStore& store,
                           const SourceTimeline& timeline,
                           std::unique_ptr<IClientAudioProcessor> processor, const IClock& clock,
                           ClientOutputSettings settings)
    : session_(session), client_(std::move(client)), store_(store), timeline_(timeline),
      processor_(std::move(processor)), clock_(clock), settings_(settings),
      packetizer_(session, processor_->plan().outputFormat)
{
    packetDurationNs_ = static_cast<uint64_t>(settings_.packetDurationMs) * 1000000ULL;
    adoptPlan();
}

void ClientOutput::adoptPlan()
{
    const OutputPlan& plan = processor_->plan();

    // The one saving phase 7 left to this phase: a bitperfect room whose two
    // formats are the same layout needs no repack at all, so the block's bytes
    // are the payload and the direct path is never entered. A bitperfect room
    // whose layouts differ -- 24 bits packed into three bytes into a 32-bit
    // container -- still goes through it, because those really are different
    // bytes for the same values.
    passThrough_.store(plan.mode == OutputMode::BitPerfect &&
                       plan.sourceFormat == plan.outputFormat);

    // A correction is only ever offered to a room whose plan was made for one,
    // and the converter's ratio is the whole conversion rather than the trim
    // alone: a 44.1 -> 48 kHz room pulled +30 ppm runs at 1.088435 * 1.000030
    // and not at either factor on its own.
    // Only the server's own converter takes a trim. A room whose client trims
    // its clock keeps the nominal ratio and output-frame timeline.
    driftCorrected_.store(plan.resamplingCorrected());
    driftActuator_.store(plan.driftActuator);
    const double nominal = plan.resamplerConfig().nominalRatio();
    nominalRatio_ = nominal > 0.0 ? nominal : 1.0;

    outputBytesPerFrame_ = plan.outputFormat.bytesPerFrame();
    sourceBytesPerFrame_ = plan.sourceFormat.bytesPerFrame();

    // Designed at the rate the converter produces, and handed to every
    // converter; a bitperfect one ignores it.
    equalizer_.setFormat(plan.outputFormat.sampleRate, plan.outputFormat.channels);
    processor_->setEqualizer(&equalizer_);
    equalizing_.store(plan.mode == OutputMode::Resampled && equalizer_.shaping());

    const Result<uint64_t, FrameMathError> perPacket =
        framesFromNanoseconds(packetDurationNs_, plan.sourceFormat.sampleRate);
    maxCarryFrames_ = perPacket && *perPacket <= std::numeric_limits<uint32_t>::max() / 2
                          ? static_cast<uint32_t>(*perPacket) * 2 + 1
                          : std::numeric_limits<uint32_t>::max();

    std::lock_guard<std::mutex> lock(mutex_);
    plan_ = plan;
}

ClientOutput::~ClientOutput()
{
    stop();

    if (sink_ != nullptr)
        sink_->close();
    if (joined_)
        static_cast<void>(store_.removeReader(reader_));
}

Result<std::unique_ptr<ClientOutput>, OutputError>
ClientOutput::create(SessionId session, ClientId client, NativePcmStore& store,
                     const SourceTimeline& timeline,
                     std::unique_ptr<IClientAudioProcessor> processor, const IClock& clock,
                     ClientOutputSettings settings)
{
    if (settings.packetDurationMs == 0 || settings.targetBufferNs == 0 ||
        settings.maxQueuedPackets == 0 || settings.queueStallTimeoutNs == 0)
    {
        return multiroom::fail(OutputError::InvalidSettings,
                            "packetDurationMs, targetBufferNs, maxQueuedPackets and "
                            "queueStallTimeoutNs must all be non-zero");
    }
    if (static_cast<uint64_t>(settings.packetDurationMs) * 1000000ULL >
        matp::kMaxPacketDurationNs)
    {
        return multiroom::fail(OutputError::InvalidSettings,
                            "a packet of " + std::to_string(settings.packetDurationMs) +
                                " ms is more than a receiver maps in one");
    }
    if (!session.valid())
    {
        return multiroom::fail(OutputError::InvalidSession,
                            "section 13's all-zero id is not a session to send audio for");
    }
    if (processor == nullptr)
        return multiroom::fail(OutputError::InvalidPlan,
                            "a room's output needs its own processor");

    const OutputPlan& plan = processor->plan();
    if (!plan.sourceFormat.isValid() || !plan.outputFormat.isValid())
        return multiroom::fail(OutputError::InvalidPlan, describePlan(plan));

    return std::unique_ptr<ClientOutput>(new ClientOutput(session, std::move(client), store,
                                                          timeline, std::move(processor), clock,
                                                          settings));
}

Status<OutputError> ClientOutput::joinAt(TimelineNs position)
{
    if (joined_)
    {
        // A second position is a moved cursor, a reset converter and a
        // re-anchored clock rather than a second beginning, and it is only
        // defensible on a stream that has been parked: rejoinAt() is that.
        return multiroom::fail(OutputError::AlreadyJoined,
                            "this room's stream already begins at " +
                                std::to_string(std::to_underlying(originNs_)) + " ns");
    }

    const Result<SourceFrame, TimelineError> frame = timeline_.frameAt(position);
    if (!frame)
        return multiroom::fail(OutputError::TimelineFailed, describe(frame.error()));

    reader_ = store_.addReader(*frame);
    anchorAt(position);
    joined_ = true;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = OutputState::Streaming;
    }

    MULTIROOM_INFO("client output {}: {} from timeline {} ns, source frame {}{}", client_.str(),
             describePlan(processor_->plan()), std::to_underlying(position),
             std::to_underlying(*frame), passThrough_.load() ? ", pass-through" : "");
    return {};
}

Status<OutputError> ClientOutput::run()
{
    if (!joined_)
        return multiroom::fail(OutputError::NotJoined,
                            "joinAt() names the position a room starts at");
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

void ClientOutput::stop()
{
    thread_.request_stop();
    if (thread_.joinable())
        thread_.join();

    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == OutputState::Streaming || state_ == OutputState::Idle ||
        state_ == OutputState::Parked)
    {
        state_ = OutputState::Stopped;
    }
}

void ClientOutput::park(bool disconnect)
{
    thread_.request_stop();
    if (thread_.joinable())
        thread_.join();

    // Reopening the device replaces the connection too. Stop production before
    // detaching so a newly attached socket cannot receive the old partial packet.
    if (disconnect)
        detach();

    // The block this room was reading goes back to its zone's pool, which a
    // stopped zone empties (phase 42); rejoinAt() reads from the store again.
    block_.reset();
    blockOffsetFrames_ = 0;

    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != OutputState::Streaming)
        return; // failed, stopped, stranded or parked already: nothing to park

    dropBuiltAudio();
    state_ = OutputState::Parked;
}

void ClientOutput::dropBuiltAudio()
{
    // Section 38 on this side of the socket, and phase 16's strand for the
    // same reason. A packet the sink is half-way through stays: the bytes
    // already gone are the first half of it, and a receiver handed the second
    // half of one packet followed by the first byte of another drops the
    // connection rather than the packet.
    const size_t keep = writtenOfFront_ > 0 ? 1 : 0;
    uint64_t dropped = 0;
    while (queue_.size() > keep)
    {
        queue_.pop_back();
        ++dropped;
    }
    queuedNs_ = queue_.empty() ? 0 : queue_.front().durationNs;

    // Those packets were numbered and never sent, and a gap in the numbering
    // on one connection is a server that skipped ahead as far as the client is
    // concerned -- which costs the connection rather than the packet. The
    // numbers go back.
    packetizer_.rewind(dropped);

    // What this room last said about its buffer described a timeline it has
    // just dropped, so pacing starts again from nothing rather than from a
    // report that is no longer about anything.
    bufferedNs_ = 0;
    producedSinceReportNs_ = queuedNs_;
    queueFullSinceNs_ = 0;
    pendingTrim_ = 0.0;

    // Not on the line any more until the zone says the moved stream is.
    onLine_.store(false);
    inReportGap_ = false;
}

Status<OutputError> ClientOutput::rejoinAt(TimelineNs position)
{
    if (!joined_)
        return multiroom::fail(OutputError::NotJoined,
                            "joinAt() names the position a room starts at");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != OutputState::Parked && state_ != OutputState::Stranded)
        {
            return multiroom::fail(OutputError::NotParked,
                                "this room's stream is " +
                                    std::string(outputStateName(state_)) +
                                    "; park() is what stops it");
        }
    }

    // A stranded room's thread is still running -- strandRoom() cannot join
    // the thread it runs on -- and everything below is that thread's. It is
    // stopped here as park() stops a streaming one, and run() starts it again.
    thread_.request_stop();
    if (thread_.joinable())
        thread_.join();

    const Result<SourceFrame, TimelineError> frame = timeline_.frameAt(position);
    if (!frame)
        return multiroom::fail(OutputError::TimelineFailed, describe(frame.error()));
    if (const auto sought = store_.seek(reader_, *frame); !sought)
        return multiroom::fail(OutputError::StoreFailed, describe(sought.error()));

    // Everything the grid, the clock and the converter had built for the old
    // position. The packetizer is deliberately not among them: its sequence
    // numbering runs on, because the client reads a gap in it on one
    // connection as a server that skipped ahead.
    anchorAt(position);
    processor_->reset();

    clearAssembly();
    atBoundary_ = false;
    cutAtEnd_ = false;

    double trim = 1.0;
    bool logged = false;
    uint64_t unlogged = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        trim = trim_;
        state_ = OutputState::Streaming;
        logged = moveLog_.allow(clock_.monotonicNowNs(), unlogged);
    }

    // The correction in force survives the move, because the card whose drift
    // it corrects did not stop drifting. The clock is freshly anchored at 1.0,
    // so it is the half of the pair that has to be told again -- phase 13's
    // rule that the two move together or neither moves, applied to a stream
    // that has just been moved.
    if (trim != 1.0)
    {
        if (const auto applied = outputClock_.setTrim(trim, position); !applied)
            return multiroom::fail(OutputError::ClockFailed, describe(applied.error()));
    }

    if (logged)
        MULTIROOM_INFO("client output {}: stream moved to timeline {} ns, source frame {} ({} more "
                 "moves since the last line)",
                 client_.str(), std::to_underlying(position), std::to_underlying(*frame), unlogged);
    else
        MULTIROOM_DEBUG("client output {}: stream moved to timeline {} ns, source frame {}",
                  client_.str(), std::to_underlying(position), std::to_underlying(*frame));
    return {};
}

Status<OutputError> ClientOutput::flushTo(TimelineNs position)
{
    if (const auto moved = rejoinAt(position); !moved)
        return moved;

    // After the move and not before it: a stream that could not be moved
    // must not flag whatever it builds next. The thread is stopped here, as
    // it is for rejoinAt(), so the packetizer is this caller's for the call.
    packetizer_.markAfterFlush();
    return {};
}

Status<OutputError> ClientOutput::reformat(std::unique_ptr<IClientAudioProcessor> processor)
{
    if (processor == nullptr)
        return multiroom::fail(OutputError::InvalidPlan, "a room's output needs its own processor");
    const OutputPlan& plan = processor->plan();
    if (!plan.sourceFormat.isValid() || !plan.outputFormat.isValid())
        return multiroom::fail(OutputError::InvalidPlan, describePlan(plan));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != OutputState::Parked && state_ != OutputState::Stranded)
        {
            return multiroom::fail(OutputError::NotParked,
                                "this room's stream is " +
                                    std::string(outputStateName(state_)) +
                                    "; park() is what stops it");
        }
    }

    // Stopped, as it is for rejoinAt(), so the converter and the packetizer
    // are this caller's for the call.
    thread_.request_stop();
    if (thread_.joinable())
        thread_.join();

    const AudioFormat previousOutput = processor_->plan().outputFormat;
    processor_ = std::move(processor);
    adoptPlan();
    const AudioFormat& output = processor_->plan().outputFormat;
    packetizer_.setFormat(output);

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // A switch still waiting for a boundary was planned for the device
        // that has just gone.
        pendingProcessor_.reset();
        pendingInPlace_ = false;
        holdNs_ = 0;
        laterHolds_.clear();

        // What is queued was built for the device that was reopened in
        // another format, so those bytes are noise to it (applyFormatSwitch).
        if (output != previousOutput)
            dropBuiltAudio();

        if (!driftCorrected_.load())
        {
            trim_ = 1.0;
            pendingTrim_ = 0.0;
        }
        else if (trim_ != 1.0)
        {
            pendingTrim_ = trim_;
        }
    }

    formatSwitches_.fetch_add(1);
    MULTIROOM_INFO("client output {}: reopened device: {}", client_.str(), describePlan(processor_->plan()));
    return {};
}

// ---------------------------------------------------------------------------

Status<OutputError> ClientOutput::holdAt(TimelineNs position)
{
    if (!joined_)
        return multiroom::fail(OutputError::NotJoined,
                            "joinAt() names the position a room starts at");

    const uint64_t at = std::to_underlying(position);
    if (at == 0)
    {
        // Zero is this class's "no boundary ahead", and a boundary there would
        // be a stream with nothing in it: the first source of a zone has no
        // format before it to change from.
        return multiroom::fail(OutputError::TimelineFailed,
                            "timeline zero is not a boundary a stream can be held at");
    }

    // A hold exactly where this stream has got to is a stream that stops
    // before its next packet -- which is what a room that joined on the far
    // side of a boundary, and has produced nothing yet, needs. Behind it there
    // is no packet left to cut.
    const uint64_t produced = producedThroughNs_.load();
    if (at < produced)
    {
        return multiroom::fail(OutputError::TimelineFailed,
                            std::to_string(at) + " ns is behind this room's stream, which "
                                                 "has been built through " +
                                std::to_string(produced) + " ns");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // A boundary past the one this stream has still to reach -- two short
    // sources in different formats inside one lookahead -- is queued behind
    // it rather than replacing it: replacing it would let the stream run
    // across the first one with the converter for the source before it.
    if (holdNs_ != 0 && at > holdNs_)
    {
        while (!laterHolds_.empty() && laterHolds_.back().holdNs >= at)
            laterHolds_.pop_back();
        laterHolds_.push_back(LaterHold{at, nullptr, TimelineNs{}});
        return {};
    }

    holdNs_ = at;
    laterHolds_.clear();

    // A converter replaceProcessor() is still waiting to install was planned
    // against the source this boundary ends; the boundary plans the room
    // again, and installing it here would take the hold with it.
    if (pendingInPlace_)
    {
        pendingProcessor_.reset();
        pendingInPlace_ = false;
    }
    return {};
}

Status<OutputError> ClientOutput::switchTo(std::unique_ptr<IClientAudioProcessor> processor,
                                           TimelineNs position)
{
    if (processor == nullptr)
        return multiroom::fail(OutputError::InvalidPlan, "a room's output needs its own processor");

    const OutputPlan& plan = processor->plan();
    if (!plan.sourceFormat.isValid() || !plan.outputFormat.isValid())
        return multiroom::fail(OutputError::InvalidPlan, describePlan(plan));

    std::lock_guard<std::mutex> lock(mutex_);
    if (!laterHolds_.empty())
    {
        // The converter for the latest boundary this stream is held at.
        LaterHold& last = laterHolds_.back();
        if (std::to_underlying(position) < last.holdNs)
        {
            return multiroom::fail(OutputError::TimelineFailed,
                                std::to_string(std::to_underlying(position)) +
                                    " ns is behind the boundary this stream is held at (" +
                                    std::to_string(last.holdNs) + " ns)");
        }
        last.processor = std::move(processor);
        last.position = position;
        return {};
    }
    if (holdNs_ == 0)
    {
        // A converter installed anywhere but on a boundary reads the wrong
        // bytes as the right ones, which is noise rather than a near miss.
        return multiroom::fail(OutputError::NotHeld,
                            "holdAt() is what stops a stream on the boundary a converter "
                            "is installed at");
    }
    if (std::to_underlying(position) < holdNs_)
    {
        return multiroom::fail(OutputError::TimelineFailed,
                            std::to_string(std::to_underlying(position)) +
                                " ns is behind the boundary this stream is held at (" +
                                std::to_string(holdNs_) + " ns)");
    }

    pendingProcessor_ = std::move(processor);
    pendingPosition_ = position;
    pendingInPlace_ = false;
    return {};
}

Status<OutputError>
ClientOutput::replaceProcessor(std::unique_ptr<IClientAudioProcessor> processor)
{
    if (processor == nullptr)
        return multiroom::fail(OutputError::InvalidPlan, "a room's output needs its own processor");

    const OutputPlan& plan = processor->plan();
    if (!plan.sourceFormat.isValid() || !plan.outputFormat.isValid())
        return multiroom::fail(OutputError::InvalidPlan, describePlan(plan));

    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != OutputState::Streaming)
    {
        return multiroom::fail(OutputError::NotStreaming,
                            "this room's stream is " + std::string(outputStateName(state_)));
    }
    if (holdNs_ != 0 || pendingProcessor_ != nullptr)
    {
        return multiroom::fail(OutputError::SwitchPending,
                            "another converter is waiting to be installed on this stream");
    }

    // The packets already queued stay, so the device that plays them has to
    // be the one the new converter writes for: nothing tells the client that
    // anything changed.
    if (plan.sourceFormat != plan_.sourceFormat || plan.outputFormat != plan_.outputFormat)
    {
        return multiroom::fail(OutputError::InvalidPlan,
                            describePlan(plan) + " is not this stream's " +
                                describeFormat(plan_.sourceFormat) + " -> " +
                                describeFormat(plan_.outputFormat));
    }

    pendingProcessor_ = std::move(processor);
    pendingPosition_ = TimelineNs{};
    pendingInPlace_ = true;
    return {};
}

void ClientOutput::applyFormatSwitch()
{
    std::unique_ptr<IClientAudioProcessor> incoming;
    TimelineNs position{};
    bool inPlace = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingProcessor_ == nullptr)
            return;

        // The tail of the old source goes out first. Production stops on the
        // boundary rather than at it, so this is the one condition worth
        // waiting for: a converter installed while the last packet of the old
        // format is still to be built would build it out of the new one.
        if (producedThroughNs_.load() < holdNs_)
            return;

        incoming = std::move(pendingProcessor_);
        inPlace = pendingInPlace_;
        pendingInPlace_ = false;

        // Never behind where this stream already is: a room moved while its
        // switch was waiting (a rejoin, or a zone that was stopped and played
        // again) carries on from where the move put it, not from a boundary
        // the store may no longer hold.
        position = TimelineNs{std::max(std::to_underlying(pendingPosition_),
                                       producedThroughNs_.load())};
    }

    const Result<SourceFrame, TimelineError> frame = timeline_.frameAt(position);
    if (!frame)
    {
        failRoom(OutputError::TimelineFailed, describe(frame.error()));
        return;
    }
    if (const auto sought = store_.seek(reader_, *frame); !sought)
    {
        failRoom(OutputError::StoreFailed, describe(sought.error()));
        return;
    }

    const AudioFormat previousOutput = processor_->plan().outputFormat;
    processor_ = std::move(incoming);
    adoptPlan();

    const AudioFormat& output = processor_->plan().outputFormat;
    packetizer_.setFormat(output);

    // Everything the grid, the clock and the converter had built for the old
    // format. The packetizer's sequence is deliberately not among them: the
    // audio connection outlives a device that was reopened on it, and a gap in
    // the numbering on one connection is a server that skipped ahead.
    anchorAt(position);

    clearAssembly();
    atBoundary_ = false;
    cutAtEnd_ = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        holdNs_ = 0;

        // The next boundary, in the same step: the stream reads on from this
        // one, and the blocks past the next are already published.
        if (!laterHolds_.empty())
        {
            LaterHold next = std::move(laterHolds_.front());
            laterHolds_.pop_front();
            holdNs_ = next.holdNs;
            pendingProcessor_ = std::move(next.processor);
            pendingPosition_ = next.position;
        }

        if (output != previousOutput)
        {
            // The device that was going to play what is queued has been
            // reopened in another format, so those bytes are noise to it. A
            // packet the sink is half-way through stays, for the reason park()
            // keeps one: half a header is not a packet.
            dropBuiltAudio();
        }

        // A room that may no longer be corrected holds no correction: a plan
        // that stopped being drift-corrected is a room whose ratio is the
        // conversion and nothing else.
        if (!driftCorrected_.load())
        {
            trim_ = 1.0;
            pendingTrim_ = 0.0;
        }
        else if (trim_ != 1.0)
        {
            // The card whose drift it corrects did not stop drifting, and the
            // converter it was in force on has just been replaced. Recorded
            // rather than applied, so that the one path which moves the clock
            // and the converter together is still the only one.
            pendingTrim_ = trim_;
        }
    }

    // Mid-track, the audio before this frame is real and the converter has
    // to have heard it. At a boundary it is the end of the old source, and
    // silence is what the converter is right to start from.
    if (inPlace && processor_->plan().mode == OutputMode::Resampled)
        preRoll(*frame);

    formatSwitches_.fetch_add(1);
    MULTIROOM_INFO("client output {}: {} from timeline {} ns, source frame {}{}{}", client_.str(),
             describePlan(processor_->plan()), std::to_underlying(position),
             std::to_underlying(*frame), passThrough_.load() ? ", pass-through" : "",
             inPlace ? ", in place" : "");
}

void ClientOutput::resumeAfterEnd()
{
    if (!cutAtEnd_)
        return;

    const uint64_t at = producedThroughNs_.load();
    if (const uint64_t end = endNs_.load(); end != 0 && end <= at)
        return; // still the end
    {
        // A boundary on the same position moves the stream itself, with the
        // converter for what follows (applyFormatSwitch()).
        std::lock_guard<std::mutex> lock(mutex_);
        if (holdNs_ != 0 && holdNs_ <= at)
            return;
    }

    // The converter was finished from silence, and the cursor may have run
    // past the end with the block it read for that; both start again from the
    // first frame after the last one sent.
    const Result<SourceFrame, TimelineError> frame = timeline_.frameAt(TimelineNs{at});
    if (!frame)
    {
        failRoom(OutputError::TimelineFailed, describe(frame.error()));
        return;
    }
    if (const auto sought = store_.seek(reader_, *frame); !sought)
    {
        strandRoom(describe(sought.error()));
        return;
    }
    processor_->reset();

    originNs_ = TimelineNs{at};
    packetIndex_ = 0;
    clearAssembly();
    cutAtEnd_ = false;
    MULTIROOM_DEBUG("client output {}: stream carries on past the zone's end at timeline {} ns",
              client_.str(), at);
}

void ClientOutput::preRoll(SourceFrame at)
{
    const OutputPlan& plan = processor_->plan();
    const uint64_t frame = std::to_underlying(at);
    const uint64_t primed =
        framesFromNanoseconds(kPreRollNs, plan.sourceFormat.sampleRate).value_or(0);

    // What the converter makes of the priming, in its own output frames. The
    // nominal ratio is the whole conversion here: the room's trim is applied
    // after this, through the one path that moves the clock with it.
    const uint64_t discard =
        static_cast<uint64_t>(std::llround(static_cast<double>(primed) * nominalRatio_));

    const auto cold = [&](const char* why) {
        processor_->reset();
        clearAssembly();
        if (const auto back = store_.seek(reader_, at); !back)
        {
            strandRoom(describe(back.error()));
            return;
        }
        MULTIROOM_DEBUG("client output {}: converter started cold: {}", client_.str(), why);
    };

    if (primed == 0 || discard == 0 || frame < primed ||
        frame - primed < std::to_underlying(store_.oldestFrame()))
    {
        cold("the store no longer holds the audio before it");
        return;
    }
    if (const auto back = store_.seek(reader_, SourceFrame{frame - primed}); !back)
    {
        cold("the store no longer holds the audio before it");
        return;
    }

    // Through the loop every packet is built by, so the cursor, the block and
    // the carry it leaves are exactly where the next packet reads on from --
    // one packet's worth at a time, which is what its buffer is for.
    const uint32_t chunk = std::max<uint32_t>(maxCarryFrames_ / 2, 1);
    uint64_t left = discard;
    while (left > 0)
    {
        const uint32_t wanted = static_cast<uint32_t>(std::min<uint64_t>(left, chunk));
        if (!fill(wanted))
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (state_ != OutputState::Streaming)
                    return; // failed or stranded on the way; that path has the room now
            }
            cold("the decode is not far enough ahead to prime it");
            return;
        }
        packetFrames_ = 0;
        left -= wanted;
    }
}

// ---------------------------------------------------------------------------

void ClientOutput::pump()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != OutputState::Streaming)
            return;
    }

    // Section 24 first: the converter for what follows a boundary is installed
    // before the trim that is expressed through it, and before anything is
    // produced in a format that has just stopped being this room's.
    applyFormatSwitch();
    resumeAfterEnd();

    // Before anything is produced, so that a correction lands on the packet
    // boundary it was computed against rather than in the middle of a
    // prebuffer this pass is about to build.
    applyDriftTrim();
    applyEqualizer();

    // Drained first, so that a socket which started taking bytes again frees
    // the queue before this pass decides whether there is room to produce.
    drain();

    // A playing room whose reports have stopped is still draining, so it is
    // paced on the line until they come back (setOnLine()). The ceiling is
    // what bounds it then, and a room without one has no line to be paced on.
    bool gapBegan = false;
    uint64_t gapNs = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const uint64_t nowNs = clock_.monotonicNowNs();
        gapNs = lastReportNs_ && nowNs > *lastReportNs_ ? nowNs - *lastReportNs_ : 0;
        const bool stale = onLine_.load() && ceilingNs_.load() != 0 && gapNs > kReportGapNs;
        gapBegan = stale && !inReportGap_;
        inReportGap_ = stale;
        if (gapBegan)
            reportGaps_.fetch_add(1);
    }
    if (gapBegan)
    {
        MULTIROOM_WARN("client output {}: no buffer status for {} ms, pacing on the line", client_.str(),
                 gapNs / 1000000ULL);
    }

    for (;;)
    {
        bool allowed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != OutputState::Streaming)
                return;

            // Section 18's rule, and the trap it exists for: what is compared
            // is what this room *holds*, plus what has been produced for it
            // since it said so. A wall clock does not appear in it, which is
            // why a paused room stops being sent audio instead of filling a
            // socket nobody is reading -- and why the one exception is a room
            // that is known to be playing (setOnLine()).
            allowed = queue_.size() < settings_.maxQueuedPackets &&
                      (inReportGap_ ||
                       bufferedNs_ + producedSinceReportNs_ < settings_.targetBufferNs);
        }
        if (!allowed)
            break;
        if (!produce())
            break;
    }

    drain();
    checkStall();
}

bool ClientOutput::nextPacketStart(uint64_t& startNs) const
{
    const uint64_t origin = std::to_underlying(originNs_);

    // The grid, computed from the packet's index rather than added to the last
    // packet's end: section 15's "absolute, never accumulated", and the reason
    // a stream of any length is exactly contiguous.
    if (packetIndex_ > (std::numeric_limits<uint64_t>::max() - origin) / packetDurationNs_)
        return false;

    startNs = origin + packetIndex_ * packetDurationNs_;
    return startNs <= std::numeric_limits<uint64_t>::max() - packetDurationNs_;
}

bool ClientOutput::produce()
{
    uint64_t startNs = 0;
    if (!nextPacketStart(startNs))
    {
        failRoom(OutputError::ClockFailed,
                 "this room's stream has run past the end of the timeline");
        return false;
    }
    uint64_t endNs = startNs + packetDurationNs_;

    // Section 24's boundary, and the one place a packet is not a grid step.
    // The source before it is played to its end, so the packet that would have
    // crossed it is cut short to end exactly on it -- and the packet after it
    // is not built at all until switchTo() has said what it is made of.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        packetHoldNs_ = holdNs_;
        if (holdNs_ != 0)
        {
            if (startNs >= holdNs_)
                return false;
            if (endNs > holdNs_)
                endNs = holdNs_;
        }
    }

    // The zone's end, cut the same way: the last source is played to its last
    // frame, and nothing is built past it until the zone has more.
    const uint64_t end = endNs_.load();
    packetEndNs_ = end;
    if (end != 0)
    {
        if (startNs >= end)
            return false;
        endNs = std::min(endNs, end);
    }
    const uint64_t durationNs = endNs - startNs;
    const bool cutAtEnd = endNs == end && durationNs < packetDurationNs_;

    // Phase 39: as far as the zone lets this room get ahead of its line, and
    // waited at like a full buffer rather than counted as starved.
    if (const uint64_t ceiling = ceilingNs_.load(); ceiling != 0)
    {
        const uint64_t floor =
            std::to_underlying(originNs_) + settings_.targetBufferNs + packetDurationNs_;
        if (endNs > std::max(ceiling, floor))
            return false;
    }

    const Result<OutputFrame, OutputClockError> target = outputClock_.frameAt(TimelineNs{endNs});
    if (!target)
    {
        failRoom(OutputError::ClockFailed, describe(target.error()));
        return false;
    }

    const uint64_t wanted64 = std::to_underlying(*target) - std::to_underlying(framesSent_);
    if (wanted64 == 0 && endNs < startNs + packetDurationNs_)
    {
        // The boundary fell less than one frame past the last packet's end, so
        // there is no frame of this room's stream left before it. The stream
        // stops here rather than sending a packet with nothing in it, and is
        // counted as built up to the boundary, because that is the condition
        // applyFormatSwitch() waits for -- a stream that is contiguous to
        // within a position no frame occupies. The zone's end is the same.
        producedThroughNs_.store(endNs);
        cutAtEnd_ = cutAtEnd_ || cutAtEnd;
        return false;
    }
    if (wanted64 == 0 || wanted64 > std::numeric_limits<uint32_t>::max())
    {
        failRoom(OutputError::ClockFailed,
             "the running target asked for " + std::to_string(wanted64) + " frame(s)");
        return false;
    }
    const uint32_t wanted = static_cast<uint32_t>(wanted64);

    const std::span<const std::byte> payload = assemble(wanted);
    if (payload.empty())
    {
        // Either the decode has not reached this far -- which is normal, and
        // is what a room that has caught up with its zone looks like -- or
        // assemble() has already failed the room.
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == OutputState::Streaming)
            starved_.fetch_add(1);
        return false;
    }

    Result<std::string, PacketizerError> packet =
        packetizer_.encode(TimelineNs{startNs}, durationNs, wanted, payload);
    if (!packet)
    {
        failRoom(OutputError::PacketFailed, describe(packet.error()));
        return false;
    }

    framesSent_ = OutputFrame{std::to_underlying(framesSent_) + wanted};
    ++packetIndex_;
    producedThroughNs_.store(endNs);
    cutAtEnd_ = cutAtEnd;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        Packet queued;
        queued.bytes = std::move(*packet);
        queued.durationNs = durationNs;
        queued.frames = wanted;
        queue_.push_back(std::move(queued));
        queuedNs_ += durationNs;
        producedSinceReportNs_ += durationNs;
    }
    return true;
}

std::span<const std::byte> ClientOutput::assemble(uint32_t wanted)
{
    // The saving, where it is available: a pass-through room whose current
    // block already holds the whole packet sends the block's own bytes, so
    // nothing is copied before the packet itself is built.
    if (passThrough_.load() && packetFrames_ == 0 && (block_ != nullptr || nextBlock()))
    {
        const uint32_t remaining = block_->frameCount() - blockOffsetFrames_;
        if (remaining >= wanted)
        {
            const size_t offset = static_cast<size_t>(blockOffsetFrames_) * sourceBytesPerFrame_;
            const size_t length = static_cast<size_t>(wanted) * sourceBytesPerFrame_;
            const std::span<const std::byte> payload = block_->bytes().subspan(offset, length);

            blockOffsetFrames_ += wanted;
            if (blockOffsetFrames_ >= block_->frameCount())
            {
                block_.reset();
                blockOffsetFrames_ = 0;
            }
            return payload;
        }
    }

    if (!fill(wanted))
        return {};

    const std::span<const std::byte> payload(
        packetBuffer_.data(), static_cast<size_t>(wanted) * outputBytesPerFrame_);
    packetFrames_ = 0;
    return payload;
}

bool ClientOutput::fill(uint32_t wanted)
{
    const size_t needBytes = static_cast<size_t>(wanted) * outputBytesPerFrame_;
    if (packetBuffer_.size() < needBytes)
        packetBuffer_.resize(needBytes);

    while (packetFrames_ < wanted)
    {
        uint32_t carried = carryFrames_ - carryOffsetFrames_;
        if (carried == 0 && block_ == nullptr && !nextBlock())
        {
            // Silence finishes a packet only on a boundary or an end it was
            // sized for. Otherwise it is kept as far as it got, and the next
            // pass cuts it there.
            if (!silenceMayFinishPacket())
                return false;
            appendSilenceToCarry();
            carried = carryFrames_ - carryOffsetFrames_;
        }

        // What the converter is shown: the frames it would not take last time
        // with the block behind them where there are any, and otherwise the
        // rest of the block the cursor is in.
        const bool fromCarry = carried > 0;
        const uint32_t availableFrames =
            fromCarry ? carried : block_->frameCount() - blockOffsetFrames_;
        const std::span<const std::byte> input =
            fromCarry
                ? std::span<const std::byte>(
                      carry_.data() +
                          static_cast<size_t>(carryOffsetFrames_) * sourceBytesPerFrame_,
                      static_cast<size_t>(availableFrames) * sourceBytesPerFrame_)
                : block_->bytes().subspan(
                      static_cast<size_t>(blockOffsetFrames_) * sourceBytesPerFrame_,
                      static_cast<size_t>(availableFrames) * sourceBytesPerFrame_);

        // Exactly what is still missing, never whatever fits: a converter
        // asked for more than the packet holds would produce frames this
        // packet cannot carry, and a buffer for them is a buffer to bound.
        const std::span<std::byte> output(
            packetBuffer_.data() + static_cast<size_t>(packetFrames_) * outputBytesPerFrame_,
            static_cast<size_t>(wanted - packetFrames_) * outputBytesPerFrame_);

        uint32_t consumed = 0;
        uint32_t produced = 0;
        if (passThrough_.load())
        {
            consumed = std::min(availableFrames, wanted - packetFrames_);
            produced = consumed;
            std::memcpy(output.data(), input.data(),
                        static_cast<size_t>(consumed) * outputBytesPerFrame_);
        }
        else
        {
            const Result<ProcessResult, ProcessorError> result =
                processor_->process(input, output);
            if (!result)
            {
                failRoom(OutputError::ProcessorFailed, describe(result.error()));
                return false;
            }
            consumed = static_cast<uint32_t>(result->inputFramesConsumed);
            produced = static_cast<uint32_t>(result->outputFramesProduced);
        }

        packetFrames_ += produced;
        if (fromCarry)
        {
            carryOffsetFrames_ += consumed;
            if (carryOffsetFrames_ >= carryFrames_)
            {
                carry_.clear();
                carryFrames_ = 0;
                carryOffsetFrames_ = 0;
            }
        }
        else
        {
            blockOffsetFrames_ += consumed;
            if (blockOffsetFrames_ >= block_->frameCount())
            {
                block_.reset();
                blockOffsetFrames_ = 0;
            }
        }

        // A converter that took nothing and gave nothing has been shown less
        // than it needs for one output frame, which at a block boundary is
        // ordinary: interpolating needs the frame *after* the one it is
        // reading, and the last frame of a block does not have one until the
        // next block is behind it. That is what the carry is for -- the
        // processor's "the caller re-presents them with more behind them",
        // across a boundary the store hands over one block at a time.
        if (consumed == 0 && produced == 0 && !extendCarry(availableFrames))
            return false;
    }
    return true;
}

bool ClientOutput::extendCarry(uint32_t shown)
{
    // A converter shown a whole packet's worth of source and still not moving
    // is broken rather than short of input, and no amount of carrying will
    // change that. The ceiling is what keeps this bounded, as every buffer
    // here is.
    if (shown >= maxCarryFrames_)
    {
        failRoom(OutputError::ProcessorFailed,
                 "the converter neither consumed nor produced a frame from " +
                     std::to_string(shown) + " source frame(s)");
        return false;
    }

    // Whatever is left of the block the cursor is in goes first, so the carry
    // is the source's own order and not a rearrangement of it.
    if (carryOffsetFrames_ > 0)
    {
        carry_.erase(carry_.begin(),
                     carry_.begin() +
                         static_cast<std::ptrdiff_t>(carryOffsetFrames_) * sourceBytesPerFrame_);
        carryFrames_ -= carryOffsetFrames_;
        carryOffsetFrames_ = 0;
    }
    if (block_ != nullptr)
    {
        appendToCarry(block_->frameCount() - blockOffsetFrames_);
        block_.reset();
        blockOffsetFrames_ = 0;
    }

    if (!nextBlock())
    {
        // The decode has not got here yet, the room has failed, or the packet
        // was sized before the hold or the end it has met: the carry is kept,
        // and the next pass cuts the packet there.
        if (!silenceMayFinishPacket())
            return false;
        appendSilenceToCarry();
        return true;
    }

    appendToCarry(block_->frameCount() - blockOffsetFrames_);
    block_.reset();
    blockOffsetFrames_ = 0;
    return true;
}

void ClientOutput::anchorAt(TimelineNs position)
{
    outputClock_ = OutputClock(processor_->plan().outputFormat.sampleRate, position);
    originNs_ = position;
    packetIndex_ = 0;
    framesSent_ = OutputFrame{0};
    producedThroughNs_.store(std::to_underlying(position));
}

void ClientOutput::clearAssembly()
{
    block_.reset();
    blockOffsetFrames_ = 0;
    carry_.clear();
    carryFrames_ = 0;
    carryOffsetFrames_ = 0;
    packetFrames_ = 0;
}

void ClientOutput::appendSilenceToCarry()
{
    // Half the carry ceiling is one packet's worth of source frames: enough
    // for any converter's tail, and a converter still not moving after two of
    // these is caught by extendCarry()'s ceiling rather than fed forever. All
    // zero bytes is silence in every layout SampleType names.
    const uint32_t frames = std::max<uint32_t>(maxCarryFrames_ / 2, 1);
    carry_.resize(carry_.size() + static_cast<size_t>(frames) * sourceBytesPerFrame_);
    carryFrames_ += frames;
}

bool ClientOutput::silenceMayFinishPacket() const
{
    const bool onBoundary = atBoundary_ && boundaryNs_ == packetHoldNs_;
    const bool onEnd = atEnd_ && packetEndNs_ != 0;
    return onBoundary || onEnd;
}

void ClientOutput::appendToCarry(uint32_t frames)
{
    if (frames == 0)
        return;

    const size_t bytes = static_cast<size_t>(frames) * sourceBytesPerFrame_;
    const size_t at = carry_.size();
    carry_.resize(at + bytes);
    std::memcpy(carry_.data() + at,
                block_->bytes().data() +
                    static_cast<size_t>(blockOffsetFrames_) * sourceBytesPerFrame_,
                bytes);
    carryFrames_ += frames;
}

bool ClientOutput::nextBlock()
{
    atEnd_ = false;
    const Result<NativePcmStore::Read, PcmStoreError> read = store_.read(reader_);
    if (!read)
    {
        if (read.error().code == PcmStoreError::AtFrontier)
        {
            // The decoder has not got here yet, which is not a fault -- or
            // never will, because the zone ended here (setEnd()).
            if (const uint64_t end = endNs_.load(); end != 0)
            {
                const Result<SourceFrame, TimelineError> last = timeline_.frameAt(TimelineNs{end});
                atEnd_ = last && std::to_underlying(store_.nextFrame()) >=
                                     std::to_underlying(*last);
            }
            return false;
        }

        // Evicted: the audio this cursor named has been dropped, which is a
        // room that stopped draining for long enough that the decode ran past
        // it -- a cut connection, a device that went away, a client that
        // stopped reading. It is not a fault of this room's and it does not
        // end it: the stream is stranded here and its zone gives it a position
        // the store still holds (section 43's rejoin).
        strandRoom(describe(read.error()));
        return false;
    }

    // Section 24's boundary, from the reading end. The block past it belongs
    // to the source that follows, which this room's converter may not be able
    // to read at all, so it is put back rather than taken -- and what the
    // converter still needs to finish the old source is silence, because the
    // old source has ended. This is also what covers a packet whose hold
    // arrived after it had started: the check in produce() and this read are
    // not one step, and fill() has that packet sized again on the hold.
    uint64_t holdNs = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        holdNs = holdNs_;
    }
    if (holdNs != 0)
    {
        const uint64_t at = std::to_underlying(read->block->firstFrame()) + read->frameOffset;
        const Result<SourceFrame, TimelineError> boundary = timeline_.frameAt(TimelineNs{holdNs});
        if (boundary && at >= std::to_underlying(*boundary))
        {
            if (const auto back = store_.seek(reader_, SourceFrame{at}); !back)
            {
                strandRoom(describe(back.error()));
                return false;
            }
            atBoundary_ = true;
            boundaryNs_ = holdNs;
            return false;
        }
    }

    if (read->block->format() != processor_->plan().sourceFormat)
    {
        // A zone that changed format where this room was not held. Section
        // 24's transition holds every room of a zone at the boundary before a
        // block of the new source is published, so meeting one here is a
        // failure with a name and not audio to reinterpret.
        failRoom(OutputError::InvalidPlan,
             "the zone published " + describeFormat(read->block->format()) +
                 " where this room's plan reads " +
                 describeFormat(processor_->plan().sourceFormat));
        return false;
    }

    block_ = read->block;
    blockOffsetFrames_ = read->frameOffset;
    return block_->frameCount() > blockOffsetFrames_;
}

void ClientOutput::drain()
{
    for (;;)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (state_ != OutputState::Streaming || sink_ == nullptr || queue_.empty())
            return;

        // The write happens under the lock. It is a non-blocking send of at
        // most one packet, and the alternative -- handing the sink out and
        // taking the lock again -- would be a pointer another thread may
        // replace while this one is inside it.
        const Packet& front = queue_.front();
        const std::span<const std::byte> rest(
            reinterpret_cast<const std::byte*>(front.bytes.data()) + writtenOfFront_,
            front.bytes.size() - writtenOfFront_);

        const Result<size_t, PacketSinkError> written = sink_->write(rest);
        if (!written)
        {
            // The socket and not the room: section 13 puts the session on the
            // control connection, so a dead audio connection costs this stream
            // nothing but the connection. What is queued and where this room
            // is on the timeline are both kept, and the next attach() resumes
            // exactly there -- which is section 43's "no discontinuity at all"
            // and is the normal case.
            const std::string detail = describe(written.error());
            lock.unlock();
            dropConnection(OutputError::SinkFailed, detail);
            return;
        }
        if (*written == 0)
            return; // the peer's window is closed; resumed next pass, waited on by nobody

        writtenOfFront_ += *written;
        bytes_.fetch_add(*written);
        if (writtenOfFront_ < front.bytes.size())
            continue;

        queuedNs_ -= front.durationNs;
        packets_.fetch_add(1);
        frames_.fetch_add(front.frames);
        queue_.pop_front();
        writtenOfFront_ = 0;
        queueFullSinceNs_ = 0;
    }
}

void ClientOutput::checkStall()
{
    std::string detail;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != OutputState::Streaming)
            return;

        // A room with no connection yet is not stalling: nothing is draining
        // the queue because nothing has attached, and a room that never
        // attaches is phase 11's readiness timeout rather than this one.
        if (sink_ == nullptr || queue_.size() < settings_.maxQueuedPackets)
        {
            queueFullSinceNs_ = 0;
            return;
        }

        const uint64_t nowNs = clock_.monotonicNowNs();
        if (queueFullSinceNs_ == 0)
        {
            queueFullSinceNs_ = nowNs;
            queueFull_.fetch_add(1);
            return;
        }
        const uint64_t fullForNs = nowNs - queueFullSinceNs_;
        if (fullForNs < settings_.queueStallTimeoutNs)
            return;

        detail = "the audio connection took no bytes for " +
                 std::to_string(fullForNs / 1000000ULL) + " ms with " +
                 std::to_string(queue_.size()) + " packet(s) queued";
    }

    // A connection that has stopped taking bytes while this room still owes
    // its buffer audio is a connection that has gone away without TCP
    // noticing, which is what a cut cable looks like from here. Closing it is
    // what tells the client, whose own audio stream binds every connection it
    // makes (section 43): it re-attaches, and this room resumes where it left
    // off or is rejoined, depending only on whether the decode ran past it in
    // the meantime.
    dropConnection(OutputError::QueueStalled, std::move(detail));
}

void ClientOutput::strandRoom(std::string detail)
{
    bool logged = false;
    uint64_t unlogged = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != OutputState::Streaming)
            return;

        // Everything built for the position this room is no longer at. The
        // audio connection is deliberately left attached: nothing is wrong
        // with it, and the packets that follow the rejoin go out on it.
        dropBuiltAudio();
        state_ = OutputState::Stranded;
        lastError_ = std::string(errorName(OutputError::StoreFailed)) + ": " + detail;
        logged = strandLog_.allow(clock_.monotonicNowNs(), unlogged);
    }

    // The thread is this one and cannot be joined from inside itself. It does
    // not need to be: pump() returns at its first check for as long as this
    // room is stranded, and rejoinAt() is what lets it produce again.
    if (logged)
        MULTIROOM_WARN("client output {}: stranded, {} ({} more since the last line)", client_.str(),
                 detail, unlogged);
    else
        MULTIROOM_DEBUG("client output {}: stranded, {}", client_.str(), detail);
}

void ClientOutput::dropConnection(OutputError code, std::string detail)
{
    std::unique_ptr<IPacketSink> sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sink_ == nullptr)
            return;
        sink = std::move(sink_);

        // The packet the old socket was half-way through has to go out whole
        // on the next one, exactly as a re-attach already arranges.
        writtenOfFront_ = 0;
        queueFullSinceNs_ = 0;
        lastDrop_ = std::string(errorName(code)) + ": " + detail;
    }

    sink->close();
    connectionDrops_.fetch_add(1);
    MULTIROOM_WARN("client output {}: audio connection let go, {}: {}", client_.str(),
             errorName(code), detail);
}

void ClientOutput::failRoom(OutputError code, std::string detail)
{
    std::unique_ptr<IPacketSink> sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == OutputState::Failed)
            return;
        state_ = OutputState::Failed;
        lastError_ = std::string(errorName(code)) + ": " + detail;
        sink = std::move(sink_);
    }

    if (sink != nullptr)
        sink->close();

    // One line per room and not one per packet: this is the end of a room, and
    // the logger takes a mutex.
    MULTIROOM_WARN("client output {}: {}", client_.str(), lastError_);
}

// ---------------------------------------------------------------------------

void ClientOutput::attach(std::unique_ptr<IPacketSink> sink)
{
    std::unique_ptr<IPacketSink> previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = std::move(sink_);
        sink_ = std::move(sink);

        // The new socket never saw the prefix of the packet the old one was
        // half-way through, and half a header is not a packet: this stream
        // begins again at that packet's first byte. The sequence number
        // carries on, which is what the client's own anchor-on-the-first-
        // packet-of-a-connection rule is there for.
        writtenOfFront_ = 0;
        queueFullSinceNs_ = 0;
    }

    if (previous != nullptr)
        previous->close();

    MULTIROOM_INFO("client output {}: audio connection {}", client_.str(),
             previous != nullptr ? "re-attached" : "attached");
}

void ClientOutput::detach()
{
    std::unique_ptr<IPacketSink> previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = std::move(sink_);
        writtenOfFront_ = 0;
        queueFullSinceNs_ = 0;
    }
    if (previous != nullptr)
        previous->close();
}

void ClientOutput::setDriftTrim(double trim)
{
    // The roadmap's phase 13, and it is not a defence in depth: a bitperfect
    // room is never given a ratio *under any circumstance*, so the rule is
    // written where the converter is rather than where the caller is. A room
    // planned without a correction also holds a converter that would refuse
    // one, which is the same answer arrived at twice and is deliberate.
    if (!driftCorrected_.load())
        return;

    // Zero is this field's "nothing pending", and a trim that is not a
    // positive number is a controller fault rather than a correction -- the
    // comparison is written so that a NaN fails it.
    if (!(trim > 0.0))
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    pendingTrim_ = trim;
}

void ClientOutput::setEqualizer(const EqualizerSettings& settings)
{
    std::lock_guard<std::mutex> lock(mutex_);
    pendingEqualizer_ = settings;
}

void ClientOutput::applyEqualizer()
{
    std::optional<EqualizerSettings> settings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings.swap(pendingEqualizer_);
    }
    if (!settings)
        return;

    equalizer_.configure(*settings);
    equalizing_.store(processor_->plan().mode == OutputMode::Resampled && equalizer_.shaping());
}

void ClientOutput::applyDriftTrim()
{
    double trim = 0.0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingTrim_ == 0.0)
            return;
        trim = pendingTrim_;
        pendingTrim_ = 0.0;
    }

    // Where it takes effect: the next packet's start, which is exactly the
    // frame count already asked for, so the clock re-anchors on a boundary
    // rather than part-way through a packet that has been sent.
    uint64_t at = 0;
    if (!nextPacketStart(at))
        return; // produce() is about to end this room by name

    // The converter first, because it is the one that can refuse. Its window
    // was fixed when its filter was designed (soxr commits one at configure),
    // and a deployment is free to configure a correction limit wider than it
    // -- which is a configuration to correct rather than a room to end, so
    // nothing moves and this room plays on at the trim it already had.
    if (const auto ratio = processor_->setRatio(nominalRatio_ * trim); !ratio)
    {
        // Once per room, like every other line here: a converter whose window
        // is too narrow for this deployment's limit stays too narrow, and a
        // correction arrives twice a second. The counter is what says how
        // often it has happened since.
        if (correctionsRefused_.fetch_add(1) == 0)
        {
            MULTIROOM_WARN("client output {}: converter refused a trim of {:.6f}: {}", client_.str(),
                     trim, describe(ratio.error()));
        }
        return;
    }

    // And the clock second. It accepts everything from half speed to double,
    // so a trim the converter took cannot be one this refuses -- reaching it
    // is this room's own arithmetic having failed, which is a room to end.
    if (const auto applied = outputClock_.setTrim(trim, TimelineNs{at}); !applied)
    {
        failRoom(OutputError::ClockFailed, describe(applied.error()));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        trim_ = trim;
    }
    corrections_.fetch_add(1);
}

void ClientOutput::onBufferStatus(const matp::BufferStatus& status)
{
    const uint64_t nowNs = clock_.monotonicNowNs();
    uint64_t endedGapNs = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bufferedNs_ = status.bufferedDurationNs;

        // What is still queued here is audio this room does not hold yet but is
        // going to, so it counts against the target from this report onwards.
        // Zeroing it instead would send that much audio twice over.
        producedSinceReportNs_ = queuedNs_;

        const uint64_t gapNs = lastReportNs_ && nowNs > *lastReportNs_ ? nowNs - *lastReportNs_ : 0;
        longestReportGapNs_ = std::max(longestReportGapNs_, gapNs);
        lastReportNs_ = nowNs;
        if (inReportGap_)
        {
            inReportGap_ = false;
            endedGapNs = gapNs;
        }
    }
    if (endedGapNs != 0)
    {
        MULTIROOM_INFO("client output {}: buffer status again after {} ms", client_.str(),
                 endedGapNs / 1000000ULL);
    }
}

// ---------------------------------------------------------------------------

OutputState ClientOutput::state() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string ClientOutput::lastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

std::string ClientOutput::lastDrop() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastDrop_;
}

OutputPlan ClientOutput::plan() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return plan_;
}

size_t ClientOutput::queuedPackets() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

double ClientOutput::driftTrim() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return trim_;
}

uint64_t ClientOutput::bufferedNs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return bufferedNs_;
}

uint64_t ClientOutput::producedSinceReportNs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return producedSinceReportNs_;
}

std::optional<uint64_t> ClientOutput::bufferReportAgeNs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!lastReportNs_)
        return std::nullopt;
    const uint64_t nowNs = clock_.monotonicNowNs();
    return nowNs > *lastReportNs_ ? nowNs - *lastReportNs_ : 0;
}

uint64_t ClientOutput::longestReportGapNs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return longestReportGapNs_;
}

bool ClientOutput::pacedOnLine() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return inReportGap_;
}

} // namespace multiroom
