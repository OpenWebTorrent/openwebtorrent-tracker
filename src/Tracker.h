#ifndef OWT_TRACKER_H
#define OWT_TRACKER_H

#include <memory>
#include <string>
#include <unordered_map>
#include <json.hpp>

#include "Swarm.h"
#include "PeerContext.h"

using namespace nlohmann;

namespace owt {

struct TrackerOptions {
	// Maximum number of offers relayed per announce
	int maxOffers = 20;
	// Announce interval sent to clients, in seconds
	int announceInterval = 120;
	// Maximum memberships (peer ID + torrent) per connection (0 = unlimited)
	size_t maxSwarmsPerPeer = 1000;
	// Scrape without info_hash returns every torrent: expensive on big trackers
	bool allowFullScrape = false;
	// Maximum number of info hashes in a single scrape request
	size_t maxScrapeInfoHashes = 100;
};

class Tracker {

public:
	using SwarmMap = std::unordered_map<std::string, std::unique_ptr<Swarm>>;

	virtual ~Tracker() = default;

	virtual void processMessage(const json& data, PeerContext* peer) = 0;
	virtual void disconnectPeer(PeerContext* peer) = 0;

	virtual const SwarmMap& getSwarms() const = 0;

	// Called periodically: removes memberships that stopped announcing
	virtual void cleanupStalePeers() {}
	// How often cleanupStalePeers should run, in milliseconds (0 = never)
	virtual unsigned int getCleanupPeriodMs() const { return 0; }

};

};

#endif // OWT_TRACKER_H
