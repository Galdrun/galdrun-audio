#include "core/Ids.h"

#include <mutex>
#include <random>

namespace multiroom {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

void appendHex64(std::string& out, uint64_t value)
{
    for (int shift = 60; shift >= 0; shift -= 4)
        out.push_back(kHexDigits[(value >> shift) & 0xF]);
}

bool hexValue(char c, uint64_t& out)
{
    if (c >= '0' && c <= '9')
        out = static_cast<uint64_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
        out = static_cast<uint64_t>(c - 'a') + 10;
    else if (c >= 'A' && c <= 'F')
        out = static_cast<uint64_t>(c - 'A') + 10;
    else
        return false;
    return true;
}

} // namespace

std::string SessionId::toHex() const
{
    std::string text;
    text.reserve(32);
    appendHex64(text, high_);
    appendHex64(text, low_);
    return text;
}

std::optional<SessionId> SessionId::fromHex(std::string_view text)
{
    // Exactly 32 digits. A shorter form would be ambiguous about which half it
    // filled, and the wire always carries the full 128 bits.
    if (text.size() != 32)
        return std::nullopt;

    uint64_t halves[2] = {0, 0};
    for (size_t i = 0; i < text.size(); ++i)
    {
        uint64_t digit = 0;
        if (!hexValue(text[i], digit))
            return std::nullopt;
        halves[i / 16] = (halves[i / 16] << 4) | digit;
    }
    return SessionId(halves[0], halves[1]);
}

SessionId randomSessionId()
{
    // Seeded once and shared, under a mutex: sessions are opened from the
    // acceptor thread today and from a zone's thread the moment phase 10 adds
    // one, and two threads drawing from one twister would produce a sequence
    // that is neither's. The lock is taken once per connection, which is not a
    // rate worth a generator per thread.
    static std::mutex mutex;
    static std::mt19937_64 generator(std::random_device{}());

    const std::lock_guard<std::mutex> lock(mutex);
    for (;;)
    {
        const uint64_t high = generator();
        const uint64_t low = generator();
        // All-zero is the "no session" value and is never generated, so that
        // valid() means what it says. One draw in 2^128 makes this loop a
        // statement about the invariant rather than a retry that ever runs.
        if (high != 0 || low != 0)
            return SessionId(high, low);
    }
}

} // namespace multiroom
