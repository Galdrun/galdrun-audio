// Which zones exist, and which zone each room is in.
//
// The second map is the one worth testing: the two listeners route by room and
// know nothing about zones, so a wrong answer here is a STREAM_ATTACH that
// reaches the wrong room or none at all. Everything below is in process, with
// a decoder that generates silence and never touches the filesystem.

#include "ManualClock.h"
#include "TestSupport.h"

#include "audio/BlockPool.h"
#include "client/OutputPlanner.h"
#include "resampler/FakeResampler.h"
#include "resampler/ResamplerRegistry.h"
#include "zone/ZoneManager.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace multiroom;

namespace {

constexpr uint32_t kRate = 44100;
constexpr uint64_t kSourceFrames = kRate / 2; // half a second is plenty here

AudioFormat formatOf(uint32_t rate = kRate)
{
    AudioFormat format;
    format.sampleRate = rate;
    format.channels = 2;
    format.sampleType = AudioFormat::SampleType::Int16;
    return format;
}

SessionId sessionOf(uint64_t low)
{
    return SessionId(0x0123456789abcdefULL, low);
}

class SilenceDecoder final : public IDecoder
{
public:
    SilenceDecoder()
    {
        info_.format = formatOf();
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
            return fail(DecodeError::EndOfStream, "finished");

        const uint32_t bytesPerFrame = info_.format.bytesPerFrame();
        const uint32_t fits = static_cast<uint32_t>(output.size() / bytesPerFrame);
        if (fits == 0)
            return fail(DecodeError::BufferTooSmall, "not one frame of room");

        const uint32_t frames =
            static_cast<uint32_t>(std::min<uint64_t>(fits, kSourceFrames - next_));
        std::fill(output.begin(),
                  output.begin() + static_cast<ptrdiff_t>(frames) * bytesPerFrame,
                  std::byte{0});

        DecodedSpan span;
        span.firstFrame = SourceFrame{next_};
        span.frameCount = frames;
        span.byteCount = static_cast<size_t>(frames) * bytesPerFrame;
        next_ += frames;
        return span;
    }

private:
    SourceStreamInfo info_;
    uint64_t next_ = 0;
};

class SilenceFactory final : public IDecoderFactory
{
public:
    Result<std::unique_ptr<IDecoder>, DecodeError> open(const std::string& uri,
                                                        uint64_t startOffsetNs) override
    {
        if (startOffsetNs != 0)
            return fail(DecodeError::NotSeekable, uri);
        return std::unique_ptr<IDecoder>(std::make_unique<SilenceDecoder>());
    }
};

struct ManagerRig
{
    ManagerRig()
    {
        CHECK_OK(resamplers.add(std::make_unique<FakeResamplerFactory>()));
        auto built = OutputPlanner::create(resamplers, "fake");
        CHECK_OK(built);
        planner.emplace(std::move(*built));

        settings.output.packetDurationMs = 20;
        settings.output.targetBufferNs = 10ULL * kNanosecondsPerSecond;
        settings.retentionNs = 60ULL * kNanosecondsPerSecond;

        // Nothing here starts a room; a zone needs somewhere to send a start
        // all the same, because one that cannot is one that cannot play.
        settings.sendCommand = [](const RoomCommand&) { return true; };
    }

    std::unique_ptr<ZoneManager> manager(size_t maxZones = ZoneManager::kDefaultMaxZones)
    {
        return std::make_unique<ZoneManager>(memory, *planner, resamplers, factory, clock,
                                             settings, maxZones);
    }

    SilenceFactory factory;
    multiroomtest::ManualClock clock;
    AudioMemory memory{blockBytesFor(192000, 2, 20), 256ULL * 1024 * 1024};
    ResamplerRegistry resamplers;
    std::optional<SharedPlanner> planner;
    ZoneSettings settings;
};

SourceSegment segmentOf(const char* id)
{
    SourceSegment segment;
    segment.sourceId = id;
    segment.mediaUri = "generated";
    return segment;
}

ClientCapabilities offering(AudioFormat format)
{
    ClientCapabilities capabilities;
    capabilities.formats.push_back(format);
    return capabilities;
}

} // namespace

MULTIROOM_TEST(zone_manager_holds_zones_by_id)
{
    ManagerRig rig;
    auto zones = rig.manager();

    CHECK_EQ(zones->size(), 0u);
    CHECK(zones->find(ZoneId("living")) == nullptr);

    auto living = zones->create(ZoneId("living"));
    CHECK_OK(living);
    CHECK_EQ(zones->size(), 1u);
    CHECK(zones->find(ZoneId("living")) == *living);

    // An idle zone costs an entry in a map: no decoder, no thread, no source.
    CHECK((*living)->state() == ZoneState::Idle);
    CHECK_EQ((*living)->memberCount(), 0u);

    // Replacing one would leave the rooms in the first playing a timeline
    // nothing holds any more, so a duplicate is refused instead.
    CHECK_ERR(zones->create(ZoneId("living")), ZoneError::AlreadyExists);

    CHECK_OK(zones->create(ZoneId("kitchen")));
    const std::vector<ZoneId> ids = zones->ids();
    CHECK_EQ(ids.size(), 2u);
    CHECK_EQ(ids[0].str(), std::string("kitchen")); // sorted, not insertion order
    CHECK_EQ(ids[1].str(), std::string("living"));

    CHECK(zones->remove(ZoneId("living")));
    CHECK(!zones->remove(ZoneId("living")));
    CHECK_EQ(zones->size(), 1u);
}

MULTIROOM_TEST(zone_manager_refuses_more_zones_than_its_ceiling)
{
    ManagerRig rig;
    auto zones = rig.manager(2);

    CHECK_OK(zones->create(ZoneId("one")));
    CHECK_OK(zones->create(ZoneId("two")));
    CHECK_ERR(zones->create(ZoneId("three")), ZoneError::Full);
    CHECK_EQ(zones->size(), 2u);
}

MULTIROOM_TEST(zone_manager_knows_which_zone_a_room_is_in)
{
    ManagerRig rig;
    auto zones = rig.manager();

    auto living = zones->create(ZoneId("living"));
    CHECK_OK(living);
    CHECK_OK((*living)->stage(segmentOf("one")));
    CHECK_OK((*living)->play());

    const ClientId room("livingroom");
    CHECK(zones->zoneOf(room) == nullptr);

    CHECK_OK(zones->assign(ZoneId("living"), sessionOf(1), room, offering(formatOf()),
                           formatOf()));
    CHECK(zones->zoneOf(room) == *living);
    CHECK_EQ((*living)->memberCount(), 1u);

    // A room is in at most one zone, so moving it leaves the first before it
    // joins the second.
    auto kitchen = zones->create(ZoneId("kitchen"));
    CHECK_OK(kitchen);
    CHECK_OK((*kitchen)->stage(segmentOf("two")));
    CHECK_OK((*kitchen)->play());

    CHECK_OK(zones->assign(ZoneId("kitchen"), sessionOf(2), room, offering(formatOf()),
                           formatOf()));
    CHECK(zones->zoneOf(room) == *kitchen);
    CHECK_EQ((*living)->memberCount(), 0u);
    CHECK_EQ((*kitchen)->memberCount(), 1u);

    // A session closing before it was ever assigned is the normal case, not an
    // error to report.
    zones->release(ClientId("never-seen"));

    zones->release(room);
    CHECK(zones->zoneOf(room) == nullptr);
    CHECK_EQ((*kitchen)->memberCount(), 0u);

    CHECK_ERR(zones->assign(ZoneId("nowhere"), sessionOf(3), room, offering(formatOf()),
                            formatOf()),
              ZoneError::NotPlaying);
}

MULTIROOM_TEST(removing_a_zone_forgets_the_rooms_that_were_in_it)
{
    ManagerRig rig;
    auto zones = rig.manager();

    auto living = zones->create(ZoneId("living"));
    CHECK_OK(living);
    CHECK_OK((*living)->stage(segmentOf("one")));
    CHECK_OK((*living)->play());

    const ClientId room("livingroom");
    CHECK_OK(zones->assign(ZoneId("living"), sessionOf(1), room, offering(formatOf()),
                           formatOf()));

    CHECK(zones->remove(ZoneId("living")));

    // The index has to go with the zone: a STREAM_ATTACH arriving afterwards
    // must find nothing rather than a zone that has been stopped.
    CHECK(zones->zoneOf(room) == nullptr);
    CHECK((*living)->member(room) == nullptr);
}
