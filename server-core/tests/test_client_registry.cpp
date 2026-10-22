// Which rooms this server has a control connection to, keyed on the client id
// of MATP section 11.

#include "ManualClock.h"
#include "TestSupport.h"

#include "client/ClientRegistry.h"

#include <memory>

using namespace multiroom;

namespace {

multiroomtest::ManualClock g_clock;

// A session that has completed HELLO under `id`, which is the only kind the
// registry accepts.
std::shared_ptr<ClientSession> identifiedSession(const std::string& id)
{
    auto session = std::make_shared<ClientSession>(randomSessionId(), g_clock);

    matp::Identity identity;
    identity.clientId = id;
    const std::string frame =
        matp::encode(matp::MessageType::Hello, matp::makeHello(identity));

    CHECK_OK(session->receive(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(frame.data()), frame.size())));
    CHECK_EQ(session->clientId().str(), id);
    return session;
}

} // namespace

MULTIROOM_TEST(registry_holds_one_session_per_client_id)
{
    ClientRegistry registry;
    CHECK(registry.empty());

    auto living = identifiedSession("livingroom");
    auto kitchen = identifiedSession("kitchen");

    auto first = registry.add(living);
    CHECK_OK(first);
    CHECK(*first == nullptr); // nothing was displaced

    CHECK_OK(registry.add(kitchen));
    CHECK_EQ(registry.size(), size_t(2));

    CHECK(registry.find(ClientId("livingroom")) == living);
    CHECK(registry.find(ClientId("kitchen")) == kitchen);
    CHECK(registry.find(ClientId("bathroom")) == nullptr);

    const std::vector<ClientId> ids = registry.ids();
    CHECK_EQ(ids.size(), size_t(2));
    CHECK_EQ(ids[0].str(), std::string("kitchen")); // sorted
    CHECK_EQ(ids[1].str(), std::string("livingroom"));
}

MULTIROOM_TEST(registry_refuses_a_session_that_has_not_been_named)
{
    // A socket that has connected and said nothing has no id to be keyed on,
    // which is why the listener holds it outside the registry until HELLO.
    ClientRegistry registry;
    auto anonymous = std::make_shared<ClientSession>(randomSessionId(), g_clock);

    CHECK_ERR(registry.add(anonymous), RegistryError::Unnamed);
    CHECK(registry.empty());

    CHECK_ERR(registry.add(nullptr), RegistryError::Unnamed);
}

MULTIROOM_TEST(registry_replaces_a_reconnecting_client_and_hands_back_the_old_session)
{
    // Section 43. The old connection is a room that is gone whether or not its
    // TCP connection has noticed; the registry hands it back rather than
    // closing it, because it owns no sockets.
    ClientRegistry registry;

    auto original = identifiedSession("livingroom");
    CHECK_OK(registry.add(original));

    auto reconnected = identifiedSession("livingroom");
    CHECK(reconnected != original);
    CHECK(reconnected->sessionId() != original->sessionId());

    auto displaced = registry.add(reconnected);
    CHECK_OK(displaced);
    CHECK(*displaced == original);

    CHECK_EQ(registry.size(), size_t(1));
    CHECK(registry.find(ClientId("livingroom")) == reconnected);
}

MULTIROOM_TEST(registry_will_not_let_a_stale_connection_remove_the_live_one)
{
    // The displaced connection tidies up after itself later. If that removal
    // were by id alone it would take the room that replaced it.
    ClientRegistry registry;

    auto original = identifiedSession("livingroom");
    CHECK_OK(registry.add(original));
    auto reconnected = identifiedSession("livingroom");
    CHECK_OK(registry.add(reconnected));

    CHECK(!registry.remove(ClientId("livingroom"), original.get()));
    CHECK_EQ(registry.size(), size_t(1));
    CHECK(registry.find(ClientId("livingroom")) == reconnected);

    CHECK(registry.remove(ClientId("livingroom"), reconnected.get()));
    CHECK(registry.empty());
}

MULTIROOM_TEST(registry_is_bounded_and_says_how_often_it_refused)
{
    // Memory is bounded, and a listener is the one place a stranger decides
    // how many rooms this process holds.
    ClientRegistry registry(2);

    CHECK_OK(registry.add(identifiedSession("one")));
    CHECK_OK(registry.add(identifiedSession("two")));
    CHECK_ERR(registry.add(identifiedSession("three")), RegistryError::Full);

    CHECK_EQ(registry.size(), size_t(2));
    CHECK_EQ(registry.refusedCount(), 1ULL);

    // A client already present is a replacement rather than an addition, so
    // the ceiling does not refuse a reconnect.
    CHECK_OK(registry.add(identifiedSession("two")));
    CHECK_EQ(registry.size(), size_t(2));
    CHECK_EQ(registry.refusedCount(), 1ULL);

    CHECK(registry.remove(ClientId("one"), nullptr));
    CHECK_OK(registry.add(identifiedSession("three")));
    CHECK_EQ(registry.size(), size_t(2));
}

MULTIROOM_TEST(session_ids_are_random_and_never_the_empty_one)
{
    // Section 13: the identifier is what keeps stale packets from an old
    // playback session out of the current one, so two sessions on one server
    // must never collide.
    const SessionId first = randomSessionId();
    const SessionId second = randomSessionId();

    CHECK(first.valid());
    CHECK(second.valid());
    CHECK(first != second);

    // The hex string and the 128 bits are two forms of one value.
    const std::optional<SessionId> roundTripped = SessionId::fromHex(first.toHex());
    CHECK(roundTripped.has_value());
    CHECK(*roundTripped == first);
    CHECK_EQ(first.toHex().size(), size_t(32));
}
