// Phase 20: media comes from the high-end server, over HTTP, from one origin.
//
// The first half is the rule as a function -- which URIs are under a base and
// which only look as if they were. The second half needs FFmpeg and runs the
// rule end to end against a loopback HTTP server started here on port 0: a
// WAV and a FLAC served by it play through a real zone byte for byte, and a
// path, a `file:` URI and a redirect are refused by the decoder factory
// behind the control API's own check.

#include "TestSupport.h"

#include "source/MediaOrigin.h"

#if MULTIROOM_WITH_FFMPEG
#include "AudioFixtures.h"
#include "HttpTestServer.h"
#include "ManualClock.h"

#include "audio/BlockPool.h"
#include "client/OutputPlanner.h"
#include "decode/DecoderFactory.h"
#include "matp/AudioPacket.h"
#include "resampler/ResamplerRegistry.h"
#include "util/Clock.h"
#include "zone/Zone.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>
#endif

#include <string>
#include <utility>

using namespace multiroom;

namespace {

MediaOrigin originOf(const char* base)
{
    auto parsed = MediaOrigin::parse(base);
    CHECK_OK(parsed);
    return std::move(*parsed);
}

} // namespace

MULTIROOM_TEST(media_origin_accepts_what_is_under_the_base)
{
    const MediaOrigin origin = originOf("http://127.0.0.1:4714/");

    for (const char* uri :
         {"http://127.0.0.1:4714/", "http://127.0.0.1:4714/track/42.flac",
          "HTTP://127.0.0.1:4714/a.flac", "http://127.0.0.1:4714/a.flac?token=x#t",
          "http://127.0.0.1:4714/a%20b.flac", "http://127.0.0.1:4714/..a/b..", "http://127.0.0.1:4714"})
    {
        CHECK_OK(origin.check(uri));
    }

    // An absent port is the scheme's own, both ways round, and a host is
    // compared without regard to case.
    const MediaOrigin plain = originOf("http://Media.LAN/music");
    CHECK_OK(plain.check("http://media.lan:80/music/a.flac"));
    CHECK_OK(plain.check("http://MEDIA.lan/music/"));
    const MediaOrigin secure = originOf("https://media.lan:443/");
    CHECK_OK(secure.check("https://media.lan/a.flac"));
    const MediaOrigin v6 = originOf("http://[::1]:4714/");
    CHECK_OK(v6.check("http://[::1]:4714/a.flac"));
}

MULTIROOM_TEST(media_origin_refuses_each_way_round_it_by_name)
{
    const MediaOrigin origin = originOf("http://127.0.0.1:4714/media/");

    // Not http(s) at all: a path on the core's disk, a file: URI, another
    // scheme FFmpeg would happily open.
    for (const char* uri : {"/srv/music/a.flac", "C:/music/a.flac", "a.flac", "file:///etc/passwd",
                            "concat:a|b", "ftp://127.0.0.1:4714/media/a.flac", ""})
    {
        const auto refused = origin.check(uri);
        CHECK_ERR(refused, MediaOriginError::UnsupportedScheme);
        if (!refused)
            CHECK(refused.error().detail.find("http://127.0.0.1:4714/media/") != std::string::npos);
    }

    // A well-formed URL of another origin, or outside the base's path.
    for (const char* uri :
         {"http://evil.example/media/a.flac", "http://127.0.0.2:4714/media/a.flac",
          "http://127.0.0.1:4715/media/a.flac", "http://127.0.0.1/media/a.flac",
          "https://127.0.0.1:4714/media/a.flac", "http://127.0.0.1:4714/a.flac",
          "http://127.0.0.1:4714/media2/a.flac", "http://127.0.0.1:4714/media",
          "http://127.0.0.1:4714/media/../secret", "http://127.0.0.1:4714/media/%2e%2E/secret",
          "http://127.0.0.1:4714/media/..%2Fsecret", "http://127.0.0.1:4714/media/./a.flac"})
    {
        const auto refused = origin.check(uri);
        CHECK_ERR(refused, MediaOriginError::Outside);
        if (!refused)
        {
            CHECK(refused.error().detail.find(uri) != std::string::npos);
            CHECK(refused.error().detail.find("http://127.0.0.1:4714/media/") != std::string::npos);
        }
    }

    // Look-alikes: the right characters at the start and another host behind
    // them. None is given a reading at all.
    for (const char* uri :
         {"http://127.0.0.1:4714.evil.example/media/a.flac",
          "http://127.0.0.1:4714@evil.example/media/a.flac",
          "http://user:pass@127.0.0.1:4714/media/a.flac", "http://127.0.0.1:4714\\@evil/media/",
          "http://127.0.0.1:4714/media/a flac", "http://127.0.0.1:/media/a.flac",
          "http://127.0.0.1:99999/media/a.flac", "http:///media/a.flac", "http://[::1/media/"})
    {
        CHECK_ERR(origin.check(uri), MediaOriginError::Malformed);
    }
}

MULTIROOM_TEST(media_origin_refuses_a_base_it_could_not_hold_a_uri_against)
{
    CHECK_ERR(MediaOrigin::parse(""), MediaOriginError::Malformed);
    CHECK_ERR(MediaOrigin::parse("http://"), MediaOriginError::Malformed);
    CHECK_ERR(MediaOrigin::parse("http://h:0/"), MediaOriginError::Malformed);
    CHECK_ERR(MediaOrigin::parse("http://h/?token=x"), MediaOriginError::Malformed);
    CHECK_ERR(MediaOrigin::parse("http://h/a/../b/"), MediaOriginError::Malformed);
    CHECK_ERR(MediaOrigin::parse("/srv/music"), MediaOriginError::UnsupportedScheme);
    CHECK_ERR(MediaOrigin::parse("file:///srv/music/"), MediaOriginError::UnsupportedScheme);

    const MediaOrigin origin = originOf("https://media.lan:8443/v");
    CHECK_EQ(origin.text(), std::string("https://media.lan:8443/v"));
    CHECK_EQ(origin.port(), uint16_t(8443));
    CHECK_EQ(originOf("http://h/").port(), uint16_t(80));
    CHECK_EQ(originOf("https://h/").port(), uint16_t(443));
}

MULTIROOM_TEST(media_origin_knows_a_loopback_host_as_written)
{
    for (const char* base : {"http://127.0.0.1:4714/", "http://127.9.8.7/", "http://LOCALHOST/",
                             "http://[::1]:4714/"})
        CHECK(originOf(base).loopback());

    // Resolved by nobody: a name that starts like an address, or one that
    // may well point here, is another machine for phase 23's warning.
    for (const char* base : {"http://media.lan/", "http://127.0.0.1.example/", "http://128.0.0.1/",
                             "http://127.1/", "http://[::2]/", "http://localhost.lan/"})
        CHECK(!originOf(base).loopback());
}

#if MULTIROOM_WITH_FFMPEG

namespace {

constexpr uint32_t kRate = 44100;
constexpr uint16_t kChannels = 2;
constexpr uint32_t kFrames = kRate; // one second, 50 packets of 20 ms exactly

AudioFormat cdFormat()
{
    AudioFormat format;
    format.sampleRate = kRate;
    format.channels = kChannels;
    format.sampleType = AudioFormat::SampleType::Int16;
    return format;
}

struct SinkState
{
    std::mutex mutex;
    std::string received;
};

class RecordingSink final : public IPacketSink
{
public:
    explicit RecordingSink(std::shared_ptr<SinkState> state) : state_(std::move(state)) {}

    Result<size_t, PacketSinkError> write(std::span<const std::byte> bytes) override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return bytes.size();
    }

    void close() override {}

private:
    std::shared_ptr<SinkState> state_;
};

// Every sample a room was sent, read with the client's own packet reader.
std::vector<int32_t> samplesReceived(SinkState& state, size_t& packets)
{
    std::string received;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        received = state.received;
    }

    matp::AudioPacketReader reader;
    reader.append(received.data(), received.size());
    std::vector<std::byte> payloads;
    packets = 0;
    for (;;)
    {
        matp::AudioPacket packet;
        std::string error;
        const auto result = reader.next(packet, error);
        if (result != matp::AudioPacketReader::Result::Packet)
        {
            CHECK(result != matp::AudioPacketReader::Result::Invalid);
            break;
        }
        const auto* bytes = reinterpret_cast<const std::byte*>(packet.payload);
        payloads.insert(payloads.end(), bytes, bytes + packet.payloadBytes);
        ++packets;
    }
    return multiroomtest::readSamples(payloads, AudioFormat::SampleType::Int16);
}

// A served file through the real decoder factory and a real zone to one
// bitperfect room: what the room is sent must be the fixture, sample for
// sample, because nothing between the socket and the packet may touch it.
void playsThroughAZone(const std::string& uri, const multiroomtest::PcmFixture& fixture)
{
    SystemClock wallClock; // the HTTP source's own deadline
    const std::unique_ptr<IDecoderFactory> decoders = makeMediaDecoderFactory(wallClock);

    AudioMemory memory(blockBytesFor(192000, 2, 20), 256ULL * 1024 * 1024);
    ResamplerRegistry resamplers;
    const SharedPlanner planner(OutputPlanner::withoutResampler());
    multiroomtest::ManualClock clock;

    ZoneSettings settings;
    settings.output.packetDurationMs = 20;
    settings.output.targetBufferNs = 10ULL * kNanosecondsPerSecond;
    settings.output.maxQueuedPackets = 1024;
    settings.retentionNs = 60ULL * kNanosecondsPerSecond;
    settings.sendCommand = [](const RoomCommand&) { return true; };

    auto created = Zone::create(ZoneId("living"), memory, planner, resamplers, *decoders, clock,
                                settings);
    CHECK_OK(created);
    Zone& zone = **created;

    SourceSegment segment;
    segment.sourceId = "one";
    segment.mediaUri = uri;
    CHECK_OK(zone.stage(segment));
    CHECK_OK(zone.play());
    CHECK_EQ(describeFormat(zone.sourceFormat()), describeFormat(cdFormat()));

    const ClientId room("livingroom");
    const SessionId session(0x0123456789abcdefULL, 1);
    ClientCapabilities capabilities;
    capabilities.formats = {cdFormat()};
    CHECK_OK(zone.admit(session, room, capabilities, cdFormat()));
    const std::shared_ptr<ClientOutput> output = zone.member(room);
    CHECK(output != nullptr && output->passThrough());

    auto sink = std::make_shared<SinkState>();
    StreamAttachment attachment;
    attachment.session = session;
    attachment.client = room;
    CHECK(zone.attach(attachment, std::make_unique<RecordingSink>(sink)));

    const uint64_t expectedPackets = kFrames / 882;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < until &&
           (zone.state() != ZoneState::Ended || output->packets() < expectedPackets))
    {
        zone.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(zone.state() == ZoneState::Ended);

    size_t packets = 0;
    const std::vector<int32_t> samples = samplesReceived(*sink, packets);
    CHECK_EQ(packets, static_cast<size_t>(expectedPackets));
    CHECK_EQ(samples.size(), fixture.samples.size());
    CHECK(samples == fixture.samples);

    zone.stop();
}

} // namespace

MULTIROOM_TEST(a_wav_and_a_flac_served_over_http_play_through_a_zone_byte_for_byte)
{
    const multiroomtest::PcmFixture fixture = multiroomtest::makeRamp(kRate, kChannels, 16, kFrames);

    multiroomtest::HttpTestServer server;
    server.serve("/media/ramp.wav", multiroomtest::encodeWav(fixture));
    server.serve("/media/ramp.flac", multiroomtest::encodeFlac(fixture));

    // The URIs a high-end server would hand out, and the origin it would be
    // configured as.
    const MediaOrigin origin = originOf(server.url("/media/").c_str());
    CHECK_OK(origin.check(server.url("/media/ramp.wav")));
    CHECK_OK(origin.check(server.url("/media/ramp.flac")));

    playsThroughAZone(server.url("/media/ramp.wav"), fixture);
    playsThroughAZone(server.url("/media/ramp.flac"), fixture);
}

MULTIROOM_TEST(the_decoder_factory_opens_nothing_but_http)
{
    SystemClock clock;
    const std::unique_ptr<IDecoderFactory> decoders = makeMediaDecoderFactory(clock);

    multiroomtest::TempDirectory temp("phase20");
    const std::string path = temp.file("ramp.wav");
    {
        const auto bytes =
            multiroomtest::encodeWav(multiroomtest::makeRamp(kRate, kChannels, 16, 1000));
        std::FILE* file = std::fopen(path.c_str(), "wb");
        CHECK(file != nullptr);
        if (file != nullptr)
        {
            std::fwrite(bytes.data(), 1, bytes.size(), file);
            std::fclose(file);
        }
    }

    // A file that exists and would decode is still refused: the core never
    // opens a file it was named, since phase 20.
    std::string fileUri = "file:///" + path;
    std::replace(fileUri.begin(), fileUri.end(), '\\', '/');
    for (const std::string& uri : {path, fileUri, std::string("concat:") + path})
    {
        const auto refused = decoders->open(uri, 0);
        CHECK_ERR(refused, DecodeError::SourceFailed);
        if (!refused)
            CHECK(refused.error().detail.find(uri) != std::string::npos);
    }
}

MULTIROOM_TEST(a_redirect_is_not_followed_even_on_the_same_origin)
{
    const multiroomtest::PcmFixture fixture = multiroomtest::makeRamp(kRate, kChannels, 16, 1000);

    multiroomtest::HttpTestServer server;
    server.serve("/media/target.wav", multiroomtest::encodeWav(fixture));
    server.redirect("/media/moved.wav", server.url("/media/target.wav"));
    server.redirect("/media/away.wav", "http://evil.example/media/target.wav");

    SystemClock clock;
    const std::unique_ptr<IDecoderFactory> decoders = makeMediaDecoderFactory(clock);

    for (const char* path : {"/media/moved.wav", "/media/away.wav"})
    {
        const std::string uri = server.url(path);
        const auto refused = decoders->open(uri, 0);
        CHECK_ERR(refused, DecodeError::SourceFailed);
        if (!refused)
        {
            CHECK(refused.error().detail.find(uri) != std::string::npos);
            CHECK(refused.error().detail.find("redirect") != std::string::npos);
        }
    }

    // Asked for once each, and the place they pointed at never.
    const std::vector<std::string> requested = server.requested();
    CHECK_EQ(std::count(requested.begin(), requested.end(), "/media/moved.wav"), 1);
    CHECK_EQ(std::count(requested.begin(), requested.end(), "/media/away.wav"), 1);
    CHECK_EQ(std::count(requested.begin(), requested.end(), "/media/target.wav"), 0);

    // And the target itself is fine, which is what makes the refusals above
    // the redirect's and not the server's.
    CHECK_OK(decoders->open(server.url("/media/target.wav"), 0));
}

#endif
