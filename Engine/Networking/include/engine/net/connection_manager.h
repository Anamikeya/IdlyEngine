#pragma once

// Server-authoritative session layer on top of an ITransport.
//
// Wire format of every message: [u16 message type][payload]. Types below first_user_message_type
// are reserved for the session protocol. A client must complete the Hello handshake before any
// user message is accepted; the server answers with HelloAck. Malformed, oversized, unknown or
// premature messages are protocol violations and close the connection.
//
// Handlers run synchronously inside poll(), on the caller's thread. That is the marshaling point
// onto the simulation/world owner thread; nothing here starts threads.

#include "engine/net/byte_stream.h"
#include "engine/net/transport.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace engine::net {

using MessageType = std::uint16_t;

inline constexpr MessageType message_type_hello = 1;
inline constexpr MessageType message_type_hello_ack = 2;
inline constexpr MessageType first_user_message_type = 64;
// "GENG" little-endian; rejects traffic from unrelated programs before version comparison.
inline constexpr std::uint32_t protocol_magic = 0x474e4547u;

enum class Role : std::uint8_t { Server, Client };

enum class PeerState : std::uint8_t {
	// Transport connection in progress.
	Connecting,
	// Transport connected, session handshake not yet complete.
	Handshaking,
	// Handshake complete; user messages flow.
	Ready,
};

struct ConnectionManagerConfig {
	Role role = Role::Server;
	// Both sides must match exactly.
	std::uint32_t protocol_version = 1;
	// Server only. Further incoming connections are rejected with CloseReason::ServerFull.
	std::size_t max_peers = 32;
	// Upper bound on any single message including the 2-byte type header.
	std::size_t max_message_bytes = 64 * 1024;
	// Messages beyond this per peer per poll() are a violation (flood protection).
	std::size_t max_messages_per_peer_per_poll = 512;
	// Peers that are not Ready this long after connecting are closed. Zero disables the timeout.
	std::chrono::nanoseconds handshake_timeout{std::chrono::seconds(10)};
};

struct PeerInfo {
	ConnectionId connection;
	PeerState state = PeerState::Connecting;
	// Host time at which poll() first observed this peer; the handshake timeout counts from here.
	std::chrono::nanoseconds first_seen{0};
	std::uint64_t messages_received = 0;
	std::uint64_t bytes_received = 0;
	std::uint64_t messages_sent = 0;
	std::uint64_t bytes_sent = 0;
};

struct DisconnectInfo {
	CloseReason reason = CloseReason::Transport;
	std::int32_t code = 0;
	std::string debug;
	// True when this side initiated the close (violation, kick, timeout, shutdown).
	bool local = false;
};

struct NetworkDiagnostics {
	std::uint64_t messages_sent = 0;
	std::uint64_t messages_received = 0;
	std::uint64_t bytes_sent = 0;
	std::uint64_t bytes_received = 0;
	std::uint64_t connections_accepted = 0;
	std::uint64_t connections_rejected = 0;
	std::uint64_t protocol_violations = 0;
	std::uint64_t handshake_timeouts = 0;
	std::uint64_t send_failures = 0;
};

class ConnectionManager {
public:
	// The handler must consume the whole payload; leftover bytes or a failed read are a violation.
	using MessageHandler = std::function<void(ConnectionId, ByteReader&)>;
	using PeerReadyHandler = std::function<void(ConnectionId)>;
	using PeerDisconnectedHandler = std::function<void(ConnectionId, const DisconnectInfo&)>;

	ConnectionManager(ITransport& transport, const ConnectionManagerConfig& config);
	~ConnectionManager();
	ConnectionManager(const ConnectionManager&) = delete;
	ConnectionManager& operator=(const ConnectionManager&) = delete;

	// Server: start accepting. Client: connect to a server. Both report results through poll().
	NetError listen(std::uint16_t port);
	NetResult<ConnectionId> connect(std::string_view address);

	NetError register_handler(MessageType type, MessageHandler handler);
	void on_peer_ready(PeerReadyHandler handler) { on_ready_ = std::move(handler); }
	void on_peer_disconnected(PeerDisconnectedHandler handler) { on_disconnected_ = std::move(handler); }

	// Sends to a Ready peer.
	NetError send(ConnectionId peer, MessageType type, std::span<const std::byte> payload, Delivery delivery);
	// Sends to every Ready peer; returns the first error but attempts all peers.
	NetError broadcast(MessageType type, std::span<const std::byte> payload, Delivery delivery);
	NetError disconnect(ConnectionId peer, CloseReason reason, std::string_view debug = {});

	// Drains the transport and dispatches. `now` is a monotonic timestamp from the host; the
	// manager never reads a clock itself, so equal inputs give equal behavior.
	NetError poll(std::chrono::nanoseconds now);

	// Closes every peer with CloseReason::Shutdown. Idempotent; also run by the destructor.
	void shutdown() noexcept;

	[[nodiscard]] const PeerInfo* peer(ConnectionId connection) const noexcept;
	[[nodiscard]] std::vector<PeerInfo> peers() const;
	[[nodiscard]] std::size_t ready_peer_count() const noexcept;
	[[nodiscard]] const NetworkDiagnostics& diagnostics() const noexcept { return diagnostics_; }
	[[nodiscard]] NetResult<ConnectionStats> transport_stats(ConnectionId connection) const;
	[[nodiscard]] const ConnectionManagerConfig& config() const noexcept { return config_; }

private:
	struct Peer {
		PeerInfo info;
		bool seen = false;
	};

	void handle_event(const TransportEvent& event);
	void handle_message(const ReceivedMessage& message);
	void handle_hello(Peer& peer, ByteReader& reader);
	void handle_hello_ack(Peer& peer, ByteReader& reader);
	NetError send_raw(Peer& peer, MessageType type, std::span<const std::byte> payload, Delivery delivery);
	void close_locally(ConnectionId connection, CloseReason reason, std::string_view debug);
	void violation(ConnectionId connection, std::string_view what);
	void mark_ready(Peer& peer);
	void notify_disconnected(ConnectionId connection, const DisconnectInfo& info);

	ITransport& transport_;
	ConnectionManagerConfig config_;
	std::map<ConnectionId, Peer> peers_;
	std::map<MessageType, MessageHandler> handlers_;
	PeerReadyHandler on_ready_;
	PeerDisconnectedHandler on_disconnected_;
	NetworkDiagnostics diagnostics_;
	std::vector<TransportEvent> event_scratch_;
	std::vector<ReceivedMessage> message_scratch_;
	std::map<ConnectionId, std::size_t> poll_message_counts_;
};

} // namespace engine::net
