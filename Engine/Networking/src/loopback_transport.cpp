#include "engine/net/loopback_transport.h"

#include <charconv>
#include <map>
#include <unordered_map>

namespace engine::net {

struct LoopbackNetwork::Hub {
	LoopbackConfig config;
	std::uint64_t rng_state;
	// Ids are unique across the whole network, which makes test output unambiguous.
	std::uint32_t next_connection_id = 1;
	std::unordered_map<std::uint16_t, LoopbackTransport::Endpoint*> listeners;

	explicit Hub(const LoopbackConfig& c) : config(c), rng_state(c.seed) {}

	// splitmix64: tiny, seedable and identical on every platform.
	double next_unit() noexcept
	{
		std::uint64_t z = (rng_state += 0x9e3779b97f4a7c15ull);
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
		z ^= z >> 31;
		return static_cast<double>(z >> 11) * 0x1.0p-53;
	}
};

struct LoopbackTransport::Endpoint {
	enum class State : std::uint8_t { PendingIncoming, Connecting, Connected };

	struct Connection {
		State state = State::Connecting;
		Endpoint* peer = nullptr;
		std::uint32_t peer_id = 0;
		std::uint64_t bytes_sent = 0;
		std::uint64_t bytes_received = 0;
	};

	LoopbackNetwork::Hub* hub = nullptr;
	std::map<std::uint32_t, Connection> connections;
	std::vector<std::uint16_t> ports;
	std::vector<TransportEvent> events;
	std::vector<ReceivedMessage> messages;
	std::uint64_t dropped_unreliable = 0;

	void push_event(TransportEvent::Type type, std::uint32_t id, std::int32_t code = 0, std::string debug = {})
	{
		events.push_back(TransportEvent{type, ConnectionId{id}, code, std::move(debug)});
	}

	// Removes the local side and notifies the peer, if any.
	void close_connection(std::uint32_t id, std::int32_t code, std::string_view debug)
	{
		const auto it = connections.find(id);
		if (it == connections.end()) {
			return;
		}
		const Connection local = it->second;
		connections.erase(it);
		if (local.peer != nullptr) {
			local.peer->peer_closed(local.peer_id, code, debug);
		}
	}

	void peer_closed(std::uint32_t id, std::int32_t code, std::string_view debug)
	{
		if (connections.erase(id) != 0) {
			push_event(TransportEvent::Type::Disconnected, id, code, std::string(debug));
		}
	}
};

namespace {

bool parse_port(std::string_view address, std::uint16_t& port) noexcept
{
	constexpr std::string_view prefix = "loopback:";
	if (address.starts_with(prefix)) {
		address.remove_prefix(prefix.size());
	}
	const char* end = address.data() + address.size();
	const auto [ptr, ec] = std::from_chars(address.data(), end, port);
	return ec == std::errc{} && ptr == end && !address.empty();
}

} // namespace

LoopbackNetwork::LoopbackNetwork(const LoopbackConfig& config) : hub_(std::make_shared<Hub>(config)) {}

LoopbackTransport::LoopbackTransport(const LoopbackNetwork& network)
	: hub_(network.hub_), endpoint_(std::make_unique<Endpoint>())
{
	endpoint_->hub = hub_.get();
}

LoopbackTransport::~LoopbackTransport()
{
	shutdown();
}

NetError LoopbackTransport::listen(std::uint16_t port)
{
	if (!hub_->listeners.try_emplace(port, endpoint_.get()).second) {
		return NetError::AddressInUse;
	}
	endpoint_->ports.push_back(port);
	return NetError::None;
}

NetResult<ConnectionId> LoopbackTransport::connect(std::string_view address)
{
	std::uint16_t port = 0;
	if (!parse_port(address, port)) {
		return NetError::InvalidArgument;
	}

	const std::uint32_t local_id = hub_->next_connection_id++;
	const auto listener = hub_->listeners.find(port);
	if (listener == hub_->listeners.end()) {
		// Asynchronous failure, like a UDP transport timing out.
		endpoint_->push_event(TransportEvent::Type::Disconnected, local_id, static_cast<std::int32_t>(CloseReason::Transport),
			"no listener on loopback port");
		return ConnectionId{local_id};
	}

	Endpoint* server = listener->second;
	const std::uint32_t remote_id = hub_->next_connection_id++;
	endpoint_->connections[local_id] = Endpoint::Connection{Endpoint::State::Connecting, server, remote_id};
	server->connections[remote_id] = Endpoint::Connection{Endpoint::State::PendingIncoming, endpoint_.get(), local_id};
	server->push_event(TransportEvent::Type::IncomingConnection, remote_id);
	return ConnectionId{local_id};
}

NetError LoopbackTransport::accept(ConnectionId connection)
{
	const auto it = endpoint_->connections.find(connection.value);
	if (it == endpoint_->connections.end()) {
		return NetError::InvalidConnection;
	}
	Endpoint::Connection& local = it->second;
	if (local.state != Endpoint::State::PendingIncoming || local.peer == nullptr) {
		return NetError::InvalidState;
	}
	local.state = Endpoint::State::Connected;
	endpoint_->push_event(TransportEvent::Type::Connected, connection.value);

	const auto peer = local.peer->connections.find(local.peer_id);
	if (peer != local.peer->connections.end()) {
		peer->second.state = Endpoint::State::Connected;
		local.peer->push_event(TransportEvent::Type::Connected, local.peer_id);
	}
	return NetError::None;
}

NetError LoopbackTransport::close(ConnectionId connection, std::int32_t reason_code, std::string_view debug)
{
	if (!endpoint_->connections.contains(connection.value)) {
		return NetError::InvalidConnection;
	}
	endpoint_->close_connection(connection.value, reason_code, debug);
	return NetError::None;
}

NetError LoopbackTransport::send(ConnectionId connection, std::span<const std::byte> data, Delivery delivery)
{
	const auto it = endpoint_->connections.find(connection.value);
	if (it == endpoint_->connections.end()) {
		return NetError::InvalidConnection;
	}
	Endpoint::Connection& local = it->second;
	if (local.state != Endpoint::State::Connected || local.peer == nullptr) {
		return NetError::InvalidState;
	}
	if (data.size() > hub_->config.max_message_size) {
		return NetError::MessageTooLarge;
	}
	local.bytes_sent += data.size();

	if (delivery == Delivery::Unreliable && hub_->config.unreliable_loss > 0.0 &&
		hub_->next_unit() < hub_->config.unreliable_loss) {
		++endpoint_->dropped_unreliable;
		return NetError::None; // a lossy network reports success to the sender
	}

	Endpoint* peer = local.peer;
	if (const auto remote = peer->connections.find(local.peer_id); remote != peer->connections.end()) {
		remote->second.bytes_received += data.size();
	}
	peer->messages.push_back(ReceivedMessage{ConnectionId{local.peer_id}, delivery, {data.begin(), data.end()}});
	return NetError::None;
}

NetError LoopbackTransport::poll(std::vector<TransportEvent>& events, std::vector<ReceivedMessage>& messages)
{
	for (TransportEvent& e : endpoint_->events) {
		events.push_back(std::move(e));
	}
	for (ReceivedMessage& m : endpoint_->messages) {
		messages.push_back(std::move(m));
	}
	endpoint_->events.clear();
	endpoint_->messages.clear();
	return NetError::None;
}

NetResult<ConnectionStats> LoopbackTransport::stats(ConnectionId connection) const
{
	const auto it = endpoint_->connections.find(connection.value);
	if (it == endpoint_->connections.end()) {
		return NetError::InvalidConnection;
	}
	ConnectionStats stats;
	stats.ping_ms = 0;
	stats.quality_local = static_cast<float>(1.0 - hub_->config.unreliable_loss);
	stats.quality_remote = stats.quality_local;
	return stats;
}

void LoopbackTransport::shutdown() noexcept
{
	if (!endpoint_) {
		return;
	}
	for (const std::uint16_t port : endpoint_->ports) {
		hub_->listeners.erase(port);
	}
	endpoint_->ports.clear();
	while (!endpoint_->connections.empty()) {
		endpoint_->close_connection(endpoint_->connections.begin()->first, static_cast<std::int32_t>(CloseReason::Shutdown),
			"endpoint shut down");
	}
}

std::size_t LoopbackTransport::max_message_size() const noexcept
{
	return hub_->config.max_message_size;
}

std::uint64_t LoopbackTransport::dropped_unreliable() const noexcept
{
	return endpoint_->dropped_unreliable;
}

} // namespace engine::net
