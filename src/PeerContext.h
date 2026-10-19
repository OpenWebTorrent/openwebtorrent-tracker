#ifndef OWT_PEERCONTEXT_H
#define OWT_PEERCONTEXT_H

#include <chrono>
#include <map>
#include <string>
#include <string_view>
#include <json.hpp>

#include "Utils.h"

using namespace nlohmann;

namespace owt {

// Represents a WebSocket connection, not a single peer: clients such as
// libtorrent announce several torrents with different peer IDs on one socket.
struct PeerContext {
	using Clock = std::chrono::steady_clock;

	// Type-erased transport so the tracker logic works with both SSL and
	// plain websockets (and can be unit tested without a network).
	using SendFn = void (*)(void* socket, std::string_view message);

	void* socket = nullptr;
	SendFn sendFn = nullptr;

	// Peer ID -> info hash -> time of the last announce for that membership
	std::map<std::string, std::map<std::string, Clock::time_point>> memberships;
	size_t membershipCount = 0;

	// Client address key (see Utils::addressKey) and message rate limiter
	std::string address;
	Utils::TokenBucket rateLimiter;

	bool hasMembership(const std::string& peerId, const std::string& infoHash) const {
		auto it = this->memberships.find(peerId);
		return it != this->memberships.end() && it->second.count(infoHash) > 0;
	}

	void sendMessage(const json& data) {
		if (this->socket != nullptr && this->sendFn != nullptr) {
			this->sendFn(this->socket, data.dump(-1, ' ', false, json::error_handler_t::replace));
		}
	}
};

};

#endif // OWT_PEERCONTEXT_H
