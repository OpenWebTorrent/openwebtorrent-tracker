// Unit tests for the tracker logic. Peers are fake sockets that record
// every message the tracker sends them.

#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "FastTracker.h"

using namespace owt;

static int failures = 0;

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond << std::endl; \
			failures++; \
		} \
	} while (0)

struct FakePeer {
	PeerContext ctx;
	std::vector<json> inbox;

	FakePeer() {
		ctx.socket = this;
		ctx.sendFn = [](void* socket, std::string_view message) {
			static_cast<FakePeer*>(socket)->inbox.push_back(json::parse(message));
		};
	}
};

static const std::string IH1(20, 'a');
static const std::string IH2(20, 'b');

static std::string peerId(char c) {
	return std::string(20, c);
}

static json announce(const std::string& infoHash, const std::string& id, const std::string& event = "started", int numOffers = 0) {
	json offers = json::array();
	for (int i = 0; i < numOffers; i++) {
		offers.push_back({{"offer_id", "o" + std::to_string(i)}, {"offer", {{"type", "offer"}, {"sdp", "sdp" + std::to_string(i)}}}});
	}
	json data = {{"action", "announce"}, {"info_hash", infoHash}, {"peer_id", id}, {"numwant", numOffers}, {"offers", offers}};
	if (!event.empty()) {
		data["event"] = event;
	}
	return data;
}

static bool throwsTrackerException(FastTracker& tracker, const json& data, PeerContext* peer) {
	try {
		tracker.processMessage(data, peer);
	} catch (const TrackerException&) {
		return true;
	}
	return false;
}

static size_t countOffers(const FakePeer& peer) {
	size_t count = 0;
	for (const auto& m : peer.inbox) {
		count += m.contains("offer");
	}
	return count;
}

static void testMalformedInputNeverCrashes() {
	FastTracker tracker;
	FakePeer p;

	// Each of these used to escape as a nlohmann exception and terminate the process
	std::vector<json> bad = {
		json::array(),
		json(5),
		{{"action", 5}},
		{{"action", "announce"}, {"event", 5}},
		{{"action", "announce"}, {"info_hash", 5}, {"peer_id", peerId('a')}},
		{{"action", "announce"}, {"info_hash", "short"}, {"peer_id", peerId('a')}},
		{{"action", "announce"}, {"info_hash", IH1}, {"peer_id", std::string(10000, 'x')}},
		{{"action", "announce"}, {"info_hash", IH1}, {"peer_id", peerId('a')}, {"offers", "nope"}},
		{{"action", "announce"}, {"info_hash", IH1}, {"peer_id", peerId('a')}, {"answer", 1}, {"to_peer_id", peerId('b')}},
		{{"action", "announce"}, {"event", "stopped"}},
		{{"action", "scrape"}, {"info_hash", 5}},
		{{"action", "unknown"}},
	};
	for (const auto& data : bad) {
		CHECK(throwsTrackerException(tracker, data, &p.ctx));
	}

	// Garbage offers in a swarm with other peers
	FakePeer other;
	tracker.processMessage(announce(IH1, peerId('o')), &other.ctx);
	json data = announce(IH1, peerId('p'), "started");
	data["offers"] = json::array({1, json::object(), {{"offer_id", 1}, {"offer", {{"sdp", "x"}}}}, {{"offer_id", "x"}, {"offer", "x"}}});
	data["numwant"] = 1e300;
	CHECK(throwsTrackerException(tracker, data, &p.ctx));
	CHECK(countOffers(other) == 0);
}

static void testAnnounceResponse() {
	FastTracker tracker(20, 120);
	FakePeer a, b;

	tracker.processMessage(announce(IH1, peerId('a')), &a.ctx);
	CHECK(a.inbox.size() == 1);
	CHECK(a.inbox[0]["action"] == "announce");
	CHECK(a.inbox[0]["interval"] == 120);
	CHECK(a.inbox[0]["info_hash"] == IH1);
	CHECK(a.inbox[0]["complete"] == 0);
	CHECK(a.inbox[0]["incomplete"] == 1);

	json seed = announce(IH1, peerId('b'));
	seed["left"] = 0;
	tracker.processMessage(seed, &b.ctx);
	CHECK(b.inbox.back()["complete"] == 1);
	CHECK(b.inbox.back()["incomplete"] == 1);

	// Re-announcing does not duplicate the peer
	tracker.processMessage(announce(IH1, peerId('a'), ""), &a.ctx);
	CHECK(tracker.getSwarms().at(IH1)->getPeersCount() == 2);

	// Changing peer_id on the same connection is rejected
	CHECK(throwsTrackerException(tracker, announce(IH1, peerId('z')), &a.ctx));
}

static void testOffersAreDistributed() {
	FastTracker tracker(20);
	std::vector<std::unique_ptr<FakePeer>> peers;
	for (int i = 0; i < 10; i++) {
		peers.push_back(std::make_unique<FakePeer>());
		tracker.processMessage(announce(IH1, peerId('A' + i)), &peers.back()->ctx);
	}

	// Joining peer sends 4 offers: exactly 4 distinct other peers get one each
	FakePeer joiner;
	tracker.processMessage(announce(IH1, peerId('z'), "started", 4), &joiner.ctx);
	size_t total = 0;
	std::set<std::string> offerIds;
	for (const auto& p : peers) {
		CHECK(countOffers(*p) <= 1);
		total += countOffers(*p);
		for (const auto& m : p->inbox) {
			if (m.contains("offer")) {
				CHECK(m["peer_id"] == peerId('z'));
				CHECK(m["info_hash"] == IH1);
				CHECK(m["offer"]["type"] == "offer");
				offerIds.insert(m["offer_id"].get<std::string>());
			}
		}
	}
	CHECK(total == 4);
	CHECK(offerIds.size() == 4);
	CHECK(countOffers(joiner) == 0);

	// numwant limits the offers, and missing numwant still forwards offers
	for (auto& p : peers) p->inbox.clear();
	json data = announce(IH1, peerId('z'), "", 6);
	data["numwant"] = 2;
	tracker.processMessage(data, &joiner.ctx);
	total = 0;
	for (const auto& p : peers) total += countOffers(*p);
	CHECK(total == 2);

	for (auto& p : peers) p->inbox.clear();
	data.erase("numwant");
	tracker.processMessage(data, &joiner.ctx);
	total = 0;
	for (const auto& p : peers) total += countOffers(*p);
	CHECK(total == 6);

	// More offers than peers: every other peer gets exactly one
	for (auto& p : peers) p->inbox.clear();
	tracker.processMessage(announce(IH1, peerId('z'), "", 15), &joiner.ctx);
	for (const auto& p : peers) CHECK(countOffers(*p) == 1);
	CHECK(countOffers(joiner) == 0);
}

static void testRandomPeersAreUniform() {
	Swarm swarm(IH1);
	std::vector<PeerContext> ctxs(10);
	for (auto& c : ctxs) swarm.addPeer(&c, false);

	std::mt19937 rng(42);
	std::map<PeerContext*, int> hits;
	const int rounds = 20000;
	for (int i = 0; i < rounds; i++) {
		auto picked = swarm.getRandomPeers(3, &ctxs[4], rng);
		CHECK(picked.size() == 3);
		std::set<PeerContext*> unique(picked.begin(), picked.end());
		CHECK(unique.size() == 3);
		for (auto* p : picked) hits[p]++;
	}
	CHECK(hits.count(&ctxs[4]) == 0);
	// Expected hits per peer: rounds * 3 / 9 ~= 6667
	for (const auto& entry : hits) {
		CHECK(entry.second > 6000 && entry.second < 7300);
	}
}

static void testAnswerIsRouted() {
	FastTracker tracker;
	FakePeer a, b;
	tracker.processMessage(announce(IH1, peerId('a')), &a.ctx);
	tracker.processMessage(announce(IH1, peerId('b'), "started", 1), &b.ctx);
	CHECK(countOffers(a) == 1);

	json answer = {
		{"action", "announce"}, {"info_hash", IH1}, {"peer_id", peerId('a')},
		{"to_peer_id", peerId('b')}, {"offer_id", "o0"},
		{"answer", {{"type", "answer"}, {"sdp", "answer-sdp"}}}, {"extra", "not forwarded"}
	};
	tracker.processMessage(answer, &a.ctx);
	const json& received = b.inbox.back();
	CHECK(received["answer"]["sdp"] == "answer-sdp");
	CHECK(received["offer_id"] == "o0");
	CHECK(received["peer_id"] == peerId('a'));
	CHECK(received["info_hash"] == IH1);
	CHECK(!received.contains("to_peer_id"));
	CHECK(!received.contains("extra"));

	// Target left: the answer is dropped without error (used to close the answerer's socket)
	tracker.disconnectPeer(&b.ctx);
	size_t before = b.inbox.size();
	CHECK(!throwsTrackerException(tracker, answer, &a.ctx));
	CHECK(b.inbox.size() == before);

	// Target not in the given swarm
	FakePeer c;
	tracker.processMessage(announce(IH2, peerId('c')), &c.ctx);
	answer["to_peer_id"] = peerId('c');
	before = c.inbox.size();
	tracker.processMessage(answer, &a.ctx);
	CHECK(c.inbox.size() == before);
}

static void testReconnectWithSamePeerId() {
	FastTracker tracker;
	auto a1 = std::make_unique<FakePeer>();
	FakePeer a2, b;

	tracker.processMessage(announce(IH1, peerId('a')), &a1->ctx);
	tracker.processMessage(announce(IH1, peerId('a')), &a2.ctx);
	CHECK(tracker.getSwarms().at(IH1)->getPeersCount() == 1);
	CHECK(tracker.getSwarms().at(IH1)->hasPeer(&a2.ctx));

	// Old socket closes afterwards: must not unregister the new connection
	tracker.disconnectPeer(&a1->ctx);
	a1.reset();

	tracker.processMessage(announce(IH1, peerId('b'), "started", 1), &b.ctx);
	CHECK(countOffers(a2) == 1);
	tracker.processMessage({
		{"action", "announce"}, {"info_hash", IH1}, {"peer_id", peerId('a')}, {"to_peer_id", peerId('b')},
		{"offer_id", "o0"}, {"answer", {{"type", "answer"}, {"sdp", "x"}}}
	}, &a2.ctx);
	CHECK(b.inbox.back().contains("answer"));

	// b answers a: reaches a2
	size_t before = a2.inbox.size();
	tracker.processMessage({
		{"action", "announce"}, {"info_hash", IH1}, {"peer_id", peerId('b')}, {"to_peer_id", peerId('a')},
		{"offer_id", "o1"}, {"answer", {{"type", "answer"}, {"sdp", "y"}}}
	}, &b.ctx);
	CHECK(a2.inbox.size() == before + 1);
}

static void testStopAndDisconnectCleanUp() {
	FastTracker tracker;
	FakePeer a, b;

	tracker.processMessage(announce(IH1, peerId('a')), &a.ctx);
	tracker.processMessage(announce(IH2, peerId('a')), &a.ctx);
	tracker.processMessage(announce(IH1, peerId('b')), &b.ctx);
	CHECK(tracker.getSwarms().size() == 2);

	// Stop the only peer of IH2: swarm is freed (it used to leak)
	tracker.processMessage(announce(IH2, peerId('a'), "stopped"), &a.ctx);
	CHECK(tracker.getSwarms().count(IH2) == 0);
	CHECK(a.ctx.infoHashes.count(IH2) == 0);

	// Stopping a torrent the peer never joined does nothing
	tracker.processMessage(announce(IH2, peerId('a'), "stopped"), &a.ctx);
	FakePeer c;
	tracker.processMessage(announce(IH1, peerId('c'), "stopped"), &c.ctx);
	CHECK(tracker.getSwarms().at(IH1)->getPeersCount() == 2);

	tracker.disconnectPeer(&a.ctx);
	CHECK(tracker.getSwarms().at(IH1)->getPeersCount() == 1);
	CHECK(a.ctx.id.empty());
	tracker.disconnectPeer(&b.ctx);
	CHECK(tracker.getSwarms().empty());
}

static void testCompletedAndScrape() {
	TrackerOptions options;
	options.allowFullScrape = true;
	FastTracker tracker(options);
	FakePeer a, b, s;

	tracker.processMessage(announce(IH1, peerId('a')), &a.ctx);
	tracker.processMessage(announce(IH1, peerId('b')), &b.ctx);
	json seed = announce(IH1, peerId('s'));
	seed["left"] = 0;
	tracker.processMessage(seed, &s.ctx);

	tracker.processMessage(announce(IH1, peerId('a'), "completed"), &a.ctx);
	tracker.processMessage(announce(IH1, peerId('a'), "completed"), &a.ctx); // duplicate doesn't count twice

	tracker.processMessage({{"action", "scrape"}, {"info_hash", IH1}}, &b.ctx);
	json file = b.inbox.back()["files"][IH1];
	CHECK(file["complete"] == 2);
	CHECK(file["incomplete"] == 1);
	CHECK(file["downloaded"] == 1);

	tracker.processMessage({{"action", "scrape"}, {"info_hash", {IH1, IH2, 7}}}, &b.ctx);
	CHECK(b.inbox.back()["files"].size() == 2);
	CHECK(b.inbox.back()["files"][IH2]["complete"] == 0);

	tracker.processMessage({{"action", "scrape"}}, &b.ctx);
	CHECK(b.inbox.back()["files"].size() == 1);

	// Seeder disconnecting keeps counters consistent
	tracker.disconnectPeer(&a.ctx);
	tracker.processMessage({{"action", "scrape"}, {"info_hash", IH1}}, &b.ctx);
	CHECK(b.inbox.back()["files"][IH1]["complete"] == 1);
	CHECK(b.inbox.back()["files"][IH1]["incomplete"] == 1);
}

static void testBinaryStrings() {
	// Info hashes are 20-byte binary strings, bytes >= 0x80 arrive as 2-byte UTF-8
	std::string binary;
	for (int i = 0; i < 20; i++) binary += "\xC3\xBF";
	CHECK(Utils::binaryStringLength(binary) == 20);
	CHECK(Utils::binaryStringLength(std::string(20, 'a')) == 20);
	CHECK(Utils::binaryStringLength("\xE2\x82\xAC") == -1); // U+20AC is not a byte

	FastTracker tracker;
	FakePeer a;
	tracker.processMessage(announce(binary, peerId('a')), &a.ctx);
	CHECK(tracker.getSwarms().count(binary) == 1);
}

static void testLimits() {
	TrackerOptions options;
	options.maxSwarmsPerPeer = 3;
	options.maxScrapeInfoHashes = 2;
	FastTracker tracker(options);
	FakePeer a;

	for (char c = 'a'; c < 'd'; c++) {
		tracker.processMessage(announce(std::string(20, c), peerId('a')), &a.ctx);
	}
	CHECK(throwsTrackerException(tracker, announce(std::string(20, 'd'), peerId('a')), &a.ctx));
	CHECK(tracker.getSwarms().count(std::string(20, 'd')) == 0);

	// Re-announcing a joined torrent is still fine, and stopping frees a slot
	tracker.processMessage(announce(std::string(20, 'a'), peerId('a'), ""), &a.ctx);
	tracker.processMessage(announce(std::string(20, 'a'), peerId('a'), "stopped"), &a.ctx);
	tracker.processMessage(announce(std::string(20, 'd'), peerId('a')), &a.ctx);
	CHECK(a.ctx.infoHashes.size() == 3);

	// Full scrape is disabled by default, scrape lists are capped
	CHECK(throwsTrackerException(tracker, {{"action", "scrape"}}, &a.ctx));
	CHECK(throwsTrackerException(tracker, {{"action", "scrape"}, {"info_hash", {IH1, IH2, IH1}}}, &a.ctx));
	CHECK(!throwsTrackerException(tracker, {{"action", "scrape"}, {"info_hash", {IH1, IH2}}}, &a.ctx));
}

static void testTokenBucket() {
	using Clock = Utils::TokenBucket::Clock;
	Clock::time_point t0 = Clock::now();
	Utils::TokenBucket bucket(10, 5, t0);

	for (int i = 0; i < 5; i++) CHECK(bucket.consume(t0));
	CHECK(!bucket.consume(t0));
	// 10 msg/s: one token every 100ms
	CHECK(bucket.consume(t0 + std::chrono::milliseconds(100)));
	CHECK(!bucket.consume(t0 + std::chrono::milliseconds(100)));
	// Refill is capped at the burst size
	Clock::time_point t1 = t0 + std::chrono::seconds(60);
	for (int i = 0; i < 5; i++) CHECK(bucket.consume(t1));
	CHECK(!bucket.consume(t1));

	Utils::TokenBucket unlimited(0, 0, t0);
	for (int i = 0; i < 1000; i++) CHECK(unlimited.consume(t0));
}

static void testAddressKey() {
	CHECK(Utils::addressKey(std::string("\x7f\x00\x00\x01", 4)) == "127.0.0.1");
	CHECK(Utils::addressKeyFromText("10.1.2.3") == "10.1.2.3");
	// IPv4-mapped IPv6 is the same client as plain IPv4
	CHECK(Utils::addressKeyFromText("::ffff:10.1.2.3") == "10.1.2.3");
	// IPv6 addresses in the same /64 share a key
	CHECK(Utils::addressKeyFromText("2001:db8:1:2::1") == "2001:db8:1:2::/64");
	CHECK(Utils::addressKeyFromText("2001:db8:1:2:ffff:ffff:ffff:ffff") == "2001:db8:1:2::/64");
	CHECK(Utils::addressKeyFromText("2001:db8:1:3::1") != "2001:db8:1:2::/64");
	CHECK(Utils::addressKeyFromText("not an ip").empty());
	CHECK(Utils::addressKeyFromText("").empty());
}

int main() {
	testMalformedInputNeverCrashes();
	testAnnounceResponse();
	testOffersAreDistributed();
	testRandomPeersAreUniform();
	testAnswerIsRouted();
	testReconnectWithSamePeerId();
	testStopAndDisconnectCleanUp();
	testCompletedAndScrape();
	testBinaryStrings();
	testLimits();
	testTokenBucket();
	testAddressKey();

	if (failures) {
		std::cerr << failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All tests passed" << std::endl;
	return 0;
}
