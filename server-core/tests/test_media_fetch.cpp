// Phase 22: a broken fetch is not a broken zone.
//
// Every case runs end to end: a loopback HTTP server started here on port 0
// with a fault on one path, the real decoder factory with a short stall
// timeout, a real zone and one bitperfect room reading what it was sent with
// the client's own packet reader. The two that recover -- a connection
// dropped half-way, and 503 twice before the file -- must give the room the
// source byte for byte. The three that cannot -- 404, a stall that outlasts
// every retry, and a resume answered with 200 -- must give it what was
// decoded before the failure and then the next staged source from its first
// sample, with the reason in the zone's list. In every case the packet
// numbering has no gap: the client ends a stream over one.

#include "TestSupport.h"

#if MULTIROOM_WITH_FFMPEG
#include "AudioFixtures.h"
#include "HttpTestServer.h"
#include "ManualClock.h"

#include "audio/BlockPool.h"
#include "client/OutputPlanner.h"
#include "decode/DecoderFactory.h"
#include "matp/AudioPacket.h"
#include "resampler/ResamplerRegistry.h"
#include "source/http/HttpMediaSource.h"
#include "util/Clock.h"
#include "util/FileUtil.h"
#include "util/Logger.h"
#include "zone/Zone.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace multiroom;

namespace {

constexpr uint32_t kRate = 44100;
constexpr uint16_t kChannels = 2;
// Long enough that its break, three quarters in, is well past the five
// seconds of a WAV FFmpeg reads while probing it: these are the failures of a
// source that is playing, and a failure inside the probe is a failed open.
constexpr uint32_t kBrokenFrames = 16 * kRate;
constexpr uint32_t kNextFrames = kRate;
constexpr size_t kWavHeaderBytes = 44;
constexpr uint32_t kPacketFrames = 882; // 20 ms at 44.1 kHz

// Short, so that a stall outlasting every retry costs about a second.
MediaFetchSettings shortFetch(std::string token = {})
{
    MediaFetchSettings fetch;
    fetch.stallTimeoutMs = 300;
    fetch.retries = 2;
    fetch.token = std::move(token);
    return fetch;
}

const MediaFetchSettings kFetch = shortFetch();

AudioFormat cdFormat()
{
    AudioFormat format;
    format.sampleRate = kRate;
    format.channels = kChannels;
    format.sampleType = AudioFormat::SampleType::Int16;
    return format;
}

// The source staged after the broken one, and a signal nothing in the first
// one continues into: every sample inverted.
multiroomtest::PcmFixture nextFixture()
{
    multiroomtest::PcmFixture fixture = multiroomtest::makeRamp(kRate, kChannels, 16, kNextFrames);
    for (int32_t& sample : fixture.samples)
        sample = ~sample;
    return fixture;
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

struct Played
{
    std::vector<int32_t> samples;
    size_t packets = 0;
    bool numberedWithoutGap = true;
    ZoneState state = ZoneState::Idle;
    std::vector<FailedSource> failed;
};

void readBack(SinkState& state, Played& played)
{
    std::string received;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        received = state.received;
    }

    matp::AudioPacketReader reader;
    reader.append(received.data(), received.size());
    std::vector<std::byte> payloads;
    played.packets = 0;
    uint64_t expected = 0;
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
        if (played.packets > 0 && packet.header.sequenceNumber != expected)
            played.numberedWithoutGap = false;
        expected = packet.header.sequenceNumber + 1;

        const auto* bytes = reinterpret_cast<const std::byte*>(packet.payload);
        payloads.insert(payloads.end(), bytes, bytes + packet.payloadBytes);
        ++played.packets;
    }
    played.samples = multiroomtest::readSamples(payloads, AudioFormat::SampleType::Int16);
}

// The URIs, staged in order, through the real factory and a real zone to one
// bitperfect room, until the zone has ended and the room has sent everything.
Played play(const std::vector<std::pair<std::string, std::string>>& sources,
            const MediaFetchSettings& fetch = kFetch)
{
    SystemClock wallClock; // the HTTP source's own deadline
    const std::unique_ptr<IDecoderFactory> decoders = makeMediaDecoderFactory(wallClock, fetch);

    AudioMemory memory(blockBytesFor(kRate, kChannels, 20), 256ULL * 1024 * 1024);
    ResamplerRegistry resamplers;
    const SharedPlanner planner(OutputPlanner::withoutResampler());
    multiroomtest::ManualClock clock;

    ZoneSettings settings;
    settings.output.packetDurationMs = 20;
    settings.output.targetBufferNs = 30ULL * kNanosecondsPerSecond; // the whole of both sources
    settings.output.maxQueuedPackets = 1024;
    settings.retentionNs = 120ULL * kNanosecondsPerSecond;
    settings.sendCommand = [](const RoomCommand&) { return true; };

    auto created = Zone::create(ZoneId("living"), memory, planner, resamplers, *decoders, clock,
                                settings);
    CHECK_OK(created);
    Zone& zone = **created;

    for (const auto& [id, uri] : sources)
    {
        SourceSegment segment;
        segment.sourceId = id;
        segment.mediaUri = uri;
        CHECK_OK(zone.stage(segment));
    }
    CHECK_OK(zone.play());

    const ClientId room("livingroom");
    const SessionId session(0x0123456789abcdefULL, 1);
    ClientCapabilities capabilities;
    capabilities.formats = {cdFormat()};
    CHECK_OK(zone.admit(session, room, capabilities, cdFormat()));

    auto sink = std::make_shared<SinkState>();
    StreamAttachment attachment;
    attachment.session = session;
    attachment.client = room;
    CHECK(zone.attach(attachment, std::make_unique<RecordingSink>(sink)));

    // Until the zone has ended and the room has stopped sending.
    Played played;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    auto quietSince = std::chrono::steady_clock::now();
    size_t lastSize = 0;
    while (std::chrono::steady_clock::now() < until)
    {
        zone.pump();
        size_t size = 0;
        {
            std::lock_guard<std::mutex> lock(sink->mutex);
            size = sink->received.size();
        }
        if (size != lastSize)
        {
            lastSize = size;
            quietSince = std::chrono::steady_clock::now();
        }
        else if (zone.state() == ZoneState::Ended &&
                 std::chrono::steady_clock::now() - quietSince > std::chrono::milliseconds(200))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    played.state = zone.state();
    played.failed = zone.failedSources();
    readBack(*sink, played);
    zone.stop();
    return played;
}

// What the room heard is some of `first` from its start and then `next` from
// its start. Returns how many samples of `first` there were.
size_t splitAt(const std::vector<int32_t>& heard, const multiroomtest::PcmFixture& first,
               const multiroomtest::PcmFixture& next)
{
    size_t head = 0;
    while (head < heard.size() && head < first.samples.size() &&
           heard[head] == first.samples[head])
        ++head;

    // `next` can begin with a value `first` happened to continue with; back
    // off a frame at a time until the rest is `next` from its start.
    for (size_t back = 0; back <= 8 && back <= head; back += kChannels)
    {
        const size_t split = head - back;
        const size_t rest = heard.size() - split;
        if (rest <= next.samples.size() &&
            std::equal(heard.begin() + static_cast<std::ptrdiff_t>(split), heard.end(),
                       next.samples.begin()))
            return split;
    }
    CHECK(false); // neither a prefix of the first nor the second from its start
    return head;
}

// The case the roadmap calls "each ends in the documented outcome" for the
// three that cannot recover: the broken source skipped with its reason, the
// next played whole, the zone ended rather than failed.
void skipsTo(const Played& played, const multiroomtest::PcmFixture& broken,
             const multiroomtest::PcmFixture& next, const char* reason, bool anyOfBroken)
{
    CHECK(played.state == ZoneState::Ended);
    CHECK(played.numberedWithoutGap);
    CHECK_EQ(played.failed.size(), 1u);
    if (!played.failed.empty())
    {
        CHECK_EQ(played.failed[0].sourceId, std::string("broken"));
        CHECK(played.failed[0].reason.find(reason) != std::string::npos);
    }

    const size_t split = splitAt(played.samples, broken, next);
    if (anyOfBroken)
        CHECK(split > 0);
    else
        CHECK_EQ(split, 0u);

    // The next source whole, bar the part of a packet a stream's end leaves
    // unsent.
    CHECK(played.samples.size() - split + kPacketFrames * kChannels > next.samples.size());
}

} // namespace

MULTIROOM_TEST(a_connection_dropped_half_way_is_resumed_and_the_room_hears_every_sample)
{
    const multiroomtest::PcmFixture fixture = multiroomtest::makeRamp(kRate, kChannels, 16, kBrokenFrames);
    const std::vector<std::byte> wav = multiroomtest::encodeWav(fixture);

    multiroomtest::HttpTestServer server;
    multiroomtest::HttpTestServer::Faults faults;
    faults.breakAt = wav.size() / 4 * 3;
    faults.drops = 1;
    server.serve("/media/drops.wav", wav, faults);

    const Played played = play({{"drops", server.url("/media/drops.wav")}});

    CHECK(played.state == ZoneState::Ended);
    CHECK(played.failed.empty());
    CHECK(played.numberedWithoutGap);
    CHECK_EQ(played.packets, static_cast<size_t>(kBrokenFrames / kPacketFrames));
    CHECK(played.samples == fixture.samples);

    // And it was a resume, from exactly the byte the connection broke at --
    // not a stream that happened to survive.
    const std::vector<size_t> ranges = server.rangesAsked("/media/drops.wav");
    CHECK(std::count(ranges.begin(), ranges.end(), faults.breakAt) == 1);
}

MULTIROOM_TEST(a_503_twice_and_then_the_file_plays_every_sample)
{
    const multiroomtest::PcmFixture fixture = multiroomtest::makeRamp(kRate, kChannels, 16, kNextFrames);

    multiroomtest::HttpTestServer server;
    multiroomtest::HttpTestServer::Faults faults;
    faults.failFirst = 2;
    faults.failStatus = 503;
    server.serve("/media/busy.wav", multiroomtest::encodeWav(fixture), faults);

    const Played played = play({{"busy", server.url("/media/busy.wav")}});

    CHECK(played.state == ZoneState::Ended);
    CHECK(played.failed.empty());
    CHECK(played.numberedWithoutGap);
    CHECK(played.samples == fixture.samples);
    CHECK(server.rangesAsked("/media/busy.wav").size() >= 3u);
}

MULTIROOM_TEST(a_404_is_skipped_for_the_next_source)
{
    const multiroomtest::PcmFixture broken = multiroomtest::makeRamp(kRate, kChannels, 16, kBrokenFrames);
    const multiroomtest::PcmFixture next = nextFixture();

    multiroomtest::HttpTestServer server;
    server.serve("/media/next.wav", multiroomtest::encodeWav(next));

    const Played played = play({{"broken", server.url("/media/gone.wav")},
                                {"next", server.url("/media/next.wav")}});

    skipsTo(played, broken, next, "NotFound", false);

    // Gone is gone: asked once, never again.
    const std::vector<std::string> requested = server.requested();
    CHECK_EQ(std::count(requested.begin(), requested.end(), "/media/gone.wav"), 1);
}

MULTIROOM_TEST(a_stall_past_every_retry_is_skipped_at_the_point_it_stalled)
{
    const multiroomtest::PcmFixture broken = multiroomtest::makeRamp(kRate, kChannels, 16, kBrokenFrames);
    const multiroomtest::PcmFixture next = nextFixture();
    const std::vector<std::byte> wav = multiroomtest::encodeWav(broken);

    multiroomtest::HttpTestServer server;
    multiroomtest::HttpTestServer::Faults faults;
    faults.breakAt = wav.size() / 4 * 3;
    faults.stalls = 100; // every response that gets there, the resumes included
    server.serve("/media/stalls.wav", wav, faults);
    server.serve("/media/next.wav", multiroomtest::encodeWav(next));

    const auto started = std::chrono::steady_clock::now();
    const Played played = play({{"broken", server.url("/media/stalls.wav")},
                                {"next", server.url("/media/next.wav")}});
    const auto took = std::chrono::steady_clock::now() - started;

    skipsTo(played, broken, next, "Timeout", true);

    // Tried again, from the byte it stalled at, as many times as it may.
    const std::vector<size_t> ranges = server.rangesAsked("/media/stalls.wav");
    CHECK_EQ(std::count(ranges.begin(), ranges.end(), faults.breakAt),
             static_cast<std::ptrdiff_t>(kFetch.retries));

    // The stall, the retries and their backoff, and nothing unbounded.
    CHECK(took >= std::chrono::milliseconds(kFetch.stallTimeoutMs * (kFetch.retries + 1)));
    CHECK(took < std::chrono::seconds(10));
}

// A seek a few kilobytes forward is read through rather than asked for, so a
// disk behind the high-end server that is still spinning up stalls it -- and
// it is tried again from the byte it was for, like a read, not failed outright.
MULTIROOM_TEST(a_seek_that_stalls_is_tried_again_from_the_byte_it_was_for)
{
    std::vector<std::byte> body(64 * 1024);
    for (size_t i = 0; i < body.size(); ++i)
        body[i] = static_cast<std::byte>(i * 7 + i / 251);

    multiroomtest::HttpTestServer server;
    multiroomtest::HttpTestServer::Faults faults;
    faults.breakAt = 8 * 1024;
    faults.stalls = 1;
    server.serve("/media/seek.bin", body, faults);

    SystemClock clock;
    auto opened = HttpMediaSource::open(server.url("/media/seek.bin"), clock, kFetch);
    CHECK_OK(opened);
    HttpMediaSource& source = **opened;

    std::vector<std::byte> head(1024);
    CHECK(source.read(head).value_or(0) > 0);

    const size_t target = faults.breakAt + 12 * 1024;
    const auto started = std::chrono::steady_clock::now();
    CHECK_OK(source.seek(target));
    const auto took = std::chrono::steady_clock::now() - started;
    CHECK_EQ(source.position(), target);

    std::vector<std::byte> after(256);
    const size_t got = source.read(after).value_or(0);
    CHECK(got > 0);
    CHECK(std::equal(after.begin(), after.begin() + static_cast<std::ptrdiff_t>(got),
                     body.begin() + static_cast<std::ptrdiff_t>(target)));

    // It did stall, and the retry asked for the byte the seek was for.
    CHECK(took >= std::chrono::milliseconds(kFetch.stallTimeoutMs));
    const std::vector<size_t> ranges = server.rangesAsked("/media/seek.bin");
    CHECK_EQ(std::count(ranges.begin(), ranges.end(), target), std::ptrdiff_t{1});
}

MULTIROOM_TEST(a_resume_answered_with_200_is_refused_and_skipped)
{
    const multiroomtest::PcmFixture broken = multiroomtest::makeRamp(kRate, kChannels, 16, kBrokenFrames);
    const multiroomtest::PcmFixture next = nextFixture();
    const std::vector<std::byte> wav = multiroomtest::encodeWav(broken);

    multiroomtest::HttpTestServer server;
    multiroomtest::HttpTestServer::Faults faults;
    faults.breakAt = wav.size() / 4 * 3;
    faults.drops = 1;
    faults.rangeAs200AfterDrop = true;
    server.serve("/media/restarts.wav", wav, faults);
    server.serve("/media/next.wav", multiroomtest::encodeWav(next));

    const Played played = play({{"broken", server.url("/media/restarts.wav")},
                                {"next", server.url("/media/next.wav")}});

    skipsTo(played, broken, next, "was not answered from that byte", true);

    // What was heard of it is what arrived before the break, and none of the
    // file the 200 started again.
    const size_t split = splitAt(played.samples, broken, next);
    CHECK(split <= (faults.breakAt - kWavHeaderBytes) / 2);
}

MULTIROOM_TEST(a_source_that_fails_inside_the_probe_is_a_failed_open_not_a_shifted_stream)
{
    const multiroomtest::PcmFixture broken = multiroomtest::makeRamp(kRate, kChannels, 16, kBrokenFrames);
    const multiroomtest::PcmFixture next = nextFixture();
    const std::vector<std::byte> wav = multiroomtest::encodeWav(broken);

    // One second in, which FFmpeg reads before it calls the open done. It
    // stops probing at the error and says the probe succeeded, and what it
    // then decodes does not begin at the first sample -- so the decoder
    // refuses the open instead, and the room hears none of this source.
    multiroomtest::HttpTestServer server;
    multiroomtest::HttpTestServer::Faults faults;
    faults.breakAt = kWavHeaderBytes + size_t{kRate} * kChannels * 2;
    faults.drops = 1;
    faults.rangeAs200AfterDrop = true;
    server.serve("/media/early.wav", wav, faults);
    server.serve("/media/next.wav", multiroomtest::encodeWav(next));

    const Played played = play({{"broken", server.url("/media/early.wav")},
                                {"next", server.url("/media/next.wav")}});

    skipsTo(played, broken, next, "was not answered from that byte", false);
}

// Phase 28: a high-end server that restarts mid-track. The connection being
// read goes with the listener, so the resume is refused -- on Windows it is
// never *told* so -- until the server listens again on the same
// port, well inside the retries this fetch allows.
MULTIROOM_TEST(a_server_restarted_mid_track_is_reconnected_and_the_room_hears_every_sample)
{
    const multiroomtest::PcmFixture fixture = multiroomtest::makeRamp(kRate, kChannels, 16, kBrokenFrames);
    const std::vector<std::byte> wav = multiroomtest::encodeWav(fixture);

    multiroomtest::HttpTestServer server;
    multiroomtest::HttpTestServer::Faults faults;
    faults.breakAt = wav.size() / 4 * 3;
    faults.drops = 1;
    faults.shutAtDrop = true;
    server.serve("/media/restarts.wav", wav, faults);

    // Up to 1.5 s of backoff and five 300 ms connects; the restart comes
    // 400 ms after the shut.
    MediaFetchSettings fetch = shortFetch();
    fetch.retries = 5;

    std::jthread restarter([&server](std::stop_token stop) {
        while (!stop.stop_requested() && !server.isShut())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (stop.stop_requested())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        server.reopen();
    });

    const multiroomtest::TempDirectory temp("phase28");
    const std::string logPath = temp.file("server.log");
    CHECK(log::setLogFile(logPath));
    const Played played = play({{"restarts", server.url("/media/restarts.wav")}}, fetch);
    log::closeLogFile();
    restarter.request_stop();
    restarter.join();

    CHECK(played.state == ZoneState::Ended);
    CHECK(played.failed.empty());
    CHECK(played.numberedWithoutGap);
    CHECK_EQ(played.packets, static_cast<size_t>(kBrokenFrames / kPacketFrames));
    CHECK(played.samples == fixture.samples);

    // Resumed once, from the byte the server went away at, and only after at
    // least one attempt that found nobody there.
    const std::vector<size_t> ranges = server.rangesAsked("/media/restarts.wav");
    CHECK(std::count(ranges.begin(), ranges.end(), faults.breakAt) == 1);
    const auto logged = fileutil::readTextFile(logPath);
    CHECK_OK(logged);
    if (logged)
        CHECK(logged->find("no response") != std::string::npos);
}

MULTIROOM_TEST(a_server_that_does_not_come_back_is_skipped_as_no_response)
{
    const multiroomtest::PcmFixture broken = multiroomtest::makeRamp(kRate, kChannels, 16, kBrokenFrames);
    const multiroomtest::PcmFixture next = nextFixture();
    const std::vector<std::byte> wav = multiroomtest::encodeWav(broken);

    multiroomtest::HttpTestServer gone;
    multiroomtest::HttpTestServer::Faults faults;
    faults.breakAt = wav.size() / 4 * 3;
    faults.drops = 1;
    faults.shutAtDrop = true;
    gone.serve("/media/broken.wav", wav, faults);

    // The next source is somewhere that is still there.
    multiroomtest::HttpTestServer server;
    server.serve("/media/next.wav", multiroomtest::encodeWav(next));

    const Played played = play({{"broken", gone.url("/media/broken.wav")},
                                {"next", server.url("/media/next.wav")}});

    skipsTo(played, broken, next, "no response", true);
    CHECK(gone.isShut());
}

// Phase 23: the shared secret. Refused without it and with the wrong one,
// served with it -- every GET of the open, the probe's seeks included, or the
// file would not play whole -- and absent from everything the server logged
// meanwhile, at the most verbose level there is.
MULTIROOM_TEST(a_media_token_is_sent_on_every_request_and_never_logged)
{
    const std::string token = "phase23-Sh4red.s3cret~token";
    const multiroomtest::PcmFixture fixture = multiroomtest::makeRamp(kRate, kChannels, 16, kNextFrames);

    multiroomtest::HttpTestServer server;
    server.serve("/media/one.wav", multiroomtest::encodeWav(fixture));
    server.requireBearer(token);
    const std::string uri = server.url("/media/one.wav");

    const multiroomtest::TempDirectory temp("phase23");
    const std::string logPath = temp.file("server.log");
    const LogLevel previousLevel = log::currentLevel();
    log::setLevel(LogLevel::Trace);
    CHECK(log::setLogFile(logPath));

    SystemClock clock;
    for (const std::string& sent : {std::string{}, "not-" + token})
    {
        const auto refused = makeMediaDecoderFactory(clock, shortFetch(sent))->open(uri, 0);
        CHECK_ERR(refused, DecodeError::SourceFailed);
        if (!refused)
        {
            MULTIROOM_WARN("refused: {}", describe(refused.error()));
            CHECK(refused.error().detail.find(uri) != std::string::npos);
            CHECK(refused.error().detail.find("media.token") != std::string::npos);
        }
    }
    // A 401 is not transient: asked once each, never retried.
    CHECK_EQ(server.unauthorised(), 2u);

    // A source skipped on the way puts its URL into the log.
    const Played played =
        play({{"gone", server.url("/media/gone.wav")}, {"one", uri}}, shortFetch(token));
    CHECK(played.state == ZoneState::Ended);
    CHECK(played.numberedWithoutGap);
    CHECK_EQ(played.failed.size(), 1u);
    if (!played.failed.empty())
        CHECK(played.failed[0].reason.find("NotFound") != std::string::npos);
    CHECK(played.samples == fixture.samples);
    CHECK_EQ(server.unauthorised(), 2u);

    log::closeLogFile();
    log::setLevel(previousLevel);

    const auto logged = fileutil::readTextFile(logPath);
    CHECK_OK(logged);
    if (logged)
    {
        CHECK(logged->find("/media/gone.wav") != std::string::npos);
        CHECK(logged->find("media.token was refused") != std::string::npos);
        CHECK(logged->find("Sh4red") == std::string::npos);
        CHECK(logged->find("Bearer") == std::string::npos);
    }
}

// --- phase 26: a source opened part-way -----------------------------------------

namespace {

// Everything a decoder gives back from where it was opened to the end, as
// values, and the position of the first frame it says it gave.
std::vector<int32_t> decodeRest(IDecoder& decoder, uint64_t& firstFrame)
{
    std::vector<std::byte> bytes;
    std::vector<std::byte> block(64 * 1024);
    bool first = true;
    for (;;)
    {
        const auto decoded = decoder.decode(block);
        if (!decoded)
        {
            CHECK_EQ(errorName(decoded.error().code), std::string("EndOfStream"));
            break;
        }
        if (first)
            firstFrame = std::to_underlying(decoded->firstFrame);
        first = false;
        bytes.insert(bytes.end(), block.begin(), block.begin() + decoded->byteCount);
    }
    return multiroomtest::readSamples(bytes, decoder.info().format.sampleType);
}

} // namespace

MULTIROOM_TEST(a_wav_and_a_flac_opened_at_an_offset_begin_on_the_exact_sample)
{
    // Phase 26's exactness, measured where it is decided: FFmpeg seeks to the
    // nearest point before the offset and the decoder walks forward to the
    // frame. An offset inside a FLAC block and between WAV packets, so that
    // neither landing is exact by luck.
    const multiroomtest::PcmFixture fixture = multiroomtest::makeRamp(kRate, kChannels, 16, 4 * kRate);
    multiroomtest::HttpTestServer server;
    server.serve("/media/ramp.wav", multiroomtest::encodeWav(fixture));
    server.serve("/media/ramp.flac", multiroomtest::encodeFlac(fixture));

    SystemClock wallClock;
    const std::unique_ptr<IDecoderFactory> decoders = makeMediaDecoderFactory(wallClock, kFetch);

    constexpr uint64_t kOffsetNs = 1234567890ULL;
    const uint64_t frame = *framesFromNanoseconds(kOffsetNs, kRate); // 54444, rounded down
    CHECK_EQ(frame, uint64_t{54444});
    const std::vector<int32_t> expected(fixture.samples.begin() +
                                            static_cast<std::ptrdiff_t>(frame * kChannels),
                                        fixture.samples.end());

    for (const char* path : {"/media/ramp.wav", "/media/ramp.flac"})
    {
        auto decoder = decoders->open(server.url(path), kOffsetNs);
        CHECK_OK(decoder);
        if (!decoder)
            continue;

        // The whole source is still what info() describes.
        CHECK(*(*decoder)->info().totalFrames == fixture.frames);

        uint64_t firstFrame = 0;
        const std::vector<int32_t> decoded = decodeRest(**decoder, firstFrame);
        CHECK_EQ(firstFrame, frame);
        CHECK_EQ(decoded.size(), expected.size());
        CHECK(decoded == expected);
    }

    // A position the source does not have is refused by name, not decoded as
    // an empty source.
    CHECK_ERR(decoders->open(server.url("/media/ramp.wav"), 4ULL * kNanosecondsPerSecond),
              DecodeError::OffsetOutOfRange);
    CHECK_ERR(decoders->open(server.url("/media/ramp.flac"), 5ULL * kNanosecondsPerSecond),
              DecodeError::OffsetOutOfRange);
}

#endif
