#ifndef OWT_PEERCONTEXT_H
#define OWT_PEERCONTEXT_H

#include <chrono>
#include <cstdint>
#include <map>
#include <vector>
#include <string>
#include <string_view>
#include <json.hpp>

#include "Utils.h"

using namespace nlohmann;

namespace owt {

// Represents a WebSocket connection, not a single peer: clients such as
// libtorrent announce several torrents with different peer IDs on one socket.
struct PeerContext {
	using Clock = std::chrono::steady_clock;

	// Type-erased transport so the tracker logic works with both SSL and
	// plain websockets (and can be unit tested without a network).
	using SendFn = void (*)(void* socket, std::string_view message);

	void* socket = nullptr;
	SendFn sendFn = nullptr;

	// Multi-threaded server: unique connection id and the event loop that owns
	// the socket. Only that loop's thread may write to the socket.
	uint64_t id = 0;
	void* owner = nullptr;

	// A message produced while the tracker lock is held. It is serialized and
	// delivered after the lock is released, possibly on another thread, so it
	// refers to the connection by id (the socket may close in the meantime).
	//
	// Relayed offers and answers (the bulk of the traffic, carrying SDP blobs)
	// aren't built under the lock: they keep a pointer into the request
	// message and are written as a frame at delivery. The server guarantees
	// the request outlives delivery (both happen within one message handler).
	struct Outgoing {
		enum class Kind : uint8_t { Json, Offer, Answer };

		uint64_t id;
		void* owner;
		json data;
		Kind kind = Kind::Json;
		// Offer: the offer item ({offer_id, offer: {sdp}}). Answer: the whole request.
		const json* source = nullptr;
		std::string peerId;
		std::string infoHash;

		std::string frame() const {
			switch (this->kind) {
			case Kind::Offer:
				return offerFrame(*this->source, this->peerId, this->infoHash);
			case Kind::Answer:
				return answerFrame(*this->source, this->peerId, this->infoHash);
			default:
				return serialize(this->data);
			}
		}
	};

	// When set (by the server, per thread), sendMessage queues instead of
	// writing to the socket. Unset in unit tests: messages are sent directly.
	static inline thread_local std::vector<Outgoing>* outbox = nullptr;

	static std::string serialize(const json& data) {
		return data.dump(-1, ' ', false, json::error_handler_t::replace);
	}

	// Same bytes as serializing the equivalent json object (keys in sorted
	// order), without building it.
	static std::string offerFrame(const json& offerItem, const std::string& fromPeerId, const std::string& infoHash) {
		std::string frame;
		frame.reserve(160 + offerItem["offer"]["sdp"].get_ref<const std::string&>().size());
		frame += R"({"action":"announce","info_hash":)";
		frame += serialize(infoHash);
		frame += R"(,"offer":{"sdp":)";
		frame += serialize(offerItem["offer"]["sdp"]);
		frame += R"(,"type":"offer"},"offer_id":)";
		frame += serialize(offerItem["offer_id"]);
		frame += R"(,"peer_id":)";
		frame += serialize(fromPeerId);
		frame += '}';
		return frame;
	}

	static std::string answerFrame(const json& request, const std::string& fromPeerId, const std::string& infoHash) {
		std::string frame;
		frame.reserve(160 + request["answer"]["sdp"].get_ref<const std::string&>().size());
		frame += R"({"action":"announce","answer":{"sdp":)";
		frame += serialize(request["answer"]["sdp"]);
		frame += R"(,"type":"answer"},"info_hash":)";
		frame += serialize(infoHash);
		frame += R"(,"offer_id":)";
		frame += serialize(request["offer_id"]);
		frame += R"(,"peer_id":)";
		frame += serialize(fromPeerId);
		frame += '}';
		return frame;
	}

	// Peer ID -> info hash -> time of the last announce for that membership
	std::map<std::string, std::map<std::string, Clock::time_point>> memberships;
	size_t membershipCount = 0;

	// Client address key (see Utils::addressKey) and message rate limiter
	std::string address;
	Utils::TokenBucket rateLimiter;

	bool hasMembership(const std::string& peerId, const std::string& infoHash) const {
		auto it = this->memberships.find(peerId);
		return it != this->memberships.end() && it->second.count(infoHash) > 0;
	}

	void sendMessage(json&& data) {
		if (this->socket == nullptr || this->sendFn == nullptr) {
			return;
		}
		if (outbox != nullptr) {
			outbox->push_back({this->id, this->owner, std::move(data)});
			return;
		}
		this->sendFn(this->socket, serialize(data));
	}

	void sendMessage(const json& data) {
		this->sendMessage(json(data));
	}

	// Relays an offer (validated by the tracker) from another peer
	void relayOffer(const json& offerItem, const std::string& fromPeerId, const std::string& infoHash) {
		this->relay(Outgoing::Kind::Offer, offerItem, fromPeerId, infoHash);
	}

	// Relays an answer request (validated by the tracker) from another peer
	void relayAnswer(const json& request, const std::string& fromPeerId, const std::string& infoHash) {
		this->relay(Outgoing::Kind::Answer, request, fromPeerId, infoHash);
	}

	void relay(Outgoing::Kind kind, const json& source, const std::string& fromPeerId, const std::string& infoHash) {
		if (this->socket == nullptr || this->sendFn == nullptr) {
			return;
		}
		Outgoing message{this->id, this->owner, json(), kind, &source, fromPeerId, infoHash};
		if (outbox != nullptr) {
			outbox->push_back(std::move(message));
			return;
		}
		this->sendFn(this->socket, message.frame());
	}
};

};

#endif // OWT_PEERCONTEXT_H
