#include "engine/net/types.h"

namespace engine::net {

const char* to_string(NetError error) noexcept
{
	switch (error) {
	case NetError::None:
		return "none";
	case NetError::NotInitialized:
		return "networking not initialized";
	case NetError::InitializationFailed:
		return "networking initialization failed";
	case NetError::InvalidArgument:
		return "invalid argument";
	case NetError::InvalidConnection:
		return "invalid connection";
	case NetError::InvalidState:
		return "invalid state";
	case NetError::AddressInUse:
		return "address in use";
	case NetError::ListenFailed:
		return "listen failed";
	case NetError::ConnectFailed:
		return "connect failed";
	case NetError::MessageTooLarge:
		return "message too large";
	case NetError::SendFailed:
		return "send failed";
	case NetError::LimitReached:
		return "limit reached";
	case NetError::ProtocolViolation:
		return "protocol violation";
	}
	return "unknown network error";
}

const char* to_string(CloseReason reason) noexcept
{
	switch (reason) {
	case CloseReason::Normal:
		return "normal";
	case CloseReason::ServerFull:
		return "server full";
	case CloseReason::ProtocolMismatch:
		return "protocol mismatch";
	case CloseReason::ProtocolViolation:
		return "protocol violation";
	case CloseReason::Kicked:
		return "kicked";
	case CloseReason::Shutdown:
		return "shutdown";
	case CloseReason::HandshakeTimeout:
		return "handshake timeout";
	case CloseReason::Transport:
		return "transport";
	}
	return "unknown close reason";
}

CloseReason close_reason_from_code(std::int32_t code) noexcept
{
	switch (static_cast<CloseReason>(code)) {
	case CloseReason::Normal:
	case CloseReason::ServerFull:
	case CloseReason::ProtocolMismatch:
	case CloseReason::ProtocolViolation:
	case CloseReason::Kicked:
	case CloseReason::Shutdown:
	case CloseReason::HandshakeTimeout:
		return static_cast<CloseReason>(code);
	case CloseReason::Transport:
		break;
	}
	return CloseReason::Transport;
}

} // namespace engine::net
