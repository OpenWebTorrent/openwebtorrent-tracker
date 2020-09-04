#ifndef OWT_PEERCONTEXT_H
#define OWT_PEERCONTEXT_H

#include <set>
#include <string>
#include <json.hpp>
#include <App.h>

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

	PeerContext(uWS::WebSocket<true, true>* ws, std::string id = "") {
		this->ws = ws;
		this->id = id;
	}

	~PeerContext() {
		this->ws = NULL;
		this->id.clear();
		this->infoHashes.clear();
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

	void setWebsocket(uWS::WebSocket<true, true>* ws) {
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
