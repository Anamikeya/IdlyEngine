#include "engine/net/gns_transport.h"

#include <steam/isteamnetworkingsockets.h>
#include <steam/steamnetworkingsockets.h>
#include <steam/steamnetworkingtypes.h>

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace engine::net {
namespace {

// GNS reports status changes through a plain function pointer dispatched from RunCallbacks(),
// which serves every connection in the process. Route each handle back to its owning transport.
// All access happens on the networking thread.
std::unordered_map<HSteamNetConnection, GnsTransport::Impl*> g_connections;
std::unordered_map<HSteamListenSocket, GnsTransport::Impl*> g_listen_sockets;
int g_library_refs = 0;

void on_status_changed(SteamNetConnectionStatusChangedCallback_t* info);

} // namespace

struct GnsTransport::Impl {
	GnsConfig config;
	ISteamNetworkingSockets* sockets = nullptr;
	HSteamNetPollGroup poll_group = k_HSteamNetPollGroup_Invalid;
	std::unordered_set<HSteamListenSocket> listen_sockets;
	std::unordered_set<HSteamNetConnection> connections;
	// Closed by the peer or the network: freed after their last messages are drained.
	std::vector<HSteamNetConnection> to_release;
	std::vector<TransportEvent> events;

	~Impl() { shutdown(); }

	int options(SteamNetworkingConfigValue_t (&out)[4]) const
	{
		out[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
			reinterpret_cast<void*>(&on_status_changed));
		out[1].SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, config.allow_unauthenticated ? 1 : 0);
		out[2].SetInt32(k_ESteamNetworkingConfig_TimeoutInitial, config.connect_timeout_ms);
		out[3].SetInt32(k_ESteamNetworkingConfig_TimeoutConnected, config.connected_timeout_ms);
		return 4;
	}

	void track(HSteamNetConnection conn)
	{
		connections.insert(conn);
		g_connections[conn] = this;
	}

	void untrack(HSteamNetConnection conn)
	{
		connections.erase(conn);
		g_connections.erase(conn);
	}

	void status_changed(const SteamNetConnectionStatusChangedCallback_t& info)
	{
		const HSteamNetConnection conn = info.m_hConn;
		switch (info.m_info.m_eState) {
		case k_ESteamNetworkingConnectionState_Connecting:
			// Only incoming connections need a decision; outgoing ones are already tracked.
			if (info.m_info.m_hListenSocket != k_HSteamListenSocket_Invalid &&
				info.m_eOldState == k_ESteamNetworkingConnectionState_None) {
				track(conn);
				events.push_back(TransportEvent{TransportEvent::Type::IncomingConnection, ConnectionId{conn}, 0, {}});
			}
			break;

		case k_ESteamNetworkingConnectionState_Connected:
			sockets->SetConnectionPollGroup(conn, poll_group);
			events.push_back(TransportEvent{TransportEvent::Type::Connected, ConnectionId{conn}, 0, {}});
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			if (connections.contains(conn)) {
				events.push_back(TransportEvent{TransportEvent::Type::Disconnected, ConnectionId{conn}, info.m_info.m_eEndReason,
					info.m_info.m_szEndDebug});
				untrack(conn);
				to_release.push_back(conn);
			}
			break;

		default:
			break;
		}
	}

	void shutdown() noexcept
	{
		if (sockets == nullptr) {
			return;
		}
		for (const HSteamNetConnection conn : connections) {
			sockets->CloseConnection(conn, static_cast<int>(CloseReason::Shutdown), "transport shut down", false);
			g_connections.erase(conn);
		}
		connections.clear();
		for (const HSteamNetConnection conn : to_release) {
			sockets->CloseConnection(conn, 0, nullptr, false);
		}
		to_release.clear();
		for (const HSteamListenSocket socket : listen_sockets) {
			sockets->CloseListenSocket(socket);
			g_listen_sockets.erase(socket);
		}
		listen_sockets.clear();
		if (poll_group != k_HSteamNetPollGroup_Invalid) {
			sockets->DestroyPollGroup(poll_group);
			poll_group = k_HSteamNetPollGroup_Invalid;
		}
		sockets = nullptr;
		if (--g_library_refs == 0) {
			GameNetworkingSockets_Kill();
		}
	}
};

namespace {

void on_status_changed(SteamNetConnectionStatusChangedCallback_t* info)
{
	GnsTransport::Impl* owner = nullptr;
	if (const auto it = g_connections.find(info->m_hConn); it != g_connections.end()) {
		owner = it->second;
	} else if (const auto ls = g_listen_sockets.find(info->m_info.m_hListenSocket); ls != g_listen_sockets.end()) {
		owner = ls->second;
	}
	if (owner != nullptr) {
		owner->status_changed(*info);
	}
}

NetError from_result(EResult result) noexcept
{
	switch (result) {
	case k_EResultOK:
		return NetError::None;
	case k_EResultNoConnection:
		return NetError::InvalidConnection;
	case k_EResultInvalidState:
		return NetError::InvalidState;
	case k_EResultInvalidParam:
		return NetError::InvalidArgument;
	case k_EResultLimitExceeded:
		return NetError::SendFailed;
	default:
		return NetError::SendFailed;
	}
}

} // namespace

NetResult<std::unique_ptr<GnsTransport>> GnsTransport::create(const GnsConfig& config)
{
	if (g_library_refs == 0) {
		SteamNetworkingErrMsg error{};
		if (!GameNetworkingSockets_Init(nullptr, error)) {
			return NetError::InitializationFailed;
		}
	}
	++g_library_refs;

	auto impl = std::make_unique<Impl>();
	impl->config = config;
	impl->sockets = SteamNetworkingSockets();
	impl->poll_group = impl->sockets->CreatePollGroup();
	if (impl->poll_group == k_HSteamNetPollGroup_Invalid) {
		return NetError::InitializationFailed; // ~Impl releases the library reference
	}
	return std::unique_ptr<GnsTransport>(new GnsTransport(std::move(impl)));
}

GnsTransport::GnsTransport(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
GnsTransport::~GnsTransport() = default;

NetError GnsTransport::listen(std::uint16_t port)
{
	if (impl_->sockets == nullptr) {
		return NetError::NotInitialized;
	}
	SteamNetworkingIPAddr address;
	address.Clear();
	address.m_port = port;
	SteamNetworkingConfigValue_t opts[4];
	const int count = impl_->options(opts);
	const HSteamListenSocket socket = impl_->sockets->CreateListenSocketIP(address, count, opts);
	if (socket == k_HSteamListenSocket_Invalid) {
		return NetError::ListenFailed;
	}
	impl_->listen_sockets.insert(socket);
	g_listen_sockets[socket] = impl_.get();
	return NetError::None;
}

NetResult<ConnectionId> GnsTransport::connect(std::string_view address)
{
	if (impl_->sockets == nullptr) {
		return NetError::NotInitialized;
	}
	const std::string text(address);
	SteamNetworkingIPAddr remote;
	if (!remote.ParseString(text.c_str()) || remote.m_port == 0) {
		return NetError::InvalidArgument;
	}
	SteamNetworkingConfigValue_t opts[4];
	const int count = impl_->options(opts);
	const HSteamNetConnection conn = impl_->sockets->ConnectByIPAddress(remote, count, opts);
	if (conn == k_HSteamNetConnection_Invalid) {
		return NetError::ConnectFailed;
	}
	impl_->track(conn);
	return ConnectionId{conn};
}

NetError GnsTransport::accept(ConnectionId connection)
{
	if (impl_->sockets == nullptr) {
		return NetError::NotInitialized;
	}
	if (!impl_->connections.contains(connection.value)) {
		return NetError::InvalidConnection;
	}
	return from_result(impl_->sockets->AcceptConnection(connection.value));
}

NetError GnsTransport::close(ConnectionId connection, std::int32_t reason_code, std::string_view debug)
{
	if (impl_->sockets == nullptr) {
		return NetError::NotInitialized;
	}
	if (!impl_->connections.contains(connection.value)) {
		return NetError::InvalidConnection;
	}
	const std::string text(debug);
	impl_->sockets->CloseConnection(connection.value, reason_code, text.c_str(), false);
	impl_->untrack(connection.value);
	return NetError::None;
}

NetError GnsTransport::send(ConnectionId connection, std::span<const std::byte> data, Delivery delivery)
{
	if (impl_->sockets == nullptr) {
		return NetError::NotInitialized;
	}
	if (!impl_->connections.contains(connection.value)) {
		return NetError::InvalidConnection;
	}
	if (data.size() > max_message_size()) {
		return NetError::MessageTooLarge;
	}
	// Game traffic is flushed every tick; Nagle batching would only add latency.
	const int flags = delivery == Delivery::Reliable ? k_nSteamNetworkingSend_ReliableNoNagle
													 : k_nSteamNetworkingSend_UnreliableNoNagle;
	return from_result(impl_->sockets->SendMessageToConnection(connection.value, data.data(),
		static_cast<uint32>(data.size()), flags, nullptr));
}

NetError GnsTransport::poll(std::vector<TransportEvent>& events, std::vector<ReceivedMessage>& messages)
{
	if (impl_->sockets == nullptr) {
		return NetError::NotInitialized;
	}
	impl_->sockets->RunCallbacks();

	SteamNetworkingMessage_t* batch[64];
	for (;;) {
		const int count = impl_->sockets->ReceiveMessagesOnPollGroup(impl_->poll_group, batch, 64);
		if (count <= 0) {
			break;
		}
		for (int i = 0; i < count; ++i) {
			SteamNetworkingMessage_t* m = batch[i];
			const auto* bytes = static_cast<const std::byte*>(m->m_pData);
			const auto size = static_cast<std::size_t>(m->m_cbSize);
			const Delivery delivery =
				(m->m_nFlags & k_nSteamNetworkingSend_Reliable) != 0 ? Delivery::Reliable : Delivery::Unreliable;
			messages.push_back(ReceivedMessage{ConnectionId{m->m_conn}, delivery, {bytes, bytes + size}});
			m->Release();
		}
	}

	// Messages from peer-closed connections were drained above; now free their handles.
	for (const HSteamNetConnection conn : impl_->to_release) {
		impl_->sockets->CloseConnection(conn, 0, nullptr, false);
	}
	impl_->to_release.clear();

	for (TransportEvent& e : impl_->events) {
		events.push_back(std::move(e));
	}
	impl_->events.clear();
	return NetError::None;
}

NetResult<ConnectionStats> GnsTransport::stats(ConnectionId connection) const
{
	if (impl_->sockets == nullptr) {
		return NetError::NotInitialized;
	}
	SteamNetConnectionRealTimeStatus_t status{};
	if (impl_->sockets->GetConnectionRealTimeStatus(connection.value, &status, 0, nullptr) != k_EResultOK) {
		return NetError::InvalidConnection;
	}
	ConnectionStats stats;
	stats.ping_ms = status.m_nPing;
	stats.quality_local = status.m_flConnectionQualityLocal;
	stats.quality_remote = status.m_flConnectionQualityRemote;
	stats.out_bytes_per_sec = status.m_flOutBytesPerSec;
	stats.in_bytes_per_sec = status.m_flInBytesPerSec;
	stats.pending_reliable_bytes = status.m_cbPendingReliable;
	stats.pending_unreliable_bytes = status.m_cbPendingUnreliable;
	return stats;
}

void GnsTransport::shutdown() noexcept
{
	impl_->shutdown();
}

std::size_t GnsTransport::max_message_size() const noexcept
{
	return static_cast<std::size_t>(k_cbMaxSteamNetworkingSocketsMessageSizeSend);
}

} // namespace engine::net
