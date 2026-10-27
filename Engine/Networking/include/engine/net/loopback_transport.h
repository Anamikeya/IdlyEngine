#pragma once

#include "engine/net/transport.h"

#include <cstdint>
#include <memory>

namespace engine::net {

struct LoopbackConfig {
	// Probability in [0, 1] that an unreliable message is dropped. Reliable messages are never dropped.
	double unreliable_loss = 0.0;
	// Seed for the loss generator; equal seeds and inputs give equal drops.
	std::uint64_t seed = 0x9e3779b97f4a7c15ull;
	std::size_t max_message_size = 512 * 1024;
};

// An in-process network that LoopbackTransport endpoints attach to. Addresses are
// "loopback:<port>" (or just "<port>"). Messages are delivered on the receiver's next poll().
// Copyable handle; the network lives as long as any handle or endpoint refers to it.
class LoopbackNetwork {
public:
	explicit LoopbackNetwork(const LoopbackConfig& config = {});

private:
	friend class LoopbackTransport;
	struct Hub;
	std::shared_ptr<Hub> hub_;
};

class LoopbackTransport final : public ITransport {
public:
	explicit LoopbackTransport(const LoopbackNetwork& network);
	~LoopbackTransport() override;
	LoopbackTransport(const LoopbackTransport&) = delete;
	LoopbackTransport& operator=(const LoopbackTransport&) = delete;

	NetError listen(std::uint16_t port) override;
	NetResult<ConnectionId> connect(std::string_view address) override;
	NetError accept(ConnectionId connection) override;
	NetError close(ConnectionId connection, std::int32_t reason_code, std::string_view debug) override;
	NetError send(ConnectionId connection, std::span<const std::byte> data, Delivery delivery) override;
	NetError poll(std::vector<TransportEvent>& events, std::vector<ReceivedMessage>& messages) override;
	[[nodiscard]] NetResult<ConnectionStats> stats(ConnectionId connection) const override;
	void shutdown() noexcept override;
	[[nodiscard]] std::size_t max_message_size() const noexcept override;

	// Number of unreliable messages this endpoint has sent that the network dropped.
	[[nodiscard]] std::uint64_t dropped_unreliable() const noexcept;

	struct Endpoint;

private:
	std::shared_ptr<LoopbackNetwork::Hub> hub_;
	std::unique_ptr<Endpoint> endpoint_;
};

} // namespace engine::net
