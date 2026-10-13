#ifndef OWT_SWARM_H
#define OWT_SWARM_H

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "PeerContext.h"

namespace owt {

class Swarm {
public:
	struct Peer {
		PeerContext* connection;
		std::string peerId;
	};

private:
	std::string infoHash;
	std::vector<Peer> peers;
	std::set<std::string> completedPeers;

public:
	explicit Swarm(std::string infoHash) : infoHash(std::move(infoHash)) {}

	std::string getInfoHash() const {
		return this->infoHash;
	}

	int getPeersCount() const {
		return static_cast<int>(this->peers.size());
	}

	int getCompletedCount() const {
		return static_cast<int>(this->completedPeers.size());
	}

	std::vector<Peer> getPeers() const {
		return this->peers;
	}

	bool hasPeer(const std::string& peerId) const {
		return std::any_of(this->peers.begin(), this->peers.end(),
			[&](const Peer& peer) { return peer.peerId == peerId; });
	}

	void addPeer(PeerContext* connection, const std::string& peerId, bool completed) {
		if (!this->hasPeer(peerId)) {
			this->peers.push_back({connection, peerId});
		}
		if (completed) {
			this->completedPeers.insert(peerId);
		}
	}

	void removePeer(const std::string& peerId) {
		this->completedPeers.erase(peerId);
		this->peers.erase(std::remove_if(this->peers.begin(), this->peers.end(),
			[&](const Peer& peer) { return peer.peerId == peerId; }), this->peers.end());
	}

	void setCompleted(const std::string& peerId) {
		if (this->hasPeer(peerId)) {
			this->completedPeers.insert(peerId);
		}
	}
};

};

#endif // OWT_SWARM_H
