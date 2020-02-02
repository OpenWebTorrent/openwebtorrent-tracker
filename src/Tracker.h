#ifndef OWT_TRACKER_H
#define OWT_TRACKER_H

#include <string>
#include <map>
#include <vector>
#include <json.hpp>

#include "Swarm.h"
#include "PeerContext.h"

using namespace nlohmann;

namespace owt {

class Tracker {

public:
	virtual void processMessage(json data, PeerContext* peer) = 0;
	virtual void disconnectPeer(PeerContext* peer) = 0;

	virtual std::map<std::string, Swarm*> getSwarms() = 0;

};

};

#endif // OWT_TRACKER_H
