#ifndef OWT_SWARM_H
#define OWT_SWARM_H

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "PeerContext.h"

namespace owt {

class Swarm {

public:
	// A swarm member is a peer ID on a connection
	struct Member {
		PeerContext* connection;
		std::string peerId;
	};

private:
	std::string infoHash;
	// Dense vector for O(1) random access, plus an index for O(1) removal.
	std::vector<Member> peers;
	std::unordered_map<std::string, size_t> peerIndices;
	std::unordered_set<std::string> completedPeers;
	// BEP 21 partial seeds: incomplete, but not downloading
	std::unordered_set<std::string> pausedPeers;
	uint64_t downloadedCount = 0;

public:
	explicit Swarm(std::string infoHash) : infoHash(std::move(infoHash)) {}

	const std::string& getInfoHash() const {
		return this->infoHash;
	}

	size_t getPeersCount() const {
		return this->peers.size();
	}

	size_t getCompletedCount() const {
		return this->completedPeers.size();
	}

	size_t getIncompleteCount() const {
		return this->peers.size() - this->completedPeers.size();
	}

	// Incomplete peers that are not BEP 21 partial seeds
	size_t getDownloadersCount() const {
		return this->getIncompleteCount() - this->pausedPeers.size();
	}

	uint64_t getDownloadedCount() const {
		return this->downloadedCount;
	}

	const std::vector<Member>& getPeers() const {
		return this->peers;
	}

	const Member* findPeer(const std::string& peerId) const {
		auto it = this->peerIndices.find(peerId);
		return it == this->peerIndices.end() ? nullptr : &this->peers[it->second];
	}

	bool hasPeer(const std::string& peerId) const {
		return this->peerIndices.count(peerId) > 0;
	}

	bool isCompleted(const std::string& peerId) const {
		return this->completedPeers.count(peerId) > 0;
	}

	bool isPaused(const std::string& peerId) const {
		return this->pausedPeers.count(peerId) > 0;
	}

	void addPeer(PeerContext* connection, const std::string& peerId) {
		auto [it, inserted] = this->peerIndices.emplace(peerId, this->peers.size());
		if (inserted) {
			this->peers.push_back({connection, peerId});
		} else {
			this->peers[it->second].connection = connection;
		}
	}

	void removePeer(const std::string& peerId) {
		auto it = this->peerIndices.find(peerId);
		if (it == this->peerIndices.end()) {
			return;
		}

		// Swap with the last peer and pop
		size_t index = it->second;
		this->peerIndices.erase(it);
		if (index != this->peers.size() - 1) {
			this->peers[index] = std::move(this->peers.back());
			this->peerIndices[this->peers[index].peerId] = index;
		}
		this->peers.pop_back();

		this->completedPeers.erase(peerId);
		this->pausedPeers.erase(peerId);
	}

	// Marks a peer as seeder. Returns true if it was not one before.
	bool setCompleted(const std::string& peerId) {
		if (!this->hasPeer(peerId)) {
			return false;
		}
		this->pausedPeers.erase(peerId);
		return this->completedPeers.insert(peerId).second;
	}

	// BEP 21: a partial seed announces with event=paused, any other announce
	// means it is downloading again. Seeders stay seeders.
	void setPaused(const std::string& peerId, bool paused) {
		if (!this->hasPeer(peerId) || this->isCompleted(peerId)) {
			return;
		}
		if (paused) {
			this->pausedPeers.insert(peerId);
		} else {
			this->pausedPeers.erase(peerId);
		}
	}

	void addDownload() {
		this->downloadedCount++;
	}

	// Uniformly picks `count` distinct members, never including `excludePeerId`.
	template <class URBG>
	std::vector<const Member*> getRandomPeers(size_t count, const std::string& excludePeerId, URBG& rng) const {
		auto excludeIt = this->peerIndices.find(excludePeerId);
		bool hasExclude = excludeIt != this->peerIndices.end();
		size_t candidates = this->peers.size() - (hasExclude ? 1 : 0);
		count = std::min(count, candidates);

		// Maps a slot in [0, candidates) to an index in peers, skipping the excluded one
		auto toPeer = [&](size_t slot) {
			if (hasExclude && slot >= excludeIt->second) {
				slot++;
			}
			return &this->peers[slot];
		};

		std::vector<const Member*> result;
		result.reserve(count);

		if (count == candidates) {
			for (size_t slot = 0; slot < candidates; slot++) {
				result.push_back(toPeer(slot));
			}
			return result;
		}

		// Floyd's sampling algorithm: O(count) regardless of swarm size
		std::unordered_set<size_t> chosen;
		for (size_t j = candidates - count; j < candidates; j++) {
			size_t slot = std::uniform_int_distribution<size_t>(0, j)(rng);
			if (!chosen.insert(slot).second) {
				slot = j;
				chosen.insert(slot);
			}
			result.push_back(toPeer(slot));
		}
		return result;
	}

};

};

#endif // OWT_SWARM_H
