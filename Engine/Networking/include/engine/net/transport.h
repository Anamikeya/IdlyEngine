#pragma once

#include "engine/net/types.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::net {

// A message-oriented transport endpoint. One endpoint may listen and connect at the same time.
// Implementations: LoopbackTransport (in-process, deterministic) and GnsTransport (UDP via
// GameNetworkingSockets). Not thread safe: use from the thread that owns networking.
//
// Event model, matching GameNetworkingSockets:
//  - connect() returns immediately; success or failure arrives later as Connected/Disconnected.
//  - An incoming connection raises IncomingConnection and stays pending until accept() or close().
//  - close() is final and silent locally: the peer gets Disconnected, the closer gets no event.
class ITransport {
public:
	virtual ~ITransport() = default;

	virtual NetError listen(std::uint16_t port) = 0;
	// Address syntax is transport specific, e.g. "127.0.0.1:27020" or "loopback:7".
	virtual NetResult<ConnectionId> connect(std::string_view address) = 0;
	virtual NetError accept(ConnectionId connection) = 0;
	virtual NetError close(ConnectionId connection, std::int32_t reason_code, std::string_view debug) = 0;
	virtual NetError send(ConnectionId connection, std::span<const std::byte> data, Delivery delivery) = 0;
	// Appends pending events and messages. A peer's final messages and its Disconnected event can
	// arrive in the same poll, so consumers should handle IncomingConnection/Connected events, then
	// messages, then Disconnected events.
	virtual NetError poll(std::vector<TransportEvent>& events, std::vector<ReceivedMessage>& messages) = 0;
	[[nodiscard]] virtual NetResult<ConnectionStats> stats(ConnectionId connection) const = 0;
	// Closes every connection and listen socket. Idempotent.
	virtual void shutdown() noexcept = 0;
	[[nodiscard]] virtual std::size_t max_message_size() const noexcept = 0;
};

} // namespace engine::net
