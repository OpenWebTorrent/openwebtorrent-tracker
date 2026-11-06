#ifndef OWT_TRACKER_H
#define OWT_TRACKER_H

#include <cstdint>
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
	// Each reply's interval is drawn from announceInterval ± this fraction, so
	// clients that connected at the same moment (e.g. after a restart) don't
	// keep announcing in lockstep. Capped at 0.5: the longest interval
	// (1.5x) must stay below the 2x expiry timeout.
	double announceIntervalJitter = 0.1;
	// Maximum memberships (peer ID + torrent) per connection (0 = unlimited)
	size_t maxSwarmsPerPeer = 1000;
	// Scrape without info_hash returns every torrent: expensive on big trackers
	bool allowFullScrape = false;
	// Maximum number of info hashes in a single scrape request
	size_t maxScrapeInfoHashes = 100;
};

// Requests accepted only because the tracker is more lenient than the
// WebTorrent protocol (see "Deviations from the WebTorrent protocol" in the
// README). Cumulative counts.
struct CompatCounters {
	// Announce, answer or stop whose info_hash, peer_id or to_peer_id isn't a
	// 20-byte binary string
	uint64_t nonStandardIds = 0;
	// Announce with an unknown event, handled as a regular update
	uint64_t unknownEvents = 0;
	// Answer from a peer ID that hadn't announced that torrent on its
	// connection: the peer was added to the swarm
	uint64_t answersWithoutAnnounce = 0;
};

class Tracker {

public:
	using SwarmMap = std::unordered_map<std::string, std::unique_ptr<Swarm>>;

	virtual ~Tracker() = default;

	virtual void processMessage(const json& data, PeerContext* peer) = 0;
	virtual void disconnectPeer(PeerContext* peer) = 0;

	virtual const SwarmMap& getSwarms() const = 0;
	// Distinct peer IDs currently in at least one swarm
	virtual size_t getPeerIdCount() const { return 0; }

	// Totals over all swarms. The defaults walk every swarm; FastTracker keeps
	// running counters so these are O(1) (they're read under the server lock).
	virtual size_t getMembershipCount() const {
		size_t count = 0;
		for (const auto& entry : this->getSwarms()) count += entry.second->getPeersCount();
		return count;
	}
	virtual size_t getSeederCount() const {
		size_t count = 0;
		for (const auto& entry : this->getSwarms()) count += entry.second->getCompletedCount();
		return count;
	}

	// Called periodically: removes memberships that stopped announcing
	virtual void cleanupStalePeers() {}
	// How often cleanupStalePeers should run, in milliseconds (0 = never)
	virtual unsigned int getCleanupPeriodMs() const { return 0; }
	// One slice of an incremental cleanup: a full sweep takes `slices` calls.
	// Lets a multi-threaded server release its lock between slices instead of
	// stalling every thread for one long scan.
	virtual void cleanupStalePeersStep(unsigned int slices) { this->cleanupStalePeers(); }

	// Tracker state relevant to a rejected request, for debugging. Called
	// under the server lock, only for sampled failures.
	virtual json diagnose(const json& data, const PeerContext* peer) const { return json::object(); }

	virtual CompatCounters getCompatCounters() const { return {}; }

};

};

#endif // OWT_TRACKER_H
