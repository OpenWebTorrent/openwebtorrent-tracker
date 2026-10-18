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

private:
	std::string infoHash;
	// Dense vector for O(1) random access, plus an index for O(1) removal.
	std::vector<PeerContext*> peers;
	std::unordered_map<PeerContext*, size_t> peerIndices;
	std::unordered_set<PeerContext*> completedPeers;
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

	uint64_t getDownloadedCount() const {
		return this->downloadedCount;
	}

	const std::vector<PeerContext*>& getPeers() const {
		return this->peers;
	}

	bool hasPeer(PeerContext* peer) const {
		return this->peerIndices.count(peer) > 0;
	}

	bool isCompleted(PeerContext* peer) const {
		return this->completedPeers.count(peer) > 0;
	}

	void addPeer(PeerContext* peer, bool completed) {
		if (this->peerIndices.emplace(peer, this->peers.size()).second) {
			this->peers.push_back(peer);
		}
		if (completed) {
			this->completedPeers.insert(peer);
		}
	}

	void removePeer(PeerContext* peer) {
		auto it = this->peerIndices.find(peer);
		if (it == this->peerIndices.end()) {
			return;
		}

		// Swap with the last peer and pop
		size_t index = it->second;
		PeerContext* last = this->peers.back();
		this->peers[index] = last;
		this->peerIndices[last] = index;
		this->peers.pop_back();
		this->peerIndices.erase(peer);

		this->completedPeers.erase(peer);
	}

	// Marks a peer as seeder. Returns true if it was not one before.
	bool setCompleted(PeerContext* peer) {
		return this->completedPeers.insert(peer).second;
	}

	void addDownload() {
		this->downloadedCount++;
	}

	// Uniformly picks `count` distinct peers, never including `exclude`.
	template <class URBG>
	std::vector<PeerContext*> getRandomPeers(size_t count, PeerContext* exclude, URBG& rng) const {
		auto excludeIt = this->peerIndices.find(exclude);
		bool hasExclude = excludeIt != this->peerIndices.end();
		size_t candidates = this->peers.size() - (hasExclude ? 1 : 0);
		count = std::min(count, candidates);

		// Maps a slot in [0, candidates) to an index in peers, skipping `exclude`
		auto toPeer = [&](size_t slot) {
			if (hasExclude && slot >= excludeIt->second) {
				slot++;
			}
			return this->peers[slot];
		};

		std::vector<PeerContext*> result;
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
