#include "StubBackend.h"
#include "TestSupport.h"

#include "app/ClientCommandHandler.h"
#include "audio/PlaybackSupervisor.h"
#include "audio/ToneGenerator.h"

#include <string>
#include <vector>

using namespace multiroom;
using multiroomtest::StubBackend;

namespace {

// Nanoseconds, so a test can walk the backoff without sleeping through it:
// poll() takes the time rather than reading the clock for exactly this.
uint64_t ms(uint64_t value)
{
    return value * 1000000ULL;
}

AudioEngine::Settings settingsFor(uint32_t rate)
{
    AudioEngine::Settings settings;
    settings.deviceId = "stub";
    settings.format.sampleRate = rate;
    settings.format.channels = 2;
    settings.format.sampleType = AudioFormat::SampleType::Int16;
    settings.accessMode = AudioAccessMode::Exclusive;
    return settings;
}

// Stands in for main's device selection: a target the test can take away and
// give back, the way an unplugged device behaves.
struct Target
{
    bool available = true;
    uint32_t sampleRate = 48000;
    int calls = 0;

    bool operator()(AudioEngine::Settings& settings, std::string& error)
    {
        ++calls;
        if (!available)
        {
            error = "no usable audio playback device";
            return false;
        }
        settings = settingsFor(sampleRate);
        return true;
    }
};

struct Fixture
{
    StubBackend* stub;
    ToneGenerator tone;
    AudioEngine engine;
    Target target;
    PlaybackSupervisor playback;

    Fixture()
        : stub(new StubBackend()), engine(std::unique_ptr<IAudioBackend>(stub)),
          playback(engine, [this](AudioEngine::Settings& settings, std::string& error) {
              return target(settings, error);
          })
    {
        engine.setSource(&tone);
    }
};

} // namespace

MULTIROOM_TEST(supervisor_starts_playback_on_the_first_attempt)
{
    Fixture fixture;

    std::string error;
    CHECK(fixture.playback.start(ms(0), error));
    CHECK(fixture.playback.state() == PlaybackState::Playing);
    CHECK(fixture.playback.lastError().empty());
    CHECK_EQ(fixture.stub->openCalls, 1);
    CHECK_EQ(fixture.stub->startCalls, 1);

    // A healthy device is polled without being touched again.
    CHECK(fixture.playback.poll(ms(100)));
    CHECK_EQ(fixture.stub->openCalls, 1);
    CHECK_EQ(fixture.playback.recoveries(), 0u);

    fixture.playback.stop();
    CHECK(fixture.playback.state() == PlaybackState::Idle);
    CHECK_EQ(fixture.stub->closeCalls, 1);
}

MULTIROOM_TEST(supervisor_reopens_a_device_that_died_under_the_render_thread)
{
    Fixture fixture;

    std::string error;
    CHECK(fixture.playback.start(ms(0), error));

    fixture.stub->simulateDeviceLoss("the device was removed or reconfigured");

    // The first poll after the loss notices it, closes the device and waits
    // out the first backoff step rather than reopening straight away.
    CHECK(!fixture.playback.poll(ms(10)));
    CHECK(fixture.playback.state() == PlaybackState::Recovering);
    CHECK_EQ(fixture.playback.lastError(), std::string("the device was removed or reconfigured"));
    CHECK_EQ(fixture.stub->closeCalls, 1);
    CHECK_EQ(fixture.stub->openCalls, 1);

    // Nothing happens before the retry is due, and it is not a busy loop: the
    // target is not even consulted.
    const int callsBefore = fixture.target.calls;
    CHECK(!fixture.playback.poll(ms(500)));
    CHECK_EQ(fixture.target.calls, callsBefore);
    CHECK(fixture.playback.retryInMs(ms(500)) > 0u);

    CHECK(fixture.playback.poll(ms(1010)));
    CHECK(fixture.playback.state() == PlaybackState::Playing);
    CHECK_EQ(fixture.stub->openCalls, 2);
    CHECK_EQ(fixture.playback.recoveries(), 1u);
    CHECK_EQ(fixture.playback.failedAttempts(), 0u);
    CHECK(fixture.playback.lastError().empty());
    CHECK_EQ(fixture.playback.retryInMs(ms(1010)), 0u);
}

MULTIROOM_TEST(supervisor_backs_off_while_the_device_stays_away)
{
    Fixture fixture;
    fixture.target.available = false;

    std::string error;
    // A device that is not there is not a hard error: the first attempt fails
    // and the supervisor goes on trying.
    CHECK(!fixture.playback.start(ms(0), error));
    CHECK(fixture.playback.state() == PlaybackState::Recovering);
    CHECK_EQ(error, std::string("no usable audio playback device"));
    CHECK_EQ(fixture.stub->openCalls, 0);

    // 1, 2 and 5 seconds: the same ladder the server connection climbs. Four
    // attempts inside the first eight seconds, not four hundred.
    const uint64_t dueAt[] = {ms(1000), ms(3000), ms(8000)};
    for (size_t i = 0; i < 3; ++i)
    {
        CHECK(!fixture.playback.poll(dueAt[i] - ms(1)));
        CHECK(!fixture.playback.poll(dueAt[i]));
    }
    CHECK_EQ(fixture.playback.failedAttempts(), 4u);
    CHECK_EQ(fixture.target.calls, 4);

    // Plugged back in: the next due attempt succeeds and counts as a recovery.
    fixture.target.available = true;
    CHECK(fixture.playback.poll(ms(18000)));
    CHECK_EQ(fixture.playback.recoveries(), 1u);
}

MULTIROOM_TEST(supervisor_rescans_once_the_known_device_keeps_refusing)
{
    Fixture fixture;
    fixture.target.available = false;

    int rescans = 0;
    fixture.playback.setRescan([&rescans](std::string&) {
        ++rescans;
        return true;
    });

    std::string error;
    CHECK(!fixture.playback.start(ms(0), error));

    // The device we already know is worth three attempts on its own -- a
    // replugged endpoint keeps its id -- before the cache itself is suspected.
    CHECK(!fixture.playback.poll(ms(1000)));
    CHECK_EQ(rescans, 0);
    CHECK(!fixture.playback.poll(ms(3000)));
    CHECK_EQ(rescans, 0);
    CHECK(!fixture.playback.poll(ms(8000)));
    CHECK_EQ(rescans, 1);

    // And not on every attempt from then on: a full scan opens every device in
    // turn and must not become the steady state.
    CHECK(!fixture.playback.poll(ms(18000)));
    CHECK_EQ(rescans, 1);
    CHECK(!fixture.playback.poll(ms(48000)));
    CHECK_EQ(rescans, 1);
    CHECK(!fixture.playback.poll(ms(78000)));
    CHECK_EQ(rescans, 2);
}

MULTIROOM_TEST(supervisor_recovers_onto_the_device_the_rescan_selected)
{
    Fixture fixture;
    fixture.target.available = false;

    // What a rescan does in main: it re-selects, and the target provider then
    // names something different from what failed.
    fixture.playback.setRescan([&fixture](std::string&) {
        fixture.target.available = true;
        fixture.target.sampleRate = 44100;
        return true;
    });

    std::string error;
    CHECK(!fixture.playback.start(ms(0), error));
    CHECK(!fixture.playback.poll(ms(1000)));
    CHECK(!fixture.playback.poll(ms(3000)));
    CHECK(fixture.playback.poll(ms(8000)));

    CHECK(fixture.playback.state() == PlaybackState::Playing);
    CHECK_EQ(fixture.stub->lastConfig.format.sampleRate, 44100u);
}

MULTIROOM_TEST(supervisor_closes_a_device_that_opened_but_would_not_start)
{
    Fixture fixture;
    fixture.stub->failStart = true;

    std::string error;
    CHECK(!fixture.playback.start(ms(0), error));
    CHECK_EQ(error, std::string("stub refuses to start"));
    // Opened, so it must be closed again: a half-open device would be held
    // against the next attempt and against every other application.
    CHECK_EQ(fixture.stub->openCalls, 1);
    CHECK_EQ(fixture.stub->closeCalls, 1);
    CHECK(fixture.playback.state() == PlaybackState::Recovering);
}

MULTIROOM_TEST(supervisor_hooks_bracket_every_open_attempt)
{
    Fixture fixture;

    std::vector<std::string> events;
    PlaybackSupervisor::Hooks hooks;
    hooks.onOpened = [&events](const AudioFormat& format) {
        // The format the driver granted, which is what the producer has to be
        // sized for -- not what was asked for.
        events.push_back("opened:" + std::to_string(format.sampleRate));
    };
    hooks.onClosed = [&events] { events.push_back("closed"); };
    fixture.playback.setHooks(hooks);

    fixture.stub->grantedSampleRate = 44100;

    std::string error;
    CHECK(fixture.playback.start(ms(0), error));
    fixture.stub->simulateDeviceLoss("device lost");
    CHECK(!fixture.playback.poll(ms(10)));
    CHECK(fixture.playback.poll(ms(1010)));
    fixture.playback.stop();

    CHECK_EQ(events.size(), static_cast<size_t>(4));
    CHECK_EQ(events[0], std::string("opened:44100"));
    CHECK_EQ(events[1], std::string("closed"));
    CHECK_EQ(events[2], std::string("opened:44100"));
    CHECK_EQ(events[3], std::string("closed"));
}

MULTIROOM_TEST(supervisor_does_nothing_while_idle)
{
    Fixture fixture;

    CHECK(!fixture.playback.poll(ms(0)));
    CHECK(!fixture.playback.poll(ms(60000)));
    CHECK_EQ(fixture.target.calls, 0);
    CHECK_EQ(fixture.stub->openCalls, 0);

    // stop() before start() must not reach the backend either.
    fixture.playback.stop();
    CHECK_EQ(fixture.stub->closeCalls, 0);
    CHECK(fixture.playback.state() == PlaybackState::Idle);
}

MULTIROOM_TEST(published_playback_says_what_the_supervisor_is_doing)
{
    // Both playback paths publish through this -- the test tone and the
    // server's stream -- because the status block they fill is the same one,
    // and `playbackState: "idle"` while a room is playing the server's stream
    // is what a second copy of it produced.
    Fixture fixture;
    ClientRuntime runtime;

    std::string error;
    CHECK(fixture.playback.start(ms(0), error));
    fixture.stub->underruns = 3;
    publishPlayback(runtime, fixture.playback, fixture.engine, ms(0));
    CHECK(runtime.playbackState == PlaybackState::Playing);
    CHECK_EQ(runtime.underruns, static_cast<uint64_t>(3));
    CHECK_EQ(runtime.bufferFrames, fixture.engine.bufferFrames());
    CHECK(runtime.playbackError.empty());

    fixture.stub->simulateDeviceLoss("device lost");
    CHECK(!fixture.playback.poll(ms(10)));
    publishPlayback(runtime, fixture.playback, fixture.engine, ms(10));

    CHECK(runtime.playbackState == PlaybackState::Recovering);
    CHECK_EQ(runtime.playbackError, std::string("device lost"));
    CHECK_EQ(runtime.playbackAttempts, static_cast<uint32_t>(1));
    CHECK(runtime.playbackRetryMs != 0);
    // No period and no clock while nothing is open, but the underrun count
    // stays at what the dead stream reached: zeroing it would claim a clean
    // run that did not happen.
    CHECK_EQ(runtime.bufferFrames, static_cast<uint32_t>(0));
    CHECK_EQ(runtime.underruns, static_cast<uint64_t>(3));
    CHECK(runtime.lastClock.quality == AudioClockQuality::Unavailable);

    CHECK(fixture.playback.poll(ms(1010)));
    publishPlayback(runtime, fixture.playback, fixture.engine, ms(1010));
    CHECK(runtime.playbackState == PlaybackState::Playing);
    CHECK_EQ(runtime.playbackRecoveries, static_cast<uint32_t>(1));
    CHECK(runtime.playbackError.empty());
    CHECK_EQ(runtime.playbackRetryMs, static_cast<uint32_t>(0));
}
