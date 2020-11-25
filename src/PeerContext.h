#ifndef OWT_PEERCONTEXT_H
#define OWT_PEERCONTEXT_H

#include <set>
#include <string>
#include <App.h>
#include <json.hpp>

#include "Utils.h"

using namespace nlohmann;

namespace owt {

struct PeerContext {
	uWS::WebSocket<true, true>* ws = nullptr;
	std::string id;
	std::set<std::string> infoHashes;

	void sendMessage(json data) {
		if (this->ws != nullptr) {
			this->ws->send(data.dump(), uWS::OpCode::TEXT);
		}
	}
};

};

#endif // OWT_PEERCONTEXT_H
