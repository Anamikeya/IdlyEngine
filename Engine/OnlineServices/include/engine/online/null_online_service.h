#pragma once

#include "engine/online/online_service.h"

#include <set>

namespace engine::online {

// Local development service. Every process is a signed-in user with a configurable id.
// Tickets are "null-v1|<audience>|<user id>" and are NOT secure: verification only proves the
// ticket is well formed and was issued for this audience. Never ship a server that trusts it.
class NullOnlineService final : public IOnlineService {
public:
	explicit NullOnlineService(std::string user_id = "local-user", std::string display_name = "Local User");

	[[nodiscard]] Provider provider() const noexcept override { return Provider::Null; }
	OnlineError initialize() override;
	void shutdown() noexcept override;
	[[nodiscard]] bool initialized() const noexcept override { return initialized_; }
	void update() override {}

	[[nodiscard]] OnlineResult<LocalUser> local_user() const override;
	OnlineResult<AuthTicket> create_auth_ticket(std::string_view audience) override;
	OnlineResult<PlatformUserId> verify_auth_ticket(const AuthTicket& ticket, std::string_view audience) override;

	OnlineError unlock_achievement(std::string_view achievement_id) override;
	[[nodiscard]] OnlineResult<bool> is_achievement_unlocked(std::string_view achievement_id) const override;

private:
	LocalUser user_;
	bool initialized_ = false;
	std::set<std::string, std::less<>> achievements_;
};

} // namespace engine::online
