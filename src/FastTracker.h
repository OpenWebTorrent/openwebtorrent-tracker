#ifndef OWT_FASTTRACKER_H
#define OWT_FASTTRACKER_H

#include <string>
#include <map>
#include <set>
#include <vector>
#include <json.hpp>
#include <algorithm>
#include <chrono>
#include <random>
#include <math.h>

#include "Tracker.h"
#include "TrackerException.h"
#include "PeerContext.h"
#include "Swarm.h"

using namespace nlohmann;

namespace owt {

class FastTracker : public Tracker {
private:
	std::map<std::string, Swarm*> swarms;
	// A peer ID identifies a swarm member; several peer IDs may share one socket.
	std::map<std::string, PeerContext*> peers;

	int maxOffers;
	int announceInterval;

public:
	FastTracker(int maxOffers = 20, int announceInterval = 120) {
		this->maxOffers = maxOffers;
		this->announceInterval = announceInterval;
	}

	std::map<std::string, Swarm*> getSwarms() {
		return this->swarms;
	}

	int getAnnounceInterval() const {
		return this->announceInterval;
	}

	// Called periodically from the owning uWebSockets event loop.
	void cleanupStalePeers() {
		const auto now = std::chrono::steady_clock::now();
		const auto timeout = std::chrono::seconds(this->announceInterval * 2);
		std::vector<std::pair<std::string, std::string>> staleMemberships;

		for (const auto& entry : this->peers) {
			const std::string& peerId = entry.first;
			PeerContext* connection = entry.second;
			auto memberships = connection->infoHashesByPeerId.find(peerId);
			if (memberships == connection->infoHashesByPeerId.end()) {
				continue;
			}
			for (const auto& membership : memberships->second) {
				if (now - membership.second > timeout) {
					staleMemberships.emplace_back(peerId, membership.first);
				}
			}
		}

		for (const auto& stale : staleMemberships) {
			auto owner = this->peers.find(stale.first);
			if (owner == this->peers.end()) {
				continue;
			}
			PeerContext* connection = owner->second;
			this->removeMembership(stale.first, connection, stale.second);
			auto memberships = connection->infoHashesByPeerId.find(stale.first);
			if (memberships != connection->infoHashesByPeerId.end() && memberships->second.empty()) {
				this->removePeerId(stale.first, connection);
			}
		}
	}

	void processMessage(json data, PeerContext* peer) {
		std::string action = (data.count("action") > 0 && data["action"].is_string()) ? data["action"].get<std::string>() : "";

		if (action == "announce") {
			std::string event = (data.count("event") > 0 && data["event"].is_string()) ? data["event"].get<std::string>() : "";

			if (event == "") {
				if (data.count("answer") == 0) {
					this->processAnnounce(data, peer);
				} else {
					this->processAnswer(data, peer);
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

	// Called when the WebSocket closes. Remove every peer identity owned by it.
	void disconnectPeer(PeerContext* peer) {
		if (peer == nullptr) {
			return;
		}
		const std::vector<std::string> peerIds(peer->peerIds.begin(), peer->peerIds.end());
		for (const auto& peerId : peerIds) {
			this->removePeerId(peerId, peer);
		}
	}

private:
	void processAnnounce(json data, PeerContext* connection, bool completed = false) {
		if (data.count("peer_id") == 0 || !data["peer_id"].is_string()) {
			throw TrackerException("announce: peer_id field is missing or wrong");
		}
		if (data.count("info_hash") == 0 || !data["info_hash"].is_string()) {
			throw TrackerException("announce: info_hash field is missing or wrong");
		}

		const std::string infoHash = data["info_hash"];
		const std::string peerId = data["peer_id"];

		auto existing = this->peers.find(peerId);
		const bool takeover = existing != this->peers.end() && existing->second != connection;

		// A peer ID may reconnect on another socket. Replace only that identity;
		// other peer IDs carried by the old socket must remain registered.
		if (takeover) {
			this->removePeerId(peerId, existing->second);
		}

		this->peers[peerId] = connection;
		connection->peerIds.insert(peerId);
		auto& memberships = connection->infoHashesByPeerId[peerId];
		const auto now = std::chrono::steady_clock::now();
		auto membership = memberships.find(infoHash);
		if (membership == memberships.end()) {
			memberships.emplace(infoHash, now);
		} else {
			membership->second = now;
		}

		bool isPeerCompleted = completed ||
			(data.count("left") > 0 && data["left"].is_number() && data["left"] == 0);

		Swarm* swarm = nullptr;
		auto swarmIt = this->swarms.find(infoHash);
		if (swarmIt == this->swarms.end()) {
			swarm = new Swarm(infoHash);
			this->swarms[infoHash] = swarm;
		} else {
			swarm = swarmIt->second;
		}

		if (!swarm->hasPeer(peerId)) {
			swarm->addPeer(connection, peerId, isPeerCompleted);
		} else if (isPeerCompleted) {
			swarm->setCompleted(peerId);
		}

		connection->sendMessage({
			{"action", "announce"},
			{"interval", this->announceInterval},
			{"info_hash", infoHash},
			{"complete", swarm->getCompletedCount()},
			{"incomplete", swarm->getPeersCount() - swarm->getCompletedCount()},
		});

		this->sendOffersToPeers(data, swarm->getPeers(), peerId, infoHash);
	}

	void processAnswer(json data, PeerContext* connection) {
		if (data.count("peer_id") == 0 || !data["peer_id"].is_string()) {
			throw TrackerException("answer: peer_id field is missing or wrong");
		}
		if (data.count("info_hash") == 0 || !data["info_hash"].is_string()) {
			throw TrackerException("answer: info_hash field is missing or wrong");
		}
		if (data.count("to_peer_id") == 0 || !data["to_peer_id"].is_string()) {
			throw TrackerException("answer: to_peer_id field is missing or wrong");
		}

		const std::string fromPeerId = data["peer_id"];
		const std::string infoHash = data["info_hash"];
		const std::string toPeerId = data["to_peer_id"];

		auto from = this->peers.find(fromPeerId);
		if (from == this->peers.end() || from->second != connection) {
			throw TrackerException("answer: peer_id is not registered on this connection");
		}
		auto fromMemberships = connection->infoHashesByPeerId.find(fromPeerId);
		if (fromMemberships == connection->infoHashesByPeerId.end() ||
			fromMemberships->second.count(infoHash) == 0) {
			throw TrackerException("answer: peer_id is not in the swarm");
		}

		auto swarmIt = this->swarms.find(infoHash);
		if (swarmIt == this->swarms.end() || !swarmIt->second->hasPeer(toPeerId)) {
			throw TrackerException("answer: to_peer_id is not in the swarm");
		}
		auto target = this->peers.find(toPeerId);
		if (target == this->peers.end()) {
			throw TrackerException("answer: to_peer_id is not connected");
		}

		data["peer_id"] = fromPeerId;
		data.erase("to_peer_id");
		target->second->sendMessage(data);
	}

	void processStop(json data, PeerContext* connection) {
		if (data.count("peer_id") == 0 || !data["peer_id"].is_string()) {
			throw TrackerException("stop: peer_id field is missing or wrong");
		}
		if (data.count("info_hash") == 0 || !data["info_hash"].is_string()) {
			throw TrackerException("stop: info_hash field is missing or wrong");
		}

		const std::string peerId = data["peer_id"];
		const std::string infoHash = data["info_hash"];
		auto owner = this->peers.find(peerId);
		if (owner == this->peers.end() || owner->second != connection) {
			return;
		}

		if (!this->removeMembership(peerId, connection, infoHash)) {
			return;
		}

		auto memberships = connection->infoHashesByPeerId.find(peerId);
		if (memberships != connection->infoHashesByPeerId.end() && memberships->second.empty()) {
			this->removePeerId(peerId, connection);
		}
	}

	bool removeMembership(const std::string& peerId, PeerContext* connection, const std::string& infoHash) {
		auto memberships = connection->infoHashesByPeerId.find(peerId);
		if (memberships == connection->infoHashesByPeerId.end() || memberships->second.erase(infoHash) == 0) {
			return false;
		}
		auto swarmIt = this->swarms.find(infoHash);
		if (swarmIt != this->swarms.end()) {
			swarmIt->second->removePeer(peerId);
			if (swarmIt->second->getPeersCount() == 0) {
				delete swarmIt->second;
				this->swarms.erase(swarmIt);
			}
		}
		return true;
	}

	void removePeerId(const std::string& peerId, PeerContext* connection) {
		auto memberships = connection->infoHashesByPeerId.find(peerId);
		if (memberships != connection->infoHashesByPeerId.end()) {
			const std::vector<std::string> infoHashes = [&]() {
				std::vector<std::string> result;
				for (const auto& entry : memberships->second) result.push_back(entry.first);
				return result;
			}();
			for (const auto& infoHash : infoHashes) {
				this->removeMembership(peerId, connection, infoHash);
			}
			connection->infoHashesByPeerId.erase(peerId);
		}
		connection->peerIds.erase(peerId);

		auto registered = this->peers.find(peerId);
		if (registered != this->peers.end() && registered->second == connection) {
			this->peers.erase(registered);
		}
	}

	void processScrape(json data, PeerContext* connection) {
		std::map<std::string, json> files;

		if (data.count("info_hash") == 0) {
			for (const auto& entry : this->swarms) {
				Swarm* swarm = entry.second;
				files[swarm->getInfoHash()] = {
					{"complete", swarm->getCompletedCount()},
					{"incomplete", swarm->getPeersCount() - swarm->getCompletedCount()},
					{"downloaded", swarm->getCompletedCount()},
				};
			}
		} else if (data["info_hash"].is_array()) {
			for (auto it = data["info_hash"].begin(); it != data["info_hash"].end(); ++it) {
				if (!it->is_string()) {
					continue;
				}
				const std::string infoHash = *it;
				auto swarmIt = this->swarms.find(infoHash);
				if (swarmIt != this->swarms.end()) {
					Swarm* swarm = swarmIt->second;
					files[infoHash] = {
						{"complete", swarm->getCompletedCount()},
						{"incomplete", swarm->getPeersCount() - swarm->getCompletedCount()},
						{"downloaded", swarm->getCompletedCount()},
					};
				} else {
					files[infoHash] = {{"complete", 0}, {"incomplete", 0}, {"downloaded", 0}};
				}
			}
		} else if (data["info_hash"].is_string()) {
			const std::string infoHash = data["info_hash"];
			auto swarmIt = this->swarms.find(infoHash);
			if (swarmIt != this->swarms.end()) {
				Swarm* swarm = swarmIt->second;
				files[infoHash] = {
					{"complete", swarm->getCompletedCount()},
					{"incomplete", swarm->getPeersCount() - swarm->getCompletedCount()},
					{"downloaded", swarm->getCompletedCount()},
				};
			} else {
				files[infoHash] = {{"complete", 0}, {"incomplete", 0}, {"downloaded", 0}};
			}
		}

		connection->sendMessage({{"action", "scrape"}, {"files", files}});
	}

	void sendOffersToPeers(json data, std::vector<Swarm::Peer> peersInSwarm,
		const std::string& fromPeerId, const std::string& infoHash) {
		if (peersInSwarm.size() <= 1 || data.count("offers") == 0) {
			return;
		}
		if (!data["offers"].is_array()) {
			throw TrackerException("announce: offers field is not an array");
		}
		if (data.count("numwant") == 0 || !data["numwant"].is_number()) {
			return;
		}

		json offers = data["offers"];
		int numwant = data["numwant"];
		int countPeersToSend = static_cast<int>(peersInSwarm.size()) - 1;
		int countOffersToSend = std::min(countPeersToSend,
			std::min(static_cast<int>(offers.size()), std::min(this->maxOffers, numwant)));

		std::vector<Swarm::Peer> recipients;
		for (const auto& target : peersInSwarm) {
			if (target.peerId != fromPeerId) {
				recipients.push_back(target);
			}
		}
		if (recipients.empty()) {
			return;
		}

		// Preserve the reference tracker's randomized offer distribution.
		static thread_local std::mt19937 randomEngine(std::random_device{}());
		std::shuffle(recipients.begin(), recipients.end(), randomEngine);

		for (int i = 0; i < countOffersToSend; ++i) {
			this->sendOffer(offers[i], fromPeerId,
				recipients[static_cast<std::size_t>(i) % recipients.size()], infoHash);
		}
	}

	void sendOffer(json offerItem, const std::string& fromPeerId,
		const Swarm::Peer& toPeer, const std::string& infoHash) {
		if (!offerItem.is_object() || offerItem.count("offer") == 0 || offerItem.count("offer_id") == 0) {
			throw TrackerException("announce: wrong offer item format");
		}
		json offer = offerItem["offer"];
		if (!offer.is_object() || offer.count("sdp") == 0) {
			throw TrackerException("announce: wrong offer item field format");
		}

		toPeer.connection->sendMessage({
			{"action", "announce"},
			{"info_hash", infoHash},
			{"offer_id", offerItem["offer_id"]},
			{"peer_id", fromPeerId},
			{"offer", {{"type", "offer"}, {"sdp", offer["sdp"]}}},
		});
	}
};

};

#endif // OWT_FASTTRACKER_H
