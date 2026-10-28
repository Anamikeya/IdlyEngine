#include "engine/online/null_online_service.h"

#include <cstring>

namespace engine::online {
namespace {

constexpr std::string_view ticket_prefix = "null-v1|";
constexpr std::size_t max_field_length = 256;

bool valid_field(std::string_view s) noexcept
{
	return !s.empty() && s.size() <= max_field_length && s.find('|') == std::string_view::npos;
}

} // namespace

const char* to_string(Provider provider) noexcept
{
	switch (provider) {
	case Provider::Null:
		return "null";
	case Provider::Steam:
		return "steam";
	case Provider::EpicOnlineServices:
		return "epic-online-services";
	case Provider::GooglePlayGames:
		return "google-play-games";
	}
	return "unknown";
}

const char* to_string(OnlineError error) noexcept
{
	switch (error) {
	case OnlineError::None:
		return "none";
	case OnlineError::NotInitialized:
		return "online service not initialized";
	case OnlineError::AlreadyInitialized:
		return "online service already initialized";
	case OnlineError::InitializationFailed:
		return "online service initialization failed";
	case OnlineError::NotSignedIn:
		return "not signed in";
	case OnlineError::InvalidArgument:
		return "invalid argument";
	case OnlineError::InvalidTicket:
		return "invalid auth ticket";
	case OnlineError::Unsupported:
		return "unsupported by provider";
	}
	return "unknown online error";
}

NullOnlineService::NullOnlineService(std::string user_id, std::string display_name)
	: user_{PlatformUserId{Provider::Null, std::move(user_id)}, std::move(display_name)}
{
}

OnlineError NullOnlineService::initialize()
{
	if (initialized_) {
		return OnlineError::AlreadyInitialized;
	}
	if (!valid_field(user_.id.id)) {
		return OnlineError::InitializationFailed;
	}
	initialized_ = true;
	return OnlineError::None;
}

void NullOnlineService::shutdown() noexcept
{
	initialized_ = false;
	achievements_.clear();
}

OnlineResult<LocalUser> NullOnlineService::local_user() const
{
	if (!initialized_) {
		return OnlineError::NotInitialized;
	}
	return user_;
}

OnlineResult<AuthTicket> NullOnlineService::create_auth_ticket(std::string_view audience)
{
	if (!initialized_) {
		return OnlineError::NotInitialized;
	}
	if (!valid_field(audience)) {
		return OnlineError::InvalidArgument;
	}
	std::string text(ticket_prefix);
	text.append(audience).append("|").append(user_.id.id);
	AuthTicket ticket;
	ticket.provider = Provider::Null;
	const auto bytes = std::as_bytes(std::span<const char>(text.data(), text.size()));
	ticket.data.assign(bytes.begin(), bytes.end());
	return ticket;
}

OnlineResult<PlatformUserId> NullOnlineService::verify_auth_ticket(const AuthTicket& ticket, std::string_view audience)
{
	if (!initialized_) {
		return OnlineError::NotInitialized;
	}
	if (ticket.provider != Provider::Null || ticket.data.size() > AuthTicket::max_size) {
		return OnlineError::InvalidTicket;
	}
	std::string text(ticket.data.size(), '\0');
	std::memcpy(text.data(), ticket.data.data(), ticket.data.size());
	std::string_view view(text);
	if (!view.starts_with(ticket_prefix)) {
		return OnlineError::InvalidTicket;
	}
	view.remove_prefix(ticket_prefix.size());
	const std::size_t split = view.find('|');
	if (split == std::string_view::npos) {
		return OnlineError::InvalidTicket;
	}
	const std::string_view ticket_audience = view.substr(0, split);
	const std::string_view user = view.substr(split + 1);
	if (ticket_audience != audience || !valid_field(user)) {
		return OnlineError::InvalidTicket;
	}
	return PlatformUserId{Provider::Null, std::string(user)};
}

OnlineError NullOnlineService::unlock_achievement(std::string_view achievement_id)
{
	if (!initialized_) {
		return OnlineError::NotInitialized;
	}
	if (!valid_field(achievement_id)) {
		return OnlineError::InvalidArgument;
	}
	achievements_.emplace(achievement_id);
	return OnlineError::None;
}

OnlineResult<bool> NullOnlineService::is_achievement_unlocked(std::string_view achievement_id) const
{
	if (!initialized_) {
		return OnlineError::NotInitialized;
	}
	return achievements_.find(achievement_id) != achievements_.end();
}

} // namespace engine::online
