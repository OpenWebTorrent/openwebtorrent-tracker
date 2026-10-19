
#ifndef OWT_FASTTRACKER_H
#define OWT_FASTTRACKER_H

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <json.hpp>
#include <algorithm>
#include <chrono>
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
	using Clock = PeerContext::Clock;

	SwarmMap swarms;
	// A peer ID identifies a swarm member; several peer IDs may share one connection.
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
					this->processAnnounce(data, peer, false, event == "paused");
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

	// Called when the connection closes: removes every peer ID it carries
	void disconnectPeer(PeerContext* peer) override {
		std::vector<std::string> peerIds;
		for (const auto& entry : peer->memberships) {
			peerIds.push_back(entry.first);
		}
		for (const auto& peerId : peerIds) {
			this->removePeerId(peerId, peer);
		}
	}

	// Memberships that were not announced for two intervals are removed,
	// even if their connection stays open.
	void cleanupStalePeers() override {
		this->cleanupStalePeers(Clock::now());
	}

	void cleanupStalePeers(Clock::time_point now) {
		const auto timeout = std::chrono::seconds(2 * this->options.announceInterval);

		std::vector<std::pair<std::string, std::string>> stale;
		for (const auto& [peerId, connection] : this->peers) {
			auto it = connection->memberships.find(peerId);
			if (it == connection->memberships.end()) {
				continue;
			}
			for (const auto& [infoHash, lastAnnounce] : it->second) {
				if (now - lastAnnounce > timeout) {
					stale.emplace_back(peerId, infoHash);
				}
			}
		}

		for (const auto& [peerId, infoHash] : stale) {
			auto it = this->peers.find(peerId);
			if (it != this->peers.end()) {
				this->removeMembership(it->second, peerId, infoHash);
			}
		}
	}

	unsigned int getCleanupPeriodMs() const override {
		// Half an interval, so stale memberships live at most 2.5 intervals
		return (unsigned int) std::max(1000, this->options.announceInterval * 500);
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

	// Associates a peer ID with the connection. A peer ID announced on
	// another connection moves here; other peer IDs of that connection stay.
	void claimPeerId(PeerContext* connection, const std::string& peerId) {
		auto it = this->peers.find(peerId);
		if (it != this->peers.end() && it->second != connection) {
			this->removePeerId(peerId, it->second);
		}
		this->peers[peerId] = connection;
	}

	void processAnnounce(const json& data, PeerContext* connection, bool completed = false, bool paused = false) {
		// Validate params
		std::string infoHash = requireBinaryId(data, "info_hash");
		std::string peerId = requireBinaryId(data, "peer_id");

		auto offersIt = data.find("offers");
		if (offersIt != data.end() && !offersIt->is_array()) {
			throw TrackerException("announce: offers field is not an array");
		}

		// Limit checked before any state changes
		size_t maxMemberships = this->options.maxSwarmsPerPeer;
		if (maxMemberships > 0 && !connection->hasMembership(peerId, infoHash) && connection->membershipCount >= maxMemberships) {
			throw TrackerException("announce: too many torrents on this connection");
		}

		this->claimPeerId(connection, peerId);
		Swarm* swarm = this->addMembership(connection, peerId, infoHash);

		auto leftIt = data.find("left");
		bool isSeeder = completed || (leftIt != data.end() && leftIt->is_number() && *leftIt == 0);
		if (isSeeder) {
			if (swarm->setCompleted(peerId) && completed) {
				swarm->addDownload();
			}
		} else {
			swarm->setPaused(peerId, paused);
		}

		connection->sendMessage({
			{"action", "announce"},
			{"interval", this->options.announceInterval},
			{"info_hash", infoHash},
			{"complete", swarm->getCompletedCount()},
			{"incomplete", swarm->getIncompleteCount()},
		});

		if (offersIt != data.end()) {
			this->sendOffersToPeers(data, *offersIt, swarm, peerId, infoHash);
		}
	}

	void processAnswer(const json& data, PeerContext* connection) {
		// Validate params
		std::string infoHash = requireBinaryId(data, "info_hash");
		std::string peerId = requireBinaryId(data, "peer_id");
		std::string toPeerId = requireBinaryId(data, "to_peer_id");

		const json& answer = data["answer"];
		if (!answer.is_object() || !answer.contains("sdp") || !answer["sdp"].is_string()) {
			throw TrackerException("answer: answer field is missing or wrong");
		}
		if (!data.contains("offer_id") || !data["offer_id"].is_string()) {
			throw TrackerException("answer: offer_id field is missing or wrong");
		}

		// The sender must have announced this torrent with this peer ID on this connection
		if (!connection->hasMembership(peerId, infoHash)) {
			throw TrackerException("answer: peer_id is not in the swarm on this connection");
		}

		// The target may have just left, which is a normal race, so the
		// answer is silently dropped.
		auto swarmIt = this->swarms.find(infoHash);
		const Swarm::Member* target = swarmIt == this->swarms.end() ? nullptr : swarmIt->second->findPeer(toPeerId);
		if (target == nullptr) {
			DOUT("answer: to_peer_id is not in the swarm");
			return;
		}

		target->connection->sendMessage({
			{"action", "announce"},
			{"answer", {
				{"type", "answer"},
				{"sdp", answer["sdp"]},
			}},
			{"offer_id", data["offer_id"]},
			{"peer_id", peerId},
			{"info_hash", infoHash},
		});
	}

	void processStop(const json& data, PeerContext* connection) {
		// Validate params
		std::string infoHash = requireBinaryId(data, "info_hash");
		std::string peerId = requireBinaryId(data, "peer_id");

		// Ignored if this connection doesn't own that membership
		this->removeMembership(connection, peerId, infoHash);
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
					// BEP 21
					{"downloaders", swarm->getDownloadersCount()},
				};
			} else {
				files[infoHash] = {
					{"complete", 0},
					{"incomplete", 0},
					{"downloaded", 0},
					{"downloaders", 0},
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

	// Adds or refreshes a membership. The peer ID must belong to the connection.
	Swarm* addMembership(PeerContext* connection, const std::string& peerId, const std::string& infoHash) {
		auto& memberships = connection->memberships[peerId];
		if (memberships.insert_or_assign(infoHash, Clock::now()).second) {
			connection->membershipCount++;
		}

		auto& swarm = this->swarms[infoHash];
		if (!swarm) {
			swarm = std::make_unique<Swarm>(infoHash);
		}
		swarm->addPeer(connection, peerId);

		return swarm.get();
	}

	// Removes a membership, and the peer ID once it has none left.
	// Returns false if the connection doesn't own it.
	bool removeMembership(PeerContext* connection, const std::string& peerId, const std::string& infoHash) {
		auto it = connection->memberships.find(peerId);
		if (it == connection->memberships.end() || it->second.erase(infoHash) == 0) {
			return false;
		}
		connection->membershipCount--;
		this->removeFromSwarm(peerId, infoHash);

		if (it->second.empty()) {
			this->removePeerId(peerId, connection);
		}
		return true;
	}

	// Removes a peer ID and all its memberships from a connection
	void removePeerId(const std::string& peerId, PeerContext* connection) {
		auto it = connection->memberships.find(peerId);
		if (it != connection->memberships.end()) {
			for (const auto& entry : it->second) {
				this->removeFromSwarm(peerId, entry.first);
			}
			connection->membershipCount -= it->second.size();
			connection->memberships.erase(it);
		}

		// Only unregister the id if it still belongs to this connection
		if (auto owner = this->peers.find(peerId); owner != this->peers.end() && owner->second == connection) {
			this->peers.erase(owner);
		}
	}

	void removeFromSwarm(const std::string& peerId, const std::string& infoHash) {
		auto it = this->swarms.find(infoHash);
		if (it == this->swarms.end()) {
			return;
		}

		it->second->removePeer(peerId);

		// Free up space of swarm here.
		if (it->second->getPeersCount() == 0) {
			this->swarms.erase(it);
		}
	}

	void sendOffersToPeers(const json& data, const json& offers, Swarm* swarm, const std::string& peerId, const std::string& infoHash) {
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

		std::vector<const Swarm::Member*> toPeers = swarm->getRandomPeers(count, peerId, this->rng);
		for (size_t i = 0; i < toPeers.size(); i++) {
			this->sendOffer(offers[i], peerId, toPeers[i]->connection, infoHash);
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
