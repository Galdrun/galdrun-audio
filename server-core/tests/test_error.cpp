#include "TestSupport.h"

#include "app/CommandLine.h"
#include "audio/BlockPool.h"
#include "config/ServerConfig.h"
#include "core/Units.h"
#include "util/FileUtil.h"

using namespace multiroom;

MULTIROOM_TEST(error_describe_puts_the_enum_and_the_detail_together)
{
    // An enum on its own cannot say which key, path or format was wrong, which
    // is why a failure carries both.
    const Failure<ConfigError> named{ConfigError::UnknownKey, "audio.blockCont"};
    CHECK_EQ(describe(named), std::string("UnknownKey: audio.blockCont"));

    const Failure<ConfigError> bare{ConfigError::Missing, ""};
    CHECK_EQ(describe(bare), std::string("Missing"));
}

MULTIROOM_TEST(error_every_module_names_its_own_codes)
{
    // errorName is found by argument-dependent lookup, so each module supplies
    // one beside its enum and nothing is registered centrally.
    CHECK_EQ(std::string(errorName(FrameMathError::Overflow)), std::string("Overflow"));
    CHECK_EQ(std::string(errorName(BlockPoolError::Exhausted)), std::string("Exhausted"));
    CHECK_EQ(std::string(errorName(BlockError::TooLarge)), std::string("TooLarge"));
    CHECK_EQ(std::string(errorName(ConfigError::WriteFailed)), std::string("WriteFailed"));
    CHECK_EQ(std::string(errorName(FileError::NotFound)), std::string("NotFound"));
    CHECK_EQ(std::string(errorName(CommandLineError::MissingValue)), std::string("MissingValue"));
}

MULTIROOM_TEST(error_fail_builds_an_unexpected_of_the_right_type)
{
    const auto make = []() -> Result<int, BlockPoolError> {
        return fail(BlockPoolError::Exhausted, "all 3 blocks are in use");
    };

    const auto result = make();
    CHECK(!result.has_value());
    CHECK(result.error().code == BlockPoolError::Exhausted);
    CHECK_EQ(result.error().detail, std::string("all 3 blocks are in use"));
}

MULTIROOM_TEST(error_status_carries_no_value)
{
    const auto ok = []() -> Status<ConfigError> { return {}; };
    const auto bad = []() -> Status<ConfigError> {
        return fail(ConfigError::OutOfRange, "audio.maxChannels is 99");
    };

    CHECK_OK(ok());
    CHECK_ERR(bad(), ConfigError::OutOfRange);
    CHECK_EQ(describe(bad().error()),
             std::string("OutOfRange: audio.maxChannels is 99"));
}
