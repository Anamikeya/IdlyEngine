#include "engine/net/connection_manager.h"
#include "engine/net/loopback_transport.h"
#include "engine/test/test.h"

#include <chrono>
#include <optional>

using namespace engine::net;
using namespace std::chrono_literals;

namespace {

constexpr MessageType chat = first_user_message_type;
constexpr std::uint16_t port = 7;

ConnectionManagerConfig server_config()
{
	ConnectionManagerConfig c;
	c.role = Role::Server;
	return c;
}

ConnectionManagerConfig client_config()
{
	ConnectionManagerConfig c;
	c.role = Role::Client;
	return c;
}

// A server, N clients and a deterministic host clock.
struct Session {
	LoopbackNetwork net;
	LoopbackTransport server_transport{net};
	ConnectionManager server;
	std::vector<std::unique_ptr<LoopbackTransport>> client_transports;
	std::vector<std::unique_ptr<ConnectionManager>> clients;
	std::chrono::nanoseconds now{0};

	explicit Session(ConnectionManagerConfig server_cfg = server_config()) : server(server_transport, server_cfg)
	{
		ENGINE_REQUIRE(server.listen(port) == NetError::None);
	}

	ConnectionManager& add_client(ConnectionManagerConfig cfg = client_config())
	{
		client_transports.push_back(std::make_unique<LoopbackTransport>(net));
		clients.push_back(std::make_unique<ConnectionManager>(*client_transports.back(), cfg));
		ENGINE_REQUIRE(clients.back()->connect("loopback:7").ok());
		return *clients.back();
	}

	void pump(int rounds = 4, std::chrono::nanoseconds step = 16ms)
	{
		for (int i = 0; i < rounds; ++i) {
			now += step;
			server.poll(now);
			for (auto& c : clients) {
				c->poll(now);
			}
		}
	}

	ConnectionId client_connection(std::size_t i) const { return clients.at(i)->peers().at(0).connection; }
};

std::vector<std::byte> text(std::string_view s)
{
	ByteWriter w;
	w.write_string(s);
	return std::move(w).take();
}

} // namespace

ENGINE_TEST(handshake_makes_both_sides_ready)
{
	Session session;
	std::vector<ConnectionId> server_ready;
	std::vector<ConnectionId> client_ready;
	session.server.on_peer_ready([&](ConnectionId c) { server_ready.push_back(c); });
	ConnectionManager& client = session.add_client();
	client.on_peer_ready([&](ConnectionId c) { client_ready.push_back(c); });

	session.pump();
	ENGINE_CHECK_EQ(server_ready.size(), 1u);
	ENGINE_CHECK_EQ(client_ready.size(), 1u);
	ENGINE_CHECK_EQ(session.server.ready_peer_count(), 1u);
	ENGINE_CHECK_EQ(client.ready_peer_count(), 1u);
	ENGINE_CHECK(session.server.peer(server_ready.at(0))->state == PeerState::Ready);
	ENGINE_CHECK_EQ(session.server.diagnostics().connections_accepted, 1u);
}

ENGINE_TEST(user_messages_dispatch_to_handlers)
{
	Session session;
	std::vector<std::string> received;
	ENGINE_REQUIRE(session.server.register_handler(chat, [&](ConnectionId, ByteReader& r) {
		received.push_back(r.read_string(256));
	}) == NetError::None);
	ConnectionManager& client = session.add_client();
	session.pump();

	ENGINE_CHECK(client.send(session.client_connection(0), chat, text("hello"), Delivery::Reliable) == NetError::None);
	ENGINE_CHECK(client.send(session.client_connection(0), chat, text("world"), Delivery::Unreliable) == NetError::None);
	session.pump(1);
	ENGINE_REQUIRE(received.size() == 2);
	ENGINE_CHECK_EQ(received[0], std::string("hello"));
	ENGINE_CHECK_EQ(received[1], std::string("world"));
	ENGINE_CHECK_EQ(session.server.diagnostics().protocol_violations, 0u);
}

ENGINE_TEST(broadcast_reaches_all_ready_clients)
{
	Session session;
	int deliveries = 0;
	session.add_client();
	session.add_client();
	session.add_client();
	for (auto& c : session.clients) {
		c->register_handler(chat, [&](ConnectionId, ByteReader& r) {
			r.read_string(64);
			++deliveries;
		});
	}
	session.pump();
	ENGINE_CHECK(session.server.broadcast(chat, text("tick"), Delivery::Reliable) == NetError::None);
	session.pump(1);
	ENGINE_CHECK_EQ(deliveries, 3);
}

ENGINE_TEST(send_before_ready_is_rejected)
{
	Session session;
	ConnectionManager& client = session.add_client();
	const ConnectionId c = client.peers().at(0).connection;
	ENGINE_CHECK(client.send(c, chat, text("early"), Delivery::Reliable) == NetError::InvalidState);
	ENGINE_CHECK(client.send(c, message_type_hello, {}, Delivery::Reliable) == NetError::InvalidArgument);
}

ENGINE_TEST(handler_registration_rules)
{
	Session session;
	auto noop = [](ConnectionId, ByteReader&) {};
	ENGINE_CHECK(session.server.register_handler(message_type_hello, noop) == NetError::InvalidArgument);
	ENGINE_CHECK(session.server.register_handler(chat, nullptr) == NetError::InvalidArgument);
	ENGINE_CHECK(session.server.register_handler(chat, noop) == NetError::None);
	ENGINE_CHECK(session.server.register_handler(chat, noop) == NetError::InvalidState);
}

ENGINE_TEST(role_restrictions)
{
	LoopbackNetwork net;
	LoopbackTransport t(net);
	ConnectionManager server(t, server_config());
	ConnectionManager client(t, client_config());
	ENGINE_CHECK(server.connect("loopback:1").error() == NetError::InvalidState);
	ENGINE_CHECK(client.listen(1) == NetError::InvalidState);
}

ENGINE_TEST(protocol_version_mismatch_disconnects)
{
	Session session;
	ConnectionManagerConfig old_client = client_config();
	old_client.protocol_version = 99;
	ConnectionManager& client = session.add_client(old_client);
	std::optional<DisconnectInfo> client_saw;
	std::optional<DisconnectInfo> server_saw;
	client.on_peer_disconnected([&](ConnectionId, const DisconnectInfo& info) { client_saw = info; });
	session.server.on_peer_disconnected([&](ConnectionId, const DisconnectInfo& info) { server_saw = info; });
	session.pump();

	ENGINE_REQUIRE(client_saw.has_value());
	ENGINE_CHECK(client_saw->reason == CloseReason::ProtocolMismatch);
	ENGINE_CHECK(!client_saw->local);
	ENGINE_REQUIRE(server_saw.has_value());
	ENGINE_CHECK(server_saw->local);
	ENGINE_CHECK_EQ(session.server.peers().size(), 0u);
}

ENGINE_TEST(server_full_rejects_extra_clients)
{
	ConnectionManagerConfig cfg = server_config();
	cfg.max_peers = 2;
	Session session(cfg);
	session.add_client();
	session.add_client();
	ConnectionManager& third = session.add_client();
	std::optional<CloseReason> reason;
	third.on_peer_disconnected([&](ConnectionId, const DisconnectInfo& info) { reason = info.reason; });
	session.pump();

	ENGINE_CHECK_EQ(session.server.ready_peer_count(), 2u);
	ENGINE_CHECK(reason == CloseReason::ServerFull);
	ENGINE_CHECK_EQ(session.server.diagnostics().connections_rejected, 1u);
}

ENGINE_TEST(unknown_message_type_is_violation)
{
	Session session;
	ConnectionManager& client = session.add_client();
	client.register_handler(chat + 1, [](ConnectionId, ByteReader&) {});
	session.pump();
	// The server has no handler for chat.
	client.send(session.client_connection(0), chat, text("?"), Delivery::Reliable);
	session.pump(1);
	ENGINE_CHECK_EQ(session.server.diagnostics().protocol_violations, 1u);
	ENGINE_CHECK_EQ(session.server.peers().size(), 0u);
	session.pump(1);
	ENGINE_CHECK_EQ(client.peers().size(), 0u);
}

ENGINE_TEST(malformed_payload_is_violation)
{
	Session session;
	session.server.register_handler(chat, [](ConnectionId, ByteReader& r) { r.read_u8(); }); // leaves bytes unread
	ConnectionManager& client = session.add_client();
	session.pump();
	client.send(session.client_connection(0), chat, text("too long"), Delivery::Reliable);
	session.pump(1);
	ENGINE_CHECK_EQ(session.server.diagnostics().protocol_violations, 1u);
	ENGINE_CHECK_EQ(session.server.peers().size(), 0u);
}

ENGINE_TEST(raw_message_before_handshake_is_violation)
{
	Session session;
	LoopbackTransport rogue(session.net);
	const ConnectionId c = rogue.connect("loopback:7").value();
	std::vector<TransportEvent> events;
	std::vector<ReceivedMessage> messages;
	session.pump(1); // server accepts
	rogue.poll(events, messages);

	ByteWriter w;
	w.write_u16(chat);
	w.write_string("skip handshake");
	rogue.send(c, w.bytes(), Delivery::Reliable);
	session.pump(1);
	ENGINE_CHECK_EQ(session.server.diagnostics().protocol_violations, 1u);
	ENGINE_CHECK_EQ(session.server.peers().size(), 0u);
}

ENGINE_TEST(oversized_and_runt_messages_are_violations)
{
	ConnectionManagerConfig cfg = server_config();
	cfg.max_message_bytes = 32;
	Session session(cfg);
	LoopbackTransport rogue(session.net);
	const ConnectionId c = rogue.connect("loopback:7").value();
	session.pump(1);
	const std::byte one{1};
	rogue.send(c, std::span(&one, 1), Delivery::Reliable); // shorter than the type header
	session.pump(1);
	ENGINE_CHECK_EQ(session.server.diagnostics().protocol_violations, 1u);

	LoopbackTransport rogue2(session.net);
	const ConnectionId c2 = rogue2.connect("loopback:7").value();
	session.pump(1);
	rogue2.send(c2, std::vector<std::byte>(33), Delivery::Reliable);
	session.pump(1);
	ENGINE_CHECK_EQ(session.server.diagnostics().protocol_violations, 2u);
}

ENGINE_TEST(message_flood_is_violation)
{
	ConnectionManagerConfig cfg = server_config();
	cfg.max_messages_per_peer_per_poll = 10;
	Session session(cfg);
	int handled = 0;
	session.server.register_handler(chat, [&](ConnectionId, ByteReader& r) {
		r.read_string(64);
		++handled;
	});
	ConnectionManager& client = session.add_client();
	session.pump();
	for (int i = 0; i < 11; ++i) {
		client.send(session.client_connection(0), chat, text("spam"), Delivery::Reliable);
	}
	session.pump(1);
	ENGINE_CHECK_EQ(handled, 10);
	ENGINE_CHECK_EQ(session.server.diagnostics().protocol_violations, 1u);
	ENGINE_CHECK_EQ(session.server.peers().size(), 0u);
}

ENGINE_TEST(handshake_timeout_closes_silent_peer)
{
	ConnectionManagerConfig cfg = server_config();
	cfg.handshake_timeout = 100ms;
	Session session(cfg);
	LoopbackTransport silent(session.net);
	silent.connect("loopback:7");
	session.pump(1, 10ms);
	ENGINE_CHECK_EQ(session.server.peers().size(), 1u);
	session.pump(1, 50ms);
	ENGINE_CHECK_EQ(session.server.peers().size(), 1u);
	session.pump(1, 60ms); // 110 ms after first seen
	ENGINE_CHECK_EQ(session.server.peers().size(), 0u);
	ENGINE_CHECK_EQ(session.server.diagnostics().handshake_timeouts, 1u);
}

ENGINE_TEST(handler_may_disconnect_sender)
{
	Session session;
	session.server.register_handler(chat, [&](ConnectionId from, ByteReader& r) {
		r.read_string(64);
		session.server.disconnect(from, CloseReason::Kicked, "kicked by handler");
	});
	ConnectionManager& client = session.add_client();
	std::optional<CloseReason> reason;
	client.on_peer_disconnected([&](ConnectionId, const DisconnectInfo& info) { reason = info.reason; });
	session.pump();
	client.send(session.client_connection(0), chat, text("hi"), Delivery::Reliable);
	client.send(session.client_connection(0), chat, text("again"), Delivery::Reliable);
	session.pump(2);
	ENGINE_CHECK(reason == CloseReason::Kicked);
	ENGINE_CHECK_EQ(session.server.diagnostics().protocol_violations, 0u);
}

ENGINE_TEST(client_learns_unreachable_server)
{
	LoopbackNetwork net;
	LoopbackTransport t(net);
	ConnectionManager client(t, client_config());
	std::optional<DisconnectInfo> info;
	client.on_peer_disconnected([&](ConnectionId, const DisconnectInfo& i) { info = i; });
	ENGINE_REQUIRE(client.connect("loopback:3").ok());
	client.poll(0ns);
	ENGINE_REQUIRE(info.has_value());
	ENGINE_CHECK(info->reason == CloseReason::Transport);
	ENGINE_CHECK_EQ(client.peers().size(), 0u);
}

ENGINE_TEST(diagnostics_count_traffic)
{
	Session session;
	session.server.register_handler(chat, [](ConnectionId, ByteReader& r) { r.read_string(64); });
	ConnectionManager& client = session.add_client();
	session.pump();
	const NetworkDiagnostics before = client.diagnostics();
	const auto payload = text("abc"); // 1 length byte + 3
	client.send(session.client_connection(0), chat, payload, Delivery::Reliable);
	session.pump(1);
	ENGINE_CHECK_EQ(client.diagnostics().messages_sent, before.messages_sent + 1);
	ENGINE_CHECK_EQ(client.diagnostics().bytes_sent, before.bytes_sent + 2 + payload.size());
	ENGINE_CHECK(client.transport_stats(session.client_connection(0)).ok());
	ENGINE_CHECK(client.transport_stats(ConnectionId{12345}).error() == NetError::InvalidConnection);
}

ENGINE_TEST(shutdown_notifies_and_is_idempotent)
{
	Session session;
	session.add_client();
	session.add_client();
	session.pump();
	int local_disconnects = 0;
	session.server.on_peer_disconnected([&](ConnectionId, const DisconnectInfo& info) {
		local_disconnects += info.local && info.reason == CloseReason::Shutdown ? 1 : 0;
	});
	session.server.shutdown();
	session.server.shutdown();
	ENGINE_CHECK_EQ(local_disconnects, 2);
	session.pump(1);
	for (auto& c : session.clients) {
		ENGINE_CHECK_EQ(c->peers().size(), 0u);
	}
}
