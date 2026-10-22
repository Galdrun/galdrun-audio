#include "TestSupport.h"

#include "audio/HardwareClock.h"
#include "audio/HardwareVolume.h"

#include <cmath>
#include <limits>

using namespace multiroom;

namespace {

HardwareVolumeInfo decibelControl(double minDb, double maxDb)
{
    HardwareVolumeInfo info;
    info.available = true;
    info.hasDecibels = true;
    info.minDb = minDb;
    info.maxDb = maxDb;
    info.maxStep = 255;
    return info;
}

bool near(double a, double b)
{
    return std::fabs(a - b) < 1e-9;
}

} // namespace

MULTIROOM_TEST(hardware_volume_unity_is_0_db_even_on_a_control_that_can_amplify)
{
    const HardwareVolumeInfo info = decibelControl(-102.0, 6.0);
    CHECK(near(hardware_volume::decibelsForGain(info, 1.0), 0.0));
    CHECK(near(hardware_volume::decibelsForGain(info, 4.0), 0.0));
    CHECK(near(hardware_volume::decibelsForGain(info, 0.5), 20.0 * std::log10(0.5)));
    CHECK(near(hardware_volume::gainForDecibels(info, 6.0), 1.0));
}

MULTIROOM_TEST(hardware_volume_unity_is_the_top_of_a_control_below_0_db)
{
    const HardwareVolumeInfo info = decibelControl(-60.0, -10.0);
    CHECK(near(hardware_volume::decibelsForGain(info, 1.0), -10.0));
    CHECK(near(hardware_volume::decibelsForGain(info, 0.1), -30.0));
    CHECK(near(hardware_volume::gainForDecibels(info, -30.0), 0.1));
}

MULTIROOM_TEST(hardware_volume_silence_is_the_bottom_of_the_range)
{
    const HardwareVolumeInfo info = decibelControl(-40.0, 0.0);
    CHECK(near(hardware_volume::decibelsForGain(info, 0.0), -40.0));
    CHECK(near(hardware_volume::decibelsForGain(info, 0.0001), -40.0));
    CHECK(near(hardware_volume::decibelsForGain(info, std::numeric_limits<double>::quiet_NaN()),
               -40.0));
    // What ALSA reports for a muted minimum.
    CHECK(near(hardware_volume::gainForDecibels(info, -99999.99), 0.0));
}

MULTIROOM_TEST(hardware_volume_without_decibels_is_linear_in_steps)
{
    HardwareVolumeInfo info;
    info.available = true;
    info.minStep = 10;
    info.maxStep = 110;
    CHECK_EQ(hardware_volume::stepForGain(info, 0.0), static_cast<int64_t>(10));
    CHECK_EQ(hardware_volume::stepForGain(info, 0.5), static_cast<int64_t>(60));
    CHECK_EQ(hardware_volume::stepForGain(info, 2.0), static_cast<int64_t>(110));
    CHECK(near(hardware_volume::gainForStep(info, 35), 0.25));
    CHECK(near(hardware_volume::gainForStep(info, 0), 0.0));
}

MULTIROOM_TEST(volume_path_follows_the_table_of_section_59)
{
    using matp::AudioControl;
    using matp::VolumeControlMode;
    struct Row
    {
        AudioControl audio;
        VolumeControlMode asked;
        bool hardware;
        VolumeControlMode effective;
        bool explained;
    };
    const Row rows[] = {
        // Bit-Perfect: hardware or nothing, and never software.
        {AudioControl::BitPerfect, VolumeControlMode::Auto, true, VolumeControlMode::Hardware, false},
        {AudioControl::BitPerfect, VolumeControlMode::Auto, false, VolumeControlMode::Disabled, true},
        {AudioControl::BitPerfect, VolumeControlMode::Hardware, false, VolumeControlMode::Disabled, true},
        {AudioControl::BitPerfect, VolumeControlMode::Software, true, VolumeControlMode::Disabled, true},
        {AudioControl::BitPerfect, VolumeControlMode::Disabled, true, VolumeControlMode::Disabled, false},
        // Auto: hardware first, software otherwise, all modes available.
        {AudioControl::Auto, VolumeControlMode::Auto, true, VolumeControlMode::Hardware, false},
        {AudioControl::Auto, VolumeControlMode::Software, true, VolumeControlMode::Software, false},
        {AudioControl::Auto, VolumeControlMode::Hardware, false, VolumeControlMode::Disabled, true},
        {AudioControl::Auto, VolumeControlMode::Auto, false, VolumeControlMode::Software, false},
        {AudioControl::Auto, VolumeControlMode::Disabled, false, VolumeControlMode::Disabled, false},
    };
    for (const Row& row : rows)
    {
        const VolumePath path = resolveVolumePath(row.audio, row.asked, row.hardware);
        CHECK(path.effective == row.effective);
        CHECK_EQ(path.reason.empty(), !row.explained);
    }
}

MULTIROOM_TEST(hardware_clock_setting_rounds_clamps_and_holds_near_the_current_step)
{
    HardwareClockInfo info;
    info.available = true;
    info.minPpm = -500;
    info.maxPpm = 500;

    hardware_clock::Setting setting = hardware_clock::settingFor(info, 12.4, 0);
    CHECK_EQ(setting.ppm, 12);
    CHECK(!setting.saturated);

    // Noise around the step the clock is on writes nothing.
    CHECK_EQ(hardware_clock::settingFor(info, 12.7, 12).ppm, 12);
    CHECK_EQ(hardware_clock::settingFor(info, 11.3, 12).ppm, 12);
    CHECK_EQ(hardware_clock::settingFor(info, 12.8, 12).ppm, 13);

    setting = hardware_clock::settingFor(info, 812.0, 12);
    CHECK_EQ(setting.ppm, 500);
    CHECK(setting.saturated);
    setting = hardware_clock::settingFor(info, -812.0, 12);
    CHECK_EQ(setting.ppm, -500);
    CHECK(setting.saturated);

    // A target that is not a number moves nothing.
    CHECK_EQ(hardware_clock::settingFor(info, std::nan(""), 7).ppm, 7);

    // A coarser step stays on it, inside the range.
    info.stepPpm = 4;
    CHECK_EQ(hardware_clock::settingFor(info, 9.9, 0).ppm, 8);
    info.maxPpm = 498;
    CHECK_EQ(hardware_clock::settingFor(info, 497.0, 0).ppm, 496);
}
