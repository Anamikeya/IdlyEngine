#include "engine/net/connection_manager.h"

#include <algorithm>

namespace engine::net {

ConnectionManager::ConnectionManager(ITransport& transport, const ConnectionManagerConfig& config)
	: transport_(transport), config_(config)
{
}

ConnectionManager::~ConnectionManager()
{
	// Owners are being torn down too; do not call back into them.
	on_ready_ = nullptr;
	on_disconnected_ = nullptr;
	shutdown();
}

NetError ConnectionManager::listen(std::uint16_t port)
{
	if (config_.role != Role::Server) {
		return NetError::InvalidState;
	}
	return transport_.listen(port);
}

NetResult<ConnectionId> ConnectionManager::connect(std::string_view address)
{
	if (config_.role != Role::Client) {
		return NetError::InvalidState;
	}
	if (!peers_.empty()) {
		return NetError::InvalidState; // a client holds one server connection
	}
	auto result = transport_.connect(address);
	if (result.ok()) {
		peers_[result.value()] = Peer{PeerInfo{result.value()}, false};
	}
	return result;
}

NetError ConnectionManager::register_handler(MessageType type, MessageHandler handler)
{
	if (type < first_user_message_type || !handler) {
		return NetError::InvalidArgument;
	}
	if (!handlers_.try_emplace(type, std::move(handler)).second) {
		return NetError::InvalidState;
	}
	return NetError::None;
}

NetError ConnectionManager::send(ConnectionId peer, MessageType type, std::span<const std::byte> payload, Delivery delivery)
{
	if (type < first_user_message_type) {
		return NetError::InvalidArgument;
	}
	const auto it = peers_.find(peer);
	if (it == peers_.end()) {
		return NetError::InvalidConnection;
	}
	if (it->second.info.state != PeerState::Ready) {
		return NetError::InvalidState;
	}
	return send_raw(it->second, type, payload, delivery);
}

NetError ConnectionManager::broadcast(MessageType type, std::span<const std::byte> payload, Delivery delivery)
{
	if (type < first_user_message_type) {
		return NetError::InvalidArgument;
	}
	NetError first_error = NetError::None;
	for (auto& [id, peer] : peers_) {
		if (peer.info.state != PeerState::Ready) {
			continue;
		}
		const NetError e = send_raw(peer, type, payload, delivery);
		if (first_error == NetError::None) {
			first_error = e;
		}
	}
	return first_error;
}

NetError ConnectionManager::disconnect(ConnectionId peer, CloseReason reason, std::string_view debug)
{
	if (!peers_.contains(peer)) {
		return NetError::InvalidConnection;
	}
	close_locally(peer, reason, debug);
	return NetError::None;
}

NetError ConnectionManager::poll(std::chrono::nanoseconds now)
{
	event_scratch_.clear();
	message_scratch_.clear();
	poll_message_counts_.clear();
	const NetError polled = transport_.poll(event_scratch_, message_scratch_);
	if (polled != NetError::None) {
		return polled;
	}

	// Handle connects first and disconnects last, so a peer's final messages are still dispatched.
	for (const TransportEvent& event : event_scratch_) {
		if (event.type != TransportEvent::Type::Disconnected) {
			handle_event(event);
		}
	}
	for (const ReceivedMessage& message : message_scratch_) {
		handle_message(message);
	}
	for (const TransportEvent& event : event_scratch_) {
		if (event.type == TransportEvent::Type::Disconnected) {
			handle_event(event);
		}
	}

	std::vector<ConnectionId> timed_out;
	for (auto& [id, peer] : peers_) {
		if (!peer.seen) {
			peer.seen = true;
			peer.info.first_seen = now;
		}
		if (peer.info.state != PeerState::Ready && config_.handshake_timeout.count() > 0 &&
			now - peer.info.first_seen >= config_.handshake_timeout) {
			timed_out.push_back(id);
		}
	}
	for (const ConnectionId id : timed_out) {
		++diagnostics_.handshake_timeouts;
		close_locally(id, CloseReason::HandshakeTimeout, "session handshake timed out");
	}
	return NetError::None;
}

void ConnectionManager::shutdown() noexcept
{
	while (!peers_.empty()) {
		close_locally(peers_.begin()->first, CloseReason::Shutdown, "connection manager shut down");
	}
}

const PeerInfo* ConnectionManager::peer(ConnectionId connection) const noexcept
{
	const auto it = peers_.find(connection);
	return it == peers_.end() ? nullptr : &it->second.info;
}

std::vector<PeerInfo> ConnectionManager::peers() const
{
	std::vector<PeerInfo> out;
	out.reserve(peers_.size());
	for (const auto& [id, peer] : peers_) {
		out.push_back(peer.info);
	}
	return out;
}

std::size_t ConnectionManager::ready_peer_count() const noexcept
{
	std::size_t count = 0;
	for (const auto& [id, peer] : peers_) {
		count += peer.info.state == PeerState::Ready ? 1 : 0;
	}
	return count;
}

NetResult<ConnectionStats> ConnectionManager::transport_stats(ConnectionId connection) const
{
	if (!peers_.contains(connection)) {
		return NetError::InvalidConnection;
	}
	return transport_.stats(connection);
}

void ConnectionManager::handle_event(const TransportEvent& event)
{
	switch (event.type) {
	case TransportEvent::Type::IncomingConnection:
		if (config_.role != Role::Server || peers_.size() >= config_.max_peers) {
			++diagnostics_.connections_rejected;
			transport_.close(event.connection, static_cast<std::int32_t>(CloseReason::ServerFull), "server full");
			return;
		}
		if (transport_.accept(event.connection) != NetError::None) {
			++diagnostics_.connections_rejected;
			return;
		}
		++diagnostics_.connections_accepted;
		peers_[event.connection] = Peer{PeerInfo{event.connection}, false};
		return;

	case TransportEvent::Type::Connected: {
		const auto it = peers_.find(event.connection);
		if (it == peers_.end()) {
			return;
		}
		Peer& peer = it->second;
		peer.info.state = PeerState::Handshaking;
		if (config_.role == Role::Client) {
			ByteWriter hello(16);
			hello.write_u32(protocol_magic);
			hello.write_u32(config_.protocol_version);
			send_raw(peer, message_type_hello, hello.bytes(), Delivery::Reliable);
		}
		return;
	}

	case TransportEvent::Type::Disconnected: {
		if (peers_.erase(event.connection) == 0) {
			return;
		}
		notify_disconnected(event.connection,
			DisconnectInfo{close_reason_from_code(event.reason_code), event.reason_code, event.debug, false});
		return;
	}
	}
}

void ConnectionManager::handle_message(const ReceivedMessage& message)
{
	const ConnectionId id = message.connection;
	auto it = peers_.find(id);
	if (it == peers_.end()) {
		return; // closed earlier in this poll
	}

	if (++poll_message_counts_[id] > config_.max_messages_per_peer_per_poll) {
		violation(id, "message flood");
		return;
	}
	if (message.data.size() < sizeof(MessageType) || message.data.size() > config_.max_message_bytes) {
		violation(id, "message size out of bounds");
		return;
	}

	Peer& peer = it->second;
	++peer.info.messages_received;
	peer.info.bytes_received += message.data.size();
	++diagnostics_.messages_received;
	diagnostics_.bytes_received += message.data.size();

	ByteReader reader(message.data);
	const MessageType type = reader.read_u16();

	if (peer.info.state != PeerState::Ready) {
		if (config_.role == Role::Server && type == message_type_hello) {
			handle_hello(peer, reader);
		} else if (config_.role == Role::Client && type == message_type_hello_ack) {
			handle_hello_ack(peer, reader);
		} else {
			violation(id, "message before handshake");
		}
		return;
	}

	if (type < first_user_message_type) {
		violation(id, "unexpected session message");
		return;
	}
	const auto handler = handlers_.find(type);
	if (handler == handlers_.end()) {
		violation(id, "unknown message type");
		return;
	}
	handler->second(id, reader);
	// The handler may have disconnected this peer; only judge the payload if it is still here.
	if (!reader.fully_consumed() && peers_.contains(id)) {
		violation(id, "malformed payload");
	}
}

void ConnectionManager::handle_hello(Peer& peer, ByteReader& reader)
{
	const std::uint32_t magic = reader.read_u32();
	const std::uint32_t version = reader.read_u32();
	if (!reader.fully_consumed() || magic != protocol_magic) {
		violation(peer.info.connection, "invalid hello");
		return;
	}
	if (version != config_.protocol_version) {
		close_locally(peer.info.connection, CloseReason::ProtocolMismatch, "protocol version mismatch");
		return;
	}
	ByteWriter ack(8);
	ack.write_u32(config_.protocol_version);
	if (send_raw(peer, message_type_hello_ack, ack.bytes(), Delivery::Reliable) == NetError::None) {
		mark_ready(peer);
	}
}

void ConnectionManager::handle_hello_ack(Peer& peer, ByteReader& reader)
{
	const std::uint32_t version = reader.read_u32();
	if (!reader.fully_consumed() || version != config_.protocol_version) {
		violation(peer.info.connection, "invalid hello ack");
		return;
	}
	mark_ready(peer);
}

NetError ConnectionManager::send_raw(Peer& peer, MessageType type, std::span<const std::byte> payload, Delivery delivery)
{
	const std::size_t limit = std::min(config_.max_message_bytes, transport_.max_message_size());
	ByteWriter writer(limit);
	writer.write_u16(type);
	writer.write_bytes(payload);
	if (!writer.ok()) {
		return NetError::MessageTooLarge;
	}
	const NetError e = transport_.send(peer.info.connection, writer.bytes(), delivery);
	if (e != NetError::None) {
		++diagnostics_.send_failures;
		return e;
	}
	++peer.info.messages_sent;
	peer.info.bytes_sent += writer.size();
	++diagnostics_.messages_sent;
	diagnostics_.bytes_sent += writer.size();
	return NetError::None;
}

void ConnectionManager::close_locally(ConnectionId connection, CloseReason reason, std::string_view debug)
{
	if (peers_.erase(connection) == 0) {
		return;
	}
	const auto code = static_cast<std::int32_t>(reason);
	transport_.close(connection, code, debug);
	notify_disconnected(connection, DisconnectInfo{reason, code, std::string(debug), true});
}

void ConnectionManager::violation(ConnectionId connection, std::string_view what)
{
	++diagnostics_.protocol_violations;
	close_locally(connection, CloseReason::ProtocolViolation, what);
}

void ConnectionManager::mark_ready(Peer& peer)
{
	peer.info.state = PeerState::Ready;
	if (on_ready_) {
		on_ready_(peer.info.connection);
	}
}

void ConnectionManager::notify_disconnected(ConnectionId connection, const DisconnectInfo& info)
{
	if (on_disconnected_) {
		on_disconnected_(connection, info);
	}
}

} // namespace engine::net
