#ifndef OWT_PEERCONTEXT_H
#define OWT_PEERCONTEXT_H

#include <set>
#include <string>
#include <string_view>
#include <json.hpp>

#include "Utils.h"

using namespace nlohmann;

namespace owt {

struct PeerContext {
	// Type-erased transport so the tracker logic works with both SSL and
	// plain websockets (and can be unit tested without a network).
	using SendFn = void (*)(void* socket, std::string_view message);

	void* socket = nullptr;
	SendFn sendFn = nullptr;

	std::string id;
	std::set<std::string> infoHashes;

	// Client address key (see Utils::addressKey) and message rate limiter
	std::string address;
	Utils::TokenBucket rateLimiter;

	void sendMessage(const json& data) {
		if (this->socket != nullptr && this->sendFn != nullptr) {
			this->sendFn(this->socket, data.dump(-1, ' ', false, json::error_handler_t::replace));
		}
	}
};

};

#endif // OWT_PEERCONTEXT_H
