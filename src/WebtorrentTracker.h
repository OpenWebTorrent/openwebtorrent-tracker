#ifndef OWT_WEBTORRENT_TRACKER_H
#define OWT_WEBTORRENT_TRACKER_H

#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <json.hpp>
#include <App.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Utils.h"
#include "Swarm.h"
#include "Tracker.h"
#include "FastTracker.h"
#include "DataObject.h"
#include "PeerContext.h"
#include "TrackerException.h"

using namespace nlohmann;

namespace owt {

struct ServerOptions {
	int port = 8000;
	// Empty host listens on all interfaces (IPv4 and IPv6)
	std::string host;

	// Without key and certificate the tracker serves plain (non-SSL) websockets
	std::string keyFile;
	std::string certFile;

	// Take the client address from X-Forwarded-For (only behind a reverse proxy)
	bool trustProxy = false;
	// Maximum simultaneous connections per IPv4 address or IPv6 /64 (0 = unlimited)
	size_t maxConnectionsPerIp = 0;
	// Messages per second allowed on each connection (0 = unlimited)
	double messageRate = 50;
	// Messages a connection can send in a burst before being rate limited
	double messageBurst = 500;

	TrackerOptions tracker;
};

class WebtorrentTracker {

private:
	ServerOptions options;

	size_t websocketCount = 0;
	std::unordered_map<std::string, size_t> connectionsPerAddress;
	std::unique_ptr<Tracker> tracker;

public:

	explicit WebtorrentTracker(const ServerOptions& options) : options(options) {
		this->tracker = std::make_unique<FastTracker>(options.tracker);
	}

	WebtorrentTracker(int port, std::string host, std::string keyFile, std::string certFile)
		: WebtorrentTracker(makeOptions(port, host, keyFile, certFile)) {}

	WebtorrentTracker(int port, std::string keyFile, std::string certFile) : WebtorrentTracker(port, "", keyFile, certFile) {}

	WebtorrentTracker(std::string host, std::string keyFile, std::string certFile) : WebtorrentTracker(0, host, keyFile, certFile) {}

	WebtorrentTracker(int port) : WebtorrentTracker(port, "", "", "") {}

	WebtorrentTracker(std::string host) : WebtorrentTracker(0, host, "", "") {}

	void run() {
		// TODO: add threads

		if (this->options.keyFile.empty() && this->options.certFile.empty()) {
			uWS::App app;
			this->runApp<false>(app);
			return;
		}

		// Check if files exists
		bool exists = this->fileExists(this->options.keyFile) && this->fileExists(this->options.certFile);
		if (!exists) {
			throw std::runtime_error("Cert and key must exist");
		}

		uWS::SSLApp app({
			.key_file_name = this->options.keyFile.c_str(),
			.cert_file_name = this->options.certFile.c_str()
		});
		if (app.constructorFailed()) {
			throw std::runtime_error("Failed to load SSL key or certificate");
		}
		this->runApp<true>(app);
	}

private:

	template <bool SSL>
	using WebSocket = uWS::WebSocket<SSL, true, PeerContext>;

	static ServerOptions makeOptions(int port, std::string host, std::string keyFile, std::string certFile) {
		ServerOptions options;
		options.port = port;
		options.host = host;
		options.keyFile = keyFile;
		options.certFile = certFile;
		return options;
	}

	template <bool SSL>
	void runApp(uWS::TemplatedApp<SSL>& app) {
		app.template ws<PeerContext>("/*", {
			/* Settings */
			.compression = uWS::SHARED_COMPRESSOR,
			.maxPayloadLength = 64 * 1024,
			.idleTimeout = 240,
			.maxBackpressure = 1 * 1024 * 1024,
			// A client that doesn't read 1 MiB of signaling messages is dead or abusive
			.closeOnBackpressureLimit = true,
			.sendPingsAutomatically = true,
			/* Handlers */
			.upgrade = [this](uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req, struct us_socket_context_t* context) {
				this->onUpgrade<SSL>(res, req, context);
			},
			.open = [this](WebSocket<SSL>* ws) {
				this->onOpen<SSL>(ws);
			},
			.message = [this](WebSocket<SSL>* ws, std::string_view message, uWS::OpCode opCode) {
				this->onMessage<SSL>(ws, message, opCode);
			},
			.close = [this](WebSocket<SSL>* ws, int code, std::string_view message) {
				this->onClose<SSL>(ws, code, message);
			}
		});

		app.get("/stats.json", [this](auto* res, auto* req) {
			size_t peersCount = 0;
			const Tracker::SwarmMap& swarms = this->tracker->getSwarms();
			for (const auto& entry : swarms) {
				peersCount += entry.second->getPeersCount();
			}

			// Server stats
			std::vector<owt::ServerStats> currentStats = {
				{
					this->getAddress(),
					(int) this->websocketCount
				}
			};

			owt::ProcessMemory memoryUsage = {this->getResidentMemory(), 0, 0, 0};

			json statsJSON = {
				{"torrentsCount", swarms.size()},
				{"peersCount", peersCount},
				{"servers", currentStats},
				{"memory", memoryUsage}
			};

			res->writeHeader("Content-Type", "application/json");
			res->end(statsJSON.dump());
		});

		app.any("/*", [](auto* res, auto* request) {
			/* Any other route */
			std::string status = "404 Not Found";
			res->writeStatus(status);
			res->end(status);
		});

		// Exclusive port: with SO_REUSEPORT a second instance would silently split the swarms
		bool listening = false;
		app.listen(this->options.host, this->options.port, LIBUS_LISTEN_EXCLUSIVE_PORT, [&listening](auto* token) {
			listening = token != nullptr;
		});
		if (!listening) {
			throw std::runtime_error("Failed to listen on " + this->getAddress());
		}

		// Stale peer expiry runs on the event loop that owns the tracker state
		struct us_timer_t* cleanupTimer = nullptr;
		if (unsigned int period = this->tracker->getCleanupPeriodMs(); period > 0) {
			cleanupTimer = us_create_timer((struct us_loop_t*) uWS::Loop::get(), 1, sizeof(Tracker*));
			*(Tracker**) us_timer_ext(cleanupTimer) = this->tracker.get();
			us_timer_set(cleanupTimer, [](struct us_timer_t* timer) {
				(*(Tracker**) us_timer_ext(timer))->cleanupStalePeers();
			}, (int) period, (int) period);
		}

		COUT("Listening on " << (SSL ? "wss://" : "ws://") << this->getAddress());
		app.run();

		if (cleanupTimer != nullptr) {
			us_timer_close(cleanupTimer);
		}
	}

	template <bool SSL>
	static void sendFrame(void* socket, std::string_view message) {
		static_cast<WebSocket<SSL>*>(socket)->send(message, uWS::OpCode::TEXT);
	}

	template <bool SSL>
	void onUpgrade(uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req, struct us_socket_context_t* context) {
		PeerContext peer;
		peer.address = this->getClientAddress(res, req);

		size_t maxConnections = this->options.maxConnectionsPerIp;
		if (maxConnections > 0 && !peer.address.empty()) {
			auto it = this->connectionsPerAddress.find(peer.address);
			if (it != this->connectionsPerAddress.end() && it->second >= maxConnections) {
				DOUT("Too many connections from " << peer.address);
				res->writeStatus("429 Too Many Requests");
				res->end("Too many connections");
				return;
			}
		}

		res->template upgrade<PeerContext>(
			std::move(peer),
			req->getHeader("sec-websocket-key"),
			req->getHeader("sec-websocket-protocol"),
			req->getHeader("sec-websocket-extensions"),
			context
		);
	}

	template <bool SSL>
	void onOpen(WebSocket<SSL>* ws) {
		DOUT("CONNECTED");

		this->websocketCount++;

		// Initialize peer context
		PeerContext* peer = ws->getUserData();
		peer->socket = ws;
		peer->sendFn = &WebtorrentTracker::sendFrame<SSL>;
		peer->rateLimiter = Utils::TokenBucket(this->options.messageRate, this->options.messageBurst);

		if (!peer->address.empty()) {
			this->connectionsPerAddress[peer->address]++;
		}
	}

	template <bool SSL>
	void onMessage(WebSocket<SSL>* ws, std::string_view message, uWS::OpCode opCode) {
		PeerContext* peer = ws->getUserData();

		// Checked before parsing, which is the expensive part
		if (!peer->rateLimiter.consume()) {
			DOUT("Rate limit exceeded by " << peer->address);
			ws->end(1008, "Rate limit exceeded");
			return;
		}

		DOUT("Received: " << message);

		// Check JSON input
		json data = json::parse(message, nullptr, false);

		if (data.is_discarded()) {
			DOUT("Invalid JSON");
			ws->close();
			return;
		}

		try {
			this->tracker->processMessage(data, peer);
		} catch (const TrackerException& e) {
			// Protocol errors are reported to the client, like bittorrent-tracker does
			DOUT("Error: " << e.what());
			peer->sendMessage(this->failureMessage(data, e.what()));
		} catch (const std::exception& e) {
			EOUT("Error: " << e.what());
			ws->close();
		}
	}

	template <bool SSL>
	void onClose(WebSocket<SSL>* ws, int code, std::string_view message) {
		DOUT("DISCONNECTED");

		this->websocketCount--;

		PeerContext* peer = ws->getUserData();
		this->tracker->disconnectPeer(peer);
		peer->socket = nullptr;

		if (auto it = this->connectionsPerAddress.find(peer->address); it != this->connectionsPerAddress.end()) {
			if (--it->second == 0) {
				this->connectionsPerAddress.erase(it);
			}
		}
	}

	template <bool SSL>
	std::string getClientAddress(uWS::HttpResponse<SSL>* res, uWS::HttpRequest* req) {
		if (this->options.trustProxy) {
			// The proxy appends the address it received the request from
			std::string_view forwarded = req->getHeader("x-forwarded-for");
			size_t comma = forwarded.rfind(',');
			std::string_view last = comma == std::string_view::npos ? forwarded : forwarded.substr(comma + 1);
			while (!last.empty() && last.front() == ' ') last.remove_prefix(1);
			while (!last.empty() && last.back() == ' ') last.remove_suffix(1);

			std::string key = Utils::addressKeyFromText(last);
			if (!key.empty()) {
				return key;
			}
		}
		return Utils::addressKey(res->getRemoteAddress());
	}

	static json failureMessage(const json& data, const std::string& reason) {
		bool isScrape = data.is_object() && data.contains("action") && data["action"] == "scrape";
		json failure = {
			{"action", isScrape ? "scrape" : "announce"},
			{"failure reason", reason}
		};

		// Clients match responses to torrents by info_hash
		if (data.is_object() && data.contains("info_hash") && data["info_hash"].is_string()) {
			failure["info_hash"] = data["info_hash"];
		}
		return failure;
	}

	std::string getAddress() const {
		return (this->options.host.empty() ? "0.0.0.0" : this->options.host) + ":" + std::to_string(this->options.port);
	}

	// Current resident set size in bytes
	long int getResidentMemory() const {
		long int pages = 0, residentPages = 0;
		std::ifstream statm("/proc/self/statm");
		if (statm >> pages >> residentPages) {
			return residentPages * sysconf(_SC_PAGESIZE);
		}

		// Fallback to peak usage (reported in kilobytes)
		struct rusage usage;
		getrusage(RUSAGE_SELF, &usage);
		return usage.ru_maxrss * 1024;
	}

	bool fileExists(const std::string& filename) {
		struct stat buffer;
		return (stat (filename.c_str(), &buffer) == 0);
	}

};

};

#endif // OWT_WEBTORRENT_TRACKER_H
