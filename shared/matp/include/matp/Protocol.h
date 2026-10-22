#pragma once

// MATP control-connection wire format: framing, message types, and the JSON
// bodies the handshake is made of.
//
// Shared by both halves of the project, and that is the point: message ids,
// framing, the sample-format table and every JSON field name have to be
// identical on the two ends of the socket, and the only way to guarantee that
// is for there to be one copy. What each half *does* with a message -- the
// client's Session and AudioStream, the server's ClientSession -- is its own
// state machine and stays in its own tree.
//
// Pure: it builds and parses bytes, opens no socket and touches no state on
// either side, which is what makes the handshake testable without a server and
// without a client. Whichever side of an exchange a function belongs to is
// named by its verb -- `make` writes what this process sends, `parse` reads
// what the peer sent -- and both directions live here so that a round trip can
// be asserted in one test.
//
// C++17, like everything under shared/: the client targets a 512 MB ARMv6
// board and a C++20 feature that slips in here compiles in the server's build
// and fails in the client's. The result convention is the client's too --
// `bool f(..., std::string& error)` rather than std::expected -- and the
// server wraps it at its own edge (ServerCore/src/matp/) where the two
// language levels are allowed to meet.
//
// Framing (MATP section 16):
//
//   uint32 messageLength    big endian
//   uint16 messageType
//   payload                 compact JSON, no terminator
//
// Two things section 16 leaves open, and a client cannot:
//
//   * `messageLength` counts the type field as well as the payload -- the
//     length of the *message*, the message being everything after the prefix.
//   * the section gives message types but no numbers for them. They are
//     assigned below, in the order of section 47's control list. Because the
//     numbers are this client's and not the protocol's, the JSON "type" string
//     is what dispatch reads; a number this client knows must agree with it,
//     and a number it does not know is accepted, so a later message type
//     cannot break a client that predates it.
//
// Byte order is big endian throughout, which is what section 14 asks for the
// audio header -- one order for the whole protocol rather than two.
//
// Every audio position in every message here is a **timeline nanosecond**
// (section 5). A frame count in this file is one client's own and is named so:
// `periodFrames`, `deviceFramePosition`, `streamFrames`. The two are both
// integers and the compiler cannot tell them apart, which is why the naming is
// strict (section 6).

#include "audio/AudioClockSnapshot.h"
#include "audio/AudioFormat.h"
#include "system/SystemSampler.h"
#include "util/Json.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace multiroom {
namespace matp {

const uint32_t kProtocolVersion = 2;

// Length prefix plus message type.
const size_t kHeaderBytes = 6;

// A control message claiming more than this is a desync or a broken server,
// not a large message: the biggest one either side sends here is an
// AUDIO_CAPABILITIES list of a few kilobytes.
const uint32_t kMaxMessageBytes = 1024 * 1024;

// Only what this client speaks. Later phases add their own; the numbers are
// stable once assigned, because a server keyed to them cannot be re-released
// in step with the client.
enum class MessageType : uint16_t
{
    Unknown = 0, // a type this client does not implement; carried by name only
    Hello = 1,
    HelloAck = 2,
    AudioCapabilities = 3,
    SessionConfig = 4,
    Error = 5,
    ClockSyncRequest = 6,
    ClockSyncReply = 7,
    PrepareStream = 8,
    StreamReady = 9,
    BufferStatus = 10,
    // The only message this client sends on the *audio* connection, and one
    // section 14 does not name: see makeStreamAttach below.
    StreamAttach = 11,
    StartAt = 12,
    PlaybackReport = 13,
    TrackBoundary = 14,
    PauseAt = 15,
    StopAt = 16,
    SetVolume = 17,
    TrackInfo = 18,
    Ping = 19,
    Pong = 20,
    // The four timestamps of a completed clock exchange, sent back so the
    // server can build a client/server clock model of its own (section 8).
    ClockSyncResult = 21,
    // Empty a room's buffer at a timeline position and keep its line: the
    // instant seek of section 54. See makeFlush below.
    Flush = 22,
    // How the room's machine is doing (section 55). See makeSystemReport.
    SystemReport = 23,
    // The room's sound cards and the one it plays on, and the server asking
    // for another (section 56). See makeAudioDevices and makeSetAudioDevice.
    AudioDevices = 24,
    SetAudioDevice = 25,
    // Run the capability scan again (section 56). See makeRescanAudio.
    RescanAudio = 26,
    // Hardware controls (section 57). See matp/Input.h.
    InputControls = 27,
    InputAction = 28,
    SetInputBindings = 29,
    // The size of the room's own buffer, set by the server and reported back
    // (section 58). See makeSetBuffer and makeBufferConfig.
    SetBuffer = 30,
    BufferConfig = 31,
    // How the room plays its volume, set by the server and reported back with
    // the path the client resolved (section 59). See makeSetVolumeControl
    // and makeVolumeConfig.
    SetVolumeControl = 32,
    VolumeConfig = 33,
    // The room's name, apart from its clientId: set by the server and
    // reported back (section 60). See makeSetRoomName and makeRoomName.
    SetRoomName = 34,
    RoomName = 35,
    // The room's drift-correction actuator, set by the server and answered
    // with AUDIO_CAPABILITIES, and the server's drift measurement and
    // hardware-clock command back to the client. See makeSetDriftCorrection
    // and makeDriftFeedback.
    SetDriftCorrection = 36,
    DriftFeedback = 37
};

// Section 59's two settings. Audio Control is the server's: whether the room
// must be bitperfect, may fall back, or has no preference. It decides the
// server's output plan and is told to the client in SESSION_CONFIG, because it
// also bounds what the client may do to the samples. Volume Control is the
// client's own (it persists it) and says how SET_VOLUME is played.
enum class AudioControl
{
    BitPerfect, // strict: the samples are never modified
    Auto        // bitperfect where possible, processing allowed where needed
};

enum class VolumeControlMode
{
    Auto,     // resolved from the Audio Control and the device (section 59)
    Hardware, // the device's own control; disabled where it has none
    Software, // the client scales the samples; never under BitPerfect
    Disabled  // SET_VOLUME's gain is not played; its mute still is
};

// "bitPerfect" | "auto", and
// "auto" | "hardware" | "software" | "disabled": the one spelling of each,
// shared by the wire, the configuration files and every API. The parser also
// takes "bitPerfectFallback", the old name for "auto", and never writes it.
const char* audioControlName(AudioControl control);
bool parseAudioControl(const std::string& name, AudioControl& out);
const char* volumeControlModeName(VolumeControlMode mode);
bool parseVolumeControlMode(const std::string& name, VolumeControlMode& out);

// How a room's drift is corrected when the server corrects it at all: the
// server resamples the stream,
// or the client trims its DAC clock on the server's command. It chooses the
// actuator, not whether the room is synchronized. The client keeps the
// setting; a peer that says nothing is Resampling, and the hardware clock
// stays at its nominal rate. Auto, a setting only, is the hardware clock
// where the device has a verified trim and resampling otherwise; the actuator
// a SESSION_CONFIG or STREAM_READY names is never Auto.
enum class DriftCorrectionMode
{
    Resampling,
    HardwareClock,
    Auto
};

// "resampling" | "hardwareClock" | "auto".
const char* driftCorrectionModeName(DriftCorrectionMode mode);
bool parseDriftCorrectionMode(const std::string& name, DriftCorrectionMode& out);

// The "type" string in the JSON body: "hello", "helloAck", ...
const char* messageTypeName(MessageType type);
bool parseMessageType(const std::string& name, MessageType& out);

// One decoded control message.
struct Message
{
    MessageType type = MessageType::Unknown;
    std::string typeName; // as it arrived, so an unknown type can be logged
    uint16_t wireType = 0;
    json::Value payload; // always an object
};

// Frame ready to hand to the socket.
std::string encode(MessageType type, const json::Value& payload);

// The receiving half: bytes in, whole messages out.
//
// TCP is a byte stream, so one read is not one message (section 16). Bytes are
// appended as they arrive -- possibly on the connection thread -- and decoded
// by whoever owns the reader.
class FrameReader
{
public:
    enum class Result
    {
        Message,    // out was filled
        Incomplete, // the rest of the message has not arrived yet
        Invalid     // the stream is out of step; only a new connection fixes it
    };

    void append(const void* data, size_t size);

    Result next(Message& out, std::string& error);

    void reset();

    size_t buffered() const { return buffer_.size(); }

private:
    std::string buffer_;
};

// Section 12 names a byte layout, not a bit depth: s24le3 (three packed bytes)
// and s24le (24 valid bits in a 32-bit container) are different layouts, and
// reporting the wrong one produces noise rather than a quiet defect.
const char* sampleFormatName(AudioFormat::SampleType type);
bool parseSampleFormat(const std::string& name, AudioFormat::SampleType& out);

// Who we are, as the server sees us (section 11).
struct Identity
{
    // Stable across restarts and reconnects: the server keys this client's
    // output stream, drift model and resampler state on it, so a client that
    // reports a new id on every start is treated as a new room.
    std::string clientId;
    std::string clientVersion;
    std::string platform;
};

json::Value makeHello(const Identity& identity);

// HELLO as the server reads it (section 11).
//
// `protocolVersion` is mandatory and is the whole reason this message is
// answered before any other: version 1 and version 2 disagree about what every
// position on the wire means, so a peer that does not say which one it speaks
// is refused rather than assumed to speak ours (section 51). `clientId` is
// mandatory for the reason section 11 gives -- the server keys the output
// stream, the drift model and the resampler state on it, and a room without a
// stable name cannot have any of the three.
//
// The other two are informational and optional. A client that reports no
// version or no platform is still a room that can be served; refusing it would
// be the parser deciding something the protocol does not.
struct Hello
{
    uint32_t protocolVersion = 0;
    Identity identity;
};

bool parseHello(const json::Value& payload, Hello& out, std::string& error);

// The selected device's own volume control (section 12). Where it has one,
// the client plays SET_VOLUME on it and sends the samples to the device
// unscaled; without one it scales them itself (section 39). Informational for
// the server either way: SET_VOLUME is the same message.
struct HardwareVolumeCapability
{
    bool available = false;
    std::string control; // the platform's name for it, for display
    uint32_t steps = 0;  // settable levels, at least 2 when available
    bool hasDecibels = false;
    double minDb = 0.0;
    double maxDb = 0.0;
    bool hasMute = false;
};

// The selected device's verified clock trim, in whole ppm relative to its
// nominal rate; positive slows the clock. Only a board whose firmware is known
// to retune cleanly reports one (Connector2's "Drift compensator"): a control
// of a similar name on other hardware is not evidence of the same behaviour.
struct HardwareClockCapability
{
    bool available = false;
    std::string control; // the platform's name for it, for display
    int32_t minPpm = 0;  // at most 0
    int32_t maxPpm = 0;  // at least 0, above minPpm
    uint32_t stepPpm = 1;
};

// What AUDIO_CAPABILITIES reports: the currently selected device and the
// formats it accepted in the access mode the client will open it in.
struct CapabilityReport
{
    std::string deviceId;
    std::string deviceName;
    uint32_t preferredSampleRate = 0; // a hint from the configuration
    std::vector<AudioFormat> formats;

    // "exclusive" (the device's own clock, nothing between the samples and the
    // card) or "shared" (through the system mixer or ALSA's plughw). Empty
    // when the client did not say, which a client from before the field does
    // not: unknown rather than either. `formats` are the ones of this mode.
    std::string accessMode;

    // False when the client did not say, which a client from before the field
    // existed does not: unknown rather than "none".
    bool hasHardwareVolume = false;
    HardwareVolumeCapability hardwareVolume;

    // The client reads DRIFT_FEEDBACK. A client from before it does not say,
    // and is sent none. `driftCorrection` is its setting, which the server
    // only honours together with this and a usable `hardwareClock`.
    bool driftFeedback = false;
    DriftCorrectionMode driftCorrection = DriftCorrectionMode::Resampling;
    bool hasHardwareClock = false;
    HardwareClockCapability hardwareClock;
};

json::Value makeAudioCapabilities(const CapabilityReport& report);

// The `hardwareVolume` object as AUDIO_CAPABILITIES carries it, for anything
// that reports it on: {"available": false} or the full description.
json::Value makeHardwareVolume(const HardwareVolumeCapability& volume);

// The most formats one AUDIO_CAPABILITIES may carry. A real scan of a real
// device produces a few dozen -- six rates times five layouts times four
// channel counts is 120 at the absolute limit of what any driver accepts --
// and this message is the one place a peer chooses how much the other end
// allocates. Memory is bounded everywhere, and a list is a queue too.
const size_t kMaxCapabilityFormats = 256;

// AUDIO_CAPABILITIES as the server reads it (section 12).
//
// An empty `formats` list is a client with no playable device: it still gets
// a session -- so that it is a room the server can ask to rescan or switch
// device -- but SESSION_CONFIG names no format and no stream is prepared until
// a later AUDIO_CAPABILITIES lists one. `deviceId`, `deviceName` and `preferredSampleRate` are all optional:
// the first two only ever reach a log line or the status API, and the third is
// a hint section 12 already lets the server ignore. A `preferredSampleRate` of
// zero is that hint being unset, not an invalid rate -- the client sends the
// field whether or not its configuration names one.
//
// A format the list names but this build cannot spell is an error rather than
// a skipped entry, for the same reason the client refuses one: the layout
// decides how the bytes are read, and a server that quietly dropped `s24le3`
// from a device that only speaks it would resample a room that could have been
// bitperfect and never say why.
//
// `hardwareVolume` is optional. When present it must be well formed -- an
// object with `available`, and when that is true a `steps` of at least 2 and,
// if either is given, both `minDb` and `maxDb` with the second above the first
// -- because a malformed one is a client that meant to say something.
// `driftFeedback`, `driftCorrection` and `hardwareClock` are optional in the
// same way; an available `hardwareClock` needs whole-ppm bounds around zero
// and a positive step.
bool parseAudioCapabilities(const json::Value& payload, CapabilityReport& out,
                            std::string& error);

// The protocol does not fix HELLO_ACK's contents, so only the one field that
// decides whether to continue is read. A missing version means "the server did
// not say", not "version 0".
struct HelloAck
{
    bool hasProtocolVersion = false;
    uint32_t protocolVersion = 0;
};

bool parseHelloAck(const json::Value& payload, HelloAck& out, std::string& error);

// HELLO_ACK as the server writes it. A version is always sent -- `HelloAck`
// models a server that stayed silent because the protocol allows one, not
// because this one does -- and it is the only field, because a handshake that
// has not yet heard AUDIO_CAPABILITIES has nothing else to say that would be
// true (section 12). A version mismatch is not answered with this message at
// all: section 51 has it answered with ERROR and the connection dropped.
json::Value makeHelloAck(uint32_t protocolVersion);

// The format the server intends to send, and the session the audio belongs to
// (sections 12 and 13). Shaped like STREAM_READY's answer, which is the only
// place the protocol spells the format object out.
struct SessionConfig
{
    std::string sessionId;
    // False when the client reported no format: the session exists, but there
    // is no stream to prepare until its capabilities change.
    bool hasFormat = true;
    AudioFormat format;

    // The room's Audio Control (section 59). Optional on the wire; a client
    // told nothing treats the room as BitPerfect, the reading that never
    // scales a sample it was not allowed to.
    bool hasAudioControl = false;
    AudioControl audioControl = AudioControl::BitPerfect;

    // The actuator the client is to be ready to drive. Absent is the server
    // resampling and the hardware clock at its nominal rate.
    bool hasDriftCorrection = false;
    DriftCorrectionMode driftCorrection = DriftCorrectionMode::Resampling;
};

bool parseSessionConfig(const json::Value& payload, SessionConfig& out, std::string& error);

// SESSION_CONFIG as the server writes it. On an established session this is a
// re-negotiation rather than an error (section 12): the same three messages in
// the same order serve a client that changed its device and a zone that
// changed its source format, which is what saves the client from needing
// separate machinery for the second case.
json::Value makeSessionConfig(const SessionConfig& config);

// The four-timestamp exchange of section 8. The section names the timestamps
// (C1, S2, S3, C4) and the arithmetic but not the JSON, so two things are
// decided here:
//
//   * the field names below, spelled the way the rest of the protocol spells
//     nanosecond quantities;
//   * C1 is the correlator. Section 8 already requires the reply to carry the
//     request's send time back, and a monotonic clock does not produce the
//     same nanosecond twice, so a separate sequence number would be a second
//     identity for the same thing.
json::Value makeClockSyncRequest(uint64_t clientSendNs);

// The request as the server reads it. C1 is the whole message, and it is the
// correlator as well as the measurement, so there is nothing else to return.
bool parseClockSyncRequest(const json::Value& payload, uint64_t& clientSendNs,
                           std::string& error);

struct ClockSyncReply
{
    uint64_t clientSendNs = 0;    // C1, echoed
    uint64_t serverReceiveNs = 0; // S2
    uint64_t serverSendNs = 0;    // S3
};

bool parseClockSyncReply(const json::Value& payload, ClockSyncReply& out, std::string& error);

// The reply as the server writes it. S2 is stamped the moment the request was
// read off the socket and S3 the moment this message is handed to it -- not
// once for both -- because the difference between them is the server's own
// processing delay, which is the part of the round trip the client is entitled
// to subtract rather than divide (section 8).
json::Value makeClockSyncReply(const ClockSyncReply& reply);

// CLOCK_SYNC_RESULT: the four timestamps of one completed exchange, reported
// so the server can build its own client-to-server clock model (section 8).
//
// The server holds C1, S2 and S3 already; C4 is the client's own receive
// instant and nothing else carries it back. Without it the server cannot put
// PLAYBACK_REPORT's `localMonotonicTimeNs` into server time, and the offset
// half of its correction (sections 29 to 31) is exactly that comparison -- so
// a server without this message can correct a room's rate but never its
// position.
//
// Raw timestamps rather than this client's own `offsetNs`: the client
// measures and the server models, the same division as for drift, and a server
// that was handed a conclusion could no longer weight a sample by its round
// trip or reject an outlier the client accepted. For the same reason every
// completed exchange is reported, including one this client's model rejected.
//
// Unanswered, best-effort telemetry. A server that ignores it still works.
json::Value makeClockSyncResult(uint64_t clientSendNs, uint64_t serverReceiveNs,
                                uint64_t serverSendNs, uint64_t clientReceiveNs);

// The same four timestamps as the server reads them, which is the only input
// its own client-to-server clock model has (sections 8 and 29 to 31).
//
// The two orderings that cannot be true of any clock are refused here rather
// than weighted low by the model: C4 before C1 and S3 before S2 are one clock
// contradicting itself, not a sample that happened to be noisy, and a model
// that fitted them would be fitting a defect. The two *cross*-clock
// comparisons -- S2 against C1, C4 against S3 -- are deliberately not checked,
// because the offset between the two clocks is exactly what the model exists
// to find and may legitimately be of either sign and any size.
struct ClockSyncResult
{
    uint64_t clientSendNs = 0;    // C1
    uint64_t serverReceiveNs = 0; // S2
    uint64_t serverSendNs = 0;    // S3
    uint64_t clientReceiveNs = 0; // C4
};

bool parseClockSyncResult(const json::Value& payload, ClockSyncResult& out,
                          std::string& error);

// PREPARE_STREAM: the server is about to start sending audio (sections 10 and
// 12). The section names the message and nothing else about it, so both fields
// below are optional and neither is invented when absent:
//
//   * `sessionId`, when present, must be the session this client is in. A
//     different one means the server is preparing a stream this client knows
//     nothing about.
//   * `audioPort` is where the audio connection goes. Section 3 gives the
//     example pair 4711/4712 but no way to learn the second number, so a
//     server that does not say is taken to mean the control port plus one --
//     which is that example, and is the caller's decision, not this parser's.
struct PrepareStream
{
    std::string sessionId; // empty when the server did not say
    bool hasAudioPort = false;
    uint16_t audioPort = 0;
};

bool parsePrepareStream(const json::Value& payload, PrepareStream& out, std::string& error);

// PREPARE_STREAM as the server writes it. Both fields are written when the
// caller set them, and the port is written even where it is the control port
// plus one: the client's fallback happens to be right for the default pair,
// and relying on a fallback to be right is how a non-default port becomes a
// silent failure.
json::Value makePrepareStream(const PrepareStream& request);

// STREAM_READY, the answer to it (section 12). `format` is what the device
// granted, not what was asked for: the server must compare the two and must
// not send audio in a format the client has not confirmed here.
struct StreamReady
{
    bool ready = false;
    // Deliberately parked audio; PREPARE_STREAM on the next play wakes it.
    // Omitted for older clients and for manual standby, which stays parked.
    bool standby = false;
    AudioFormat format;        // meaningful only when ready
    uint32_t periodFrames = 0; // what the driver granted, 0 when unknown
    std::string reason;        // human-readable, and only when not ready
    std::string powerState;    // on, standby, standbyManual, sleep, sleepButtonOnly; absent on older clients
    // Only with powerState "sleep": the wired MAC address a Wake-on-LAN magic
    // packet wakes, as six lower-case hex pairs separated by colons.
    std::string wakeOnLan;

    // Only when ready: the actuator the opened device is ready for. A
    // SESSION_CONFIG that selected hardwareClock and an answer without it is
    // a hardware clock that could not be opened at its nominal rate; the
    // server then corrects this stream by resampling, or not at all.
    bool hasDriftCorrection = false;
    DriftCorrectionMode driftCorrection = DriftCorrectionMode::Resampling;
};

json::Value makeStreamReady(const std::string& sessionId, const StreamReady& ready);

// STREAM_READY as the server reads it.
//
// The session id comes back as its own argument rather than as a field of
// `StreamReady`, mirroring the `make` above: every message the client sends
// carries one, and none of them is meaningful without knowing which session it
// answers for. It is required here, unlike in the messages the *server* sends,
// where section 12 lets the sender leave it out -- the server has one stream
// per client and has to attribute this answer to one of them.
//
// `format` is read only when `ready` is true, which is what the make writes:
// the format a device did not grant is not a format, and a zero-filled one
// would be indistinguishable from a driver that answered 0 Hz.
bool parseStreamReady(const json::Value& payload, std::string& sessionId, StreamReady& out,
                      std::string& error);

// BUFFER_STATUS (section 19), which is how the server learns whether this room
// can be scheduled. `hasContent` is false until the first packet has arrived
// and the positions are then left out of the message rather than sent as
// zeros: `timelineNs` 0 is a real position on the timeline, and "no audio yet"
// is not it.
//
// Every position is a timeline nanosecond, because the timeline is the only
// axis the protocol has: the server compares this room's answer against every
// other room's, and PCM frames are not comparable between rooms at different
// rates or resampling ratios. `streamFrames` is the one exception and never
// leaves the machine -- the control API reports it because a full ring buffer
// is a local fact worth seeing.
struct BufferStatus
{
    bool hasContent = false;

    // The oldest timeline position still buffered, and one past the newest.
    uint64_t playheadTimelineNs = 0;
    uint64_t bufferedUntilTimelineNs = 0;

    // The span between the two above: how much media time this room holds.
    // Always sent, unlike the two positions -- an empty buffer holds zero
    // nanoseconds, which is a fact rather than a missing value.
    uint64_t bufferedDurationNs = 0;

    // What the ring buffer holds in this room's own frames. Local only; it is
    // not sent.
    uint32_t streamFrames = 0;
};

json::Value makeBufferStatus(const std::string& sessionId, const BufferStatus& status);

// BUFFER_STATUS as the server reads it, which is what its pacing runs on
// (section 19) and what decides whether a room can be scheduled (section 18).
//
// `hasContent` is derived rather than sent: the client omits both positions
// until the first packet has arrived, so their presence *is* the flag. Present
// means both, because one without the other is a span with one end.
// `bufferedDurationNs` is always there -- an empty buffer holds zero
// nanoseconds, which is a fact rather than a missing value -- and
// `streamFrames` never leaves the client at all, so it stays at its default
// here rather than being invented from the duration.
bool parseBufferStatus(const json::Value& payload, std::string& sessionId, BufferStatus& out,
                       std::string& error);

// START_AT: the instant on the server's clock at which a given position on the
// zone's media timeline has to be audible in every room (section 20).
//
// Both numbers are required. Neither has a defensible default -- a start that
// guessed one would be a room playing at the wrong moment rather than a room
// that did not start -- and the section always sends them together.
// `sessionId` is optional and checked like PREPARE_STREAM's.
//
// The two are in different clocks and neither is a frame: `serverTimeNs` is an
// instant on the server's monotonic clock, `timelineNs` a position in the
// media. Which frame of this room's own stream that position is, is the
// mapping of section 33 and belongs to the layer that holds the packets.
struct StartAt
{
    std::string sessionId; // empty when the server did not say
    uint64_t serverTimeNs = 0;
    uint64_t timelineNs = 0;
};

bool parseStartAt(const json::Value& payload, StartAt& out, std::string& error);

// START_AT as the server writes it. The server computes one number from the
// other -- `serverTimeNs` is where `timelineNs` falls once the prebuffer and
// this room's clock offset are accounted for -- and sends both, because the
// client places the event by the timeline position and verifies it by the
// clock (section 20).
json::Value makeStartAt(const StartAt& start);

// TRACK_BOUNDARY: where one track ends and the next begins on the zone's media
// timeline (section 23). Metadata and nothing else -- the stream carries no
// gap at a boundary of the same format (section 22), so a client that reopened
// the device for one would be the only thing producing a gap. Where the format
// does change, what reconfigures the device is the server's own PAUSE_AT and
// PREPARE_STREAM (section 24), not this message.
//
// Only `timelineNs` is required. The section's example carries a track id and
// a title as well, and a boundary that names neither is still a boundary: the
// position is the part every room has to agree on.
struct TrackBoundary
{
    std::string sessionId; // empty when the server did not say
    uint64_t timelineNs = 0;
    std::string trackId;
    std::string title;
};

bool parseTrackBoundary(const json::Value& payload, TrackBoundary& out, std::string& error);

// TRACK_BOUNDARY as the server writes it. `trackId` and `title` are written
// only when they are known: a boundary that names neither is still a boundary,
// and an empty string is not the same statement as saying nothing.
json::Value makeTrackBoundary(const TrackBoundary& boundary);

// TRACK_INFO: what the room is playing now. Sections 17 and 47 list it beside
// TRACK_BOUNDARY and say nothing else about it anywhere, so two things are
// decided here.
//
//   * **It carries no position, and that is the whole difference from
//     TRACK_BOUNDARY.** A boundary says where the *next* track begins and is
//     announced before the room gets there; this says what is playing at the
//     moment it arrives. The case that needs it is section 44's: a room that
//     joins in the middle of a track never receives the boundary that named
//     it, so without this it plays a track it cannot name.
//   * **The fields are the two a boundary already carries** and no others. An
//     artist, an album or a track length would each be a response key with
//     nothing behind it until something asks for one.
//
// A message naming neither is an error, for the same reason SET_VOLUME's
// naming neither is: it asks for nothing, and succeeding quietly would hide a
// server that meant to say something.
struct TrackInfo
{
    std::string sessionId; // empty when the server did not say
    std::string trackId;
    std::string title;
};

bool parseTrackInfo(const json::Value& payload, TrackInfo& out, std::string& error);

// TRACK_INFO as the server writes it. Unlike a boundary this message is
// nothing but the two names, so one that would carry neither is refused here
// rather than sent for the client to refuse: the caller has asked to say
// nothing, and a message is the wrong way to do that.
bool makeTrackInfo(const TrackInfo& info, json::Value& out, std::string& error);

// PONG, the answer to a PING (section 17). The section lists both and gives
// neither a body, so the answer is the request with its type changed: whatever
// the server correlates on -- a sequence number, its own send time, or nothing
// at all -- comes back to it, and no field name had to be invented for a
// message the protocol does not describe.
//
// The client sends no PING of its own. What it would need a heartbeat for --
// noticing a server that has stopped answering -- the clock sync exchange of
// section 8 already provides every five seconds, and a second heartbeat beside
// it would measure the same silence twice (see Session's silence timeout).
json::Value makePong(const json::Value& ping);

// PING as the server writes it, and the answer as it reads it back.
//
// The client echoes the ping's whole body with the type changed, so the server
// is free to choose what it correlates on and gets exactly that back. It
// chooses a sequence number: liveness is the thing being measured, an
// unanswered ping has to be attributable to a particular ping, and the
// server's own clock is already exchanged four timestamps at a time by section
// 8 -- putting a timestamp here as well would measure the same silence twice.
//
// A PONG without the sequence back is refused. It cannot have come from a
// client that echoed, which means the peer answered a question it did not
// read, and a liveness check that accepted that would be measuring nothing.
json::Value makePing(uint64_t sequence);

bool parsePong(const json::Value& payload, uint64_t& sequence, std::string& error);

// PAUSE_AT (section 37) and STOP_AT (section 38) carry exactly what START_AT
// carries: an instant on the server's clock and the timeline position that
// belongs to it. They differ in what a room does when one arrives, not in what
// they say, so one parser reads both.
//
// Which of the two numbers places the event is a decision the sections do not
// make, and this client uses the **timeline position**. Section 37 asks for
// every room to pause "at the same logical position"; a playing room already
// holds an exact mapping from the timeline onto its own frame axis -- the one
// START_AT established -- so going back through the server's clock would add
// the clock model's uncertainty to a position that is already known to the
// frame. `serverTimeNs` is required and parsed, and the two agree by
// construction: the server computes one from the other.
struct TransportAt
{
    std::string sessionId; // empty when the server did not say
    uint64_t serverTimeNs = 0;
    uint64_t timelineNs = 0;
};

bool parseTransportAt(const json::Value& payload, TransportAt& out, std::string& error);

// PAUSE_AT and STOP_AT as the server writes them. Two functions and one
// parser, which is the shape the difference has: the two messages say the same
// thing and differ only in what a room does when one arrives, and the type
// string is the entire distinction on the wire.
json::Value makePauseAt(const TransportAt& at);
json::Value makeStopAt(const TransportAt& at);

// FLUSH (section 54) carries the same three fields, so parseTransportAt reads
// it too; `timelineNs` is the cut. What differs is again only what a room does:
// it plays up to the cut, drops everything it holds from there on and keeps
// the start mapping it is playing on, so the packets that follow the cut --
// the ones carrying kAudioFlagAfterFlush -- play on the same line without a
// new START_AT.
json::Value makeFlush(const TransportAt& at);

// SET_VOLUME (section 39), which is also where mute lives.
//
// Two things the section does not say, and a client cannot avoid deciding:
//
//   * **Mute is a field here, not a message of its own.** Sections 17 and 47
//     list SET_VOLUME and no MUTE, and a mute expressed as `value: 0` would
//     lose the volume to come back to. `muted` is an optional boolean beside
//     `value`, and a message may carry either or both; a server that never
//     sends one behaves exactly as section 39 describes. **A field that is
//     absent means unchanged**, so a volume sent to a muted room stays silent
//     until the server says otherwise: the server is the authority on what a
//     room is doing, and a client that unmuted itself because a slider moved
//     would be deciding that for it.
//   * **A message that names neither is an error.** Sending SET_VOLUME to ask
//     for nothing is a server bug, and silently succeeding would hide it.
//
// `value` is a linear gain and the schedule is section 39's own
// `applyAtTimelineNs` -- a timeline position rather than an instant, and
// optional, because an unscheduled change is what a volume knob does. Negative
// gains are rejected here; clamping one above unity is the caller's, because
// what a room does about a fixed-point stream with no headroom is not a
// parsing question.
//
// The gain is applied by the client, in its own render path, which is also
// what keeps the server's stream bitperfect for a room that only wanted to be
// quieter (section 39).
struct SetVolume
{
    std::string sessionId; // empty when the server did not say
    bool hasValue = false;
    double value = 1.0;
    bool hasMuted = false;
    bool muted = false;
    bool hasApplyAtTimelineNs = false;
    uint64_t applyAtTimelineNs = 0;
};

bool parseSetVolume(const json::Value& payload, SetVolume& out, std::string& error);

// SET_VOLUME as the server writes it. Refused rather than written where it
// would carry neither a value nor a mute, for the same reason the parse
// refuses one: the caller has asked for nothing, and a server that sent it
// anyway would be producing a message its own parser rejects.
bool makeSetVolume(const SetVolume& request, json::Value& out, std::string& error);

// PLAYBACK_REPORT (section 25): what the sound card actually played, which is
// what the server's sync controller runs on and what it resamples this
// client's stream against.
//
// The two position fields are mandatory even when the device reports no
// position worth having: a server that sees `clockQuality: "unavailable"`
// knows the client is there and that its position is worthless, which is not
// what a missing report says. The measured values go the other way and are
// left out entirely while there is no fit -- a drift of zero is what a perfect
// clock looks like, not what an unmeasured one looks like, and it is the same
// rule that keeps the timeline out of an empty BUFFER_STATUS.
struct PlaybackReport
{
    uint64_t localMonotonicTimeNs = 0;
    uint64_t deviceFramePosition = 0;
    AudioClockQuality clockQuality = AudioClockQuality::Unavailable;

    // Absent until the sound card has reached the scheduled frame: before that
    // there is no audible timeline position at all, and `timelineNs` 0 is a
    // real one. `deviceFramePosition` above is this room's own frame axis and
    // this is the shared timeline; the two must never be confused for each
    // other (section 25).
    bool hasTimelineNsAudible = false;
    uint64_t timelineNsAudible = 0;

    // How much media time this room still holds, not how many frames it was
    // resampled into: a frame count is not comparable with another room's.
    uint64_t bufferedDurationNs = 0;

    // Periods the device had to silence because the source came up short.
    // Section 36 has the server treat this client's position as unreliable
    // until a new stable measurement replaces it, so it is a count of events
    // and not of frames.
    uint64_t underruns = 0;

    bool hasDrift = false;
    double measuredSampleRate = 0.0;
    double measuredDriftPpm = 0.0;
    double driftConfidence = 0.0;

    // Only while the session negotiated the hardware clock: what the client
    // last programmed. A successful write is not a measured frequency; the
    // drift above stays the measured residual of the trimmed clock.
    bool hasHardwareClock = false;
    int32_t hardwareClockPpm = 0;
    uint64_t hardwareClockSequence = 0; // the DRIFT_FEEDBACK applied, 0 before any
    bool hardwareClockSaturated = false; // the last target was beyond the device's range
    std::string hardwareClockError;      // the last write failed
};

json::Value makePlaybackReport(const std::string& sessionId, const PlaybackReport& report);

// PLAYBACK_REPORT as the server reads it: what the sound card actually played,
// and the input its sync controller runs on (sections 25 and 26).
//
// The three measured drift fields are read only when all three are present,
// because the client sends them as a set and a fit without a confidence is not
// a fit the controller may weight. `timelineNsAudible` is optional for the
// reason the client omits it -- before the scheduled frame there is no audible
// position at all, and 0 is a real one.
//
// An unknown `clockQuality` is an error rather than `Unavailable`: the two
// mean different things to the controller -- "this device has no usable
// position" against "this client means something I have never heard of" --
// and collapsing them would silently downgrade a room that was fine.
bool parsePlaybackReport(const json::Value& payload, std::string& sessionId,
                         PlaybackReport& out, std::string& error);

// The `system` object of section 55: SystemMetrics field by field, each one
// left out when the sampler has none. The same object is what the client's
// and the core's `status` answer, so it is written in one place.
json::Value makeSystemMetrics(const SystemMetrics& metrics);

// Reads it back. A missing key stays absent; a present one of the wrong type,
// a percentage outside 0 to 100 or a negative count is an error.
bool parseSystemMetrics(const json::Value& object, SystemMetrics& out, std::string& error);

// SYSTEM_REPORT (section 55): unanswered, best-effort telemetry about the
// room's machine, every 2 s once the session is established, whether the room
// plays or not. A report that does not parse is the receiver's to count and
// ignore -- a room is not dropped over its diagnostics.
json::Value makeSystemReport(const std::string& sessionId, const SystemMetrics& metrics);
bool parseSystemReport(const json::Value& payload, std::string& sessionId, SystemMetrics& out,
                       std::string& error);

// AUDIO_DEVICES (section 56): every playback device the client's last scan
// found, the one its configuration names and the one it actually plays on.
// The two ids differ exactly when the configured device is missing and the
// client fell back to the default, which is worth being able to see.
struct AudioDeviceEntry
{
    std::string id; // never empty
    std::string name;
    bool isDefault = false;
};

struct AudioDeviceList
{
    std::string configuredDeviceId; // empty: the system default
    std::string selectedDeviceId;   // empty: no usable device, and left out on the wire
    std::vector<AudioDeviceEntry> devices;
};

// A house has a handful; this is the one list a peer sizes on the other end.
const size_t kMaxAudioDevices = 64;

json::Value makeAudioDevices(const std::string& sessionId, const AudioDeviceList& list);
bool parseAudioDevices(const json::Value& payload, std::string& sessionId, AudioDeviceList& out,
                       std::string& error);

// SET_AUDIO_DEVICE (section 56): the server asks the client to play on
// another device from now on. The client persists it, answers with a new
// AUDIO_DEVICES, and re-sends AUDIO_CAPABILITIES when the device it plays on
// changed -- the re-negotiation of section 12 is what reopens it. An empty
// `deviceId` is "the system default", not a missing field.
struct SetAudioDevice
{
    std::string sessionId; // empty when the server did not say
    std::string deviceId;
};

json::Value makeSetAudioDevice(const SetAudioDevice& request);
bool parseSetAudioDevice(const json::Value& payload, SetAudioDevice& out, std::string& error);

// RESCAN_AUDIO (section 56): the server asks the client to scan its sound
// cards again -- one plugged in since it started, say. The client answers
// with AUDIO_DEVICES, and with AUDIO_CAPABILITIES when the card it plays on or
// its formats changed. `sessionId` is optional, as for SET_AUDIO_DEVICE.
json::Value makeRescanAudio(const std::string& sessionId);
bool parseRescanAudio(const json::Value& payload, std::string& sessionId, std::string& error);

// SET_BUFFER (section 58): the server sets the size of the room's own buffer,
// in milliseconds of audio. It is memory, not pacing: the server fills every
// room to a target of its own. The client persists it, applies it at once and
// answers with BUFFER_CONFIG carrying the value it now uses -- or with an ERROR
// (code "setBuffer") and its old value untouched. The range is the client's
// to judge, so the parser only asks for a positive whole number.
struct SetBuffer
{
    std::string sessionId; // empty when the server did not say
    uint32_t bufferMs = 0;
};

json::Value makeSetBuffer(const SetBuffer& request);
bool parseSetBuffer(const json::Value& payload, SetBuffer& out, std::string& error);

// BUFFER_CONFIG (section 58): the buffer size the client uses. Sent once
// the session is established, as the answer to every SET_BUFFER it accepted,
// and whenever the value changes by any other way.
json::Value makeBufferConfig(const std::string& sessionId, uint32_t bufferMs);
bool parseBufferConfig(const json::Value& payload, std::string& sessionId,
                       uint32_t& bufferMs, std::string& error);

// SET_VOLUME_CONTROL (section 59): the server sets how the room plays its
// volume. The client persists it and answers with VOLUME_CONFIG -- or with an
// ERROR (code "setVolumeControl") and its old setting untouched, which is
// always the answer to "software" while the room's Audio Control is
// bitPerfect. `sessionId` is optional, as for SET_BUFFER.
struct SetVolumeControl
{
    std::string sessionId;
    VolumeControlMode volumeControl = VolumeControlMode::Auto;
};

json::Value makeSetVolumeControl(const SetVolumeControl& request);
bool parseSetVolumeControl(const json::Value& payload, SetVolumeControl& out,
                           std::string& error);

// VOLUME_CONFIG (section 59): the setting the client has, and the path it
// resolved from it, the room's Audio Control and its device -- never Auto.
// `reason` says why the path is not the one asked for (no hardware control,
// software not allowed); empty where it is. Sent once the session is
// established, as the answer to every SET_VOLUME_CONTROL it accepted, and
// whenever either value changes by any other way.
struct VolumeConfig
{
    std::string sessionId;
    VolumeControlMode volumeControl = VolumeControlMode::Auto;
    VolumeControlMode effective = VolumeControlMode::Disabled;
    std::string reason;
};

json::Value makeVolumeConfig(const VolumeConfig& config);
bool parseVolumeConfig(const json::Value& payload, VolumeConfig& out, std::string& error);

// Section 60's rule for a room's name: 1 to 64 bytes of UTF-8, no control
// characters, no blank at either end. Never trimmed or shortened: a name that
// breaks it is refused.
bool isValidRoomName(const std::string& name);

// SET_ROOM_NAME (section 60): the server renames the room. The client
// persists the name and answers with ROOM_NAME -- or with an ERROR (code
// "setRoomName") and its old name untouched. Nothing else about the session
// changes. `sessionId` is optional, as for SET_BUFFER.
struct SetRoomName
{
    std::string sessionId;
    std::string name;
};

json::Value makeSetRoomName(const SetRoomName& request);
bool parseSetRoomName(const json::Value& payload, SetRoomName& out, std::string& error);

// ROOM_NAME (section 60): the name the client has. Sent once the session is
// established, as the answer to every SET_ROOM_NAME it accepted, and whenever
// the name changes by any other way.
json::Value makeRoomName(const std::string& sessionId, const std::string& name);
bool parseRoomName(const json::Value& payload, std::string& sessionId, std::string& name,
                   std::string& error);

// SET_DRIFT_CORRECTION: the server sets the actuator the room asks for. The
// client persists it and answers with AUDIO_CAPABILITIES carrying it, which
// renegotiates the session -- or with an ERROR (code "setDriftCorrection")
// and its old setting untouched. `sessionId` is optional, as for SET_BUFFER.
struct SetDriftCorrection
{
    std::string sessionId;
    DriftCorrectionMode driftCorrection = DriftCorrectionMode::Resampling;
};

json::Value makeSetDriftCorrection(const SetDriftCorrection& request);
bool parseSetDriftCorrection(const json::Value& payload, SetDriftCorrection& out,
                             std::string& error);

// DRIFT_FEEDBACK: the server's drift measurement for this room, sent only to a
// client that advertised `driftFeedback`. The measurement is the physical
// residual of the room's clock against the server's, and is absent while
// there is none: unknown is not zero. `hardwareTargetPpm` is an absolute trim
// relative to nominal (positive slows), present only while the session
// negotiated the hardware clock; a client that did not never writes it.
// `sequence` rises within a session, and the client ignores one that does not.
struct DriftFeedback
{
    std::string sessionId;
    uint64_t sequence = 0;
    bool hasDrift = false;
    double measuredDriftPpm = 0.0;
    double driftConfidence = 0.0;
    bool hasHardwareTarget = false;
    double hardwareTargetPpm = 0.0;
};

json::Value makeDriftFeedback(const DriftFeedback& feedback);
bool parseDriftFeedback(const json::Value& payload, DriftFeedback& out, std::string& error);

// The first thing the client sends on the audio connection, and the one
// message here the protocol does not name.
//
// Section 13 allows audio packets to omit the session id when they arrive "on
// an audio TCP connection already bound to exactly one session", but nothing
// in the protocol binds one: a fresh TCP connection to the audio port carries
// no identity at all, and the server has one stream per client (Info.md) that
// it has to attach this connection to. This is that binding, framed like every
// other control message so the server needs no second parser for it.
json::Value makeStreamAttach(const std::string& sessionId, const Identity& identity);

// STREAM_ATTACH as the server reads it, and the message that turns a bare TCP
// connection on the audio port into this room's stream.
//
// Both fields are required. The session id is what the connection is bound to;
// the client id is what lets a server that has already torn that session down
// still say which room came back, and a connection that names neither is one
// the server can do nothing with but close.
bool parseStreamAttach(const json::Value& payload, std::string& sessionId,
                       std::string& clientId, std::string& error);

// ERROR has no defined shape beyond something a human can read.
std::string errorText(const json::Value& payload);

// ERROR as the server writes it, which errorText above is the reader for.
// `code` is optional and machine-readable, `message` is for a person; a code
// with no message would be an error only the server's own source explains.
json::Value makeError(const std::string& code, const std::string& message);

} // namespace matp
} // namespace multiroom
