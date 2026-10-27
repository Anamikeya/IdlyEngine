#pragma once

// Platform identity and services (Steamworks, Epic Online Services, Google Play Games, ...).
//
// Deliberately independent of engine::net: a transport moves bytes between connections, while an
// online service says who a player is. The session layer joins the two by carrying an AuthTicket
// over the transport and verifying it with the server's IOnlineService. No provider SDK is
// integrated yet; NullOnlineService is the local development implementation.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::online {

enum class Provider : std::uint8_t {
	Null,
	Steam,
	EpicOnlineServices,
	GooglePlayGames,
};

const char* to_string(Provider provider) noexcept;

enum class OnlineError : std::uint8_t {
	None = 0,
	NotInitialized,
	AlreadyInitialized,
	InitializationFailed,
	NotSignedIn,
	InvalidArgument,
	InvalidTicket,
	Unsupported,
};

const char* to_string(OnlineError error) noexcept;

template <typename T>
class OnlineResult {
public:
	OnlineResult(T value) : value_(std::move(value)) {}
	OnlineResult(OnlineError error) : value_(), error_(error) {}

	[[nodiscard]] bool ok() const noexcept { return error_ == OnlineError::None; }
	explicit operator bool() const noexcept { return ok(); }
	[[nodiscard]] OnlineError error() const noexcept { return error_; }
	[[nodiscard]] T& value() & noexcept { return value_; }
	[[nodiscard]] const T& value() const& noexcept { return value_; }
	// By value: a reference into a temporary result would dangle at the end of the statement.
	[[nodiscard]] T value() && noexcept { return std::move(value_); }

private:
	T value_;
	OnlineError error_ = OnlineError::None;
};

// Stable, provider-scoped player identity. `id` is the provider's own account id rendered as text
// (SteamID64, EOS ProductUserId, Play Games player id). Never derive authority from display names.
struct PlatformUserId {
	Provider provider = Provider::Null;
	std::string id;

	[[nodiscard]] bool empty() const noexcept { return id.empty(); }
	friend bool operator==(const PlatformUserId&, const PlatformUserId&) = default;
};

struct LocalUser {
	PlatformUserId id;
	std::string display_name;
};

// Opaque proof of identity produced by a client and verified by the server. Bounded so it can
// travel inside one network message.
struct AuthTicket {
	static constexpr std::size_t max_size = 4096;
	Provider provider = Provider::Null;
	std::vector<std::byte> data;
};

class IOnlineService {
public:
	virtual ~IOnlineService() = default;

	[[nodiscard]] virtual Provider provider() const noexcept = 0;
	virtual OnlineError initialize() = 0;
	// Idempotent.
	virtual void shutdown() noexcept = 0;
	[[nodiscard]] virtual bool initialized() const noexcept = 0;
	// Pumps provider callbacks. Call once per frame from the owning thread.
	virtual void update() = 0;

	[[nodiscard]] virtual OnlineResult<LocalUser> local_user() const = 0;
	// Client side: produce a ticket identifying the local user to `audience` (e.g. a server id).
	virtual OnlineResult<AuthTicket> create_auth_ticket(std::string_view audience) = 0;
	// Server side: verify a ticket received from a client for this server's `audience`.
	virtual OnlineResult<PlatformUserId> verify_auth_ticket(const AuthTicket& ticket, std::string_view audience) = 0;

	virtual OnlineError unlock_achievement(std::string_view achievement_id) = 0;
	[[nodiscard]] virtual OnlineResult<bool> is_achievement_unlocked(std::string_view achievement_id) const = 0;
};

} // namespace engine::online
