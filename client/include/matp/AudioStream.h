#pragma once

// The audio connection: the second TCP connection of MATP section 3, and the
// single producer of the ring buffer the render callback reads from.
//
// Threading, which is what this class is shaped around:
//
//   * onData() runs on the audio connection's own thread. It decodes packets
//     and pushes their frames straight into the ring buffer -- it is the
//     producer, and it is the only one. Unlike matp::Session, which buffers
//     bytes for its owner to decode, nothing is handed to another thread:
//     audio that went through a second queue on the way to the ring buffer
//     would only be buffered twice.
//   * attach(), detach(), poll() and every accessor run on the thread that
//     owns the client's state. A small mutex covers what the two share, which
//     is the stream's configuration and its counters -- fifty packets a second
//     is not contention, and the audio thread never touches any of it.
//
// Back pressure is TCP's. When the ring buffer is full the connection thread
// waits inside onData() rather than dropping frames, the socket's receive
// window closes, and the server stops sending: that is the prebuffer of
// section 18 doing its job, not a stall. Dropping frames instead would put a
// hole in the timeline, which is the one thing the buffer exists to prevent.
//
// The same waiting is what a rejoin is built on (sections 43 and 32). The
// ring buffer holds frames and not positions, so a jump on the zone's media
// timeline cannot be represented in it: everything buffered has to go first,
// and only the thread that reads the buffer may empty it. So the connection
// thread parks on the jump exactly as it parks on a full buffer, the owner
// thread drops the old timeline and calls resume(), and the stream carries on
// at the new position with nothing of it lost.

#include "audio/AudioFormat.h"
#include "audio/AudioRingBuffer.h"
#include "matp/AudioPacket.h"
#include "matp/Protocol.h"
#include "matp/TimelineMap.h"
#include "net/ServerConnection.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace multiroom {
namespace matp {

class AudioStream
{
public:
    AudioStream(ServerConnection& connection, AudioRingBuffer& buffer);

    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;

    // Owner thread. Starts accepting audio for one session, in one format:
    // the format the device granted and the client confirmed in STREAM_READY.
    // The ring buffer must already be configured for it, which is what opening
    // the device does.
    void attach(const std::string& sessionId, const Identity& identity,
                const AudioFormat& format);

    // Owner thread. Stops accepting and returns only once the connection
    // thread has left the ring buffer, so the caller may reconfigure it
    // afterwards -- the same barrier the test tone gets by joining its
    // producer around each open attempt. A thread that has not left within
    // two seconds fails the stream and has its connection dropped, and
    // attach() then refuses to start until it has gone.
    void detach();

    // Owner thread. Binds the audio connection to this session by sending
    // STREAM_ATTACH -- the one that is up now, and every one that replaces it
    // after a drop (section 43). The stream survives the socket it was made
    // on: the session did not end, so neither did what the server is sending.
    void poll();

    // Owner thread. A jump on the media timeline: the connection thread has
    // parked and nothing more will reach the ring buffer until resume() says
    // the old timeline is gone. resyncTimelineNs() is where the stream carries
    // on, and it is meaningful only while one is pending.
    bool resyncPending() const;
    uint64_t resyncTimelineNs() const;

    // Owner thread. Whether the pending jump is the first packet after a
    // FLUSH (kAudioFlagAfterFlush, section 54) rather than a rejoin. The
    // stream parks on it for the same reason, and on such a packet even when
    // it does not jump: the flush emptied the buffer, so the positions the
    // stream holds for it are gone either way.
    bool resyncAfterFlush() const;

    // Owner thread: the ring buffer is empty and nothing is scheduled against
    // the timeline that was in it, so the audio of the new position may land.
    // A flagged jump resumed this way was a rejoin after all, and is counted
    // as one.
    void resume();

    // Owner thread: the flush's other ending. The ring buffer is empty and the
    // playback layer has placed the audio after the cut on the line it kept;
    // `streamFrame` is where that puts the packet on the stream axis. The axis
    // jumps there with the timeline, so the start mapping the room is playing
    // on stays true for the new audio.
    void resumeAfterFlush(uint64_t streamFrame);

    // Connection thread.
    void onData(const void* data, size_t size);

    bool isAttached() const;

    // What BUFFER_STATUS reports (section 19), built from what the ring buffer
    // actually holds rather than from what was received: frames that have been
    // played are no longer buffered.
    BufferStatus status() const;

    // Owner thread. The zone's media timeline against this room's own stream
    // of PCM frames, which is the conversion every scheduled event needs: the
    // server names a timeline position and the playback layer works on the
    // stream. See matp/TimelineMap.h for what each direction can and cannot
    // answer.
    //
    // The stream axis counts frames pushed into the ring buffer since the
    // stream was attached. It is not the device's frame axis -- the device was
    // running, and counting silence, long before the first packet arrived --
    // and TimelineSource is what ties the two together at the start.
    bool timelineToStream(uint64_t timelineNs, uint64_t& streamFrame) const;
    bool streamToTimeline(uint64_t streamFrame, uint64_t& timelineNs) const;

    // Owner thread. Where the oldest frame the ring buffer still holds sits on
    // that stream axis, which is what a start has to be measured from: the
    // frames between it and the start are the ones to drop.
    uint64_t bufferHeadStreamFrame() const;

    // What the ring buffer holds at most, in frames of the stream's format.
    uint32_t capacityFrames() const;

    // Packets accepted and frames handed to the ring buffer, for diagnostics
    // and tests.
    uint64_t packets() const;
    uint64_t frames() const;

    // How often the connection thread had to wait for the consumer. Normal
    // while the buffer fills before a start, and whenever the buffer is
    // smaller than what the server keeps it filled to -- it is what a full
    // buffer looks like from the producer's end.
    uint32_t waits() const;

    // Audio connections this stream has been bound to after the first, and
    // timeline positions it has been moved to. The two are counted apart
    // because they say different things: one is the network, the other is the
    // server deciding where this room belongs.
    uint32_t reattachments() const;
    uint32_t rejoins() const;

    // Flushes this stream carried on the line it was playing: seeks, from the
    // room's end (section 54).
    uint32_t flushes() const;

    // Why the stream stopped accepting audio; empty while it is healthy.
    std::string lastError() const;

private:
    // Connection thread. Decodes and pushes everything that is complete.
    void decode();
    bool accept(const AudioPacket& packet);
    // The packet's timeline span is carried through so that a packet which
    // only half fits advances the timeline position by half of what it covers:
    // a status taken between two pushes must not count frames it has no
    // position for.
    bool push(const AudioPacket& packet, uint32_t bytesPerFrame);
    // Connection thread. Parks while the timeline map is full and its oldest
    // segment still describes buffered audio, so appending cannot leave that
    // audio without a position; false when detached or failed meanwhile.
    bool awaitTimelineRoom();
    // Connection thread. Parks until the owner thread has emptied the ring
    // buffer of the timeline that was in it; false when the stream was
    // detached or failed while waiting, and the caller then stops decoding.
    bool awaitResync(uint64_t timelineNs, bool afterFlush);
    void fail(const std::string& reason);

    ServerConnection& connection_;
    AudioRingBuffer& buffer_;

    // Guards everything the two threads share. Never taken on the audio
    // thread, which reaches the same frames through the ring buffer alone.
    mutable std::mutex mutex_;

    std::string sessionId_;
    Identity identity_;
    AudioFormat format_;
    uint32_t bytesPerFrame_ = 0;

    // Which connection this stream was bound to. Bytes from any other one
    // belong to a connection nobody attached, and a stream whose connection
    // has been replaced is not this stream any more.
    uint64_t attachedGeneration_ = 0;
    // The connection that has been dropped and is never bound again: the one
    // detach() gave up waiting on with the connection thread still inside the
    // ring buffer, the one the stream failed on, or the one a re-attach found
    // still carrying the last attach's audio. Kept across attaches on purpose.
    uint64_t abandonedGeneration_ = 0;
    // The connection STREAM_ATTACH last went out on. Unlike
    // attachedGeneration_ it survives detach(), which is how attach() tells a
    // connection that has carried audio from one that has not.
    uint64_t boundGeneration_ = 0;

    // One past the newest frame in the ring buffer, as a position on the media
    // timeline, advanced as frames land rather than once a packet is whole.
    uint64_t nextTimelineNs_ = 0;
    bool hasContent_ = false;

    // The same frontier on this room's own axis: output frames pushed into the
    // ring buffer since the stream was attached. The oldest frame still in the
    // buffer is this minus what the buffer holds -- which is a subtraction the
    // timeline frontier above cannot do at all, because nanoseconds and frames
    // are not the same unit.
    uint64_t streamPushed_ = 0;

    // Timeline against stream, one segment per packet. Written by the
    // connection thread inside accept(), read by the owner thread; the mutex
    // above is what covers both.
    TimelineMap map_;

    uint64_t packets_ = 0;
    uint64_t frames_ = 0;
    uint32_t waits_ = 0;
    uint32_t reattachments_ = 0;
    uint32_t rejoins_ = 0;
    uint32_t flushes_ = 0;
    std::string lastError_;

    // A jump the connection thread has seen and the owner thread has not
    // cleared yet, where the stream goes on once it has, and whether the
    // packet said it follows a flush.
    bool resyncPending_ = false;
    uint64_t resyncTimelineNs_ = 0;
    bool resyncAfterFlush_ = false;

    // Set by poll() when it binds a connection that replaces the one the
    // stream was on, cleared by the connection thread when it has thrown away
    // what the old socket left in the packet reader. It is safe as a plain
    // flag under the mutex because the server sends no audio on a connection
    // before the STREAM_ATTACH that poll() sends on it.
    bool connectionChanged_ = false;

    // The fast path's copy of "still wanted": read once per packet and inside
    // the wait for buffer space, where taking the mutex would mean holding it
    // across a sleep.
    std::atomic<bool> accepting_;

    // Set while the connection thread is inside onData(), so detach() can wait
    // for it to leave rather than guess.
    std::atomic<bool> inHandler_;

    // Connection thread only, from attach() until detach().
    AudioPacketReader reader_;
    // The session id and the sequence are anchored per connection: the id
    // must not change on one, and a new one starts the numbering wherever the
    // server left it. The timeline position is anchored per *stream* and
    // outlives any one connection -- that is what makes a reconnect the
    // server resumes seamlessly cost nothing, and a jump a rejoin.
    bool hasSessionId_ = false;
    bool hasSequence_ = false;
    bool hasTimelineAnchor_ = false;
    uint64_t expectedSequence_ = 0;
    // timelineStartNs + timelineDurationNs of the last packet. Timeline
    // progress comes from the packet's own duration and never from its frame
    // count and rate, which is the whole point of the two being separate
    // fields (section 14).
    uint64_t expectedTimelineNs_ = 0;
    uint64_t sessionIdHigh_ = 0;
    uint64_t sessionIdLow_ = 0;
};

} // namespace matp
} // namespace multiroom
