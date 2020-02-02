#ifndef OWT_FASTTRACKER_H
#define OWT_FASTTRACKER_H

#include <string>
#include <map>
#include <json.hpp>
#include <algorithm>
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

	void processMessage(json data, PeerContext* peer) {
		std::string action = (data.count("action") > 0) ? data["action"] : "";

		if (action == "announce") {
			std::string event = (data.count("event") > 0) ? data["event"] : "";

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

	void disconnectPeer(PeerContext* peer) {
		// Peer didn't send any id
		if (peer->id.empty()) {
			return;
		}

		for (std::string infoHash : peer->infoHashes) {
			std::map<std::string, Swarm*>::iterator it = this->swarms.find(infoHash);

			if (it == this->swarms.end()) {
				continue;
			}

			Swarm* swarm = this->swarms[infoHash];
			swarm->removePeer(peer);
			peer->infoHashes.erase(infoHash);

			if (swarm->getPeersCount() == 0) {
				this->swarms.erase(infoHash);

				// Free up space of swarm here
				delete swarm;
			}
		}

		this->peers.erase(peer->id);
	}

private:

	 void processAnnounce(json data, PeerContext* peer, bool completed = false) {
		// Validate params
		if (data.count("peer_id") == 0 || !data["peer_id"].is_string()) {
			throw TrackerException("announce: peer_id field is missing or wrong");
		}
		if (data.count("info_hash") == 0 || !data["info_hash"].is_string()) {
			throw TrackerException("announce: info_hash field is missing or wrong");
		}

		std::string infoHash = data["info_hash"];
		std::string peerId = data["peer_id"];
		Swarm* swarm = NULL;

		if (peer->id.empty()) {
			// New socket with no peer attached
			peer->id = peerId;

			// Peer has changed of socket
			if (this->peers.find(peerId) != this->peers.end()) {
				this->disconnectPeer(this->peers[peerId]);
			}

			// Add to peers
			this->peers[peerId] = peer;
		} else if (peer->id != peerId) {
			throw TrackerException("announce: different peer_id on the same connection");
		} else {
			// Get swarm from "peer"
			std::set<std::string>::iterator it = peer->infoHashes.find(infoHash);
			if (it != peer->infoHashes.end()) {
				std::map<std::string, Swarm*>::iterator it = this->swarms.find(infoHash);
				if (it != this->swarms.end()) {
					swarm = it->second;
				}
			}
		}

		bool isPeerCompleted = (completed || (data.count("left") > 0 && data["left"].is_number() && data["left"] == 0));

		if (swarm == NULL) {
			// Not yet in swarm
			swarm = this->addPeerToSwarm(peer, infoHash, isPeerCompleted);
		} else if (swarm != NULL) {
			// Peer is already in swarm
			if (isPeerCompleted) {
				swarm->setCompleted(peer);
			}
		} else {
			throw TrackerException("announce: illegal info_hash field");
		}

		peer->sendMessage({
			{"action", "announce"},
			{"interval", this->announceInterval},
			{"info_hash", infoHash},
			{"complete", swarm->getCompletedCount()},
			{"incomplete", swarm->getPeersCount() - swarm->getCompletedCount()},
		});

		this->sendOffersToPeers(data, swarm->getPeers(), peer, infoHash);
	}

	void processAnswer(json data, PeerContext* peer) {
		// Validate params
		if (data.count("to_peer_id") == 0 || !data["to_peer_id"].is_string()) {
			throw TrackerException("announce: to_peer_id field is missing or wrong");
		}

		std::string toPeerId = data["to_peer_id"];
		std::map<std::string, PeerContext*>::iterator it = this->peers.find(toPeerId);
		if (it == this->peers.end()) {
			throw TrackerException("answer: to_peer_id is not in the swarm");
		}

		PeerContext* toPeer = it->second;
		data["peer_id"] = peer->id;
		data.erase("to_peer_id");
		toPeer->sendMessage(data);
	}

	void processStop(json data, PeerContext* peer) {
		// Validate params
		if (data.count("info_hash") == 0 || !data["info_hash"].is_string()) {
			throw TrackerException("announce: info_hash field is missing or wrong");
		}
		std::string infoHash = data["info_hash"];

		std::map<std::string, Swarm*>::iterator it = this->swarms.find(infoHash);
		if (it == this->swarms.end()) {
			return;
		}

		Swarm* swarm = it->second;

		swarm->removePeer(peer);

		peer->infoHashes.erase(infoHash)

		if (swarm->getPeersCount() == 0) {
			this->swarms.erase(infoHash);
		}
	}

	void processScrape(json data, PeerContext* peer) {
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
			for (json::iterator it = data["info_hash"].begin(); it != data["info_hash"].end(); it++) {
				if ((*it).is_string()) {
					std::string singleInfoHash = *it;
					std::map<std::string, Swarm*>::iterator swarmIt = this->swarms.find(singleInfoHash);
					if (swarmIt != this->swarms.end()) {
						Swarm* swarm = swarmIt->second;
						files[singleInfoHash] = {
							{"complete", swarm->getCompletedCount()},
							{"incomplete", swarm->getPeersCount() - swarm->getCompletedCount()},
							{"downloaded", swarm->getCompletedCount()},
						};
					} else {
						files[singleInfoHash] = {
							{"complete", 0},
							{"incomplete", 0},
							{"downloaded", 0},
						};
					}
				}
			}
		} else if (data["info_hash"].is_string()) {
			std::string infoHash = data["info_hash"];
			std::map<std::string, Swarm*>::iterator it = this->swarms.find(infoHash);
			if (it != this->swarms.end()) {
				Swarm* swarm = it->second;
				files[infoHash] = {
					{"complete", swarm->getCompletedCount()},
					{"incomplete", swarm->getPeersCount() - swarm->getCompletedCount()},
					{"downloaded", swarm->getCompletedCount()},
				};
			} else {
				files[infoHash] =  {
					{"complete", 0},
					{"incomplete", 0},
					{"downloaded", 0},
				};
			}
		}

		peer->sendMessage({
			{"action", "scrape"},
			{"files", files}
		});
	}

	Swarm* addPeerToSwarm(PeerContext* peer, std::string infoHash, bool completed) {
		std::map<std::string, Swarm*>::iterator it = this->swarms.find(infoHash);
		Swarm* swarm;

		if (it == this->swarms.end()) {
			swarm = new Swarm(infoHash);
			this->swarms[infoHash] = swarm;
		} else {
			swarm = it->second;
		}

		swarm->addPeer(peer, completed);

		peer->infoHashes.insert(infoHash);

		return swarm;
	}

	void sendOffersToPeers(json data, std::vector<PeerContext*> peers, PeerContext* peer, std::string infoHash) {
		// Check if there are other peeers
		if (peers.size() <= 1) {
			return;
		}

		// Offers
		if (data.count("offers") == 0) {
			return;
		} else if (!data["offers"].is_array()) {
			throw TrackerException("announce: offers field is not an array");
		}
		json offers = data["offers"];

		// Numwant
		if (data.count("numwant") == 0 || !data["numwant"].is_number()) {
			return;
		}
		int numwant = data["numwant"];

		int countPeersToSend = peers.size() - 1;
		int countOffersToSend = std::min(countPeersToSend, std::min((int) offers.size(), std::min(this->maxOffers, numwant)));

		if (countOffersToSend == countPeersToSend) {
			// We have offers for all the peers from the swarm - send offers to all
			int i = 0;
			for (const auto& toPeer : peers) {
				if (toPeer != peer) {
					this->sendOffer(offers[i], peer->id, toPeer, infoHash);
					i++;
				}
			}
		} else {
			// Send offers to random peers
			srand((unsigned int) time (NULL));
			int peerIndex = rand() % peers.size();

			for (int i = 0; i < countOffersToSend; i++) {
				PeerContext* toPeer = peers[peerIndex];

				if (toPeer == peer) {
					// Do one more iteration
					i--;
				} else {
					this->sendOffer(offers[i], peer->id, toPeer, infoHash);
				}

				peerIndex++;
				if (peerIndex == peers.size()) {
					peerIndex = 0;
				}
			}
		}
	}

	void sendOffer(json offerItem, std::string fromPeerId, PeerContext* toPeer, std::string infoHash) {
		// Validate params
		if (offerItem.count("offer") == 0 || offerItem.count("offer_id") == 0) {
			throw TrackerException("announce: wrong offer item format");
		}

		json offer = offerItem["offer"];
		json offerId = offerItem["offer_id"];

		if (offer.count("sdp") == 0) {
			 throw TrackerException("announce: wrong offer item field format");
		}

		toPeer->sendMessage({
			{"action", "announce"},
			{"info_hash", infoHash},
			{"offer_id", offerId}, // offerId is not validated to be a string
			{"peer_id", fromPeerId},
			{"offer", {
				{"type", "offer"},
				{"sdp", offer["sdp"]}, // offer.sdp is not validated to be a string
			}}
		});
	}

};

};

#endif // OWT_FASTTRACKER_H
