#ifndef OWT_METRICS_H
#define OWT_METRICS_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <json.hpp>
#include "TrackerException.h"
#include "Tracker.h"
#include <sys/types.h>
#include <unistd.h>

using namespace nlohmann;

namespace owt {

// Counters written by one worker thread (relaxed atomics) and read by the
// sampler. Cache-line aligned so threads don't contend on each other's lines.
struct alignas(64) WorkerCounters {
	std::atomic<int64_t> websockets{0};
	std::atomic<uint64_t> opened{0};
	std::atomic<uint64_t> closed{0};
	std::atomic<uint64_t> messagesIn{0};
	std::atomic<uint64_t> bytesIn{0};
	std::atomic<uint64_t> messagesOut{0};
	std::atomic<uint64_t> bytesOut{0};
	// Messages delivered to a socket owned by another thread
	std::atomic<uint64_t> crossThreadOut{0};
	std::atomic<uint64_t> announces{0};
	std::atomic<uint64_t> answers{0};
	std::atomic<uint64_t> scrapes{0};
	// Protocol errors reported to clients ("failure reason"), in total and per reason
	std::atomic<uint64_t> failures{0};
	std::atomic<uint64_t> failureReasons[FAILURE_REASON_COUNT]{};
	// Connections closed for exceeding --rate-limit / sending invalid JSON
	std::atomic<uint64_t> rateLimited{0};
	std::atomic<uint64_t> invalid{0};
	// Time spent waiting for / holding the shared tracker lock
	std::atomic<uint64_t> lockWaitNs{0};
	std::atomic<uint64_t> lockHeldNs{0};
	// Kernel thread id, for per-thread CPU from /proc
	std::atomic<pid_t> tid{0};

	static void add(std::atomic<uint64_t>& counter, uint64_t value = 1) {
		counter.fetch_add(value, std::memory_order_relaxed);
	}
};

// One point in time. Counters are cumulative; rates are computed between two
// samples, so any two points (e.g. when downsampling) give a correct rate.
struct MetricsSample {
	int64_t timeMs = 0;

	// Gauges
	uint64_t torrents = 0;
	uint64_t peers = 0;        // distinct peer IDs
	uint64_t memberships = 0;  // peer ID + torrent pairs
	uint64_t seeders = 0;
	uint64_t leechers = 0;
	int64_t websockets = 0;
	uint64_t rssBytes = 0;

	// Cumulative counters (all workers)
	uint64_t opened = 0, closed = 0;
	uint64_t messagesIn = 0, bytesIn = 0, messagesOut = 0, bytesOut = 0, crossThreadOut = 0;
	uint64_t announces = 0, answers = 0, scrapes = 0, failures = 0, rateLimited = 0, invalid = 0;
	uint64_t lockWaitNs = 0, lockHeldNs = 0;
	std::array<uint64_t, FAILURE_REASON_COUNT> failureReasons{};
	// Requests accepted outside the WebTorrent protocol (from the tracker)
	CompatCounters compat;

	// CPU time in clock ticks: whole process, and per thread (workers, then acceptor)
	uint64_t processTicks = 0;
	std::vector<uint64_t> threadTicks;
	std::vector<int64_t> threadWebsockets;
};

class Metrics {

public:
	static constexpr int SAMPLE_PERIOD_MS = 5000;
	// 5 s samples for the last hour, 1 min samples for the last 7 days
	static constexpr size_t FINE_CAPACITY = 720;
	static constexpr size_t COARSE_EVERY = 12;
	static constexpr size_t COARSE_CAPACITY = 7 * 24 * 60;
	// Window for the per-reason failure rates (individual reasons are too rare
	// for a 5 s rate to mean much)
	static constexpr int64_t FAILURE_WINDOW_MS = 15 * 60 * 1000;

	size_t threadCount = 0;
	std::unique_ptr<WorkerCounters[]> workers;
	std::atomic<pid_t> acceptorTid{0};
	int64_t startTimeMs = nowMs();
	json config = json::object();

	void init(size_t threads) {
		this->threadCount = threads;
		this->workers = std::make_unique<WorkerCounters[]>(threads);
	}

	static int64_t nowMs() {
		using namespace std::chrono;
		return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
	}

	static uint64_t residentMemory() {
		long pages = 0, residentPages = 0;
		std::ifstream statm("/proc/self/statm");
		if (statm >> pages >> residentPages) {
			return (uint64_t) residentPages * (uint64_t) sysconf(_SC_PAGESIZE);
		}
		return 0;
	}

	static pid_t currentTid() {
		return (pid_t) ::gettid();
	}

	// Fills the counter and CPU fields of a sample (gauges come from the tracker)
	void collect(MetricsSample& sample) const {
		sample.timeMs = nowMs();
		sample.threadTicks.clear();
		sample.threadWebsockets.clear();
		for (size_t i = 0; i < this->threadCount; i++) {
			const WorkerCounters& w = this->workers[i];
			auto r = [](const std::atomic<uint64_t>& c) { return c.load(std::memory_order_relaxed); };
			int64_t ws = w.websockets.load(std::memory_order_relaxed);
			sample.websockets += ws;
			sample.threadWebsockets.push_back(ws);
			sample.opened += r(w.opened);
			sample.closed += r(w.closed);
			sample.messagesIn += r(w.messagesIn);
			sample.bytesIn += r(w.bytesIn);
			sample.messagesOut += r(w.messagesOut);
			sample.bytesOut += r(w.bytesOut);
			sample.crossThreadOut += r(w.crossThreadOut);
			sample.announces += r(w.announces);
			sample.answers += r(w.answers);
			sample.scrapes += r(w.scrapes);
			sample.failures += r(w.failures);
			for (size_t f = 0; f < FAILURE_REASON_COUNT; f++) {
				sample.failureReasons[f] += r(w.failureReasons[f]);
			}
			sample.rateLimited += r(w.rateLimited);
			sample.invalid += r(w.invalid);
			sample.lockWaitNs += r(w.lockWaitNs);
			sample.lockHeldNs += r(w.lockHeldNs);
			sample.threadTicks.push_back(threadTicks(w.tid.load()));
		}
		sample.threadTicks.push_back(threadTicks(this->acceptorTid.load()));
		sample.processTicks = readTicks("/proc/self/stat");
		sample.rssBytes = residentMemory();
	}

	void record(MetricsSample sample) {
		std::lock_guard<std::mutex> lock(this->historyMutex);
		this->fine.push_back(sample);
		if (this->fine.size() > FINE_CAPACITY) {
			this->fine.pop_front();
		}
		if (++this->samplesSinceCoarse >= COARSE_EVERY || this->coarse.empty()) {
			this->samplesSinceCoarse = 0;
			this->coarse.push_back(std::move(sample));
			if (this->coarse.size() > COARSE_CAPACITY) {
				this->coarse.pop_front();
			}
		}
		this->currentJson = std::make_shared<const std::string>(this->buildCurrent().dump());
	}

	// Latest metrics as JSON (cached, rebuilt every sample)
	std::shared_ptr<const std::string> current() const {
		std::lock_guard<std::mutex> lock(this->historyMutex);
		return this->currentJson;
	}

	// Columnar history for charts. range: 15m, 1h, 6h, 24h, 7d
	std::string history(std::string_view range) const {
		int64_t spanMs = 3600 * 1000;
		if (range == "15m") spanMs = 15 * 60 * 1000;
		else if (range == "6h") spanMs = 6 * 3600 * 1000;
		else if (range == "24h") spanMs = 24 * 3600 * 1000;
		else if (range == "7d") spanMs = 7 * 24 * 3600 * 1000LL;

		std::lock_guard<std::mutex> lock(this->historyMutex);
		const std::deque<MetricsSample>& source = spanMs <= 3600 * 1000 ? this->fine : this->coarse;
		if (source.empty()) {
			return json{{"range", range}, {"t", json::array()}}.dump();
		}

		int64_t from = source.back().timeMs - spanMs;
		std::vector<const MetricsSample*> points;
		for (const auto& s : source) {
			if (s.timeMs >= from) points.push_back(&s);
		}
		// At most ~720 points per chart: downsample (rates stay exact, see MetricsSample)
		size_t stride = std::max<size_t>(1, (points.size() + 719) / 720);
		std::vector<const MetricsSample*> picked;
		for (size_t i = points.size() - 1;; i -= std::min(i, stride)) {
			picked.insert(picked.begin(), points[i]);
			if (i == 0) break;
		}

		json out = {{"range", range}, {"threads", this->threadCount}};
		const char* gauges[] = {"t", "websockets", "peers", "memberships", "torrents", "seeders", "leechers", "rssMB"};
		const char* rates[] = {"messagesIn", "messagesOut", "bytesIn", "bytesOut", "opened", "closed",
			"announces", "answers", "scrapes", "failures", "rateLimited", "invalid"};
		for (const char* key : gauges) out[key] = json::array();
		for (const char* key : rates) out[key] = json::array();
		out["cpu"] = json::array();
		out["threadCpu"] = json::array();
		out["crossThreadPct"] = json::array();
		out["lockWaitPct"] = json::array();
		out["lockHeldPct"] = json::array();

		for (size_t i = 1; i < picked.size(); i++) {
			const MetricsSample& a = *picked[i - 1];
			const MetricsSample& b = *picked[i];
			double seconds = std::max(0.001, (b.timeMs - a.timeMs) / 1000.0);
			out["t"].push_back(b.timeMs);
			out["websockets"].push_back(b.websockets);
			out["peers"].push_back(b.peers);
			out["memberships"].push_back(b.memberships);
			out["torrents"].push_back(b.torrents);
			out["seeders"].push_back(b.seeders);
			out["leechers"].push_back(b.leechers);
			out["rssMB"].push_back(round1(b.rssBytes / 1048576.0));
			auto rate = [&](uint64_t x, uint64_t y) { return round1((double) (y >= x ? y - x : 0) / seconds); };
			out["messagesIn"].push_back(rate(a.messagesIn, b.messagesIn));
			out["messagesOut"].push_back(rate(a.messagesOut, b.messagesOut));
			out["bytesIn"].push_back(rate(a.bytesIn, b.bytesIn));
			out["bytesOut"].push_back(rate(a.bytesOut, b.bytesOut));
			out["opened"].push_back(rate(a.opened, b.opened));
			out["closed"].push_back(rate(a.closed, b.closed));
			out["announces"].push_back(rate(a.announces, b.announces));
			out["answers"].push_back(rate(a.answers, b.answers));
			out["scrapes"].push_back(rate(a.scrapes, b.scrapes));
			out["failures"].push_back(rate(a.failures, b.failures));
			out["rateLimited"].push_back(rate(a.rateLimited, b.rateLimited));
			out["invalid"].push_back(rate(a.invalid, b.invalid));
			out["cpu"].push_back(cpuPercent(a.processTicks, b.processTicks, seconds));
			json perThread = json::array();
			for (size_t t = 0; t < b.threadTicks.size() && t < a.threadTicks.size(); t++) {
				perThread.push_back(cpuPercent(a.threadTicks[t], b.threadTicks[t], seconds));
			}
			out["threadCpu"].push_back(perThread);
			uint64_t sent = b.messagesOut - std::min(b.messagesOut, a.messagesOut);
			uint64_t cross = b.crossThreadOut - std::min(b.crossThreadOut, a.crossThreadOut);
			out["crossThreadPct"].push_back(sent ? round1(100.0 * cross / sent) : 0.0);
			// % of one core's time, summed over workers
			out["lockWaitPct"].push_back(round1((b.lockWaitNs - std::min(b.lockWaitNs, a.lockWaitNs)) / (seconds * 1e7)));
			out["lockHeldPct"].push_back(round1((b.lockHeldNs - std::min(b.lockHeldNs, a.lockHeldNs)) / (seconds * 1e7)));
		}
		return out.dump();
	}

private:
	mutable std::mutex historyMutex;
	std::deque<MetricsSample> fine;
	std::deque<MetricsSample> coarse;
	size_t samplesSinceCoarse = 0;
	std::shared_ptr<const std::string> currentJson = std::make_shared<const std::string>("{}");

	static double round1(double value) {
		return std::round(value * 10.0) / 10.0;
	}

	static double cpuPercent(uint64_t a, uint64_t b, double seconds) {
		static const double ticksPerSecond = (double) sysconf(_SC_CLK_TCK);
		return round1(100.0 * (double) (b >= a ? b - a : 0) / ticksPerSecond / seconds);
	}

	// utime + stime from a /proc/.../stat file (fields 14 and 15)
	static uint64_t readTicks(const std::string& path) {
		std::ifstream file(path);
		std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		size_t paren = content.rfind(')');
		if (paren == std::string::npos) {
			return 0;
		}
		std::istringstream fields(content.substr(paren + 2));
		std::string skip;
		// After "pid (comm) ": state is field 3, utime field 14, stime field 15
		for (int i = 3; i < 14; i++) fields >> skip;
		uint64_t utime = 0, stime = 0;
		fields >> utime >> stime;
		return utime + stime;
	}

	static uint64_t threadTicks(pid_t tid) {
		return tid > 0 ? readTicks("/proc/self/task/" + std::to_string(tid) + "/stat") : 0;
	}

	// The "current" document: latest gauges, rates over the last sample period
	json buildCurrent() const {
		const MetricsSample& b = this->fine.back();
		const MetricsSample& a = this->fine.size() > 1 ? this->fine[this->fine.size() - 2] : b;
		double seconds = std::max(0.001, (b.timeMs - a.timeMs) / 1000.0);
		auto rate = [&](uint64_t x, uint64_t y) { return round1((double) (y >= x ? y - x : 0) / seconds); };

		json threads = json::array();
		for (size_t t = 0; t < b.threadTicks.size(); t++) {
			bool acceptor = t == b.threadTicks.size() - 1;
			threads.push_back({
				{"name", acceptor ? std::string("acceptor") : "worker " + std::to_string(t)},
				{"cpu", t < a.threadTicks.size() ? cpuPercent(a.threadTicks[t], b.threadTicks[t], seconds) : 0.0},
				{"websockets", acceptor ? 0 : b.threadWebsockets[t]}
			});
		}

		uint64_t sent = b.messagesOut - std::min(b.messagesOut, a.messagesOut);
		uint64_t cross = b.crossThreadOut - std::min(b.crossThreadOut, a.crossThreadOut);

		// Oldest fine sample within the failure window (the first one if the
		// tracker hasn't been up that long)
		const MetricsSample* windowStart = &this->fine.front();
		for (auto it = this->fine.rbegin(); it != this->fine.rend(); ++it) {
			if (b.timeMs - it->timeMs > FAILURE_WINDOW_MS) break;
			windowStart = &*it;
		}
		double windowSeconds = std::max(0.001, (b.timeMs - windowStart->timeMs) / 1000.0);
		json failureReasons = json::array();
		for (size_t f = 0; f < FAILURE_REASON_COUNT; f++) {
			FailureReason reason = static_cast<FailureReason>(f);
			uint64_t inWindow = b.failureReasons[f] - std::min(b.failureReasons[f], windowStart->failureReasons[f]);
			failureReasons.push_back({
				{"reason", failureReasonKey(reason)},
				{"message", failureReasonMessage(reason)},
				{"rate", rate(a.failureReasons[f], b.failureReasons[f])},
				{"windowCount", inWindow},
				{"windowRate", round1(inWindow / windowSeconds)},
				{"total", b.failureReasons[f]}
			});
		}

		// Requests accepted outside the WebTorrent protocol, same window
		json nonStandard = json::array();
		auto addNonStandard = [&](const char* key, const char* description, uint64_t CompatCounters::*field) {
			uint64_t inWindow = b.compat.*field - std::min(b.compat.*field, windowStart->compat.*field);
			nonStandard.push_back({
				{"key", key},
				{"description", description},
				{"rate", rate(a.compat.*field, b.compat.*field)},
				{"windowCount", inWindow},
				{"windowRate", round1(inWindow / windowSeconds)},
				{"total", b.compat.*field}
			});
		};
		addNonStandard("non_standard_ids", "info_hash or peer_id that isn't a 20-byte binary string (hex, base64, other lengths)", &CompatCounters::nonStandardIds);
		addNonStandard("unknown_events", "Announce with an unknown event, handled as an update", &CompatCounters::unknownEvents);
		addNonStandard("answers_without_announce", "Answer from a peer that hadn't announced on its connection; added to the swarm", &CompatCounters::answersWithoutAnnounce);

		return {
			{"time", b.timeMs},
			{"startTime", this->startTimeMs},
			{"uptimeSeconds", (b.timeMs - this->startTimeMs) / 1000},
			{"samplePeriodSeconds", SAMPLE_PERIOD_MS / 1000},
			{"config", this->config},
			{"websockets", b.websockets},
			{"peers", b.peers},
			{"memberships", b.memberships},
			{"torrents", b.torrents},
			{"seeders", b.seeders},
			{"leechers", b.leechers},
			{"rssBytes", b.rssBytes},
			{"cpuPercent", cpuPercent(a.processTicks, b.processTicks, seconds)},
			{"threads", threads},
			{"rates", {
				{"messagesIn", rate(a.messagesIn, b.messagesIn)},
				{"messagesOut", rate(a.messagesOut, b.messagesOut)},
				{"bytesIn", rate(a.bytesIn, b.bytesIn)},
				{"bytesOut", rate(a.bytesOut, b.bytesOut)},
				{"opened", rate(a.opened, b.opened)},
				{"closed", rate(a.closed, b.closed)},
				{"announces", rate(a.announces, b.announces)},
				{"answers", rate(a.answers, b.answers)},
				{"scrapes", rate(a.scrapes, b.scrapes)},
				{"failures", rate(a.failures, b.failures)},
				{"rateLimited", rate(a.rateLimited, b.rateLimited)},
				{"invalid", rate(a.invalid, b.invalid)}
			}},
			{"failureReasons", failureReasons},
			{"nonStandard", nonStandard},
			{"failureWindowSeconds", (int64_t) std::round(windowSeconds)},
			{"crossThreadPct", sent ? round1(100.0 * cross / sent) : 0.0},
			{"lockWaitPct", round1((b.lockWaitNs - std::min(b.lockWaitNs, a.lockWaitNs)) / (seconds * 1e7))},
			{"lockHeldPct", round1((b.lockHeldNs - std::min(b.lockHeldNs, a.lockHeldNs)) / (seconds * 1e7))},
			{"totals", {
				{"opened", b.opened}, {"closed", b.closed},
				{"messagesIn", b.messagesIn}, {"messagesOut", b.messagesOut},
				{"announces", b.announces}, {"answers", b.answers}, {"scrapes", b.scrapes},
				{"failures", b.failures}, {"rateLimited", b.rateLimited}, {"invalid", b.invalid}
			}}
		};
	}

};

};

#endif // OWT_METRICS_H
