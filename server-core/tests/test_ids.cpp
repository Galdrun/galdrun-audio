#include "TestSupport.h"

#include "core/Ids.h"

#include <type_traits>
#include <unordered_map>

using namespace multiroom;

MULTIROOM_TEST(ids_of_different_kinds_are_different_types)
{
    const ZoneId zone("livingroom");
    const ClientId client("livingroom");

    CHECK_EQ(zone.str(), client.str());

    // Same spelling, different type. This is the whole point: a client id
    // cannot be handed to a zone lookup by accident.
    static_assert(!std::is_same_v<ZoneId, ClientId>);
    static_assert(!std::is_convertible_v<ZoneId, ClientId>);
    static_assert(!std::is_convertible_v<std::string, ZoneId>);
}

MULTIROOM_TEST(ids_compare_and_hash_by_their_text)
{
    CHECK(ZoneId("kitchen") == ZoneId("kitchen"));
    CHECK(ZoneId("kitchen") != ZoneId("office"));
    CHECK(ZoneId("kitchen") < ZoneId("office"));

    CHECK(ZoneId().empty());
    CHECK(!ZoneId("kitchen").empty());

    std::unordered_map<ClientId, int> rooms;
    rooms[ClientId("livingroom")] = 1;
    rooms[ClientId("kitchen")] = 2;
    rooms[ClientId("livingroom")] = 3;

    CHECK_EQ(rooms.size(), size_t{2});
    CHECK_EQ(rooms[ClientId("livingroom")], 3);
}

MULTIROOM_TEST(session_id_hex_form_and_bits_agree)
{
    // SESSION_CONFIG carries the string, the packet header carries the bits.
    // One type holding both is what stops them disagreeing.
    const SessionId id(0x0123456789abcdefULL, 0xfedcba9876543210ULL);

    CHECK_EQ(id.toHex(), std::string("0123456789abcdeffedcba9876543210"));
    CHECK_EQ(id.high(), 0x0123456789abcdefULL);
    CHECK_EQ(id.low(), 0xfedcba9876543210ULL);

    const auto parsed = SessionId::fromHex(id.toHex());
    CHECK(parsed.has_value());
    CHECK(*parsed == id);

    // Leading zeros survive, because the string is always 32 digits.
    const SessionId small(0, 1);
    CHECK_EQ(small.toHex(), std::string("00000000000000000000000000000001"));
    CHECK(SessionId::fromHex(small.toHex()) == small);
}

MULTIROOM_TEST(session_id_rejects_anything_that_is_not_32_hex_digits)
{
    CHECK(!SessionId::fromHex("").has_value());
    CHECK(!SessionId::fromHex("0123456789abcdef").has_value());               // 16
    CHECK(!SessionId::fromHex("0123456789abcdeffedcba98765432100").has_value()); // 33
    CHECK(!SessionId::fromHex("0123456789abcdeffedcba987654321g").has_value()); // not hex
    CHECK(!SessionId::fromHex("0123456789abcdef fedcba98765432").has_value());

    // Upper case is accepted on the way in and normalized on the way out: the
    // client compares the header's bits for change, not the string.
    const auto upper = SessionId::fromHex("0123456789ABCDEFFEDCBA9876543210");
    CHECK(upper.has_value());
    CHECK_EQ(upper->toHex(), std::string("0123456789abcdeffedcba9876543210"));
}

MULTIROOM_TEST(session_id_default_is_no_session)
{
    const SessionId none;
    CHECK(!none.valid());
    CHECK_EQ(none.toHex(), std::string("00000000000000000000000000000000"));

    CHECK(SessionId(1, 0).valid());
    CHECK(SessionId(0, 1).valid());
    CHECK(none != SessionId(0, 1));

    std::unordered_map<SessionId, int> sessions;
    sessions[SessionId(1, 2)] = 1;
    sessions[SessionId(2, 1)] = 2;
    CHECK_EQ(sessions.size(), size_t{2});
}
