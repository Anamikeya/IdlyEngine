#include "engine/net/loopback_transport.h"
#include "engine/test/test.h"

#include <algorithm>

using namespace engine::net;

namespace {

struct Polled {
	std::vector<TransportEvent> events;
	std::vector<ReceivedMessage> messages;
};

Polled poll(ITransport& t)
{
	Polled p;
	ENGINE_CHECK(t.poll(p.events, p.messages) == NetError::None);
	return p;
}

std::vector<std::byte> payload(std::string_view text)
{
	const auto b = std::as_bytes(std::span<const char>(text.data(), text.size()));
	return {b.begin(), b.end()};
}

// Connects client to server on port 1 and returns {client side id, server side id}.
std::pair<ConnectionId, ConnectionId> connect_pair(LoopbackTransport& server, LoopbackTransport& client)
{
	ENGINE_REQUIRE(server.listen(1) == NetError::None);
	const auto c = client.connect("loopback:1");
	ENGINE_REQUIRE(c.ok());
	const Polled incoming = poll(server);
	ENGINE_REQUIRE(incoming.events.size() == 1);
	ENGINE_REQUIRE(incoming.events[0].type == TransportEvent::Type::IncomingConnection);
	const ConnectionId s = incoming.events[0].connection;
	ENGINE_REQUIRE(server.accept(s) == NetError::None);
	poll(server); // Connected
	const Polled connected = poll(client);
	ENGINE_REQUIRE(connected.events.size() == 1);
	ENGINE_REQUIRE(connected.events[0].type == TransportEvent::Type::Connected);
	return {c.value(), s};
}

} // namespace

ENGINE_TEST(connect_accept_and_exchange)
{
	LoopbackNetwork net;
	LoopbackTransport server(net);
	LoopbackTransport client(net);
	const auto [c, s] = connect_pair(server, client);

	ENGINE_CHECK(client.send(c, payload("ping"), Delivery::Reliable) == NetError::None);
	const Polled at_server = poll(server);
	ENGINE_REQUIRE(at_server.messages.size() == 1);
	ENGINE_CHECK(at_server.messages[0].connection == s);
	ENGINE_CHECK(at_server.messages[0].data == payload("ping"));
	ENGINE_CHECK(at_server.messages[0].delivery == Delivery::Reliable);

	ENGINE_CHECK(server.send(s, payload("pong"), Delivery::Unreliable) == NetError::None);
	const Polled at_client = poll(client);
	ENGINE_REQUIRE(at_client.messages.size() == 1);
	ENGINE_CHECK(at_client.messages[0].data == payload("pong"));
}

ENGINE_TEST(reliable_messages_arrive_in_order)
{
	LoopbackNetwork net(LoopbackConfig{.unreliable_loss = 0.9});
	LoopbackTransport server(net);
	LoopbackTransport client(net);
	const auto [c, s] = connect_pair(server, client);
	for (int i = 0; i < 100; ++i) {
		const std::byte b{static_cast<unsigned char>(i)};
		client.send(c, std::span(&b, 1), Delivery::Reliable);
	}
	const Polled p = poll(server);
	ENGINE_REQUIRE(p.messages.size() == 100);
	for (int i = 0; i < 100; ++i) {
		ENGINE_CHECK(p.messages[static_cast<std::size_t>(i)].data[0] == std::byte{static_cast<unsigned char>(i)});
	}
}

ENGINE_TEST(unreliable_loss_is_seeded_and_counted)
{
	auto run = [](std::uint64_t seed) {
		LoopbackNetwork net(LoopbackConfig{.unreliable_loss = 0.5, .seed = seed});
		LoopbackTransport server(net);
		LoopbackTransport client(net);
		const auto [c, s] = connect_pair(server, client);
		const std::byte b{1};
		for (int i = 0; i < 1000; ++i) {
			client.send(c, std::span(&b, 1), Delivery::Unreliable);
		}
		const std::size_t received = poll(server).messages.size();
		ENGINE_CHECK_EQ(received + client.dropped_unreliable(), 1000u);
		return received;
	};
	const std::size_t a = run(42);
	ENGINE_CHECK_EQ(a, run(42));
	ENGINE_CHECK(a > 400 && a < 600);
}

ENGINE_TEST(connect_without_listener_fails_asynchronously)
{
	LoopbackNetwork net;
	LoopbackTransport client(net);
	const auto c = client.connect("loopback:9");
	ENGINE_REQUIRE(c.ok());
	const Polled p = poll(client);
	ENGINE_REQUIRE(p.events.size() == 1);
	ENGINE_CHECK(p.events[0].type == TransportEvent::Type::Disconnected);
	ENGINE_CHECK(p.events[0].connection == c.value());
	ENGINE_CHECK(close_reason_from_code(p.events[0].reason_code) == CloseReason::Transport);
}

ENGINE_TEST(invalid_address_and_port_reuse)
{
	LoopbackNetwork net;
	LoopbackTransport a(net);
	LoopbackTransport b(net);
	ENGINE_CHECK(a.connect("not-a-port").error() == NetError::InvalidArgument);
	ENGINE_CHECK(a.connect("loopback:70000").error() == NetError::InvalidArgument);
	ENGINE_CHECK(a.listen(5) == NetError::None);
	ENGINE_CHECK(b.listen(5) == NetError::AddressInUse);
	a.shutdown();
	ENGINE_CHECK(b.listen(5) == NetError::None); // released on shutdown
}

ENGINE_TEST(send_before_accept_is_invalid_state)
{
	LoopbackNetwork net;
	LoopbackTransport server(net);
	LoopbackTransport client(net);
	server.listen(1);
	const ConnectionId c = client.connect("1").value();
	ENGINE_CHECK(client.send(c, payload("x"), Delivery::Reliable) == NetError::InvalidState);
	ENGINE_CHECK(client.send(ConnectionId{999}, payload("x"), Delivery::Reliable) == NetError::InvalidConnection);
}

ENGINE_TEST(rejecting_incoming_notifies_client)
{
	LoopbackNetwork net;
	LoopbackTransport server(net);
	LoopbackTransport client(net);
	server.listen(1);
	const ConnectionId c = client.connect("1").value();
	const ConnectionId s = poll(server).events.at(0).connection;
	ENGINE_CHECK(server.close(s, static_cast<std::int32_t>(CloseReason::ServerFull), "full") == NetError::None);
	const Polled p = poll(client);
	ENGINE_REQUIRE(p.events.size() == 1);
	ENGINE_CHECK(p.events[0].connection == c);
	ENGINE_CHECK(close_reason_from_code(p.events[0].reason_code) == CloseReason::ServerFull);
	ENGINE_CHECK_EQ(p.events[0].debug, std::string("full"));
}

ENGINE_TEST(close_is_silent_locally_and_final)
{
	LoopbackNetwork net;
	LoopbackTransport server(net);
	LoopbackTransport client(net);
	const auto [c, s] = connect_pair(server, client);
	ENGINE_CHECK(client.close(c, static_cast<std::int32_t>(CloseReason::Normal), "bye") == NetError::None);
	ENGINE_CHECK(poll(client).events.empty());
	ENGINE_CHECK(client.send(c, payload("late"), Delivery::Reliable) == NetError::InvalidConnection);
	ENGINE_CHECK(client.close(c, 0, {}) == NetError::InvalidConnection);
	const Polled p = poll(server);
	ENGINE_REQUIRE(p.events.size() == 1);
	ENGINE_CHECK(p.events[0].type == TransportEvent::Type::Disconnected);
	ENGINE_CHECK(server.stats(s).error() == NetError::InvalidConnection);
}

ENGINE_TEST(messages_sent_before_close_are_still_delivered)
{
	LoopbackNetwork net;
	LoopbackTransport server(net);
	LoopbackTransport client(net);
	const auto [c, s] = connect_pair(server, client);
	client.send(c, payload("last words"), Delivery::Reliable);
	client.close(c, static_cast<std::int32_t>(CloseReason::Normal), {});
	const Polled p = poll(server);
	ENGINE_CHECK_EQ(p.messages.size(), 1u);
	ENGINE_CHECK_EQ(p.events.size(), 1u);
}

ENGINE_TEST(destroying_endpoint_disconnects_peers)
{
	LoopbackNetwork net;
	LoopbackTransport server(net);
	{
		LoopbackTransport client(net);
		connect_pair(server, client);
	}
	const Polled p = poll(server);
	ENGINE_REQUIRE(p.events.size() == 1);
	ENGINE_CHECK(close_reason_from_code(p.events[0].reason_code) == CloseReason::Shutdown);
}

ENGINE_TEST(oversized_message_rejected)
{
	LoopbackNetwork net(LoopbackConfig{.max_message_size = 8});
	LoopbackTransport server(net);
	LoopbackTransport client(net);
	const auto [c, s] = connect_pair(server, client);
	ENGINE_CHECK_EQ(client.max_message_size(), 8u);
	ENGINE_CHECK(client.send(c, std::vector<std::byte>(9), Delivery::Reliable) == NetError::MessageTooLarge);
	ENGINE_CHECK(client.send(c, std::vector<std::byte>(8), Delivery::Reliable) == NetError::None);
}
