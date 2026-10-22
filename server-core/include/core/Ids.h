#pragma once

// The three identities this server keys everything on.
//
// Zone and client ids are both non-empty strings chosen by the high-end server
// above this one, and they travel through the same call chains -- which is
// exactly how a client id ends up in a zone lookup. The tag parameter costs
// nothing at runtime and turns that into a compile error.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace multiroom {

template <typename Tag>
class StringId
{
public:
    StringId() = default;
    explicit StringId(std::string value) : value_(std::move(value)) {}

    const std::string& str() const noexcept { return value_; }
    bool empty() const noexcept { return value_.empty(); }

    friend bool operator==(const StringId&, const StringId&) = default;
    friend auto operator<=>(const StringId&, const StringId&) = default;

private:
    std::string value_;
};

struct ZoneTag
{
};
struct ClientTag
{
};

// A zone is one media timeline and the rooms attached to it.
using ZoneId = StringId<ZoneTag>;

// A client is one room: one connection, one output stream, one drift model.
// It is the identity the client sends in HELLO and must stay stable across
// reconnects, because this server keys that room's whole state on it.
using ClientId = StringId<ClientTag>;

// A session id is 128 bits and a string at the same time: SESSION_CONFIG
// carries the hex form (MATP section 13) and every audio packet header carries
// the same bits as two big-endian uint64s (section 14). One type holding the
// bits, with the string derived from them, is what keeps the two from ever
// disagreeing -- the client compares the header's id for *change*, so a
// mismatch between the two forms would read as a session that restarted.
//
// randomSessionId() below is where the bits come from, added by phase 8, which
// is the phase that opens sessions.
class SessionId
{
public:
    SessionId() = default;
    SessionId(uint64_t high, uint64_t low) noexcept : high_(high), low_(low) {}

    uint64_t high() const noexcept { return high_; }
    uint64_t low() const noexcept { return low_; }

    // A default-constructed id is all zero and means "no session". It is not a
    // value this server ever generates.
    bool valid() const noexcept { return high_ != 0 || low_ != 0; }

    // 32 lowercase hex digits, high half first.
    std::string toHex() const;
    static std::optional<SessionId> fromHex(std::string_view text);

    friend bool operator==(const SessionId&, const SessionId&) = default;
    friend auto operator<=>(const SessionId&, const SessionId&) = default;

private:
    uint64_t high_ = 0;
    uint64_t low_ = 0;
};

// 128 fresh random bits, and never a default-constructed id.
//
// Section 13's only requirement is that the identifier be random, and what it
// is for is keeping stale packets from an old playback session out of the
// current one -- so what matters is that two sessions on one server never
// collide and that a client cannot predict the next one from the last. A
// cryptographic generator is not called for and a counter is not enough; this
// seeds a 64-bit Mersenne twister from std::random_device once per process and
// draws two words from it.
[[nodiscard]] SessionId randomSessionId();

} // namespace multiroom

// Ids are map keys in every registry from phase 8 onwards.
template <typename Tag>
struct std::hash<multiroom::StringId<Tag>>
{
    size_t operator()(const multiroom::StringId<Tag>& id) const noexcept
    {
        return std::hash<std::string>{}(id.str());
    }
};

template <>
struct std::hash<multiroom::SessionId>
{
    size_t operator()(const multiroom::SessionId& id) const noexcept
    {
        const size_t high = std::hash<uint64_t>{}(id.high());
        const size_t low = std::hash<uint64_t>{}(id.low());
        return high ^ (low + 0x9e3779b97f4a7c15ULL + (high << 6) + (high >> 2));
    }
};
