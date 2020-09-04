#ifndef OWT_WEBTORRENT_TRACKER_H
#define OWT_WEBTORRENT_TRACKER_H

#include <exception>
#include <string>
#include <iostream>
#include <vector>
#include <json.hpp>
#include <App.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "Utils.h"
#include "Swarm.h"
#include "Tracker.h"
#include "FastTracker.h"
#include "DataObject.h"
#include "PeerContext.h"

using namespace nlohmann;

namespace owt {

class WebtorrentTracker {

private:
	int port;
	std::string host;

	std::string key_file;
	std::string cert_file;

	int websocketCount;
	Tracker* tracker;

public:

	WebtorrentTracker(int port, std::string host, std::string key_file, std::string cert_file) {
		// Settings
		this->port = port;
		this->host = host;

		this->key_file = key_file;
		this->cert_file = cert_file;

		// Initialize
		this->websocketCount = 0;
		this->tracker = new FastTracker();
	}

	WebtorrentTracker(int port, std::string key_file, std::string cert_file) : WebtorrentTracker(port, "0.0.0.0", key_file, cert_file) {}

	WebtorrentTracker(std::string host, std::string key_file, std::string cert_file) : WebtorrentTracker(0, host, key_file, cert_file) {}

	WebtorrentTracker(int port) : WebtorrentTracker(port, "0.0.0.0", "", "") {}

	WebtorrentTracker(std::string host) : WebtorrentTracker(0, host, "", "") {}

	void run(int numThreads = 0) {
		// TODO: add threads
		// TODO: add non-ssl app

		uWS::SSLApp({
			.key_file_name = this->key_file.c_str(),
			.cert_file_name = this->cert_file.c_str()
		}).ws<PeerContext>("/*", {
			/* Settings */
			.compression = uWS::SHARED_COMPRESSOR,
			.maxPayloadLength = 64 * 1024,
			.idleTimeout = 240,
			//.maxBackpressure = 1 * 1024 * 1204,
			/* Handlers */
			.open = [this](auto* ws, auto* req) {
				this->onOpen(ws, req);
			},
			.message = [this](auto* ws, std::string_view message, uWS::OpCode opCode) {
				this->onMessage(ws, message, opCode);
			},
			.drain = [](auto* ws) {
				/* Check getBufferedAmount here */
			},
			.ping = [](auto *ws) {
				/* Not implemented yet */
			},
			.pong = [](auto *ws) {
				/* Not implemented yet */
			},
			.close = [this](auto* ws, int code, std::string_view message) {
				this->onClose(ws, code, message);
			}
		}).get("/stats.json", [this](auto* res, auto* req) {
			int peersCount = 0;
			std::map<std::string, Swarm*> swarms = this->tracker->getSwarms();
			for (const auto& entry : swarms) {
				Swarm* swarm = entry.second;
				peersCount += swarm->getPeersCount();
			}

			std::vector<owt::ServerStats> currentStats = {
				{
					std::string(this->host + ":") + std::to_string(this->port),
					this->websocketCount
				}
			};

			// Memory usage
			struct rusage usage;
			int ret = getrusage(RUSAGE_SELF, &usage);

			owt::ProcessMemory memoryUsage = {usage.ru_maxrss, 0, 0, 0};

			json statsJSON = {
				{"torrentsCount", swarms.size()},
				{"peersCount", peersCount},
				{"servers", currentStats},
				{"memory", memoryUsage}
			};

			res->writeHeader("Content-Type", "application/json");
			res->end(statsJSON.dump());
		}).any("/*", [](auto* res, auto* request) {
			/* Any other route */
			std::string status = "404 Not Found";
			res->writeStatus(status);
			res->end(status);
        }).listen(this->port, [this](auto *token) {
			if (token) {
				COUT("Listening on port " << this->port);
			} else {
				EOUT("Error: Failed to listen on port " << this->port);
            }
		}).run();
	}

private:

	void onOpen(uWS::WebSocket<true, true>* ws, uWS::HttpRequest* req) {
		DOUT("CONNECTED");

		this->websocketCount++;

		// Intiialze peer context
		PeerContext* peer = (PeerContext*) ws->getUserData();
		peer->ws = ws;
	}

	void onMessage(uWS::WebSocket<true, true>* ws, std::string_view message, uWS::OpCode opCode) {
		DOUT("Received:");
		DOUT(message);

		// Check JSON input
		json data = json::parse(message, nullptr, false);

		if (data.is_discarded()) {
			EOUT("Invalid JSON");
			ws->close();
			return;
		}

		try {
			PeerContext* peerContext = (PeerContext*) ws->getUserData();
			this->tracker->processMessage(data, peerContext);
		} catch (TrackerException& e) {
			EOUT("Error: " << e.what());
			ws->close();
		}
	}

	void onClose(uWS::WebSocket<true, true>* ws, int code, std::string_view message) {
		DOUT("DISCONNECTED");

		this->websocketCount--;

		PeerContext* peer = (PeerContext*) ws->getUserData();
		if (!peer->id.empty()) {
			this->tracker->disconnectPeer(peer);
		}
	}

};

};

#endif // OWT_WEBTORRENT_TRACKER_H
