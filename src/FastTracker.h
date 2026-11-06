
#ifndef OWT_FASTTRACKER_H
#define OWT_FASTTRACKER_H

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <json.hpp>
#include <algorithm>
#include <cmath>
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

	size_t getPeerIdCount() const override {
		return this->peers.size();
	}

	size_t getMembershipCount() const override {
		return this->membershipTotal;
	}

	size_t getSeederCount() const override {
		return this->seederTotal;
	}

	void processMessage(const json& data, PeerContext* peer) override {
		if (!data.is_object()) {
			throw TrackerException(FailureReason::NotObject);
		}

		std::string action = optionalString(data, "action", FailureReason::ActionNotString);

		if (action == "announce") {
			std::string event = optionalString(data, "event", FailureReason::EventNotString);

			// Absent and "empty" mean "update", like bittorrent-tracker
			if (event == "" || event == "update" || event == "empty" || event == "paused") {
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
				throw TrackerException(FailureReason::UnknownEvent);
			}
		} else if (action == "scrape") {
			this->processScrape(data, peer);
		} else {
			throw TrackerException(FailureReason::UnknownAction);
		}
	}

	json diagnose(const json& data, const PeerContext* connection) const override {
		json out = {
			{"connectionTorrents", connection->membershipCount},
			{"connectionPeerIds", connection->memberships.size()}
		};
		if (!data.is_object()) {
			return out;
		}
		auto stringField = [&](const char* key) -> const std::string* {
			auto it = data.find(key);
			return it != data.end() && it->is_string() ? &it->get_ref<const std::string&>() : nullptr;
		};
		// Which connection a peer ID is registered to, relative to this one
		auto owner = [&](const PeerContext* c) { return c == connection ? "this" : "other"; };
		const std::string* peerId = stringField("peer_id");
		const std::string* toPeerId = stringField("to_peer_id");
		const std::string* infoHash = stringField("info_hash");
		if (peerId != nullptr) {
			auto it = this->peers.find(*peerId);
			out["peerIdConnection"] = it == this->peers.end() ? "none" : owner(it->second);
			out["peerIdOnThisConnection"] = connection->memberships.count(*peerId) > 0;
		}
		if (infoHash != nullptr) {
			auto swarmIt = this->swarms.find(*infoHash);
			out["swarmExists"] = swarmIt != this->swarms.end();
			if (swarmIt != this->swarms.end()) {
				const Swarm& swarm = *swarmIt->second;
				out["swarmPeers"] = swarm.getPeersCount();
				if (peerId != nullptr) {
					const Swarm::Member* member = swarm.findPeer(*peerId);
					out["peerInSwarm"] = member == nullptr ? "no" : owner(member->connection);
				}
				if (toPeerId != nullptr) {
					out["toPeerInSwarm"] = swarm.findPeer(*toPeerId) != nullptr;
				}
			}
		}
		return out;
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
		std::vector<std::pair<std::string, std::string>> stale;
		for (const auto& [peerId, connection] : this->peers) {
			this->collectStale(peerId, connection, now, stale);
		}
		this->removeStale(stale);
	}

	void cleanupStalePeersStep(unsigned int slices) override {
		this->cleanupStalePeersStep(Clock::now(), slices);
	}

	// Scans the next 1/slices of the peer table's buckets. Bucket iterators
	// stay valid across calls as long as the table isn't rehashed; if it was,
	// the sweep restarts (rehashes are rare: the table only grows by doubling).
	void cleanupStalePeersStep(Clock::time_point now, unsigned int slices) {
		size_t buckets = this->peers.bucket_count();
		if (buckets != this->cleanupBuckets) {
			this->cleanupBuckets = buckets;
			this->cleanupCursor = 0;
		}
		size_t perStep = (buckets + std::max(1u, slices) - 1) / std::max(1u, slices);
		size_t end = std::min(buckets, this->cleanupCursor + perStep);

		std::vector<std::pair<std::string, std::string>> stale;
		for (size_t bucket = this->cleanupCursor; bucket < end; bucket++) {
			for (auto it = this->peers.begin(bucket); it != this->peers.end(bucket); ++it) {
				this->collectStale(it->first, it->second, now, stale);
			}
		}
		this->cleanupCursor = end >= buckets ? 0 : end;
		this->removeStale(stale);
	}

	unsigned int getCleanupPeriodMs() const override {
		// Half an interval, so stale memberships live at most 2.5 intervals
		return (unsigned int) std::max(1000, this->options.announceInterval * 500);
	}

private:

	// Announce interval for one reply: uniform in announceInterval ± jitter, whole seconds
	int nextInterval() {
		const int base = this->options.announceInterval;
		const double jitter = std::clamp(this->options.announceIntervalJitter, 0.0, 0.5);
		const int low = std::max(1, (int) std::lround(base * (1.0 - jitter)));
		const int high = std::max(low, (int) std::lround(base * (1.0 + jitter)));
		if (low == high) {
			return low;
		}
		return std::uniform_int_distribution<int>(low, high)(this->rng);
	}

	// Running totals over all swarms (see getMembershipCount)
	size_t membershipTotal = 0;
	size_t seederTotal = 0;

	// Incremental cleanup position (bucket index) and the bucket count it refers to
	size_t cleanupCursor = 0;
	size_t cleanupBuckets = 0;

	void collectStale(const std::string& peerId, PeerContext* connection, Clock::time_point now, std::vector<std::pair<std::string, std::string>>& stale) const {
		const auto timeout = std::chrono::seconds(2 * this->options.announceInterval);
		auto it = connection->memberships.find(peerId);
		if (it == connection->memberships.end()) {
			return;
		}
		for (const auto& [infoHash, lastAnnounce] : it->second) {
			if (now - lastAnnounce > timeout) {
				stale.emplace_back(peerId, infoHash);
			}
		}
	}

	void removeStale(const std::vector<std::pair<std::string, std::string>>& stale) {
		for (const auto& [peerId, infoHash] : stale) {
			auto it = this->peers.find(peerId);
			if (it != this->peers.end()) {
				this->removeMembership(it->second, peerId, infoHash);
			}
		}
	}

	// Returns the string value of an optional field, "" if missing or null
	static std::string optionalString(const json& data, const char* key, FailureReason reason) {
		auto it = data.find(key);
		if (it == data.end() || it->is_null()) {
			return "";
		}
		if (!it->is_string()) {
			throw TrackerException(reason);
		}
		return it->get<std::string>();
	}

	// info_hash and peer_id are 20-byte binary strings (one code point per byte)
	static std::string requireBinaryId(const json& data, const char* key, FailureReason reason) {
		auto it = data.find(key);
		if (it == data.end() || !it->is_string() || Utils::binaryStringLength(it->get_ref<const std::string&>()) != 20) {
			throw TrackerException(reason);
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
		std::string infoHash = requireBinaryId(data, "info_hash", FailureReason::AnnounceInfoHash);
		std::string peerId = requireBinaryId(data, "peer_id", FailureReason::AnnouncePeerId);

		auto offersIt = data.find("offers");
		if (offersIt != data.end() && !offersIt->is_array()) {
			throw TrackerException(FailureReason::OffersNotArray);
		}

		// Limit checked before any state changes
		size_t maxMemberships = this->options.maxSwarmsPerPeer;
		if (maxMemberships > 0 && !connection->hasMembership(peerId, infoHash) && connection->membershipCount >= maxMemberships) {
			throw TrackerException(FailureReason::TooManyTorrents);
		}

		this->claimPeerId(connection, peerId);
		Swarm* swarm = this->addMembership(connection, peerId, infoHash);

		auto leftIt = data.find("left");
		bool isSeeder = completed || (leftIt != data.end() && leftIt->is_number() && *leftIt == 0);
		if (isSeeder) {
			if (swarm->setCompleted(peerId)) {
				this->seederTotal++;
				if (completed) {
					swarm->addDownload();
				}
			}
		} else {
			swarm->setPaused(peerId, paused);
		}

		connection->sendMessage({
			{"action", "announce"},
			{"interval", this->nextInterval()},
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
		std::string infoHash = requireBinaryId(data, "info_hash", FailureReason::AnswerInfoHash);
		std::string peerId = requireBinaryId(data, "peer_id", FailureReason::AnswerPeerId);
		std::string toPeerId = requireBinaryId(data, "to_peer_id", FailureReason::AnswerToPeerId);

		const json& answer = data["answer"];
		if (!answer.is_object() || !answer.contains("sdp") || !answer["sdp"].is_string()) {
			throw TrackerException(FailureReason::BadAnswer);
		}
		if (!data.contains("offer_id") || !data["offer_id"].is_string()) {
			throw TrackerException(FailureReason::BadOfferId);
		}

		auto swarmIt = this->swarms.find(infoHash);
		auto owner = this->peers.find(peerId);
		bool ownedElsewhere = owner != this->peers.end() && owner->second != connection;
		if (!connection->hasMembership(peerId, infoHash) && !ownedElsewhere) {
			// bittorrent-tracker handles an answer as an announce ("update"),
			// which adds a sender that isn't in the swarm yet. libtorrent
			// (qBittorrent) relies on it: it answers an offer on a new
			// connection before announcing there. If there's no swarm, nobody
			// can receive the answer: dropped below without creating one.
			//
			// A peer ID owned by another connection is never moved by an
			// answer (only an announce moves it): the answer is just relayed.
			// bittorrent-tracker relays it too and keeps the peer's original
			// socket. Relaying is harmless: the recipient only accepts an
			// answer to an offer_id it sent to that peer.
			if (swarmIt != this->swarms.end()) {
				size_t maxMemberships = this->options.maxSwarmsPerPeer;
				if (maxMemberships > 0 && connection->membershipCount >= maxMemberships) {
					throw TrackerException(FailureReason::TooManyTorrents);
				}
				this->claimPeerId(connection, peerId);
				this->addMembership(connection, peerId, infoHash);
				swarmIt = this->swarms.find(infoHash);
			}
		}

		// The target may have just left, which is a normal race, so the
		// answer is silently dropped.
		const Swarm::Member* target = swarmIt == this->swarms.end() ? nullptr : swarmIt->second->findPeer(toPeerId);
		if (target == nullptr) {
			DOUT("answer: to_peer_id is not in the swarm");
			return;
		}

		target->connection->relayAnswer(data, peerId, infoHash);
	}

	void processStop(const json& data, PeerContext* connection) {
		// Validate params
		std::string infoHash = requireBinaryId(data, "info_hash", FailureReason::StopInfoHash);
		std::string peerId = requireBinaryId(data, "peer_id", FailureReason::StopPeerId);

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
				throw TrackerException(FailureReason::FullScrapeDisabled);
			}
			for (const auto& entry : this->swarms) {
				addFile(entry.first);
			}
		} else if (infoHashIt->is_array()) {
			if (infoHashIt->size() > this->options.maxScrapeInfoHashes) {
				throw TrackerException(FailureReason::TooManyScrapeHashes);
			}
			for (const auto& item : *infoHashIt) {
				if (isValidInfoHash(item)) {
					addFile(item.get<std::string>());
				}
			}
		} else if (isValidInfoHash(*infoHashIt)) {
			addFile(infoHashIt->get<std::string>());
		} else {
			throw TrackerException(FailureReason::BadScrapeInfoHash);
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
		size_t before = swarm->getPeersCount();
		swarm->addPeer(connection, peerId);
		this->membershipTotal += swarm->getPeersCount() - before;

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

		size_t peersBefore = it->second->getPeersCount();
		size_t seedersBefore = it->second->getCompletedCount();
		it->second->removePeer(peerId);
		this->membershipTotal -= peersBefore - it->second->getPeersCount();
		this->seederTotal -= seedersBefore - it->second->getCompletedCount();

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
				throw TrackerException(FailureReason::BadOffer);
			}
			const json& offer = item["offer"];
			if (!offer.contains("sdp") || !offer["sdp"].is_string()) {
				throw TrackerException(FailureReason::BadOfferSdp);
			}
		}

		std::vector<const Swarm::Member*> toPeers = swarm->getRandomPeers(count, peerId, this->rng);
		for (size_t i = 0; i < toPeers.size(); i++) {
			this->sendOffer(offers[i], peerId, toPeers[i]->connection, infoHash);
		}
	}

	void sendOffer(const json& offerItem, const std::string& fromPeerId, PeerContext* toPeer, const std::string& infoHash) {
		toPeer->relayOffer(offerItem, fromPeerId, infoHash);
	}

};

};

#endif // OWT_FASTTRACKER_H
