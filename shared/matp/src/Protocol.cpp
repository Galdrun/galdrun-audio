#include "matp/Protocol.h"

#include <cmath>

namespace multiroom {
namespace matp {
namespace {

void appendUint16(std::string& out, uint16_t value)
{
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

void appendUint32(std::string& out, uint32_t value)
{
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

uint16_t readUint16(const char* bytes)
{
    return static_cast<uint16_t>((static_cast<unsigned char>(bytes[0]) << 8) |
                                 static_cast<unsigned char>(bytes[1]));
}

uint32_t readUint32(const char* bytes)
{
    return (static_cast<uint32_t>(static_cast<unsigned char>(bytes[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes[2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(bytes[3]));
}

json::Value formatToJson(const AudioFormat& format)
{
    json::Value object = json::Value::makeObject();
    object["sampleRate"] = json::Value(format.sampleRate);
    object["channels"] = json::Value(static_cast<uint32_t>(format.channels));
    object["sampleFormat"] = json::Value(sampleFormatName(format.sampleType));
    return object;
}

// Rates above this are not a format the protocol can mean; anything that large
// is a parse accident or a hostile value.
const int64_t kMaxSampleRate = 768000;
const int64_t kMaxChannels = 64;

bool formatFromJson(const json::Value& object, AudioFormat& out, std::string& error)
{
    int64_t sampleRate = 0;
    if (!json::getInt(object, "sampleRate", sampleRate) || sampleRate <= 0 ||
        sampleRate > kMaxSampleRate)
    {
        error = "format.sampleRate must be a positive sample rate";
        return false;
    }

    int64_t channels = 0;
    if (!json::getInt(object, "channels", channels) || channels <= 0 || channels > kMaxChannels)
    {
        error = "format.channels must be between 1 and 64";
        return false;
    }

    std::string name;
    if (!json::getString(object, "sampleFormat", name))
    {
        error = "format.sampleFormat must be a string";
        return false;
    }

    AudioFormat::SampleType sampleType = AudioFormat::SampleType::Int16;
    if (!parseSampleFormat(name, sampleType))
    {
        // Not a format this client could play even if it tried: the layout
        // decides how the bytes are read, so guessing here produces noise.
        error = "unknown sampleFormat \"" + name + "\"";
        return false;
    }

    out.sampleRate = static_cast<uint32_t>(sampleRate);
    out.channels = static_cast<uint16_t>(channels);
    out.sampleType = sampleType;
    return true;
}

// A nanosecond timestamp, and strictly an integral one. The JSON layer keeps
// integers exactly and only integers: a value written with a fraction or an
// exponent has already been through a double, and a double stops resolving
// nanoseconds long before a monotonic clock stops producing them.
bool timestampFromJson(const json::Value& object, const char* key, uint64_t& out,
                       std::string& error)
{
    const json::Value* value = object.find(key);
    if (value == nullptr || !value->isNumber() || !value->isIntegral() || value->asInt() < 0)
    {
        error = std::string(key) + " must be a whole number of nanoseconds";
        return false;
    }
    out = static_cast<uint64_t>(value->asInt());
    return true;
}

// The session a message names, which PREPARE_STREAM and START_AT both leave
// out when the server does not care to repeat it. Absent is not an error;
// present and empty is, because that is a server that meant to say something.
bool sessionIdFromJson(const json::Value& object, std::string& out, std::string& error)
{
    const json::Value* value = object.find("sessionId");
    if (value == nullptr)
        return true;
    if (!value->isString() || value->asString().empty())
    {
        error = "sessionId must be a non-empty string";
        return false;
    }
    out = value->asString();
    return true;
}

// A string the server may leave out. Absent keeps whatever the caller had;
// present and of the wrong type is an error, because that is a server that
// meant to say something -- a title that arrived as a number is not a title.
bool optionalStringFromJson(const json::Value& object, const char* key, std::string& out,
                            std::string& error)
{
    const json::Value* value = object.find(key);
    if (value == nullptr)
        return true;
    if (!value->isString())
    {
        error = std::string(key) + " must be a string";
        return false;
    }
    out = value->asString();
    return true;
}

// The session a message must name, which is every message the client sends:
// the server has one stream per client and cannot attribute an answer without
// it. The optional form above is for the other direction.
bool requiredSessionIdFromJson(const json::Value& object, std::string& out, std::string& error)
{
    std::string sessionId;
    if (!json::getString(object, "sessionId", sessionId) || sessionId.empty())
    {
        error = "sessionId must be a non-empty string";
        return false;
    }
    out = sessionId;
    return true;
}

// A position on the zone's media timeline. Whole nanoseconds, and never
// negative: 0 is where the timeline begins (section 5).
//
// Not the same function as timestampFromJson above, although the two read the
// same JSON: that one is an instant on a monotonic clock and this one is a
// position in the media. Keeping them apart is the one place the difference is
// visible to the compiler at all.
bool timelineNsFromJson(const json::Value& object, const char* key, uint64_t& out,
                        std::string& error)
{
    const json::Value* value = object.find(key);
    if (value == nullptr || !value->isNumber() || !value->isIntegral() || value->asInt() < 0)
    {
        error = std::string(key) + " must be a whole number of nanoseconds";
        return false;
    }
    out = static_cast<uint64_t>(value->asInt());
    return true;
}

} // namespace

const char* messageTypeName(MessageType type)
{
    switch (type)
    {
    case MessageType::Unknown:
        return "unknown";
    case MessageType::Hello:
        return "hello";
    case MessageType::HelloAck:
        return "helloAck";
    case MessageType::AudioCapabilities:
        return "audioCapabilities";
    case MessageType::SessionConfig:
        return "sessionConfig";
    case MessageType::Error:
        return "error";
    case MessageType::ClockSyncRequest:
        return "clockSyncRequest";
    case MessageType::ClockSyncReply:
        return "clockSyncReply";
    case MessageType::PrepareStream:
        return "prepareStream";
    case MessageType::StreamReady:
        return "streamReady";
    case MessageType::BufferStatus:
        return "bufferStatus";
    case MessageType::StreamAttach:
        return "streamAttach";
    case MessageType::StartAt:
        return "startAt";
    case MessageType::PlaybackReport:
        return "playbackReport";
    case MessageType::TrackBoundary:
        return "trackBoundary";
    case MessageType::PauseAt:
        return "pauseAt";
    case MessageType::StopAt:
        return "stopAt";
    case MessageType::SetVolume:
        return "setVolume";
    case MessageType::TrackInfo:
        return "trackInfo";
    case MessageType::Ping:
        return "ping";
    case MessageType::Pong:
        return "pong";
    case MessageType::ClockSyncResult:
        return "clockSyncResult";
    case MessageType::Flush:
        return "flush";
    case MessageType::SystemReport:
        return "systemReport";
    case MessageType::AudioDevices:
        return "audioDevices";
    case MessageType::SetAudioDevice:
        return "setAudioDevice";
    case MessageType::RescanAudio:
        return "rescanAudio";
    case MessageType::InputControls:
        return "inputControls";
    case MessageType::InputAction:
        return "inputAction";
    case MessageType::SetInputBindings:
        return "setInputBindings";
    case MessageType::SetBuffer:
        return "setBuffer";
    case MessageType::BufferConfig:
        return "bufferConfig";
    case MessageType::SetVolumeControl:
        return "setVolumeControl";
    case MessageType::VolumeConfig:
        return "volumeConfig";
    case MessageType::SetRoomName:
        return "setRoomName";
    case MessageType::RoomName:
        return "roomName";
    case MessageType::SetDriftCorrection:
        return "setDriftCorrection";
    case MessageType::DriftFeedback:
        return "driftFeedback";
    }
    return "unknown";
}

const char* audioControlName(AudioControl control)
{
    switch (control)
    {
    case AudioControl::BitPerfect:
        return "bitPerfect";
    case AudioControl::Auto:
        return "auto";
    }
    return "bitPerfect";
}

bool parseAudioControl(const std::string& name, AudioControl& out)
{
    // The old name for auto, from configurations and peers that predate the
    // merge.
    if (name == "bitPerfectFallback")
    {
        out = AudioControl::Auto;
        return true;
    }
    for (const AudioControl control : {AudioControl::BitPerfect, AudioControl::Auto})
    {
        if (name == audioControlName(control))
        {
            out = control;
            return true;
        }
    }
    return false;
}

const char* volumeControlModeName(VolumeControlMode mode)
{
    switch (mode)
    {
    case VolumeControlMode::Auto:
        return "auto";
    case VolumeControlMode::Hardware:
        return "hardware";
    case VolumeControlMode::Software:
        return "software";
    case VolumeControlMode::Disabled:
        return "disabled";
    }
    return "auto";
}

bool parseVolumeControlMode(const std::string& name, VolumeControlMode& out)
{
    for (const VolumeControlMode mode : {VolumeControlMode::Auto, VolumeControlMode::Hardware,
                                         VolumeControlMode::Software, VolumeControlMode::Disabled})
    {
        if (name == volumeControlModeName(mode))
        {
            out = mode;
            return true;
        }
    }
    return false;
}

const char* driftCorrectionModeName(DriftCorrectionMode mode)
{
    switch (mode)
    {
    case DriftCorrectionMode::Resampling:
        return "resampling";
    case DriftCorrectionMode::HardwareClock:
        return "hardwareClock";
    case DriftCorrectionMode::Auto:
        return "auto";
    }
    return "resampling";
}

bool parseDriftCorrectionMode(const std::string& name, DriftCorrectionMode& out)
{
    for (const DriftCorrectionMode mode :
         {DriftCorrectionMode::Resampling, DriftCorrectionMode::HardwareClock, DriftCorrectionMode::Auto})
    {
        if (name == driftCorrectionModeName(mode))
        {
            out = mode;
            return true;
        }
    }
    return false;
}

bool parseMessageType(const std::string& name, MessageType& out)
{
    const MessageType known[] = {MessageType::Hello,
                                 MessageType::HelloAck,
                                 MessageType::AudioCapabilities,
                                 MessageType::SessionConfig,
                                 MessageType::Error,
                                 MessageType::ClockSyncRequest,
                                 MessageType::ClockSyncReply,
                                 MessageType::PrepareStream,
                                 MessageType::StreamReady,
                                 MessageType::BufferStatus,
                                 MessageType::StreamAttach,
                                 MessageType::StartAt,
                                 MessageType::PlaybackReport,
                                 MessageType::TrackBoundary,
                                 MessageType::PauseAt,
                                 MessageType::StopAt,
                                 MessageType::SetVolume,
                                 MessageType::TrackInfo,
                                 MessageType::Ping,
                                 MessageType::Pong,
                                 MessageType::ClockSyncResult,
                                 MessageType::Flush,
                                 MessageType::SystemReport,
                                 MessageType::AudioDevices,
                                 MessageType::SetAudioDevice,
                                 MessageType::RescanAudio,
                                 MessageType::InputControls,
                                 MessageType::InputAction,
                                 MessageType::SetInputBindings,
                                 MessageType::SetBuffer,
                                 MessageType::BufferConfig,
                                 MessageType::SetVolumeControl,
                                 MessageType::VolumeConfig,
                                 MessageType::SetRoomName,
                                 MessageType::RoomName,
                                 MessageType::SetDriftCorrection,
                                 MessageType::DriftFeedback};
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i)
    {
        if (name == messageTypeName(known[i]))
        {
            out = known[i];
            return true;
        }
    }
    return false;
}

std::string encode(MessageType type, const json::Value& payload)
{
    const std::string body = json::serialize(payload);

    std::string frame;
    frame.reserve(kHeaderBytes + body.size());
    // The length of the message, the message being the type plus the payload.
    appendUint32(frame, static_cast<uint32_t>(sizeof(uint16_t) + body.size()));
    appendUint16(frame, static_cast<uint16_t>(type));
    frame += body;
    return frame;
}

void FrameReader::append(const void* data, size_t size)
{
    if (data == nullptr || size == 0)
        return;
    buffer_.append(static_cast<const char*>(data), size);
}

void FrameReader::reset()
{
    buffer_.clear();
}

FrameReader::Result FrameReader::next(Message& out, std::string& error)
{
    if (buffer_.size() < kHeaderBytes)
        return Result::Incomplete;

    const uint32_t length = readUint32(buffer_.data());
    if (length < sizeof(uint16_t))
    {
        error = "message length " + std::to_string(length) + " is shorter than its type field";
        return Result::Invalid;
    }
    if (length > kMaxMessageBytes)
    {
        error = "message length " + std::to_string(length) + " exceeds the " +
                std::to_string(kMaxMessageBytes) + " byte limit";
        return Result::Invalid;
    }

    const size_t total = sizeof(uint32_t) + length;
    if (buffer_.size() < total)
        return Result::Incomplete;

    const uint16_t wireType = readUint16(buffer_.data() + sizeof(uint32_t));
    const std::string body = buffer_.substr(kHeaderBytes, total - kHeaderBytes);
    buffer_.erase(0, total);

    json::Value payload;
    std::string parseError;
    if (!json::parse(body, payload, parseError))
    {
        error = "invalid JSON: " + parseError;
        return Result::Invalid;
    }
    if (!payload.isObject())
    {
        error = "message payload is not an object";
        return Result::Invalid;
    }

    std::string typeName;
    if (!json::getString(payload, "type", typeName) || typeName.empty())
    {
        error = "message has no type";
        return Result::Invalid;
    }

    MessageType type = MessageType::Unknown;
    // The name decides, because it is the protocol's and the number is this
    // client's (see the header). A number it does know must still agree: a
    // disagreement is a desync or a server numbering things differently, and
    // both are worth saying out loud rather than working around.
    if (parseMessageType(typeName, type) && wireType != static_cast<uint16_t>(type))
    {
        error = typeName + " arrived as message type " + std::to_string(wireType) +
                ", this client sends and expects " +
                std::to_string(static_cast<uint16_t>(type));
        return Result::Invalid;
    }

    out.type = type;
    out.typeName = typeName;
    out.wireType = wireType;
    out.payload = std::move(payload);
    return Result::Message;
}

const char* sampleFormatName(AudioFormat::SampleType type)
{
    switch (type)
    {
    case AudioFormat::SampleType::Int16:
        return "s16le";
    case AudioFormat::SampleType::Int24:
        return "s24le3";
    case AudioFormat::SampleType::Int24In32:
        return "s24le";
    case AudioFormat::SampleType::Int32:
        return "s32le";
    case AudioFormat::SampleType::Float32:
        return "f32le";
    }
    return "s16le";
}

bool parseSampleFormat(const std::string& name, AudioFormat::SampleType& out)
{
    if (name == "s16le")
        out = AudioFormat::SampleType::Int16;
    else if (name == "s24le3")
        out = AudioFormat::SampleType::Int24;
    else if (name == "s24le")
        out = AudioFormat::SampleType::Int24In32;
    else if (name == "s32le")
        out = AudioFormat::SampleType::Int32;
    else if (name == "f32le")
        out = AudioFormat::SampleType::Float32;
    else
        return false;
    return true;
}

json::Value makeHello(const Identity& identity)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::Hello));
    root["protocolVersion"] = json::Value(kProtocolVersion);
    root["clientId"] = json::Value(identity.clientId);
    root["clientVersion"] = json::Value(identity.clientVersion);
    root["platform"] = json::Value(identity.platform);
    return root;
}

bool parseHello(const json::Value& payload, Hello& out, std::string& error)
{
    const json::Value* version = payload.find("protocolVersion");
    if (version == nullptr || !version->isNumber() || !version->isIntegral() ||
        version->asInt() < 0 || version->asInt() > 0xFFFF)
    {
        // Not defaulted to kProtocolVersion: a peer that did not say which
        // version it speaks is exactly the peer section 51 refuses.
        error = "protocolVersion must be a protocol version number";
        return false;
    }

    Hello hello;
    hello.protocolVersion = static_cast<uint32_t>(version->asInt());

    if (!json::getString(payload, "clientId", hello.identity.clientId) ||
        hello.identity.clientId.empty())
    {
        error = "clientId must be a non-empty string";
        return false;
    }

    // Informational. A room with no version and no platform is still a room.
    if (!optionalStringFromJson(payload, "clientVersion", hello.identity.clientVersion, error))
        return false;
    if (!optionalStringFromJson(payload, "platform", hello.identity.platform, error))
        return false;

    out = hello;
    return true;
}

namespace {

bool hardwareVolumeFromJson(const json::Value& object, HardwareVolumeCapability& out,
                            std::string& error)
{
    HardwareVolumeCapability volume;
    const json::Value* available = object.find("available");
    if (!object.isObject() || available == nullptr || !available->isBool())
    {
        error = "hardwareVolume must be an object with a boolean available";
        return false;
    }
    volume.available = available->asBool(false);
    if (!volume.available)
    {
        out = volume;
        return true;
    }

    if (!optionalStringFromJson(object, "control", volume.control, error))
        return false;

    const json::Value* steps = object.find("steps");
    if (steps == nullptr || !steps->isIntegral() || steps->asInt() < 2 ||
        steps->asInt() > 0xFFFFFFFFLL)
    {
        error = "hardwareVolume.steps must be a whole number of at least 2";
        return false;
    }
    volume.steps = static_cast<uint32_t>(steps->asInt());

    const json::Value* minDb = object.find("minDb");
    const json::Value* maxDb = object.find("maxDb");
    if (minDb != nullptr || maxDb != nullptr)
    {
        if (minDb == nullptr || maxDb == nullptr || !minDb->isNumber() || !maxDb->isNumber() ||
            !std::isfinite(minDb->asDouble()) || !std::isfinite(maxDb->asDouble()) ||
            !(maxDb->asDouble() > minDb->asDouble()))
        {
            error = "hardwareVolume needs both minDb and maxDb, with maxDb above minDb";
            return false;
        }
        volume.hasDecibels = true;
        volume.minDb = minDb->asDouble();
        volume.maxDb = maxDb->asDouble();
    }

    const json::Value* hasMute = object.find("hasMute");
    if (hasMute != nullptr)
    {
        if (!hasMute->isBool())
        {
            error = "hardwareVolume.hasMute must be a boolean";
            return false;
        }
        volume.hasMute = hasMute->asBool(false);
    }

    out = volume;
    return true;
}

// A trim far beyond any oscillator's error is a parse accident, not a range.
const int64_t kMaxClockTrimPpm = 10000;

bool hardwareClockFromJson(const json::Value& object, HardwareClockCapability& out,
                           std::string& error)
{
    HardwareClockCapability clock;
    const json::Value* available = object.find("available");
    if (!object.isObject() || available == nullptr || !available->isBool())
    {
        error = "hardwareClock must be an object with a boolean available";
        return false;
    }
    clock.available = available->asBool(false);
    if (!clock.available)
    {
        out = clock;
        return true;
    }
    if (!optionalStringFromJson(object, "control", clock.control, error))
        return false;

    const json::Value* minPpm = object.find("minPpm");
    const json::Value* maxPpm = object.find("maxPpm");
    const json::Value* step = object.find("stepPpm");
    if (minPpm == nullptr || maxPpm == nullptr || step == nullptr || !minPpm->isIntegral() ||
        !maxPpm->isIntegral() || !step->isIntegral() || minPpm->asInt() > 0 ||
        maxPpm->asInt() < 0 || minPpm->asInt() >= maxPpm->asInt() ||
        minPpm->asInt() < -kMaxClockTrimPpm || maxPpm->asInt() > kMaxClockTrimPpm ||
        step->asInt() <= 0 || step->asInt() > maxPpm->asInt() - minPpm->asInt())
    {
        error = "hardwareClock needs whole minPpm <= 0 <= maxPpm and a positive stepPpm";
        return false;
    }
    clock.minPpm = static_cast<int32_t>(minPpm->asInt());
    clock.maxPpm = static_cast<int32_t>(maxPpm->asInt());
    clock.stepPpm = static_cast<uint32_t>(step->asInt());
    out = clock;
    return true;
}

bool driftCorrectionModeFromJson(const json::Value& payload, const char* key,
                                 DriftCorrectionMode& out, std::string& error)
{
    const json::Value* value = payload.find(key);
    if (value == nullptr || !value->isString() ||
        !parseDriftCorrectionMode(value->asString(), out))
    {
        error = std::string(key) + " must be resampling, hardwareClock or auto";
        return false;
    }
    return true;
}

} // namespace

json::Value makeHardwareVolume(const HardwareVolumeCapability& volume)
{
    json::Value object = json::Value::makeObject();
    object["available"] = json::Value(volume.available);
    if (!volume.available)
        return object;
    if (!volume.control.empty())
        object["control"] = json::Value(volume.control);
    object["steps"] = json::Value(volume.steps);
    if (volume.hasDecibels)
    {
        object["minDb"] = json::Value(volume.minDb);
        object["maxDb"] = json::Value(volume.maxDb);
    }
    object["hasMute"] = json::Value(volume.hasMute);
    return object;
}

json::Value makeAudioCapabilities(const CapabilityReport& report)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::AudioCapabilities));
    root["deviceId"] = json::Value(report.deviceId);
    root["deviceName"] = json::Value(report.deviceName);
    root["preferredSampleRate"] = json::Value(report.preferredSampleRate);

    json::Value formats = json::Value::makeArray();
    for (size_t i = 0; i < report.formats.size(); ++i)
        formats.pushBack(formatToJson(report.formats[i]));
    root["formats"] = formats;
    if (!report.accessMode.empty())
        root["accessMode"] = json::Value(report.accessMode);
    if (report.hasHardwareVolume)
        root["hardwareVolume"] = makeHardwareVolume(report.hardwareVolume);
    if (report.driftFeedback)
    {
        root["driftFeedback"] = json::Value(true);
        root["driftCorrection"] = json::Value(driftCorrectionModeName(report.driftCorrection));
    }
    if (report.hasHardwareClock)
    {
        json::Value clock = json::Value::makeObject();
        clock["available"] = json::Value(report.hardwareClock.available);
        if (report.hardwareClock.available)
        {
            if (!report.hardwareClock.control.empty())
                clock["control"] = json::Value(report.hardwareClock.control);
            clock["minPpm"] = json::Value(report.hardwareClock.minPpm);
            clock["maxPpm"] = json::Value(report.hardwareClock.maxPpm);
            clock["stepPpm"] = json::Value(report.hardwareClock.stepPpm);
        }
        root["hardwareClock"] = clock;
    }
    return root;
}

bool parseAudioCapabilities(const json::Value& payload, CapabilityReport& out,
                            std::string& error)
{
    CapabilityReport report;
    if (!optionalStringFromJson(payload, "deviceId", report.deviceId, error))
        return false;
    if (!optionalStringFromJson(payload, "deviceName", report.deviceName, error))
        return false;

    const json::Value* preferred = payload.find("preferredSampleRate");
    if (preferred != nullptr)
    {
        // Zero is allowed and means "no configured preference". The client
        // writes this field unconditionally -- it is a hint from its
        // configuration, and an unset hint is what zero says here -- so
        // refusing it would refuse every client that has not set one.
        if (!preferred->isNumber() || !preferred->isIntegral() || preferred->asInt() < 0 ||
            preferred->asInt() > kMaxSampleRate)
        {
            error = "preferredSampleRate must be a sample rate, or 0 for no preference";
            return false;
        }
        report.preferredSampleRate = static_cast<uint32_t>(preferred->asInt());
    }

    const json::Value* formats = payload.find("formats");
    if (formats == nullptr || !formats->isArray())
    {
        error = "formats must be an array";
        return false;
    }
    if (formats->size() > kMaxCapabilityFormats)
    {
        error = "formats lists " + std::to_string(formats->size()) + " entries, more than the " +
                std::to_string(kMaxCapabilityFormats) + " a device scan can produce";
        return false;
    }

    report.formats.reserve(formats->size());
    for (size_t i = 0; i < formats->size(); ++i)
    {
        const json::Value& entry = formats->at(i);
        if (!entry.isObject())
        {
            error = "formats[" + std::to_string(i) + "] is not an object";
            return false;
        }
        AudioFormat format;
        if (!formatFromJson(entry, format, error))
        {
            error = "formats[" + std::to_string(i) + "]: " + error;
            return false;
        }
        report.formats.push_back(format);
    }

    if (!optionalStringFromJson(payload, "accessMode", report.accessMode, error))
        return false;
    if (!report.accessMode.empty() && report.accessMode != "exclusive" &&
        report.accessMode != "shared")
    {
        error = "accessMode must be \"exclusive\" or \"shared\"";
        return false;
    }

    const json::Value* hardwareVolume = payload.find("hardwareVolume");
    if (hardwareVolume != nullptr)
    {
        if (!hardwareVolumeFromJson(*hardwareVolume, report.hardwareVolume, error))
            return false;
        report.hasHardwareVolume = true;
    }

    if (payload.find("driftFeedback") != nullptr &&
        !json::getBool(payload, "driftFeedback", report.driftFeedback))
    {
        error = "driftFeedback must be a boolean";
        return false;
    }
    if (payload.find("driftCorrection") != nullptr &&
        !driftCorrectionModeFromJson(payload, "driftCorrection", report.driftCorrection, error))
        return false;
    if (const json::Value* clock = payload.find("hardwareClock"); clock != nullptr)
    {
        if (!hardwareClockFromJson(*clock, report.hardwareClock, error))
            return false;
        report.hasHardwareClock = true;
    }

    out = report;
    return true;
}

bool parseHelloAck(const json::Value& payload, HelloAck& out, std::string& error)
{
    const json::Value* version = payload.find("protocolVersion");
    if (version == nullptr)
        return true; // the server did not say, which is not the same as zero

    if (!version->isIntegral() || version->asInt() < 0 || version->asInt() > 0xFFFF)
    {
        error = "protocolVersion must be a protocol version number";
        return false;
    }

    out.hasProtocolVersion = true;
    out.protocolVersion = static_cast<uint32_t>(version->asInt());
    return true;
}

json::Value makeHelloAck(uint32_t protocolVersion)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::HelloAck));
    root["protocolVersion"] = json::Value(protocolVersion);
    return root;
}

bool parseSessionConfig(const json::Value& payload, SessionConfig& out, std::string& error)
{
    std::string sessionId;
    if (!json::getString(payload, "sessionId", sessionId) || sessionId.empty())
    {
        // Audio packets carry the session id (section 13). Without one there
        // is no way to tell this session's audio from a previous session's.
        error = "sessionId must be a non-empty string";
        return false;
    }

    SessionConfig config;
    config.sessionId = sessionId;

    // Absent when the client reported no format: a session without a stream.
    const json::Value* format = payload.find("format");
    config.hasFormat = format != nullptr;
    if (config.hasFormat)
    {
        if (!format->isObject())
        {
            error = "format must be an object";
            return false;
        }
        if (!formatFromJson(*format, config.format, error))
            return false;
    }
    if (const json::Value* control = payload.find("audioControl"); control != nullptr)
    {
        if (!control->isString() || !parseAudioControl(control->asString(), config.audioControl))
        {
            error = "audioControl must be bitPerfect or auto";
            return false;
        }
        config.hasAudioControl = true;
    }
    if (payload.find("driftCorrection") != nullptr)
    {
        if (!driftCorrectionModeFromJson(payload, "driftCorrection", config.driftCorrection,
                                         error))
            return false;
        config.hasDriftCorrection = true;
    }

    out = config;
    return true;
}

json::Value makeSessionConfig(const SessionConfig& config)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::SessionConfig));
    root["sessionId"] = json::Value(config.sessionId);
    if (config.hasFormat)
        root["format"] = formatToJson(config.format);
    if (config.hasAudioControl)
        root["audioControl"] = json::Value(audioControlName(config.audioControl));
    if (config.hasDriftCorrection)
        root["driftCorrection"] = json::Value(driftCorrectionModeName(config.driftCorrection));
    return root;
}

json::Value makeClockSyncRequest(uint64_t clientSendNs)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::ClockSyncRequest));
    root["clientSendNs"] = json::Value(clientSendNs);
    return root;
}

bool parseClockSyncRequest(const json::Value& payload, uint64_t& clientSendNs,
                           std::string& error)
{
    return timestampFromJson(payload, "clientSendNs", clientSendNs, error);
}

bool parseClockSyncReply(const json::Value& payload, ClockSyncReply& out, std::string& error)
{
    ClockSyncReply reply;
    if (!timestampFromJson(payload, "clientSendNs", reply.clientSendNs, error))
        return false;
    if (!timestampFromJson(payload, "serverReceiveNs", reply.serverReceiveNs, error))
        return false;
    if (!timestampFromJson(payload, "serverSendNs", reply.serverSendNs, error))
        return false;

    if (reply.serverSendNs < reply.serverReceiveNs)
    {
        // The server cannot have answered before it was asked. Checked here
        // rather than only in the model, because this one is the server
        // contradicting itself rather than a noisy measurement.
        error = "serverSendNs is before serverReceiveNs";
        return false;
    }

    out = reply;
    return true;
}

json::Value makeClockSyncReply(const ClockSyncReply& reply)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::ClockSyncReply));
    // C1 first, echoed: it is the correlator, and a reply that arrived without
    // it would be a measurement the client could not place (section 8).
    root["clientSendNs"] = json::Value(reply.clientSendNs);
    root["serverReceiveNs"] = json::Value(reply.serverReceiveNs);
    root["serverSendNs"] = json::Value(reply.serverSendNs);
    return root;
}

json::Value makeClockSyncResult(uint64_t clientSendNs, uint64_t serverReceiveNs,
                                uint64_t serverSendNs, uint64_t clientReceiveNs)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::ClockSyncResult));
    // All four, in the order section 8 names them. The server already has the
    // middle two; they are echoed so that one message is one complete sample
    // rather than a half a sample the server has to pair up with a request it
    // may have answered seconds ago.
    root["clientSendNs"] = json::Value(clientSendNs);
    root["serverReceiveNs"] = json::Value(serverReceiveNs);
    root["serverSendNs"] = json::Value(serverSendNs);
    root["clientReceiveNs"] = json::Value(clientReceiveNs);
    return root;
}

bool parseClockSyncResult(const json::Value& payload, ClockSyncResult& out, std::string& error)
{
    ClockSyncResult result;
    if (!timestampFromJson(payload, "clientSendNs", result.clientSendNs, error))
        return false;
    if (!timestampFromJson(payload, "serverReceiveNs", result.serverReceiveNs, error))
        return false;
    if (!timestampFromJson(payload, "serverSendNs", result.serverSendNs, error))
        return false;
    if (!timestampFromJson(payload, "clientReceiveNs", result.clientReceiveNs, error))
        return false;

    // Each clock against itself only. A monotonic clock cannot run backwards,
    // so either of these is the sample contradicting itself rather than a
    // round trip that happened to be noisy -- and the server rejects outliers
    // by round trip, which it cannot even compute from a negative one.
    if (result.serverSendNs < result.serverReceiveNs)
    {
        error = "serverSendNs is before serverReceiveNs";
        return false;
    }
    if (result.clientReceiveNs < result.clientSendNs)
    {
        error = "clientReceiveNs is before clientSendNs";
        return false;
    }

    out = result;
    return true;
}

bool parsePrepareStream(const json::Value& payload, PrepareStream& out, std::string& error)
{
    PrepareStream request;
    if (!sessionIdFromJson(payload, request.sessionId, error))
        return false;

    const json::Value* port = payload.find("audioPort");
    if (port != nullptr)
    {
        if (!port->isNumber() || !port->isIntegral() || port->asInt() < 1 ||
            port->asInt() > 65535)
        {
            error = "audioPort must be a port number";
            return false;
        }
        request.hasAudioPort = true;
        request.audioPort = static_cast<uint16_t>(port->asInt());
    }

    out = request;
    return true;
}

json::Value makePrepareStream(const PrepareStream& request)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::PrepareStream));
    if (!request.sessionId.empty())
        root["sessionId"] = json::Value(request.sessionId);
    if (request.hasAudioPort)
        root["audioPort"] = json::Value(static_cast<uint32_t>(request.audioPort));
    return root;
}

bool parseStartAt(const json::Value& payload, StartAt& out, std::string& error)
{
    StartAt request;
    if (!sessionIdFromJson(payload, request.sessionId, error))
        return false;
    if (!timestampFromJson(payload, "serverTimeNs", request.serverTimeNs, error))
        return false;
    if (!timelineNsFromJson(payload, "timelineNs", request.timelineNs, error))
        return false;

    out = request;
    return true;
}

json::Value makeStartAt(const StartAt& start)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::StartAt));
    if (!start.sessionId.empty())
        root["sessionId"] = json::Value(start.sessionId);
    // Both, always: neither has a defensible default, and a room that started
    // at the wrong moment is worse than one that did not start (section 20).
    root["serverTimeNs"] = json::Value(start.serverTimeNs);
    root["timelineNs"] = json::Value(start.timelineNs);
    return root;
}

bool parseTrackBoundary(const json::Value& payload, TrackBoundary& out, std::string& error)
{
    TrackBoundary boundary;
    if (!sessionIdFromJson(payload, boundary.sessionId, error))
        return false;
    if (!timelineNsFromJson(payload, "timelineNs", boundary.timelineNs, error))
        return false;

    // Both are optional: the position is the part every room has to agree on,
    // and a boundary that names neither is still a boundary.
    if (!optionalStringFromJson(payload, "trackId", boundary.trackId, error))
        return false;
    if (!optionalStringFromJson(payload, "title", boundary.title, error))
        return false;

    out = boundary;
    return true;
}

json::Value makeTrackBoundary(const TrackBoundary& boundary)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::TrackBoundary));
    if (!boundary.sessionId.empty())
        root["sessionId"] = json::Value(boundary.sessionId);
    root["timelineNs"] = json::Value(boundary.timelineNs);
    // Omitted rather than sent empty: an empty title is not the same statement
    // as not naming one, and the position is the part every room agrees on.
    if (!boundary.trackId.empty())
        root["trackId"] = json::Value(boundary.trackId);
    if (!boundary.title.empty())
        root["title"] = json::Value(boundary.title);
    return root;
}

bool parseTrackInfo(const json::Value& payload, TrackInfo& out, std::string& error)
{
    TrackInfo info;
    if (!sessionIdFromJson(payload, info.sessionId, error))
        return false;
    if (!optionalStringFromJson(payload, "trackId", info.trackId, error))
        return false;
    if (!optionalStringFromJson(payload, "title", info.title, error))
        return false;

    if (info.trackId.empty() && info.title.empty())
    {
        // Unlike a boundary, this message is nothing but the two of them.
        error = "trackInfo names neither a trackId nor a title";
        return false;
    }

    out = info;
    return true;
}

bool makeTrackInfo(const TrackInfo& info, json::Value& out, std::string& error)
{
    if (info.trackId.empty() && info.title.empty())
    {
        error = "trackInfo names neither a trackId nor a title";
        return false;
    }

    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::TrackInfo));
    if (!info.sessionId.empty())
        root["sessionId"] = json::Value(info.sessionId);
    if (!info.trackId.empty())
        root["trackId"] = json::Value(info.trackId);
    if (!info.title.empty())
        root["title"] = json::Value(info.title);

    out = root;
    return true;
}

json::Value makePong(const json::Value& ping)
{
    // The ping's own body, type replaced. A server that correlates its pings
    // on a field this client has never heard of gets that field back.
    json::Value root = ping.isObject() ? ping : json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::Pong));
    return root;
}

json::Value makePing(uint64_t sequence)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::Ping));
    root["sequence"] = json::Value(sequence);
    return root;
}

bool parsePong(const json::Value& payload, uint64_t& sequence, std::string& error)
{
    const json::Value* value = payload.find("sequence");
    if (value == nullptr || !value->isNumber() || !value->isIntegral() || value->asInt() < 0)
    {
        // The client echoes the ping's body, so this can only be missing from
        // a peer that answered without reading -- which is not liveness.
        error = "sequence must be the whole number the ping carried";
        return false;
    }
    sequence = static_cast<uint64_t>(value->asInt());
    return true;
}

bool parseTransportAt(const json::Value& payload, TransportAt& out, std::string& error)
{
    TransportAt request;
    if (!sessionIdFromJson(payload, request.sessionId, error))
        return false;
    if (!timestampFromJson(payload, "serverTimeNs", request.serverTimeNs, error))
        return false;
    if (!timelineNsFromJson(payload, "timelineNs", request.timelineNs, error))
        return false;

    out = request;
    return true;
}

namespace {

// PAUSE_AT, STOP_AT and FLUSH say the same thing; only the type string differs.
json::Value makeTransportAt(MessageType type, const TransportAt& at)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(type));
    if (!at.sessionId.empty())
        root["sessionId"] = json::Value(at.sessionId);
    root["serverTimeNs"] = json::Value(at.serverTimeNs);
    root["timelineNs"] = json::Value(at.timelineNs);
    return root;
}

} // namespace

json::Value makePauseAt(const TransportAt& at)
{
    return makeTransportAt(MessageType::PauseAt, at);
}

json::Value makeStopAt(const TransportAt& at)
{
    return makeTransportAt(MessageType::StopAt, at);
}

json::Value makeFlush(const TransportAt& at)
{
    return makeTransportAt(MessageType::Flush, at);
}

bool parseSetVolume(const json::Value& payload, SetVolume& out, std::string& error)
{
    SetVolume request;
    if (!sessionIdFromJson(payload, request.sessionId, error))
        return false;

    const json::Value* value = payload.find("value");
    if (value != nullptr)
    {
        // A gain, so a fraction is the normal case and isIntegral() is not
        // required here the way it is for a nanosecond timestamp.
        if (!value->isNumber() || !(value->asDouble() >= 0.0))
        {
            error = "value must be a gain of zero or more";
            return false;
        }
        request.hasValue = true;
        request.value = value->asDouble();
    }

    const json::Value* muted = payload.find("muted");
    if (muted != nullptr)
    {
        if (!muted->isBool())
        {
            error = "muted must be a boolean";
            return false;
        }
        request.hasMuted = true;
        request.muted = muted->asBool();
    }

    if (!request.hasValue && !request.hasMuted)
    {
        error = "setVolume must carry a value, a muted flag, or both";
        return false;
    }

    if (payload.find("applyAtTimelineNs") != nullptr)
    {
        if (!timelineNsFromJson(payload, "applyAtTimelineNs", request.applyAtTimelineNs,
                                error))
            return false;
        request.hasApplyAtTimelineNs = true;
    }

    out = request;
    return true;
}

bool makeSetVolume(const SetVolume& request, json::Value& out, std::string& error)
{
    if (!request.hasValue && !request.hasMuted)
    {
        error = "setVolume must carry a value, a muted flag, or both";
        return false;
    }
    if (request.hasValue && !(request.value >= 0.0))
    {
        error = "value must be a gain of zero or more";
        return false;
    }

    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::SetVolume));
    if (!request.sessionId.empty())
        root["sessionId"] = json::Value(request.sessionId);
    // A field that is absent means unchanged, so only what the caller set is
    // written: a volume sent to a muted room leaves it muted.
    if (request.hasValue)
        root["value"] = json::Value(request.value);
    if (request.hasMuted)
        root["muted"] = json::Value(request.muted);
    if (request.hasApplyAtTimelineNs)
        root["applyAtTimelineNs"] = json::Value(request.applyAtTimelineNs);

    out = root;
    return true;
}

json::Value makeStreamReady(const std::string& sessionId, const StreamReady& ready)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::StreamReady));
    root["sessionId"] = json::Value(sessionId);
    root["ready"] = json::Value(ready.ready);
    if (!ready.powerState.empty())
        root["powerState"] = json::Value(ready.powerState);
    if (!ready.wakeOnLan.empty())
        root["wakeOnLan"] = json::Value(ready.wakeOnLan);
    if (!ready.ready && ready.standby)
        root["standby"] = json::Value(true);

    if (ready.ready)
    {
        // What the driver granted. The server compares it against the
        // SESSION_CONFIG it sent, so sending the requested format instead
        // would defeat the only check standing between the two 24-bit layouts
        // of section 12 and an hour of noise.
        root["format"] = formatToJson(ready.format);
        if (ready.periodFrames != 0)
            root["periodFrames"] = json::Value(ready.periodFrames);
        if (ready.hasDriftCorrection)
            root["driftCorrection"] = json::Value(driftCorrectionModeName(ready.driftCorrection));
    }
    else if (!ready.reason.empty())
    {
        root["reason"] = json::Value(ready.reason);
    }
    return root;
}

bool parseStreamReady(const json::Value& payload, std::string& sessionId, StreamReady& out,
                      std::string& error)
{
    std::string session;
    if (!requiredSessionIdFromJson(payload, session, error))
        return false;

    const json::Value* ready = payload.find("ready");
    if (ready == nullptr || !ready->isBool())
    {
        error = "ready must be a boolean";
        return false;
    }

    StreamReady result;
    result.ready = ready->asBool();
    if (payload.find("standby") != nullptr &&
        (!json::getBool(payload, "standby", result.standby) ||
         (result.ready && result.standby)))
    {
        error = "standby must be a boolean and requires ready=false";
        return false;
    }

    if (payload.find("powerState") != nullptr)
    {
        if (!json::getString(payload, "powerState", result.powerState) ||
            (result.powerState != "on" && result.powerState != "standby" &&
             result.powerState != "standbyManual" && result.powerState != "sleep" &&
             result.powerState != "sleepButtonOnly") ||
            (result.ready && result.powerState != "on") ||
            (result.standby != (result.powerState == "standby")))
        {
            error = "powerState must be on, standby, standbyManual, sleep or sleepButtonOnly and "
                    "agree with ready and standby";
            return false;
        }
    }

    // "sleep" is the one state a server can end, so it names what to wake.
    if (payload.find("wakeOnLan") != nullptr || result.powerState == "sleep")
    {
        const std::string& mac = result.wakeOnLan;
        bool valid = json::getString(payload, "wakeOnLan", result.wakeOnLan) &&
                     result.powerState == "sleep" && mac.size() == 17;
        for (size_t i = 0; valid && i < mac.size(); ++i)
            valid = i % 3 == 2 ? mac[i] == ':'
                               : (mac[i] >= '0' && mac[i] <= '9') || (mac[i] >= 'a' && mac[i] <= 'f');
        if (!valid)
        {
            error = "wakeOnLan must be a lower-case MAC address and comes with powerState sleep";
            return false;
        }
    }

    if (result.ready)
    {
        const json::Value* format = payload.find("format");
        if (format == nullptr || !format->isObject())
        {
            // The one check standing between the two 24-bit layouts of section
            // 12 and an hour of noise is comparing this against what was
            // asked for, and there is nothing to compare without it.
            error = "format must be an object when ready is true";
            return false;
        }
        if (!formatFromJson(*format, result.format, error))
            return false;

        const json::Value* period = payload.find("periodFrames");
        if (period != nullptr)
        {
            if (!period->isNumber() || !period->isIntegral() || period->asInt() <= 0 ||
                period->asInt() > 0xFFFFFFFF)
            {
                error = "periodFrames must be a positive frame count";
                return false;
            }
            result.periodFrames = static_cast<uint32_t>(period->asInt());
        }
        if (payload.find("driftCorrection") != nullptr)
        {
            if (!driftCorrectionModeFromJson(payload, "driftCorrection", result.driftCorrection,
                                             error))
                return false;
            result.hasDriftCorrection = true;
        }
    }
    else if (!optionalStringFromJson(payload, "reason", result.reason, error))
    {
        return false;
    }

    sessionId = session;
    out = result;
    return true;
}

json::Value makeBufferStatus(const std::string& sessionId, const BufferStatus& status)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::BufferStatus));
    root["sessionId"] = json::Value(sessionId);
    if (status.hasContent)
    {
        root["playheadTimelineNs"] = json::Value(status.playheadTimelineNs);
        root["bufferedUntilTimelineNs"] = json::Value(status.bufferedUntilTimelineNs);
    }
    // Media time, not the ring buffer's own frames: section 18 has the server
    // hold playback back until every room has enough, and "enough" is a length
    // of music rather than a count of samples this room happens to have been
    // resampled into.
    root["bufferedDurationNs"] = json::Value(status.bufferedDurationNs);
    return root;
}

bool parseBufferStatus(const json::Value& payload, std::string& sessionId, BufferStatus& out,
                       std::string& error)
{
    std::string session;
    if (!requiredSessionIdFromJson(payload, session, error))
        return false;

    BufferStatus status;
    if (!timelineNsFromJson(payload, "bufferedDurationNs", status.bufferedDurationNs, error))
        return false;

    // The two positions are sent together or not at all: their presence is
    // what "this room has content" means, and one without the other is a span
    // with one end.
    const bool hasPlayhead = payload.find("playheadTimelineNs") != nullptr;
    const bool hasUntil = payload.find("bufferedUntilTimelineNs") != nullptr;
    if (hasPlayhead != hasUntil)
    {
        error = "playheadTimelineNs and bufferedUntilTimelineNs must be sent together";
        return false;
    }
    if (hasPlayhead)
    {
        if (!timelineNsFromJson(payload, "playheadTimelineNs", status.playheadTimelineNs,
                                error))
            return false;
        if (!timelineNsFromJson(payload, "bufferedUntilTimelineNs",
                                status.bufferedUntilTimelineNs, error))
            return false;
        if (status.bufferedUntilTimelineNs < status.playheadTimelineNs)
        {
            error = "bufferedUntilTimelineNs is before playheadTimelineNs";
            return false;
        }
        status.hasContent = true;
    }

    // streamFrames is deliberately left at its default: it never leaves the
    // client, and deriving one from the duration would be the server inventing
    // a number it reports as measured.

    sessionId = session;
    out = status;
    return true;
}

json::Value makePlaybackReport(const std::string& sessionId, const PlaybackReport& report)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::PlaybackReport));
    root["sessionId"] = json::Value(sessionId);
    root["localMonotonicTimeNs"] = json::Value(report.localMonotonicTimeNs);
    root["deviceFramePosition"] = json::Value(report.deviceFramePosition);
    if (report.hasTimelineNsAudible)
        root["timelineNsAudible"] = json::Value(report.timelineNsAudible);
    root["bufferedDurationNs"] = json::Value(report.bufferedDurationNs);
    root["underruns"] = json::Value(report.underruns);
    root["clockQuality"] = json::Value(audioClockQualityName(report.clockQuality));
    if (report.hasDrift)
    {
        root["measuredSampleRate"] = json::Value(report.measuredSampleRate);
        root["measuredDriftPpm"] = json::Value(report.measuredDriftPpm);
        root["driftConfidence"] = json::Value(report.driftConfidence);
    }
    if (report.hasHardwareClock)
    {
        json::Value clock = json::Value::makeObject();
        clock["appliedPpm"] = json::Value(report.hardwareClockPpm);
        clock["appliedSequence"] = json::Value(report.hardwareClockSequence);
        clock["saturated"] = json::Value(report.hardwareClockSaturated);
        if (!report.hardwareClockError.empty())
            clock["error"] = json::Value(report.hardwareClockError);
        root["hardwareClock"] = clock;
    }
    return root;
}

bool parsePlaybackReport(const json::Value& payload, std::string& sessionId,
                         PlaybackReport& out, std::string& error)
{
    std::string session;
    if (!requiredSessionIdFromJson(payload, session, error))
        return false;

    PlaybackReport report;
    if (!timestampFromJson(payload, "localMonotonicTimeNs", report.localMonotonicTimeNs,
                           error))
        return false;

    const json::Value* position = payload.find("deviceFramePosition");
    if (position == nullptr || !position->isNumber() || !position->isIntegral() ||
        position->asInt() < 0)
    {
        // Mandatory even where the device reports nothing worth having: a
        // report that says the position is worthless is not a missing report.
        error = "deviceFramePosition must be a whole frame count";
        return false;
    }
    report.deviceFramePosition = static_cast<uint64_t>(position->asInt());

    std::string quality;
    if (!json::getString(payload, "clockQuality", quality) ||
        !parseAudioClockQuality(quality, report.clockQuality))
    {
        error = "clockQuality must be one of unavailable, estimated, driver, hardware";
        return false;
    }

    if (!timelineNsFromJson(payload, "bufferedDurationNs", report.bufferedDurationNs, error))
        return false;

    const json::Value* underruns = payload.find("underruns");
    if (underruns == nullptr || !underruns->isNumber() || !underruns->isIntegral() ||
        underruns->asInt() < 0)
    {
        error = "underruns must be a whole count of events";
        return false;
    }
    report.underruns = static_cast<uint64_t>(underruns->asInt());

    if (payload.find("timelineNsAudible") != nullptr)
    {
        if (!timelineNsFromJson(payload, "timelineNsAudible", report.timelineNsAudible, error))
            return false;
        report.hasTimelineNsAudible = true;
    }

    // The three go together: the client sends them as one fit, and a rate
    // without the confidence that belongs to it is not something the
    // controller may weight.
    const bool hasRate = payload.find("measuredSampleRate") != nullptr;
    const bool hasPpm = payload.find("measuredDriftPpm") != nullptr;
    const bool hasConfidence = payload.find("driftConfidence") != nullptr;
    if (hasRate != hasPpm || hasRate != hasConfidence)
    {
        error = "measuredSampleRate, measuredDriftPpm and driftConfidence must be sent "
                "together";
        return false;
    }
    if (hasRate)
    {
        if (!json::getDouble(payload, "measuredSampleRate", report.measuredSampleRate) ||
            !(report.measuredSampleRate > 0.0))
        {
            error = "measuredSampleRate must be a positive rate";
            return false;
        }
        if (!json::getDouble(payload, "measuredDriftPpm", report.measuredDriftPpm))
        {
            error = "measuredDriftPpm must be a number";
            return false;
        }
        if (!json::getDouble(payload, "driftConfidence", report.driftConfidence))
        {
            error = "driftConfidence must be a number";
            return false;
        }
        report.hasDrift = true;
    }

    if (const json::Value* clock = payload.find("hardwareClock"); clock != nullptr)
    {
        const json::Value* applied = clock->isObject() ? clock->find("appliedPpm") : nullptr;
        const json::Value* sequence = clock->isObject() ? clock->find("appliedSequence") : nullptr;
        if (applied == nullptr || sequence == nullptr || !applied->isIntegral() ||
            !sequence->isIntegral() || applied->asInt() < -kMaxClockTrimPpm ||
            applied->asInt() > kMaxClockTrimPpm || sequence->asInt() < 0 ||
            !json::getBool(*clock, "saturated", report.hardwareClockSaturated) ||
            !optionalStringFromJson(*clock, "error", report.hardwareClockError, error))
        {
            error = "hardwareClock must carry a whole appliedPpm, appliedSequence and saturated";
            return false;
        }
        report.hardwareClockPpm = static_cast<int32_t>(applied->asInt());
        report.hardwareClockSequence = static_cast<uint64_t>(sequence->asInt());
        report.hasHardwareClock = true;
    }

    sessionId = session;
    out = report;
    return true;
}

namespace {

bool optionalPercentFromJson(const json::Value& object, const char* key, bool& has,
                             double& out, std::string& error)
{
    const json::Value* value = object.find(key);
    if (value == nullptr)
        return true;
    if (!value->isNumber() || !(value->asDouble() >= 0.0 && value->asDouble() <= 100.0))
    {
        error = std::string(key) + " must be a percentage from 0 to 100";
        return false;
    }
    has = true;
    out = value->asDouble();
    return true;
}

bool optionalCountFromJson(const json::Value& object, const char* key, bool& has,
                           uint64_t& out, std::string& error)
{
    const json::Value* value = object.find(key);
    if (value == nullptr)
        return true;
    if (!value->isNumber() || !value->isIntegral() || value->asInt() < 0)
    {
        error = std::string(key) + " must be a whole, non-negative number";
        return false;
    }
    has = true;
    out = static_cast<uint64_t>(value->asInt());
    return true;
}

} // namespace

json::Value makeSystemMetrics(const SystemMetrics& metrics)
{
    json::Value object = json::Value::makeObject();
    if (metrics.hasCpuPercent)
        object["cpuPercent"] = json::Value(metrics.cpuPercent);
    if (metrics.hasSystemCpuPercent)
        object["systemCpuPercent"] = json::Value(metrics.systemCpuPercent);
    if (metrics.hasMemoryBytes)
        object["memoryBytes"] = json::Value(metrics.memoryBytes);
    if (metrics.hasPrivateMemoryBytes)
        object["privateMemoryBytes"] = json::Value(metrics.privateMemoryBytes);
    if (metrics.hasThreads)
        object["threads"] = json::Value(metrics.threads);
    if (metrics.hasHandles)
        object["handles"] = json::Value(metrics.handles);
    if (metrics.hasSystemMemoryTotalBytes)
        object["systemMemoryTotalBytes"] = json::Value(metrics.systemMemoryTotalBytes);
    if (metrics.hasSystemMemoryAvailableBytes)
        object["systemMemoryAvailableBytes"] = json::Value(metrics.systemMemoryAvailableBytes);
    if (metrics.hasInternalMemoryTotalBytes)
        object["internalMemoryTotalBytes"] = json::Value(metrics.internalMemoryTotalBytes);
    if (metrics.hasInternalMemoryAvailableBytes)
        object["internalMemoryAvailableBytes"] = json::Value(metrics.internalMemoryAvailableBytes);
    if (metrics.hasPsramTotalBytes)
        object["psramTotalBytes"] = json::Value(metrics.psramTotalBytes);
    if (metrics.hasPsramAvailableBytes)
        object["psramAvailableBytes"] = json::Value(metrics.psramAvailableBytes);
    if (metrics.hasUptimeMs)
        object["uptimeMs"] = json::Value(metrics.uptimeMs);
    if (metrics.hasSystemUptimeMs)
        object["systemUptimeMs"] = json::Value(metrics.systemUptimeMs);
    if (metrics.hasTemperatureC)
        object["temperatureC"] = json::Value(metrics.temperatureC);
    if (!metrics.cpuArchitecture.empty())
        object["cpuArchitecture"] = json::Value(metrics.cpuArchitecture);
    if (!metrics.cpuModel.empty())
        object["cpuModel"] = json::Value(metrics.cpuModel);
    if (!metrics.machineModel.empty())
        object["machineModel"] = json::Value(metrics.machineModel);
    if (metrics.hasCpuCores)
        object["cpuCores"] = json::Value(metrics.cpuCores);
    if (metrics.hasCpuMhz)
        object["cpuMhz"] = json::Value(metrics.cpuMhz);
    if (metrics.hasWifiRssiDbm)
        object["wifiRssiDbm"] = json::Value(static_cast<int64_t>(metrics.wifiRssiDbm));
    if (metrics.hasWifiChannel)
        object["wifiChannel"] = json::Value(metrics.wifiChannel);
    if (!metrics.wifiBssid.empty())
        object["wifiBssid"] = json::Value(metrics.wifiBssid);
    if (metrics.hasWifiRxRateKbps)
        object["wifiRxRateKbps"] = json::Value(metrics.wifiRxRateKbps);
    if (!metrics.networkLink.empty())
        object["networkLink"] = json::Value(metrics.networkLink);
    return object;
}

bool parseSystemMetrics(const json::Value& object, SystemMetrics& out, std::string& error)
{
    if (!object.isObject())
    {
        error = "system must be an object";
        return false;
    }

    SystemMetrics metrics;
    if (!optionalPercentFromJson(object, "cpuPercent", metrics.hasCpuPercent,
                                 metrics.cpuPercent, error) ||
        !optionalPercentFromJson(object, "systemCpuPercent", metrics.hasSystemCpuPercent,
                                 metrics.systemCpuPercent, error) ||
        !optionalCountFromJson(object, "memoryBytes", metrics.hasMemoryBytes,
                               metrics.memoryBytes, error) ||
        !optionalCountFromJson(object, "privateMemoryBytes", metrics.hasPrivateMemoryBytes,
                               metrics.privateMemoryBytes, error) ||
        !optionalCountFromJson(object, "threads", metrics.hasThreads, metrics.threads, error) ||
        !optionalCountFromJson(object, "handles", metrics.hasHandles, metrics.handles, error) ||
        !optionalCountFromJson(object, "systemMemoryTotalBytes",
                               metrics.hasSystemMemoryTotalBytes,
                               metrics.systemMemoryTotalBytes, error) ||
        !optionalCountFromJson(object, "systemMemoryAvailableBytes",
                               metrics.hasSystemMemoryAvailableBytes,
                               metrics.systemMemoryAvailableBytes, error) ||
        !optionalCountFromJson(object, "internalMemoryTotalBytes",
                               metrics.hasInternalMemoryTotalBytes,
                               metrics.internalMemoryTotalBytes, error) ||
        !optionalCountFromJson(object, "internalMemoryAvailableBytes",
                               metrics.hasInternalMemoryAvailableBytes,
                               metrics.internalMemoryAvailableBytes, error) ||
        !optionalCountFromJson(object, "psramTotalBytes", metrics.hasPsramTotalBytes,
                               metrics.psramTotalBytes, error) ||
        !optionalCountFromJson(object, "psramAvailableBytes", metrics.hasPsramAvailableBytes,
                               metrics.psramAvailableBytes, error) ||
        !optionalCountFromJson(object, "uptimeMs", metrics.hasUptimeMs, metrics.uptimeMs,
                               error) ||
        !optionalCountFromJson(object, "systemUptimeMs", metrics.hasSystemUptimeMs,
                               metrics.systemUptimeMs, error))
    {
        return false;
    }

    if (const json::Value* temperature = object.find("temperatureC"))
    {
        if (!temperature->isNumber())
        {
            error = "temperatureC must be a number";
            return false;
        }
        metrics.hasTemperatureC = true;
        metrics.temperatureC = temperature->asDouble();
    }

    // dBm is the one signed figure here: a whole number, and never above 0.
    if (const json::Value* rssi = object.find("wifiRssiDbm"))
    {
        if (!rssi->isIntegral() || rssi->asInt() > 0 || rssi->asInt() < -255)
        {
            error = "wifiRssiDbm must be a whole number from -255 to 0";
            return false;
        }
        metrics.hasWifiRssiDbm = true;
        metrics.wifiRssiDbm = static_cast<int32_t>(rssi->asInt());
    }
    if (!optionalStringFromJson(object, "cpuArchitecture", metrics.cpuArchitecture, error) ||
        !optionalStringFromJson(object, "cpuModel", metrics.cpuModel, error) ||
        !optionalStringFromJson(object, "machineModel", metrics.machineModel, error) ||
        !optionalCountFromJson(object, "cpuCores", metrics.hasCpuCores, metrics.cpuCores,
                               error) ||
        !optionalCountFromJson(object, "cpuMhz", metrics.hasCpuMhz, metrics.cpuMhz, error) ||
        !optionalCountFromJson(object, "wifiChannel", metrics.hasWifiChannel,
                               metrics.wifiChannel, error) ||
        !optionalStringFromJson(object, "wifiBssid", metrics.wifiBssid, error) ||
        !optionalCountFromJson(object, "wifiRxRateKbps", metrics.hasWifiRxRateKbps,
                               metrics.wifiRxRateKbps, error) ||
        !optionalStringFromJson(object, "networkLink", metrics.networkLink, error))
    {
        return false;
    }

    out = metrics;
    return true;
}

json::Value makeSystemReport(const std::string& sessionId, const SystemMetrics& metrics)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::SystemReport));
    root["sessionId"] = json::Value(sessionId);
    root["system"] = makeSystemMetrics(metrics);
    return root;
}

bool parseSystemReport(const json::Value& payload, std::string& sessionId, SystemMetrics& out,
                       std::string& error)
{
    std::string session;
    if (!requiredSessionIdFromJson(payload, session, error))
        return false;

    const json::Value* system = payload.find("system");
    if (system == nullptr)
    {
        error = "system is missing";
        return false;
    }
    SystemMetrics metrics;
    if (!parseSystemMetrics(*system, metrics, error))
        return false;

    sessionId = session;
    out = metrics;
    return true;
}

json::Value makeAudioDevices(const std::string& sessionId, const AudioDeviceList& list)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::AudioDevices));
    root["sessionId"] = json::Value(sessionId);
    root["configuredDeviceId"] = json::Value(list.configuredDeviceId);
    if (!list.selectedDeviceId.empty())
        root["selectedDeviceId"] = json::Value(list.selectedDeviceId);

    json::Value devices = json::Value::makeArray();
    for (size_t i = 0; i < list.devices.size(); ++i)
    {
        json::Value device = json::Value::makeObject();
        device["id"] = json::Value(list.devices[i].id);
        device["name"] = json::Value(list.devices[i].name);
        device["isDefault"] = json::Value(list.devices[i].isDefault);
        devices.pushBack(device);
    }
    root["devices"] = devices;
    return root;
}

bool parseAudioDevices(const json::Value& payload, std::string& sessionId, AudioDeviceList& out,
                       std::string& error)
{
    std::string session;
    if (!requiredSessionIdFromJson(payload, session, error))
        return false;

    AudioDeviceList list;
    const json::Value* configured = payload.find("configuredDeviceId");
    if (configured == nullptr || !configured->isString())
    {
        error = "configuredDeviceId must be a string";
        return false;
    }
    list.configuredDeviceId = configured->asString();

    if (!optionalStringFromJson(payload, "selectedDeviceId", list.selectedDeviceId, error))
        return false;

    const json::Value* devices = payload.find("devices");
    if (devices == nullptr || !devices->isArray())
    {
        error = "devices must be an array";
        return false;
    }
    if (devices->size() > kMaxAudioDevices)
    {
        error = "devices lists " + std::to_string(devices->size()) + " entries, more than " +
                std::to_string(kMaxAudioDevices);
        return false;
    }

    list.devices.reserve(devices->size());
    for (size_t i = 0; i < devices->size(); ++i)
    {
        const json::Value& entry = devices->at(i);
        AudioDeviceEntry device;
        if (!entry.isObject() || !json::getString(entry, "id", device.id) || device.id.empty())
        {
            error = "devices[" + std::to_string(i) + "] needs a non-empty id";
            return false;
        }
        if (!optionalStringFromJson(entry, "name", device.name, error))
        {
            error = "devices[" + std::to_string(i) + "]: " + error;
            return false;
        }
        if (const json::Value* isDefault = entry.find("isDefault"))
        {
            if (!isDefault->isBool())
            {
                error = "devices[" + std::to_string(i) + "]: isDefault must be a boolean";
                return false;
            }
            device.isDefault = isDefault->asBool();
        }
        list.devices.push_back(device);
    }

    sessionId = session;
    out = list;
    return true;
}

json::Value makeSetAudioDevice(const SetAudioDevice& request)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::SetAudioDevice));
    if (!request.sessionId.empty())
        root["sessionId"] = json::Value(request.sessionId);
    root["deviceId"] = json::Value(request.deviceId);
    return root;
}

bool parseSetAudioDevice(const json::Value& payload, SetAudioDevice& out, std::string& error)
{
    SetAudioDevice request;
    if (!sessionIdFromJson(payload, request.sessionId, error))
        return false;

    // Required although it may be empty: empty is the system default, and a
    // message that left the key out asked for nothing.
    const json::Value* deviceId = payload.find("deviceId");
    if (deviceId == nullptr || !deviceId->isString())
    {
        error = "deviceId must be a string";
        return false;
    }
    request.deviceId = deviceId->asString();

    out = request;
    return true;
}

json::Value makeRescanAudio(const std::string& sessionId)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::RescanAudio));
    if (!sessionId.empty())
        root["sessionId"] = json::Value(sessionId);
    return root;
}

bool parseRescanAudio(const json::Value& payload, std::string& sessionId, std::string& error)
{
    std::string session;
    if (!sessionIdFromJson(payload, session, error))
        return false;
    sessionId = session;
    return true;
}

namespace {

// A buffer size in milliseconds: a positive whole number that fits the field.
bool bufferMsFromJson(const json::Value& object, uint32_t& out, std::string& error)
{
    const json::Value* value = object.find("bufferMs");
    if (value == nullptr || !value->isNumber() || !value->isIntegral() || value->asInt() <= 0 ||
        value->asInt() > static_cast<int64_t>(UINT32_MAX))
    {
        error = "bufferMs must be a positive whole number of milliseconds";
        return false;
    }
    out = static_cast<uint32_t>(value->asInt());
    return true;
}

} // namespace

json::Value makeSetBuffer(const SetBuffer& request)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::SetBuffer));
    if (!request.sessionId.empty())
        root["sessionId"] = json::Value(request.sessionId);
    root["bufferMs"] = json::Value(request.bufferMs);
    return root;
}

bool parseSetBuffer(const json::Value& payload, SetBuffer& out, std::string& error)
{
    SetBuffer request;
    if (!sessionIdFromJson(payload, request.sessionId, error) ||
        !bufferMsFromJson(payload, request.bufferMs, error))
        return false;
    out = request;
    return true;
}

json::Value makeBufferConfig(const std::string& sessionId, uint32_t bufferMs)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::BufferConfig));
    root["sessionId"] = json::Value(sessionId);
    root["bufferMs"] = json::Value(bufferMs);
    return root;
}

bool parseBufferConfig(const json::Value& payload, std::string& sessionId,
                       uint32_t& bufferMs, std::string& error)
{
    std::string session;
    uint32_t value = 0;
    if (!requiredSessionIdFromJson(payload, session, error) ||
        !bufferMsFromJson(payload, value, error))
        return false;
    sessionId = session;
    bufferMs = value;
    return true;
}

namespace {

bool volumeControlModeFromJson(const json::Value& payload, const char* key,
                               VolumeControlMode& out, std::string& error)
{
    const json::Value* value = payload.find(key);
    if (value == nullptr || !value->isString() || !parseVolumeControlMode(value->asString(), out))
    {
        error = std::string(key) + " must be auto, hardware, software or disabled";
        return false;
    }
    return true;
}

} // namespace

json::Value makeSetVolumeControl(const SetVolumeControl& request)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::SetVolumeControl));
    if (!request.sessionId.empty())
        root["sessionId"] = json::Value(request.sessionId);
    root["volumeControl"] = json::Value(volumeControlModeName(request.volumeControl));
    return root;
}

bool parseSetVolumeControl(const json::Value& payload, SetVolumeControl& out,
                           std::string& error)
{
    SetVolumeControl request;
    if (!sessionIdFromJson(payload, request.sessionId, error) ||
        !volumeControlModeFromJson(payload, "volumeControl", request.volumeControl, error))
        return false;
    out = request;
    return true;
}

json::Value makeVolumeConfig(const VolumeConfig& config)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::VolumeConfig));
    root["sessionId"] = json::Value(config.sessionId);
    root["volumeControl"] = json::Value(volumeControlModeName(config.volumeControl));
    root["effective"] = json::Value(volumeControlModeName(config.effective));
    if (!config.reason.empty())
        root["reason"] = json::Value(config.reason);
    return root;
}

bool parseVolumeConfig(const json::Value& payload, VolumeConfig& out, std::string& error)
{
    VolumeConfig config;
    if (!requiredSessionIdFromJson(payload, config.sessionId, error) ||
        !volumeControlModeFromJson(payload, "volumeControl", config.volumeControl, error) ||
        !volumeControlModeFromJson(payload, "effective", config.effective, error) ||
        !optionalStringFromJson(payload, "reason", config.reason, error))
        return false;
    if (config.effective == VolumeControlMode::Auto)
    {
        error = "effective is the path the client resolved, never auto";
        return false;
    }
    out = config;
    return true;
}

json::Value makeSetDriftCorrection(const SetDriftCorrection& request)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::SetDriftCorrection));
    if (!request.sessionId.empty())
        root["sessionId"] = json::Value(request.sessionId);
    root["driftCorrection"] = json::Value(driftCorrectionModeName(request.driftCorrection));
    return root;
}

bool parseSetDriftCorrection(const json::Value& payload, SetDriftCorrection& out,
                             std::string& error)
{
    SetDriftCorrection request;
    if (!sessionIdFromJson(payload, request.sessionId, error) ||
        !driftCorrectionModeFromJson(payload, "driftCorrection", request.driftCorrection, error))
        return false;
    out = request;
    return true;
}

json::Value makeDriftFeedback(const DriftFeedback& feedback)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::DriftFeedback));
    root["sessionId"] = json::Value(feedback.sessionId);
    root["sequence"] = json::Value(feedback.sequence);
    if (feedback.hasDrift)
    {
        root["measuredDriftPpm"] = json::Value(feedback.measuredDriftPpm);
        root["driftConfidence"] = json::Value(feedback.driftConfidence);
    }
    if (feedback.hasHardwareTarget)
        root["hardwareTargetPpm"] = json::Value(feedback.hardwareTargetPpm);
    return root;
}

bool parseDriftFeedback(const json::Value& payload, DriftFeedback& out, std::string& error)
{
    DriftFeedback feedback;
    if (!requiredSessionIdFromJson(payload, feedback.sessionId, error))
        return false;
    const json::Value* sequence = payload.find("sequence");
    if (sequence == nullptr || !sequence->isIntegral() || sequence->asInt() <= 0)
    {
        error = "sequence must be a positive whole number";
        return false;
    }
    feedback.sequence = static_cast<uint64_t>(sequence->asInt());

    const bool hasPpm = payload.find("measuredDriftPpm") != nullptr;
    if (hasPpm != (payload.find("driftConfidence") != nullptr))
    {
        error = "measuredDriftPpm and driftConfidence must be sent together";
        return false;
    }
    if (hasPpm)
    {
        if (!json::getDouble(payload, "measuredDriftPpm", feedback.measuredDriftPpm) ||
            !std::isfinite(feedback.measuredDriftPpm) ||
            !json::getDouble(payload, "driftConfidence", feedback.driftConfidence) ||
            !(feedback.driftConfidence >= 0.0 && feedback.driftConfidence <= 1.0))
        {
            error = "measuredDriftPpm must be finite and driftConfidence within 0 to 1";
            return false;
        }
        feedback.hasDrift = true;
    }
    if (payload.find("hardwareTargetPpm") != nullptr)
    {
        if (!json::getDouble(payload, "hardwareTargetPpm", feedback.hardwareTargetPpm) ||
            !std::isfinite(feedback.hardwareTargetPpm) ||
            std::fabs(feedback.hardwareTargetPpm) > static_cast<double>(kMaxClockTrimPpm))
        {
            error = "hardwareTargetPpm must be a finite trim";
            return false;
        }
        feedback.hasHardwareTarget = true;
    }
    out = feedback;
    return true;
}

bool isValidRoomName(const std::string& name)
{
    if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ')
        return false;
    for (const char c : name)
    {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F)
            return false;
    }
    return true;
}

namespace {

bool roomNameFromJson(const json::Value& payload, std::string& out, std::string& error)
{
    const json::Value* value = payload.find("name");
    if (value == nullptr || !value->isString() || !isValidRoomName(value->asString()))
    {
        error = "name must be 1 to 64 bytes without control characters or surrounding blanks";
        return false;
    }
    out = value->asString();
    return true;
}

} // namespace

json::Value makeSetRoomName(const SetRoomName& request)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::SetRoomName));
    if (!request.sessionId.empty())
        root["sessionId"] = json::Value(request.sessionId);
    root["name"] = json::Value(request.name);
    return root;
}

bool parseSetRoomName(const json::Value& payload, SetRoomName& out, std::string& error)
{
    SetRoomName request;
    if (!sessionIdFromJson(payload, request.sessionId, error) ||
        !roomNameFromJson(payload, request.name, error))
        return false;
    out = request;
    return true;
}

json::Value makeRoomName(const std::string& sessionId, const std::string& name)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::RoomName));
    root["sessionId"] = json::Value(sessionId);
    root["name"] = json::Value(name);
    return root;
}

bool parseRoomName(const json::Value& payload, std::string& sessionId, std::string& name,
                   std::string& error)
{
    std::string session;
    std::string value;
    if (!requiredSessionIdFromJson(payload, session, error) ||
        !roomNameFromJson(payload, value, error))
        return false;
    sessionId = session;
    name = value;
    return true;
}

json::Value makeStreamAttach(const std::string& sessionId, const Identity& identity)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::StreamAttach));
    root["sessionId"] = json::Value(sessionId);
    // The client id as well as the session: a server that has already torn the
    // session down can still tell which room reconnected.
    root["clientId"] = json::Value(identity.clientId);
    return root;
}

bool parseStreamAttach(const json::Value& payload, std::string& sessionId,
                       std::string& clientId, std::string& error)
{
    std::string session;
    if (!requiredSessionIdFromJson(payload, session, error))
        return false;

    std::string client;
    if (!json::getString(payload, "clientId", client) || client.empty())
    {
        error = "clientId must be a non-empty string";
        return false;
    }

    sessionId = session;
    clientId = client;
    return true;
}

std::string errorText(const json::Value& payload)
{
    std::string message;
    json::getString(payload, "message", message);
    if (message.empty())
        message = "no message";

    std::string code;
    if (json::getString(payload, "code", code) && !code.empty())
        return code + ": " + message;
    return message;
}

json::Value makeError(const std::string& code, const std::string& message)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::Error));
    if (!code.empty())
        root["code"] = json::Value(code);
    root["message"] = json::Value(message);
    return root;
}

} // namespace matp
} // namespace multiroom
