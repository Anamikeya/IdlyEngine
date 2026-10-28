#pragma once

// UDP transport over Valve GameNetworkingSockets. Only available when the engine is configured with
// GAMEENGINE_WITH_GNS=ON (target engine::networking_gns). No GNS type appears in this header.

#include "engine/net/transport.h"

#include <memory>

namespace engine::net {

struct GnsConfig {
	// Without certificates every connection is unauthenticated. Must stay true until an
	// identity/cert flow exists (see engine::online for platform identity).
	bool allow_unauthenticated = true;
	int connect_timeout_ms = 10'000;
	int connected_timeout_ms = 10'000;
};

class GnsTransport final : public ITransport {
public:
	// The GNS library is initialized on the first create() and shut down with the last transport.
	static NetResult<std::unique_ptr<GnsTransport>> create(const GnsConfig& config = {});
	~GnsTransport() override;
	GnsTransport(const GnsTransport&) = delete;
	GnsTransport& operator=(const GnsTransport&) = delete;

	// Binds all interfaces (IPv4 and IPv6) on the port.
	NetError listen(std::uint16_t port) override;
	// "a.b.c.d:port" or "[ipv6]:port".
	NetResult<ConnectionId> connect(std::string_view address) override;
	NetError accept(ConnectionId connection) override;
	NetError close(ConnectionId connection, std::int32_t reason_code, std::string_view debug) override;
	NetError send(ConnectionId connection, std::span<const std::byte> data, Delivery delivery) override;
	NetError poll(std::vector<TransportEvent>& events, std::vector<ReceivedMessage>& messages) override;
	[[nodiscard]] NetResult<ConnectionStats> stats(ConnectionId connection) const override;
	void shutdown() noexcept override;
	[[nodiscard]] std::size_t max_message_size() const noexcept override;

	struct Impl;

private:
	explicit GnsTransport(std::unique_ptr<Impl> impl) noexcept;
	std::unique_ptr<Impl> impl_;
};

} // namespace engine::net
