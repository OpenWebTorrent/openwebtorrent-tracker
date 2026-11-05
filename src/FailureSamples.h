#ifndef OWT_FAILURE_SAMPLES_H
#define OWT_FAILURE_SAMPLES_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <json.hpp>

#include "TrackerException.h"
#include "Utils.h"

using namespace nlohmann;

namespace owt {

// Recent rejected requests, per failure reason, for debugging protocol errors
// (served on /debug/failures.json, loopback only).
//
// Requests are summarized, never stored whole: the summary describes each field
// (type, length, a short preview) but leaves out SDP, which carries the
// client's IP addresses.
class FailureSamples {

public:
	static constexpr size_t PER_REASON = 20;
	// At most one sample per reason per interval, so a flood of bad requests
	// costs nothing beyond the counter
	static constexpr int64_t MIN_INTERVAL_MS = 100;
	static constexpr size_t PREVIEW_BYTES = 48;
	static constexpr size_t MAX_KEYS = 16;

	// Claims the next sample slot for this reason (lock-free, any thread)
	bool shouldSample(FailureReason reason) {
		std::atomic<int64_t>& last = this->lastSampleMs[static_cast<size_t>(reason)];
		int64_t now = steadyMs();
		int64_t previous = last.load(std::memory_order_relaxed);
		return now - previous >= MIN_INTERVAL_MS && last.compare_exchange_strong(previous, now, std::memory_order_relaxed);
	}

	void record(FailureReason reason, json sample) {
		std::lock_guard<std::mutex> lock(this->mutex);
		std::deque<json>& ring = this->samples[static_cast<size_t>(reason)];
		if (ring.size() >= PER_REASON) {
			ring.pop_front();
		}
		ring.push_back(std::move(sample));
	}

	// {reason key: [samples, newest first]}, only reasons with samples
	std::string dump() const {
		json out = json::object();
		std::lock_guard<std::mutex> lock(this->mutex);
		for (size_t i = 0; i < FAILURE_REASON_COUNT; i++) {
			const std::deque<json>& ring = this->samples[i];
			if (!ring.empty()) {
				json newestFirst = json::array();
				for (auto it = ring.rbegin(); it != ring.rend(); ++it) {
					newestFirst.push_back(*it);
				}
				out[failureReasonKey(static_cast<FailureReason>(i))] = std::move(newestFirst);
			}
		}
		return out.dump(1, '\t', false, json::error_handler_t::replace);
	}

	// Shape of a request: which fields it has and what they look like
	static json summarize(const json& data) {
		if (!data.is_object()) {
			return describe(data);
		}
		json out = {{"keys", keys(data)}};
		for (const char* key : {"action", "event", "info_hash", "peer_id", "to_peer_id", "offer_id", "numwant", "offers", "answer"}) {
			auto it = data.find(key);
			if (it != data.end()) {
				out[key] = describe(*it);
			}
		}
		// First offer: its fields (offer_id, offer keys), not the SDP
		auto offers = data.find("offers");
		if (offers != data.end() && offers->is_array() && !offers->empty() && offers->front().is_object()) {
			const json& first = offers->front();
			json offer = {{"keys", keys(first)}};
			for (const char* key : {"offer_id", "offer"}) {
				auto it = first.find(key);
				if (it != first.end()) {
					offer[key] = describe(*it);
				}
			}
			out["firstOffer"] = offer;
		}
		return out;
	}

	// Type, size and a short preview. Objects list their keys only.
	static json describe(const json& value, int depth = 0) {
		if (value.is_string()) {
			const std::string& s = value.get_ref<const std::string&>();
			json out = {
				{"type", "string"},
				{"bytes", s.size()},
				// Code points if every one is <= U+00FF (binary string), else -1
				{"binaryChars", Utils::binaryStringLength(s)},
				{"preview", preview(s)}
			};
			if (!s.empty() && s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
				out["hex"] = true;
			}
			return out;
		}
		if (value.is_array()) {
			json out = {{"type", "array"}, {"size", value.size()}};
			if (!value.empty() && depth < 2) {
				out["first"] = describe(value.front(), depth + 1);
			}
			return out;
		}
		if (value.is_object()) {
			return {{"type", "object"}, {"keys", keys(value)}};
		}
		return {{"type", value.type_name()}, {"value", value}};
	}

private:
	mutable std::mutex mutex;
	std::array<std::deque<json>, FAILURE_REASON_COUNT> samples;
	std::array<std::atomic<int64_t>, FAILURE_REASON_COUNT> lastSampleMs{};

	static int64_t steadyMs() {
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	}

	static json keys(const json& object) {
		json out = json::array();
		for (auto it = object.begin(); it != object.end() && out.size() < MAX_KEYS; ++it) {
			out.push_back(preview(it.key()));
		}
		return out;
	}

	// First bytes of a string, cut at a UTF-8 character boundary
	static std::string preview(const std::string& s) {
		if (s.size() <= PREVIEW_BYTES) {
			return s;
		}
		size_t cut = PREVIEW_BYTES;
		while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
			cut--;
		}
		return s.substr(0, cut) + "…";
	}

};

};

#endif // OWT_FAILURE_SAMPLES_H
