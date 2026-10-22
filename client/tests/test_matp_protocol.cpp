#include "TestSupport.h"

#include "matp/Protocol.h"

#include <string>
#include <vector>

using namespace multiroom;

MULTIROOM_TEST(matp_standby_is_optional_and_requires_a_non_ready_stream)
{
    matp::StreamReady ready, parsed;
    std::string id, error;
    ready.reason = "powered off";
    auto message = matp::makeStreamReady("s-power", ready);
    CHECK(message.find("standby") == nullptr);
    CHECK(matp::parseStreamReady(message, id, parsed, error));
    CHECK(!parsed.ready && !parsed.standby);
    ready.standby = true;
    message = matp::makeStreamReady("s-power", ready);
    CHECK(matp::parseStreamReady(message, id, parsed, error));
    CHECK(!parsed.ready && parsed.standby);
    message["standby"] = json::Value("true");
    CHECK(!matp::parseStreamReady(message, id, parsed, error));
    message["standby"] = json::Value(true);
    message["ready"] = json::Value(true);
    CHECK(!matp::parseStreamReady(message, id, parsed, error));
}

MULTIROOM_TEST(matp_power_state_is_optional_and_rejects_invalid_or_conflicting_states)
{
    matp::StreamReady ready, parsed;
    std::string id, error;
    auto message = matp::makeStreamReady("s-power", ready);
    CHECK(matp::parseStreamReady(message, id, parsed, error));
    CHECK(parsed.powerState.empty());
    for (const std::string state : {"on", "standby", "standbyManual", "sleepButtonOnly"})
    {
        ready.powerState = state;
        ready.standby = state == "standby";
        message = matp::makeStreamReady("s-power", ready);
        CHECK(matp::parseStreamReady(message, id, parsed, error));
        CHECK_EQ(parsed.powerState, state);
    }
    // "off" was manual standby before 2026-10-19; it is left for a real power-off.
    for (const auto& invalid : {json::Value("off"), json::Value("sleep"), json::Value(""), json::Value(true)})
    {
        message["powerState"] = invalid;
        CHECK(!matp::parseStreamReady(message, id, parsed, error));
    }
    message["powerState"] = json::Value("standbyManual");
    message["ready"] = json::Value(true);
    CHECK(!matp::parseStreamReady(message, id, parsed, error));
    message["ready"] = json::Value(false);
    message["standby"] = json::Value(true);
    CHECK(!matp::parseStreamReady(message, id, parsed, error));
}

MULTIROOM_TEST(matp_sleep_names_its_wake_address_and_nothing_else_does)
{
    matp::StreamReady ready, parsed;
    std::string id, error;
    ready.powerState = "sleep";
    ready.wakeOnLan = "98:5d:ad:b2:c5:66";
    auto message = matp::makeStreamReady("s-sleep", ready);
    CHECK(matp::parseStreamReady(message, id, parsed, error));
    CHECK_EQ(parsed.wakeOnLan, ready.wakeOnLan);
    for (const char* invalid : {"98:5D:AD:B2:C5:66", "98-5d-ad-b2-c5-66", "98:5d:ad:b2:c5", ""})
    {
        message["wakeOnLan"] = json::Value(invalid);
        CHECK(!matp::parseStreamReady(message, id, parsed, error));
    }
    ready.wakeOnLan.clear();
    message = matp::makeStreamReady("s-sleep", ready);
    CHECK(!matp::parseStreamReady(message, id, parsed, error)); // sleep without an address
    ready.wakeOnLan = "98:5d:ad:b2:c5:66";
    ready.powerState = "sleepButtonOnly";
    message = matp::makeStreamReady("s-sleep", ready);
    CHECK(!matp::parseStreamReady(message, id, parsed, error)); // an address without sleep
}

namespace {

// The frame header, read the way a server would have to read it: big endian,
// length first, type second.
uint32_t frameLength(const std::string& frame)
{
    return (static_cast<uint32_t>(static_cast<unsigned char>(frame[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(frame[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(frame[2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(frame[3]));
}

uint16_t frameType(const std::string& frame)
{
    return static_cast<uint16_t>((static_cast<unsigned char>(frame[4]) << 8) |
                                 static_cast<unsigned char>(frame[5]));
}

// Builds a frame by hand so a test can put something in it that encode() never
// would.
std::string makeFrame(uint16_t type, const std::string& body)
{
    const uint32_t length = static_cast<uint32_t>(body.size() + sizeof(uint16_t));
    std::string frame;
    frame.push_back(static_cast<char>((length >> 24) & 0xFF));
    frame.push_back(static_cast<char>((length >> 16) & 0xFF));
    frame.push_back(static_cast<char>((length >> 8) & 0xFF));
    frame.push_back(static_cast<char>(length & 0xFF));
    frame.push_back(static_cast<char>((type >> 8) & 0xFF));
    frame.push_back(static_cast<char>(type & 0xFF));
    frame += body;
    return frame;
}

matp::FrameReader::Result readOne(const std::string& bytes, matp::Message& out,
                                  std::string& error)
{
    matp::FrameReader reader;
    reader.append(bytes.data(), bytes.size());
    return reader.next(out, error);
}

AudioFormat format(uint32_t rate, uint16_t channels, AudioFormat::SampleType type)
{
    AudioFormat result;
    result.sampleRate = rate;
    result.channels = channels;
    result.sampleType = type;
    return result;
}

} // namespace

MULTIROOM_TEST(matp_frame_header_is_big_endian_and_counts_the_type)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("helloAck");

    const std::string frame = matp::encode(matp::MessageType::HelloAck, payload);
    const std::string body = json::serialize(payload);

    CHECK_EQ(frame.size(), matp::kHeaderBytes + body.size());
    // The length of the message, the message being the type plus the payload.
    CHECK_EQ(frameLength(frame), static_cast<uint32_t>(body.size() + 2));
    CHECK_EQ(frameType(frame), static_cast<uint16_t>(2));
    CHECK_EQ(frame.substr(matp::kHeaderBytes), body);
}

MULTIROOM_TEST(matp_frame_round_trips_through_the_reader)
{
    matp::Identity identity;
    identity.clientId = "livingroom";
    identity.clientVersion = "0.1.0";
    identity.platform = "linux-armv6";

    matp::FrameReader reader;
    const std::string frame = matp::encode(matp::MessageType::Hello, matp::makeHello(identity));
    reader.append(frame.data(), frame.size());

    matp::Message message;
    std::string error;
    CHECK(reader.next(message, error) == matp::FrameReader::Result::Message);
    CHECK(message.type == matp::MessageType::Hello);
    CHECK_EQ(message.typeName, std::string("hello"));
    CHECK_EQ(message.wireType, static_cast<uint16_t>(1));

    std::string clientId;
    CHECK(json::getString(message.payload, "clientId", clientId));
    CHECK_EQ(clientId, std::string("livingroom"));

    // Nothing left over, and nothing more to report.
    CHECK(message.payload.isObject());
    CHECK(reader.next(message, error) == matp::FrameReader::Result::Incomplete);
    CHECK_EQ(reader.buffered(), static_cast<size_t>(0));
}

MULTIROOM_TEST(matp_reader_reassembles_a_message_split_across_reads)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("sessionConfig");
    payload["sessionId"] = json::Value("s-1");
    const std::string frame = matp::encode(matp::MessageType::SessionConfig, payload);

    matp::FrameReader reader;
    matp::Message message;
    std::string error;

    // One byte per read is the worst TCP can do, and the reader must survive
    // it: a message is not a recv().
    for (size_t i = 0; i + 1 < frame.size(); ++i)
    {
        reader.append(frame.data() + i, 1);
        CHECK(reader.next(message, error) == matp::FrameReader::Result::Incomplete);
    }
    reader.append(frame.data() + frame.size() - 1, 1);
    CHECK(reader.next(message, error) == matp::FrameReader::Result::Message);
    CHECK(message.type == matp::MessageType::SessionConfig);
}

MULTIROOM_TEST(matp_reader_returns_two_messages_from_one_read)
{
    json::Value ack = json::Value::makeObject();
    ack["type"] = json::Value("helloAck");
    json::Value errorPayload = json::Value::makeObject();
    errorPayload["type"] = json::Value("error");
    errorPayload["message"] = json::Value("busy");

    const std::string bytes = matp::encode(matp::MessageType::HelloAck, ack) +
                              matp::encode(matp::MessageType::Error, errorPayload);

    matp::FrameReader reader;
    reader.append(bytes.data(), bytes.size());

    matp::Message message;
    std::string error;
    CHECK(reader.next(message, error) == matp::FrameReader::Result::Message);
    CHECK(message.type == matp::MessageType::HelloAck);
    CHECK(reader.next(message, error) == matp::FrameReader::Result::Message);
    CHECK(message.type == matp::MessageType::Error);
    CHECK_EQ(matp::errorText(message.payload), std::string("busy"));
    CHECK(reader.next(message, error) == matp::FrameReader::Result::Incomplete);
}

MULTIROOM_TEST(matp_reader_refuses_a_stream_it_cannot_trust)
{
    matp::Message message;
    std::string error;

    // A length that does not even cover the type field.
    std::string tooShort = makeFrame(2, "{}");
    tooShort[3] = 1;
    CHECK(readOne(tooShort, message, error) == matp::FrameReader::Result::Invalid);
    CHECK(!error.empty());

    // A length nobody could mean: buffering towards it would be the desync,
    // not the recovery.
    std::string huge = makeFrame(2, "{}");
    huge[0] = static_cast<char>(0x7F);
    CHECK(readOne(huge, message, error) == matp::FrameReader::Result::Invalid);

    CHECK(readOne(makeFrame(2, "not json"), message, error) ==
          matp::FrameReader::Result::Invalid);
    CHECK(readOne(makeFrame(2, "[1,2]"), message, error) == matp::FrameReader::Result::Invalid);
    CHECK(readOne(makeFrame(2, "{\"clientId\":\"x\"}"), message, error) ==
          matp::FrameReader::Result::Invalid);

    // A type this client knows, framed with somebody else's number. The name
    // is the protocol's and the number is ours, so a contradiction is a
    // disagreement worth reporting rather than working around.
    CHECK(readOne(makeFrame(9, "{\"type\":\"helloAck\"}"), message, error) ==
          matp::FrameReader::Result::Invalid);
}

MULTIROOM_TEST(matp_reader_accepts_a_message_type_it_does_not_know)
{
    // Every later phase adds message types to this connection; a client that
    // predates one must not treat it as a broken stream.
    matp::Message message;
    std::string error;
    CHECK(readOne(makeFrame(41, "{\"type\":\"roomGrouping\",\"room\":5}"), message, error) ==
          matp::FrameReader::Result::Message);
    CHECK(message.type == matp::MessageType::Unknown);
    CHECK_EQ(message.typeName, std::string("roomGrouping"));
    CHECK_EQ(message.wireType, static_cast<uint16_t>(41));
}

MULTIROOM_TEST(matp_sample_format_names_keep_the_two_24_bit_layouts_apart)
{
    CHECK_EQ(std::string(matp::sampleFormatName(AudioFormat::SampleType::Int16)),
             std::string("s16le"));
    CHECK_EQ(std::string(matp::sampleFormatName(AudioFormat::SampleType::Int24)),
             std::string("s24le3"));
    CHECK_EQ(std::string(matp::sampleFormatName(AudioFormat::SampleType::Int24In32)),
             std::string("s24le"));
    CHECK_EQ(std::string(matp::sampleFormatName(AudioFormat::SampleType::Int32)),
             std::string("s32le"));
    CHECK_EQ(std::string(matp::sampleFormatName(AudioFormat::SampleType::Float32)),
             std::string("f32le"));

    // Packed and padded 24-bit are different layouts, and swapping them
    // produces noise rather than a quiet defect.
    AudioFormat::SampleType type = AudioFormat::SampleType::Int16;
    CHECK(matp::parseSampleFormat("s24le3", type));
    CHECK(type == AudioFormat::SampleType::Int24);
    CHECK(matp::parseSampleFormat("s24le", type));
    CHECK(type == AudioFormat::SampleType::Int24In32);
    CHECK(!matp::parseSampleFormat("s24", type));
    CHECK(!matp::parseSampleFormat("", type));
}

MULTIROOM_TEST(matp_hello_reports_the_identity_the_server_keys_on)
{
    matp::Identity identity;
    identity.clientId = "kitchen";
    identity.clientVersion = "0.1.0";
    identity.platform = "windows-x86_64";

    const json::Value hello = matp::makeHello(identity);
    std::string text;
    CHECK(json::getString(hello, "type", text));
    CHECK_EQ(text, std::string("hello"));
    CHECK(json::getString(hello, "clientId", text));
    CHECK_EQ(text, std::string("kitchen"));
    CHECK(json::getString(hello, "clientVersion", text));
    CHECK_EQ(text, std::string("0.1.0"));
    CHECK(json::getString(hello, "platform", text));
    CHECK_EQ(text, std::string("windows-x86_64"));

    int64_t version = 0;
    CHECK(json::getInt(hello, "protocolVersion", version));
    CHECK_EQ(version, static_cast<int64_t>(matp::kProtocolVersion));
}

MULTIROOM_TEST(matp_audio_capabilities_carries_the_hardware_volume)
{
    matp::CapabilityReport report;
    report.formats.push_back(format(48000, 2, AudioFormat::SampleType::Int16));

    // Left out by a client that does not say, and read back as unknown.
    matp::CapabilityReport parsed;
    std::string error;
    CHECK(matp::parseAudioCapabilities(matp::makeAudioCapabilities(report), parsed, error));
    CHECK(!parsed.hasHardwareVolume);
    CHECK(parsed.accessMode.empty());

    report.hasHardwareVolume = true;
    report.hardwareVolume.available = true;
    report.hardwareVolume.control = "Digital";
    report.hardwareVolume.steps = 208;
    report.hardwareVolume.hasDecibels = true;
    report.hardwareVolume.minDb = -103.5;
    report.hardwareVolume.maxDb = 0.0;
    report.hardwareVolume.hasMute = true;
    CHECK(matp::parseAudioCapabilities(matp::makeAudioCapabilities(report), parsed, error));
    CHECK(parsed.hasHardwareVolume);
    CHECK(parsed.hardwareVolume.available);
    CHECK_EQ(parsed.hardwareVolume.control, std::string("Digital"));
    CHECK_EQ(parsed.hardwareVolume.steps, static_cast<uint32_t>(208));
    CHECK(parsed.hardwareVolume.hasDecibels);
    CHECK_EQ(parsed.hardwareVolume.minDb, -103.5);
    CHECK_EQ(parsed.hardwareVolume.maxDb, 0.0);
    CHECK(parsed.hardwareVolume.hasMute);

    // A device without one says so.
    report.hardwareVolume = matp::HardwareVolumeCapability();
    CHECK(matp::parseAudioCapabilities(matp::makeAudioCapabilities(report), parsed, error));
    CHECK(parsed.hasHardwareVolume);
    CHECK(!parsed.hardwareVolume.available);

    // Malformed is refused: no steps, half a dB range, a range upside down.
    const char* const malformed[] = {
        "{\"available\":true}",
        "{\"available\":true,\"steps\":64,\"minDb\":-60}",
        "{\"available\":true,\"steps\":64,\"minDb\":0,\"maxDb\":-60}",
        "{\"available\":\"yes\"}",
        "[]",
    };
    for (const char* text : malformed)
    {
        json::Value payload = matp::makeAudioCapabilities(report);
        json::Value volume;
        CHECK(json::parse(text, volume, error));
        payload["hardwareVolume"] = volume;
        CHECK(!matp::parseAudioCapabilities(payload, parsed, error));
    }
}

MULTIROOM_TEST(matp_audio_capabilities_carries_the_access_mode)
{
    matp::CapabilityReport report;
    report.formats.push_back(format(48000, 2, AudioFormat::SampleType::Int16));

    matp::CapabilityReport parsed;
    std::string error;
    for (const char* mode : {"exclusive", "shared"})
    {
        report.accessMode = mode;
        CHECK(matp::parseAudioCapabilities(matp::makeAudioCapabilities(report), parsed, error));
        CHECK_EQ(parsed.accessMode, std::string(mode));
    }

    json::Value payload = matp::makeAudioCapabilities(report);
    payload["accessMode"] = json::Value("exclusive-only");
    CHECK(!matp::parseAudioCapabilities(payload, parsed, error));
    payload["accessMode"] = json::Value(true);
    CHECK(!matp::parseAudioCapabilities(payload, parsed, error));
}

MULTIROOM_TEST(matp_audio_capabilities_lists_what_the_device_accepted)
{
    matp::CapabilityReport report;
    report.deviceId = "hw:CARD=USB,DEV=0";
    report.deviceName = "USB DAC";
    report.preferredSampleRate = 48000;
    report.formats.push_back(format(48000, 2, AudioFormat::SampleType::Int24In32));
    report.formats.push_back(format(44100, 2, AudioFormat::SampleType::Int16));

    const json::Value capabilities = matp::makeAudioCapabilities(report);

    std::string text;
    CHECK(json::getString(capabilities, "type", text));
    CHECK_EQ(text, std::string("audioCapabilities"));
    CHECK(json::getString(capabilities, "deviceId", text));
    CHECK_EQ(text, std::string("hw:CARD=USB,DEV=0"));

    int64_t rate = 0;
    CHECK(json::getInt(capabilities, "preferredSampleRate", rate));
    CHECK_EQ(rate, static_cast<int64_t>(48000));

    const json::Value* formats = capabilities.find("formats");
    CHECK(formats != nullptr && formats->isArray());
    CHECK_EQ(formats->size(), static_cast<size_t>(2));

    CHECK(json::getInt(formats->at(0), "sampleRate", rate));
    CHECK_EQ(rate, static_cast<int64_t>(48000));
    CHECK(json::getString(formats->at(0), "sampleFormat", text));
    CHECK_EQ(text, std::string("s24le"));
    CHECK(json::getString(formats->at(1), "sampleFormat", text));
    CHECK_EQ(text, std::string("s16le"));
}

MULTIROOM_TEST(matp_hello_ack_without_a_version_is_not_a_version_of_zero)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("helloAck");

    matp::HelloAck ack;
    std::string error;
    CHECK(matp::parseHelloAck(payload, ack, error));
    CHECK(!ack.hasProtocolVersion);

    payload["protocolVersion"] = json::Value(static_cast<uint32_t>(1));
    CHECK(matp::parseHelloAck(payload, ack, error));
    CHECK(ack.hasProtocolVersion);
    CHECK_EQ(ack.protocolVersion, static_cast<uint32_t>(1));

    // Present but of the wrong type is an error, the same rule the config
    // parser follows.
    json::Value broken = json::Value::makeObject();
    broken["protocolVersion"] = json::Value("one");
    CHECK(!matp::parseHelloAck(broken, ack, error));
    CHECK(!error.empty());

    // 2.9 is not version 2.
    broken["protocolVersion"] = json::Value(2.9);
    CHECK(!matp::parseHelloAck(broken, ack, error));
}

MULTIROOM_TEST(matp_session_config_needs_a_session_and_a_complete_format)
{
    json::Value formatValue = json::Value::makeObject();
    formatValue["sampleRate"] = json::Value(static_cast<uint32_t>(48000));
    formatValue["channels"] = json::Value(static_cast<uint32_t>(2));
    formatValue["sampleFormat"] = json::Value("s24le3");

    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("sessionConfig");
    payload["sessionId"] = json::Value("1f0c-...");
    payload["format"] = formatValue;

    matp::SessionConfig config;
    std::string error;
    CHECK(matp::parseSessionConfig(payload, config, error));
    CHECK_EQ(config.sessionId, std::string("1f0c-..."));
    CHECK_EQ(config.format.sampleRate, static_cast<uint32_t>(48000));
    CHECK_EQ(config.format.channels, static_cast<uint16_t>(2));
    CHECK(config.format.sampleType == AudioFormat::SampleType::Int24);

    // Audio packets are keyed on the session id (section 13), so a config
    // without one describes a stream that cannot be told from a stale one.
    json::Value withoutId = json::Value::makeObject();
    withoutId["type"] = json::Value("sessionConfig");
    withoutId["format"] = formatValue;
    CHECK(!matp::parseSessionConfig(withoutId, config, error));

    json::Value badRate = json::Value::makeObject();
    badRate["sampleRate"] = json::Value(static_cast<uint32_t>(0));
    badRate["channels"] = json::Value(static_cast<uint32_t>(2));
    badRate["sampleFormat"] = json::Value("s16le");
    json::Value withBadRate = json::Value::makeObject();
    withBadRate["sessionId"] = json::Value("s");
    withBadRate["format"] = badRate;
    CHECK(!matp::parseSessionConfig(withBadRate, config, error));

    json::Value unknownLayout = json::Value::makeObject();
    unknownLayout["sampleRate"] = json::Value(static_cast<uint32_t>(48000));
    unknownLayout["channels"] = json::Value(static_cast<uint32_t>(2));
    unknownLayout["sampleFormat"] = json::Value("s24le4");
    json::Value withUnknownLayout = json::Value::makeObject();
    withUnknownLayout["sessionId"] = json::Value("s");
    withUnknownLayout["format"] = unknownLayout;
    CHECK(!matp::parseSessionConfig(withUnknownLayout, config, error));

    // No format: a client that reported none, with a session but no stream.
    json::Value withoutFormat = json::Value::makeObject();
    withoutFormat["sessionId"] = json::Value("s");
    CHECK(matp::parseSessionConfig(withoutFormat, config, error));
    CHECK(!config.hasFormat);
}

MULTIROOM_TEST(matp_protocol_builds_and_parses_a_clock_sync_exchange)
{
    const uint64_t sendNs = 1234567890123456789ULL;

    const json::Value request = matp::makeClockSyncRequest(sendNs);
    std::string type;
    CHECK(json::getString(request, "type", type));
    CHECK_EQ(type, std::string("clockSyncRequest"));

    // A nanosecond timestamp has more significant digits than a double has
    // mantissa, so it has to survive the wire as an integer or not at all.
    const std::string encoded = json::serialize(request);
    json::Value reparsed;
    std::string parseError;
    CHECK(json::parse(encoded, reparsed, parseError));
    int64_t echoed = 0;
    CHECK(json::getInt(reparsed, "clientSendNs", echoed));
    CHECK_EQ(static_cast<uint64_t>(echoed), sendNs);

    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("clockSyncReply");
    payload["clientSendNs"] = json::Value(sendNs);
    payload["serverReceiveNs"] = json::Value(static_cast<uint64_t>(500000000000ULL));
    payload["serverSendNs"] = json::Value(static_cast<uint64_t>(500000000120ULL));

    matp::ClockSyncReply reply;
    std::string error;
    CHECK(matp::parseClockSyncReply(payload, reply, error));
    CHECK_EQ(reply.clientSendNs, sendNs);
    CHECK_EQ(reply.serverReceiveNs, static_cast<uint64_t>(500000000000ULL));
    CHECK_EQ(reply.serverSendNs, static_cast<uint64_t>(500000000120ULL));
}

MULTIROOM_TEST(matp_protocol_refuses_clock_timestamps_that_are_not_whole_nanoseconds)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("clockSyncReply");
    payload["clientSendNs"] = json::Value(static_cast<uint64_t>(1000));
    payload["serverReceiveNs"] = json::Value(static_cast<uint64_t>(2000));

    matp::ClockSyncReply reply;
    std::string error;
    CHECK(!matp::parseClockSyncReply(payload, reply, error));

    // A value that has been through a double has already lost the precision
    // the exchange exists to measure.
    payload["serverSendNs"] = json::Value(2.5e18);
    CHECK(!matp::parseClockSyncReply(payload, reply, error));

    payload["serverSendNs"] = json::Value("2500");
    CHECK(!matp::parseClockSyncReply(payload, reply, error));

    // The server answering before it was asked.
    payload["serverSendNs"] = json::Value(static_cast<uint64_t>(1999));
    CHECK(!matp::parseClockSyncReply(payload, reply, error));

    payload["serverSendNs"] = json::Value(static_cast<uint64_t>(2100));
    CHECK(matp::parseClockSyncReply(payload, reply, error));
}

MULTIROOM_TEST(matp_playback_report_omits_the_drift_it_has_not_measured)
{
    matp::PlaybackReport report;
    report.localMonotonicTimeNs = 821773812345ULL;
    report.deviceFramePosition = 23849231;
    report.bufferedDurationNs = 1470000000ULL;

    // No clock, so no measurement -- but the client is there and the two
    // position fields say so (section 25).
    json::Value payload = matp::makePlaybackReport("s-1", report);
    std::string type;
    std::string quality;
    int64_t instant = 0;
    int64_t position = 0;
    int64_t buffered = 0;
    int64_t underruns = -1;
    CHECK(json::getString(payload, "type", type));
    CHECK(json::getString(payload, "clockQuality", quality));
    CHECK(json::getInt(payload, "localMonotonicTimeNs", instant));
    CHECK(json::getInt(payload, "deviceFramePosition", position));
    CHECK(json::getInt(payload, "bufferedDurationNs", buffered));
    CHECK(json::getInt(payload, "underruns", underruns));
    CHECK_EQ(type, std::string("playbackReport"));
    CHECK_EQ(quality, std::string("unavailable"));
    CHECK_EQ(instant, static_cast<int64_t>(821773812345ULL));
    CHECK_EQ(position, static_cast<int64_t>(23849231));
    CHECK_EQ(buffered, static_cast<int64_t>(1470000000ULL));
    CHECK_EQ(underruns, static_cast<int64_t>(0));

    // A drift of zero is what a perfect clock looks like, not what an
    // unmeasured one looks like, so neither it nor its confidence is sent.
    CHECK(payload.find("measuredSampleRate") == nullptr);
    CHECK(payload.find("measuredDriftPpm") == nullptr);
    CHECK(payload.find("driftConfidence") == nullptr);

    // And 0 ns is a real position on the media timeline, so a room that has
    // not reached the start reports no audible position at all.
    CHECK(payload.find("timelineNsAudible") == nullptr);

    report.clockQuality = AudioClockQuality::Hardware;
    report.hasTimelineNsAudible = true;
    report.timelineNsAudible = 195349312000ULL;
    report.underruns = 1;
    report.hasDrift = true;
    report.measuredSampleRate = 48001.44;
    report.measuredDriftPpm = 30.0;
    report.driftConfidence = 0.97;

    payload = matp::makePlaybackReport("s-1", report);
    double rate = 0.0;
    double ppm = 0.0;
    double confidence = 0.0;
    int64_t audible = 0;
    CHECK(json::getString(payload, "clockQuality", quality));
    CHECK(json::getInt(payload, "timelineNsAudible", audible));
    CHECK(json::getInt(payload, "underruns", underruns));
    CHECK(json::getDouble(payload, "measuredSampleRate", rate));
    CHECK(json::getDouble(payload, "measuredDriftPpm", ppm));
    CHECK(json::getDouble(payload, "driftConfidence", confidence));
    CHECK_EQ(quality, std::string("hardware"));
    CHECK_EQ(audible, static_cast<int64_t>(195349312000ULL));
    CHECK_EQ(underruns, static_cast<int64_t>(1));
    CHECK_NEAR(rate, 48001.44, 1e-9);
    CHECK_NEAR(ppm, 30.0, 1e-9);
    CHECK_NEAR(confidence, 0.97, 1e-9);
}

MULTIROOM_TEST(matp_start_at_needs_both_an_instant_and_a_timeline_position)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("startAt");

    matp::StartAt request;
    std::string error;
    CHECK(!matp::parseStartAt(payload, request, error));

    payload["serverTimeNs"] = json::Value(static_cast<uint64_t>(102000000000ULL));
    // Neither number has a defensible default: a start that guessed one would
    // be a room playing at the wrong moment.
    CHECK(!matp::parseStartAt(payload, request, error));

    payload["timelineNs"] = json::Value(180000000000.5);
    CHECK(!matp::parseStartAt(payload, request, error));

    // The two numbers are in different clocks and neither is a frame: an
    // instant on the server's monotonic clock, and a position in the media.
    payload["timelineNs"] = json::Value(static_cast<uint64_t>(180000000000ULL));
    CHECK(matp::parseStartAt(payload, request, error));
    CHECK_EQ(request.serverTimeNs, static_cast<uint64_t>(102000000000ULL));
    CHECK_EQ(request.timelineNs, static_cast<uint64_t>(180000000000ULL));
    CHECK(request.sessionId.empty()); // the server did not say, which is allowed

    payload["sessionId"] = json::Value("");
    CHECK(!matp::parseStartAt(payload, request, error));

    payload["sessionId"] = json::Value("s-1");
    CHECK(matp::parseStartAt(payload, request, error));
    CHECK_EQ(request.sessionId, std::string("s-1"));
}

MULTIROOM_TEST(matp_transport_at_reads_pause_and_stop_the_same_way)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("pauseAt");

    matp::TransportAt request;
    std::string error;
    CHECK(!matp::parseTransportAt(payload, request, error));

    payload["serverTimeNs"] = json::Value(static_cast<uint64_t>(145000000000ULL));
    CHECK(!matp::parseTransportAt(payload, request, error));

    payload["timelineNs"] = json::Value(static_cast<uint64_t>(240000000000ULL));
    CHECK(matp::parseTransportAt(payload, request, error));
    CHECK_EQ(request.serverTimeNs, static_cast<uint64_t>(145000000000ULL));
    CHECK_EQ(request.timelineNs, static_cast<uint64_t>(240000000000ULL));
    CHECK(request.sessionId.empty());

    // Section 38's stopAt carries the same two numbers, which is why one
    // parser reads both: only what the room does with them differs.
    payload["type"] = json::Value("stopAt");
    payload["sessionId"] = json::Value("s-1");
    CHECK(matp::parseTransportAt(payload, request, error));
    CHECK_EQ(request.sessionId, std::string("s-1"));
}

MULTIROOM_TEST(matp_set_volume_accepts_a_gain_a_mute_or_both)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("setVolume");

    matp::SetVolume request;
    std::string error;
    // A message that asks for nothing is a server bug, and succeeding quietly
    // would hide it.
    CHECK(!matp::parseSetVolume(payload, request, error));

    payload["value"] = json::Value(0.75);
    CHECK(matp::parseSetVolume(payload, request, error));
    CHECK(request.hasValue);
    CHECK_NEAR(request.value, 0.75, 1e-12);
    CHECK(!request.hasMuted);
    CHECK(!request.hasApplyAtTimelineNs);

    // Mute is a field here rather than a message of its own, so a mute that
    // does not name a volume keeps the one the room already had.
    json::Value muteOnly = json::Value::makeObject();
    muteOnly["type"] = json::Value("setVolume");
    muteOnly["muted"] = json::Value(true);
    CHECK(matp::parseSetVolume(muteOnly, request, error));
    CHECK(!request.hasValue);
    CHECK(request.hasMuted);
    CHECK(request.muted);

    payload["muted"] = json::Value("yes");
    CHECK(!matp::parseSetVolume(payload, request, error));

    payload["muted"] = json::Value(false);
    payload["value"] = json::Value(-0.1);
    CHECK(!matp::parseSetVolume(payload, request, error));

    // Above unity parses: what a room without headroom does about it is not a
    // parsing question, and refusing here would drop the mute flag with it.
    payload["value"] = json::Value(2.0);
    CHECK(matp::parseSetVolume(payload, request, error));

    payload["value"] = json::Value(0.5);
    payload["applyAtTimelineNs"] = json::Value(static_cast<uint64_t>(142000000000ULL));
    CHECK(matp::parseSetVolume(payload, request, error));
    CHECK(request.hasApplyAtTimelineNs);
    CHECK_EQ(request.applyAtTimelineNs, static_cast<uint64_t>(142000000000ULL));

    payload["applyAtTimelineNs"] = json::Value(-1);
    CHECK(!matp::parseSetVolume(payload, request, error));
}

MULTIROOM_TEST(matp_track_boundary_needs_only_a_position)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("trackBoundary");

    matp::TrackBoundary boundary;
    std::string error;
    // The position is the part every room has to agree on; the names are not.
    CHECK(!matp::parseTrackBoundary(payload, boundary, error));

    payload["timelineNs"] = json::Value(static_cast<uint64_t>(180000000000ULL));
    CHECK(matp::parseTrackBoundary(payload, boundary, error));
    CHECK_EQ(boundary.timelineNs, static_cast<uint64_t>(180000000000ULL));
    CHECK(boundary.trackId.empty());
    CHECK(boundary.title.empty());

    payload["trackId"] = json::Value("track-B");
    payload["title"] = json::Value("Second Song");
    CHECK(matp::parseTrackBoundary(payload, boundary, error));
    CHECK_EQ(boundary.trackId, std::string("track-B"));
    CHECK_EQ(boundary.title, std::string("Second Song"));

    // A title that arrived as a number is a server that meant to say
    // something, not a boundary without a title.
    payload["title"] = json::Value(static_cast<uint64_t>(7));
    CHECK(!matp::parseTrackBoundary(payload, boundary, error));
}

MULTIROOM_TEST(matp_track_info_names_a_track_and_no_position)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("trackInfo");

    matp::TrackInfo info;
    std::string error;
    // Unlike a boundary, this message is nothing but the names: one that
    // carries neither asks for nothing at all.
    CHECK(!matp::parseTrackInfo(payload, info, error));

    payload["title"] = json::Value("Second Song");
    CHECK(matp::parseTrackInfo(payload, info, error));
    CHECK_EQ(info.title, std::string("Second Song"));
    CHECK(info.trackId.empty());

    payload["trackId"] = json::Value("track-B");
    payload["sessionId"] = json::Value("s-1");
    CHECK(matp::parseTrackInfo(payload, info, error));
    CHECK_EQ(info.trackId, std::string("track-B"));
    CHECK_EQ(info.sessionId, std::string("s-1"));

    payload["trackId"] = json::Value(static_cast<uint64_t>(7));
    CHECK(!matp::parseTrackInfo(payload, info, error));
}

MULTIROOM_TEST(matp_pong_answers_with_the_pings_own_body)
{
    // The protocol gives PING no fields, so whatever the server correlates on
    // has to come back to it untouched -- including a field this client has
    // never heard of.
    json::Value ping = json::Value::makeObject();
    ping["type"] = json::Value("ping");
    ping["seq"] = json::Value(static_cast<uint64_t>(4711));
    ping["serverSendNs"] = json::Value(static_cast<uint64_t>(123456789));

    const json::Value pong = matp::makePong(ping);
    std::string type;
    CHECK(json::getString(pong, "type", type));
    CHECK_EQ(type, std::string("pong"));

    int64_t seq = 0;
    CHECK(json::getInt(pong, "seq", seq));
    CHECK_EQ(seq, static_cast<int64_t>(4711));

    int64_t sentNs = 0;
    CHECK(json::getInt(pong, "serverSendNs", sentNs));
    CHECK_EQ(sentNs, static_cast<int64_t>(123456789));
}

MULTIROOM_TEST(matp_ping_and_pong_and_track_info_have_stable_numbers)
{
    // A server keyed to these cannot be re-released in step with the client,
    // so the numbers are part of the wire format once assigned.
    CHECK_EQ(static_cast<uint16_t>(matp::MessageType::TrackInfo), static_cast<uint16_t>(18));
    CHECK_EQ(static_cast<uint16_t>(matp::MessageType::Ping), static_cast<uint16_t>(19));
    CHECK_EQ(static_cast<uint16_t>(matp::MessageType::Pong), static_cast<uint16_t>(20));

    matp::MessageType type = matp::MessageType::Unknown;
    CHECK(matp::parseMessageType("ping", type));
    CHECK(type == matp::MessageType::Ping);
    CHECK(matp::parseMessageType("pong", type));
    CHECK(type == matp::MessageType::Pong);
    CHECK(matp::parseMessageType("trackInfo", type));
    CHECK(type == matp::MessageType::TrackInfo);
}

MULTIROOM_TEST(matp_buffer_status_reports_media_time_and_omits_what_it_does_not_know)
{
    // Section 19: until the first packet has arrived there is no position to
    // report, and 0 ns is a real position rather than "nothing yet".
    matp::BufferStatus status;
    status.streamFrames = 1234; // local truth, and never sent

    json::Value payload = matp::makeBufferStatus("s-1", status);
    std::string type;
    int64_t duration = -1;
    CHECK(json::getString(payload, "type", type));
    CHECK_EQ(type, std::string("bufferStatus"));
    CHECK(payload.find("playheadTimelineNs") == nullptr);
    CHECK(payload.find("bufferedUntilTimelineNs") == nullptr);
    CHECK(payload.find("streamFrames") == nullptr);
    CHECK(payload.find("bufferedFrames") == nullptr);

    // The duration is always there: an empty buffer holds zero nanoseconds,
    // which is a fact and not a missing value.
    CHECK(json::getInt(payload, "bufferedDurationNs", duration));
    CHECK_EQ(duration, static_cast<int64_t>(0));

    status.hasContent = true;
    status.playheadTimelineNs = 195000000000ULL;
    status.bufferedUntilTimelineNs = 197000000000ULL;
    status.bufferedDurationNs = 2000000000ULL;

    payload = matp::makeBufferStatus("s-1", status);
    int64_t playhead = 0;
    int64_t until = 0;
    CHECK(json::getInt(payload, "playheadTimelineNs", playhead));
    CHECK(json::getInt(payload, "bufferedUntilTimelineNs", until));
    CHECK(json::getInt(payload, "bufferedDurationNs", duration));
    CHECK_EQ(playhead, static_cast<int64_t>(195000000000ULL));
    CHECK_EQ(until, static_cast<int64_t>(197000000000ULL));
    CHECK_EQ(duration, static_cast<int64_t>(2000000000ULL));
}

MULTIROOM_TEST(matp_clock_sync_result_carries_all_four_timestamps)
{
    // Section 8: the server holds C1, S2 and S3 already but can never know C4,
    // so without this message it has no client-to-server clock model and
    // cannot place a PLAYBACK_REPORT's instant in its own time.
    const uint64_t c1 = 821773000000ULL;
    const uint64_t s2 = 102000120000ULL;
    const uint64_t s3 = 102000135000ULL;
    const uint64_t c4 = 821773310000ULL;

    const json::Value payload = matp::makeClockSyncResult(c1, s2, s3, c4);

    // Through the serializer and back: a nanosecond timestamp has more
    // significant digits than a double has mantissa.
    const std::string encoded = json::serialize(payload);
    json::Value reparsed;
    std::string parseError;
    CHECK(json::parse(encoded, reparsed, parseError));

    std::string type;
    int64_t clientSend = 0;
    int64_t serverReceive = 0;
    int64_t serverSend = 0;
    int64_t clientReceive = 0;
    CHECK(json::getString(reparsed, "type", type));
    CHECK(json::getInt(reparsed, "clientSendNs", clientSend));
    CHECK(json::getInt(reparsed, "serverReceiveNs", serverReceive));
    CHECK(json::getInt(reparsed, "serverSendNs", serverSend));
    CHECK(json::getInt(reparsed, "clientReceiveNs", clientReceive));
    CHECK_EQ(type, std::string("clockSyncResult"));
    CHECK_EQ(static_cast<uint64_t>(clientSend), c1);
    CHECK_EQ(static_cast<uint64_t>(serverReceive), s2);
    CHECK_EQ(static_cast<uint64_t>(serverSend), s3);
    CHECK_EQ(static_cast<uint64_t>(clientReceive), c4);

    // Raw timestamps only: the client measures and the server models, so a
    // conclusion of this client's has no business in the message.
    CHECK(reparsed.find("offsetNs") == nullptr);
    CHECK(reparsed.find("roundTripNs") == nullptr);

    CHECK_EQ(static_cast<uint16_t>(matp::MessageType::ClockSyncResult),
             static_cast<uint16_t>(21));
    matp::MessageType parsedType = matp::MessageType::Unknown;
    CHECK(matp::parseMessageType("clockSyncResult", parsedType));
    CHECK(parsedType == matp::MessageType::ClockSyncResult);
}

MULTIROOM_TEST(matp_protocol_flush_round_trips_and_its_number_is_pinned)
{
    matp::TransportAt at;
    at.sessionId = "s-seek";
    at.serverTimeNs = 145000000000ULL;
    at.timelineNs = 240000000000ULL;

    // FLUSH says what PAUSE_AT and STOP_AT say, so the same parser reads it.
    const json::Value payload = matp::makeFlush(at);
    std::string type;
    CHECK(json::getString(payload, "type", type));
    CHECK_EQ(type, std::string("flush"));

    matp::TransportAt parsed;
    std::string error;
    CHECK(matp::parseTransportAt(payload, parsed, error));
    CHECK_EQ(parsed.sessionId, std::string("s-seek"));
    CHECK_EQ(parsed.serverTimeNs, at.serverTimeNs);
    CHECK_EQ(parsed.timelineNs, at.timelineNs);

    // Section 47's order, after clockSyncResult; the server is keyed to it.
    CHECK_EQ(static_cast<uint16_t>(matp::MessageType::Flush), static_cast<uint16_t>(22));
    matp::MessageType parsedType = matp::MessageType::Unknown;
    CHECK(matp::parseMessageType("flush", parsedType));
    CHECK(parsedType == matp::MessageType::Flush);
}

MULTIROOM_TEST(matp_protocol_version_is_two)
{
    // Version 1 expressed every position as a frame of a canonical timeline at
    // one fixed rate and is withdrawn (section 51). The two are
    // wire-incompatible, so the version is what keeps them apart.
    CHECK_EQ(matp::kProtocolVersion, static_cast<uint32_t>(2));
}

MULTIROOM_TEST(matp_audio_devices_round_trips)
{
    matp::AudioDeviceList list;
    list.configuredDeviceId = "hw:CARD=Gone,DEV=0";
    list.selectedDeviceId = "hw:CARD=Audio,DEV=0";
    matp::AudioDeviceEntry device;
    device.id = "hw:CARD=Audio,DEV=0";
    device.name = "Onboard";
    device.isDefault = true;
    list.devices.push_back(device);
    device.id = "hw:CARD=USB,DEV=0";
    device.name = "USB DAC";
    device.isDefault = false;
    list.devices.push_back(device);

    const json::Value payload = matp::makeAudioDevices("s-1", list);
    std::string sessionId;
    matp::AudioDeviceList parsed;
    std::string error;
    CHECK(matp::parseAudioDevices(payload, sessionId, parsed, error));
    CHECK_EQ(sessionId, std::string("s-1"));
    CHECK_EQ(parsed.configuredDeviceId, list.configuredDeviceId);
    CHECK_EQ(parsed.selectedDeviceId, list.selectedDeviceId);
    CHECK_EQ(parsed.devices.size(), static_cast<size_t>(2));
    CHECK_EQ(parsed.devices[1].name, std::string("USB DAC"));
    CHECK(parsed.devices[0].isDefault);
    CHECK(!parsed.devices[1].isDefault);

    // No device in use is left out, not sent as an empty id.
    list.selectedDeviceId.clear();
    CHECK(matp::makeAudioDevices("s-1", list).find("selectedDeviceId") == nullptr);

    json::Value broken = payload;
    broken["devices"].pushBack(json::Value::makeObject());
    CHECK(!matp::parseAudioDevices(broken, sessionId, parsed, error));

    CHECK_EQ(static_cast<uint16_t>(matp::MessageType::AudioDevices), static_cast<uint16_t>(24));
    CHECK_EQ(static_cast<uint16_t>(matp::MessageType::SetAudioDevice), static_cast<uint16_t>(25));
}

MULTIROOM_TEST(matp_set_audio_device_round_trips_and_allows_the_default)
{
    matp::SetAudioDevice request;
    request.sessionId = "s-1";
    request.deviceId = "";

    matp::SetAudioDevice parsed;
    parsed.deviceId = "stale";
    std::string error;
    CHECK(matp::parseSetAudioDevice(matp::makeSetAudioDevice(request), parsed, error));
    CHECK_EQ(parsed.sessionId, std::string("s-1"));
    CHECK_EQ(parsed.deviceId, std::string());

    json::Value missing = json::Value::makeObject();
    missing["type"] = json::Value("setAudioDevice");
    CHECK(!matp::parseSetAudioDevice(missing, parsed, error));

    matp::MessageType type = matp::MessageType::Unknown;
    CHECK(matp::parseMessageType("setAudioDevice", type));
    CHECK(type == matp::MessageType::SetAudioDevice);
}

MULTIROOM_TEST(matp_set_buffer_and_buffer_config_round_trip_and_refuse_a_non_positive_target)
{
    CHECK_EQ(static_cast<uint16_t>(matp::MessageType::SetBuffer), static_cast<uint16_t>(30));
    CHECK_EQ(static_cast<uint16_t>(matp::MessageType::BufferConfig), static_cast<uint16_t>(31));
    matp::MessageType type = matp::MessageType::Unknown;
    CHECK(matp::parseMessageType("setBuffer", type));
    CHECK(type == matp::MessageType::SetBuffer);
    CHECK(matp::parseMessageType("bufferConfig", type));
    CHECK(type == matp::MessageType::BufferConfig);

    matp::SetBuffer request;
    request.sessionId = "s-1";
    request.bufferMs = 2500;
    matp::SetBuffer parsed;
    std::string error;
    CHECK(matp::parseSetBuffer(matp::makeSetBuffer(request), parsed, error));
    CHECK_EQ(parsed.sessionId, std::string("s-1"));
    CHECK_EQ(parsed.bufferMs, static_cast<uint32_t>(2500));

    // The range is the client's to judge; zero, a fraction, a string and a
    // value past 32 bits are not a target at all.
    json::Value bad = matp::makeSetBuffer(request);
    for (const json::Value& value : {json::Value(0), json::Value(-5), json::Value(1.5),
                                     json::Value("2500"), json::Value(int64_t(1) << 33)})
    {
        bad["bufferMs"] = value;
        CHECK(!matp::parseSetBuffer(bad, parsed, error));
    }

    std::string sessionId;
    uint32_t bufferMs = 0;
    CHECK(matp::parseBufferConfig(matp::makeBufferConfig("s-2", 800), sessionId, bufferMs,
                                  error));
    CHECK_EQ(sessionId, std::string("s-2"));
    CHECK_EQ(bufferMs, static_cast<uint32_t>(800));
    // The client's report names its session, always.
    json::Value anonymous = matp::makeBufferConfig("s-2", 800);
    anonymous["sessionId"] = json::Value("");
    CHECK(!matp::parseBufferConfig(anonymous, sessionId, bufferMs, error));
}

MULTIROOM_TEST(matp_rescan_audio_round_trips_with_and_without_a_session)
{
    std::string sessionId;
    std::string error;
    CHECK(matp::parseRescanAudio(matp::makeRescanAudio("s-1"), sessionId, error));
    CHECK_EQ(sessionId, std::string("s-1"));
    CHECK(matp::makeRescanAudio("").find("sessionId") == nullptr);
    CHECK_EQ(static_cast<uint16_t>(matp::MessageType::RescanAudio), static_cast<uint16_t>(26));
}

MULTIROOM_TEST(matp_audio_and_volume_control_round_trip_and_refuse_old_names)
{
    std::string error;
    matp::SessionConfig config;
    config.sessionId = "s-1";
    config.format = format(48000, 2, AudioFormat::SampleType::Int16);
    matp::SessionConfig parsed;
    CHECK(matp::parseSessionConfig(matp::makeSessionConfig(config), parsed, error));
    CHECK(!parsed.hasAudioControl);

    config.hasAudioControl = true;
    config.audioControl = matp::AudioControl::Auto;
    json::Value payload = matp::makeSessionConfig(config);
    std::string text;
    CHECK(json::getString(payload, "audioControl", text));
    CHECK_EQ(text, std::string("auto"));
    CHECK(matp::parseSessionConfig(payload, parsed, error));
    CHECK(parsed.hasAudioControl);
    CHECK(parsed.audioControl == matp::AudioControl::Auto);

    // bitPerfectFallback is the old name for auto: read, never written.
    payload["audioControl"] = json::Value("bitPerfectFallback");
    parsed.audioControl = matp::AudioControl::BitPerfect;
    CHECK(matp::parseSessionConfig(payload, parsed, error));
    CHECK(parsed.audioControl == matp::AudioControl::Auto);

    // The names this setting had before section 59 are not read.
    for (const char* old : {"force", "whenPossible", "no"})
    {
        payload["audioControl"] = json::Value(old);
        CHECK(!matp::parseSessionConfig(payload, parsed, error));
    }

    matp::SetVolumeControl set;
    set.sessionId = "s-1";
    set.volumeControl = matp::VolumeControlMode::Hardware;
    matp::SetVolumeControl setParsed;
    CHECK(matp::parseSetVolumeControl(matp::makeSetVolumeControl(set), setParsed, error));
    CHECK(setParsed.volumeControl == matp::VolumeControlMode::Hardware);

    matp::VolumeConfig report;
    report.sessionId = "s-1";
    report.volumeControl = matp::VolumeControlMode::Software;
    report.effective = matp::VolumeControlMode::Disabled;
    report.reason = "not allowed";
    matp::VolumeConfig reportParsed;
    CHECK(matp::parseVolumeConfig(matp::makeVolumeConfig(report), reportParsed, error));
    CHECK(reportParsed.volumeControl == matp::VolumeControlMode::Software);
    CHECK(reportParsed.effective == matp::VolumeControlMode::Disabled);
    CHECK_EQ(reportParsed.reason, std::string("not allowed"));

    // The effective path is resolved, never auto.
    report.effective = matp::VolumeControlMode::Auto;
    CHECK(!matp::parseVolumeConfig(matp::makeVolumeConfig(report), reportParsed, error));
}

MULTIROOM_TEST(matp_drift_correction_fields_default_to_resampling_and_round_trip)
{
    CHECK_EQ(static_cast<uint16_t>(matp::MessageType::SetDriftCorrection),
             static_cast<uint16_t>(36));
    CHECK_EQ(static_cast<uint16_t>(matp::MessageType::DriftFeedback), static_cast<uint16_t>(37));
    std::string error;

    // A peer that says nothing: resampling, no feedback, no clock.
    matp::CapabilityReport old;
    CHECK(matp::parseAudioCapabilities(matp::makeAudioCapabilities(matp::CapabilityReport()), old,
                                       error));
    CHECK(!old.driftFeedback);
    CHECK(old.driftCorrection == matp::DriftCorrectionMode::Resampling);
    CHECK(!old.hasHardwareClock);

    matp::CapabilityReport report;
    report.driftFeedback = true;
    report.driftCorrection = matp::DriftCorrectionMode::HardwareClock;
    report.hasHardwareClock = true;
    report.hardwareClock.available = true;
    report.hardwareClock.control = "Drift compensator";
    report.hardwareClock.minPpm = -500;
    report.hardwareClock.maxPpm = 500;
    matp::CapabilityReport parsed;
    CHECK(matp::parseAudioCapabilities(matp::makeAudioCapabilities(report), parsed, error));
    CHECK(parsed.driftFeedback);
    CHECK(parsed.driftCorrection == matp::DriftCorrectionMode::HardwareClock);
    CHECK(parsed.hardwareClock.available);
    CHECK_EQ(parsed.hardwareClock.minPpm, -500);
    CHECK_EQ(parsed.hardwareClock.maxPpm, 500);
    CHECK_EQ(parsed.hardwareClock.stepPpm, 1u);

    // A range that excludes nominal, or a mode nobody knows, is refused.
    json::Value payload = matp::makeAudioCapabilities(report);
    payload["hardwareClock"]["minPpm"] = json::Value(static_cast<int64_t>(10));
    CHECK(!matp::parseAudioCapabilities(payload, parsed, error));
    payload = matp::makeAudioCapabilities(report);
    payload["driftCorrection"] = json::Value("pll");
    CHECK(!matp::parseAudioCapabilities(payload, parsed, error));

    matp::SessionConfig config;
    config.sessionId = "s-1";
    config.format.sampleRate = 48000;
    config.format.channels = 2;
    matp::SessionConfig configParsed;
    CHECK(matp::parseSessionConfig(matp::makeSessionConfig(config), configParsed, error));
    CHECK(!configParsed.hasDriftCorrection);
    config.hasDriftCorrection = true;
    config.driftCorrection = matp::DriftCorrectionMode::HardwareClock;
    CHECK(matp::parseSessionConfig(matp::makeSessionConfig(config), configParsed, error));
    CHECK(configParsed.driftCorrection == matp::DriftCorrectionMode::HardwareClock);

    matp::StreamReady ready;
    ready.ready = true;
    ready.format = config.format;
    ready.hasDriftCorrection = true;
    ready.driftCorrection = matp::DriftCorrectionMode::HardwareClock;
    std::string session;
    matp::StreamReady readyParsed;
    CHECK(matp::parseStreamReady(matp::makeStreamReady("s-1", ready), session, readyParsed, error));
    CHECK(readyParsed.hasDriftCorrection);
    CHECK(readyParsed.driftCorrection == matp::DriftCorrectionMode::HardwareClock);

    matp::PlaybackReport playback;
    playback.clockQuality = AudioClockQuality::Driver;
    playback.hasHardwareClock = true;
    playback.hardwareClockPpm = -42;
    playback.hardwareClockSequence = 9;
    playback.hardwareClockSaturated = true;
    playback.hardwareClockError = "i2c";
    matp::PlaybackReport playbackParsed;
    CHECK(matp::parsePlaybackReport(matp::makePlaybackReport("s-1", playback), session,
                                    playbackParsed, error));
    CHECK(playbackParsed.hasHardwareClock);
    CHECK_EQ(playbackParsed.hardwareClockPpm, -42);
    CHECK_EQ(playbackParsed.hardwareClockSequence, static_cast<uint64_t>(9));
    CHECK(playbackParsed.hardwareClockSaturated);
    CHECK_EQ(playbackParsed.hardwareClockError, std::string("i2c"));

    matp::SetDriftCorrection set;
    set.driftCorrection = matp::DriftCorrectionMode::HardwareClock;
    matp::SetDriftCorrection setParsed;
    CHECK(matp::parseSetDriftCorrection(matp::makeSetDriftCorrection(set), setParsed, error));
    CHECK(setParsed.driftCorrection == matp::DriftCorrectionMode::HardwareClock);

    // Auto, the setting that leaves the actuator to the server, round-trips too.
    set.driftCorrection = matp::DriftCorrectionMode::Auto;
    CHECK(matp::parseSetDriftCorrection(matp::makeSetDriftCorrection(set), setParsed, error));
    CHECK(setParsed.driftCorrection == matp::DriftCorrectionMode::Auto);
    report.driftCorrection = matp::DriftCorrectionMode::Auto;
    CHECK(matp::parseAudioCapabilities(matp::makeAudioCapabilities(report), parsed, error));
    CHECK(parsed.driftCorrection == matp::DriftCorrectionMode::Auto);
}

MULTIROOM_TEST(matp_drift_feedback_keeps_unknown_drift_absent_and_refuses_bad_numbers)
{
    std::string error;
    matp::DriftFeedback feedback;
    feedback.sessionId = "s-1";
    feedback.sequence = 4;
    matp::DriftFeedback parsed;
    CHECK(matp::parseDriftFeedback(matp::makeDriftFeedback(feedback), parsed, error));
    CHECK(!parsed.hasDrift);
    CHECK(!parsed.hasHardwareTarget);
    CHECK(matp::makeDriftFeedback(feedback).find("measuredDriftPpm") == nullptr);

    feedback.hasDrift = true;
    feedback.measuredDriftPpm = -12.5;
    feedback.driftConfidence = 0.75;
    feedback.hasHardwareTarget = true;
    feedback.hardwareTargetPpm = 40.25;
    CHECK(matp::parseDriftFeedback(matp::makeDriftFeedback(feedback), parsed, error));
    CHECK(parsed.hasDrift);
    CHECK_EQ(parsed.measuredDriftPpm, -12.5);
    CHECK_EQ(parsed.hardwareTargetPpm, 40.25);
    CHECK_EQ(parsed.sequence, static_cast<uint64_t>(4));

    json::Value payload = matp::makeDriftFeedback(feedback);
    payload["sequence"] = json::Value(static_cast<int64_t>(0));
    CHECK(!matp::parseDriftFeedback(payload, parsed, error));
    payload = matp::makeDriftFeedback(feedback);
    payload["driftConfidence"] = json::Value(1.5);
    CHECK(!matp::parseDriftFeedback(payload, parsed, error));
    payload = matp::makeDriftFeedback(feedback);
    payload["hardwareTargetPpm"] = json::Value(1e9);
    CHECK(!matp::parseDriftFeedback(payload, parsed, error));
}

MULTIROOM_TEST(matp_set_room_name_and_room_name_round_trip_and_refuse_a_bad_name)
{
    CHECK_EQ(static_cast<uint16_t>(matp::MessageType::SetRoomName), static_cast<uint16_t>(34));
    CHECK_EQ(static_cast<uint16_t>(matp::MessageType::RoomName), static_cast<uint16_t>(35));
    matp::MessageType type = matp::MessageType::Unknown;
    CHECK(matp::parseMessageType("setRoomName", type));
    CHECK(type == matp::MessageType::SetRoomName);
    CHECK(matp::parseMessageType("roomName", type));
    CHECK(type == matp::MessageType::RoomName);

    matp::SetRoomName request;
    request.sessionId = "s-1";
    request.name = "K\xC3\xBC" "che";
    matp::SetRoomName parsed;
    std::string error;
    CHECK(matp::parseSetRoomName(matp::makeSetRoomName(request), parsed, error));
    CHECK_EQ(parsed.sessionId, std::string("s-1"));
    CHECK_EQ(parsed.name, request.name);

    // Never trimmed or shortened: refused.
    json::Value bad = matp::makeSetRoomName(request);
    for (const json::Value& value : {json::Value(""), json::Value(" Kitchen"),
                                     json::Value("Kit\tchen"), json::Value(std::string(65, 'a')),
                                     json::Value(5)})
    {
        bad["name"] = value;
        CHECK(!matp::parseSetRoomName(bad, parsed, error));
    }

    std::string sessionId;
    std::string name;
    CHECK(matp::parseRoomName(matp::makeRoomName("s-2", "Arbeitsraum"), sessionId, name, error));
    CHECK_EQ(sessionId, std::string("s-2"));
    CHECK_EQ(name, std::string("Arbeitsraum"));
    json::Value anonymous = matp::makeRoomName("s-2", "Arbeitsraum");
    anonymous["sessionId"] = json::Value("");
    CHECK(!matp::parseRoomName(anonymous, sessionId, name, error));
}
