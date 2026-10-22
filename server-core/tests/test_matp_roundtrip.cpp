// Every message the server sends, decoded by the parser the client dispatches
// on -- and every message the client sends, decoded by the server's.
//
// This is the test the shared `multiroom-matp` library exists for. Before the
// move there were two implementations of one wire format and no cheap way to
// prove they agreed; now both halves of each exchange are in one library, so a
// round trip is an assertion rather than an integration run. A field name that
// changes on one side and not the other fails here, in a build with no socket
// in it.
//
// It deliberately goes through `encode` and `FrameReader` rather than handing
// a `json::Value` straight to the parser: the framing is part of the format,
// and `messageLength` counting the type field is exactly the kind of thing
// that is obvious in one direction and wrong in the other.

#include "TestSupport.h"

#include "matp/Protocol.h"

using namespace multiroom;
using namespace multiroom::matp;

namespace {

// One message through the wire and back: encode it, feed the bytes to a
// reader, and hand back the decoded message. The type is checked here so that
// no individual test has to remember to.
Message roundTrip(MessageType type, const json::Value& payload)
{
    const std::string frame = encode(type, payload);

    FrameReader reader;
    reader.append(frame.data(), frame.size());

    Message message;
    std::string error;
    const FrameReader::Result result = reader.next(message, error);
    if (result != FrameReader::Result::Message)
        multiroomtest::failAt("frame did not decode: " + error, __FILE__, __LINE__);

    CHECK_EQ(static_cast<int>(message.type), static_cast<int>(type));
    CHECK_EQ(message.typeName, std::string(messageTypeName(type)));
    CHECK_EQ(message.wireType, static_cast<uint16_t>(type));
    CHECK_EQ(reader.buffered(), size_t(0));
    return message;
}

AudioFormat makeFormat(uint32_t rate, uint16_t channels, AudioFormat::SampleType type)
{
    AudioFormat format;
    format.sampleRate = rate;
    format.channels = channels;
    format.sampleType = type;
    return format;
}

} // namespace

// --------------------------------------------------------------------------
// Client to server: what the server's new parsers read.
// --------------------------------------------------------------------------

MULTIROOM_TEST(matp_hello_survives_the_round_trip)
{
    Identity identity;
    identity.clientId = "livingroom";
    identity.clientVersion = "0.1.0";
    identity.platform = "linux-armv6";

    const Message message = roundTrip(MessageType::Hello, makeHello(identity));

    Hello hello;
    std::string error;
    CHECK(parseHello(message.payload, hello, error));
    CHECK_EQ(hello.protocolVersion, kProtocolVersion);
    CHECK_EQ(hello.identity.clientId, identity.clientId);
    CHECK_EQ(hello.identity.clientVersion, identity.clientVersion);
    CHECK_EQ(hello.identity.platform, identity.platform);
}

MULTIROOM_TEST(matp_hello_without_a_version_is_refused)
{
    // Section 51: a peer that does not say which version it speaks is refused
    // rather than assumed to speak ours, because version 1 and version 2
    // disagree about what every position on the wire means.
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value(messageTypeName(MessageType::Hello));
    payload["clientId"] = json::Value("livingroom");

    Hello hello;
    std::string error;
    CHECK(!parseHello(payload, hello, error));
    CHECK(!error.empty());
}

MULTIROOM_TEST(matp_hello_without_a_client_id_is_refused)
{
    // Section 11: the server keys the output stream, the drift model and the
    // resampler state on it, so a room with no stable name cannot have any.
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value(messageTypeName(MessageType::Hello));
    payload["protocolVersion"] = json::Value(kProtocolVersion);

    Hello hello;
    std::string error;
    CHECK(!parseHello(payload, hello, error));
    CHECK(!error.empty());
}

MULTIROOM_TEST(matp_audio_capabilities_survives_the_round_trip)
{
    CapabilityReport report;
    report.deviceId = "hw:CARD=USB,DEV=0";
    report.deviceName = "USB DAC";
    report.preferredSampleRate = 48000;
    report.formats.push_back(makeFormat(44100, 2, AudioFormat::SampleType::Int24In32));
    report.formats.push_back(makeFormat(48000, 2, AudioFormat::SampleType::Int24In32));
    report.formats.push_back(makeFormat(96000, 2, AudioFormat::SampleType::Int16));
    report.formats.push_back(makeFormat(44100, 2, AudioFormat::SampleType::Int24));

    const Message message =
        roundTrip(MessageType::AudioCapabilities, makeAudioCapabilities(report));

    CapabilityReport parsed;
    std::string error;
    CHECK(parseAudioCapabilities(message.payload, parsed, error));
    CHECK_EQ(parsed.deviceId, report.deviceId);
    CHECK_EQ(parsed.deviceName, report.deviceName);
    CHECK_EQ(parsed.preferredSampleRate, report.preferredSampleRate);
    CHECK_EQ(parsed.formats.size(), report.formats.size());
    for (size_t i = 0; i < report.formats.size(); ++i)
        CHECK(parsed.formats[i] == report.formats[i]);
}

MULTIROOM_TEST(matp_audio_capabilities_keeps_the_two_24_bit_layouts_apart)
{
    // The whole reason section 12 names a layout rather than a depth. If these
    // two ever collapsed into one wire name, a packed-24 device would be sent
    // 24-in-32 and the room would play noise.
    CapabilityReport packed;
    packed.formats.push_back(makeFormat(44100, 2, AudioFormat::SampleType::Int24));

    CapabilityReport inContainer;
    inContainer.formats.push_back(makeFormat(44100, 2, AudioFormat::SampleType::Int24In32));

    CapabilityReport parsedPacked;
    CapabilityReport parsedInContainer;
    std::string error;
    CHECK(parseAudioCapabilities(makeAudioCapabilities(packed), parsedPacked, error));
    CHECK(parseAudioCapabilities(makeAudioCapabilities(inContainer), parsedInContainer,
                                 error));

    CHECK(parsedPacked.formats[0].sampleType == AudioFormat::SampleType::Int24);
    CHECK(parsedInContainer.formats[0].sampleType == AudioFormat::SampleType::Int24In32);
    CHECK(parsedPacked.formats[0] != parsedInContainer.formats[0]);
}

MULTIROOM_TEST(matp_audio_capabilities_with_no_formats_is_a_client_without_a_device)
{
    CapabilityReport empty;
    empty.deviceId = "hw:0";

    CapabilityReport parsed;
    std::string error;
    CHECK(parseAudioCapabilities(makeAudioCapabilities(empty), parsed, error));
    CHECK(parsed.formats.empty());
}

MULTIROOM_TEST(matp_session_config_without_a_format_survives_the_round_trip)
{
    SessionConfig config;
    config.sessionId = "0123456789abcdef";
    config.hasFormat = false;

    const Message message = roundTrip(MessageType::SessionConfig, makeSessionConfig(config));
    SessionConfig parsed;
    std::string error;
    CHECK(parseSessionConfig(message.payload, parsed, error));
    CHECK(!parsed.hasFormat);
    CHECK_EQ(parsed.sessionId, config.sessionId);
}

MULTIROOM_TEST(matp_clock_sync_request_and_result_survive_the_round_trip)
{
    const Message request =
        roundTrip(MessageType::ClockSyncRequest, makeClockSyncRequest(1000000000ULL));

    uint64_t clientSendNs = 0;
    std::string error;
    CHECK(parseClockSyncRequest(request.payload, clientSendNs, error));
    CHECK_EQ(clientSendNs, 1000000000ULL);

    const Message result = roundTrip(
        MessageType::ClockSyncResult,
        makeClockSyncResult(1000000000ULL, 1000400000ULL, 1000450000ULL, 1000900000ULL));

    ClockSyncResult sample;
    CHECK(parseClockSyncResult(result.payload, sample, error));
    CHECK_EQ(sample.clientSendNs, 1000000000ULL);
    CHECK_EQ(sample.serverReceiveNs, 1000400000ULL);
    CHECK_EQ(sample.serverSendNs, 1000450000ULL);
    CHECK_EQ(sample.clientReceiveNs, 1000900000ULL);
}

MULTIROOM_TEST(matp_clock_sync_result_refuses_a_clock_that_ran_backwards)
{
    ClockSyncResult sample;
    std::string error;

    // C4 before C1: one clock contradicting itself, not a noisy round trip.
    CHECK(!parseClockSyncResult(
        makeClockSyncResult(1000000000ULL, 1000400000ULL, 1000450000ULL, 999000000ULL), sample,
        error));
    CHECK(!error.empty());

    // S3 before S2, likewise.
    error.clear();
    CHECK(!parseClockSyncResult(
        makeClockSyncResult(1000000000ULL, 1000450000ULL, 1000400000ULL, 1000900000ULL),
        sample, error));
    CHECK(!error.empty());
}

MULTIROOM_TEST(matp_clock_sync_result_accepts_an_offset_of_either_sign)
{
    // The two *cross*-clock comparisons are not checked, and must not be: the
    // offset between the client's clock and the server's is what the model
    // exists to find, and a server that started hours after the client sees
    // every server timestamp below every client one.
    ClockSyncResult sample;
    std::string error;
    CHECK(parseClockSyncResult(makeClockSyncResult(5000000000ULL, 100000ULL, 150000ULL,
                                                   5000900000ULL),
                               sample, error));
    CHECK_EQ(sample.serverReceiveNs, 100000ULL);
}

MULTIROOM_TEST(matp_stream_ready_survives_the_round_trip)
{
    StreamReady ready;
    ready.ready = true;
    ready.format = makeFormat(44100, 2, AudioFormat::SampleType::Int24In32);
    ready.periodFrames = 480;

    const Message message =
        roundTrip(MessageType::StreamReady, makeStreamReady("a1b2c3", ready));

    std::string sessionId;
    StreamReady parsed;
    std::string error;
    CHECK(parseStreamReady(message.payload, sessionId, parsed, error));
    CHECK_EQ(sessionId, std::string("a1b2c3"));
    CHECK(parsed.ready);
    CHECK(parsed.format == ready.format);
    CHECK_EQ(parsed.periodFrames, ready.periodFrames);
}

MULTIROOM_TEST(matp_stream_ready_that_refused_carries_a_reason_and_no_format)
{
    StreamReady refused;
    refused.ready = false;
    refused.reason = "device busy";

    std::string sessionId;
    StreamReady parsed;
    std::string error;
    CHECK(parseStreamReady(makeStreamReady("a1b2c3", refused), sessionId, parsed, error));
    CHECK(!parsed.ready);
    CHECK_EQ(parsed.reason, refused.reason);
    // Not invented: a format the device did not grant is not a format, and a
    // zero-filled one is indistinguishable from a driver answering 0 Hz.
    CHECK(!parsed.format.isValid());
}

MULTIROOM_TEST(matp_buffer_status_survives_the_round_trip)
{
    BufferStatus status;
    status.hasContent = true;
    status.playheadTimelineNs = 2000000000ULL;
    status.bufferedUntilTimelineNs = 3500000000ULL;
    status.bufferedDurationNs = 1500000000ULL;
    status.streamFrames = 66150; // local only, never sent

    std::string sessionId;
    BufferStatus parsed;
    std::string error;
    CHECK(parseBufferStatus(makeBufferStatus("a1b2c3", status), sessionId, parsed, error));
    CHECK_EQ(sessionId, std::string("a1b2c3"));
    CHECK(parsed.hasContent);
    CHECK_EQ(parsed.playheadTimelineNs, status.playheadTimelineNs);
    CHECK_EQ(parsed.bufferedUntilTimelineNs, status.bufferedUntilTimelineNs);
    CHECK_EQ(parsed.bufferedDurationNs, status.bufferedDurationNs);
    CHECK_EQ(parsed.streamFrames, 0u);
}

MULTIROOM_TEST(matp_buffer_status_without_content_omits_the_positions)
{
    // timelineNs 0 is a real position and "no audio yet" is not it, so an
    // empty buffer sends the duration and nothing else.
    BufferStatus empty;
    empty.hasContent = false;
    empty.bufferedDurationNs = 0;

    std::string sessionId;
    BufferStatus parsed;
    std::string error;
    CHECK(parseBufferStatus(makeBufferStatus("a1b2c3", empty), sessionId, parsed, error));
    CHECK(!parsed.hasContent);
    CHECK_EQ(parsed.bufferedDurationNs, 0ULL);
}

MULTIROOM_TEST(matp_playback_report_survives_the_round_trip)
{
    PlaybackReport report;
    report.localMonotonicTimeNs = 9000000000ULL;
    report.deviceFramePosition = 441000;
    report.clockQuality = AudioClockQuality::Hardware;
    report.hasTimelineNsAudible = true;
    report.timelineNsAudible = 10000000000ULL;
    report.bufferedDurationNs = 1400000000ULL;
    report.underruns = 2;
    report.hasDrift = true;
    report.measuredSampleRate = 44101.3;
    report.measuredDriftPpm = 29.5;
    report.driftConfidence = 0.93;

    std::string sessionId;
    PlaybackReport parsed;
    std::string error;
    CHECK(parsePlaybackReport(makePlaybackReport("a1b2c3", report), sessionId, parsed, error));
    CHECK_EQ(sessionId, std::string("a1b2c3"));
    CHECK_EQ(parsed.localMonotonicTimeNs, report.localMonotonicTimeNs);
    CHECK_EQ(parsed.deviceFramePosition, report.deviceFramePosition);
    CHECK(parsed.clockQuality == AudioClockQuality::Hardware);
    CHECK(parsed.hasTimelineNsAudible);
    CHECK_EQ(parsed.timelineNsAudible, report.timelineNsAudible);
    CHECK_EQ(parsed.bufferedDurationNs, report.bufferedDurationNs);
    CHECK_EQ(parsed.underruns, report.underruns);
    CHECK(parsed.hasDrift);
    CHECK_NEAR(parsed.measuredSampleRate, report.measuredSampleRate, 1e-9);
    CHECK_NEAR(parsed.measuredDriftPpm, report.measuredDriftPpm, 1e-9);
    CHECK_NEAR(parsed.driftConfidence, report.driftConfidence, 1e-9);
}

MULTIROOM_TEST(matp_playback_report_without_a_fit_omits_the_drift_rather_than_sending_zero)
{
    // A drift of zero is what a perfect clock looks like, not what an
    // unmeasured one looks like (section 25).
    PlaybackReport report;
    report.localMonotonicTimeNs = 9000000000ULL;
    report.deviceFramePosition = 0;
    report.clockQuality = AudioClockQuality::Unavailable;
    report.hasDrift = false;

    std::string sessionId;
    PlaybackReport parsed;
    std::string error;
    CHECK(parsePlaybackReport(makePlaybackReport("a1b2c3", report), sessionId, parsed, error));
    CHECK(!parsed.hasDrift);
    CHECK(!parsed.hasTimelineNsAudible);
    CHECK(parsed.clockQuality == AudioClockQuality::Unavailable);
}

MULTIROOM_TEST(matp_playback_report_refuses_a_clock_quality_it_does_not_know)
{
    // "unavailable" and "a word I have never heard" are different statements
    // about a room, and only one of them is the room's fault.
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value(messageTypeName(MessageType::PlaybackReport));
    payload["sessionId"] = json::Value("a1b2c3");
    payload["localMonotonicTimeNs"] = json::Value(uint64_t(1));
    payload["deviceFramePosition"] = json::Value(uint64_t(0));
    payload["bufferedDurationNs"] = json::Value(uint64_t(0));
    payload["underruns"] = json::Value(uint64_t(0));
    payload["clockQuality"] = json::Value("telepathy");

    std::string sessionId;
    PlaybackReport parsed;
    std::string error;
    CHECK(!parsePlaybackReport(payload, sessionId, parsed, error));
    CHECK(!error.empty());
}

MULTIROOM_TEST(matp_stream_attach_survives_the_round_trip)
{
    Identity identity;
    identity.clientId = "livingroom";

    const Message message =
        roundTrip(MessageType::StreamAttach, makeStreamAttach("a1b2c3", identity));

    std::string sessionId;
    std::string clientId;
    std::string error;
    CHECK(parseStreamAttach(message.payload, sessionId, clientId, error));
    CHECK_EQ(sessionId, std::string("a1b2c3"));
    CHECK_EQ(clientId, identity.clientId);
}

MULTIROOM_TEST(matp_pong_returns_the_sequence_the_ping_carried)
{
    // The client echoes the ping's whole body with the type changed, so the
    // correlator the server chose comes back to it untouched.
    const json::Value ping = makePing(42);
    const Message message = roundTrip(MessageType::Pong, makePong(ping));

    uint64_t sequence = 0;
    std::string error;
    CHECK(parsePong(message.payload, sequence, error));
    CHECK_EQ(sequence, 42ULL);
}

// --------------------------------------------------------------------------
// Server to client: what the client's existing parsers read.
// --------------------------------------------------------------------------

MULTIROOM_TEST(matp_hello_ack_survives_the_round_trip)
{
    const Message message = roundTrip(MessageType::HelloAck, makeHelloAck(kProtocolVersion));

    HelloAck ack;
    std::string error;
    CHECK(parseHelloAck(message.payload, ack, error));
    CHECK(ack.hasProtocolVersion);
    CHECK_EQ(ack.protocolVersion, kProtocolVersion);
}

MULTIROOM_TEST(matp_session_config_survives_the_round_trip)
{
    SessionConfig config;
    config.sessionId = "0123456789abcdef0123456789abcdef";
    config.format = makeFormat(44100, 2, AudioFormat::SampleType::Int24);

    const Message message = roundTrip(MessageType::SessionConfig, makeSessionConfig(config));

    SessionConfig parsed;
    std::string error;
    CHECK(parseSessionConfig(message.payload, parsed, error));
    CHECK_EQ(parsed.sessionId, config.sessionId);
    CHECK(parsed.format == config.format);
}

MULTIROOM_TEST(matp_clock_sync_reply_survives_the_round_trip)
{
    ClockSyncReply reply;
    reply.clientSendNs = 1000000000ULL;
    reply.serverReceiveNs = 1000400000ULL;
    reply.serverSendNs = 1000450000ULL;

    const Message message = roundTrip(MessageType::ClockSyncReply, makeClockSyncReply(reply));

    ClockSyncReply parsed;
    std::string error;
    CHECK(parseClockSyncReply(message.payload, parsed, error));
    CHECK_EQ(parsed.clientSendNs, reply.clientSendNs);
    CHECK_EQ(parsed.serverReceiveNs, reply.serverReceiveNs);
    CHECK_EQ(parsed.serverSendNs, reply.serverSendNs);
}

MULTIROOM_TEST(matp_prepare_stream_survives_the_round_trip)
{
    PrepareStream request;
    request.sessionId = "a1b2c3";
    request.hasAudioPort = true;
    request.audioPort = 4712;

    const Message message =
        roundTrip(MessageType::PrepareStream, makePrepareStream(request));

    PrepareStream parsed;
    std::string error;
    CHECK(parsePrepareStream(message.payload, parsed, error));
    CHECK_EQ(parsed.sessionId, request.sessionId);
    CHECK(parsed.hasAudioPort);
    CHECK_EQ(parsed.audioPort, request.audioPort);
}

MULTIROOM_TEST(matp_start_at_survives_the_round_trip)
{
    StartAt start;
    start.sessionId = "a1b2c3";
    start.serverTimeNs = 12345678901234ULL;
    start.timelineNs = 1500000000ULL;

    const Message message = roundTrip(MessageType::StartAt, makeStartAt(start));

    StartAt parsed;
    std::string error;
    CHECK(parseStartAt(message.payload, parsed, error));
    CHECK_EQ(parsed.sessionId, start.sessionId);
    CHECK_EQ(parsed.serverTimeNs, start.serverTimeNs);
    CHECK_EQ(parsed.timelineNs, start.timelineNs);
}

MULTIROOM_TEST(matp_pause_at_and_stop_at_share_one_parser_and_differ_only_in_type)
{
    TransportAt at;
    at.sessionId = "a1b2c3";
    at.serverTimeNs = 12345678901234ULL;
    at.timelineNs = 1500000000ULL;

    const Message paused = roundTrip(MessageType::PauseAt, makePauseAt(at));
    const Message stopped = roundTrip(MessageType::StopAt, makeStopAt(at));

    TransportAt parsedPause;
    TransportAt parsedStop;
    std::string error;
    CHECK(parseTransportAt(paused.payload, parsedPause, error));
    CHECK(parseTransportAt(stopped.payload, parsedStop, error));
    CHECK_EQ(parsedPause.timelineNs, at.timelineNs);
    CHECK_EQ(parsedStop.timelineNs, at.timelineNs);
    CHECK_EQ(parsedPause.serverTimeNs, parsedStop.serverTimeNs);
}

MULTIROOM_TEST(matp_set_volume_survives_the_round_trip)
{
    SetVolume request;
    request.sessionId = "a1b2c3";
    request.hasValue = true;
    request.value = 0.5;
    request.hasApplyAtTimelineNs = true;
    request.applyAtTimelineNs = 2000000000ULL;

    json::Value payload;
    std::string error;
    CHECK(makeSetVolume(request, payload, error));

    const Message message = roundTrip(MessageType::SetVolume, payload);

    SetVolume parsed;
    CHECK(parseSetVolume(message.payload, parsed, error));
    CHECK(parsed.hasValue);
    CHECK_NEAR(parsed.value, request.value, 1e-12);
    // Absent means unchanged: a volume sent to a muted room leaves it muted.
    CHECK(!parsed.hasMuted);
    CHECK(parsed.hasApplyAtTimelineNs);
    CHECK_EQ(parsed.applyAtTimelineNs, request.applyAtTimelineNs);
}

MULTIROOM_TEST(matp_set_volume_that_asks_for_nothing_is_refused_by_both_ends)
{
    SetVolume nothing;
    nothing.sessionId = "a1b2c3";

    json::Value payload;
    std::string error;
    CHECK(!makeSetVolume(nothing, payload, error));
    CHECK(!error.empty());
}

MULTIROOM_TEST(matp_track_boundary_survives_the_round_trip)
{
    TrackBoundary boundary;
    boundary.sessionId = "a1b2c3";
    boundary.timelineNs = 180000000000ULL;
    boundary.trackId = "track-2";
    boundary.title = "Second";

    const Message message =
        roundTrip(MessageType::TrackBoundary, makeTrackBoundary(boundary));

    TrackBoundary parsed;
    std::string error;
    CHECK(parseTrackBoundary(message.payload, parsed, error));
    CHECK_EQ(parsed.timelineNs, boundary.timelineNs);
    CHECK_EQ(parsed.trackId, boundary.trackId);
    CHECK_EQ(parsed.title, boundary.title);
}

MULTIROOM_TEST(matp_track_boundary_that_names_nothing_is_still_a_boundary)
{
    TrackBoundary bare;
    bare.timelineNs = 180000000000ULL;

    TrackBoundary parsed;
    std::string error;
    CHECK(parseTrackBoundary(makeTrackBoundary(bare), parsed, error));
    CHECK_EQ(parsed.timelineNs, bare.timelineNs);
    CHECK(parsed.trackId.empty());
    CHECK(parsed.title.empty());
}

MULTIROOM_TEST(matp_track_info_survives_the_round_trip)
{
    TrackInfo info;
    info.sessionId = "a1b2c3";
    info.title = "Something already playing";

    json::Value payload;
    std::string error;
    CHECK(makeTrackInfo(info, payload, error));

    const Message message = roundTrip(MessageType::TrackInfo, payload);

    TrackInfo parsed;
    CHECK(parseTrackInfo(message.payload, parsed, error));
    CHECK_EQ(parsed.title, info.title);
    CHECK(parsed.trackId.empty());
}

MULTIROOM_TEST(matp_track_info_that_names_nothing_is_refused_by_both_ends)
{
    // Unlike a boundary, this message is nothing but the two names.
    TrackInfo nothing;
    nothing.sessionId = "a1b2c3";

    json::Value payload;
    std::string error;
    CHECK(!makeTrackInfo(nothing, payload, error));
    CHECK(!error.empty());
}

MULTIROOM_TEST(matp_error_survives_the_round_trip)
{
    const Message message =
        roundTrip(MessageType::Error, makeError("versionMismatch", "this server speaks 2"));

    CHECK_EQ(errorText(message.payload),
             std::string("versionMismatch: this server speaks 2"));
}

MULTIROOM_TEST(matp_error_without_a_code_is_still_readable)
{
    CHECK_EQ(errorText(makeError("", "something went wrong")),
             std::string("something went wrong"));
}

// --------------------------------------------------------------------------
// The framing itself, which is shared by every message above.
// --------------------------------------------------------------------------

MULTIROOM_TEST(matp_framing_splits_a_stream_of_messages_at_the_right_boundaries)
{
    // One read is not one message: the whole reason FrameReader exists. Three
    // messages are concatenated and fed in one byte at a time, which is the
    // worst case a TCP stream can produce.
    Identity identity;
    identity.clientId = "livingroom";

    std::string stream;
    stream += encode(MessageType::Hello, makeHello(identity));
    stream += encode(MessageType::ClockSyncRequest, makeClockSyncRequest(7));
    stream += encode(MessageType::HelloAck, makeHelloAck(kProtocolVersion));

    FrameReader reader;
    Message message;
    std::string error;
    int decoded = 0;
    MessageType seen[3] = {MessageType::Unknown, MessageType::Unknown, MessageType::Unknown};

    for (size_t i = 0; i < stream.size(); ++i)
    {
        reader.append(stream.data() + i, 1);
        FrameReader::Result result = reader.next(message, error);
        while (result == FrameReader::Result::Message)
        {
            CHECK(decoded < 3);
            seen[decoded++] = message.type;
            result = reader.next(message, error);
        }
        CHECK(result != FrameReader::Result::Invalid);
    }

    CHECK_EQ(decoded, 3);
    CHECK(seen[0] == MessageType::Hello);
    CHECK(seen[1] == MessageType::ClockSyncRequest);
    CHECK(seen[2] == MessageType::HelloAck);
    CHECK_EQ(reader.buffered(), size_t(0));
}

MULTIROOM_TEST(matp_framing_refuses_a_number_that_disagrees_with_the_type_string)
{
    // The name decides and the number must agree. A disagreement is a desync
    // or a peer numbering things differently, and both are worth saying out
    // loud rather than working around.
    const std::string frame =
        encode(MessageType::HelloAck, makeHelloAck(kProtocolVersion)); // says "helloAck"

    std::string mangled = frame;
    mangled[4] = 0;
    mangled[5] = static_cast<char>(MessageType::Hello); // but claims Hello

    FrameReader reader;
    reader.append(mangled.data(), mangled.size());

    Message message;
    std::string error;
    CHECK(reader.next(message, error) == FrameReader::Result::Invalid);
    CHECK(!error.empty());
}

MULTIROOM_TEST(matp_system_report_survives_the_round_trip_with_every_field)
{
    SystemMetrics sent;
    sent.hasCpuPercent = true;
    sent.cpuPercent = 1.5;
    sent.hasSystemCpuPercent = true;
    sent.systemCpuPercent = 42.25;
    sent.hasMemoryBytes = true;
    sent.memoryBytes = 14680064;
    sent.hasPrivateMemoryBytes = true;
    sent.privateMemoryBytes = 10485760;
    sent.hasThreads = true;
    sent.threads = 12;
    sent.hasHandles = true;
    sent.handles = 240;
    sent.hasSystemMemoryTotalBytes = true;
    sent.systemMemoryTotalBytes = 8589934592ULL;
    sent.hasSystemMemoryAvailableBytes = true;
    sent.systemMemoryAvailableBytes = 4294967296ULL;
    sent.hasInternalMemoryTotalBytes = true;
    sent.internalMemoryTotalBytes = 327680;
    sent.hasInternalMemoryAvailableBytes = true;
    sent.internalMemoryAvailableBytes = 65536;
    sent.hasPsramTotalBytes = true;
    sent.psramTotalBytes = 4194304;
    sent.hasPsramAvailableBytes = true;
    sent.psramAvailableBytes = 2097152;
    sent.hasUptimeMs = true;
    sent.uptimeMs = 86400000;
    sent.hasSystemUptimeMs = true;
    sent.systemUptimeMs = 172800000;
    sent.hasTemperatureC = true;
    sent.temperatureC = 51.2;
    sent.cpuArchitecture = "armv6l";
    sent.cpuModel = "ARMv6-compatible processor rev 7 (v6l)";
    sent.machineModel = "Raspberry Pi Model B Plus Rev 1.2";
    sent.hasCpuCores = true;
    sent.cpuCores = 1;
    sent.hasCpuMhz = true;
    sent.cpuMhz = 700;
    sent.hasWifiRssiDbm = true;
    sent.wifiRssiDbm = -67;
    sent.hasWifiChannel = true;
    sent.wifiChannel = 11;
    sent.wifiBssid = "aa:bb:cc:dd:ee:ff";
    sent.hasWifiRxRateKbps = true;
    sent.wifiRxRateKbps = 72200;
    sent.networkLink = "wifi";

    const Message message = roundTrip(MessageType::SystemReport, makeSystemReport("s-1", sent));

    std::string session;
    SystemMetrics read;
    std::string error;
    CHECK(parseSystemReport(message.payload, session, read, error));
    CHECK_EQ(session, std::string("s-1"));
    CHECK(read.hasCpuPercent);
    CHECK_NEAR(read.cpuPercent, 1.5, 1e-9);
    CHECK(read.hasSystemCpuPercent);
    CHECK_NEAR(read.systemCpuPercent, 42.25, 1e-9);
    CHECK(read.hasMemoryBytes);
    CHECK_EQ(read.memoryBytes, uint64_t(14680064));
    CHECK(read.hasPrivateMemoryBytes);
    CHECK_EQ(read.privateMemoryBytes, uint64_t(10485760));
    CHECK(read.hasThreads);
    CHECK_EQ(read.threads, uint64_t(12));
    CHECK(read.hasHandles);
    CHECK_EQ(read.handles, uint64_t(240));
    CHECK(read.hasSystemMemoryTotalBytes);
    CHECK_EQ(read.systemMemoryTotalBytes, uint64_t(8589934592ULL));
    CHECK(read.hasSystemMemoryAvailableBytes);
    CHECK_EQ(read.systemMemoryAvailableBytes, uint64_t(4294967296ULL));
    CHECK(read.hasInternalMemoryTotalBytes);
    CHECK_EQ(read.internalMemoryTotalBytes, uint64_t(327680));
    CHECK(read.hasInternalMemoryAvailableBytes);
    CHECK_EQ(read.internalMemoryAvailableBytes, uint64_t(65536));
    CHECK(read.hasPsramTotalBytes);
    CHECK_EQ(read.psramTotalBytes, uint64_t(4194304));
    CHECK(read.hasPsramAvailableBytes);
    CHECK_EQ(read.psramAvailableBytes, uint64_t(2097152));
    CHECK(read.hasUptimeMs);
    CHECK_EQ(read.uptimeMs, uint64_t(86400000));
    CHECK(read.hasSystemUptimeMs);
    CHECK_EQ(read.systemUptimeMs, uint64_t(172800000));
    CHECK(read.hasTemperatureC);
    CHECK_NEAR(read.temperatureC, 51.2, 1e-9);
    CHECK_EQ(read.cpuArchitecture, std::string("armv6l"));
    CHECK_EQ(read.cpuModel, std::string("ARMv6-compatible processor rev 7 (v6l)"));
    CHECK_EQ(read.machineModel, std::string("Raspberry Pi Model B Plus Rev 1.2"));
    CHECK(read.hasCpuCores);
    CHECK_EQ(read.cpuCores, uint64_t(1));
    CHECK(read.hasCpuMhz);
    CHECK_EQ(read.cpuMhz, uint64_t(700));
    CHECK(read.hasWifiRssiDbm);
    CHECK_EQ(read.wifiRssiDbm, int32_t(-67));
    CHECK(read.hasWifiChannel);
    CHECK_EQ(read.wifiChannel, uint64_t(11));
    CHECK_EQ(read.wifiBssid, std::string("aa:bb:cc:dd:ee:ff"));
    CHECK(read.hasWifiRxRateKbps);
    CHECK_EQ(read.wifiRxRateKbps, uint64_t(72200));
    CHECK_EQ(read.networkLink, std::string("wifi"));
}

MULTIROOM_TEST(matp_system_report_survives_the_round_trip_with_no_field)
{
    const Message message =
        roundTrip(MessageType::SystemReport, makeSystemReport("s-1", SystemMetrics()));

    // Nothing known is nothing sent: an empty object, never zeros.
    const json::Value* system = message.payload.find("system");
    CHECK(system != nullptr && system->isObject() && system->members().empty());

    std::string session;
    SystemMetrics read;
    std::string error;
    CHECK(parseSystemReport(message.payload, session, read, error));
    CHECK(!read.hasCpuPercent && !read.hasSystemCpuPercent && !read.hasMemoryBytes);
    CHECK(!read.hasSystemMemoryTotalBytes && !read.hasSystemMemoryAvailableBytes);
    CHECK(!read.hasInternalMemoryTotalBytes && !read.hasInternalMemoryAvailableBytes &&
          !read.hasPsramTotalBytes && !read.hasPsramAvailableBytes);
    CHECK(!read.hasUptimeMs && !read.hasTemperatureC);
    CHECK(read.cpuArchitecture.empty() && read.cpuModel.empty() && read.machineModel.empty() &&
          !read.hasCpuCores && !read.hasCpuMhz);
    CHECK(!read.hasWifiRssiDbm && !read.hasWifiChannel && read.wifiBssid.empty() &&
          !read.hasWifiRxRateKbps && read.networkLink.empty());
}

MULTIROOM_TEST(matp_system_report_refuses_a_malformed_figure)
{
    std::string session;
    SystemMetrics read;
    std::string error;

    json::Value report = makeSystemReport("s-1", SystemMetrics());
    report["system"]["cpuPercent"] = json::Value(140.0);
    CHECK(!parseSystemReport(report, session, read, error));

    report = makeSystemReport("s-1", SystemMetrics());
    report["system"]["memoryBytes"] = json::Value("lots");
    CHECK(!parseSystemReport(report, session, read, error));

    // A positive dBm is a relative figure some driver mislabelled, not a signal.
    report = makeSystemReport("s-1", SystemMetrics());
    report["system"]["wifiRssiDbm"] = json::Value(int64_t(54));
    CHECK(!parseSystemReport(report, session, read, error));

    report = makeSystemReport("s-1", SystemMetrics());
    report["system"]["wifiBssid"] = json::Value(3.0);
    CHECK(!parseSystemReport(report, session, read, error));

    report = makeSystemReport("s-1", SystemMetrics());
    report["system"]["networkLink"] = json::Value(1.0);
    CHECK(!parseSystemReport(report, session, read, error));

    report = makeSystemReport("s-1", SystemMetrics());
    report["system"] = json::Value(3.0);
    CHECK(!parseSystemReport(report, session, read, error));

    json::Value missing = json::Value::makeObject();
    missing["type"] = json::Value("systemReport");
    missing["sessionId"] = json::Value("s-1");
    CHECK(!parseSystemReport(missing, session, read, error));
}
