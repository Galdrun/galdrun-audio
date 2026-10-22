#include "matp/AudioStream.h"

#include "util/Logger.h"

#include <chrono>
#include <thread>

namespace multiroom {
namespace matp {
namespace {

// How long the connection thread sleeps before trying a full ring buffer
// again. Short against a prebuffer measured in seconds, long enough not to
// spin a core that the audio thread wants.
const uint32_t kBufferFullWaitMs = 2;

// How long detach() waits for the connection thread to leave the ring buffer.
// It has one sleep to finish and one packet to push, so reaching this means
// something else is wrong and carrying on would be worse than saying so.
const uint32_t kDetachTimeoutMs = 2000;

// How long the connection thread waits for the owner thread to empty the ring
// buffer for a rejoin. It takes one polling interval plus one device period,
// so this is orders of magnitude more than it should ever need: reaching it
// means the render callback has stopped running, and a room whose device is
// gone is better off saying so than waiting on it forever.
const uint32_t kResyncTimeoutMs = 5000;

} // namespace

AudioStream::AudioStream(ServerConnection& connection, AudioRingBuffer& buffer)
    : connection_(connection), buffer_(buffer), accepting_(false), inHandler_(false)
{
}

void AudioStream::attach(const std::string& sessionId, const Identity& identity,
                         const AudioFormat& format)
{
    detach(); // whatever was running is not this stream, and it owned the reader

    // The connection thread is still in there, and the reader and the anchors
    // below are its own. The stream stays failed until it has let go.
    if (inHandler_.load())
        return;

    reader_.reset();
    hasSessionId_ = false;
    hasSequence_ = false;
    hasTimelineAnchor_ = false;
    expectedSequence_ = 0;
    expectedTimelineNs_ = 0;
    sessionIdHigh_ = 0;
    sessionIdLow_ = 0;

    bool dropStale = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessionId_ = sessionId;
        identity_ = identity;
        format_ = format;
        bytesPerFrame_ = format.bytesPerFrame();
        attachedGeneration_ = 0; // poll() binds the connection that is up now
        nextTimelineNs_ = 0;
        streamPushed_ = 0;
        map_.clear();
        hasContent_ = false;
        packets_ = 0;
        frames_ = 0;
        waits_ = 0;
        reattachments_ = 0;
        rejoins_ = 0;
        flushes_ = 0;
        resyncPending_ = false;
        resyncTimelineNs_ = 0;
        resyncAfterFlush_ = false;
        connectionChanged_ = false;
        lastError_.clear();

        // A reopened device attaches again on a connection that is still
        // carrying the audio of the last attach, and whatever packet was in
        // flight when that one detached is gone: decoding the socket from here
        // would begin in the middle of it. So the connection is dropped and
        // the next one bound in its place, which the server resumes on a
        // packet boundary (section 43).
        if (boundGeneration_ != 0 && boundGeneration_ == connection_.connectionCount() &&
            connection_.state() == ServerConnectionState::Connected)
        {
            abandonedGeneration_ = boundGeneration_;
            dropStale = true;
        }
    }

    // Outside the lock, as in fail(): the connection has a lock of its own.
    if (dropStale)
        connection_.dropConnection("the audio device was reopened");

    accepting_.store(true);
}

void AudioStream::detach()
{
    accepting_.store(false);

    // Ordered against onData(), which raises the flag before it reads
    // accepting_: a handler that got in first is seen here and waited for,
    // and one that has not started yet finds accepting_ already false.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kDetachTimeoutMs);
    while (inHandler_.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    if (inHandler_.load())
    {
        // Carrying on would hand the caller a ring buffer that still has a
        // live producer, and the next attach() the state that producer is
        // using. So the stream fails instead, and the connection it is stuck
        // on is dropped and never bound again: only a new one is certain to
        // be free of it.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            abandonedGeneration_ = connection_.connectionCount();
        }
        fail("the audio connection thread did not leave the ring buffer within " +
             std::to_string(kDetachTimeoutMs) + " ms");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    sessionId_.clear();
    attachedGeneration_ = 0;
    hasContent_ = false;
    // Whatever the connection thread was waiting for, it has stopped waiting:
    // accepting_ is what released it, and leaving the flag set would have the
    // next owner poll answer a rejoin that belongs to a stream that is gone.
    resyncPending_ = false;
    resyncAfterFlush_ = false;
    connectionChanged_ = false;
    // lastError_ is kept: it is why the stream stopped, and the status command
    // would otherwise report a torn-down stream as if nothing had happened.
}

void AudioStream::poll()
{
    if (!accepting_.load())
        return;

    // The state before the count, as in Session::poll(): a Connected read
    // first is never paired with the count of the socket before it.
    const ServerConnectionState connectionState = connection_.state();
    const uint64_t generation = connection_.connectionCount();

    std::string sessionId;
    Identity identity;
    bool replacing = false;
    uint64_t previousAttached = 0;
    uint64_t previousBound = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (attachedGeneration_ != 0 && generation == attachedGeneration_)
            return;

        if (generation == 0 || generation == abandonedGeneration_ ||
            connectionState != ServerConnectionState::Connected)
            return;

        // A connection that replaces the one this stream was bound to. The
        // session did not end with it -- it lives on the control connection
        // (section 13) -- so the stream does not either: it is bound again,
        // and the server resumes on the new socket (section 43). Where it
        // resumes decides the rest: the same timeline position costs nothing
        // at all, and a different one is a rejoin, which accept() sees as the
        // jump it is.
        replacing = attachedGeneration_ != 0;
        sessionId = sessionId_;
        identity = identity_;

        // Published before the STREAM_ATTACH goes out, not after: the server
        // may answer with audio before send() has even returned here, and the
        // connection thread must already see this connection as bound -- and,
        // on a replacement, throw away the half-received packet the old one
        // left behind -- when those first bytes arrive.
        previousAttached = attachedGeneration_;
        previousBound = boundGeneration_;
        attachedGeneration_ = generation;
        boundGeneration_ = generation;
        if (replacing)
            connectionChanged_ = true;
    }

    // Sent outside the lock: it goes out on the calling thread, and the
    // connection has a lock of its own.
    const std::string frame = encode(MessageType::StreamAttach,
                                     makeStreamAttach(sessionId, identity));
    if (!connection_.send(frame.data(), frame.size()))
    {
        // The connection went away again; the next poll tries the new one.
        std::lock_guard<std::mutex> lock(mutex_);
        if (attachedGeneration_ == generation)
        {
            attachedGeneration_ = previousAttached;
            boundGeneration_ = previousBound;
        }
        return;
    }

    if (replacing)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++reattachments_;
    }

    if (replacing)
        MULTIROOM_INFO("audio connection re-attached to session %s", sessionId.c_str());
    else
        MULTIROOM_INFO("audio connection attached to session %s", sessionId.c_str());
}

void AudioStream::onData(const void* data, size_t size)
{
    // Raised before accepting_ is read, which is what lets detach() wait for
    // this handler instead of guessing whether one had already started.
    inHandler_.store(true);

    if (accepting_.load())
    {
        uint64_t generation = 0;
        uint64_t abandoned = 0;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            generation = attachedGeneration_;
            abandoned = abandonedGeneration_;
            changed = connectionChanged_;
            connectionChanged_ = false;
        }

        if (changed)
        {
            // Whatever the old socket left mid-packet is not the start of
            // anything on this one, and the server's numbering starts wherever
            // it happens to be. The timeline position is deliberately *not*
            // reset: it is what tells a seamless resume from a rejoin.
            reader_.reset();
            hasSequence_ = false;
        }

        // Not on a connection the stream has failed on: it is being dropped,
        // and what is still arriving on it is the rest of a byte stream that
        // is out of step.
        if (generation != 0 && generation != abandoned &&
            generation == connection_.connectionCount())
        {
            reader_.append(data, size);
            decode();
        }
        else
        {
            // Audio on a connection this client has not bound to a session
            // yet. The server cannot know which stream it belongs to either,
            // so there is nothing here worth keeping.
            MULTIROOM_DEBUG("ignoring %lu audio byte(s) on an unattached connection",
                      static_cast<unsigned long>(size));
        }
    }

    inHandler_.store(false);
}

void AudioStream::decode()
{
    for (;;)
    {
        AudioPacket packet;
        std::string error;
        const AudioPacketReader::Result result = reader_.next(packet, error);
        if (result == AudioPacketReader::Result::Incomplete)
            return;
        if (result == AudioPacketReader::Result::Invalid)
        {
            // The byte stream is out of step and nothing but a new connection
            // puts it back, exactly as on the control connection.
            fail(error);
            return;
        }

        if (!accept(packet))
            return;
    }
}

bool AudioStream::accept(const AudioPacket& packet)
{
    AudioFormat format;
    uint32_t bytesPerFrame = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        format = format_;
        bytesPerFrame = bytesPerFrame_;
    }

    if (packet.header.protocolVersion != kProtocolVersion)
    {
        fail("audio packet claims protocol version " +
             std::to_string(packet.header.protocolVersion) + ", this client speaks " +
             std::to_string(kProtocolVersion));
        return false;
    }

    if (packet.header.format != format)
    {
        // The client confirmed one format in STREAM_READY and the server must
        // send that one (section 12). Playing anything else through a buffer
        // sized in frames of the confirmed format is noise, not a near miss.
        fail("audio packet is " + describeFormat(packet.header.format) +
             ", this client confirmed " + describeFormat(format));
        return false;
    }

    if (!hasSessionId_)
    {
        sessionIdHigh_ = packet.header.sessionIdHigh;
        sessionIdLow_ = packet.header.sessionIdLow;
        hasSessionId_ = true;
    }
    else if (packet.header.sessionIdHigh != sessionIdHigh_ ||
             packet.header.sessionIdLow != sessionIdLow_)
    {
        // Section 13's purpose for the id: this stream carries exactly one
        // session, so a different one on it is a stale or crossed stream. The
        // id is compared for change rather than against SESSION_CONFIG's,
        // which is a string and cannot be matched against 128 bits.
        fail("audio packets changed session id mid-stream");
        return false;
    }

    if (!hasSequence_)
    {
        // The first packet of a connection sets the numbering: a server that
        // resumes on a new socket may carry its counter on or begin again,
        // and neither says anything about the audio.
        expectedSequence_ = packet.header.sequenceNumber;
        hasSequence_ = true;
    }
    else if (packet.header.sequenceNumber != expectedSequence_)
    {
        // TCP does not lose or reorder, so a gap on one connection is the
        // server skipping ahead rather than the network.
        fail("audio packet " + std::to_string(packet.header.sequenceNumber) +
             " arrived where " + std::to_string(expectedSequence_) + " was expected");
        return false;
    }

    // The first packet after a FLUSH parks whether or not it jumps: the flush
    // empties the buffer, so even a packet that happens to continue where the
    // last one ended has nothing left in front of it to continue from. It
    // parks even as the first packet of a stream: a FLUSH that has already cut
    // waits for it, and with no line to keep the owner turns it into a rejoin.
    const bool afterFlush = (packet.header.flags & kAudioFlagAfterFlush) != 0;
    if (afterFlush ||
        (hasTimelineAnchor_ && packet.header.timelineStartNs != expectedTimelineNs_))
    {
        // A jump on the media timeline: a rejoin after a gap, or a hard resync
        // (sections 43 and 32). The ring buffer holds frames and not
        // positions, so what is in it has to go before this can land -- and
        // only the thread that reads it can empty it.
        //
        // Judged on the packet's own timeline span alone. A server that
        // resamples this room sends a few more or a few fewer frames than its
        // nominal rate implies, every packet, for as long as it is correcting,
        // and at 44.1 kHz a frame is not a whole nanosecond either -- so an
        // expectation built from the frame count and the rate would call every
        // second packet a rejoin and this room would never play anything.
        //
        // A flagged packet parks the same way, and the owner thread decides
        // whether it ends a flush or a rejoin: the FLUSH it follows is on the
        // control connection and may not have arrived yet.
        if (!awaitResync(packet.header.timelineStartNs, afterFlush))
            return false;
    }

    if (!hasTimelineAnchor_)
    {
        // The first packet sets where the stream is: section 15 is explicit
        // that the timeline position comes from the packet and never from
        // when it arrived. That is as true of a late join (section 44) as of
        // a stream that starts at position zero.
        std::lock_guard<std::mutex> lock(mutex_);
        nextTimelineNs_ = packet.header.timelineStartNs;
        hasContent_ = true;
    }
    hasTimelineAnchor_ = true;

    if (!awaitTimelineRoom())
        return false;

    {
        // The segment goes in before the frames do. It describes what this
        // packet *is* on both axes, which is true whether or not a full ring
        // buffer makes the push take several passes, and the owner thread may
        // ask about the position of a frame the moment it lands.
        TimelineSegment segment;
        segment.timelineStartNs = packet.header.timelineStartNs;
        segment.timelineDurationNs = packet.header.timelineDurationNs;
        segment.streamFrameCount = packet.header.outputFrameCount;

        std::lock_guard<std::mutex> lock(mutex_);
        segment.streamStartFrame = streamPushed_;
        map_.append(segment);
    }

    if (!push(packet, bytesPerFrame))
        return false;

    expectedSequence_ = packet.header.sequenceNumber + 1;
    expectedTimelineNs_ =
        packet.header.timelineStartNs + packet.header.timelineDurationNs;

    std::lock_guard<std::mutex> lock(mutex_);
    // Snapped to the packet's own end rather than left wherever the
    // proportional advance inside push() reached, so the rounding of a partial
    // push cannot accumulate over a stream that runs for hours.
    nextTimelineNs_ = expectedTimelineNs_;
    ++packets_;
    frames_ += packet.header.outputFrameCount;
    // Audio accepted is the stream recovered: an error from before it is not
    // why the stream stopped any longer (lastError() is empty while healthy).
    lastError_.clear();
    return true;
}

bool AudioStream::push(const AudioPacket& packet, uint32_t bytesPerFrame)
{
    const unsigned char* frames = packet.payload;
    const uint32_t frameCount = packet.header.outputFrameCount;
    const uint64_t durationNs = packet.header.timelineDurationNs;
    const uint64_t startNs = packet.header.timelineStartNs;

    uint32_t written = 0;
    bool waited = false;

    while (written < frameCount)
    {
        if (!accepting_.load())
            return false; // detached, or the stream has already failed

        uint32_t pushed = 0;
        {
            // The write and the timeline position it puts frames at are
            // published together. Advanced as the frames land rather than once
            // the packet is whole -- a packet that is half in the buffer has
            // put half its frames on the timeline -- and a status taken
            // between the two would otherwise count frames it has no position
            // for. The wait below is deliberately outside this.
            std::lock_guard<std::mutex> lock(mutex_);
            pushed = buffer_.push(frames + static_cast<size_t>(written) * bytesPerFrame,
                                  frameCount - written);
            written += pushed;
            streamPushed_ += pushed;
            // The timeline position of a partial push is that fraction of the
            // packet's own span, not a duration derived from the frames that
            // landed: nanoseconds and frames are different units, which is
            // what this whole split is about. accept() snaps it to the exact
            // end once the packet is whole, so the rounding here lives for one
            // push and no longer.
            nextTimelineNs_ =
                startNs + (static_cast<uint64_t>(written) * durationNs) / frameCount;
            if (pushed == 0 && !waited)
            {
                waited = true;
                ++waits_;
            }
        }

        if (pushed == 0)
        {
            // The buffer is full, which is what the prebuffer reaching its
            // target looks like from this end. Waiting closes the receive
            // window and lets the server pace itself; dropping the frames
            // would silently put a hole in the timeline instead.
            std::this_thread::sleep_for(std::chrono::milliseconds(kBufferFullWaitMs));
            continue;
        }
    }
    return true;
}

bool AudioStream::awaitTimelineRoom()
{
    // The map counts packets and the ring buffer frames. Packets smaller than
    // section 15 asks for fill the map first, and overwriting a segment whose
    // audio is still buffered would leave that audio with no timeline
    // position -- status() would then report no content and the room could
    // never be started. Waiting paces the server exactly as a full buffer does.
    bool waited = false;
    for (;;)
    {
        if (!accepting_.load())
            return false; // detached, or the stream has already failed

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const uint64_t head = streamPushed_ - buffer_.readableFrames();
            if (!map_.full() || map_.oldestStreamEnd() <= head)
                return true;
            if (!waited)
            {
                waited = true;
                ++waits_;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kBufferFullWaitMs));
    }
}

// Sections 43 and 32: the server has moved this room to another timeline
// position. Nothing is dropped and nothing is asked for again -- the thread
// simply stops reading the socket, which closes the receive window behind it
// exactly as a full ring buffer does, so the audio of the new position is
// still there when the old one has been cleared away.
bool AudioStream::awaitResync(uint64_t timelineNs, bool afterFlush)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        resyncTimelineNs_ = timelineNs;
        resyncPending_ = true;
        resyncAfterFlush_ = afterFlush;
        if (!afterFlush)
            ++rejoins_;
    }

    MULTIROOM_INFO("the server moved this room from timeline %llu ns to %llu ns; %s",
             static_cast<unsigned long long>(expectedTimelineNs_),
             static_cast<unsigned long long>(timelineNs),
             afterFlush ? "after a flush" : "rejoining");

    // A deadline rather than a count of sleeps: a sleep is rounded up to the
    // scheduler's tick, which on Windows makes 2 ms nearer 16.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kResyncTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!accepting_.load())
            return false; // detached, or the stream has already failed

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!resyncPending_)
                return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kBufferFullWaitMs));
    }

    fail("the timeline that was buffered was not cleared for the rejoin within " +
         std::to_string(kResyncTimeoutMs) + " ms");
    return false;
}

void AudioStream::fail(const std::string& reason)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_ = reason;
        // The connection is what failed, not the stream: the session lives on
        // the control connection (section 13), so poll() binds the next
        // connection as it would after any drop (section 43), and this one is
        // never bound again. detach() has already named its own.
        if (attachedGeneration_ != 0)
            abandonedGeneration_ = attachedGeneration_;
        // A rejoin nobody will finish: left set, the owner would resume a
        // stream whose connection is already gone.
        resyncPending_ = false;
        resyncAfterFlush_ = false;
    }

    MULTIROOM_ERROR("audio stream failed: %s", reason.c_str());
    connection_.dropConnection(reason);
}

bool AudioStream::isAttached() const
{
    return accepting_.load();
}

bool AudioStream::resyncPending() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return resyncPending_;
}

uint64_t AudioStream::resyncTimelineNs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return resyncTimelineNs_;
}

bool AudioStream::resyncAfterFlush() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return resyncPending_ && resyncAfterFlush_;
}

void AudioStream::resume()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!resyncPending_)
        return;

    // Published here rather than left to the connection thread's next pass:
    // the buffer is empty from this instant, and a BUFFER_STATUS taken before
    // that thread wakes would otherwise report the position of a timeline
    // nobody holds any more.
    nextTimelineNs_ = resyncTimelineNs_;
    // The segments describe a timeline this room has stopped playing. Kept,
    // they would answer a lookup for a position that means nothing here any
    // more -- and the stream axis runs on across the rejoin, so a stale
    // segment is not even distinguishable by its own coordinates.
    map_.clear();
    hasContent_ = true;
    resyncPending_ = false;

    // The line was lost after all, which is what the counter means.
    if (resyncAfterFlush_)
        ++rejoins_;
    resyncAfterFlush_ = false;
}

void AudioStream::resumeAfterFlush(uint64_t streamFrame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!resyncPending_)
        return;

    nextTimelineNs_ = resyncTimelineNs_;
    // The segments describe audio before the cut, and the axis they are on is
    // about to jump, so they would answer lookups on the new audio wrongly.
    map_.clear();
    // The buffer is empty and nothing has been pushed since, so the next frame
    // lands at `streamFrame`. It may be behind frames pushed before the cut and
    // then dropped, and that is fine: none of them is in the buffer any more,
    // and nothing else numbers frames on this axis.
    streamPushed_ = streamFrame;
    hasContent_ = true;
    resyncPending_ = false;
    resyncAfterFlush_ = false;
    ++flushes_;
}

BufferStatus AudioStream::status() const
{
    BufferStatus out;

    std::lock_guard<std::mutex> lock(mutex_);
    out.streamFrames = buffer_.readableFrames();
    out.hasContent = hasContent_;
    if (!hasContent_)
        return out;

    // Both ends of what the buffer holds. The newest is the frontier the
    // connection thread keeps; the oldest is where the buffer's head sits on
    // the media timeline, which is a map lookup and not a subtraction -- a
    // frame is not a nanosecond. A pop between the two reads leaves the
    // playhead one device period stale, which under-claims this room's
    // progress rather than over-claiming it.
    out.bufferedUntilTimelineNs = nextTimelineNs_;

    if (out.streamFrames == 0)
    {
        // An empty buffer needs no lookup: the room holds nothing, so both
        // ends are the frontier. This is also the state a rejoin leaves
        // behind -- the buffer dropped and the mapping cleared -- and section
        // 43 step 5 has the server schedule the rejoin from exactly this
        // report, so answering it with "no content" would stall the rejoin
        // until the first packet of the new position happened to land.
        out.playheadTimelineNs = nextTimelineNs_;
        out.bufferedDurationNs = 0;
        return out;
    }

    const uint64_t head = streamPushed_ - out.streamFrames;
    if (!map_.streamToTimeline(head, out.playheadTimelineNs))
    {
        // The audio in the buffer is older than the mapping still covers, so
        // there is no honest timeline position to report for it. Section 19 is
        // answered with the positions left out rather than with a guess.
        out.hasContent = false;
        return out;
    }

    out.bufferedDurationNs = out.bufferedUntilTimelineNs > out.playheadTimelineNs
                                 ? out.bufferedUntilTimelineNs - out.playheadTimelineNs
                                 : 0;
    return out;
}

bool AudioStream::timelineToStream(uint64_t timelineNs, uint64_t& streamFrame) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.timelineToStream(timelineNs, streamFrame);
}

bool AudioStream::streamToTimeline(uint64_t streamFrame, uint64_t& timelineNs) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.streamToTimeline(streamFrame, timelineNs);
}

uint64_t AudioStream::bufferHeadStreamFrame() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t buffered = buffer_.readableFrames();
    return streamPushed_ > buffered ? streamPushed_ - buffered : 0;
}

uint32_t AudioStream::capacityFrames() const
{
    // Configured only while the owner thread has the stream detached, which
    // is the thread that asks.
    return buffer_.capacityFrames();
}

uint64_t AudioStream::packets() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return packets_;
}

uint64_t AudioStream::frames() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_;
}

uint32_t AudioStream::waits() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return waits_;
}

uint32_t AudioStream::reattachments() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return reattachments_;
}

uint32_t AudioStream::rejoins() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return rejoins_;
}

uint32_t AudioStream::flushes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return flushes_;
}

std::string AudioStream::lastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

} // namespace matp
} // namespace multiroom
