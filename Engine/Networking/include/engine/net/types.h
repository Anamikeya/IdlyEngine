#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace engine::net {

enum class NetError : std::uint8_t {
	None = 0,
	NotInitialized,
	InitializationFailed,
	InvalidArgument,
	InvalidConnection,
	InvalidState,
	AddressInUse,
	ListenFailed,
	ConnectFailed,
	MessageTooLarge,
	SendFailed,
	LimitReached,
	ProtocolViolation,
};

const char* to_string(NetError error) noexcept;

// Value-or-error. Kept local to networking until engine::core publishes a shared Result type.
template <typename T>
class NetResult {
public:
	NetResult(T value) : value_(std::move(value)) {}
	NetResult(NetError error) : value_(), error_(error) {}

	[[nodiscard]] bool ok() const noexcept { return error_ == NetError::None; }
	explicit operator bool() const noexcept { return ok(); }
	[[nodiscard]] NetError error() const noexcept { return error_; }
	[[nodiscard]] T& value() & noexcept { return value_; }
	[[nodiscard]] const T& value() const& noexcept { return value_; }
	// By value: a reference into a temporary result would dangle at the end of the statement.
	[[nodiscard]] T value() && noexcept { return std::move(value_); }

private:
	T value_;
	NetError error_ = NetError::None;
};

// Transport-local connection handle. Never serialized and never sent to peers: it only names a
// connection on this endpoint. Value 0 is invalid.
struct ConnectionId {
	std::uint32_t value = 0;

	[[nodiscard]] bool is_null() const noexcept { return value == 0; }
	friend bool operator==(const ConnectionId&, const ConnectionId&) = default;
	friend auto operator<=>(const ConnectionId&, const ConnectionId&) = default;
};

enum class Delivery : std::uint8_t {
	// May be dropped, duplicated never, reordered relative to other unreliable messages.
	Unreliable,
	// Delivered exactly once, in order with other reliable messages on the same connection.
	Reliable,
};

// Application close codes. Values sit in the GameNetworkingSockets application range [1000, 1999]
// so they pass through that transport unchanged.
enum class CloseReason : std::int32_t {
	Normal = 1000,
	ServerFull = 1001,
	ProtocolMismatch = 1002,
	ProtocolViolation = 1003,
	Kicked = 1004,
	Shutdown = 1005,
	HandshakeTimeout = 1006,
	// Not an application code: the transport closed the connection (timeout, unreachable, ...).
	Transport = 0,
};

const char* to_string(CloseReason reason) noexcept;

// Maps a raw end-reason integer from a transport onto CloseReason (Transport for unknown codes).
CloseReason close_reason_from_code(std::int32_t code) noexcept;

struct TransportEvent {
	enum class Type : std::uint8_t {
		// A remote endpoint wants to connect. Call accept() or close() on the transport.
		IncomingConnection,
		Connected,
		// The connection is gone. No further events or messages arrive for it.
		Disconnected,
	};

	Type type = Type::Connected;
	ConnectionId connection;
	// Raw transport end reason (see CloseReason) and a human readable diagnostic.
	std::int32_t reason_code = 0;
	std::string debug;
};

struct ReceivedMessage {
	ConnectionId connection;
	Delivery delivery = Delivery::Reliable;
	std::vector<std::byte> data;
};

struct ConnectionStats {
	int ping_ms = 0;
	// [0, 1]; fraction of packets delivered as seen locally / reported by the remote. -1 if unknown.
	float quality_local = -1.0f;
	float quality_remote = -1.0f;
	float out_bytes_per_sec = 0.0f;
	float in_bytes_per_sec = 0.0f;
	int pending_reliable_bytes = 0;
	int pending_unreliable_bytes = 0;
};

} // namespace engine::net
