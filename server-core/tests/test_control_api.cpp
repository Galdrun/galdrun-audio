// Phase 17's control API, driven the way the high-end server drives it: one
// JSON line at a time over a real loopback socket, on a port the system chose.
//
// Every command is sent over the wire, error cases included, because the
// parser, the framing and the listener are what a caller actually meets. The
// rooms behind the zones are real `ClientSession`s fed with the client's own
// half of the wire library -- the same arrangement test_client_session.cpp
// uses -- and joined to the zones by the API's own session hooks, so the last
// test is a house brought from nothing to playing in two rooms and back with
// no part of the path between a request line and a START_AT left out.
//
// Nothing sleeps for a duration the server uses. The clock is a ManualClock
// that never moves, so no timeout here can fire; waits are for events, with a
// deadline that bounds a hang and measures nothing.

#include "ManualClock.h"
#include "TestSupport.h"

#include "audio/BlockPool.h"
#include "client/ClientSession.h"
#include "client/OutputPlanner.h"
#include "control/ControlApi.h"
#include "control/ControlListener.h"
#include "control/RoomDirectory.h"
#include "matp/Protocol.h"
#include "net/Socket.h"
#include "resampler/FakeResampler.h"
#include "resampler/ResamplerRegistry.h"
#include "zone/ZoneManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace multiroom;

namespace {

constexpr auto kDeadline = std::chrono::seconds(5);
constexpr uint32_t kRate = 44100;
constexpr uint64_t kSourceFrames = kRate * 3;

// Winsock is reference counted per process; one here keeps it up for every
// socket this file opens, whichever listener comes and goes.
net::Library g_socketLibrary;

AudioFormat formatOf(uint32_t rate, AudioFormat::SampleType type = AudioFormat::SampleType::Int16)
{
    AudioFormat format;
    format.sampleRate = rate;
    format.channels = 2;
    format.sampleType = type;
    return format;
}

template <typename Predicate>
bool waitFor(Predicate predicate)
{
    const auto until = std::chrono::steady_clock::now() + kDeadline;
    while (std::chrono::steady_clock::now() < until)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

// --- a source that needs no file --------------------------------------------

class SilentDecoder final : public IDecoder
{
public:
    explicit SilentDecoder(uint64_t start = 0) : next_(start)
    {
        info_.format = formatOf(kRate);
        info_.totalFrames = kSourceFrames;
        info_.codecName = "silence";
        info_.containerName = "generated";
    }

    Status<DecodeError> open(std::unique_ptr<IMediaSource>) override { return {}; }
    bool opened() const noexcept override { return true; }
    const SourceStreamInfo& info() const noexcept override { return info_; }

    Result<DecodedSpan, DecodeError> decode(std::span<std::byte> output) override
    {
        if (next_ >= kSourceFrames)
            return fail(DecodeError::EndOfStream, "done");
        const uint32_t bytesPerFrame = info_.format.bytesPerFrame();
        const uint64_t frames =
            std::min<uint64_t>({output.size() / bytesPerFrame, 1024, kSourceFrames - next_});
        std::fill_n(output.begin(), frames * bytesPerFrame, std::byte{0});

        DecodedSpan span;
        span.firstFrame = SourceFrame{next_};
        span.frameCount = static_cast<uint32_t>(frames);
        span.byteCount = static_cast<size_t>(frames * bytesPerFrame);
        next_ += frames;
        return span;
    }

private:
    SourceStreamInfo info_;
    uint64_t next_ = 0;
};

// "missing" is the URI that cannot be opened and "live" the one that cannot
// seek; everything else is silence, from wherever it is opened.
class SilentDecoderFactory final : public IDecoderFactory
{
public:
    Result<std::unique_ptr<IDecoder>, DecodeError> open(const std::string& uri,
                                                        uint64_t startOffsetNs) override
    {
        if (uri == "http://127.0.0.1:4714/missing")
            return fail(DecodeError::SourceFailed, "NotFound: " + uri);
        if (startOffsetNs != 0 && uri == "http://127.0.0.1:4714/live")
            return fail(DecodeError::NotSeekable, uri + " cannot seek");
        const uint64_t frame = framesFromNanoseconds(startOffsetNs, kRate).value_or(kSourceFrames);
        if (frame >= kSourceFrames)
            return fail(DecodeError::OffsetOutOfRange, uri);
        return std::unique_ptr<IDecoder>(std::make_unique<SilentDecoder>(frame));
    }
};

// --- what the API and the zones asked the rooms for -------------------------

struct CommandLog
{
    mutable std::mutex mutex;
    std::vector<RoomCommand> sent;

    std::vector<RoomCommand> all() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return sent;
    }

    size_t count(RoomCommandKind kind, const std::string& client) const
    {
        size_t n = 0;
        for (const RoomCommand& command : all())
            n += command.kind == kind && command.client.str() == client ? 1 : 0;
        return n;
    }
};

// --- the caller --------------------------------------------------------------

class LineClient
{
public:
    // `receiveBytes` above zero shrinks this end's receive window, which is
    // what a caller that stops reading fills first.
    explicit LineClient(uint16_t port, int receiveBytes = 0)
    {
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (receiveBytes > 0)
        {
            ::setsockopt(socket_, SOL_SOCKET, SO_RCVBUF,
                         reinterpret_cast<const char*>(&receiveBytes), sizeof(receiveBytes));
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (socket_ == net::kInvalidSocket ||
            ::connect(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        {
            multiroomtest::failAt("could not connect to the control API", __FILE__, __LINE__);
        }
        if (!net::setNonBlocking(socket_))
            multiroomtest::failAt("could not make the caller non-blocking", __FILE__, __LINE__);
    }

    ~LineClient() { net::closeSocket(socket_); }

    LineClient(const LineClient&) = delete;
    LineClient& operator=(const LineClient&) = delete;

    void sendRaw(const std::string& bytes)
    {
        size_t written = 0;
        while (written < bytes.size())
        {
            const int sent = net::transmit(socket_, bytes.data() + written,
                                           static_cast<int>(bytes.size() - written));
            if (sent > 0)
                written += static_cast<size_t>(sent);
            else if (!(sent < 0 && net::wouldBlock(net::lastError())))
                multiroomtest::failAt("the control API stopped reading", __FILE__, __LINE__);
        }
    }

    // The next response line, parsed. A line that is not JSON is a failure:
    // every answer this API gives is one.
    json::Value readLine()
    {
        const auto until = std::chrono::steady_clock::now() + kDeadline;
        for (;;)
        {
            if (const size_t newline = inbox_.find('\n'); newline != std::string::npos)
            {
                const std::string line = inbox_.substr(0, newline);
                inbox_.erase(0, newline + 1);
                json::Value value;
                std::string error;
                if (!json::parse(line, value, error))
                    multiroomtest::failAt("an answer that is not JSON: " + line, __FILE__, __LINE__);
                return value;
            }
            if (std::chrono::steady_clock::now() > until)
                multiroomtest::failAt("timed out waiting for an answer", __FILE__, __LINE__);

            char buffer[4096];
            const int received = net::receive(socket_, buffer, static_cast<int>(sizeof(buffer)));
            if (received > 0)
                inbox_.append(buffer, static_cast<size_t>(received));
            else if (received == 0)
                multiroomtest::failAt("the control API closed the connection", __FILE__, __LINE__);
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    json::Value request(const std::string& line)
    {
        sendRaw(line + "\n");
        return readLine();
    }

    // The next line if one arrives within `quiet`, for reading a connection
    // until it has nothing more to say.
    std::optional<std::string> readRawWithin(std::chrono::milliseconds quiet)
    {
        auto until = std::chrono::steady_clock::now() + quiet;
        for (;;)
        {
            if (const size_t newline = inbox_.find('\n'); newline != std::string::npos)
            {
                std::string line = inbox_.substr(0, newline);
                inbox_.erase(0, newline + 1);
                return line;
            }
            if (std::chrono::steady_clock::now() > until)
                return std::nullopt;

            char buffer[4096];
            const int received = net::receive(socket_, buffer, static_cast<int>(sizeof(buffer)));
            if (received > 0)
            {
                inbox_.append(buffer, static_cast<size_t>(received));
                until = std::chrono::steady_clock::now() + quiet;
            }
            else if (received == 0)
                return std::nullopt;
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

private:
    net::Socket socket_ = net::kInvalidSocket;
    std::string inbox_;
};

bool isOk(const json::Value& response)
{
    bool ok = false;
    return json::getBool(response, "ok", ok) && ok;
}

std::string errorOf(const json::Value& response)
{
    std::string code;
    json::getString(response, "error", code);
    return code;
}

std::string text(const json::Value& object, const char* key)
{
    std::string out;
    json::getString(object, key, out);
    return out;
}

bool flag(const json::Value& object, const char* key)
{
    bool out = false;
    json::getBool(object, key, out);
    return out;
}

std::vector<std::string> strings(const json::Value& object, const char* key)
{
    std::vector<std::string> out;
    const json::Value* list = object.find(key);
    if (list == nullptr || !list->isArray())
        return out;
    for (size_t i = 0; i < list->size(); ++i)
        out.push_back(list->at(i).asString());
    return out;
}

// The entry of `key`'s array whose `idKey` is `id`, or null.
const json::Value* entry(const json::Value& object, const char* key, const char* idKey,
                         const std::string& id)
{
    const json::Value* list = object.find(key);
    if (list == nullptr || !list->isArray())
        return nullptr;
    for (size_t i = 0; i < list->size(); ++i)
    {
        if (text(list->at(i), idKey) == id)
            return &list->at(i);
    }
    return nullptr;
}

// --- the server, without main() ----------------------------------------------

struct ApiRig
{
    ApiRig()
    {

        // The fake is registered and *not* chosen: the server starts with no
        // converter, and resampler.configure is what picks one.
        CHECK_OK(resamplers.add(std::make_unique<FakeResamplerFactory>()));
        planner.emplace(OutputPlanner::withoutResampler());

        ZoneSettings settings;
        settings.output.packetDurationMs = 20;
        settings.output.targetBufferNs = kNanosecondsPerSecond;
        settings.output.maxQueuedPackets = 1024;
        settings.retentionNs = 60ULL * kNanosecondsPerSecond;
        settings.start.minimumStartBufferNs = 100ULL * 1000000ULL;
        settings.start.minClockExchanges = 0; // no clock exchange is walked here
        settings.roomPreference = [this](const ClientId& client) {
            return rooms.preference(client);
        };
        settings.roomEqualizer = [this](const ClientId& client) {
            const std::optional<RoomRecord> room = rooms.find(client);
            return room && room->equalizer ? *room->equalizer : EqualizerSettings{};
        };
        settings.sendCommand = [this](const RoomCommand& command) { return record(command); };
        settings.onEvent = [this](const ZoneEvent& event) {
            publish(ControlApi::eventLine(event));
        };

        zones = std::make_unique<ZoneManager>(memory, *planner, resamplers, decoders, clock,
                                              settings);
        auto media = MediaOrigin::parse(MediaOrigin::kDefaultBaseUrl);
        CHECK_OK(media);
        api = std::make_unique<ControlApi>(
            *zones, rooms, discovered, *planner, resamplers, clock, std::move(*media),
            [this](const RoomCommand& command) { return record(command); },
            [this](const std::string& line) { publish(line); });

        ControlListenerSettings listening;
        listening.port = 0;
        auto started = ControlListener::start(listening,
                                              [this](const std::string& line, bool& subscribed) {
                                                  return api->handleLine(line, subscribed);
                                              });
        CHECK_OK(started);
        listener = std::move(*started);
        std::lock_guard<std::mutex> lock(eventMutex);
        events = listener.get();
    }

    ~ApiRig()
    {
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            events = nullptr;
        }
        listener.reset();
        zones->stop();
    }

    // main.cpp's arrangement: the listener is made after everything that
    // produces an event, so the producers reach it through a guarded pointer.
    void publish(const std::string& line)
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        if (events != nullptr)
            events->publish(line);
    }

    bool record(const RoomCommand& command)
    {
        std::lock_guard<std::mutex> lock(log.mutex);
        log.sent.push_back(command);
        return true;
    }

    SilentDecoderFactory decoders;
    multiroomtest::ManualClock clock{1000000000ULL};
    AudioMemory memory{blockBytesFor(192000, 2, 20), 256ULL * 1024 * 1024};
    ResamplerRegistry resamplers;
    std::optional<OutputPlanner> planner;
    CommandLog log;
    RoomDirectory rooms{RoomAudioSettings{}};
    DiscoveredClients discovered;
    std::unique_ptr<ZoneManager> zones;
    std::unique_ptr<ControlApi> api;
    std::mutex eventMutex;
    ControlListener* events = nullptr;         // guarded by eventMutex
    std::unique_ptr<ControlListener> listener; // last: stopped before what it calls
};

// --- a room, reduced to its session ------------------------------------------

class Room
{
public:
    // `opens` false is a device that answers every PREPARE_STREAM with
    // ready:false -- held by another application, say.
    Room(ApiRig& rig, const std::string& id, std::vector<AudioFormat> formats, uint64_t low,
         bool opens = true)
        : id_(id), formats_(std::move(formats)), opens_(opens),
          session_(SessionId(0x0123456789abcdefULL, low), rig.clock, settingsFor(rig))
    {
    }

    const std::string& id() const { return id_; }
    ClientSession& session() { return session_; }

    // What connect() reports of the device's own volume; unsaid when empty.
    std::optional<matp::HardwareVolumeCapability> hardwareVolume;

    // What connect() reports of the device's access mode; unsaid when empty.
    std::string accessMode;

    // The Audio Control the last SESSION_CONFIG named; empty where none.
    std::optional<matp::AudioControl> announcedAudioControl() const
    {
        return announcedAudioControl_;
    }

    // AUDIO_CAPABILITIES once more on the same session (section 12).
    void reportCapabilitiesAgain()
    {
        matp::CapabilityReport report;
        report.deviceId = id_ + "-device";
        report.deviceName = id_ + " new device";
        report.formats = formats_;
        feed(matp::MessageType::AudioCapabilities, matp::makeAudioCapabilities(report));
        answerSessionConfig();
    }

    // Section 59's report, as the client sends it once established.
    void reportVolumeConfig(matp::VolumeControlMode configured, matp::VolumeControlMode effective)
    {
        matp::VolumeConfig config;
        config.sessionId = session_.sessionId().toHex();
        config.volumeControl = configured;
        config.effective = effective;
        feed(matp::MessageType::VolumeConfig, matp::makeVolumeConfig(config));
    }

    // HELLO and AUDIO_CAPABILITIES, and the device answering whatever format
    // was announced -- which, with no zone for this room yet, is its ceiling.
    void connect()
    {
        matp::Identity identity;
        identity.clientId = id_;
        identity.clientVersion = "0.1.0";
        identity.platform = "test";
        feed(matp::MessageType::Hello, matp::makeHello(identity));

        matp::CapabilityReport report;
        report.deviceId = id_ + "-device";
        report.deviceName = id_ + " device";
        report.formats = formats_;
        report.accessMode = accessMode;
        if (hardwareVolume)
        {
            report.hasHardwareVolume = true;
            report.hardwareVolume = *hardwareVolume;
        }
        feed(matp::MessageType::AudioCapabilities, matp::makeAudioCapabilities(report));
        answerSessionConfig();
    }

    // What the listener does with a Renegotiate, and then the client's half:
    // SESSION_CONFIG and PREPARE_STREAM read, and the device opened in the
    // format that was announced.
    void renegotiate()
    {
        CHECK_OK(session_.renegotiate());
        answerSessionConfig();
    }

    // Section 19, claiming what a started room would hold from its origin.
    void reportBuffer(uint64_t fromNs, uint64_t durationNs)
    {
        matp::BufferStatus status;
        status.hasContent = true;
        status.playheadTimelineNs = fromNs;
        status.bufferedUntilTimelineNs = fromNs + durationNs;
        status.bufferedDurationNs = durationNs;
        feed(matp::MessageType::BufferStatus,
             matp::makeBufferStatus(session_.sessionId().toHex(), status));
    }

    // Section 25, naming an audible position: the acknowledgement that puts a
    // started room on the line (phase 26 flushes only those).
    void reportAudible(uint64_t timelineNs)
    {
        matp::PlaybackReport report;
        report.clockQuality = AudioClockQuality::Hardware;
        report.hasTimelineNsAudible = true;
        report.timelineNsAudible = timelineNs;
        feed(matp::MessageType::PlaybackReport,
             matp::makePlaybackReport(session_.sessionId().toHex(), report));
    }

    // Section 55, as the client's session sends it -- or any payload at all
    // under that type, for the malformed case.
    void reportSystem(const json::Value& payload) { feed(matp::MessageType::SystemReport, payload); }
    void reportSystem(const SystemMetrics& metrics)
    {
        reportSystem(matp::makeSystemReport(session_.sessionId().toHex(), metrics));
    }

    // Section 56, as the client's session sends it.
    void reportAudioDevices(const matp::AudioDeviceList& devices)
    {
        feed(matp::MessageType::AudioDevices,
             matp::makeAudioDevices(session_.sessionId().toHex(), devices));
    }

    void reportBuffer(uint32_t bufferMs)
    {
        feed(matp::MessageType::BufferConfig,
             matp::makeBufferConfig(session_.sessionId().toHex(), bufferMs));
    }

    void park(bool automatic)
    {
        matp::StreamReady ready;
        ready.standby = automatic;
        ready.reason = automatic ? "automatic standby" : "manual standby";
        feed(matp::MessageType::StreamReady,
             matp::makeStreamReady(session_.sessionId().toHex(), ready));
    }

    void reportName(const std::string& name)
    {
        feed(matp::MessageType::RoomName,
             matp::makeRoomName(session_.sessionId().toHex(), name));
    }

    void reportPowerState(const std::string& state, const std::string& wakeOnLan = std::string())
    {
        matp::StreamReady ready;
        ready.ready = state == "on";
        ready.standby = state == "standby";
        ready.powerState = state;
        ready.wakeOnLan = wakeOnLan;
        ready.format = formats_.front();
        feed(matp::MessageType::StreamReady,
             matp::makeStreamReady(session_.sessionId().toHex(), ready));
    }

    void reportInputs(const std::vector<matp::InputControl>& inputs, bool deepSleep = false)
    {
        feed(matp::MessageType::InputControls,
             matp::makeInputControls(session_.sessionId().toHex(), inputs, deepSleep));
    }

    void press(const matp::InputActionEvent& action)
    {
        feed(matp::MessageType::InputAction,
             matp::makeInputAction(session_.sessionId().toHex(), action));
    }

    AudioFormat announced() const { return announced_; }

    // What the listener does with a Volume command, and every SET_VOLUME the
    // client then reads off its connection.
    std::vector<matp::SetVolume> deliver(const matp::SetVolume& request)
    {
        CHECK_OK(session_.setVolume(request));
        matp::FrameReader reader;
        std::string frame;
        while (session_.nextOutbound(frame))
            reader.append(frame.data(), frame.size());

        std::vector<matp::SetVolume> read;
        for (;;)
        {
            matp::Message message;
            std::string error;
            if (reader.next(message, error) != matp::FrameReader::Result::Message)
                break;
            if (message.type != matp::MessageType::SetVolume)
                continue;
            matp::SetVolume volume;
            CHECK(matp::parseSetVolume(message.payload, volume, error));
            read.push_back(volume);
        }
        return read;
    }

private:
    static ClientSessionSettings settingsFor(ApiRig& rig)
    {
        ClientSessionSettings settings;
        settings.hooks = rig.api->sessionHooks();
        return settings;
    }

    void feed(matp::MessageType type, const json::Value& payload)
    {
        const std::string frame = matp::encode(type, payload);
        CHECK_OK(session_.receive(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(frame.data()), frame.size())));
    }

    void answerSessionConfig()
    {
        matp::FrameReader reader;
        std::string frame;
        while (session_.nextOutbound(frame))
            reader.append(frame.data(), frame.size());

        bool configured = false;
        for (;;)
        {
            matp::Message message;
            std::string error;
            if (reader.next(message, error) != matp::FrameReader::Result::Message)
                break;
            if (message.type != matp::MessageType::SessionConfig)
                continue;
            matp::SessionConfig config;
            CHECK(matp::parseSessionConfig(message.payload, config, error));
            announced_ = config.format;
            announcedAudioControl_ = config.hasAudioControl
                                         ? std::optional<matp::AudioControl>(config.audioControl)
                                         : std::nullopt;
            configured = true;
        }
        CHECK(configured);

        matp::StreamReady ready;
        ready.ready = opens_;
        if (opens_)
            ready.format = announced_;
        else
            ready.reason = "the device is busy";
        feed(matp::MessageType::StreamReady,
             matp::makeStreamReady(session_.sessionId().toHex(), ready));
    }

    std::string id_;
    std::vector<AudioFormat> formats_;
    bool opens_ = true;
    std::optional<matp::AudioControl> announcedAudioControl_;
    ClientSession session_;
    AudioFormat announced_;
};

// Answers every re-negotiation the API asked for since the last call, the way
// the MATP listener and the client between them would.
void answerRenegotiations(ApiRig& rig, size_t& handled, std::map<std::string, Room*> rooms)
{
    const std::vector<RoomCommand> sent = rig.log.all();
    for (; handled < sent.size(); ++handled)
    {
        if (sent[handled].kind != RoomCommandKind::Renegotiate)
            continue;
        const auto found = rooms.find(sent[handled].client.str());
        if (found != rooms.end())
            found->second->renegotiate();
    }
}

} // namespace

// ---------------------------------------------------------------------------

MULTIROOM_TEST(control_api_play_wakes_automatic_standby_only)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    Room automatic(rig, "automatic", {formatOf(kRate)}, 1);
    Room manual(rig, "manual", {formatOf(kRate)}, 2);
    Room awake(rig, "awake", {formatOf(kRate)}, 3);
    automatic.connect();
    manual.connect();
    awake.connect();
    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));
    CHECK(isOk(caller.request(R"({"command":"zone.setClients","params":{"zone":"living","clients":["automatic","manual","awake"]}})")));
    CHECK(isOk(caller.request(R"({"command":"source.stage","params":{"zone":"living","sourceId":"s1","mediaUri":"http://127.0.0.1:4714/one.flac"}})")));
    automatic.park(true);
    manual.park(false);
    const auto clients = caller.request(R"({"command":"clients"})");
    CHECK(flag(*entry(clients, "clients", "clientId", "automatic")->find("stream"), "standby"));
    CHECK(isOk(caller.request(R"({"command":"zone.play","params":{"zone":"living"}})")));
    CHECK_EQ(rig.log.count(RoomCommandKind::WakeStream, "automatic"), size_t(1));
    CHECK_EQ(rig.log.count(RoomCommandKind::WakeStream, "manual"), size_t(0));
    CHECK_EQ(rig.log.count(RoomCommandKind::WakeStream, "awake"), size_t(0));
}

MULTIROOM_TEST(control_api_answers_every_malformed_line_by_name)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());

    CHECK_EQ(errorOf(caller.request("this is not json")), std::string("invalidJson"));
    CHECK_EQ(errorOf(caller.request("[1,2,3]")), std::string("invalidJson"));
    CHECK_EQ(errorOf(caller.request(R"({"params":{}})")), std::string("invalidRequest"));
    CHECK_EQ(errorOf(caller.request(R"({"command":"zones","params":[]})")),
             std::string("invalidRequest"));

    // An unknown key is refused *with the key named*, at both levels.
    json::Value misspelt = caller.request(R"({"command":"zones","parms":{}})");
    CHECK_EQ(errorOf(misspelt), std::string("invalidRequest"));
    CHECK(text(misspelt, "message").find("parms") != std::string::npos);

    misspelt = caller.request(R"({"command":"zone.create","params":{"zone":"a","zones":"b"}})");
    CHECK_EQ(errorOf(misspelt), std::string("invalidArgument"));
    CHECK(text(misspelt, "message").find("zones") != std::string::npos);
    CHECK_EQ(errorOf(caller.request(R"({"command":"status","params":{"verbose":true}})")),
             std::string("invalidArgument"));

    // The command and the id come back even on a refusal, so a caller that
    // pipelines can match it.
    const json::Value unknown = caller.request(R"({"command":"zone.explode","id":"q7"})");
    CHECK_EQ(errorOf(unknown), std::string("unknownCommand"));
    CHECK_EQ(text(unknown, "command"), std::string("zone.explode"));
    CHECK_EQ(text(unknown, "id"), std::string("q7"));

    // A blank line is keepalive noise and gets no answer; CRLF is tolerated.
    caller.sendRaw("\n   \r\n");
    const json::Value answered = caller.request("{\"command\":\"zones\",\"id\":3}\r");
    CHECK(isOk(answered));
    int64_t id = 0;
    CHECK(json::getInt(answered, "id", id));
    CHECK_EQ(id, int64_t(3));

    // A request line past the ceiling is answered once and dropped up to its
    // newline, however much more of it arrives; the next line is served.
    LineClient flooder(rig.listener->port());
    flooder.sendRaw(std::string(70 * 1024, 'x'));
    CHECK_EQ(errorOf(flooder.readLine()), std::string("invalidRequest"));
    flooder.sendRaw(std::string(10 * 1024, 'x'));
    CHECK(isOk(flooder.request("\n{\"command\":\"zones\"}")));

    // ...and the first caller never noticed.
    CHECK(isOk(caller.request(R"({"command":"status"})")));
}

MULTIROOM_TEST(control_api_refuses_an_address_it_cannot_bind)
{
    ControlListenerSettings settings;
    settings.bindAddress = "not-an-address";
    settings.port = 0;
    CHECK_ERR(ControlListener::start(settings,
                                     [](const std::string&, bool&) { return std::string(); }),
              ControlListenerError::BindFailed);
}

MULTIROOM_TEST(control_api_creates_and_deletes_zones_idempotently)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());

    json::Value created = caller.request(R"({"command":"zone.create","params":{"zone":"living"}})");
    CHECK(isOk(created));
    CHECK(flag(created, "created"));

    // Replayed: ok, and nothing happens a second time.
    created = caller.request(R"({"command":"zone.create","params":{"zone":"living"}})");
    CHECK(isOk(created));
    CHECK(!flag(created, "created"));
    CHECK_EQ(rig.zones->size(), size_t(1));

    CHECK_EQ(errorOf(caller.request(R"({"command":"zone.create","params":{}})")),
             std::string("invalidArgument"));
    CHECK_EQ(errorOf(caller.request(R"({"command":"zone.create","params":{"zone":""}})")),
             std::string("invalidArgument"));
    CHECK_EQ(errorOf(caller.request(R"({"command":"zone.create","params":{"zone":7}})")),
             std::string("invalidArgument"));

    const json::Value zones = caller.request(R"({"command":"zones"})");
    CHECK(isOk(zones));
    const json::Value* living = entry(zones, "zones", "zone", "living");
    CHECK(living != nullptr);
    CHECK_EQ(text(*living, "state"), std::string("idle"));
    CHECK_EQ(text(*living, "transport"), std::string("stopped"));

    json::Value deleted = caller.request(R"({"command":"zone.delete","params":{"zone":"living"}})");
    CHECK(isOk(deleted));
    CHECK(flag(deleted, "deleted"));
    deleted = caller.request(R"({"command":"zone.delete","params":{"zone":"living"}})");
    CHECK(isOk(deleted));
    CHECK(!flag(deleted, "deleted"));
    CHECK_EQ(rig.zones->size(), size_t(0));
    CHECK_EQ(errorOf(caller.request(R"({"command":"zone.delete"})")),
             std::string("invalidArgument"));
}

MULTIROOM_TEST(control_api_stages_and_clears_sources_by_id)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());

    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"source.stage","params":{"zone":"nowhere","sourceId":"a","mediaUri":"http://127.0.0.1:4714/a.flac"}})")),
             std::string("notFound"));
    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"source.stage","params":{"zone":"living","sourceId":"a"}})")),
             std::string("invalidArgument"));
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"source.stage","params":{"zone":"living","sourceId":"a","mediaUri":"http://127.0.0.1:4714/a.flac","trackId":3}})")),
             std::string("invalidArgument"));

    json::Value staged = caller.request(
        R"({"command":"source.stage","params":{"zone":"living","sourceId":"a","mediaUri":"http://127.0.0.1:4714/a.flac","trackId":"t1","title":"First"}})");
    CHECK(isOk(staged));
    CHECK(flag(staged, "staged"));

    // The same segment id again is the same request replayed.
    staged = caller.request(
        R"({"command":"source.stage","params":{"zone":"living","sourceId":"a","mediaUri":"http://127.0.0.1:4714/a.flac"}})");
    CHECK(isOk(staged));
    CHECK(!flag(staged, "staged"));
    CHECK(isOk(caller.request(
        R"({"command":"source.stage","params":{"zone":"living","sourceId":"b","mediaUri":"http://127.0.0.1:4714/b.flac"}})")));

    const json::Value zones = caller.request(R"({"command":"zones"})");
    const json::Value* living = entry(zones, "zones", "zone", "living");
    CHECK(living != nullptr);
    const json::Value* list = living->find("staged");
    CHECK(list != nullptr && list->size() == 2);
    CHECK_EQ(text(list->at(0), "sourceId"), std::string("a"));
    CHECK_EQ(text(list->at(0), "title"), std::string("First"));
    CHECK_EQ(text(list->at(1), "sourceId"), std::string("b"));
    CHECK(list->at(1).find("title") == nullptr); // absent, not empty

    json::Value cleared =
        caller.request(R"({"command":"source.clearStaged","params":{"zone":"living"}})");
    CHECK(isOk(cleared));
    int64_t count = 0;
    CHECK(json::getInt(cleared, "cleared", count));
    CHECK_EQ(count, int64_t(2));
    cleared = caller.request(R"({"command":"source.clearStaged","params":{"zone":"living"}})");
    CHECK(json::getInt(cleared, "cleared", count));
    CHECK_EQ(count, int64_t(0));
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"source.clearStaged","params":{"zone":"nowhere"}})")),
             std::string("notFound"));
}

MULTIROOM_TEST(control_api_stages_media_only_from_the_configured_origin)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));

    // A path, a file: URI, another host, a look-alike host, another port and
    // a path outside the base: each refused, naming the URI and the origin.
    for (const char* uri : {"C:/music/a.flac", "/srv/music/a.flac", "file:///srv/a.flac",
                            "http://evil.example/a.flac",
                            "http://127.0.0.1:4714.evil.example/a.flac",
                            "http://127.0.0.1:4715/a.flac", "https://127.0.0.1:4714/a.flac",
                            "http://127.0.0.1:4714/../a.flac"})
    {
        json::Value request = json::Value::makeObject();
        request["command"] = json::Value("source.stage");
        json::Value params = json::Value::makeObject();
        params["zone"] = json::Value("living");
        params["sourceId"] = json::Value("a");
        params["mediaUri"] = json::Value(uri);
        request["params"] = std::move(params);

        const json::Value refused = caller.request(json::serialize(request, false));
        CHECK_EQ(errorOf(refused), std::string("invalidArgument"));
        CHECK(text(refused, "message").find(uri) != std::string::npos);
        CHECK(text(refused, "message").find("http://127.0.0.1:4714/") != std::string::npos);
    }

    // Nothing was staged by any of them.
    const json::Value zones = caller.request(R"({"command":"zones"})");
    const json::Value* living = entry(zones, "zones", "zone", "living");
    CHECK(living != nullptr);
    const json::Value* list = living->find("staged");
    CHECK(list != nullptr && list->size() == 0);
}

MULTIROOM_TEST(control_api_plays_pauses_and_stops_a_zone_with_no_rooms)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());

    CHECK_EQ(errorOf(caller.request(R"({"command":"zone.play","params":{"zone":"living"}})")),
             std::string("notFound"));
    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));
    CHECK_EQ(errorOf(caller.request(R"({"command":"zone.play","params":{"zone":"living"}})")),
             std::string("unavailable")); // nothing staged

    // A source that cannot be opened is answered to the caller that staged it.
    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"broken"}})")));
    CHECK(isOk(caller.request(
        R"({"command":"source.stage","params":{"zone":"broken","sourceId":"x","mediaUri":"http://127.0.0.1:4714/missing"}})")));
    const json::Value refused =
        caller.request(R"({"command":"zone.play","params":{"zone":"broken"}})");
    CHECK_EQ(errorOf(refused), std::string("failed"));
    CHECK(text(refused, "message").find("missing") != std::string::npos);

    // Phase 22: skipped rather than fatal, and status says which and why.
    {
        const json::Value zones = caller.request(R"({"command":"zones"})");
        const json::Value* broken = entry(zones, "zones", "zone", "broken");
        CHECK(broken != nullptr);
        CHECK_EQ(text(*broken, "state"), std::string("idle"));
        const json::Value* failed = entry(*broken, "failedSources", "sourceId", "x");
        CHECK(failed != nullptr);
        CHECK(text(*failed, "reason").find("NotFound") != std::string::npos);
    }

    CHECK(isOk(caller.request(
        R"({"command":"source.stage","params":{"zone":"living","sourceId":"a","mediaUri":"http://127.0.0.1:4714/a.flac"}})")));
    json::Value played = caller.request(R"({"command":"zone.play","params":{"zone":"living"}})");
    CHECK(isOk(played));
    CHECK_EQ(text(played, "state"), std::string("playing"));
    CHECK_EQ(text(played, "transport"), std::string("stopped")); // no room has been placed
    played = caller.request(R"({"command":"zone.play","params":{"zone":"living"}})");
    CHECK(isOk(played));

    // Section 37 needs a running line, and there is none.
    CHECK_EQ(errorOf(caller.request(R"({"command":"zone.pause","params":{"zone":"living"}})")),
             std::string("unavailable"));

    json::Value stopped = caller.request(R"({"command":"zone.stop","params":{"zone":"living"}})");
    CHECK(isOk(stopped));
    CHECK(flag(stopped, "stopped"));
    CHECK(stopped.find("timelineNs") == nullptr); // there was no line to stop on
    CHECK(rig.zones->find(ZoneId("living"))->state() == ZoneState::Idle);

    stopped = caller.request(R"({"command":"zone.stop","params":{"zone":"living"}})");
    CHECK(isOk(stopped));
    CHECK(!flag(stopped, "stopped"));
    CHECK_EQ(errorOf(caller.request(R"({"command":"zone.stop","params":{"zone":"nowhere"}})")),
             std::string("notFound"));
    CHECK_EQ(errorOf(caller.request(R"({"command":"zone.pause","params":{"zone":"nowhere"}})")),
             std::string("notFound"));
}

MULTIROOM_TEST(control_api_stores_a_room_s_settings_and_reports_why)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());

    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"client.setAudioControl","params":{"client":"kitchen","audioControl":"always"}})")),
             std::string("invalidArgument"));
    // There is no switch for whether a room is corrected
    //; the drift setting only names the
    // actuator, and is the client's to keep.
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"client.setDriftCorrection","params":{"client":"kitchen","driftCorrection":true}})")),
             std::string("invalidArgument"));
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"client.setDriftCorrection","params":{"client":"kitchen","driftCorrection":"hardwareClock"}})")),
             std::string("unavailable"));

    // A room that has never connected can be configured: it is what it will
    // be admitted with when it does.
    json::Value set = caller.request(
        R"({"command":"client.setAudioControl","params":{"client":"kitchen","audioControl":"bitPerfect"}})");
    CHECK(isOk(set));
    CHECK(flag(set, "changed"));
    set = caller.request(
        R"({"command":"client.setAudioControl","params":{"client":"kitchen","audioControl":"bitPerfect"}})");
    CHECK(!flag(set, "changed"));
    set = caller.request(
        R"({"command":"client.setGrouped","params":{"client":"kitchen","grouped":true}})");
    CHECK(isOk(set));
    CHECK(flag(set, "changed"));

    const json::Value clients = caller.request(R"({"command":"clients"})");
    const json::Value* kitchen = entry(clients, "clients", "clientId", "kitchen");
    CHECK(kitchen != nullptr);
    CHECK(!flag(*kitchen, "connected"));
    CHECK(!flag(*kitchen, "member"));

    const json::Value* settings = kitchen->find("settings");
    CHECK(settings != nullptr);
    CHECK_EQ(text(*settings, "audioControl"), std::string("bitPerfect"));
    CHECK(settings->find("driftCorrection") == nullptr);
    CHECK(!flag(*settings, "audioControlIsDefault"));
    CHECK(flag(*settings, "grouped"));

    // The question status exists for: a grouped room that will not be
    // corrected, and why.
    const json::Value* drift = kitchen->find("driftCorrection");
    CHECK(drift != nullptr);
    CHECK(flag(*drift, "requested"));
    CHECK(drift->find("active") == nullptr); // no plan yet: not known, not false
    CHECK_EQ(text(*drift, "disabledBy"),
             std::string(driftDisabledText(DriftDisabledReason::BitperfectForced)));
}

MULTIROOM_TEST(control_api_changes_the_resampler_only_while_no_zone_exists)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());

    json::Value status = caller.request(R"({"command":"status"})");
    CHECK(status.find("server")->find("resampler") == nullptr); // none, not ""

    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"resampler.configure","params":{"resampler":"fake"}})")),
             std::string("unavailable"));
    CHECK(isOk(caller.request(R"({"command":"zone.delete","params":{"zone":"living"}})")));

    const json::Value unknown =
        caller.request(R"({"command":"resampler.configure","params":{"resampler":"sox"}})");
    CHECK_EQ(errorOf(unknown), std::string("invalidArgument"));
    CHECK(text(unknown, "message").find("fake") != std::string::npos); // the ids that exist

    json::Value configured =
        caller.request(R"({"command":"resampler.configure","params":{"resampler":"fake"}})");
    CHECK(isOk(configured));
    CHECK(flag(configured, "changed"));
    CHECK_EQ(rig.planner->resamplerId(), std::string("fake"));
    configured =
        caller.request(R"({"command":"resampler.configure","params":{"resampler":"fake"}})");
    CHECK(!flag(configured, "changed"));

    status = caller.request(R"({"command":"status"})");
    CHECK_EQ(text(*status.find("server"), "resampler"), std::string("fake"));
}

MULTIROOM_TEST(control_api_reconciles_a_zone_s_whole_membership)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());

    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"zone.setClients","params":{"zone":"living","clients":[]}})")),
             std::string("notFound"));
    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"zone.setClients","params":{"zone":"living","clients":"a"}})")),
             std::string("invalidArgument"));
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"zone.setClients","params":{"zone":"living","clients":["a","a"]}})")),
             std::string("invalidArgument"));
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"zone.setClients","params":{"zone":"living","clients":["a",""]}})")),
             std::string("invalidArgument"));

    json::Value set = caller.request(
        R"({"command":"zone.setClients","params":{"zone":"living","clients":["a","b"]}})");
    CHECK(isOk(set));
    CHECK(strings(set, "added") == std::vector<std::string>({"a", "b"}));
    CHECK(strings(set, "removed").empty());

    // Desired state: the same list again is no change at all.
    set = caller.request(
        R"({"command":"zone.setClients","params":{"zone":"living","clients":["b","a"]}})");
    CHECK(strings(set, "added").empty());
    CHECK(strings(set, "removed").empty());

    set = caller.request(
        R"({"command":"zone.setClients","params":{"zone":"living","clients":["b","c"]}})");
    CHECK(strings(set, "added") == std::vector<std::string>({"c"}));
    CHECK(strings(set, "removed") == std::vector<std::string>({"a"}));

    const json::Value zones = caller.request(R"({"command":"zones"})");
    CHECK(strings(*entry(zones, "zones", "zone", "living"), "clients") ==
          std::vector<std::string>({"b", "c"}));

    // A room that was never connected and has nothing else asked of it is
    // forgotten outright rather than kept as an empty record.
    const json::Value clients = caller.request(R"({"command":"clients"})");
    CHECK(entry(clients, "clients", "clientId", "a") == nullptr);
    CHECK_EQ(text(*entry(clients, "clients", "clientId", "b"), "zone"), std::string("living"));

    // Deleting the zone takes the rooms out of it.
    CHECK(isOk(caller.request(R"({"command":"zone.delete","params":{"zone":"living"}})")));
    CHECK_EQ(rig.rooms.size(), size_t(0));
}

MULTIROOM_TEST(control_api_brings_a_zone_from_nothing_to_playing_in_two_rooms_and_back)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    size_t handled = 0;

    // Room A's device plays the source exactly; room B's does not play 44.1
    // kHz at all, so it can only be served converted.
    Room roomA(rig, "roomA", {formatOf(kRate)}, 1);
    Room roomB(rig, "roomB", {formatOf(48000, AudioFormat::SampleType::Int32)}, 2);
    const std::map<std::string, Room*> rooms{{"roomA", &roomA}, {"roomB", &roomB}};

    // Two rooms connect to a server with no zone: known, idle, and in nothing.
    roomA.connect();
    roomB.connect();
    json::Value clients = caller.request(R"({"command":"clients"})");
    CHECK(flag(*entry(clients, "clients", "clientId", "roomA"), "connected"));
    CHECK(!flag(*entry(clients, "clients", "clientId", "roomA"), "member"));

    CHECK(isOk(caller.request(R"({"command":"resampler.configure","params":{"resampler":"fake"}})")));
    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));

    // Membership before there is a source: each room is asked to choose again,
    // and there is still nothing to choose from, so neither is admitted.
    CHECK(isOk(caller.request(
        R"({"command":"zone.setClients","params":{"zone":"living","clients":["roomA","roomB"]}})")));
    CHECK_EQ(rig.log.count(RoomCommandKind::Renegotiate, "roomA"), size_t(1));
    answerRenegotiations(rig, handled, rooms);
    CHECK(rig.zones->zoneOf(ClientId("roomA")) == nullptr);

    CHECK(isOk(caller.request(
        R"({"command":"source.stage","params":{"zone":"living","sourceId":"s1","mediaUri":"http://127.0.0.1:4714/one.flac","trackId":"t1","title":"One"}})")));

    // Play: the source opens, and every room that belongs here is asked again
    // -- now there is an answer, and the devices' replies admit them.
    CHECK(isOk(caller.request(R"({"command":"zone.play","params":{"zone":"living"}})")));
    CHECK_EQ(rig.log.count(RoomCommandKind::Renegotiate, "roomB"), size_t(2));
    answerRenegotiations(rig, handled, rooms);
    CHECK(roomA.announced() == formatOf(kRate));
    CHECK(roomB.announced() == formatOf(48000, AudioFormat::SampleType::Int32));
    const std::shared_ptr<Zone> living = rig.zones->find(ZoneId("living"));
    CHECK_EQ(living->memberCount(), size_t(2));

    // Both rooms hold enough, and section 20 places them on one line.
    roomA.reportBuffer(0, 500ULL * 1000000ULL);
    roomB.reportBuffer(0, 500ULL * 1000000ULL);
    CHECK(waitFor([&] {
        return rig.log.count(RoomCommandKind::Start, "roomA") == 1 &&
               rig.log.count(RoomCommandKind::Start, "roomB") == 1;
    }));

    // Status answers the question it exists for, per room.
    const json::Value status = caller.request(R"({"command":"status"})");
    CHECK(isOk(status));
    const json::Value* zone = entry(status, "zones", "zone", "living");
    CHECK(zone != nullptr);
    CHECK_EQ(text(*zone, "state"), std::string("playing"));
    CHECK_EQ(text(*zone, "transport"), std::string("playing"));
    CHECK(strings(*zone, "members") == std::vector<std::string>({"roomA", "roomB"}));
    CHECK_EQ(text(*zone->find("source"), "title"), std::string("One"));
    CHECK(zone->find("timelineNs") != nullptr);

    const json::Value* a = entry(status, "clients", "clientId", "roomA");
    const json::Value* b = entry(status, "clients", "clientId", "roomB");
    CHECK(a != nullptr && b != nullptr);
    CHECK(flag(*a, "member"));
    CHECK(flag(*a->find("output"), "bitPerfect"));
    CHECK_EQ(text(*a->find("output"), "reason"),
             std::string(planReasonText(PlanReason::SourceFormatOffered)));
    CHECK(!flag(*b->find("output"), "bitPerfect"));
    CHECK_EQ(text(*b->find("output"), "resampler"), std::string("fake"));
    CHECK_EQ(text(*b->find("output"), "reason"),
             std::string(planReasonText(PlanReason::FormatNotOffered)));
    // Sent a start and not yet heard to play it: section 20's acknowledgement
    // is a PLAYBACK_REPORT, and these rooms have sent none.
    CHECK_EQ(text(*b, "start"), std::string("scheduled"));

    // Pause names one position, a replay names the same one, and play resumes.
    json::Value paused = caller.request(R"({"command":"zone.pause","params":{"zone":"living"}})");
    CHECK(flag(paused, "paused"));
    int64_t pausedAt = 0;
    int64_t again = -1;
    CHECK(json::getInt(paused, "timelineNs", pausedAt));
    paused = caller.request(R"({"command":"zone.pause","params":{"zone":"living"}})");
    CHECK(isOk(paused));
    CHECK(!flag(paused, "paused"));
    CHECK(json::getInt(paused, "timelineNs", again));
    CHECK_EQ(again, pausedAt);
    CHECK_EQ(rig.log.count(RoomCommandKind::Pause, "roomA"), size_t(1));

    // An idle timer may park just one room during a long pause. Resuming
    // wakes that room while the others retain their prepared audio devices.
    roomA.park(true);
    const json::Value resumed =
        caller.request(R"({"command":"zone.play","params":{"zone":"living"}})");
    CHECK_EQ(text(resumed, "transport"), std::string("starting"));
    CHECK_EQ(rig.log.count(RoomCommandKind::WakeStream, "roomA"), size_t(1));
    CHECK_EQ(rig.log.count(RoomCommandKind::WakeStream, "roomB"), size_t(0));

    // And back: stopped, emptied, deleted -- with both rooms still connected
    // and idle, which is what a room with no zone is.
    const json::Value stopped =
        caller.request(R"({"command":"zone.stop","params":{"zone":"living"}})");
    CHECK(flag(stopped, "stopped"));
    CHECK(living->state() == ZoneState::Idle);

    const json::Value emptied = caller.request(
        R"({"command":"zone.setClients","params":{"zone":"living","clients":[]}})");
    CHECK(strings(emptied, "removed") == std::vector<std::string>({"roomA", "roomB"}));
    CHECK_EQ(living->memberCount(), size_t(0));

    CHECK(flag(caller.request(R"({"command":"zone.delete","params":{"zone":"living"}})"),
               "deleted"));
    clients = caller.request(R"({"command":"clients"})");
    const json::Value* idle = entry(clients, "clients", "clientId", "roomB");
    CHECK(idle != nullptr);
    CHECK(flag(*idle, "connected"));
    CHECK(idle->find("zone") == nullptr);
    CHECK(!flag(*idle, "member"));
    CHECK(strings(caller.request(R"({"command":"zones"})"), "zones").empty());
}

// audio.configure: validated by the config's own rules through the persist
// hook, written back, and live in every zone.
MULTIROOM_TEST(control_api_reads_and_changes_the_live_audio_settings)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());

    const char* const toFaster =
        R"({"command":"audio.configure","params":{"startDelayMs":500}})";
    CHECK_EQ(errorOf(caller.request(R"({"command":"audio.settings"})")), std::string("unavailable"));
    CHECK_EQ(errorOf(caller.request(toFaster)), std::string("unavailable"));

    AudioConfig saved;
    int saves = 0;
    rig.api->setAudioConfig(AudioConfig{}, [&](const AudioConfig& audio, std::string& error) {
        ServerConfig candidate;
        candidate.audio = audio;
        if (auto valid = server_config::validate(candidate); !valid)
        {
            error = describe(valid.error());
            return false;
        }
        saved = audio;
        ++saves;
        return true;
    });

    json::Value settings = caller.request(R"({"command":"audio.settings"})");
    CHECK(isOk(settings));
    CHECK_EQ(settings.find("audio")->find("startDelayMs")->asInt(), int64_t(1000));
    CHECK_EQ(settings.find("live")->size(), size_t(6));

    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));
    json::Value answer = caller.request(toFaster);
    CHECK(isOk(answer));
    CHECK(flag(answer, "changed"));
    CHECK_EQ(answer.find("audio")->find("startDelayMs")->asInt(), int64_t(500));
    CHECK_EQ(saved.startDelayMs, uint32_t(500));
    CHECK_EQ(rig.zones->timing().startDelayNs, uint64_t(500) * 1000000ULL);

    // The config's rules: below 20 ms is refused, and so is anything a
    // startup-only key would need; nothing changes and nothing is saved.
    CHECK_EQ(errorOf(caller.request(R"({"command":"audio.configure","params":{"startDelayMs":5}})")),
             std::string("invalidArgument"));
    CHECK_EQ(errorOf(caller.request(R"({"command":"audio.configure","params":{"targetBufferMs":900}})")),
             std::string("invalidArgument"));
    CHECK_EQ(errorOf(caller.request(R"({"command":"audio.configure","params":{}})")),
             std::string("invalidArgument"));
    CHECK_EQ(saves, 1);
    CHECK_EQ(rig.zones->timing().startDelayNs, uint64_t(500) * 1000000ULL);
}

MULTIROOM_TEST(control_api_checks_and_changes_the_audio_memory_limit_while_zones_play)
{
    // Phase 42: memoryLimitMb is live, and the core checks every value
    // whoever sends it -- against one zone at the ceiling, against what the
    // playing zones hold, and against this machine.
    ApiRig rig;
    LineClient caller(rig.listener->port());

    AudioConfig saved;
    int saves = 0;
    rig.api->setAudioConfig(AudioConfig{}, [&](const AudioConfig& audio, std::string& error) {
        ServerConfig candidate;
        candidate.audio = audio;
        if (auto valid = server_config::validate(candidate); !valid)
        {
            error = describe(valid.error());
            return false;
        }
        saved = audio;
        ++saves;
        return true;
    });

    // A zone that exists costs nothing; one that plays holds its blocks.
    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));
    CHECK_EQ(rig.memory.usedBytes(), 0ULL);
    CHECK(isOk(caller.request(
        R"({"command":"source.stage","params":{"zone":"living","sourceId":"s1","mediaUri":"http://127.0.0.1:4714/one.flac"}})")));
    CHECK(isOk(caller.request(R"({"command":"zone.play","params":{"zone":"living"}})")));
    const uint64_t held = (rig.memory.usedBytes() + 1024 * 1024 - 1) / (1024 * 1024);
    CHECK(held > 8);

    const json::Value status = caller.request(R"({"command":"status"})");
    const json::Value* living = entry(status, "zones", "zone", "living");
    CHECK(living != nullptr);
    if (living != nullptr)
    {
        CHECK(living->find("blocks")->asInt() > 0);
        CHECK_EQ(static_cast<uint64_t>(living->find("memoryBytes")->asInt()),
                 rig.memory.usedBytes());
    }
    CHECK_EQ(static_cast<uint64_t>(status.find("server")->find("memoryUsedBytes")->asInt()),
             rig.memory.usedBytes());

    // The range, before anything is sent.
    json::Value settings = caller.request(R"({"command":"audio.settings"})");
    const json::Value* limit = settings.find("memoryLimit");
    CHECK(limit != nullptr);
    if (limit != nullptr)
    {
        CHECK_EQ(limit->find("mb")->asInt(), int64_t(64));
        CHECK_EQ(limit->find("minMb")->asInt(), static_cast<int64_t>(held));
        CHECK_EQ(limit->find("usedMb")->asInt(), static_cast<int64_t>(held));
        CHECK(limit->find("maxMb") != nullptr);
    }

    // Each refusal names the bound it broke, and nothing is saved.
    const auto refusal = [&](uint64_t mb) {
        const json::Value answer = caller.request(
            R"({"command":"audio.configure","params":{"memoryLimitMb":)" + std::to_string(mb) +
            "}}");
        CHECK_EQ(errorOf(answer), std::string("invalidArgument"));
        return text(answer, "message");
    };
    CHECK(refusal(7).find("one zone at") != std::string::npos);
    CHECK(refusal(held - 1).find("what the playing zones hold") != std::string::npos);
    CHECK(refusal(4ULL * 1024 * 1024).find("half this machine") != std::string::npos);
    CHECK_EQ(saves, 0);

    // A lower limit inside the range is written, answered with the range
    // recomputed, and stops nothing that plays.
    json::Value answer = caller.request(
        R"({"command":"audio.configure","params":{"memoryLimitMb":)" + std::to_string(held) + "}}");
    CHECK(isOk(answer));
    CHECK(flag(answer, "changed"));
    CHECK_EQ(saved.memoryLimitMb, static_cast<uint32_t>(held));
    CHECK_EQ(rig.memory.limitBytes(), held * 1024 * 1024);
    CHECK_EQ(answer.find("memoryLimit")->find("mb")->asInt(), static_cast<int64_t>(held));
    CHECK(rig.zones->find(ZoneId("living"))->state() == ZoneState::Playing);
    CHECK_EQ(saves, 1);
}

MULTIROOM_TEST(control_api_moves_a_room_whose_settings_changed)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    size_t handled = 0;

    Room room(rig, "roomA", {formatOf(kRate), formatOf(48000)}, 1);
    const std::map<std::string, Room*> rooms{{"roomA", &room}};
    room.connect();

    CHECK(isOk(caller.request(R"({"command":"resampler.configure","params":{"resampler":"fake"}})")));
    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));
    CHECK(isOk(caller.request(
        R"({"command":"source.stage","params":{"zone":"living","sourceId":"s1","mediaUri":"http://127.0.0.1:4714/one.flac"}})")));
    CHECK(isOk(caller.request(R"({"command":"zone.play","params":{"zone":"living"}})")));
    CHECK(isOk(caller.request(
        R"({"command":"zone.setClients","params":{"zone":"living","clients":["roomA"]}})")));
    answerRenegotiations(rig, handled, rooms);

    // auto, alone, on a device that plays the source exactly: bitperfect, and
    // nothing to correct.
    const std::shared_ptr<Zone> living = rig.zones->find(ZoneId("living"));
    CHECK(living->member(ClientId("roomA")) != nullptr);
    CHECK(living->member(ClientId("roomA"))->plan().mode == OutputMode::BitPerfect);
    json::Value clients = caller.request(R"({"command":"clients"})");
    const json::Value* drift = entry(clients, "clients", "clientId", "roomA")->find("driftCorrection");
    CHECK(!flag(*drift, "requested"));
    CHECK(!flag(*drift, "active"));
    CHECK(drift->find("disabledBy") == nullptr);

    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"client.setGrouped","params":{"client":"roomA","grouped":"yes"}})")),
             std::string("invalidArgument"));

    // Grouped while it plays: resampled and corrected from the next packet on
    // the stream it already has, in the format its device already plays --
    // not taken out of the zone, not asked to choose again.
    const std::shared_ptr<ClientOutput> playing = living->member(ClientId("roomA"));
    size_t asked = rig.log.count(RoomCommandKind::Renegotiate, "roomA");
    json::Value set = caller.request(
        R"({"command":"client.setGrouped","params":{"client":"roomA","grouped":true}})");
    CHECK(isOk(set));
    CHECK(flag(set, "changed"));
    CHECK(living->member(ClientId("roomA")) == playing);
    CHECK(waitFor([&] { return playing->formatSwitches() == 1; }));
    CHECK_EQ(rig.log.count(RoomCommandKind::Renegotiate, "roomA"), asked);
    CHECK(playing->plan().mode == OutputMode::Resampled);
    CHECK(playing->plan().outputFormat == playing->plan().sourceFormat);
    CHECK(living->member(ClientId("roomA"))->plan().driftCorrected());
    CHECK(living->member(ClientId("roomA"))->plan().reason == PlanReason::Grouped);
    clients = caller.request(R"({"command":"clients"})");
    drift = entry(clients, "clients", "clientId", "roomA")->find("driftCorrection");
    CHECK(flag(*drift, "requested"));
    CHECK(flag(*drift, "active"));

    // Replayed: no change, and no re-negotiation either.
    asked = rig.log.count(RoomCommandKind::Renegotiate, "roomA");
    set = caller.request(
        R"({"command":"client.setGrouped","params":{"client":"roomA","grouped":true}})");
    CHECK(!flag(set, "changed"));
    CHECK_EQ(rig.log.count(RoomCommandKind::Renegotiate, "roomA"), asked);

    // bitPerfect opts out, grouped or not: bitperfect again, and status says
    // why it is not corrected.
    set = caller.request(
        R"({"command":"client.setAudioControl","params":{"client":"roomA","audioControl":"bitPerfect"}})");
    CHECK(flag(set, "changed"));
    answerRenegotiations(rig, handled, rooms);
    CHECK(living->member(ClientId("roomA"))->plan().mode == OutputMode::BitPerfect);
    clients = caller.request(R"({"command":"clients"})");
    drift = entry(clients, "clients", "clientId", "roomA")->find("driftCorrection");
    CHECK(flag(*drift, "requested"));
    CHECK(!flag(*drift, "active"));
    CHECK_EQ(text(*drift, "disabledBy"),
             std::string(driftDisabledText(DriftDisabledReason::BitperfectForced)));

    // Grouping cannot change a bitPerfect room's plan, so it does not
    // interrupt it.
    asked = rig.log.count(RoomCommandKind::Renegotiate, "roomA");
    set = caller.request(
        R"({"command":"client.setGrouped","params":{"client":"roomA","grouped":false}})");
    CHECK(flag(set, "changed"));
    CHECK_EQ(rig.log.count(RoomCommandKind::Renegotiate, "roomA"), asked);
}

// ---------------------------------------------------------------------------
// Phase 25: volume and a forced rejoin. That a volume change leaves a
// bitperfect room's bytes alone, and that a rejoin leaves the other rooms'
// packet streams unbroken, is asserted on the packets in test_zone.cpp; these
// are about what the command reaches, what it keeps and what it refuses.

namespace {

// The Volume commands sent to `client` so far, in order.
std::vector<matp::SetVolume> volumesSent(const ApiRig& rig, const std::string& client)
{
    std::vector<matp::SetVolume> out;
    for (const RoomCommand& command : rig.log.all())
    {
        if (command.kind == RoomCommandKind::Volume && command.client.str() == client)
            out.push_back(command.volume);
    }
    return out;
}

} // namespace

MULTIROOM_TEST(control_api_keeps_a_room_s_volume_and_sends_it_on_every_admission)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    size_t handled = 0;

    const char* refused[] = {
        R"({"command":"client.setVolume","params":{"client":"roomA"}})",
        R"({"command":"client.setVolume","params":{"client":"roomA","value":1.5}})",
        R"({"command":"client.setVolume","params":{"client":"roomA","value":"loud"}})",
        R"({"command":"client.setVolume","params":{"client":"roomA","muted":"yes"}})",
        R"({"command":"client.setVolume","params":{"client":"roomA","value":0.5,"applyAtTimelineNs":-1}})",
        R"({"command":"client.setVolume","params":{"client":"roomA","value":0.5,"gain":2}})",
    };
    for (const char* line : refused)
        CHECK_EQ(errorOf(caller.request(line)), std::string("invalidArgument"));

    // A position means something only on a line, and this room is on none.
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"client.setVolume","params":{"client":"roomA","value":0.5,"applyAtTimelineNs":0}})")),
             std::string("unavailable"));

    // Before the room is anywhere: stored, reported, and sent to nobody.
    json::Value set =
        caller.request(R"({"command":"client.setVolume","params":{"client":"roomA","value":0.5}})");
    CHECK(isOk(set));
    CHECK(flag(set, "changed"));
    CHECK(set.find("muted") == nullptr);
    json::Value clients = caller.request(R"({"command":"clients"})");
    const json::Value* settings = entry(clients, "clients", "clientId", "roomA")->find("settings");
    double stored = 0.0;
    CHECK(json::getDouble(*settings, "volume", stored));
    CHECK_EQ(stored, 0.5);
    CHECK(settings->find("muted") == nullptr); // never asked, so not reported
    CHECK(volumesSent(rig, "roomA").empty());

    Room room(rig, "roomA", {formatOf(kRate)}, 1);
    const std::map<std::string, Room*> rooms{{"roomA", &room}};
    room.connect();
    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));
    CHECK(isOk(caller.request(
        R"({"command":"source.stage","params":{"zone":"living","sourceId":"s1","mediaUri":"http://127.0.0.1:4714/one.flac"}})")));
    CHECK(isOk(caller.request(R"({"command":"zone.play","params":{"zone":"living"}})")));
    CHECK(isOk(caller.request(
        R"({"command":"zone.setClients","params":{"zone":"living","clients":["roomA"]}})")));
    answerRenegotiations(rig, handled, rooms);
    const std::shared_ptr<Zone> living = rig.zones->find(ZoneId("living"));
    CHECK(living->member(ClientId("roomA")) != nullptr);

    // Admitted, and sent what was stored -- once, and only what was asked.
    std::vector<matp::SetVolume> sent = volumesSent(rig, "roomA");
    CHECK_EQ(sent.size(), size_t(1));
    CHECK(sent.back().hasValue && !sent.back().hasMuted && !sent.back().hasApplyAtTimelineNs);
    CHECK_EQ(sent.back().value, 0.5);

    room.reportBuffer(0, 500ULL * 1000000ULL);
    CHECK(waitFor([&] { return rig.log.count(RoomCommandKind::Start, "roomA") == 1; }));

    // On a line: forwarded as asked, and read by a real session as one
    // SET_VOLUME carrying the value, the mute and the position.
    set = caller.request(
        R"({"command":"client.setVolume","params":{"client":"roomA","value":0.25,"muted":true,"applyAtTimelineNs":5000000000}})");
    CHECK(flag(set, "changed"));
    CHECK(flag(set, "muted"));
    sent = volumesSent(rig, "roomA");
    CHECK_EQ(sent.size(), size_t(2));
    const std::vector<matp::SetVolume> read = room.deliver(sent.back());
    CHECK_EQ(read.size(), size_t(1));
    CHECK(read.front().hasValue && read.front().hasMuted && read.front().hasApplyAtTimelineNs);
    CHECK_NEAR(read.front().value, 0.005623413, 1e-9); // the position 0.25 as a gain: -45 dB
    CHECK(read.front().muted);
    CHECK_EQ(read.front().applyAtTimelineNs, uint64_t{5000000000});
    CHECK_EQ(read.front().sessionId, room.session().sessionId().toHex());

    // Replayed: nothing changes and nothing is sent.
    set = caller.request(
        R"({"command":"client.setVolume","params":{"client":"roomA","value":0.25,"muted":true,"applyAtTimelineNs":5000000000}})");
    CHECK(isOk(set));
    CHECK(!flag(set, "changed"));
    CHECK_EQ(volumesSent(rig, "roomA").size(), size_t(2));

    // Gone: a change is kept rather than sent, and survives the disconnect.
    room.session().close(kClosedByPeer);
    CHECK(living->member(ClientId("roomA")) == nullptr);
    set = caller.request(R"({"command":"client.setVolume","params":{"client":"roomA","value":0.75}})");
    CHECK(flag(set, "changed"));
    CHECK_EQ(volumesSent(rig, "roomA").size(), size_t(2));

    // Back on a new session: admitted, and put back as it was asked to be,
    // once -- the value changed while it was away and the mute from before.
    Room back(rig, "roomA", {formatOf(kRate)}, 2);
    back.connect();
    CHECK(living->member(ClientId("roomA")) != nullptr);
    sent = volumesSent(rig, "roomA");
    CHECK_EQ(sent.size(), size_t(3));
    CHECK(sent.back().hasValue && sent.back().hasMuted && !sent.back().hasApplyAtTimelineNs);
    CHECK_EQ(sent.back().value, 0.75);
    CHECK(sent.back().muted);
    CHECK_EQ(back.deliver(sent.back()).size(), size_t(1));
}

MULTIROOM_TEST(control_api_rejoins_one_room_of_a_playing_zone_and_refuses_a_parked_one)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    LineClient feed(rig.listener->port());
    size_t handled = 0;

    Room roomA(rig, "roomA", {formatOf(kRate)}, 1);
    Room roomB(rig, "roomB", {formatOf(kRate)}, 2);
    const std::map<std::string, Room*> rooms{{"roomA", &roomA}, {"roomB", &roomB}};
    roomA.connect();
    roomB.connect();

    CHECK_EQ(errorOf(caller.request(R"({"command":"client.rejoin","params":{"client":"roomA"}})")),
             std::string("unavailable")); // in no zone
    CHECK_EQ(errorOf(caller.request(R"({"command":"client.rejoin","params":{}})")),
             std::string("invalidArgument"));

    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));
    CHECK(isOk(caller.request(
        R"({"command":"source.stage","params":{"zone":"living","sourceId":"s1","mediaUri":"http://127.0.0.1:4714/one.flac"}})")));
    CHECK(isOk(caller.request(R"({"command":"zone.play","params":{"zone":"living"}})")));
    CHECK(isOk(caller.request(
        R"({"command":"zone.setClients","params":{"zone":"living","clients":["roomA","roomB"]}})")));
    answerRenegotiations(rig, handled, rooms);
    const std::shared_ptr<Zone> living = rig.zones->find(ZoneId("living"));
    CHECK_EQ(living->memberCount(), size_t(2));

    roomA.reportBuffer(0, 500ULL * 1000000ULL);
    roomB.reportBuffer(0, 500ULL * 1000000ULL);
    CHECK(waitFor([&] {
        return rig.log.count(RoomCommandKind::Start, "roomA") == 1 &&
               rig.log.count(RoomCommandKind::Start, "roomB") == 1;
    }));
    const std::shared_ptr<ClientOutput> untouched = living->member(ClientId("roomB"));

    CHECK(isOk(feed.request(R"({"command":"events.subscribe"})")));
    const json::Value moved =
        caller.request(R"({"command":"client.rejoin","params":{"client":"roomA"}})");
    CHECK(isOk(moved));
    CHECK_EQ(text(moved, "zone"), std::string("living"));
    int64_t movedTo = -1;
    CHECK(json::getInt(moved, "timelineNs", movedTo));
    CHECK(movedTo > 0); // a position ahead of the playhead, never back

    // Said as the fall behind it was, with the trigger that asked for it.
    bool announced = false;
    while (const std::optional<std::string> line = feed.readRawWithin(std::chrono::milliseconds(200)))
    {
        json::Value event;
        std::string error;
        CHECK(json::parse(*line, event, error));
        if (text(event, "event") == "room.fellBehind")
        {
            CHECK_EQ(text(event, "client"), std::string("roomA"));
            CHECK_EQ(text(event, "trigger"), std::string("hardResync"));
            int64_t at = -1;
            CHECK(json::getInt(event, "timelineNs", at));
            CHECK_EQ(at, movedTo);
            announced = true;
        }
    }
    CHECK(announced);

    // One room moved and the other not touched: the same stream, no move, no
    // second start.
    CHECK_EQ(living->rejoins(ClientId("roomA")), 1ULL);
    CHECK_EQ(living->rejoins(ClientId("roomB")), 0ULL);
    CHECK(living->member(ClientId("roomB")) == untouched);
    CHECK(untouched->state() == OutputState::Streaming);
    CHECK_EQ(rig.log.count(RoomCommandKind::Start, "roomB"), size_t(1));

    // A room a stop parked is Zone::rejoin's refusal, and says so by name.
    CHECK(isOk(caller.request(R"({"command":"zone.stop","params":{"zone":"living"}})")));
    CHECK(waitFor([&] {
        return living->member(ClientId("roomB"))->state() == OutputState::Parked;
    }));
    const json::Value parked =
        caller.request(R"({"command":"client.rejoin","params":{"client":"roomB"}})");
    CHECK_EQ(errorOf(parked), std::string("unavailable"));
    CHECK(text(parked, "message").find(outputStateName(OutputState::Parked)) != std::string::npos);
    CHECK_EQ(living->rejoins(ClientId("roomB")), 0ULL);
}

// ---------------------------------------------------------------------------
// Phase 24: events on the connection that asked for them. The zone half of
// each event -- when it fires -- is test_zone.cpp's; these are about who is
// sent what, in what order, and what a caller that stops reading costs.

namespace {

// The next line on `feed`, which must be the event `name`.
json::Value expectEvent(LineClient& feed, const std::string& name)
{
    const json::Value line = feed.readLine();
    const std::string got = text(line, "event");
    if (got != name)
        multiroomtest::failAt("expected " + name + ", got " + json::serialize(line, false), __FILE__,
                        __LINE__);
    return line;
}

bool hasKey(const json::Value& object, const char* key)
{
    return object.find(key) != nullptr;
}

} // namespace

MULTIROOM_TEST(control_api_reports_power_changes_in_status_and_live_events)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    Room room(rig, "power-room", {formatOf(kRate)}, 1);
    room.connect();
    const auto older = caller.request(R"({"command":"clients"})");
    CHECK(entry(older, "clients", "clientId", "power-room")->find("stream")->find("powerState") == nullptr);
    LineClient events(rig.listener->port());
    CHECK(isOk(events.request(R"({"command":"events.subscribe"})")));
    for (const std::string state : {"on", "standby", "standbyManual", "on"})
    {
        room.reportPowerState(state);
        const auto event = expectEvent(events, "room.powerState");
        CHECK_EQ(text(event, "client"), std::string("power-room"));
        CHECK_EQ(text(event, "powerState"), state);
        const auto clients = caller.request(R"({"command":"clients"})");
        CHECK_EQ(text(*entry(clients, "clients", "clientId", "power-room")->find("stream"), "powerState"), state);
    }
    room.session().close(kClosedByPeer);
    const auto closed = caller.request(R"({"command":"clients"})");
    const auto record = entry(closed, "clients", "clientId", "power-room");
    CHECK(record == nullptr || record->find("stream") == nullptr);
    CHECK(record == nullptr || record->find("powerState") == nullptr);
}

MULTIROOM_TEST(control_api_keeps_an_announced_deep_sleep_through_the_disconnect)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    Room room(rig, "sleep-room", {formatOf(kRate)}, 1);
    room.connect();
    room.reportPowerState("sleepButtonOnly");
    const auto asleep = caller.request(R"({"command":"clients"})");
    CHECK_EQ(text(*entry(asleep, "clients", "clientId", "sleep-room")->find("stream"), "powerState"),
             std::string("sleepButtonOnly"));
    room.session().close(kClosedByPeer);
    const auto away = caller.request(R"({"command":"clients"})");
    const auto record = entry(away, "clients", "clientId", "sleep-room");
    CHECK(record != nullptr);
    CHECK(record->find("stream") == nullptr);
    CHECK_EQ(text(*record, "powerState"), std::string("sleepButtonOnly"));
    CHECK(record->find("wakeOnLan") == nullptr);
    // Only POWER ends this one.
    CHECK_EQ(errorOf(caller.request(R"({"command":"client.wake","params":{"client":"sleep-room"}})")),
             std::string("unavailable"));
}

MULTIROOM_TEST(control_api_keeps_the_wake_address_of_a_sleeping_room_and_wakes_it)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    Room room(rig, "lan-room", {formatOf(kRate)}, 1);
    room.connect();
    CHECK_EQ(errorOf(caller.request(R"({"command":"client.wake","params":{"client":"lan-room"}})")),
             std::string("unavailable"));
    // Locally administered: no real device answers to it.
    room.reportPowerState("sleep", "02:00:00:00:00:01");
    room.session().close(kClosedByPeer);
    const auto away = caller.request(R"({"command":"clients"})");
    const auto record = entry(away, "clients", "clientId", "lan-room");
    CHECK(record != nullptr);
    CHECK_EQ(text(*record, "powerState"), std::string("sleep"));
    CHECK_EQ(text(*record, "wakeOnLan"), std::string("02:00:00:00:00:01"));
    const auto woken = caller.request(R"({"command":"client.wake","params":{"client":"lan-room"}})");
    CHECK(isOk(woken));
    CHECK_EQ(text(woken, "wakeOnLan"), std::string("02:00:00:00:00:01"));
    CHECK_EQ(errorOf(caller.request(R"({"command":"client.wake","params":{"client":"nobody"}})")),
             std::string("notFound"));
}

MULTIROOM_TEST(control_api_sends_events_only_to_a_connection_that_asked_for_them)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    LineClient feed(rig.listener->port());

    json::Value answer = feed.request(R"({"command":"events.subscribe"})");
    CHECK(isOk(answer));
    CHECK(flag(answer, "subscribed"));
    CHECK(flag(answer, "changed"));
    answer = feed.request(R"({"command":"events.subscribe"})");
    CHECK(isOk(answer));
    CHECK(!flag(answer, "changed")); // replayed, like every other command
    CHECK_EQ(errorOf(feed.request(R"({"command":"events.subscribe","params":{"zone":"x"}})")),
             std::string("invalidArgument"));

    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));
    CHECK(isOk(caller.request(
        R"({"command":"source.stage","params":{"zone":"living","sourceId":"s1","mediaUri":"http://127.0.0.1:4714/one.flac"}})")));
    CHECK(isOk(caller.request(R"({"command":"zone.play","params":{"zone":"living"}})")));

    // The subscriber is told; the caller that did not ask is not, and its
    // next line is the answer to its next request.
    const json::Value needed = expectEvent(feed, "zone.sourceNeeded");
    CHECK_EQ(text(needed, "zone"), std::string("living"));
    CHECK_EQ(text(needed, "afterSourceId"), std::string("s1"));
    int64_t remaining = 0;
    CHECK(json::getInt(needed, "remainingNs", remaining));
    CHECK_EQ(remaining, int64_t{3} * static_cast<int64_t>(kNanosecondsPerSecond));
    CHECK(!hasKey(needed, "ok"));
    CHECK(isOk(caller.request(R"({"command":"zones"})")));

    // Unsubscribed, the same question goes to nobody.
    answer = feed.request(R"({"command":"events.unsubscribe"})");
    CHECK(flag(answer, "changed"));
    CHECK(!flag(answer, "subscribed"));
    CHECK(isOk(caller.request(
        R"({"command":"source.stage","params":{"zone":"living","sourceId":"s2","mediaUri":"http://127.0.0.1:4714/two.flac"}})")));
    CHECK(isOk(caller.request(R"({"command":"source.clearStaged","params":{"zone":"living"}})")));
    answer = feed.request(R"({"command":"zones"})");
    CHECK(isOk(answer));
    CHECK_EQ(text(answer, "command"), std::string("zones"));
}

MULTIROOM_TEST(control_api_announces_rooms_as_they_come_join_move_fall_silent_and_go)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    LineClient feed(rig.listener->port());
    CHECK(isOk(feed.request(R"({"command":"events.subscribe"})")));
    size_t handled = 0;

    // Connected, and in no zone yet: the first moment the core knows who it
    // is and what its device can do.
    Room first(rig, "roomA", {formatOf(kRate)}, 1);
    first.connect();
    json::Value event = expectEvent(feed, "room.connected");
    CHECK_EQ(text(event, "client"), std::string("roomA"));
    CHECK_EQ(text(event, "deviceName"), std::string("roomA device"));
    CHECK(!flag(event, "reconnect"));
    CHECK(!hasKey(event, "zone"));

    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));
    CHECK(isOk(caller.request(
        R"({"command":"source.stage","params":{"zone":"living","sourceId":"s1","mediaUri":"http://127.0.0.1:4714/one.flac"}})")));
    CHECK(isOk(caller.request(R"({"command":"zone.play","params":{"zone":"living"}})")));
    expectEvent(feed, "zone.sourceNeeded");

    // Joined: admitted and streaming, with the plan it was given.
    CHECK(isOk(caller.request(
        R"({"command":"zone.setClients","params":{"zone":"living","clients":["roomA"]}})")));
    answerRenegotiations(rig, handled, {{"roomA", &first}});
    event = expectEvent(feed, "room.joined");
    CHECK_EQ(text(event, "client"), std::string("roomA"));
    CHECK_EQ(text(event, "zone"), std::string("living"));
    CHECK(flag(event, "bitPerfect"));
    CHECK_EQ(text(event, "reason"),
             std::string(planReasonText(PlanReason::SourceFormatOffered)));
    CHECK(!flag(event, "driftCorrected"));
    CHECK(hasKey(event, "outputFormat"));

    // A cut: disconnected with the zone it keeps a place in, and back as a
    // return rather than a stranger.
    first.session().close(kClosedByPeer);
    event = expectEvent(feed, "room.disconnected");
    CHECK_EQ(text(event, "reason"), std::string("closed"));
    CHECK_EQ(text(event, "zone"), std::string("living"));

    Room back(rig, "roomA", {formatOf(kRate)}, 2);
    back.connect();
    event = expectEvent(feed, "room.connected");
    CHECK(flag(event, "reconnect"));
    CHECK_EQ(text(event, "zone"), std::string("living"));
    CHECK_EQ(text(expectEvent(feed, "room.joined"), "zone"), std::string("living"));

    // Moved: out of the first zone, then into the second.
    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"kitchen"}})")));
    CHECK(isOk(caller.request(
        R"({"command":"source.stage","params":{"zone":"kitchen","sourceId":"k1","mediaUri":"http://127.0.0.1:4714/k.flac"}})")));
    CHECK(isOk(caller.request(R"({"command":"zone.play","params":{"zone":"kitchen"}})")));
    expectEvent(feed, "zone.sourceNeeded");
    CHECK(isOk(caller.request(
        R"({"command":"zone.setClients","params":{"zone":"kitchen","clients":["roomA"]}})")));
    event = expectEvent(feed, "room.left");
    CHECK_EQ(text(event, "zone"), std::string("living"));
    CHECK_EQ(text(event, "cause"), std::string("moved"));
    answerRenegotiations(rig, handled, {{"roomA", &back}});
    CHECK_EQ(text(expectEvent(feed, "room.joined"), "zone"), std::string("kitchen"));

    // A device that will not open: in the zone it was asked into, and silent.
    Room busy(rig, "roomB", {formatOf(kRate)}, 3, false);
    busy.connect();
    expectEvent(feed, "room.connected");
    CHECK(isOk(caller.request(
        R"({"command":"zone.setClients","params":{"zone":"kitchen","clients":["roomA","roomB"]}})")));
    answerRenegotiations(rig, handled, {{"roomA", &back}, {"roomB", &busy}});
    event = expectEvent(feed, "room.notReady");
    CHECK_EQ(text(event, "client"), std::string("roomB"));
    CHECK_EQ(text(event, "zone"), std::string("kitchen"));
    CHECK_EQ(text(event, "reason"), std::string("the device is busy"));

    // Removed, and then a zone deleted with a room in it.
    CHECK(isOk(caller.request(
        R"({"command":"zone.setClients","params":{"zone":"kitchen","clients":["roomB"]}})")));
    event = expectEvent(feed, "room.left");
    CHECK_EQ(text(event, "client"), std::string("roomA"));
    CHECK_EQ(text(event, "cause"), std::string("removed"));
    CHECK(isOk(caller.request(
        R"({"command":"zone.setClients","params":{"zone":"living","clients":["roomA"]}})")));
    answerRenegotiations(rig, handled, {{"roomA", &back}, {"roomB", &busy}});
    expectEvent(feed, "room.joined");
    CHECK(isOk(caller.request(R"({"command":"zone.delete","params":{"zone":"living"}})")));
    event = expectEvent(feed, "room.left");
    CHECK_EQ(text(event, "zone"), std::string("living"));
    CHECK_EQ(text(event, "cause"), std::string("deleted"));

    // Displaced: the listener closes the old session as the new one says
    // HELLO, before the new one has reported anything -- and only the
    // session the room was known by counts as it going.
    back.session().close(kReplacedByReconnect);
    CHECK_EQ(text(expectEvent(feed, "room.disconnected"), "reason"), std::string("displaced"));
    first.session().close(kClosedByPeer); // closed long ago: says nothing again
    CHECK(isOk(feed.request(R"({"command":"zones"})")));
}

MULTIROOM_TEST(control_api_a_subscriber_that_stops_reading_loses_events_and_not_memory)
{
    // The listener alone, with a handler that subscribes whoever asks: the
    // bound belongs to it, not to any command.
    auto started = ControlListener::start({"127.0.0.1", 0},
                                          [](const std::string& line, bool& subscribed) {
                                              subscribed = subscribed || line == "subscribe";
                                              return std::string("{\"ok\":true}\n");
                                          });
    CHECK_OK(started);
    ControlListener& listener = **started;

    // A small receive window, so the stalled reader's side fills in bytes
    // rather than in minutes.
    LineClient stalled(listener.port(), 8 * 1024);
    CHECK(isOk(stalled.request("subscribe")));

    // Published from this thread while nobody reads: every call returns,
    // because none of them waits on a socket.
    constexpr int kPublished = 20000;
    const std::string padding(512, 'x');
    for (int n = 0; n < kPublished; ++n)
    {
        listener.publish("{\"event\":\"test\",\"n\":" + std::to_string(n) + ",\"pad\":\"" +
                         padding + "\"}\n");
    }

    // Another caller is answered at once meanwhile: the stalled one costs the
    // listener nothing but its own queue.
    LineClient other(listener.port());
    CHECK(isOk(other.request("status")));

    // Now read it all. What reached the socket before it filled comes in
    // order, then how much was dropped, then at most one queue's worth.
    int delivered = 0;
    int64_t lost = 0;
    int lostLines = 0;
    int afterLastLoss = 0;
    int64_t previous = -1;
    while (const std::optional<std::string> raw = stalled.readRawWithin(std::chrono::milliseconds(500)))
    {
        json::Value line;
        std::string error;
        CHECK(json::parse(*raw, line, error));
        const std::string name = text(line, "event");
        if (name == "events.lost")
        {
            int64_t count = 0;
            CHECK(json::getInt(line, "count", count));
            CHECK(count > 0);
            lost += count;
            ++lostLines;
            afterLastLoss = 0;
            continue;
        }
        CHECK_EQ(name, std::string("test"));
        int64_t n = -1;
        CHECK(json::getInt(line, "n", n));
        CHECK(n > previous);
        previous = n;
        ++delivered;
        ++afterLastLoss;
    }

    CHECK(lostLines >= 1);
    CHECK(afterLastLoss <= static_cast<int>(ControlListener::kMaxQueuedEvents));
    CHECK_EQ(delivered + lost, int64_t{kPublished});
    CHECK_EQ(previous, int64_t{kPublished - 1}); // the newest is never the one dropped
}

// ---------------------------------------------------------------------------
// Phase 26: zone.seek, and a source staged part-way. What a seek does to the
// rooms' streams is test_zone.cpp's; these are the command's shape, its
// answers and the event it leaves behind.

MULTIROOM_TEST(control_api_stages_a_source_part_way_in)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));

    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"source.stage","params":{"zone":"living","sourceId":"s1","mediaUri":"http://127.0.0.1:4714/one.flac","startOffsetNs":-1}})")),
             std::string("invalidArgument"));
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"source.stage","params":{"zone":"living","sourceId":"s1","mediaUri":"http://127.0.0.1:4714/one.flac","startOffsetNs":"soon"}})")),
             std::string("invalidArgument"));
    CHECK(isOk(caller.request(
        R"({"command":"source.stage","params":{"zone":"living","sourceId":"s1","mediaUri":"http://127.0.0.1:4714/one.flac","startOffsetNs":1500000000}})")));

    const std::shared_ptr<Zone> living = rig.zones->find(ZoneId("living"));
    const std::vector<SourceSegment> staged = living->staged();
    CHECK_EQ(staged.size(), size_t(1));
    CHECK_EQ(staged.front().startOffsetNs, 1500000000ULL);

    // Opened where it was asked to begin: a second and a half of three is
    // what is left of it.
    CHECK(isOk(caller.request(R"({"command":"zone.play","params":{"zone":"living"}})")));
    const std::vector<StagedSource> opened = living->sources();
    CHECK_EQ(opened.size(), size_t(1));
    CHECK(opened.front().totalFrames.has_value());
    CHECK_EQ(*opened.front().totalFrames, kSourceFrames - kRate * 3 / 2);
}

MULTIROOM_TEST(control_api_seeks_a_playing_zone_and_answers_where_it_put_the_line)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    LineClient feed(rig.listener->port());
    size_t handled = 0;

    Room roomA(rig, "roomA", {formatOf(kRate)}, 1);
    Room roomB(rig, "roomB", {formatOf(kRate)}, 2);
    const std::map<std::string, Room*> rooms{{"roomA", &roomA}, {"roomB", &roomB}};
    roomA.connect();
    roomB.connect();

    CHECK_EQ(errorOf(caller.request(R"({"command":"zone.seek","params":{"zone":"nowhere","positionNs":0}})")),
             std::string("notFound"));
    CHECK_EQ(errorOf(caller.request(R"({"command":"zone.seek","params":{"zone":"living"}})")),
             std::string("invalidArgument"));

    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));
    CHECK(isOk(caller.request(
        R"({"command":"source.stage","params":{"zone":"living","sourceId":"s1","mediaUri":"http://127.0.0.1:4714/one.flac"}})")));
    CHECK(isOk(caller.request(R"({"command":"zone.play","params":{"zone":"living"}})")));

    CHECK_EQ(errorOf(caller.request(R"({"command":"zone.seek","params":{"zone":"living","positionNs":-5}})")),
             std::string("invalidArgument"));
    CHECK_EQ(errorOf(caller.request(R"({"command":"zone.seek","params":{"zone":"living","positionNs":0,"at":1}})")),
             std::string("invalidArgument"));

    // Decoding, and no line yet: nothing is audible to seek in.
    CHECK_EQ(errorOf(caller.request(R"({"command":"zone.seek","params":{"zone":"living","positionNs":500000000}})")),
             std::string("unavailable"));

    CHECK(isOk(caller.request(
        R"({"command":"zone.setClients","params":{"zone":"living","clients":["roomA","roomB"]}})")));
    answerRenegotiations(rig, handled, rooms);
    const std::shared_ptr<Zone> living = rig.zones->find(ZoneId("living"));
    roomA.reportBuffer(0, 500ULL * 1000000ULL);
    roomB.reportBuffer(0, 500ULL * 1000000ULL);
    CHECK(waitFor([&] {
        return rig.log.count(RoomCommandKind::Start, "roomA") == 1 &&
               rig.log.count(RoomCommandKind::Start, "roomB") == 1;
    }));
    roomA.reportAudible(0);
    roomB.reportAudible(0);
    CHECK(waitFor([&] {
        return living->roomStart(ClientId("roomA")) == RoomStartState::Playing &&
               living->roomStart(ClientId("roomB")) == RoomStartState::Playing;
    }));

    // Not a position in a three-second source.
    CHECK_EQ(errorOf(caller.request(R"({"command":"zone.seek","params":{"zone":"living","positionNs":3000000000}})")),
             std::string("invalidArgument"));
    CHECK_EQ(rig.log.count(RoomCommandKind::Flush, "roomA"), size_t(0));

    CHECK(isOk(feed.request(R"({"command":"events.subscribe"})")));
    const json::Value sought =
        caller.request(R"({"command":"zone.seek","params":{"zone":"living","positionNs":2000000000}})");
    CHECK(isOk(sought));
    CHECK_EQ(text(sought, "sourceId"), std::string("s1"));
    int64_t position = -1;
    int64_t cut = -1;
    int64_t resumeAt = -1;
    CHECK(json::getInt(sought, "positionNs", position));
    CHECK(json::getInt(sought, "cutTimelineNs", cut));
    CHECK(json::getInt(sought, "timelineNs", resumeAt));
    CHECK_EQ(position, int64_t(2000000000));
    CHECK_EQ(resumeAt - cut, int64_t(150000000));

    // One FLUSH per room at the cut, and the line never stopped for it.
    for (const char* room : {"roomA", "roomB"})
    {
        CHECK_EQ(rig.log.count(RoomCommandKind::Flush, room), size_t(1));
        CHECK_EQ(rig.log.count(RoomCommandKind::Start, room), size_t(1));
    }
    for (const RoomCommand& command : rig.log.all())
    {
        if (command.kind == RoomCommandKind::Flush)
            CHECK_EQ(command.at.timelineNs, static_cast<uint64_t>(cut));
    }

    // "Now playing" again, for the same source, from where it was seeked to.
    rig.clock.advanceNs(5ULL * kNanosecondsPerSecond);
    bool announced = false;
    while (const std::optional<std::string> line = feed.readRawWithin(std::chrono::milliseconds(300)))
    {
        json::Value event;
        std::string error;
        CHECK(json::parse(*line, event, error));
        if (text(event, "event") != "zone.sourceStarted")
            continue;
        int64_t offset = -1;
        int64_t at = -1;
        CHECK(json::getInt(event, "startOffsetNs", offset));
        CHECK(json::getInt(event, "timelineNs", at));
        if (offset == position)
        {
            CHECK_EQ(at, resumeAt);
            CHECK_EQ(text(event, "sourceId"), std::string("s1"));
            announced = true;
        }
    }
    CHECK(announced);
}

// ---------------------------------------------------------------------------
// Phase 27: a room's output latency, as desired state like its volume.

MULTIROOM_TEST(control_api_keeps_a_room_s_output_latency_and_moves_a_playing_room_once)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    size_t handled = 0;

    const char* refused[] = {
        R"({"command":"client.setOutputLatency","params":{"client":"roomA"}})",
        R"({"command":"client.setOutputLatency","params":{"client":"roomA","latencyMs":-1}})",
        R"({"command":"client.setOutputLatency","params":{"client":"roomA","latencyMs":"80"}})",
        R"({"command":"client.setOutputLatency","params":{"client":"roomA","latencyMs":80,"delay":1}})",
    };
    for (const char* line : refused)
        CHECK_EQ(errorOf(caller.request(line)), std::string("invalidArgument"));

    // The rig's prebuffer is one second and its packets 20 ms, so the ceiling
    // is 980 ms, and a figure past it is refused with the ceiling named.
    const json::Value tooMuch = caller.request(
        R"({"command":"client.setOutputLatency","params":{"client":"roomA","latencyMs":981}})");
    CHECK_EQ(errorOf(tooMuch), std::string("invalidArgument"));
    CHECK(text(tooMuch, "message").find("980 ms") != std::string::npos);
    CHECK(isOk(caller.request(
        R"({"command":"client.setOutputLatency","params":{"client":"roomB","latencyMs":980}})")));

    // Before the room is anywhere: stored and reported.
    json::Value set = caller.request(
        R"({"command":"client.setOutputLatency","params":{"client":"roomA","latencyMs":80}})");
    CHECK(isOk(set));
    CHECK(flag(set, "changed"));
    CHECK(!flag(set, "rejoined"));
    json::Value clients = caller.request(R"({"command":"clients"})");
    const json::Value* settings = entry(clients, "clients", "clientId", "roomA")->find("settings");
    int64_t stored = 0;
    CHECK(json::getInt(*settings, "outputLatencyMs", stored));
    CHECK_EQ(stored, int64_t{80});

    // Admitted, and given it before anything could start it.
    Room room(rig, "roomA", {formatOf(kRate)}, 1);
    const std::map<std::string, Room*> rooms{{"roomA", &room}};
    room.connect();
    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));
    CHECK(isOk(caller.request(
        R"({"command":"source.stage","params":{"zone":"living","sourceId":"s1","mediaUri":"http://127.0.0.1:4714/one.flac"}})")));
    CHECK(isOk(caller.request(R"({"command":"zone.play","params":{"zone":"living"}})")));
    CHECK(isOk(caller.request(
        R"({"command":"zone.setClients","params":{"zone":"living","clients":["roomA"]}})")));
    answerRenegotiations(rig, handled, rooms);
    const std::shared_ptr<Zone> living = rig.zones->find(ZoneId("living"));
    CHECK(living->member(ClientId("roomA")) != nullptr);
    CHECK_EQ(living->syncStatus(ClientId("roomA")).targetOffsetNs, int64_t{80000000});

    room.reportBuffer(0, 500ULL * 1000000ULL);
    CHECK(waitFor([&] { return rig.log.count(RoomCommandKind::Start, "roomA") == 1; }));
    for (const RoomCommand& command : rig.log.all())
    {
        if (command.kind == RoomCommandKind::Start)
            CHECK_EQ(living->startServerTimeNs() - command.start.serverTimeNs,
                     uint64_t{80000000});
    }

    // Replayed: nothing changes and nothing moves.
    set = caller.request(
        R"({"command":"client.setOutputLatency","params":{"client":"roomA","latencyMs":80}})");
    CHECK(isOk(set));
    CHECK(!flag(set, "changed"));
    CHECK_EQ(living->rejoins(ClientId("roomA")), 0ULL);

    // Changed on a room the line has started: moved once.
    set = caller.request(
        R"({"command":"client.setOutputLatency","params":{"client":"roomA","latencyMs":40}})");
    CHECK(isOk(set));
    CHECK(flag(set, "changed"));
    CHECK(flag(set, "rejoined"));
    CHECK_EQ(living->rejoins(ClientId("roomA")), 1ULL);
    CHECK_EQ(living->syncStatus(ClientId("roomA")).targetOffsetNs, int64_t{40000000});
}

MULTIROOM_TEST(control_api_keeps_a_room_s_equalizer_and_never_applies_it_to_a_bitperfect_room)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    size_t handled = 0;

    const char* refused[] = {
        R"({"command":"client.setEqualizer","params":{"client":"roomA"}})",
        R"({"command":"client.setEqualizer","params":{"client":"roomA","enabled":1}})",
        R"({"command":"client.setEqualizer","params":{"client":"roomA","enabled":true,"preampDb":13}})",
        R"({"command":"client.setEqualizer","params":{"client":"roomA","enabled":true,"bands":{}}})",
        R"({"command":"client.setEqualizer","params":{"client":"roomA","enabled":true,"bands":[{"type":"notch","frequencyHz":100,"q":1}]}})",
        R"({"command":"client.setEqualizer","params":{"client":"roomA","enabled":true,"bands":[{"type":"peak","frequencyHz":5,"q":1}]}})",
        R"({"command":"client.setEqualizer","params":{"client":"roomA","enabled":true,"bands":[{"type":"peak","frequencyHz":100,"q":1,"on":true}]}})",
        R"({"command":"client.setEqualizer","params":{"client":"roomA","enabled":true,"extra":1}})",
    };
    for (const char* line : refused)
        CHECK_EQ(errorOf(caller.request(line)), std::string("invalidArgument"));

    // Before the room is anywhere: stored and reported in the shape it was sent.
    const char* wanted =
        R"({"command":"client.setEqualizer","params":{"client":"roomA","enabled":true,"preampDb":-6,"bands":[{"type":"peak","frequencyHz":1000,"gainDb":3,"q":1.41},{"type":"highPass","frequencyHz":30,"q":0.7071}]}})";
    json::Value set = caller.request(wanted);
    CHECK(isOk(set));
    CHECK(flag(set, "changed"));

    json::Value clients = caller.request(R"({"command":"clients"})");
    const json::Value* settings = entry(clients, "clients", "clientId", "roomA")->find("settings");
    const json::Value* equalizer = settings->find("equalizer");
    CHECK(equalizer != nullptr);
    CHECK(flag(*equalizer, "enabled"));
    double preamp = 0.0;
    CHECK(json::getDouble(*equalizer, "preampDb", preamp));
    CHECK_NEAR(preamp, -6.0, 1e-9);
    CHECK_EQ(equalizer->find("bands")->size(), size_t{2});
    CHECK_EQ(text(equalizer->find("bands")->at(1), "type"), std::string("highPass"));

    // Replayed: nothing changes.
    set = caller.request(wanted);
    CHECK(isOk(set));
    CHECK(!flag(set, "changed"));

    // Admitted to a zone in its own format, on a server with no converter
    // (this rig's): it stays bitperfect, so the equalizer is on and not
    // active. With one, an Auto room is resampled for it (test_output_planner).
    Room room(rig, "roomA", {formatOf(kRate)}, 1);
    const std::map<std::string, Room*> rooms{{"roomA", &room}};
    room.connect();
    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"living"}})")));
    CHECK(isOk(caller.request(
        R"({"command":"source.stage","params":{"zone":"living","sourceId":"s1","mediaUri":"http://127.0.0.1:4714/one.flac"}})")));
    CHECK(isOk(caller.request(R"({"command":"zone.play","params":{"zone":"living"}})")));
    CHECK(isOk(caller.request(
        R"({"command":"zone.setClients","params":{"zone":"living","clients":["roomA"]}})")));
    answerRenegotiations(rig, handled, rooms);
    const std::shared_ptr<Zone> living = rig.zones->find(ZoneId("living"));
    const std::shared_ptr<ClientOutput> output = living->member(ClientId("roomA"));
    CHECK(output != nullptr);
    CHECK(output->plan().mode == OutputMode::BitPerfect);

    clients = caller.request(R"({"command":"clients"})");
    const json::Value* reported =
        entry(clients, "clients", "clientId", "roomA")->find("output")->find("equalizer");
    CHECK(reported != nullptr);
    CHECK(!flag(*reported, "active"));
    CHECK(!output->equalizing());

    // A change reaches the member without moving it.
    CHECK(isOk(caller.request(
        R"({"command":"client.setEqualizer","params":{"client":"roomA","enabled":false}})")));
    CHECK_EQ(living->rejoins(ClientId("roomA")), 0ULL);
    CHECK(!rig.rooms.find(ClientId("roomA"))->equalizer->enabled);
}

MULTIROOM_TEST(control_api_lists_the_clients_the_beacons_announced)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());

    json::Value listed = caller.request(R"({"command":"discovery.clients"})");
    CHECK(isOk(listed));
    CHECK(entry(listed, "clients", "client", "kitchen") == nullptr);

    matp::ClientBeacon beacon;
    beacon.protocolVersion = matp::kProtocolVersion;
    beacon.clientId = "kitchen";
    beacon.clientVersion = "0.1.0";
    beacon.controlPort = 4720;
    rig.discovered.onBeacon(beacon, "10.0.0.7", rig.clock.monotonicNowNs());
    rig.clock.advanceMs(500);

    listed = caller.request(R"({"command":"discovery.clients"})");
    const json::Value* kitchen = entry(listed, "clients", "client", "kitchen");
    CHECK(kitchen != nullptr);
    CHECK_EQ(text(*kitchen, "address"), std::string("10.0.0.7"));
    CHECK_EQ(text(*kitchen, "clientVersion"), std::string("0.1.0"));
    CHECK(kitchen->find("platform") == nullptr); // the beacon did not say
    int64_t value = 0;
    CHECK(json::getInt(*kitchen, "controlPort", value));
    CHECK_EQ(value, int64_t(4720));
    CHECK(json::getInt(*kitchen, "lastSeenMsAgo", value));
    CHECK_EQ(value, int64_t(500));
    CHECK(flag(*kitchen, "present"));
    // Heard, but it has not said HELLO here: this is one to attach.
    CHECK(!flag(*kitchen, "connected"));

    // Silent for longer than three beacons: still known, no longer present.
    rig.clock.advanceMs(6000);
    listed = caller.request(R"({"command":"discovery.clients"})");
    kitchen = entry(listed, "clients", "client", "kitchen");
    CHECK(kitchen != nullptr);
    CHECK(!flag(*kitchen, "present"));

    CHECK_EQ(errorOf(caller.request(R"({"command":"discovery.clients","params":{"x":1}})")),
             std::string("invalidArgument"));
}

namespace {

// A client's control API reduced to one connection: it takes one request
// line and answers it with `reply`.
class FakeClientControl
{
public:
    explicit FakeClientControl(std::string reply) : reply_(std::move(reply))
    {
        std::string error;
        listener_ = net::listenOnPort(0, 4, error);
        CHECK(listener_ != net::kInvalidSocket);
        CHECK(net::boundPort(listener_, port_));
        thread_ = std::thread([this]() { serve(); });
    }

    ~FakeClientControl()
    {
        stop_ = true;
        thread_.join();
        net::closeSocket(listener_);
    }

    FakeClientControl(const FakeClientControl&) = delete;
    FakeClientControl& operator=(const FakeClientControl&) = delete;

    uint16_t port() const { return port_; }

    std::string request()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return request_;
    }

private:
    void serve()
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        net::Socket accepted = net::kInvalidSocket;
        while (!stop_ && accepted == net::kInvalidSocket &&
               std::chrono::steady_clock::now() < until)
        {
            bool none = false;
            std::string error;
            accepted = net::acceptOne(listener_, none, error);
            if (accepted == net::kInvalidSocket)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::string received;
        char buffer[256];
        while (!stop_ && accepted != net::kInvalidSocket &&
               received.find('\n') == std::string::npos &&
               std::chrono::steady_clock::now() < until)
        {
            const int count = net::receive(accepted, buffer, sizeof(buffer));
            if (count > 0)
                received.append(buffer, static_cast<size_t>(count));
            else if (count == 0 || !net::wouldBlock(net::lastError()))
                break;
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            request_ = received.substr(0, received.find('\n'));
        }
        const std::string line = reply_ + "\n";
        if (accepted != net::kInvalidSocket)
            net::transmit(accepted, line.data(), static_cast<int>(line.size()));
        net::closeSocket(accepted);
    }

    net::Library library_;
    net::Socket listener_ = net::kInvalidSocket;
    uint16_t port_ = 0;
    std::string reply_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::string request_;
    std::thread thread_;
};

void announce(ApiRig& rig, const std::string& id, uint16_t controlPort, const char* server)
{
    matp::ClientBeacon beacon;
    beacon.protocolVersion = matp::kProtocolVersion;
    beacon.clientId = id;
    beacon.controlPort = controlPort;
    if (server != nullptr)
        beacon.server = server;
    rig.discovered.onBeacon(beacon, "127.0.0.1", rig.clock.monotonicNowNs());
}

} // namespace

MULTIROOM_TEST(control_api_reports_where_a_discovered_client_is_pointed)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());

    announce(rig, "old", 4720, nullptr);
    announce(rig, "free", 4720, "");
    announce(rig, "taken", 4720, "10.0.0.9:4711");
    const json::Value listed = caller.request(R"({"command":"discovery.clients"})");
    CHECK(entry(listed, "clients", "client", "old")->find("server") == nullptr);
    CHECK_EQ(text(*entry(listed, "clients", "client", "free"), "server"), std::string(""));
    CHECK_EQ(text(*entry(listed, "clients", "client", "taken"), "server"),
             std::string("10.0.0.9:4711"));
}

MULTIROOM_TEST(control_api_attaches_and_detaches_a_discovered_client)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());

    // Nobody to call yet.
    CHECK_EQ(errorOf(caller.request(R"({"command":"client.attach","params":{"client":"kitchen"}})")),
             std::string("notFound"));
    CHECK_EQ(errorOf(caller.request(R"({"command":"client.detach","params":{}})")),
             std::string("invalidArgument"));

    {
        // No server named: this core's own address toward the client, and its
        // MATP port.
        FakeClientControl client(R"({"ok":true,"command":"setServer","server":"x","changed":true})");
        announce(rig, "kitchen", client.port(), "");
        CHECK_EQ(errorOf(caller.request(
                     R"({"command":"client.attach","params":{"client":"kitchen"}})")),
                 std::string("unavailable")); // the room port is not known yet
    }
    rig.api->setRoomPort(4711);
    {
        FakeClientControl client(R"({"ok":true,"command":"setServer","server":"x","changed":true})");
        announce(rig, "kitchen", client.port(), "");
        const json::Value attached =
            caller.request(R"({"command":"client.attach","params":{"client":"kitchen"}})");
        CHECK(isOk(attached));
        CHECK_EQ(text(attached, "server"), std::string("127.0.0.1:4711"));
        CHECK(flag(attached, "changed"));
        CHECK_EQ(client.request(),
                 std::string(R"({"command":"setServer","params":{"server":"127.0.0.1:4711"}})"));
    }
    {
        FakeClientControl client(R"({"ok":true,"command":"setServer","server":"","changed":true})");
        announce(rig, "kitchen", client.port(), "127.0.0.1:4711");
        CHECK(isOk(caller.request(R"({"command":"client.detach","params":{"client":"kitchen"}})")));
        CHECK_EQ(client.request(),
                 std::string(R"({"command":"setServer","params":{"server":""}})"));
    }
    {
        // The client's refusal is passed on.
        FakeClientControl client(
            R"({"ok":false,"command":"setServer","error":"invalidArgument","message":"bad"})");
        announce(rig, "kitchen", client.port(), "");
        const json::Value refused = caller.request(
            R"({"command":"client.attach","params":{"client":"kitchen","server":"nope"}})");
        CHECK_EQ(errorOf(refused), std::string("failed"));
        CHECK_EQ(client.request(),
                 std::string(R"({"command":"setServer","params":{"server":"nope"}})"));
    }

    // Silent too long: not called.
    rig.clock.advanceMs(7000);
    CHECK_EQ(errorOf(caller.request(R"({"command":"client.detach","params":{"client":"kitchen"}})")),
             std::string("unavailable"));
}

MULTIROOM_TEST(control_api_proxies_firmware_settings_to_the_discovered_control_port)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    CHECK_EQ(errorOf(caller.request(R"({"command":"client.getLeds","params":{"client":"missing"}})")),
             std::string("notFound"));
    CHECK_EQ(errorOf(caller.request(R"({"command":"client.setLeds","params":{"client":"kitchen","off":"true"}})")),
             std::string("invalidArgument"));
    CHECK_EQ(errorOf(caller.request(R"({"command":"client.setEthernetSpeed","params":{"client":"kitchen","speedMbps":"1000"}})")),
             std::string("invalidArgument"));
    {
        FakeClientControl client(R"({"ok":true,"supported":true,"off":true})");
        announce(rig, "kitchen", client.port(), "");
        const auto result = caller.request(R"({"command":"client.setLeds","params":{"client":"kitchen","off":true}})");
        CHECK(isOk(result)); CHECK(flag(result, "supported")); CHECK(flag(result, "off"));
        CHECK_EQ(client.request(), std::string(R"({"command":"setLeds","params":{"off":true}})"));
    }
    {
        FakeClientControl client(R"({"ok":true,"supported":true,"network":{"ethernet":{"speedMbps":"10"}},"applyDelaySeconds":2})");
        announce(rig, "kitchen", client.port(), "");
        const auto result = caller.request(R"({"command":"client.setEthernetSpeed","params":{"client":"kitchen","speedMbps":"10"}})");
        CHECK(isOk(result));
        CHECK_EQ(result.find("network")->find("ethernet")->find("speedMbps")->asString(), std::string("10"));
        CHECK_EQ(client.request(), std::string(R"({"command":"setNetwork","params":{"ethernet":{"speedMbps":"10"}}})"));
    }
    for (const char* command : {"getNetwork", "getLeds"})
    {
        FakeClientControl client(R"({"ok":true,"supported":false})");
        announce(rig, "kitchen", client.port(), "");
        const auto result = caller.request("{\"command\":\"client." + std::string(command) + "\",\"params\":{\"client\":\"kitchen\"}}");
        CHECK(isOk(result)); CHECK(!flag(result, "supported"));
        CHECK_EQ(client.request(), "{\"command\":\"" + std::string(command) + "\",\"params\":{}}");
    }
    {
        FakeClientControl client(R"({"ok":false,"error":"unavailable","message":"unsupported firmware"})");
        announce(rig, "kitchen", client.port(), "");
        CHECK_EQ(errorOf(caller.request(R"({"command":"client.setLeds","params":{"client":"kitchen","off":false}})")),
                 std::string("unavailable"));
    }
    rig.clock.advanceMs(7000);
    CHECK_EQ(errorOf(caller.request(R"({"command":"client.getNetwork","params":{"client":"kitchen"}})")),
             std::string("unavailable"));
}

MULTIROOM_TEST(control_api_proxies_power_settings_and_commands_to_the_client)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    CHECK_EQ(errorOf(caller.request(R"({"command":"client.setPower","params":{"client":"kitchen"}})")),
             std::string("invalidArgument"));
    CHECK_EQ(errorOf(caller.request(R"({"command":"client.setPower","params":{"client":"kitchen","deepSleepAfterSeconds":"3600"}})")),
             std::string("invalidArgument"));
    CHECK_EQ(errorOf(caller.request(R"({"command":"client.power","params":{"client":"kitchen","action":"powerOff"}})")),
             std::string("invalidArgument"));
    {
        FakeClientControl client(R"({"ok":true,"supported":true,"state":"on","standbyAfterSeconds":900,"deepSleepAfterSeconds":3600,"deepSleep":true})");
        announce(rig, "kitchen", client.port(), "");
        const auto result = caller.request(R"({"command":"client.setPower","params":{"client":"kitchen","standbyAfterSeconds":900,"deepSleepAfterSeconds":3600}})");
        CHECK(isOk(result));
        CHECK_EQ(text(result, "state"), std::string("on"));
        CHECK_EQ(result.find("deepSleepAfterSeconds")->asInt(), int64_t(3600));
        CHECK(flag(result, "deepSleep"));
        CHECK_EQ(client.request(),
                 std::string(R"({"command":"setPower","params":{"standbyAfterSeconds":900,"deepSleepAfterSeconds":3600}})"));
    }
    for (const char* action : {"standbyOn", "standbyOff", "toggleStandby", "deepSleep"})
    {
        FakeClientControl client(R"({"ok":true,"supported":true,"state":"standbyManual"})");
        announce(rig, "kitchen", client.port(), "");
        const auto result = caller.request("{\"command\":\"client.power\",\"params\":{\"client\":\"kitchen\",\"action\":\"" +
                                           std::string(action) + "\"}}");
        CHECK(isOk(result));
        CHECK_EQ(client.request(), "{\"command\":\"" + std::string(action) + "\",\"params\":{}}");
    }
    {
        FakeClientControl client(R"({"ok":true,"supported":true,"state":"on","standbyAfterSeconds":0})");
        announce(rig, "kitchen", client.port(), "");
        CHECK(isOk(caller.request(R"({"command":"client.getPower","params":{"client":"kitchen"}})")));
        CHECK_EQ(client.request(), std::string(R"({"command":"getPower","params":{}})"));
    }
}

MULTIROOM_TEST(control_api_answers_the_core_s_and_each_room_s_system_figures)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());

    // Nothing sampled and nothing reported: the keys are left out, not zeros.
    json::Value status = caller.request(R"({"command":"status"})");
    CHECK(isOk(status));
    CHECK(status.find("server")->find("system") == nullptr);

    rig.api->sampleSystem();
    status = caller.request(R"({"command":"status"})");
    const json::Value* system = status.find("server")->find("system");
    CHECK(system != nullptr);
    int64_t bytes = 0;
    CHECK(json::getInt(*system, "memoryBytes", bytes));
    CHECK(bytes > 0);
    CHECK(system->find("cpuPercent") == nullptr); // one sample is not a rate

    Room room(rig, "roomA", {formatOf(kRate)}, 1);
    room.connect();
    // A setting of its own, so the record outlives the connection below.
    CHECK(isOk(caller.request(
        R"({"command":"client.setGrouped","params":{"client":"roomA","grouped":true}})")));

    json::Value clients = caller.request(R"({"command":"clients"})");
    const json::Value* roomA = entry(clients, "clients", "clientId", "roomA");
    CHECK(roomA != nullptr);
    CHECK(roomA->find("system") == nullptr);
    CHECK(roomA->find("systemAgeMs") == nullptr);

    SystemMetrics metrics;
    metrics.hasMemoryBytes = true;
    metrics.memoryBytes = 14680064;
    metrics.hasTemperatureC = true;
    metrics.temperatureC = 51.5;
    metrics.hasWifiRssiDbm = true;
    metrics.wifiRssiDbm = -71;
    room.reportSystem(metrics);
    rig.clock.advanceMs(300);

    status = caller.request(R"({"command":"status"})");
    roomA = entry(status, "clients", "clientId", "roomA");
    CHECK(roomA != nullptr);
    system = roomA->find("system");
    CHECK(system != nullptr);
    CHECK(json::getInt(*system, "memoryBytes", bytes));
    CHECK_EQ(bytes, int64_t(14680064));
    double celsius = 0.0;
    CHECK(json::getDouble(*system, "temperatureC", celsius));
    CHECK_NEAR(celsius, 51.5, 1e-9);
    int64_t rssi = 0;
    CHECK(json::getInt(*system, "wifiRssiDbm", rssi));
    CHECK_EQ(rssi, int64_t(-71));
    CHECK(system->find("cpuPercent") == nullptr);
    int64_t age = -1;
    CHECK(json::getInt(*roomA, "systemAgeMs", age));
    CHECK_EQ(age, int64_t(300));

    // A malformed one is counted and ignored: the session stays up and the
    // last good figures stand.
    json::Value broken = matp::makeSystemReport(room.session().sessionId().toHex(), metrics);
    broken["system"]["cpuPercent"] = json::Value(500.0);
    room.reportSystem(broken);
    CHECK(room.session().state() != SessionState::Closed);
    CHECK_EQ(room.session().systemReports(), uint64_t(1));
    CHECK_EQ(room.session().systemReportsRejected(), uint64_t(1));
    clients = caller.request(R"({"command":"clients"})");
    roomA = entry(clients, "clients", "clientId", "roomA");
    CHECK(roomA != nullptr && roomA->find("system") != nullptr);

    // Gone with the connection: never older than one.
    room.session().close(kClosedByPeer);
    clients = caller.request(R"({"command":"clients"})");
    roomA = entry(clients, "clients", "clientId", "roomA");
    CHECK(roomA != nullptr);
    CHECK(!flag(*roomA, "connected"));
    CHECK(roomA->find("system") == nullptr);
    CHECK(roomA->find("systemAgeMs") == nullptr);
}

MULTIROOM_TEST(control_api_shows_a_room_s_buffer_and_relays_a_new_size)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());

    Room room(rig, "roomA", {formatOf(kRate)}, 1);
    room.connect();

    // Nothing reported yet: a room that never said is one that may not know
    // the message.
    const char* const toLonger =
        R"({"command":"client.setBuffer","params":{"client":"roomA","bufferMs":2500}})";
    CHECK_EQ(errorOf(caller.request(toLonger)), std::string("unavailable"));

    room.reportBuffer(1500);
    CHECK_EQ(room.session().bufferConfigReports(), uint64_t(1));
    json::Value clients = caller.request(R"({"command":"clients"})");
    const json::Value* roomA = entry(clients, "clients", "clientId", "roomA");
    CHECK(roomA != nullptr && roomA->find("bufferMs") != nullptr);
    CHECK_EQ(roomA->find("bufferMs")->asInt(), int64_t(1500));

    // Not a target at all; the range itself is the client's to refuse.
    for (const char* bad :
         {R"({"command":"client.setBuffer","params":{"client":"roomA","bufferMs":0}})",
          R"({"command":"client.setBuffer","params":{"client":"roomA","bufferMs":"2500"}})",
          R"({"command":"client.setBuffer","params":{"client":"roomA"}})"})
        CHECK_EQ(errorOf(caller.request(bad)), std::string("invalidArgument"));

    json::Value answer = caller.request(toLonger);
    CHECK(isOk(answer));
    CHECK(flag(answer, "changed"));
    CHECK_EQ(rig.log.count(RoomCommandKind::SetBuffer, "roomA"), size_t(1));

    // What the listener then writes for it.
    CHECK_OK(room.session().setBuffer(2500));
    matp::FrameReader reader;
    std::string frame;
    while (room.session().nextOutbound(frame))
        reader.append(frame.data(), frame.size());
    bool written = false;
    for (;;)
    {
        matp::Message message;
        std::string error;
        if (reader.next(message, error) != matp::FrameReader::Result::Message)
            break;
        if (message.type != matp::MessageType::SetBuffer)
            continue;
        matp::SetBuffer request;
        CHECK(matp::parseSetBuffer(message.payload, request, error));
        CHECK_EQ(request.bufferMs, uint32_t(2500));
        CHECK_EQ(request.sessionId, room.session().sessionId().toHex());
        written = true;
    }
    CHECK(written);

    // The client's answer is what `clients` shows, and a request for what it
    // already has is not relayed.
    room.reportBuffer(2500);
    clients = caller.request(R"({"command":"clients"})");
    roomA = entry(clients, "clients", "clientId", "roomA");
    CHECK_EQ(roomA->find("bufferMs")->asInt(), int64_t(2500));
    answer = caller.request(toLonger);
    CHECK(isOk(answer));
    CHECK(!flag(answer, "changed"));
    CHECK_EQ(rig.log.count(RoomCommandKind::SetBuffer, "roomA"), size_t(1));

    // Gone with the connection -- and with it the whole record, which holds
    // no desired state to keep.
    room.session().close(kClosedByPeer);
    clients = caller.request(R"({"command":"clients"})");
    roomA = entry(clients, "clients", "clientId", "roomA");
    CHECK(roomA == nullptr || roomA->find("bufferMs") == nullptr);
}

MULTIROOM_TEST(control_api_lists_a_room_s_audio_devices_and_asks_it_to_switch)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());

    Room room(rig, "roomA", {formatOf(kRate)}, 1);
    room.connect();

    // Nothing reported yet: nothing to choose from.
    const char* const toUsb =
        R"({"command":"client.setAudioDevice","params":{"client":"roomA","deviceId":"usb"}})";
    CHECK_EQ(errorOf(caller.request(toUsb)), std::string("unavailable"));
    json::Value clients = caller.request(R"({"command":"clients"})");
    const json::Value* roomA = entry(clients, "clients", "clientId", "roomA");
    CHECK(roomA != nullptr && roomA->find("audioDevices") == nullptr);

    matp::AudioDeviceList devices;
    devices.selectedDeviceId = "onboard";
    matp::AudioDeviceEntry device;
    device.id = "onboard";
    device.name = "Onboard";
    device.isDefault = true;
    devices.devices.push_back(device);
    device.id = "usb";
    device.name = "USB DAC";
    device.isDefault = false;
    devices.devices.push_back(device);
    room.reportAudioDevices(devices);
    CHECK_EQ(room.session().audioDeviceReports(), uint64_t(1));

    clients = caller.request(R"({"command":"clients"})");
    roomA = entry(clients, "clients", "clientId", "roomA");
    CHECK(roomA != nullptr);
    const json::Value* listed = roomA->find("audioDevices");
    CHECK(listed != nullptr);
    CHECK_EQ(text(*listed, "configured"), std::string());
    CHECK_EQ(text(*listed, "selected"), std::string("onboard"));
    CHECK_EQ(listed->find("devices")->size(), size_t(2));

    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"client.setAudioDevice","params":{"client":"roomA","deviceId":"hdmi"}})")),
             std::string("invalidArgument"));
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"client.setAudioDevice","params":{"client":"roomA"}})")),
             std::string("invalidArgument"));

    json::Value answer = caller.request(toUsb);
    CHECK(isOk(answer));
    CHECK(flag(answer, "changed"));
    CHECK_EQ(rig.log.count(RoomCommandKind::SetAudioDevice, "roomA"), size_t(1));
    for (const RoomCommand& command : rig.log.all())
    {
        if (command.kind == RoomCommandKind::SetAudioDevice)
            CHECK_EQ(command.audioDevice.deviceId, std::string("usb"));
    }

    // What the listener then writes for it.
    CHECK_OK(room.session().setAudioDevice("usb"));
    matp::FrameReader reader;
    std::string frame;
    while (room.session().nextOutbound(frame))
        reader.append(frame.data(), frame.size());
    bool written = false;
    for (;;)
    {
        matp::Message message;
        std::string error;
        if (reader.next(message, error) != matp::FrameReader::Result::Message)
            break;
        if (message.type != matp::MessageType::SetAudioDevice)
            continue;
        matp::SetAudioDevice request;
        CHECK(matp::parseSetAudioDevice(message.payload, request, error));
        CHECK_EQ(request.deviceId, std::string("usb"));
        CHECK_EQ(request.sessionId, room.session().sessionId().toHex());
        written = true;
    }
    CHECK(written);

    // The default is what the room is already configured for.
    answer = caller.request(
        R"({"command":"client.setAudioDevice","params":{"client":"roomA","deviceId":""}})");
    CHECK(isOk(answer));
    CHECK(!flag(answer, "changed"));
    CHECK_EQ(rig.log.count(RoomCommandKind::SetAudioDevice, "roomA"), size_t(1));

    // A rescan is relayed without a payload, and written as RESCAN_AUDIO.
    const char* const rescan = R"({"command":"client.rescanAudio","params":{"client":"roomA"}})";
    CHECK(isOk(caller.request(rescan)));
    CHECK_EQ(rig.log.count(RoomCommandKind::RescanAudio, "roomA"), size_t(1));
    CHECK_OK(room.session().rescanAudio());
    matp::FrameReader rescanReader;
    while (room.session().nextOutbound(frame))
        rescanReader.append(frame.data(), frame.size());
    matp::Message rescanMessage;
    std::string rescanError;
    CHECK(rescanReader.next(rescanMessage, rescanError) == matp::FrameReader::Result::Message);
    CHECK(rescanMessage.type == matp::MessageType::RescanAudio);

    // Gone with the connection.
    CHECK(isOk(caller.request(
        R"({"command":"client.setGrouped","params":{"client":"roomA","grouped":true}})")));
    room.session().close(kClosedByPeer);
    clients = caller.request(R"({"command":"clients"})");
    roomA = entry(clients, "clients", "clientId", "roomA");
    CHECK(roomA != nullptr && roomA->find("audioDevices") == nullptr);
    CHECK_EQ(errorOf(caller.request(toUsb)), std::string("unavailable"));
    CHECK_EQ(errorOf(caller.request(rescan)), std::string("unavailable"));
}

MULTIROOM_TEST(control_api_announces_a_room_s_inputs_and_rebinds_them)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    LineClient feed(rig.listener->port());
    CHECK(isOk(feed.request(R"({"command":"events.subscribe"})")));

    Room room(rig, "roomA", {formatOf(kRate)}, 1);
    room.connect();
    expectEvent(feed, "room.connected");

    const char* const rebind =
        R"({"command":"client.setInputBindings","params":{"client":"roomA","inputId":"play","bindings":{"doubleClick":{"action":"next"}}}})";
    CHECK_EQ(errorOf(caller.request(rebind)), std::string("unavailable"));

    std::vector<matp::InputControl> inputs(2);
    inputs[0].id = "play";
    inputs[0].bindings = matp::defaultInputBindings(matp::InputKind::Button);
    inputs[1].id = "knob";
    inputs[1].kind = matp::InputKind::Encoder;
    inputs[1].bindings = matp::defaultInputBindings(matp::InputKind::Encoder);
    room.reportInputs(inputs, true);
    CHECK_EQ(room.session().inputControlReports(), uint64_t(1));

    json::Value clients = caller.request(R"({"command":"clients"})");
    const json::Value* roomA = entry(clients, "clients", "clientId", "roomA");
    CHECK(roomA != nullptr);
    const json::Value* listed = roomA->find("inputs");
    CHECK(listed != nullptr && listed->size() == size_t(2));
    bool deepSleep = false;
    CHECK(json::getBool(*roomA, "deepSleep", deepSleep) && deepSleep);

    // A gesture becomes an event, in the wire's spelling; the core does
    // nothing else with it.
    matp::InputActionEvent turn;
    turn.inputId = "knob";
    turn.gesture = matp::InputGesture::Clockwise;
    turn.binding = inputs[1].bindings[static_cast<size_t>(turn.gesture)];
    turn.count = 3;
    room.press(turn);
    json::Value event = expectEvent(feed, "room.input");
    CHECK_EQ(text(event, "client"), std::string("roomA"));
    CHECK_EQ(text(event, "inputId"), std::string("knob"));
    CHECK_EQ(text(event, "gesture"), std::string("clockwise"));
    CHECK_EQ(text(event, "action"), std::string("volumeUp"));
    CHECK_EQ(text(event, "scope"), std::string("room"));
    CHECK_EQ(event.find("count")->asInt(), int64_t(3));
    CHECK(!hasKey(event, "sessionId"));
    CHECK_EQ(room.session().inputActions(), uint64_t(1));

    // Checked with the client's own rules before anything is sent.
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"client.setInputBindings","params":{"client":"roomA","inputId":"gone","bindings":{}}})")),
             std::string("invalidArgument"));
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"client.setInputBindings","params":{"client":"roomA","inputId":"play","bindings":{"clockwise":{"action":"next"}}}})")),
             std::string("invalidArgument"));
    CHECK_EQ(rig.log.count(RoomCommandKind::SetInputBindings, "roomA"), size_t(0));

    json::Value answer = caller.request(rebind);
    CHECK(isOk(answer));
    CHECK(flag(answer, "changed"));
    CHECK_EQ(rig.log.count(RoomCommandKind::SetInputBindings, "roomA"), size_t(1));

    // What the listener then writes for it.
    matp::SetInputBindings request;
    request.inputId = "play";
    std::string error;
    CHECK(json::parse(R"({"doubleClick":{"action":"next"}})", request.bindings, error));
    CHECK_OK(room.session().setInputBindings(request));
    matp::FrameReader reader;
    std::string frame;
    while (room.session().nextOutbound(frame))
        reader.append(frame.data(), frame.size());
    bool written = false;
    for (;;)
    {
        matp::Message message;
        if (reader.next(message, error) != matp::FrameReader::Result::Message)
            break;
        if (message.type != matp::MessageType::SetInputBindings)
            continue;
        matp::SetInputBindings read;
        CHECK(matp::parseSetInputBindings(message.payload, read, error));
        CHECK_EQ(read.inputId, std::string("play"));
        CHECK_EQ(read.sessionId, room.session().sessionId().toHex());
        written = true;
    }
    CHECK(written);

    // Already so: nothing to send.
    CHECK(isOk(caller.request(
        R"({"command":"client.setInputBindings","params":{"client":"roomA","inputId":"play","bindings":{"click":{"action":"togglePause"}}}})")));
    CHECK_EQ(rig.log.count(RoomCommandKind::SetInputBindings, "roomA"), size_t(1));

    // Gone with the connection.
    // (A setting keeps the room in the directory once it is gone.)
    CHECK(isOk(caller.request(
        R"({"command":"client.setGrouped","params":{"client":"roomA","grouped":true}})")));
    room.session().close(kClosedByPeer);
    clients = caller.request(R"({"command":"clients"})");
    roomA = entry(clients, "clients", "clientId", "roomA");
    CHECK(roomA != nullptr && roomA->find("inputs") == nullptr);
    CHECK_EQ(errorOf(caller.request(rebind)), std::string("unavailable"));
}

MULTIROOM_TEST(control_api_reports_the_room_hardware_volume)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());

    Room said(rig, "roomA", {formatOf(kRate)}, 1);
    said.hardwareVolume = matp::HardwareVolumeCapability();
    said.hardwareVolume->available = true;
    said.hardwareVolume->control = "PCM";
    said.hardwareVolume->steps = 256;
    said.hardwareVolume->hasDecibels = true;
    said.hardwareVolume->minDb = -102.0;
    said.hardwareVolume->maxDb = 4.0;
    said.hardwareVolume->hasMute = true;
    said.accessMode = "exclusive";
    said.connect();
    Room unsaid(rig, "roomB", {formatOf(kRate)}, 2);
    unsaid.connect();

    const json::Value clients = caller.request(R"({"command":"clients"})");
    const json::Value* roomA = entry(clients, "clients", "clientId", "roomA");
    CHECK(roomA != nullptr);
    CHECK_EQ(text(*roomA->find("capabilities"), "accessMode"), std::string("exclusive"));
    const json::Value* volume = roomA->find("capabilities")->find("hardwareVolume");
    CHECK(volume != nullptr);
    CHECK(flag(*volume, "available"));
    CHECK_EQ(text(*volume, "control"), std::string("PCM"));
    CHECK_EQ(volume->find("steps")->asInt(), static_cast<int64_t>(256));
    CHECK_EQ(volume->find("minDb")->asDouble(), -102.0);
    CHECK(flag(*volume, "hasMute"));

    // A client from before the field: unknown, so left out rather than "none".
    const json::Value* roomB = entry(clients, "clients", "clientId", "roomB");
    CHECK(roomB != nullptr);
    CHECK(roomB->find("capabilities")->find("hardwareVolume") == nullptr);
    CHECK(roomB->find("capabilities")->find("accessMode") == nullptr);
}

MULTIROOM_TEST(control_api_names_the_audio_control_and_relays_the_volume_control)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());

    // Every SESSION_CONFIG names the room's Audio Control, the default first.
    Room room(rig, "roomA", {formatOf(kRate)}, 1);
    room.connect();
    CHECK(room.announcedAudioControl() == matp::AudioControl::Auto);

    // A change re-negotiates the room even with no zone, so the client learns it.
    const size_t before = rig.log.count(RoomCommandKind::Renegotiate, "roomA");
    CHECK(isOk(caller.request(
        R"({"command":"client.setAudioControl","params":{"client":"roomA","audioControl":"bitPerfect"}})")));
    CHECK_EQ(rig.log.count(RoomCommandKind::Renegotiate, "roomA"), before + 1);
    room.renegotiate();
    CHECK(room.announcedAudioControl() == matp::AudioControl::BitPerfect);

    // The old command is gone.
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"client.setOutputPreference","params":{"client":"roomA","bitperfect":"force"}})")),
             std::string("unknownCommand"));

    // Volume Control is the client's: nothing to change before it has said.
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"client.setVolumeControl","params":{"client":"roomA","volumeControl":"hardware"}})")),
             std::string("unavailable"));
    room.reportVolumeConfig(matp::VolumeControlMode::Auto, matp::VolumeControlMode::Disabled);

    // Software under bitPerfect is refused here, before the client is asked.
    CHECK_EQ(errorOf(caller.request(
                 R"({"command":"client.setVolumeControl","params":{"client":"roomA","volumeControl":"software"}})")),
             std::string("invalidArgument"));
    CHECK_EQ(rig.log.count(RoomCommandKind::SetVolumeControl, "roomA"), size_t(0));

    const json::Value set = caller.request(
        R"({"command":"client.setVolumeControl","params":{"client":"roomA","volumeControl":"disabled"}})");
    CHECK(isOk(set));
    CHECK(flag(set, "changed"));
    CHECK_EQ(rig.log.count(RoomCommandKind::SetVolumeControl, "roomA"), size_t(1));

    const json::Value clients = caller.request(R"({"command":"clients"})");
    const json::Value* entryA = entry(clients, "clients", "clientId", "roomA");
    CHECK(entryA != nullptr);
    const json::Value* volume = entryA->find("volumeControl");
    CHECK(volume != nullptr);
    CHECK_EQ(text(*volume, "configured"), std::string("auto"));
    CHECK_EQ(text(*volume, "effective"), std::string("disabled"));
    CHECK_EQ(text(*entryA->find("settings"), "audioControl"), std::string("bitPerfect"));
}

MULTIROOM_TEST(control_api_announces_what_a_room_reports_so_nothing_has_to_be_polled)
{
    ApiRig rig;
    LineClient feed(rig.listener->port());
    CHECK(isOk(feed.request(R"({"command":"events.subscribe"})")));

    Room room(rig, "roomA", {formatOf(kRate)}, 1);
    room.connect();
    expectEvent(feed, "room.connected");

    room.reportVolumeConfig(matp::VolumeControlMode::Hardware, matp::VolumeControlMode::Disabled);
    json::Value event = expectEvent(feed, "room.volumeControl");
    CHECK_EQ(text(event, "client"), std::string("roomA"));
    CHECK_EQ(text(*event.find("volumeControl"), "configured"), std::string("hardware"));
    CHECK_EQ(text(*event.find("volumeControl"), "effective"), std::string("disabled"));

    room.reportBuffer(3000);
    event = expectEvent(feed, "room.bufferConfig");
    CHECK_EQ(event.find("bufferMs")->asInt(), static_cast<int64_t>(3000));

    // A re-sent report is announced; a plain re-negotiation is not.
    room.reportCapabilitiesAgain();
    event = expectEvent(feed, "room.capabilities");
    CHECK_EQ(text(event, "device"), std::string("roomA new device"));
    CHECK_EQ(event.find("capabilities")->find("formats")->size(), size_t(1));
    room.renegotiate();
    room.reportBuffer(3500);
    expectEvent(feed, "room.bufferConfig");
}


MULTIROOM_TEST(control_api_spells_a_rooms_unstable_episode_on_the_wire)
{
    // Phase 41's two events, as the high-end server reads them.
    ZoneEvent unstable;
    unstable.kind = ZoneEventKind::RoomUnstable;
    unstable.zone = ZoneId("living");
    unstable.client = ClientId("esp");
    unstable.trigger = RejoinTrigger::Evicted;
    unstable.rejoins = 3;
    unstable.backoffNs = 4000ULL * 1000000ULL;
    unstable.reason = "StoreFailed: Evicted: frame 10 is behind frame 20";

    json::Value line;
    std::string error;
    CHECK(json::parse(ControlApi::eventLine(unstable), line, error));
    CHECK_EQ(text(line, "event"), std::string("room.unstable"));
    CHECK_EQ(text(line, "client"), std::string("esp"));
    CHECK_EQ(text(line, "zone"), std::string("living"));
    CHECK_EQ(text(line, "trigger"), std::string("evicted"));
    CHECK_EQ(text(line, "detail"), unstable.reason);
    int64_t number = 0;
    CHECK(json::getInt(line, "rejoins", number));
    CHECK_EQ(number, 3);
    CHECK(json::getInt(line, "backoffMs", number));
    CHECK_EQ(number, 4000);

    ZoneEvent stable;
    stable.kind = ZoneEventKind::RoomStable;
    stable.zone = ZoneId("living");
    stable.client = ClientId("esp");
    stable.rejoins = 5;
    stable.unstableForNs = 42ULL * 1000000000ULL;
    CHECK(json::parse(ControlApi::eventLine(stable), line, error));
    CHECK_EQ(text(line, "event"), std::string("room.stable"));
    CHECK(json::getInt(line, "rejoins", number));
    CHECK_EQ(number, 5);
    CHECK(json::getInt(line, "unstableForMs", number));
    CHECK_EQ(number, 42000);
}

MULTIROOM_TEST(control_api_shows_a_room_s_name_keeps_it_while_away_and_relays_a_new_one)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    LineClient feed(rig.listener->port());
    CHECK(isOk(feed.request(R"({"command":"events.subscribe"})")));

    Room room(rig, "3f9c2a71e04b8d15", {formatOf(kRate)}, 1);
    room.connect();
    expectEvent(feed, "room.connected");

    // A room that never said a name may not know section 60.
    const char* const rename =
        R"({"command":"client.setName","params":{"client":"3f9c2a71e04b8d15","name":"Arbeitsraum"}})";
    CHECK_EQ(errorOf(caller.request(rename)), std::string("unavailable"));

    room.reportName("Kitchen");
    json::Value event = expectEvent(feed, "room.name");
    CHECK_EQ(text(event, "client"), std::string("3f9c2a71e04b8d15"));
    CHECK_EQ(text(event, "name"), std::string("Kitchen"));
    CHECK_EQ(room.session().roomNameReports(), uint64_t(1));

    for (const char* bad :
         {R"({"command":"client.setName","params":{"client":"3f9c2a71e04b8d15","name":" Kitchen"}})",
          R"({"command":"client.setName","params":{"client":"3f9c2a71e04b8d15","name":""}})",
          R"({"command":"client.setName","params":{"client":"3f9c2a71e04b8d15"}})"})
        CHECK_EQ(errorOf(caller.request(bad)), std::string("invalidArgument"));

    const json::Value answer = caller.request(rename);
    CHECK(isOk(answer));
    CHECK(flag(answer, "changed"));
    CHECK_EQ(rig.log.count(RoomCommandKind::SetRoomName, "3f9c2a71e04b8d15"), size_t(1));

    // What the listener then writes for it.
    CHECK_OK(room.session().setRoomName("Arbeitsraum"));
    matp::FrameReader reader;
    std::string frame;
    while (room.session().nextOutbound(frame))
        reader.append(frame.data(), frame.size());
    bool sent = false;
    for (;;)
    {
        matp::Message message;
        std::string error;
        if (reader.next(message, error) != matp::FrameReader::Result::Message)
            break;
        if (message.type != matp::MessageType::SetRoomName)
            continue;
        matp::SetRoomName request;
        CHECK(matp::parseSetRoomName(message.payload, request, error));
        CHECK_EQ(request.name, std::string("Arbeitsraum"));
        sent = true;
    }
    CHECK(sent);

    // Away, the room is still shown by its name -- but cannot be renamed.
    room.reportName("Arbeitsraum");
    room.session().close(kClosedByPeer);
    const json::Value clients = caller.request(R"({"command":"clients"})");
    const json::Value* entryA = entry(clients, "clients", "clientId", "3f9c2a71e04b8d15");
    CHECK(entryA == nullptr || text(*entryA, "name") == std::string("Arbeitsraum"));
    CHECK_EQ(errorOf(caller.request(rename)), std::string("unavailable"));
}

MULTIROOM_TEST(control_api_forgets_a_room_that_is_away_and_refuses_one_that_is_here)
{
    ApiRig rig;
    LineClient caller(rig.listener->port());
    LineClient feed(rig.listener->port());
    CHECK(isOk(feed.request(R"({"command":"events.subscribe"})")));

    CHECK(isOk(caller.request(R"({"command":"zone.create","params":{"zone":"office"}})")));
    CHECK(isOk(caller.request(
        R"({"command":"zone.setClients","params":{"zone":"office","clients":["ESP-AUDIO-A1S"]}})")));

    Room room(rig, "ESP-AUDIO-A1S", {formatOf(kRate)}, 1);
    room.connect();

    const char* const forget = R"({"command":"client.forget","params":{"client":"ESP-AUDIO-A1S"}})";
    CHECK_EQ(errorOf(caller.request(forget)), std::string("unavailable"));
    CHECK_EQ(errorOf(caller.request(R"({"command":"client.forget","params":{"client":"nobody"}})")),
             std::string("notFound"));

    room.session().close(kClosedByPeer);
    // Kept while away: it is still in its zone.
    json::Value clients = caller.request(R"({"command":"clients"})");
    CHECK(entry(clients, "clients", "clientId", "ESP-AUDIO-A1S") != nullptr);

    const json::Value answer = caller.request(forget);
    CHECK(isOk(answer));
    CHECK_EQ(text(answer, "zone"), std::string("office"));
    // After the room's own arrival and departure.
    json::Value event;
    for (int i = 0; i < 10 && text(event, "event") != "room.forgotten"; ++i)
        event = feed.readLine();
    CHECK_EQ(text(event, "event"), std::string("room.forgotten"));
    CHECK_EQ(text(event, "client"), std::string("ESP-AUDIO-A1S"));

    clients = caller.request(R"({"command":"clients"})");
    CHECK(entry(clients, "clients", "clientId", "ESP-AUDIO-A1S") == nullptr);
    CHECK_EQ(errorOf(caller.request(forget)), std::string("notFound"));
}
