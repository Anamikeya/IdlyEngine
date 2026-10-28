#include "engine/online/null_online_service.h"
#include "engine/test/test.h"

using namespace engine::online;

ENGINE_TEST(lifecycle_and_idempotent_shutdown)
{
	NullOnlineService service;
	ENGINE_CHECK(!service.initialized());
	ENGINE_CHECK(service.local_user().error() == OnlineError::NotInitialized);
	ENGINE_CHECK(service.initialize() == OnlineError::None);
	ENGINE_CHECK(service.initialize() == OnlineError::AlreadyInitialized);
	ENGINE_CHECK(service.initialized());
	service.shutdown();
	service.shutdown();
	ENGINE_CHECK(!service.initialized());
	ENGINE_CHECK(service.initialize() == OnlineError::None); // re-initializable
}

ENGINE_TEST(local_user_identity)
{
	NullOnlineService service("player-7", "Seven");
	service.initialize();
	const LocalUser user = service.local_user().value();
	ENGINE_CHECK(user.id.provider == Provider::Null);
	ENGINE_CHECK_EQ(user.id.id, std::string("player-7"));
	ENGINE_CHECK_EQ(user.display_name, std::string("Seven"));
}

ENGINE_TEST(invalid_user_id_fails_initialization)
{
	NullOnlineService empty("");
	ENGINE_CHECK(empty.initialize() == OnlineError::InitializationFailed);
	NullOnlineService piped("a|b");
	ENGINE_CHECK(piped.initialize() == OnlineError::InitializationFailed);
}

ENGINE_TEST(auth_ticket_roundtrip_between_client_and_server)
{
	NullOnlineService client("alice");
	NullOnlineService server("dedicated-server");
	client.initialize();
	server.initialize();

	const AuthTicket ticket = client.create_auth_ticket("server-1").value();
	ENGINE_CHECK(ticket.data.size() <= AuthTicket::max_size);
	const auto verified = server.verify_auth_ticket(ticket, "server-1");
	ENGINE_REQUIRE(verified.ok());
	ENGINE_CHECK(verified.value() == (PlatformUserId{Provider::Null, "alice"}));
}

ENGINE_TEST(auth_ticket_rejections)
{
	NullOnlineService client("alice");
	NullOnlineService server("srv");
	client.initialize();
	server.initialize();
	AuthTicket ticket = client.create_auth_ticket("server-1").value();

	ENGINE_CHECK(server.verify_auth_ticket(ticket, "server-2").error() == OnlineError::InvalidTicket);

	AuthTicket wrong_provider = ticket;
	wrong_provider.provider = Provider::Steam;
	ENGINE_CHECK(server.verify_auth_ticket(wrong_provider, "server-1").error() == OnlineError::InvalidTicket);

	AuthTicket garbage;
	garbage.data = {std::byte{0xff}, std::byte{0x00}};
	ENGINE_CHECK(server.verify_auth_ticket(garbage, "server-1").error() == OnlineError::InvalidTicket);

	AuthTicket oversized = ticket;
	oversized.data.resize(AuthTicket::max_size + 1);
	ENGINE_CHECK(server.verify_auth_ticket(oversized, "server-1").error() == OnlineError::InvalidTicket);

	ENGINE_CHECK(client.create_auth_ticket("").error() == OnlineError::InvalidArgument);
	NullOnlineService offline;
	ENGINE_CHECK(offline.verify_auth_ticket(ticket, "server-1").error() == OnlineError::NotInitialized);
}

ENGINE_TEST(achievements)
{
	NullOnlineService service;
	ENGINE_CHECK(service.unlock_achievement("first_blood") == OnlineError::NotInitialized);
	service.initialize();
	ENGINE_CHECK(!service.is_achievement_unlocked("first_blood").value());
	ENGINE_CHECK(service.unlock_achievement("first_blood") == OnlineError::None);
	ENGINE_CHECK(service.unlock_achievement("first_blood") == OnlineError::None); // idempotent
	ENGINE_CHECK(service.is_achievement_unlocked("first_blood").value());
	ENGINE_CHECK(service.unlock_achievement("") == OnlineError::InvalidArgument);
}

ENGINE_TEST(provider_names)
{
	ENGINE_CHECK_EQ(std::string(to_string(Provider::Steam)), std::string("steam"));
	ENGINE_CHECK_EQ(std::string(to_string(Provider::EpicOnlineServices)), std::string("epic-online-services"));
	ENGINE_CHECK_EQ(std::string(to_string(Provider::GooglePlayGames)), std::string("google-play-games"));
}
