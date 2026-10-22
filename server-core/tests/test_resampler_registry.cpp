#include "TestSupport.h"

#include "resampler/FakeResampler.h"
#include "resampler/ResamplerRegistry.h"

#include <memory>
#include <string>
#include <vector>

using namespace multiroom;

namespace {

// A stand-in for r8brain, which takes its ratio at construction and cannot
// vary it. Phase 3 brings the real one and it is off by default, so the rule
// the registry exists to enforce -- a fixed-ratio implementation refused for
// drift correction -- needs something here to enforce it against.
class StubResampler final : public IAudioResampler
{
public:
    explicit StubResampler(const ResamplerCapabilities& caps) : caps_(caps) {}

    const ResamplerCapabilities& capabilities() const noexcept override { return caps_; }

    Status<ResamplerError> configure(const ResamplerConfig& config) override
    {
        if (!config.isValid())
            return fail(ResamplerError::InvalidConfig, "stub");
        ratio_ = config.effectiveRatio();
        return {};
    }

    bool configured() const noexcept override { return ratio_ > 0.0; }
    double ratio() const noexcept override { return ratio_; }

    Status<ResamplerError> setRatio(double) override
    {
        return fail(ResamplerError::FixedRatioOnly, caps_.id + " takes its ratio once");
    }

    Result<ProcessResult, ResamplerError> process(std::span<const double>,
                                                  std::span<double>) override
    {
        return fail(ResamplerError::NotConfigured, "the stub does not convert");
    }

    void reset() noexcept override {}
    double latencyFrames() const noexcept override { return 0.0; }

private:
    ResamplerCapabilities caps_;
    double ratio_ = 0.0;
};

class StubFactory final : public IResamplerFactory
{
public:
    StubFactory(std::string id, bool variableRatio)
    {
        caps_.id = std::move(id);
        caps_.variableRatio = variableRatio;
        caps_.maxChannels = 2;
        caps_.minRatio = 0.5;
        caps_.maxRatio = 2.0;
    }

    const ResamplerCapabilities& capabilities() const noexcept override { return caps_; }

    std::unique_ptr<IAudioResampler> create() const override
    {
        return std::make_unique<StubResampler>(caps_);
    }

private:
    ResamplerCapabilities caps_;
};

} // namespace

MULTIROOM_TEST(resampler_registry_registers_and_looks_up)
{
    ResamplerRegistry registry;
    CHECK_EQ(registry.size(), size_t{0});
    CHECK(!registry.contains("fake"));

    CHECK_OK(registry.add(std::make_unique<FakeResamplerFactory>()));
    CHECK_EQ(registry.size(), size_t{1});
    CHECK(registry.contains("fake"));

    const auto caps = registry.capabilities("fake");
    CHECK_OK(caps);
    CHECK_EQ(caps->id, std::string("fake"));
    CHECK(caps->variableRatio);

    const auto made = registry.create("fake");
    CHECK_OK(made);
    CHECK(*made != nullptr);

    // What comes back is usable through the interface and nothing else --
    // which is the whole point of the registry existing.
    ResamplerConfig config;
    config.inputRate = 44100;
    config.outputRate = 48000;
    config.channels = 2;
    CHECK_OK((*made)->configure(config));
    CHECK_NEAR((*made)->ratio(), 48000.0 / 44100.0, 1e-12);
}

MULTIROOM_TEST(resampler_registry_refuses_an_unknown_id_and_names_the_alternatives)
{
    ResamplerRegistry registry;

    // Empty is still an honest answer rather than a crash.
    CHECK_ERR(registry.create("soxr"), ResamplerError::UnknownId);
    CHECK_ERR(registry.capabilities("soxr"), ResamplerError::UnknownId);

    CHECK_OK(registry.add(std::make_unique<FakeResamplerFactory>()));
    CHECK_OK(registry.add(std::make_unique<StubFactory>("r8brain", false)));

    const auto missing = registry.create("soxr");
    CHECK_ERR(missing, ResamplerError::UnknownId);

    // A config naming a resampler that is not in this build is the commonest
    // way to meet this error, so the message says what *is* in this build.
    const std::string detail = describe(missing.error());
    CHECK(detail.find("soxr") != std::string::npos);
    CHECK(detail.find("fake") != std::string::npos);
    CHECK(detail.find("r8brain") != std::string::npos);
}

MULTIROOM_TEST(resampler_registry_refuses_a_duplicate_id)
{
    // Which of the two a room got would otherwise depend on registration
    // order, and an id is the only name a resampler has.
    ResamplerRegistry registry;
    CHECK_OK(registry.add(std::make_unique<FakeResamplerFactory>()));
    CHECK_ERR(registry.add(std::make_unique<FakeResamplerFactory>()),
              ResamplerError::DuplicateId);
    CHECK_EQ(registry.size(), size_t{1});

    CHECK_ERR(registry.add(std::make_unique<StubFactory>("", true)), ResamplerError::UnknownId);
    CHECK_ERR(registry.add(nullptr), ResamplerError::UnknownId);
    CHECK_EQ(registry.size(), size_t{1});
}

MULTIROOM_TEST(resampler_registry_ids_are_sorted)
{
    // They are printed -- in a startup line, in status, in an error detail --
    // and a listing that reorders itself between runs is a diff nobody wants.
    ResamplerRegistry registry;
    CHECK_OK(registry.add(std::make_unique<StubFactory>("soxr", true)));
    CHECK_OK(registry.add(std::make_unique<FakeResamplerFactory>()));
    CHECK_OK(registry.add(std::make_unique<StubFactory>("r8brain", false)));

    const std::vector<std::string> ids = registry.ids();
    CHECK_EQ(ids.size(), size_t{3});
    CHECK_EQ(ids[0], std::string("fake"));
    CHECK_EQ(ids[1], std::string("r8brain"));
    CHECK_EQ(ids[2], std::string("soxr"));
}

MULTIROOM_TEST(resampler_registry_answers_capabilities_without_constructing_anything)
{
    // This is what OutputPlanner (phase 6) does at startup for a client that
    // may never connect: a configuration naming a fixed-ratio implementation
    // for drift correction has to fail then, with the id in the message, and
    // not hours later when the room first needs correcting.
    ResamplerRegistry registry;
    CHECK_OK(registry.add(std::make_unique<StubFactory>("r8brain", false)));
    CHECK_OK(registry.add(std::make_unique<FakeResamplerFactory>()));

    const auto fixed = registry.capabilities("r8brain");
    CHECK_OK(fixed);
    CHECK(!fixed->variableRatio);

    const auto variable = registry.capabilities("fake");
    CHECK_OK(variable);
    CHECK(variable->variableRatio);

    // And if one were used for drift anyway, the refusal is explicit rather
    // than a silent fallback to a fixed ratio.
    const auto made = registry.create("r8brain");
    CHECK_OK(made);
    ResamplerConfig config;
    config.inputRate = 48000;
    config.outputRate = 48000;
    config.channels = 2;
    CHECK_OK((*made)->configure(config));
    CHECK_ERR((*made)->setRatio(1.000030), ResamplerError::FixedRatioOnly);
}
