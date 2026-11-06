#ifndef OWT_WEBTORRENT_TRACKER_H
#define OWT_WEBTORRENT_TRACKER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <json.hpp>
#include <App.h>
#include <sys/stat.h>

#include "Utils.h"
#include "Swarm.h"
#include "Tracker.h"
#include "FastTracker.h"
#include "DataObject.h"
#include "Metrics.h"
#include "PeerContext.h"
#include "StatusPage.h"
#include "TrackerException.h"
#include "FailureSamples.h"

using namespace nlohmann;

namespace owt {

struct ServerOptions {
	int port = 8000;
	// Empty host listens on all interfaces (IPv4 and IPv6)
	std::string host;

	// Without key and certificate the tracker serves plain (non-SSL) websockets
	std::string keyFile;
	std::string certFile;

	// Worker threads handling connections (0 = one per CPU core)
	unsigned int threads = 0;

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

// Threading model
// ---------------
// The acceptor (the thread calling run()) owns the listening socket and hands
// each accepted connection, round robin, to one of N worker threads, each with
// its own uWS event loop. Workers do the expensive part in parallel: HTTP,
// websocket framing, JSON parsing and serialization, socket I/O.
//
// The tracker state (swarms, peer IDs) is shared and guarded by one mutex,
// held only while the tracker updates its maps. Messages the tracker produces
// meanwhile are queued (PeerContext::outbox) and delivered after the lock is
// released: directly when the target socket belongs to the current thread,
// otherwise batched and deferred to the owning thread's loop. Delivery looks
// the connection up by id in that thread's registry, so a connection that
// closed in the meantime is skipped instead of touched after free.
class WebtorrentTracker {

private:
	ServerOptions options;

	std::unique_ptr<Tracker> tracker;
	std::mutex trackerMutex;
	// Guarded by trackerMutex; only maintained when maxConnectionsPerIp > 0
	std::unordered_map<std::string, size_t> connectionsPerAddress;

	std::atomic<uint64_t> nextConnectionId{1};
	Metrics metrics;
	FailureSamples failureSamples;

	static constexpr unsigned int CLEANUP_SLICES = 64;

	// Per worker thread
	static inline thread_local std::unordered_map<uint64_t, void*> localSockets;
	static inline thread_local WorkerCounters* localCounters = nullptr;
	// Distinct "Origin | User-Agent" strings seen by this thread (see internClient)
	static inline thread_local std::unordered_set<std::string> localClients;

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
		if (this->options.keyFile.empty() && this->options.certFile.empty()) {
			this->runServer<false>();
			return;
		}

		bool exists = this->fileExists(this->options.keyFile) && this->fileExists(this->options.certFile);
		if (!exists) {
			throw std::runtime_error("Cert and key must exist");
		}
		this->runServer<true>();
	}

private:

	template <bool SSL>
	using WebSocket = uWS::WebSocket<SSL, true, PeerContext>;

	template <bool SSL>
	using App = uWS::TemplatedApp<SSL>;

	static ServerOptions makeOptions(int port, std::string host, std::string keyFile, std::string certFile) {
		ServerOptions options;
		options.port = port;
		options.host = host;
		options.keyFile = keyFile;
		options.certFile = certFile;
		return options;
	}

	unsigned int threadCount() const {
		if (this->options.threads > 0) {
			return this->options.threads;
		}
		return std::max(1u, std::thread::hardware_concurrency());
	}

	template <bool SSL>
	std::unique_ptr<App<SSL>> makeApp() {
		if constexpr (SSL) {
			auto app = std::make_unique<App<SSL>>(uWS::SocketContextOptions{
				.key_file_name = this->options.keyFile.c_str(),
				.cert_file_name = this->options.certFile.c_str()
			});
			if (app->constructorFailed()) {
				throw std::runtime_error("Failed to load SSL key or certificate");
			}
			return app;
		} else {
			return std::make_unique<App<SSL>>();
		}
	}

	template <bool SSL>
	void runServer() {
		const unsigned int threads = this->threadCount();
		this->metrics.init(threads);
		this->metrics.acceptorTid = Metrics::currentTid();
		this->metrics.config = {
			{"threads", threads},
			{"announceInterval", this->options.tracker.announceInterval},
			{"announceJitterPercent", this->options.tracker.announceIntervalJitter * 100},
			{"rateLimit", this->options.messageRate},
			{"rateBurst", this->options.messageBurst},
			{"maxTorrentsPerConnection", this->options.tracker.maxSwarmsPerPeer},
			{"maxConnectionsPerIp", this->options.maxConnectionsPerIp},
			{"allowFullScrape", this->options.tracker.allowFullScrape},
			{"trustProxy", this->options.trustProxy},
			{"ssl", SSL}
		};

		// Listen before starting workers: a failure here exits cleanly.
		// Exclusive port: with SO_REUSEPORT a second instance would silently split the swarms.
		std::unique_ptr<App<SSL>> acceptor = this->makeApp<SSL>();
		bool listening = false;
		acceptor->listen(this->options.host, this->options.port, LIBUS_LISTEN_EXCLUSIVE_PORT, [&listening](auto* token) {
			listening = token != nullptr;
		});
		if (!listening) {
			throw std::runtime_error("Failed to listen on " + this->getAddress());
		}

		// Start workers and wait until each has created its app
		std::vector<App<SSL>*> apps(threads, nullptr);
		std::mutex readyMutex;
		std::condition_variable readyCondition;
		unsigned int ready = 0;
		std::string workerError;

		std::vector<std::thread> workers;
		for (unsigned int i = 0; i < threads; i++) {
			workers.emplace_back([&, i]() {
				localCounters = &this->metrics.workers[i];
				localCounters->tid = Metrics::currentTid();

				std::unique_ptr<App<SSL>> app;
				std::string error;
				try {
					app = this->makeApp<SSL>();
					this->configureWorker<SSL>(*app);
				} catch (const std::exception& e) {
					error = e.what();
					app.reset();
				}

				// Child apps have no listening socket: an unarmed, non-fallthrough
				// timer keeps the loop running while it waits for adopted sockets.
				struct us_timer_t* keepAlive = us_create_timer((struct us_loop_t*) uWS::Loop::get(), 0, 0);

				{
					std::lock_guard<std::mutex> lock(readyMutex);
					apps[i] = app.get();
					if (!error.empty()) workerError = error;
					ready++;
				}
				readyCondition.notify_all();

				if (app) {
					app->run();
				}
				us_timer_close(keepAlive);
			});
		}

		{
			std::unique_lock<std::mutex> lock(readyMutex);
			readyCondition.wait(lock, [&]() { return ready == threads; });
		}
		if (!workerError.empty()) {
			EOUT("Error: worker failed to start: " << workerError);
			std::_Exit(1);
		}

		for (App<SSL>* app : apps) {
			acceptor->addChildApp(app);
		}

		// Stale peer expiry and metrics sampling run on the acceptor's loop
		struct us_loop_t* loop = (struct us_loop_t*) uWS::Loop::get();
		struct us_timer_t* cleanupTimer = nullptr;
		// One full sweep per period, in CLEANUP_SLICES short lock holds
		if (unsigned int period = this->tracker->getCleanupPeriodMs(); period > 0) {
			int step = std::max(1, (int) (period / CLEANUP_SLICES));
			cleanupTimer = us_create_timer(loop, 1, sizeof(WebtorrentTracker*));
			*(WebtorrentTracker**) us_timer_ext(cleanupTimer) = this;
			us_timer_set(cleanupTimer, [](struct us_timer_t* timer) {
				(*(WebtorrentTracker**) us_timer_ext(timer))->runLocked<SSL>([](Tracker& tracker) {
					tracker.cleanupStalePeersStep(CLEANUP_SLICES);
				});
			}, step, step);
		}

		struct us_timer_t* metricsTimer = us_create_timer(loop, 1, sizeof(WebtorrentTracker*));
		*(WebtorrentTracker**) us_timer_ext(metricsTimer) = this;
		us_timer_set(metricsTimer, [](struct us_timer_t* timer) {
			(*(WebtorrentTracker**) us_timer_ext(timer))->sampleMetrics();
		}, Metrics::SAMPLE_PERIOD_MS, Metrics::SAMPLE_PERIOD_MS);
		this->sampleMetrics();

		COUT("Listening on " << (SSL ? "wss://" : "ws://") << this->getAddress() << " with " << threads << " worker thread" << (threads == 1 ? "" : "s"));
		acceptor->run();

		us_timer_close(metricsTimer);
		if (cleanupTimer != nullptr) {
			us_timer_close(cleanupTimer);
		}
		for (auto& worker : workers) {
			worker.detach();
		}
	}

	template <bool SSL>
	void configureWorker(App<SSL>& app) {
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

		// Original stats format, computed live (like the single-threaded version)
		app.get("/stats.json", [this](auto* res, auto* req) {
			size_t torrents = 0, peersCount = 0;
			{
				std::lock_guard<std::mutex> lock(this->trackerMutex);
				torrents = this->tracker->getSwarms().size();
				peersCount = this->tracker->getMembershipCount();
			}
			int64_t websockets = 0;
			for (size_t i = 0; i < this->metrics.threadCount; i++) {
				websockets += this->metrics.workers[i].websockets.load(std::memory_order_relaxed);
			}
			json statsJSON = {
				{"torrentsCount", torrents},
				{"peersCount", peersCount},
				{"servers", std::vector<ServerStats>{{this->getAddress(), (int) websockets}}},
				{"memory", ProcessMemory{(long int) Metrics::residentMemory(), 0, 0, 0}}
			};
			res->writeHeader("Content-Type", "application/json");
			res->writeHeader("Cache-Control", "no-store");
			res->end(statsJSON.dump());
		});

		// Monitoring (status page): cached, refreshed every sample, no lock taken
		app.get("/stats/metrics.json", [this](auto* res, auto* req) {
			std::shared_ptr<const std::string> current = this->metrics.current();
			res->writeHeader("Content-Type", "application/json");
			res->writeHeader("Cache-Control", "no-store");
			res->writeHeader("X-Robots-Tag", "noindex, nofollow");
			res->writeHeader("Access-Control-Allow-Origin", "*");
			res->end(*current);
		});

		app.get("/stats/history.json", [this](auto* res, auto* req) {
			std::string body = this->metrics.history(req->getQuery("range"));
			res->writeHeader("Content-Type", "application/json");
			res->writeHeader("Cache-Control", "no-store");
			res->writeHeader("X-Robots-Tag", "noindex, nofollow");
			res->writeHeader("Access-Control-Allow-Origin", "*");
			res->end(body);
		});

		// Recent rejected requests per reason, summarized (no SDP). Loopback
		// only: clients reach the tracker through Cloudflare, never from
		// 127.0.0.1, so this isn't reachable from outside.
		//   curl -s http://127.0.0.1/debug/failures.json
		app.get("/debug/failures.json", [this](auto* res, auto* req) {
			if (!Utils::isLoopback(res->getRemoteAddress())) {
				res->writeStatus("404 Not Found");
				res->end("404 Not Found");
				return;
			}
			std::string body = this->failureSamples.dump();
			res->writeHeader("Content-Type", "application/json");
			res->writeHeader("Cache-Control", "no-store");
			res->writeHeader("X-Robots-Tag", "noindex, nofollow");
			res->end(body);
		});

		app.get("/status", [](auto* res, auto* req) {
			res->writeHeader("Content-Type", "text/html; charset=utf-8");
			res->writeHeader("Cache-Control", "no-cache");
			res->writeHeader("X-Robots-Tag", "noindex, nofollow");
			res->end(STATUS_PAGE_HTML);
		});

		app.any("/*", [](auto* res, auto* request) {
			/* Any other route */
			std::string status = "404 Not Found";
			res->writeStatus(status);
			res->end(status);
		});
	}

	// Runs fn on the tracker under the lock, then delivers the messages it produced
	template <bool SSL, typename Fn>
	void runLocked(Fn&& fn) {
		std::vector<PeerContext::Outgoing> out;
		PeerContext::outbox = &out;
		{
			auto start = std::chrono::steady_clock::now();
			std::lock_guard<std::mutex> lock(this->trackerMutex);
			auto acquired = std::chrono::steady_clock::now();
			fn(*this->tracker);
			if (localCounters != nullptr) {
				auto done = std::chrono::steady_clock::now();
				WorkerCounters::add(localCounters->lockWaitNs, std::chrono::duration_cast<std::chrono::nanoseconds>(acquired - start).count());
				WorkerCounters::add(localCounters->lockHeldNs, std::chrono::duration_cast<std::chrono::nanoseconds>(done - acquired).count());
			}
		}
		PeerContext::outbox = nullptr;
		this->deliver<SSL>(std::move(out));
	}

	template <bool SSL>
	void deliver(std::vector<PeerContext::Outgoing>&& items) {
		if (items.empty()) {
			return;
		}

		void* self = uWS::Loop::get();
		// Batches for other threads, keyed by owning loop (few threads: linear search)
		std::vector<std::pair<void*, std::vector<std::pair<uint64_t, std::string>>>> remote;

		for (auto& item : items) {
			std::string frame = item.frame();
			if (localCounters != nullptr) {
				WorkerCounters::add(localCounters->messagesOut);
				WorkerCounters::add(localCounters->bytesOut, frame.size());
			}
			if (item.owner == self) {
				sendLocal<SSL>(item.id, frame);
				continue;
			}
			if (localCounters != nullptr) {
				WorkerCounters::add(localCounters->crossThreadOut);
			}
			auto it = std::find_if(remote.begin(), remote.end(), [&](const auto& batch) { return batch.first == item.owner; });
			if (it == remote.end()) {
				remote.emplace_back(item.owner, std::vector<std::pair<uint64_t, std::string>>{});
				it = remote.end() - 1;
			}
			it->second.emplace_back(item.id, std::move(frame));
		}

		for (auto& [owner, batch] : remote) {
			static_cast<uWS::Loop*>(owner)->defer([batch = std::move(batch)]() {
				for (const auto& [id, frame] : batch) {
					sendLocal<SSL>(id, frame);
				}
			});
		}
	}

	// Must run on the thread that owns the connection
	template <bool SSL>
	static void sendLocal(uint64_t id, std::string_view frame) {
		auto it = localSockets.find(id);
		if (it != localSockets.end()) {
			static_cast<WebSocket<SSL>*>(it->second)->send(frame, uWS::OpCode::TEXT);
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
			std::lock_guard<std::mutex> lock(this->trackerMutex);
			auto it = this->connectionsPerAddress.find(peer.address);
			if (it != this->connectionsPerAddress.end() && it->second >= maxConnections) {
				DOUT("Too many connections from " << peer.address);
				res->writeStatus("429 Too Many Requests");
				res->end("Too many connections");
				return;
			}
		}

		peer.client = internClient(req->getHeader("origin"), req->getHeader("user-agent"));

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

		PeerContext* peer = ws->getUserData();
		peer->socket = ws;
		peer->sendFn = &WebtorrentTracker::sendFrame<SSL>;
		peer->id = this->nextConnectionId.fetch_add(1, std::memory_order_relaxed);
		peer->owner = uWS::Loop::get();
		peer->rateLimiter = Utils::TokenBucket(this->options.messageRate, this->options.messageBurst);
		peer->openedAt = PeerContext::Clock::now();
		localSockets[peer->id] = ws;

		localCounters->websockets.fetch_add(1, std::memory_order_relaxed);
		WorkerCounters::add(localCounters->opened);

		if (this->options.maxConnectionsPerIp > 0 && !peer->address.empty()) {
			std::lock_guard<std::mutex> lock(this->trackerMutex);
			this->connectionsPerAddress[peer->address]++;
		}
	}

	template <bool SSL>
	void onMessage(WebSocket<SSL>* ws, std::string_view message, uWS::OpCode opCode) {
		PeerContext* peer = ws->getUserData();
		WorkerCounters::add(localCounters->messagesIn);
		WorkerCounters::add(localCounters->bytesIn, message.size());

		// Checked before parsing, which is the expensive part
		if (!peer->rateLimiter.consume()) {
			DOUT("Rate limit exceeded by " << peer->address);
			WorkerCounters::add(localCounters->rateLimited);
			ws->end(1008, "Rate limit exceeded");
			return;
		}

		DOUT("Received: " << message);

		// Parsed outside the tracker lock
		json data = json::parse(message, nullptr, false);

		if (data.is_discarded()) {
			DOUT("Invalid JSON");
			WorkerCounters::add(localCounters->invalid);
			ws->close();
			return;
		}

		if (data.is_object()) {
			auto action = data.find("action");
			if (action != data.end() && action->is_string()) {
				const std::string& name = action->get_ref<const std::string&>();
				if (name == "scrape") {
					WorkerCounters::add(localCounters->scrapes);
				} else if (data.contains("answer")) {
					WorkerCounters::add(localCounters->answers);
				} else {
					WorkerCounters::add(localCounters->announces);
				}
			}
		}

		// `data` must outlive delivery: queued offers/answers point into it
		// (PeerContext::Outgoing). runLocked delivers before returning.
		const uint64_t id = peer->id;
		// Read now: delivery below can close the socket and free `peer`
		const std::string* client = peer->client;
		const PeerContext::Clock::time_point openedAt = peer->openedAt;
		bool closeConnection = false;
		bool sampled = false;
		FailureReason failure{};
		json diagnosis;
		this->runLocked<SSL>([&](Tracker& tracker) {
			try {
				tracker.processMessage(data, peer);
			} catch (const TrackerException& e) {
				// Protocol errors are reported to the client, like bittorrent-tracker does
				DOUT("Error: " << e.what());
				WorkerCounters::add(localCounters->failures);
				WorkerCounters::add(localCounters->failureReasons[static_cast<size_t>(e.reason())]);
				peer->sendMessage(this->failureMessage(data, e.what()));
				if (this->failureSamples.shouldSample(e.reason())) {
					sampled = true;
					failure = e.reason();
					diagnosis = tracker.diagnose(data, peer);
				}
			} catch (const std::exception& e) {
				EOUT("Error: " << e.what());
				closeConnection = true;
			}
		});

		// Summarized outside the lock
		if (sampled) {
			using namespace std::chrono;
			this->failureSamples.record(failure, {
				{"time", Metrics::nowMs()},
				{"client", client != nullptr ? *client : std::string()},
				{"connectionAgeSeconds", duration_cast<seconds>(PeerContext::Clock::now() - openedAt).count()},
				{"tracker", std::move(diagnosis)},
				{"request", FailureSamples::summarize(data)}
			});
		}

		// Delivery can close this very socket (backpressure limit): check it still exists
		if (closeConnection && localSockets.count(id) > 0) {
			ws->close();
		}
	}

	template <bool SSL>
	void onClose(WebSocket<SSL>* ws, int code, std::string_view message) {
		DOUT("DISCONNECTED");

		PeerContext* peer = ws->getUserData();
		localSockets.erase(peer->id);
		localCounters->websockets.fetch_sub(1, std::memory_order_relaxed);
		WorkerCounters::add(localCounters->closed);

		this->runLocked<SSL>([&](Tracker& tracker) {
			tracker.disconnectPeer(peer);
			if (auto it = this->connectionsPerAddress.find(peer->address); it != this->connectionsPerAddress.end()) {
				if (--it->second == 0) {
					this->connectionsPerAddress.erase(it);
				}
			}
		});
		peer->socket = nullptr;
	}

	// Runs on the acceptor thread every Metrics::SAMPLE_PERIOD_MS
	void sampleMetrics() {
		MetricsSample sample;
		{
			std::lock_guard<std::mutex> lock(this->trackerMutex);
			sample.torrents = this->tracker->getSwarms().size();
			sample.peers = this->tracker->getPeerIdCount();
			sample.memberships = this->tracker->getMembershipCount();
			sample.seeders = this->tracker->getSeederCount();
			sample.leechers = sample.memberships - sample.seeders;
		}
		this->metrics.collect(sample);
		this->metrics.record(std::move(sample));
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

	// Interned "Origin | User-Agent" of a connection. Each value is stored once
	// per thread and shared by its connections; past MAX_CLIENTS distinct
	// values (forged headers), new ones aren't recorded.
	static constexpr size_t MAX_CLIENTS = 4096;
	static constexpr size_t MAX_HEADER = 160;

	static const std::string* internClient(std::string_view origin, std::string_view userAgent) {
		static const std::string tooMany = "(not recorded: too many distinct clients)";
		std::string key = std::string(origin.substr(0, MAX_HEADER)) + " | " + std::string(userAgent.substr(0, MAX_HEADER));
		auto it = localClients.find(key);
		if (it != localClients.end()) {
			return &*it;
		}
		if (localClients.size() >= MAX_CLIENTS) {
			return &tooMany;
		}
		return &*localClients.insert(std::move(key)).first;
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

	bool fileExists(const std::string& filename) {
		struct stat buffer;
		return (stat (filename.c_str(), &buffer) == 0);
	}

};

};

#endif // OWT_WEBTORRENT_TRACKER_H
