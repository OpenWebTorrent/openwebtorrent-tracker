#ifndef OWT_SWARM_H
#define OWT_SWARM_H

#include <string>
#include <vector>
#include <set>
#include <algorithm>

#include "PeerContext.h"

namespace owt {

class Swarm {

private:
	std::string infoHash;
	std::vector<PeerContext*> peers;
	std::set<std::string> completedPeers;

public:
	Swarm(std::string infoHash) {
		this->infoHash = infoHash;
	}

	~Swarm() {
		// TODO: delete peers?
	}

	std::string getInfoHash() {
		return this->infoHash;
	}

	int getPeersCount() {
		return this->peers.size();
	}

	int getCompletedCount() {
		return this->completedPeers.size();
	}

	std::vector<PeerContext*> getPeers() {
		return this->peers;
	}

	void addPeer(PeerContext* peer, bool completed) {
		this->peers.push_back(peer);
		if (completed) {
			this->completedPeers.insert(peer->id);
		}
	}

	void removePeer(PeerContext* peer) {
        this->completedPeers.erase(peer->id);
		this->peers.erase(std::remove(this->peers.begin(), this->peers.end(), peer), this->peers.end());
    }

	void setCompleted(PeerContext* peer) {
		if (this->completedPeers.find(peer->id) == this->completedPeers.end()) {
			this->completedPeers.insert(peer->id);
		}
	}

};

};

#endif // OWT_SWARM_H
