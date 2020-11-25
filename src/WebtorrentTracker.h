#ifndef OWT_WEBTORRENT_TRACKER_H
#define OWT_WEBTORRENT_TRACKER_H

#include <exception>
#include <iostream>
#include <string>
#include <vector>
#include <json.hpp>
#include <App.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/stat.h>

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

	std::string keyFile;
	std::string certFile;

	int websocketCount;
	Tracker* tracker;

public:

	WebtorrentTracker(int port, std::string host, std::string keyFile, std::string certFile) {
		// Settings
		this->port = port;
		this->host = host;

		this->keyFile = keyFile;
		this->certFile = certFile;

		// Initialize
		this->websocketCount = 0;
		this->tracker = new FastTracker();
	}

	WebtorrentTracker(int port, std::string keyFile, std::string certFile) : WebtorrentTracker(port, "0.0.0.0", keyFile, certFile) {}

	WebtorrentTracker(std::string host, std::string keyFile, std::string certFile) : WebtorrentTracker(0, host, keyFile, certFile) {}

	WebtorrentTracker(int port) : WebtorrentTracker(port, "0.0.0.0", "", "") {}

	WebtorrentTracker(std::string host) : WebtorrentTracker(0, host, "", "") {}

	void run(int numThreads = 0) {
		// TODO: add threads
		// TODO: add non-ssl app

		// Check if files exists
		bool exists = this->fileExists(this->keyFile) && this->fileExists(this->certFile);
		if (!exists) {
			throw std::runtime_error("Cert and key must exists");
		}

		uWS::SSLApp({
			.key_file_name = this->keyFile.c_str(),
			.cert_file_name = this->certFile.c_str()
		})
		.ws<PeerContext>("/*", {
			/* Settings */
			.compression = uWS::SHARED_COMPRESSOR,
			.maxPayloadLength = 64 * 1024,
			.idleTimeout = 240,
			.maxBackpressure = 1 * 1024 * 1204,
			/* Handlers */
			.upgrade = nullptr,
			.open = [this](auto* ws) {
				this->onOpen(ws);
			},
			.message = [this](auto* ws, std::string_view message, uWS::OpCode opCode) {
				this->onMessage(ws, message, opCode);
			},
			.drain = [](auto* ws) {
				// Check ws->getBufferedAmount() here
			},
			.ping = [](auto *ws) {
				// Not implemented yet
			},
			.pong = [](auto *ws) {
				// Not implemented yet
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

			// Server stats
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

	void onOpen(uWS::WebSocket<true, true>* ws) {
		DOUT("CONNECTED");

		this->websocketCount++;

		// Intiialze peer context
		PeerContext* peer = static_cast<PeerContext*>(ws->getUserData());
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
			PeerContext* peerContext = static_cast<PeerContext*>(ws->getUserData());
			this->tracker->processMessage(data, peerContext);
		} catch (TrackerException& e) {
			EOUT("Error: " << e.what());
			ws->close();
		}
	}

	void onClose(uWS::WebSocket<true, true>* ws, int code, std::string_view message) {
		DOUT("DISCONNECTED");

		this->websocketCount--;

		PeerContext* peer = static_cast<PeerContext*>(ws->getUserData());
		if (!peer->id.empty()) {
			this->tracker->disconnectPeer(peer);
		}
	}

	bool fileExists(const std::string& filename) {
		struct stat buffer;
		return (stat (filename.c_str(), &buffer) == 0);
	}

};

};

#endif // OWT_WEBTORRENT_TRACKER_H

