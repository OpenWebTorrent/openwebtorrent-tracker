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

	// A second peer ID on the same connection is a separate member
	tracker.processMessage(announce(IH1, peerId('z')), &a.ctx);
	CHECK(tracker.getSwarms().at(IH1)->getPeersCount() == 3);
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
	PeerContext ctx;
	for (char c = 'a'; c < 'a' + 10; c++) swarm.addPeer(&ctx, peerId(c));

	std::mt19937 rng(42);
	std::map<std::string, int> hits;
	const int rounds = 20000;
	for (int i = 0; i < rounds; i++) {
		auto picked = swarm.getRandomPeers(3, peerId('e'), rng);
		CHECK(picked.size() == 3);
		std::set<std::string> unique;
		for (auto* p : picked) unique.insert(p->peerId);
		CHECK(unique.size() == 3);
		for (auto* p : picked) hits[p->peerId]++;
	}
	CHECK(hits.count(peerId('e')) == 0);
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
	CHECK(tracker.getSwarms().at(IH1)->findPeer(peerId('a'))->connection == &a2.ctx);
	CHECK(a1->ctx.memberships.empty());

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
	CHECK(!a.ctx.hasMembership(peerId('a'), IH2));

	// Stopping a torrent the peer never joined does nothing
	tracker.processMessage(announce(IH2, peerId('a'), "stopped"), &a.ctx);
	FakePeer c;
	tracker.processMessage(announce(IH1, peerId('c'), "stopped"), &c.ctx);
	CHECK(tracker.getSwarms().at(IH1)->getPeersCount() == 2);

	tracker.disconnectPeer(&a.ctx);
	CHECK(tracker.getSwarms().at(IH1)->getPeersCount() == 1);
	CHECK(a.ctx.memberships.empty());
	CHECK(a.ctx.membershipCount == 0);
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
	CHECK(a.ctx.membershipCount == 3);

	// The limit counts memberships across all peer IDs of a connection
	CHECK(throwsTrackerException(tracker, announce(std::string(20, 'e'), peerId('b')), &a.ctx));
	CHECK(a.ctx.memberships.count(peerId('b')) == 0);

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

static json answerMessage(const std::string& infoHash, const std::string& from, const std::string& to, const std::string& sdp) {
	return {
		{"action", "announce"}, {"info_hash", infoHash}, {"peer_id", from}, {"to_peer_id", to},
		{"offer_id", "o0"}, {"answer", {{"type", "answer"}, {"sdp", sdp}}}
	};
}

static size_t answersWithSdp(const FakePeer& peer, const std::string& sdp) {
	size_t count = 0;
	for (const auto& m : peer.inbox) {
		count += m.contains("answer") && m["answer"]["sdp"] == sdp;
	}
	return count;
}

static size_t incomplete(FastTracker& tracker, const std::string& infoHash) {
	auto it = tracker.getSwarms().find(infoHash);
	return it == tracker.getSwarms().end() ? 0 : it->second->getIncompleteCount();
}

// libtorrent shares one connection between torrents with different peer IDs (#17)
static void testMultiplePeerIdsPerConnection() {
	FastTracker tracker;
	FakePeer shared, b, c;

	tracker.processMessage(announce(IH1, peerId('x')), &shared.ctx);
	tracker.processMessage(announce(IH2, peerId('y')), &shared.ctx);
	CHECK(shared.inbox.size() == 2);
	CHECK(shared.ctx.memberships.size() == 2);

	// Offers and answers route by peer ID
	tracker.processMessage(announce(IH1, peerId('b'), "started", 1), &b.ctx);
	CHECK(countOffers(shared) == 1);
	tracker.processMessage(answerMessage(IH1, peerId('x'), peerId('b'), "s1"), &shared.ctx);
	CHECK(answersWithSdp(b, "s1") == 1);

	// The answer must come from a peer ID this connection announced in that swarm
	CHECK(throwsTrackerException(tracker, answerMessage(IH1, peerId('y'), peerId('b'), "s2"), &shared.ctx));
	CHECK(throwsTrackerException(tracker, answerMessage(IH1, peerId('x'), peerId('b'), "s2"), &c.ctx));
	CHECK(answersWithSdp(b, "s2") == 0);

	// Target left: dropped without error, the shared connection keeps its other torrents
	tracker.disconnectPeer(&b.ctx);
	CHECK(!throwsTrackerException(tracker, answerMessage(IH1, peerId('x'), peerId('b'), "s3"), &shared.ctx));
	CHECK(incomplete(tracker, IH2) == 1);

	// Two peer IDs of one connection in the same swarm are distinct members
	tracker.processMessage(announce(IH2, peerId('z')), &shared.ctx);
	CHECK(incomplete(tracker, IH2) == 2);
	// They are different peers, so they get offers from each other
	tracker.processMessage(announce(IH2, peerId('z'), "", 1), &shared.ctx);
	CHECK(countOffers(shared) == 2);
	CHECK(shared.inbox.back()["peer_id"] == peerId('z'));

	// Stopping one identity keeps the other
	tracker.processMessage(announce(IH2, peerId('z'), "stopped"), &shared.ctx);
	CHECK(incomplete(tracker, IH2) == 1);
	CHECK(shared.ctx.memberships.count(peerId('z')) == 0);

	// Another connection cannot stop a membership it doesn't own
	tracker.processMessage(announce(IH2, peerId('y'), "stopped"), &c.ctx);
	CHECK(incomplete(tracker, IH2) == 1);

	// Takeover moves one peer ID with all its memberships, nothing else
	tracker.processMessage(announce(IH1, peerId('y')), &shared.ctx);
	tracker.processMessage(announce(IH2, peerId('y')), &c.ctx);
	CHECK(incomplete(tracker, IH1) == 1);  // only x left in IH1
	CHECK(incomplete(tracker, IH2) == 1);  // y, now on c
	CHECK(tracker.getSwarms().at(IH2)->findPeer(peerId('y'))->connection == &c.ctx);
	CHECK(shared.ctx.memberships.count(peerId('y')) == 0);
	CHECK(shared.ctx.membershipCount == 1);

	// The old connection closing doesn't affect the moved peer ID
	tracker.disconnectPeer(&shared.ctx);
	CHECK(tracker.getSwarms().count(IH1) == 0);
	CHECK(incomplete(tracker, IH2) == 1);
	tracker.disconnectPeer(&c.ctx);
	CHECK(tracker.getSwarms().empty());
}

static void testPausedPartialSeeds() {
	TrackerOptions options;
	options.allowFullScrape = true;
	FastTracker tracker(options);
	FakePeer seed, partial, observer;

	json seedAnnounce = announce(IH1, peerId('s'));
	seedAnnounce["left"] = 0;
	tracker.processMessage(seedAnnounce, &seed.ctx);

	// BEP 21: a partial seed announces with event=paused, even the first time
	tracker.processMessage(announce(IH1, peerId('p'), "paused", 1), &partial.ctx);
	CHECK(partial.inbox.back()["complete"] == 1);
	CHECK(partial.inbox.back()["incomplete"] == 1);
	CHECK(countOffers(seed) == 1);

	auto scrapeFile = [&]() {
		tracker.processMessage({{"action", "scrape"}, {"info_hash", IH1}}, &observer.ctx);
		return observer.inbox.back()["files"][IH1];
	};
	CHECK(scrapeFile()["downloaders"] == 0);
	CHECK(scrapeFile()["incomplete"] == 1);

	// Any other announce means it is downloading again
	tracker.processMessage(announce(IH1, peerId('p'), ""), &partial.ctx);
	CHECK(scrapeFile()["downloaders"] == 1);
	tracker.processMessage(announce(IH1, peerId('p'), "paused"), &partial.ctx);
	CHECK(scrapeFile()["downloaders"] == 0);

	// An answer sent with event=paused is still an answer
	tracker.processMessage([&] {
		json a = answerMessage(IH1, peerId('p'), peerId('s'), "to-seed");
		a["event"] = "paused";
		return a;
	}(), &partial.ctx);
	CHECK(answersWithSdp(seed, "to-seed") == 1);

	// Paused -> completed counts one download, and seeders are never paused
	tracker.processMessage(announce(IH1, peerId('p'), "completed"), &partial.ctx);
	json file = scrapeFile();
	CHECK(file["complete"] == 2);
	CHECK(file["incomplete"] == 0);
	CHECK(file["downloaders"] == 0);
	CHECK(file["downloaded"] == 1);
	tracker.processMessage(announce(IH1, peerId('p'), "paused"), &partial.ctx);
	CHECK(scrapeFile()["complete"] == 2);

	// Unknown hashes and full scrape include the BEP 21 field
	tracker.processMessage({{"action", "scrape"}, {"info_hash", {IH2}}}, &observer.ctx);
	CHECK(observer.inbox.back()["files"][IH2]["downloaders"] == 0);
	tracker.processMessage({{"action", "scrape"}}, &observer.ctx);
	CHECK(observer.inbox.back()["files"][IH1].contains("downloaders"));

	// Stopping a paused peer clears its state
	tracker.processMessage(announce(IH2, peerId('q'), "paused"), &partial.ctx);
	tracker.processMessage(announce(IH2, peerId('q'), "stopped"), &partial.ctx);
	CHECK(tracker.getSwarms().count(IH2) == 0);
}

static void testStaleMembershipsExpire() {
	FastTracker tracker(20, 10);  // 10s interval: memberships expire after 20s
	FakePeer a, b;

	tracker.processMessage(announce(IH1, peerId('a')), &a.ctx);
	tracker.processMessage(announce(IH2, peerId('a')), &a.ctx);
	tracker.processMessage(announce(IH1, peerId('b')), &b.ctx);

	auto now = std::chrono::steady_clock::now();
	tracker.cleanupStalePeers(now + std::chrono::seconds(15));
	CHECK(tracker.getSwarms().size() == 2);

	// Refreshing one membership keeps it while the other expires
	auto& refreshed = a.ctx.memberships[peerId('a')][IH2];
	refreshed = now + std::chrono::seconds(15);
	tracker.cleanupStalePeers(now + std::chrono::seconds(25));
	CHECK(tracker.getSwarms().count(IH1) == 0);
	CHECK(incomplete(tracker, IH2) == 1);
	CHECK(a.ctx.membershipCount == 1);
	CHECK(b.ctx.memberships.empty());

	// A peer ID whose last membership expired is unregistered: re-announcing works
	tracker.cleanupStalePeers(now + std::chrono::seconds(40));
	CHECK(tracker.getSwarms().empty());
	CHECK(a.ctx.memberships.empty());
	tracker.processMessage(announce(IH1, peerId('a')), &a.ctx);
	CHECK(incomplete(tracker, IH1) == 1);

	CHECK(tracker.getCleanupPeriodMs() == 5000);
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
	testMultiplePeerIdsPerConnection();
	testPausedPartialSeeds();
	testStaleMembershipsExpire();

	if (failures) {
		std::cerr << failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All tests passed" << std::endl;
	return 0;
}
