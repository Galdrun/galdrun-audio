#define RAUMFELD_LEDS_TEST
#include "../board/connector2/status-leds.cpp"
#include <cassert>
#include <cstdio>

int main()
{
    Value response;
    std::string error;
    auto parse = [&](const char* text) {
        assert(multiroom::json::parse(text, response, error));
        return statusState(response);
    };
    assert(parse("{}") == State::Error);
    assert(parse(R"({"status":{"server":{"state":"notConfigured"},"session":{"state":"idle"},"audio":{"deviceSelected":true}}})") == State::Setup);
    assert(parse(R"({"status":{"server":{"state":"connecting"},"session":{"state":"idle"},"audio":{"deviceSelected":true}}})") == State::Connecting);
    assert(parse(R"({"status":{"server":{"state":"connected"},"session":{"state":"established","formatPlayable":true},"audio":{"deviceSelected":true,"playing":false,"playbackState":"paused"}}})") == State::Ready);
    assert(parse(R"({"status":{"server":{"state":"connected"},"session":{"state":"failed"},"audio":{"deviceSelected":true}}})") == State::Error);
    assert(parse(R"({"status":{"server":{"state":"connected"},"session":{"state":"established","formatPlayable":false},"audio":{"deviceSelected":true}}})") == State::Error);
    assert(parse(R"({"status":{"server":{"state":"connected"},"session":{"state":"established"},"audio":{"deviceSelected":false}}})") == State::Error);
    assert(parse(R"({"status":{"server":{"state":"connected"},"session":{"state":"established"},"audio":{"deviceSelected":true,"playbackState":"recovering"}}})") == State::Error);
    assert(brightness(State::Boot, 0) == std::make_pair(true, false));
    assert(brightness(State::Boot, 500) == std::make_pair(false, false));
    assert(brightness(State::Connecting, 0) == std::make_pair(true, true));
    assert(brightness(State::Connecting, 500) == std::make_pair(true, false));
    assert(brightness(State::Ready, 1500) == std::make_pair(true, true));
    assert(brightness(State::Setup, 0).second);
    assert(!brightness(State::Setup, 200).second);
    assert(brightness(State::Setup, 400).second);
    assert(!brightness(State::Setup, 600).second);
    assert(brightness(State::Error, 0).second);
    assert(!brightness(State::Error, 200).second);
    assert(brightness(State::Off, 0) == std::make_pair(false, false));
    assert(brightness(State::Off, 1500) == std::make_pair(false, false));
    std::puts("PASS: ready/paused, connecting, setup, malformed/failure status and distinct LED patterns");
}
