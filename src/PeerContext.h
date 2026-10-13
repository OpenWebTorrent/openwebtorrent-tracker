#ifndef OWT_PEERCONTEXT_H
#define OWT_PEERCONTEXT_H

#include <chrono>
#include <map>
#include <set>
#include <string>
#include <App.h>
#include <json.hpp>

#include "Utils.h"

using namespace nlohmann;

namespace owt {

// Represents a WebSocket connection, not a single WebTorrent peer.
// Each peer ID on this connection has independent swarm memberships.
struct PeerContext {
	uWS::WebSocket<true, true>* ws = nullptr;
	std::set<std::string> peerIds;
	std::map<std::string, std::map<std::string, std::chrono::steady_clock::time_point>> infoHashesByPeerId;

	void sendMessage(json data) {
		if (this->ws != nullptr) {
			this->ws->send(data.dump(), uWS::OpCode::TEXT);
		}
	}
};

};

#endif // OWT_PEERCONTEXT_H
