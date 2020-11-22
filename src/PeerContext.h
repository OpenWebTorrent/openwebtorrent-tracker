#ifndef OWT_PEERCONTEXT_H
#define OWT_PEERCONTEXT_H

#include <set>
#include <string>
#include <App.h>
#include <json.hpp>

#include "Utils.h"

using namespace nlohmann;

namespace owt {

class PeerContext {
public:
	uWS::WebSocket<true, true>* ws;
	std::string id;
	std::set<std::string> infoHashes;

	PeerContext() {
		this->ws = NULL;
		this->id = "";
	}

	PeerContext(auto* ws, std::string id = "") {
		this->ws = ws;
		this->id = id;
	}

	~PeerContext() {
		this->ws = NULL;
	}

	uWS::WebSocket<true, true>* getWebsocket() {
		return this->ws;
	}

	std::string getId() {
		return this->id;
	}

	std::set<std::string> getInfoHashes() {
		return this->infoHashes;
	}

	void setWebsocket(auto* ws) {
		this->ws = ws;
	}

	void setId(std::string id) {
		this->id = id;
	}

	void sendMessage(json data) {
		// Check if the websocket is valid
		if (this->ws != NULL) {
			this->ws->send(data.dump(), uWS::OpCode::TEXT);
		}
	}
};

};

#endif // OWT_PEERCONTEXT_H
