#include "TestSupport.h"

#include "client/OutputPlanner.h"
#include "util/FileUtil.h"
#include "util/Logger.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace multiroom;

namespace {

using SampleType = AudioFormat::SampleType;

// A converter that counts how many times it was asked for. Criterion 5 of the
// definition of done is that a bitperfect room never has one built, and the
// only way to prove a negative is to be able to count the positive -- which is
// why every test that asserts zero also proves the counter can reach one.
class CountingResampler final : public IAudioResampler
{
public:
    explicit CountingResampler(const ResamplerCapabilities& caps) : caps_(caps) {}

    const ResamplerCapabilities& capabilities() const noexcept override { return caps_; }

    Status<ResamplerError> configure(const ResamplerConfig& config) override
    {
        if (!config.isValid())
            return fail(ResamplerError::InvalidConfig, "counting");
        ratio_ = config.effectiveRatio();
        return {};
    }

    bool configured() const noexcept override { return ratio_ > 0.0; }
    double ratio() const noexcept override { return ratio_; }

    Status<ResamplerError> setRatio(double value) override
    {
        if (!caps_.variableRatio)
            return fail(ResamplerError::FixedRatioOnly, caps_.id);
        ratio_ = value;
        return {};
    }

    Result<ProcessResult, ResamplerError> process(std::span<const double>,
                                                  std::span<double>) override
    {
        return fail(ResamplerError::NotConfigured, "the counting stub does not convert");
    }

    void reset() noexcept override {}
    double latencyFrames() const noexcept override { return 0.0; }

private:
    ResamplerCapabilities caps_;
    double ratio_ = 0.0;
};

class CountingFactory final : public IResamplerFactory
{
public:
    CountingFactory(std::string id, bool variableRatio, int* creations, uint16_t maxChannels,
                    double minRatio, double maxRatio)
        : creations_(creations)
    {
        caps_.id = std::move(id);
        caps_.variableRatio = variableRatio;
        caps_.maxChannels = maxChannels;
        caps_.minRatio = minRatio;
        caps_.maxRatio = maxRatio;
    }

    const ResamplerCapabilities& capabilities() const noexcept override { return caps_; }

    std::unique_ptr<IAudioResampler> create() const override
    {
        ++(*creations_);
        return std::make_unique<CountingResampler>(caps_);
    }

private:
    ResamplerCapabilities caps_;
    int* creations_;
};

// The planner reads the registry once, at create(), so the registry has to
// outlive that call and nothing else -- but the creation counter has to
// outlive the whole test, which is what this holds together.
class Fixture
{
public:
    explicit Fixture(std::string id = "counting", bool variableRatio = true,
                     uint16_t maxChannels = 8, double minRatio = 1.0 / 32.0,
                     double maxRatio = 32.0)
    {
        const std::string key = id;
        CHECK_OK(registry_.add(std::make_unique<CountingFactory>(
            std::move(id), variableRatio, &creations_, maxChannels, minRatio, maxRatio)));

        // The startup line is asserted by its own test; everywhere else it is
        // noise on the same stream the test results use.
        const LogLevel previous = log::currentLevel();
        log::setLevel(LogLevel::Error);
        auto made = OutputPlanner::create(registry_, key);
        log::setLevel(previous);

        CHECK_OK(made);
        planner_.emplace(std::move(*made));
    }

    const OutputPlanner& planner() const { return *planner_; }
    const ResamplerRegistry& registry() const { return registry_; }
    int creations() const { return creations_; }

private:
    ResamplerRegistry registry_;
    int creations_ = 0;
    std::optional<OutputPlanner> planner_;
};

AudioFormat format(uint32_t rate, uint16_t channels, SampleType type)
{
    AudioFormat out;
    out.sampleRate = rate;
    out.channels = channels;
    out.sampleType = type;
    return out;
}

ClientCapabilities capabilities(std::vector<AudioFormat> formats, uint32_t preferred = 0)
{
    ClientCapabilities out;
    out.formats = std::move(formats);
    out.preferredSampleRate = preferred;
    return out;
}

const AudioFormat kSource = format(44100, 2, SampleType::Int24In32);

// A device that accepts the source's own format, and one that does not. Two
// lists are enough for the table below, because "can play exactly" is the only
// thing the ten rows branch on.
ClientCapabilities matching()
{
    return capabilities({format(44100, 2, SampleType::Int24In32),
                         format(48000, 2, SampleType::Int24In32)},
                        48000);
}

ClientCapabilities mismatching()
{
    return capabilities({format(48000, 2, SampleType::Int24In32)}, 48000);
}

struct Row
{
    AudioControl audioControl;
    bool grouped;
    bool deviceTakesTheSource;
    bool corrected;

    // describePlan()'s whole output, which carries the mode, both formats, the
    // correction state and both reasons -- so one comparison covers a row and
    // prints everything that differs when it does not.
    const char* expected;
};

void checkRows(const OutputPlanner& planner, const std::vector<Row>& rows)
{
    for (const Row& row : rows)
    {
        const RoomAudioSettings settings{row.audioControl, row.grouped};
        const ClientCapabilities caps = row.deviceTakesTheSource ? matching() : mismatching();
        const auto plan = planner.plan(kSource, settings, caps);
        CHECK_OK(plan);
        CHECK_EQ(describePlan(*plan), std::string(row.expected));
        CHECK(plan->driftCorrected() == row.corrected);
    }
}

const char* kBitperfect =
    "44100 Hz 2ch int24in32 -> 44100 Hz 2ch int24in32: bitperfect, uncorrected "
    "(the client offers the source format)";

} // namespace

MULTIROOM_TEST(output_planner_covers_every_row_of_the_bitperfect_table)
{
    const Fixture fixture;

    // The six rows, reached through all eight combinations -- BitPerfect on a
    // device that cannot play the source is an error, grouped or not.
    checkRows(fixture.planner(),
              {
                  {AudioControl::BitPerfect, false, true, false, kBitperfect},

                  // The one way out of correction in a shared zone: it drifts,
                  // and status says why.
                  {AudioControl::BitPerfect, true, true, false,
                   "44100 Hz 2ch int24in32 -> 44100 Hz 2ch int24in32: bitperfect, uncorrected "
                   "(the client offers the source format; the room's Audio Control is bitPerfect)"},

                  // Alone, with nothing asking for a conversion: bitperfect.
                  {AudioControl::Auto, false, true, false, kBitperfect},

                  // Resampled because it has to be, and corrected because the
                  // converter is there anyway.
                  {AudioControl::Auto, false, false, true,
                   "44100 Hz 2ch int24in32 -> 48000 Hz 2ch int24in32: resampled by counting, "
                   "drift corrected (the client does not offer the source format)"},

                  // The one row that resamples a room which did not need it:
                  // rooms that drift apart are not one zone.
                  {AudioControl::Auto, true, true, true,
                   "44100 Hz 2ch int24in32 -> 44100 Hz 2ch int24in32: resampled by counting, "
                   "drift corrected (the room shares its zone with other rooms and is drift "
                   "corrected)"},
                  {AudioControl::Auto, true, false, true,
                   "44100 Hz 2ch int24in32 -> 48000 Hz 2ch int24in32: resampled by counting, "
                   "drift corrected (the room shares its zone with other rooms and is drift "
                   "corrected)"},
              });

    // BitPerfect on a device that cannot play the source exactly, both ways round.
    for (const bool grouped : {false, true})
    {
        CHECK_ERR(fixture.planner().plan(kSource, RoomAudioSettings{AudioControl::BitPerfect, grouped},
                                         mismatching()),
                  PlanError::BitperfectUnavailable);
    }
}

MULTIROOM_TEST(output_planner_leaves_bitperfect_for_an_equalizer_unless_it_is_forced)
{
    const Fixture fixture;
    RoomAudioSettings settings{AudioControl::Auto, false};
    settings.equalized = true;

    // Auto, alone, the device takes the source: resampled all the same, at the
    // source's own rate, because only a resampled path runs the equalizer.
    const auto equalized = fixture.planner().plan(kSource, settings, matching());
    CHECK_OK(equalized);
    CHECK(equalized->mode == OutputMode::Resampled);
    CHECK(equalized->reason == PlanReason::Equalized);
    CHECK_EQ(describePlan(*equalized),
             std::string("44100 Hz 2ch int24in32 -> 44100 Hz 2ch int24in32: resampled by counting, "
                         "drift corrected (the room's equalizer is on)"));

    // Shared zone too: the zone's reason is the one reported.
    settings.grouped = true;
    const auto grouped = fixture.planner().plan(kSource, settings, matching());
    CHECK_OK(grouped);
    CHECK(grouped->reason == PlanReason::Grouped);

    // Forced bitperfect wins: the equalizer stays on and is not applied.
    const RoomAudioSettings forced{AudioControl::BitPerfect, false, true};
    const auto kept = fixture.planner().plan(kSource, forced, matching());
    CHECK_OK(kept);
    CHECK_EQ(describePlan(*kept), std::string(kBitperfect));

    // A server with no converter cannot run it, and the room stays bitperfect.
    RoomAudioSettings alone{AudioControl::Auto, false};
    alone.equalized = true;
    const auto none = OutputPlanner::withoutResampler().plan(kSource, alone, matching());
    CHECK_OK(none);
    CHECK(none->mode == OutputMode::BitPerfect);
}

MULTIROOM_TEST(output_planner_corrects_nothing_with_a_fixed_ratio_resampler_and_says_why)
{
    const Fixture variable;
    CHECK(variable.planner().correctionUnavailable() == DriftDisabledReason::NotDisabled);

    // Naming a fixed-ratio implementation disables correction for every room,
    // and the server starts. This is the one place a configuration is resolved
    // rather than refused, and it earns that by being visible per room.
    const Fixture fixed("fixed", false);
    CHECK(fixed.planner().correctionUnavailable() == DriftDisabledReason::FixedRatioResampler);

    // And the case that makes that matter: a grouped room with a matching
    // format stays bitperfect, instead of being resampled to chase a
    // correction that never arrives.
    checkRows(fixed.planner(),
              {
                  {AudioControl::Auto, true, true, false,
                   "44100 Hz 2ch int24in32 -> 44100 Hz 2ch int24in32: bitperfect, uncorrected "
                   "(the client offers the source format; the configured resampler cannot vary a "
                   "ratio)"},
                  {AudioControl::Auto, false, false, false,
                   "44100 Hz 2ch int24in32 -> 48000 Hz 2ch int24in32: resampled by fixed, "
                   "uncorrected (the client does not offer the source format; the configured "
                   "resampler cannot vary a ratio)"},
                  // Alone and bitperfect, nobody asked: nothing to say.
                  {AudioControl::Auto, false, true, false, kBitperfect},
              });
}

MULTIROOM_TEST(output_planner_treats_a_lossless_repack_as_bitperfect)
{
    const Fixture fixture;
    const RoomAudioSettings settings =
        RoomAudioSettings{AudioControl::BitPerfect};

    // Section 2: the two 24-bit layouts carry the same twenty-four bits, so a
    // device that offers only one of them is not disqualified.
    struct Case
    {
        SampleType source;
        std::vector<SampleType> offered;
        SampleType expected;
    };

    const Case cases[] = {
        // An identical layout is preferred over an equivalent one.
        {SampleType::Int24, {SampleType::Int24In32, SampleType::Int24}, SampleType::Int24},
        {SampleType::Int24In32, {SampleType::Int24, SampleType::Int24In32},
         SampleType::Int24In32},

        // Repacking, both ways.
        {SampleType::Int24, {SampleType::Int24In32}, SampleType::Int24In32},
        {SampleType::Int24In32, {SampleType::Int24}, SampleType::Int24},

        // Widening is lossless too, and the narrowest container that holds the
        // source's bits wins: the extra bits carry nothing and cost bandwidth.
        {SampleType::Int16, {SampleType::Int32}, SampleType::Int32},
        {SampleType::Int16, {SampleType::Int32, SampleType::Int24}, SampleType::Int24},
        {SampleType::Int16, {SampleType::Int24In32, SampleType::Int24}, SampleType::Int24},
        {SampleType::Int24, {SampleType::Int32}, SampleType::Int32},
    };

    for (const Case& test : cases)
    {
        std::vector<AudioFormat> offered;
        for (const SampleType type : test.offered)
            offered.push_back(format(44100, 2, type));

        const auto plan =
            fixture.planner().plan(format(44100, 2, test.source), settings, capabilities(offered));
        CHECK_OK(plan);
        CHECK(plan->mode == OutputMode::BitPerfect);
        CHECK(plan->outputFormat == format(44100, 2, test.expected));
        CHECK(plan->reason == (test.source == test.expected ? PlanReason::SourceFormatOffered
                                                            : PlanReason::SourceFormatRepacked));
    }
}

MULTIROOM_TEST(output_planner_refuses_a_repack_that_would_change_a_sample)
{
    const Fixture fixture;
    const RoomAudioSettings forced =
        RoomAudioSettings{AudioControl::BitPerfect};

    // Narrowing loses bits, and crossing between integer and float is a change
    // of representation rather than of layout -- phase 7's direct path is
    // allowed a layout change and nothing else.
    const std::pair<SampleType, SampleType> refused[] = {
        {SampleType::Int32, SampleType::Int24},
        {SampleType::Int32, SampleType::Int24In32},
        {SampleType::Int24, SampleType::Int16},
        {SampleType::Int24, SampleType::Float32},
        {SampleType::Int16, SampleType::Float32},
        {SampleType::Float32, SampleType::Int32},
        {SampleType::Float32, SampleType::Int24},
    };

    for (const auto& pair : refused)
    {
        CHECK(!isValuePreserving(pair.first, pair.second));

        const ClientCapabilities caps = capabilities({format(44100, 2, pair.second)});
        CHECK_ERR(fixture.planner().plan(format(44100, 2, pair.first), forced, caps),
                  PlanError::BitperfectUnavailable);

        // The same source through a room that allows conversion is served, at
        // the same rate: the depth change is the conversion.
        const RoomAudioSettings when =
            RoomAudioSettings{AudioControl::Auto};
        const auto plan = fixture.planner().plan(format(44100, 2, pair.first), when, caps);
        CHECK_OK(plan);
            CHECK(plan->mode == OutputMode::Resampled);
    }

    // And every pair the predicate does allow is reflexive and widening only.
    CHECK(isValuePreserving(SampleType::Float32, SampleType::Float32));
    CHECK(isValuePreserving(SampleType::Int24, SampleType::Int24In32));
    CHECK(isValuePreserving(SampleType::Int24In32, SampleType::Int24));
    CHECK(isValuePreserving(SampleType::Int16, SampleType::Int32));
}

MULTIROOM_TEST(output_planner_prefers_an_integer_related_rate_then_the_hint_then_the_highest)
{
    const Fixture fixture;
    const RoomAudioSettings settings =
        RoomAudioSettings{AudioControl::Auto};

    const auto rateFor = [&](std::vector<uint32_t> rates, uint32_t preferred) {
        std::vector<AudioFormat> offered;
        for (const uint32_t rate : rates)
            offered.push_back(format(rate, 2, SampleType::Int32));
        const auto plan = fixture.planner().plan(kSource, settings, capabilities(offered, preferred));
        CHECK_OK(plan);
        return plan ? plan->outputFormat.sampleRate : 0u;
    };

    // An integer multiple beats the hint: both vendored libraries take a
    // different and cheaper path for a small exact ratio, which is a quality
    // argument and not only a cost one.
    CHECK_EQ(rateFor({48000, 88200, 96000}, 48000), 88200u);

    // The smallest multiple, because past the first there is nothing further
    // to preserve.
    CHECK_EQ(rateFor({88200, 176400}, 0), 88200u);

    // A multiple beats a divisor, which would throw bandwidth away.
    CHECK_EQ(rateFor({22050, 88200}, 22050), 88200u);

    // A divisor still beats the hint and the highest.
    CHECK_EQ(rateFor({48000, 22050}, 48000), 22050u);

    // With no integer relation the hint wins...
    CHECK_EQ(rateFor({48000, 96000}, 48000), 48000u);
    CHECK_EQ(rateFor({48000, 96000}, 96000), 96000u);

    // ...and with no hint either, the highest the device offers.
    CHECK_EQ(rateFor({48000, 96000, 192000}, 0), 192000u);
}

MULTIROOM_TEST(output_planner_prefers_the_most_bits_at_or_above_the_source_depth)
{
    const Fixture fixture;
    const RoomAudioSettings settings =
        RoomAudioSettings{AudioControl::Auto};

    const auto typeFor = [&](SampleType source, std::vector<SampleType> offered) {
        std::vector<AudioFormat> formats;
        for (const SampleType type : offered)
            formats.push_back(format(48000, 2, type));
        const auto plan =
            fixture.planner().plan(format(44100, 2, source), settings, capabilities(formats));
        CHECK_OK(plan);
        return plan ? plan->outputFormat.sampleType : SampleType::Int16;
    };

    // The samples a converter produces are not the source's samples, so bits
    // above the source's depth are not headroom that goes to waste: they are
    // what the rounding at the output end costs less of.
    CHECK(typeFor(SampleType::Int24, {SampleType::Int16, SampleType::Int24,
                                      SampleType::Int32}) == SampleType::Int32);

    // Integer beats float at the same width: a float32 mantissa carries 24
    // bits where an int32 carries 32.
    CHECK(typeFor(SampleType::Int24, {SampleType::Float32, SampleType::Int32}) ==
          SampleType::Int32);

    // At the same depth the narrower container wins -- equally exact, smaller
    // on the wire.
    CHECK(typeFor(SampleType::Int16, {SampleType::Int24In32, SampleType::Int24}) ==
          SampleType::Int24);

    // Depth is reduced only where the client offers nothing else, and then by
    // as little as possible.
    CHECK(typeFor(SampleType::Int32, {SampleType::Int16, SampleType::Int24}) ==
          SampleType::Int24);
}

MULTIROOM_TEST(output_planner_requires_the_source_channel_count)
{
    // Nothing in this server mixes or reorders channels -- IAudioResampler
    // converts one count to itself -- so a plan naming a different one would
    // be a plan the pipeline cannot execute.
    const Fixture fixture;
    const RoomAudioSettings settings =
        RoomAudioSettings{AudioControl::Auto};

    const ClientCapabilities monoOnly =
        capabilities({format(44100, 1, SampleType::Int24In32),
                      format(48000, 1, SampleType::Int24In32)},
                     48000);
    CHECK_ERR(fixture.planner().plan(kSource, settings, monoOnly),
              PlanError::ChannelCountNotOffered);

    // The same list with the source's count present is served.
    const ClientCapabilities both =
        capabilities({format(48000, 1, SampleType::Int24In32),
                      format(48000, 2, SampleType::Int24In32)},
                     48000);
    const auto plan = fixture.planner().plan(kSource, settings, both);
    CHECK_OK(plan);
    CHECK_EQ(plan->outputFormat.channels, uint16_t{2});
}

MULTIROOM_TEST(output_planner_never_constructs_a_resampler)
{
    // Criterion 5 of the definition of done. A plan is data: the converter is
    // built by the processor of phase 7, and a bitperfect room never has one
    // at all -- bitperfect is not a ratio of one, it is no resampler.
    Fixture fixture;
    const RoomAudioSettings settings{AudioControl::Auto};

    const auto bitperfect = fixture.planner().plan(kSource, settings, matching());
    CHECK_OK(bitperfect);
    CHECK(bitperfect->mode == OutputMode::BitPerfect);
    CHECK_EQ(fixture.creations(), 0);

    // The capabilities a resampled room is validated against are answered
    // without constructing anything either, which is what lets a configuration
    // be refused for a client that may never connect.
    const auto resampled = fixture.planner().plan(kSource, settings, mismatching());
    CHECK_OK(resampled);
    CHECK(resampled->mode == OutputMode::Resampled);
    CHECK_EQ(fixture.creations(), 0);

    // And the counter is not simply broken.
    CHECK_OK(fixture.registry().create("counting"));
    CHECK_EQ(fixture.creations(), 1);
}

MULTIROOM_TEST(output_planner_hands_the_drift_flag_to_the_converter)
{
    // The flag selects the engine and not only the ratio: SoXR builds its
    // constant-rate engine for a room that is not corrected and its
    // variable-rate one for a room that is, and the difference is 15.5 dB.
    const Fixture fixture;

    const RoomAudioSettings settings{AudioControl::Auto};
    const auto withDrift = fixture.planner().plan(kSource, settings, mismatching());
    CHECK_OK(withDrift);
    const ResamplerConfig driftConfig = withDrift->resamplerConfig();
    CHECK(driftConfig.requiresVariableRatio);
    CHECK_EQ(driftConfig.inputRate, 44100u);
    CHECK_EQ(driftConfig.outputRate, 48000u);
    CHECK_EQ(driftConfig.channels, uint16_t{2});
    CHECK(driftConfig.isValid());
    CHECK_NEAR(driftConfig.effectiveRatio(), 48000.0 / 44100.0, 1e-12);

    // Only a converter that takes its ratio once leaves a resampled room
    // uncorrected.
    const Fixture fixed("fixed", false);
    const auto withoutDrift = fixed.planner().plan(kSource, settings, mismatching());
    CHECK_OK(withoutDrift);
    CHECK(!withoutDrift->resamplerConfig().requiresVariableRatio);

    // A bitperfect plan has no converter to configure, and says so by handing
    // back a configuration that is not valid rather than one of ratio 1.0.
    const auto bitperfect = fixture.planner().plan(kSource, settings, matching());
    CHECK_OK(bitperfect);
    CHECK(!bitperfect->resamplerConfig().isValid());
    CHECK(bitperfect->resamplerId.empty());
}

MULTIROOM_TEST(output_planner_replans_a_room_whose_driver_granted_another_format)
{
    const Fixture fixture;
    const RoomAudioSettings settings =
        RoomAudioSettings{AudioControl::Auto};

    const auto requested = fixture.planner().plan(kSource, settings, matching());
    CHECK_OK(requested);
    CHECK(requested->mode == OutputMode::BitPerfect);

    // The format the device actually opened is the only one this room can be
    // sent, so it is the whole of its capability list for the second decision.
    const auto granted = fixture.planner().replanForGrantedFormat(
        *requested, settings, format(48000, 2, SampleType::Int24In32), matching());
    CHECK_OK(granted);
    CHECK(granted->mode == OutputMode::Resampled);
    CHECK(granted->reason == PlanReason::DriverGrantedDifferentFormat);
    CHECK(granted->outputFormat == format(48000, 2, SampleType::Int24In32));

    // A driver that granted what it was asked for is not a re-negotiation.
    const auto unchanged =
        fixture.planner().replanForGrantedFormat(*requested, settings, requested->outputFormat,
                                                 matching());
    CHECK_OK(unchanged);
    CHECK(unchanged->reason == requested->reason);
    CHECK(unchanged->mode == OutputMode::BitPerfect);

    // A granted format that still carries the source's sample values leaves
    // the room bitperfect, and says the plan changed anyway: a false
    // bitPerfect is the one lie this server must not tell, and so is a false
    // resampled.
    const auto repacked = fixture.planner().replanForGrantedFormat(
        *requested, settings, format(44100, 2, SampleType::Int24), matching());
    CHECK_OK(repacked);
    CHECK(repacked->mode == OutputMode::BitPerfect);
    CHECK(repacked->reason == PlanReason::DriverGrantedDifferentFormat);

    // Force cannot be honoured by a driver that substituted a rate, and that
    // is that room's error rather than the zone's.
    const RoomAudioSettings forced =
        RoomAudioSettings{AudioControl::BitPerfect};
    const auto forcedPlan = fixture.planner().plan(kSource, forced, matching());
    CHECK_OK(forcedPlan);
    CHECK_ERR(fixture.planner().replanForGrantedFormat(*forcedPlan, forced,
                                                       format(48000, 2, SampleType::Int24In32),
                                                       matching()),
              PlanError::BitperfectUnavailable);
}

MULTIROOM_TEST(output_planner_isolates_one_room_that_cannot_be_served)
{
    // Fault isolation, stated once in the roadmap and enforced here: a room
    // configured Force on a device that turns out to do 48 kHz only is a
    // configuration mistake, and the useful behaviour is to say so and keep
    // the house playing.
    const Fixture fixture;

    const RoomAudioSettings forced =
        RoomAudioSettings{AudioControl::BitPerfect};
    const auto failed = fixture.planner().plan(kSource, forced, mismatching());
    CHECK_ERR(failed, PlanError::BitperfectUnavailable);

    // The detail names both sides, because "this room is in error" without the
    // two format lists sends an operator to the wrong place.
    const std::string detail = describe(failed.error());
    CHECK(detail.find("44100 Hz 2ch int24in32") != std::string::npos);
    CHECK(detail.find("48000 Hz 2ch int24in32") != std::string::npos);

    // The next room, on the same source and the same server, is unaffected.
    const RoomAudioSettings neighbour =
        RoomAudioSettings{AudioControl::Auto};
    const auto served = fixture.planner().plan(kSource, neighbour, mismatching());
    CHECK_OK(served);
        CHECK(served->mode == OutputMode::Resampled);

    // And so is a third that is bitperfect on a device that does take it.
    const auto bitperfect = fixture.planner().plan(kSource, forced, matching());
    CHECK_OK(bitperfect);
        CHECK(bitperfect->mode == OutputMode::BitPerfect);
}

MULTIROOM_TEST(output_planner_refuses_a_resampler_that_cannot_do_the_conversion)
{
    // Asked of the capabilities rather than of an instance, so a client that
    // never connects still invalidates the configuration.
    const Fixture narrow("narrow", true, 2, 0.5, 2.0);
    const RoomAudioSettings settings =
        RoomAudioSettings{AudioControl::Auto};

    const auto tooFar = narrow.planner().plan(
        kSource, settings, capabilities({format(352800, 2, SampleType::Int32)}));
    CHECK_ERR(tooFar, PlanError::UnsupportedRatio);
    CHECK(describe(tooFar.error()).find("8.000000") != std::string::npos);

    const auto tooMany = narrow.planner().plan(
        format(44100, 4, SampleType::Int24In32), settings,
        capabilities({format(48000, 4, SampleType::Int24In32)}));
    CHECK_ERR(tooMany, PlanError::UnsupportedChannels);

    // A bitperfect room is served by a resampler that could do neither,
    // because it is not served by a resampler at all.
    const auto bitperfect = narrow.planner().plan(
        format(44100, 4, SampleType::Int24In32), settings,
        capabilities({format(44100, 4, SampleType::Int24In32)}));
    CHECK_OK(bitperfect);
        CHECK(bitperfect->mode == OutputMode::BitPerfect);
}

MULTIROOM_TEST(output_planner_refuses_a_resampler_that_is_not_in_this_build)
{
    // A build with both resampler options off registers nothing, and this is
    // where a deployment naming one meets a refused configuration rather than
    // a silent resample.
    ResamplerRegistry empty;
    const auto none = OutputPlanner::create(empty, "soxr");
    CHECK_ERR(none, PlanError::UnknownResampler);
    CHECK(describe(none.error()).find("soxr") != std::string::npos);

    int creations = 0;
    ResamplerRegistry one;
    CHECK_OK(one.add(std::make_unique<CountingFactory>("r8brain", false, &creations, uint16_t{8}, 0.1, 10.0)));

    const auto missing = OutputPlanner::create(one, "soxr");
    CHECK_ERR(missing, PlanError::UnknownResampler);

    // The message names what this build does have, which is what a config
    // naming a resampler a build option left out needs to see.
    CHECK(describe(missing.error()).find("r8brain") != std::string::npos);
}

MULTIROOM_TEST(output_planner_refuses_a_client_that_reported_nothing)
{
    const Fixture fixture;
    const RoomAudioSettings settings =
        RoomAudioSettings{AudioControl::Auto};

    // Section 12: a client must not be sent audio before it has reported its
    // capabilities, so an empty list is a planning error and not a room with
    // no options.
    CHECK_ERR(fixture.planner().plan(kSource, settings, capabilities({})),
              PlanError::NoFormatsOffered);

    CHECK_ERR(fixture.planner().plan(kSource, settings,
                                     capabilities({format(0, 2, SampleType::Int32)})),
              PlanError::NoFormatsOffered);

    CHECK_ERR(fixture.planner().plan(format(0, 2, SampleType::Int24In32), settings, matching()),
              PlanError::InvalidSourceFormat);
    CHECK_ERR(fixture.planner().plan(format(44100, 0, SampleType::Int24In32), settings, matching()),
              PlanError::InvalidSourceFormat);
}

// resampler.configure swaps the server's planner while zones plan rooms on
// their own threads: every plan is made whole by one of them, never a mix.
MULTIROOM_TEST(shared_planner_swaps_while_rooms_are_planned)
{
    const Fixture variable;
    const Fixture fixed("fixed", false);
    SharedPlanner shared(variable.planner());

    std::atomic<bool> stop{false};
    std::atomic<int> plans{0};
    std::atomic<int> mixed{0};
    std::thread planning([&] {
        const RoomAudioSettings grouped{AudioControl::Auto, true};
        while (!stop.load())
        {
            const std::shared_ptr<const OutputPlanner> planner = shared.get();
            const auto plan = planner->plan(kSource, grouped, mismatching());
            if (!plan || plan->resamplerId != planner->resamplerId() ||
                plan->driftCorrected() != planner->resamplerCapabilities().variableRatio)
                ++mixed;
            ++plans;
        }
    });
    for (int i = 0; i < 2000 || plans.load() < 100; ++i)
        shared.set(i % 2 == 0 ? fixed.planner() : variable.planner());
    stop = true;
    planning.join();

    CHECK_EQ(mixed.load(), 0);
}

MULTIROOM_TEST(output_planner_logs_the_fixed_ratio_resolution_once_at_startup)
{
    // Choosing a fixed-ratio converter is choosing rooms that drift. It is
    // resolved rather than refused, and it earns that by being visible in the
    // log as well as in status.
    const multiroomtest::TempDirectory temp("planner");
    const std::string path = temp.file("server.log");

    const LogLevel previous = log::currentLevel();
    log::setLevel(LogLevel::Info);
    CHECK(log::setLogFile(path));

    int creations = 0;
    ResamplerRegistry registry;
    CHECK_OK(
        registry.add(std::make_unique<CountingFactory>("r8brain", false, &creations, uint16_t{8}, 0.1, 10.0)));
    CHECK_OK(OutputPlanner::create(registry, "r8brain"));

    log::closeLogFile();
    log::setLevel(previous);

    const auto contents = fileutil::readTextFile(path);
    CHECK_OK(contents);
    CHECK(contents->find(
              "resampler r8brain cannot vary a ratio; drift correction is disabled for all rooms") !=
          std::string::npos);
}

MULTIROOM_TEST(output_planner_without_a_resampler_still_serves_every_room_that_matches)
{
    // A build with both resampler options off registers nothing, so there is
    // no id to name. That is not a server that cannot plan -- it is one that
    // can only plan bitperfect rooms, and the planner is still the single
    // place that says which rooms those are.
    const LogLevel previous = log::currentLevel();
    log::setLevel(LogLevel::Error);
    const OutputPlanner planner = OutputPlanner::withoutResampler();
    log::setLevel(previous);

    CHECK(!planner.hasResampler());
    CHECK_EQ(planner.resamplerId(), std::string());

    const RoomAudioSettings settings =
        RoomAudioSettings{AudioControl::Auto};

    // Every bitperfect row of the table is unchanged, including the repack.
    const auto exact = planner.plan(kSource, settings, matching());
    CHECK_OK(exact);
    CHECK(exact->mode == OutputMode::BitPerfect);
    CHECK(exact->resamplerId.empty());

    const auto repacked =
        planner.plan(kSource, settings,
                     capabilities({format(44100, 2, SampleType::Int32)}, 44100));
    CHECK_OK(repacked);
    CHECK(repacked->mode == OutputMode::BitPerfect);
    CHECK(repacked->reason == PlanReason::SourceFormatRepacked);

    // And a room that would have to be converted is that room's failure, by
    // name, with the zone still playing for the others.
    const auto converted = planner.plan(kSource, settings, mismatching());
    CHECK_ERR(converted, PlanError::NoResampler);

    // bitperfect Force answers before the converter question is reached, so
    // its reason stays the one an operator configured.
    const RoomAudioSettings forced =
        RoomAudioSettings{AudioControl::BitPerfect};
    CHECK_ERR(planner.plan(kSource, forced, mismatching()), PlanError::BitperfectUnavailable);
}

MULTIROOM_TEST(output_planner_without_a_resampler_corrects_nothing_by_its_own_name)
{
    const LogLevel previous = log::currentLevel();
    log::setLevel(LogLevel::Error);
    const OutputPlanner planner = OutputPlanner::withoutResampler();
    log::setLevel(previous);

    // Correction is resampling (section 28), so a server with no converter
    // cannot do it -- and says that rather than "the configured resampler
    // cannot vary a ratio", which would name a resampler that does not exist.
    CHECK(planner.correctionUnavailable() == DriftDisabledReason::NoResampler);

    // A grouped room with a matching format stays bitperfect rather than
    // being refused for a correction that could never arrive.
    const auto plan = planner.plan(kSource, RoomAudioSettings{AudioControl::Auto, true}, matching());
    CHECK_OK(plan);
    CHECK(plan->mode == OutputMode::BitPerfect);
    CHECK(!plan->driftCorrected());
    CHECK(plan->driftDisabledBy == DriftDisabledReason::NoResampler);
}

// a room whose client trims its own
// clock is corrected through it, keeps its samples where its device takes the
// source, and needs no converter for the correction itself.
MULTIROOM_TEST(output_planner_corrects_a_trimmed_clock_without_resampling_for_drift)
{
    const Fixture fixture;
    const auto trimmed = [](ClientCapabilities caps) {
        caps.driftFeedback = true;
        caps.hardwareClock.available = true;
        caps.hardwareClock.minPpm = -500;
        caps.hardwareClock.maxPpm = 500;
        return caps;
    };

    // Grouped, Auto, the device takes the source: bitperfect and synchronized.
    RoomAudioSettings grouped{AudioControl::Auto, true};
    const auto kept = fixture.planner().plan(kSource, grouped, trimmed(matching()));
    CHECK_OK(kept);
    CHECK(kept->mode == OutputMode::BitPerfect);
    CHECK(kept->driftActuator == DriftActuator::ClientHardwareClock);
    CHECK(kept->driftCorrected());
    CHECK(!kept->resamplerConfig().requiresVariableRatio);
    CHECK_EQ(describePlan(*kept),
             std::string("44100 Hz 2ch int24in32 -> 44100 Hz 2ch int24in32: bitperfect, hardware "
                         "clock synchronized (the client offers the source format)"));

    // Forced bitperfect too, which resampling could never correct.
    const RoomAudioSettings forced{AudioControl::BitPerfect, true};
    const auto strict = fixture.planner().plan(kSource, forced, trimmed(matching()));
    CHECK_OK(strict);
    CHECK(strict->mode == OutputMode::BitPerfect);
    CHECK(strict->driftActuator == DriftActuator::ClientHardwareClock);
    const auto drifting = fixture.planner().plan(kSource, forced, matching());
    CHECK_OK(drifting);
    CHECK(drifting->driftActuator == DriftActuator::None);
    CHECK(drifting->driftDisabledBy == DriftDisabledReason::BitperfectForced);

    // A device that cannot play the source is still converted -- at a fixed
    // ratio, because the clock and not the converter takes the correction.
    const auto converted = fixture.planner().plan(kSource, grouped, trimmed(mismatching()));
    CHECK_OK(converted);
    CHECK(converted->mode == OutputMode::Resampled);
    CHECK(converted->driftActuator == DriftActuator::ClientHardwareClock);
    CHECK(!converted->resamplerConfig().requiresVariableRatio);

    // So is a room whose equalizer is on.
    RoomAudioSettings equalized = grouped;
    equalized.equalized = true;
    const auto shaped = fixture.planner().plan(kSource, equalized, trimmed(matching()));
    CHECK_OK(shaped);
    CHECK(shaped->mode == OutputMode::Resampled);
    CHECK(shaped->driftActuator == DriftActuator::ClientHardwareClock);

    // Alone and exact: nothing to correct, as before.
    const RoomAudioSettings alone{AudioControl::Auto, false};
    const auto solo = fixture.planner().plan(kSource, alone, trimmed(matching()));
    CHECK_OK(solo);
    CHECK(solo->driftActuator == DriftActuator::None);

    // A trim without the feedback to drive it is no actuator.
    ClientCapabilities unread = trimmed(matching());
    unread.driftFeedback = false;
    const auto resampled = fixture.planner().plan(kSource, grouped, unread);
    CHECK_OK(resampled);
    CHECK(resampled->driftActuator == DriftActuator::ServerResampling);
    CHECK(resampled->resamplerConfig().requiresVariableRatio);

    // A server with no converter corrects the trimmed room all the same, and
    // still cannot convert one whose device needs it.
    const OutputPlanner none = OutputPlanner::withoutResampler();
    const auto plain = none.plan(kSource, grouped, trimmed(matching()));
    CHECK_OK(plain);
    CHECK(plain->driftActuator == DriftActuator::ClientHardwareClock);
    CHECK_ERR(none.plan(kSource, grouped, trimmed(mismatching())), PlanError::NoResampler);

    // A device that granted another format keeps its clock.
    const auto granted = fixture.planner().replanForGrantedFormat(
        *kept, grouped, format(48000, 2, SampleType::Int24In32), trimmed(matching()));
    CHECK_OK(granted);
    CHECK(granted->mode == OutputMode::Resampled);
    CHECK(granted->driftActuator == DriftActuator::ClientHardwareClock);
}
