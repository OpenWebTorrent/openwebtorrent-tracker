
#ifndef OWT_FASTTRACKER_H
#define OWT_FASTTRACKER_H

#include <string>
#include <unordered_map>
#include <json.hpp>
#include <algorithm>
#include <random>

#include "Tracker.h"
#include "TrackerException.h"
#include "PeerContext.h"
#include "Swarm.h"
#include "Utils.h"

using namespace nlohmann;

namespace owt {

class FastTracker : public Tracker {
private:
	SwarmMap swarms;
	std::unordered_map<std::string, PeerContext*> peers;

	TrackerOptions options;

	std::mt19937 rng{std::random_device{}()};

public:

	explicit FastTracker(const TrackerOptions& options) : options(options) {}

	FastTracker(int maxOffers = 20, int announceInterval = 120) {
		this->options.maxOffers = maxOffers;
		this->options.announceInterval = announceInterval;
	}

	const SwarmMap& getSwarms() const override {
		return this->swarms;
	}

	void processMessage(const json& data, PeerContext* peer) override {
		if (!data.is_object()) {
			throw TrackerException("message must be a JSON object");
		}

		std::string action = optionalString(data, "action");

		if (action == "announce") {
			std::string event = optionalString(data, "event");

			if (event == "" || event == "update" || event == "paused") {
				if (data.contains("answer")) {
					this->processAnswer(data, peer);
				} else {
					this->processAnnounce(data, peer);
				}
			} else if (event == "started") {
				this->processAnnounce(data, peer);
			} else if (event == "stopped") {
				this->processStop(data, peer);
			} else if (event == "completed") {
				this->processAnnounce(data, peer, true);
			} else {
				throw TrackerException("unknown announce event");
			}
		} else if (action == "scrape") {
			this->processScrape(data, peer);
		} else {
			throw TrackerException("unknown action");
		}
	}

	void disconnectPeer(PeerContext* peer) override {
		// Peer didn't send any id
		if (peer->id.empty()) {
			return;
		}

		// Remove peer from its swarms.
		for (const auto& infoHash: peer->infoHashes) {
			this->removePeerFromSwarm(peer, infoHash);
		}
		peer->infoHashes.clear();

		// Only unregister the id if it still belongs to this peer (it may
		// have been taken over by a newer connection with the same peer_id).
		if (auto it = this->peers.find(peer->id); it != this->peers.end() && it->second == peer) {
			this->peers.erase(it);
		}
		peer->id.clear();
	}

private:

	// Returns the string value of an optional field, "" if missing or null
	static std::string optionalString(const json& data, const char* key) {
		auto it = data.find(key);
		if (it == data.end() || it->is_null()) {
			return "";
		}
		if (!it->is_string()) {
			throw TrackerException(std::string(key) + " field must be a string");
		}
		return it->get<std::string>();
	}

	// info_hash and peer_id are 20-byte binary strings (one code point per byte)
	static std::string requireBinaryId(const json& data, const char* key) {
		auto it = data.find(key);
		if (it == data.end() || !it->is_string() || Utils::binaryStringLength(it->get_ref<const std::string&>()) != 20) {
			throw TrackerException(std::string(key) + " field is missing or wrong");
		}
		return it->get<std::string>();
	}

	static bool isValidInfoHash(const json& value) {
		return value.is_string() && Utils::binaryStringLength(value.get_ref<const std::string&>()) == 20;
	}

	// Associates a peer_id with the connection
	void bindPeerId(PeerContext* peer, const std::string& peerId) {
		if (peer->id.empty()) {
			// Peer has changed of socket: detach the old one
			if (auto it = this->peers.find(peerId); it != this->peers.end() && it->second != peer) {
				this->disconnectPeer(it->second);
			}

			peer->id = peerId;
			this->peers[peerId] = peer;
		} else if (peer->id != peerId) {
			throw TrackerException("announce: different peer_id on the same connection");
		}
	}

	void processAnnounce(const json& data, PeerContext* peer, bool completed = false) {
		// Validate params
		std::string infoHash = requireBinaryId(data, "info_hash");
		std::string peerId = requireBinaryId(data, "peer_id");

		auto offersIt = data.find("offers");
		if (offersIt != data.end() && !offersIt->is_array()) {
			throw TrackerException("announce: offers field is not an array");
		}

		this->bindPeerId(peer, peerId);

		auto leftIt = data.find("left");
		bool isSeeder = completed || (leftIt != data.end() && leftIt->is_number() && *leftIt == 0);

		Swarm* swarm = this->addPeerToSwarm(peer, infoHash);
		if (isSeeder && swarm->setCompleted(peer) && completed) {
			swarm->addDownload();
		}

		peer->sendMessage({
			{"action", "announce"},
			{"interval", this->options.announceInterval},
			{"info_hash", infoHash},
			{"complete", swarm->getCompletedCount()},
			{"incomplete", swarm->getIncompleteCount()},
		});

		if (offersIt != data.end()) {
			this->sendOffersToPeers(data, *offersIt, swarm, peer, infoHash);
		}
	}

	void processAnswer(const json& data, PeerContext* peer) {
		// Validate params
		std::string infoHash = requireBinaryId(data, "info_hash");
		std::string toPeerId = requireBinaryId(data, "to_peer_id");

		const json& answer = data["answer"];
		if (!answer.is_object() || !answer.contains("sdp") || !answer["sdp"].is_string()) {
			throw TrackerException("answer: answer field is missing or wrong");
		}
		if (!data.contains("offer_id") || !data["offer_id"].is_string()) {
			throw TrackerException("answer: offer_id field is missing or wrong");
		}

		if (peer->id.empty()) {
			this->bindPeerId(peer, requireBinaryId(data, "peer_id"));
		}

		// Both peers must be in the swarm. The target may have just left,
		// which is a normal race, so the answer is silently dropped.
		auto swarmIt = this->swarms.find(infoHash);
		auto toPeerIt = this->peers.find(toPeerId);
		if (swarmIt == this->swarms.end() || toPeerIt == this->peers.end() ||
				!swarmIt->second->hasPeer(toPeerIt->second) || !swarmIt->second->hasPeer(peer)) {
			DOUT("answer: to_peer_id is not in the swarm");
			return;
		}

		toPeerIt->second->sendMessage({
			{"action", "announce"},
			{"answer", {
				{"type", "answer"},
				{"sdp", answer["sdp"]},
			}},
			{"offer_id", data["offer_id"]},
			{"peer_id", peer->id},
			{"info_hash", infoHash},
		});
	}

	void processStop(const json& data, PeerContext* peer) {
		// Validate params
		std::string infoHash = requireBinaryId(data, "info_hash");

		this->removePeerFromSwarm(peer, infoHash);
		peer->infoHashes.erase(infoHash);
	}

	void processScrape(const json& data, PeerContext* peer) {
		json files = json::object();

		auto addFile = [&](const std::string& infoHash) {
			auto it = this->swarms.find(infoHash);
			if (it != this->swarms.end()) {
				Swarm* swarm = it->second.get();
				files[infoHash] = {
					{"complete", swarm->getCompletedCount()},
					{"incomplete", swarm->getIncompleteCount()},
					{"downloaded", swarm->getDownloadedCount()},
				};
			} else {
				files[infoHash] = {
					{"complete", 0},
					{"incomplete", 0},
					{"downloaded", 0},
				};
			}
		};

		auto infoHashIt = data.find("info_hash");
		if (infoHashIt == data.end() || infoHashIt->is_null()) {
			if (!this->options.allowFullScrape) {
				throw TrackerException("scrape: full scrape is disabled");
			}
			for (const auto& entry : this->swarms) {
				addFile(entry.first);
			}
		} else if (infoHashIt->is_array()) {
			if (infoHashIt->size() > this->options.maxScrapeInfoHashes) {
				throw TrackerException("scrape: too many info_hash values");
			}
			for (const auto& item : *infoHashIt) {
				if (isValidInfoHash(item)) {
					addFile(item.get<std::string>());
				}
			}
		} else if (isValidInfoHash(*infoHashIt)) {
			addFile(infoHashIt->get<std::string>());
		} else {
			throw TrackerException("scrape: info_hash field is wrong");
		}

		peer->sendMessage({
			{"action", "scrape"},
			{"files", files}
		});
	}

	Swarm* addPeerToSwarm(PeerContext* peer, const std::string& infoHash) {
		size_t maxSwarms = this->options.maxSwarmsPerPeer;
		if (maxSwarms > 0 && peer->infoHashes.size() >= maxSwarms && peer->infoHashes.count(infoHash) == 0) {
			throw TrackerException("announce: too many torrents on this connection");
		}

		auto& swarm = this->swarms[infoHash];
		if (!swarm) {
			swarm = std::make_unique<Swarm>(infoHash);
		}

		swarm->addPeer(peer, false);
		peer->infoHashes.insert(infoHash);

		return swarm.get();
	}

	void removePeerFromSwarm(PeerContext* peer, const std::string& infoHash) {
		auto it = this->swarms.find(infoHash);
		if (it == this->swarms.end()) {
			return;
		}

		it->second->removePeer(peer);

		// Free up space of swarm here.
		if (it->second->getPeersCount() == 0) {
			this->swarms.erase(it);
		}
	}

	void sendOffersToPeers(const json& data, const json& offers, Swarm* swarm, PeerContext* peer, const std::string& infoHash) {
		size_t count = std::min(offers.size(), (size_t) std::max(this->options.maxOffers, 0));

		// Numwant (optional, clients always send one offer per wanted peer)
		auto numwantIt = data.find("numwant");
		if (numwantIt != data.end() && numwantIt->is_number()) {
			count = (size_t) std::clamp(numwantIt->get<double>(), 0.0, (double) count);
		}

		if (count == 0 || swarm->getPeersCount() <= 1) {
			return;
		}

		// Validate the offers before sending anything
		for (size_t i = 0; i < count; i++) {
			const json& item = offers[i];
			if (!item.is_object() || !item.contains("offer_id") || !item["offer_id"].is_string() ||
					!item.contains("offer") || !item["offer"].is_object()) {
				throw TrackerException("announce: wrong offer item format");
			}
			const json& offer = item["offer"];
			if (!offer.contains("sdp") || !offer["sdp"].is_string()) {
				throw TrackerException("announce: wrong offer item field format");
			}
		}

		std::vector<PeerContext*> toPeers = swarm->getRandomPeers(count, peer, this->rng);
		for (size_t i = 0; i < toPeers.size(); i++) {
			this->sendOffer(offers[i], peer->id, toPeers[i], infoHash);
		}
	}

	void sendOffer(const json& offerItem, const std::string& fromPeerId, PeerContext* toPeer, const std::string& infoHash) {
		toPeer->sendMessage({
			{"action", "announce"},
			{"info_hash", infoHash},
			{"offer_id", offerItem["offer_id"]},
			{"peer_id", fromPeerId},
			{"offer", {
				{"type", "offer"},
				{"sdp", offerItem["offer"]["sdp"]},
			}}
		});
	}

};

};

#endif // OWT_FASTTRACKER_H
