// Unit tests for the tracker logic. Peers are fake sockets that record
// every message the tracker sends them.

#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "FastTracker.h"
#include "Metrics.h"
#include "FailureSamples.h"

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
	// Default jitter: 120 s ± 10%
	CHECK(a.inbox[0]["interval"] >= 108 && a.inbox[0]["interval"] <= 132);
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

	// A peer ID live on another connection can't answer from here
	CHECK(throwsTrackerException(tracker, answerMessage(IH1, peerId('x'), peerId('b'), "s2"), &c.ctx));
	CHECK(answersWithSdp(b, "s2") == 0);
	// A peer ID of this connection that isn't in that swarm joins it by
	// answering, like an announce (see testAnswerIsAnAnnounce)
	CHECK(!throwsTrackerException(tracker, answerMessage(IH1, peerId('y'), peerId('b'), "s2"), &shared.ctx));
	CHECK(answersWithSdp(b, "s2") == 1);
	CHECK(shared.ctx.hasMembership(peerId('y'), IH1));
	tracker.processMessage(announce(IH1, peerId('y'), "stopped"), &shared.ctx);

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

// Multi-threaded server: while an outbox is set, messages are queued with the
// target's connection id and owning loop instead of written to the socket.
static void testOutboxQueuesMessages() {
	FastTracker tracker;
	FakePeer a, b;
	a.ctx.id = 1; a.ctx.owner = (void*) 0x1;
	b.ctx.id = 2; b.ctx.owner = (void*) 0x2;

	tracker.processMessage(announce(IH1, peerId('a')), &a.ctx);
	a.inbox.clear();

	std::vector<PeerContext::Outgoing> out;
	PeerContext::outbox = &out;
	// b announces with an offer: b gets its reply, a gets b's offer.
	// Queued offers point into the request, which must outlive delivery.
	json request = announce(IH1, peerId('b'), "started", 1);
	tracker.processMessage(request, &b.ctx);
	PeerContext::outbox = nullptr;

	CHECK(a.inbox.empty());
	CHECK(b.inbox.empty());
	CHECK(out.size() == 2);
	bool toA = false, toB = false;
	for (const auto& m : out) {
		json frame = json::parse(m.frame());
		if (m.id == 1 && m.owner == (void*) 0x1 && frame.contains("offer")) toA = true;
		if (m.id == 2 && m.owner == (void*) 0x2 && frame.contains("interval")) toB = true;
	}
	CHECK(toA);
	CHECK(toB);

	// No outbox: sent directly, as before
	tracker.processMessage(announce(IH1, peerId('b'), "update"), &b.ctx);
	CHECK(b.inbox.size() == 1);

	// Closed connection (socket cleared): nothing queued
	b.ctx.socket = nullptr;
	PeerContext::outbox = &out;
	out.clear();
	b.ctx.sendMessage(json{{"x", 1}});
	PeerContext::outbox = nullptr;
	CHECK(out.empty());
}

// Offers and answers are written as frames without building a json object:
// the bytes must be identical to serializing the original object.
static void testRelayFramesMatchJson() {
	// Binary ids with invalid UTF-8, and an SDP with characters that need escaping
	std::string infoHash = std::string("\xff\xfe\x00\x01", 4) + std::string(16, 'h');
	std::string fromPeer = "\"quote\\back\x7f" + std::string(8, '\xc3');
	json offerItem = {{"offer_id", "id\n1"}, {"offer", {{"type", "offer"}, {"sdp", "v=0\r\no=- \"x\" \u00e9\t\x01"}}}};
	json expectedOffer = {
		{"action", "announce"}, {"info_hash", infoHash}, {"offer_id", offerItem["offer_id"]}, {"peer_id", fromPeer},
		{"offer", {{"type", "offer"}, {"sdp", offerItem["offer"]["sdp"]}}}
	};
	CHECK(PeerContext::offerFrame(offerItem, fromPeer, infoHash) == PeerContext::serialize(expectedOffer));

	json request = {{"answer", {{"type", "answer"}, {"sdp", "a=\\b\"c"}}}, {"offer_id", "o\u2028"}};
	json expectedAnswer = {
		{"action", "announce"}, {"answer", {{"type", "answer"}, {"sdp", request["answer"]["sdp"]}}},
		{"offer_id", request["offer_id"]}, {"peer_id", fromPeer}, {"info_hash", infoHash}
	};
	CHECK(PeerContext::answerFrame(request, fromPeer, infoHash) == PeerContext::serialize(expectedAnswer));
}

// Running totals must match a full walk after any sequence of operations
static void testCountersMatchWalk() {
	FastTracker tracker;
	std::vector<std::unique_ptr<FakePeer>> peers;
	for (int i = 0; i < 40; i++) peers.push_back(std::make_unique<FakePeer>());
	std::mt19937 rng(7);
	const std::string hashes[] = {std::string(20, 'p'), std::string(20, 'q'), std::string(20, 'r')};
	for (int step = 0; step < 4000; step++) {
		FakePeer& p = *peers[rng() % peers.size()];
		std::string id = peerId('A' + rng() % 26);
		const std::string& ih = hashes[rng() % 3];
		switch (rng() % 6) {
		case 0: { json m = announce(ih, id); m["left"] = 0; tracker.processMessage(m, &p.ctx); break; }
		case 1: tracker.processMessage(announce(ih, id, "completed"), &p.ctx); break;
		case 2: tracker.processMessage(announce(ih, id, "stopped"), &p.ctx); break;
		case 3: tracker.disconnectPeer(&p.ctx); break;
		case 4: tracker.cleanupStalePeers(std::chrono::steady_clock::now() + std::chrono::seconds(rng() % 2 ? 0 : 100000)); break;
		default: tracker.processMessage(announce(ih, id, "paused"), &p.ctx); break;
		}
		size_t memberships = 0, seeders = 0;
		for (const auto& entry : tracker.getSwarms()) {
			memberships += entry.second->getPeersCount();
			seeders += entry.second->getCompletedCount();
		}
		if (memberships != tracker.getMembershipCount() || seeders != tracker.getSeederCount()) {
			CHECK(memberships == tracker.getMembershipCount());
			CHECK(seeders == tracker.getSeederCount());
			return;
		}
	}
}

// An incremental sweep in N steps removes exactly what one full sweep removes
static void testIncrementalCleanup() {
	FastTracker tracker(20, 10);
	std::vector<std::unique_ptr<FakePeer>> peers;
	for (int i = 0; i < 500; i++) {
		peers.push_back(std::make_unique<FakePeer>());
		std::string id = std::string(16, 'z') + std::to_string(1000 + i);
		tracker.processMessage(announce(IH1, id), &peers.back()->ctx);
	}
	auto later = std::chrono::steady_clock::now() + std::chrono::seconds(100);
	// Partial sweep: something left
	for (int i = 0; i < 3; i++) tracker.cleanupStalePeersStep(later, 8);
	CHECK(tracker.getPeerIdCount() > 0);
	CHECK(tracker.getPeerIdCount() < 500);
	// Finish the sweep (plus one extra round in case the table rehashed)
	for (int i = 0; i < 16; i++) tracker.cleanupStalePeersStep(later, 8);
	CHECK(tracker.getPeerIdCount() == 0);
	CHECK(tracker.getSwarms().empty());
	CHECK(tracker.getMembershipCount() == 0);
}

// Reply intervals are spread over announceInterval ± jitter, and exact without jitter
static void testAnnounceIntervalJitter() {
	TrackerOptions options;
	options.announceInterval = 120;
	FastTracker tracker(options);
	FakePeer a;
	std::map<int, int> seen;
	for (int i = 0; i < 2000; i++) {
		tracker.processMessage(announce(IH1, peerId('a'), "update"), &a.ctx);
		int interval = a.inbox.back()["interval"];
		CHECK(interval >= 108 && interval <= 132);
		seen[interval]++;
	}
	// Uniform over 25 values: every value shows up, none dominates
	CHECK(seen.size() == 25);
	for (const auto& [value, count] : seen) {
		CHECK(count > 2000 / 25 / 3 && count < 2000 / 25 * 3);
	}

	options.announceIntervalJitter = 0;
	FastTracker exact(options);
	FakePeer b;
	exact.processMessage(announce(IH1, peerId('b')), &b.ctx);
	CHECK(b.inbox.back()["interval"] == 120);

	// Out-of-range jitter is clamped to 50%: never above 1.5x (below the 2x expiry)
	options.announceIntervalJitter = 5;
	FastTracker clamped(options);
	FakePeer c;
	for (int i = 0; i < 500; i++) {
		clamped.processMessage(announce(IH1, peerId('c'), "update"), &c.ctx);
		int interval = c.inbox.back()["interval"];
		CHECK(interval >= 60 && interval <= 180);
	}

	// Tiny intervals stay positive
	options.announceInterval = 1;
	options.announceIntervalJitter = 0.5;
	FastTracker tiny(options);
	FakePeer d;
	for (int i = 0; i < 100; i++) {
		tiny.processMessage(announce(IH1, peerId('d'), "update"), &d.ctx);
		CHECK(d.inbox.back()["interval"] >= 1);
	}
}

// The reason a message fails with, or nullopt-like "none" if it doesn't
static std::string failureOf(FastTracker& tracker, const json& data, PeerContext* peer) {
	try {
		tracker.processMessage(data, peer);
	} catch (const TrackerException& e) {
		return failureReasonKey(e.reason());
	}
	return "none";
}

static void testFailureReasons() {
	// Keys are unique identifiers, every reason has a message
	std::set<std::string> keys;
	for (size_t i = 0; i < FAILURE_REASON_COUNT; i++) {
		FailureReason reason = static_cast<FailureReason>(i);
		std::string key = failureReasonKey(reason);
		CHECK(!key.empty() && key.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") == std::string::npos);
		CHECK(keys.insert(key).second);
		CHECK(std::string(failureReasonMessage(reason)).size() > 5);
		CHECK(std::string(TrackerException(reason).what()) == failureReasonMessage(reason));
	}

	TrackerOptions options;
	options.maxSwarmsPerPeer = 2;
	options.maxScrapeInfoHashes = 2;
	FastTracker tracker(options);
	FakePeer p, other;
	tracker.processMessage(announce(IH1, peerId('o')), &other.ctx);

	json badOffer = announce(IH1, peerId('p'), "started", 1);
	badOffer["offers"][0]["offer"] = "x";
	json badOfferSdp = announce(IH1, peerId('p'), "started", 1);
	badOfferSdp["offers"][0]["offer"].erase("sdp");
	json noOfferId = answerMessage(IH1, peerId('p'), peerId('o'), "s");
	noOfferId.erase("offer_id");
	json badAnswer = answerMessage(IH1, peerId('p'), peerId('o'), "s");
	badAnswer["answer"] = {{"type", "answer"}};
	json answerBadToPeer = answerMessage(IH1, peerId('p'), "short", "s");
	json stopBadPeer = {{"action", "announce"}, {"event", "stopped"}, {"info_hash", IH1}, {"peer_id", 5}};

	// Client-visible messages are unchanged from the plain-string version
	const std::vector<std::tuple<json, std::string, std::string>> cases = {
		{json::array(), "not_object", "message must be a JSON object"},
		{{{"action", 5}}, "action_not_string", "action field must be a string"},
		{{{"action", "announce"}, {"event", 5}}, "event_not_string", "event field must be a string"},
		{announce(IH1, peerId('p'), "bogus"), "unknown_event", "unknown announce event"},
		{{{"action", "unknown"}}, "unknown_action", "unknown action"},
		{announce("short", peerId('p')), "announce_info_hash", "info_hash field is missing or wrong"},
		{announce(IH1, "short"), "announce_peer_id", "peer_id field is missing or wrong"},
		{{{"action", "announce"}, {"info_hash", IH1}, {"peer_id", peerId('p')}, {"offers", "x"}}, "offers_not_array", "announce: offers field is not an array"},
		{answerMessage("short", peerId('p'), peerId('o'), "s"), "answer_info_hash", "info_hash field is missing or wrong"},
		{answerMessage(IH1, "short", peerId('o'), "s"), "answer_peer_id", "peer_id field is missing or wrong"},
		{answerBadToPeer, "answer_to_peer_id", "to_peer_id field is missing or wrong"},
		{badAnswer, "bad_answer", "answer: answer field is missing or wrong"},
		{noOfferId, "bad_offer_id", "answer: offer_id field is missing or wrong"},
		{answerMessage(IH1, peerId('o'), peerId('p'), "s"), "answer_peer_id_taken", "answer: peer_id belongs to another connection"},
		{{{"action", "announce"}, {"event", "stopped"}}, "stop_info_hash", "info_hash field is missing or wrong"},
		{stopBadPeer, "stop_peer_id", "peer_id field is missing or wrong"},
		{{{"action", "scrape"}}, "full_scrape_disabled", "scrape: full scrape is disabled"},
		{{{"action", "scrape"}, {"info_hash", {IH1, IH2, IH1}}}, "too_many_scrape_hashes", "scrape: too many info_hash values"},
		{{{"action", "scrape"}, {"info_hash", 5}}, "bad_scrape_info_hash", "scrape: info_hash field is wrong"},
	};
	std::set<std::string> seen;
	for (const auto& [data, key, message] : cases) {
		std::string got = failureOf(tracker, data, &p.ctx);
		if (got != key) {
			std::cerr << "  expected " << key << ", got " << got << " for " << data.dump() << std::endl;
		}
		CHECK(got == key);
		seen.insert(key);
		for (size_t i = 0; i < FAILURE_REASON_COUNT; i++) {
			if (failureReasonKey(static_cast<FailureReason>(i)) == key) {
				CHECK(failureReasonMessage(static_cast<FailureReason>(i)) == message);
			}
		}
	}

	// Offers are validated after joining the swarm, so these use another connection
	FakePeer q;
	badOffer["peer_id"] = badOfferSdp["peer_id"] = peerId('q');
	CHECK(failureOf(tracker, badOffer, &q.ctx) == "bad_offer");
	CHECK(failureOf(tracker, badOfferSdp, &q.ctx) == "bad_offer_sdp");
	CHECK(countOffers(other) == 0);
	seen.insert("bad_offer");
	seen.insert("bad_offer_sdp");

	// Torrent limit (p is in no swarm yet: every case above failed before joining)
	CHECK(p.ctx.membershipCount == 0);
	tracker.processMessage(announce(IH1, peerId('p')), &p.ctx);
	tracker.processMessage(announce(IH2, peerId('p')), &p.ctx);
	CHECK(failureOf(tracker, announce(std::string(20, 'c'), peerId('p')), &p.ctx) == "too_many_torrents");
	seen.insert("too_many_torrents");

	// Every reason is covered by a case
	CHECK(seen.size() == FAILURE_REASON_COUNT);
}

static void testFailureReasonMetrics() {
	Metrics metrics;
	metrics.init(1);
	const size_t answerNotInSwarm = static_cast<size_t>(FailureReason::AnswerPeerIdTaken);
	const size_t badOffer = static_cast<size_t>(FailureReason::BadOffer);

	auto sample = [&](int64_t timeMs, uint64_t notInSwarm, uint64_t offers) {
		MetricsSample s;
		s.timeMs = timeMs;
		s.failureReasons[answerNotInSwarm] = notInSwarm;
		s.failureReasons[badOffer] = offers;
		s.failures = notInSwarm + offers;
		metrics.record(s);
	};
	auto reasonOf = [](const json& current, const char* key) {
		for (const auto& r : current["failureReasons"]) {
			if (r["reason"] == key) return r;
		}
		return json();
	};

	// One sample: nothing to compare against yet
	sample(1000000, 100, 0);
	json current = json::parse(*metrics.current());
	CHECK(current["failureReasons"].size() == FAILURE_REASON_COUNT);
	CHECK(reasonOf(current, "answer_peer_id_taken")["total"] == 100);
	CHECK(reasonOf(current, "answer_peer_id_taken")["windowCount"] == 0);
	CHECK(reasonOf(current, "answer_peer_id_taken")["rate"] == 0.0);

	// 5 s samples for 20 minutes: 2 failures per sample, one bad offer per minute
	int64_t t = 1000000;
	uint64_t n = 100, offers = 0;
	for (int i = 1; i <= 240; i++) {
		t += 5000;
		n += 2;
		if (i % 12 == 0) offers++;
		sample(t, n, offers);
	}
	current = json::parse(*metrics.current());
	json r = reasonOf(current, "answer_peer_id_taken");
	CHECK(r["message"] == "answer: peer_id belongs to another connection");
	CHECK(r["total"] == n);
	CHECK(r["rate"] == 0.4);              // 2 per 5 s
	CHECK(r["windowCount"] == 360);       // 15 min window = 180 samples
	CHECK(r["windowRate"] == 0.4);
	CHECK(current["failureWindowSeconds"] == 900);
	json o = reasonOf(current, "bad_offer");
	CHECK(o["total"] == 20);
	CHECK(o["windowCount"] == 15);
	CHECK(reasonOf(current, "unknown_action")["total"] == 0);
	// Sum of reasons matches the total counter
	uint64_t sum = 0;
	for (const auto& x : current["failureReasons"]) sum += x["total"].get<uint64_t>();
	CHECK(sum == current["totals"]["failures"].get<uint64_t>());
}

static void testFailureSampleSummary() {
	// A rejected announce with offers: the summary describes fields, never SDP
	json data = announce("not-a-binary-info-hash-but-hex-0123456789abcdef", peerId('p'), "started", 2);
	data["offers"][0]["offer"]["sdp"] = "v=0 c=IN IP4 203.0.113.7 a=candidate:1 1 udp 2122260223 198.51.100.4 54400 typ host";
	json summary = FailureSamples::summarize(data);
	std::string text = summary.dump();
	CHECK(text.find("203.0.113.7") == std::string::npos);
	CHECK(text.find("198.51.100.4") == std::string::npos);
	CHECK(text.find("candidate") == std::string::npos);
	CHECK(summary["keys"].size() == data.size());
	CHECK(summary["action"]["preview"] == "announce");
	CHECK(summary["offers"]["type"] == "array");
	CHECK(summary["offers"]["size"] == 2);
	CHECK(summary["offers"]["first"]["type"] == "object");
	CHECK(summary["firstOffer"]["offer"]["type"] == "object");
	CHECK(summary["firstOffer"]["offer"]["keys"] == json::array({"sdp", "type"}));
	CHECK(summary["firstOffer"]["offer_id"]["preview"] == "o0");

	// Long strings are cut to a preview; binary length and hex detection
	json ih = summary["info_hash"];
	CHECK(ih["bytes"] == 47);
	CHECK(ih["binaryChars"] == 47);
	CHECK(ih["preview"] == "not-a-binary-info-hash-but-hex-0123456789abcdef");
	CHECK(!ih.contains("hex"));
	json hex = FailureSamples::describe(std::string(40, 'a') + std::string(20, 'F'));
	CHECK(hex["hex"] == true);
	CHECK(hex["bytes"] == 60);
	CHECK(hex["preview"].get<std::string>() == std::string(40, 'a') + std::string(8, 'F') + "…");

	// Preview never splits a UTF-8 character: 47 ASCII bytes + "é" (2 bytes)
	std::string accented = std::string(47, 'x') + "\xc3\xa9" + "tail";
	CHECK(FailureSamples::describe(accented)["preview"].get<std::string>() == std::string(47, 'x') + "…");
	CHECK(FailureSamples::describe(std::string("\xe2\x82\xac"))["binaryChars"] == -1);

	// Non-strings
	CHECK(FailureSamples::describe(5)["type"] == "number");
	CHECK(FailureSamples::describe(5)["value"] == 5);
	CHECK(FailureSamples::describe(json::array({json::array({1})}))["first"]["first"]["type"] == "number");
	CHECK(FailureSamples::summarize(json::array())["type"] == "array");
	json many = json::object();
	for (int i = 0; i < 40; i++) many["k" + std::to_string(i)] = i;
	CHECK(FailureSamples::summarize(many)["keys"].size() == FailureSamples::MAX_KEYS);
}

static void testFailureSampleStore() {
	FailureSamples samples;
	// One sample per reason per interval
	CHECK(samples.shouldSample(FailureReason::BadOffer));
	CHECK(!samples.shouldSample(FailureReason::BadOffer));
	CHECK(samples.shouldSample(FailureReason::UnknownAction));

	// Keeps the last PER_REASON, newest first
	for (int i = 0; i < 30; i++) {
		samples.record(FailureReason::BadOffer, {{"n", i}});
	}
	json dump = json::parse(samples.dump());
	CHECK(dump.size() == 1);
	CHECK(dump["bad_offer"].size() == FailureSamples::PER_REASON);
	CHECK(dump["bad_offer"][0]["n"] == 29);
	CHECK(dump["bad_offer"].back()["n"] == 10);
	CHECK(json::parse(FailureSamples().dump()).empty());
}

static void testFailureDiagnosis() {
	FastTracker tracker;
	FakePeer a, b, a2;
	tracker.processMessage(announce(IH1, peerId('a')), &a.ctx);
	tracker.processMessage(announce(IH1, peerId('b')), &b.ctx);

	// Answer for a torrent the sender never announced on this connection
	json answer = answerMessage(IH2, peerId('a'), peerId('b'), "s");
	json d = tracker.diagnose(answer, &a.ctx);
	CHECK(d["connectionTorrents"] == 1);
	CHECK(d["connectionPeerIds"] == 1);
	CHECK(d["peerIdConnection"] == "this");
	CHECK(d["peerIdOnThisConnection"] == true);
	CHECK(d["swarmExists"] == false);
	CHECK(!d.contains("peerInSwarm"));

	// The peer ID reconnected on a2: the old connection answers
	tracker.processMessage(announce(IH1, peerId('a')), &a2.ctx);
	d = tracker.diagnose(answerMessage(IH1, peerId('a'), peerId('b'), "s"), &a.ctx);
	CHECK(d["peerIdConnection"] == "other");
	CHECK(d["peerInSwarm"] == "other");
	CHECK(d["toPeerInSwarm"] == true);
	CHECK(d["swarmPeers"] == 2);

	// Unknown peer, and non-object requests
	d = tracker.diagnose(answerMessage(IH1, peerId('z'), peerId('y'), "s"), &b.ctx);
	CHECK(d["peerIdConnection"] == "none");
	CHECK(d["peerIdOnThisConnection"] == false);
	CHECK(d["peerInSwarm"] == "no");
	CHECK(d["toPeerInSwarm"] == false);
	CHECK(tracker.diagnose(json::array(), &b.ctx).size() == 2);
	CHECK(tracker.diagnose({{"peer_id", 5}}, &b.ctx).size() == 2);
}

static void testIsLoopback() {
	auto bin = [](std::initializer_list<int> bytes) { std::string s; for (int b : bytes) s.push_back((char) b); return s; };
	CHECK(Utils::isLoopback(bin({127, 0, 0, 1})));
	CHECK(Utils::isLoopback(bin({127, 1, 2, 3})));
	CHECK(!Utils::isLoopback(bin({10, 0, 0, 1})));
	CHECK(!Utils::isLoopback(bin({172, 64, 0, 1})));
	CHECK(Utils::isLoopback(bin({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1})));
	CHECK(Utils::isLoopback(bin({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0, 0, 1})));
	CHECK(!Utils::isLoopback(bin({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 8, 8, 8, 8})));
	CHECK(!Utils::isLoopback(bin({0x26, 0x06, 0x47, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1})));
	CHECK(!Utils::isLoopback(""));
}

// bittorrent-tracker handles an answer as an announce ("update"): libtorrent
// (qBittorrent) answers an offer on a new connection before announcing there
static void testAnswerIsAnAnnounce() {
	TrackerOptions options;
	options.maxSwarmsPerPeer = 2;
	FastTracker tracker(options);
	FakePeer offerer, oldConn, newConn;

	tracker.processMessage(announce(IH1, peerId('q')), &oldConn.ctx);
	tracker.processMessage(announce(IH1, peerId('w'), "started", 1), &offerer.ctx);
	CHECK(countOffers(oldConn) == 1);
	tracker.disconnectPeer(&oldConn.ctx);
	tracker.processMessage(answerMessage(IH1, peerId('q'), peerId('w'), "late"), &newConn.ctx);
	CHECK(answersWithSdp(offerer, "late") == 1);
	CHECK(offerer.inbox.back()["peer_id"] == peerId('q'));
	// The sender joined the swarm on the new connection, as a leecher
	CHECK(newConn.ctx.hasMembership(peerId('q'), IH1));
	CHECK(tracker.getSwarms().at(IH1)->getIncompleteCount() == 2);
	CHECK(tracker.getMembershipCount() == 2);
	// No announce reply for an answer (bittorrent-tracker doesn't send one either)
	CHECK(newConn.inbox.empty());

	// Stricter than bittorrent-tracker: no taking over a live peer ID
	FakePeer thief;
	CHECK(failureOf(tracker, answerMessage(IH1, peerId('q'), peerId('w'), "x"), &thief.ctx) == "answer_peer_id_taken");
	CHECK(answersWithSdp(offerer, "x") == 0);
	CHECK(thief.ctx.membershipCount == 0);

	// No swarm: dropped, nothing created
	FakePeer stray;
	CHECK(failureOf(tracker, answerMessage(IH2, peerId('s'), peerId('w'), "y"), &stray.ctx) == "none");
	CHECK(tracker.getSwarms().count(IH2) == 0);
	CHECK(stray.ctx.membershipCount == 0);

	// The torrent limit applies
	FakePeer full;
	tracker.processMessage(announce(IH2, peerId('f')), &full.ctx);
	tracker.processMessage(announce(std::string(20, 'c'), peerId('f')), &full.ctx);
	CHECK(failureOf(tracker, answerMessage(IH1, peerId('g'), peerId('w'), "z"), &full.ctx) == "too_many_torrents");
	CHECK(answersWithSdp(offerer, "z") == 0);

	// Removed with the connection like any membership
	tracker.disconnectPeer(&newConn.ctx);
	CHECK(tracker.getSwarms().at(IH1)->getPeersCount() == 1);
}

// Decided 2026-11-06: follow bittorrent-tracker, no support for other formats
// (README: "Compatibility with bittorrent-tracker")
static void testStrictProtocol() {
	FastTracker tracker;
	FakePeer a;
	tracker.processMessage(announce(IH1, peerId('a')), &a.ctx);

	// "empty" is an update, like in bittorrent-tracker
	size_t before = a.inbox.size();
	tracker.processMessage(announce(IH1, peerId('a'), "empty"), &a.ctx);
	CHECK(a.inbox.size() == before + 1);
	CHECK(a.inbox.back()["interval"].is_number());

	// Unknown events are rejected (Gopeed's "<regular>")
	CHECK(failureOf(tracker, announce(IH1, peerId('a'), "<regular>"), &a.ctx) == "unknown_event");
	// Identifiers must be 20-byte binary strings: base64 or hex hashes and
	// 16-character peer IDs are rejected
	CHECK(failureOf(tracker, announce("JhMOjx9NZsExpIDm5Y1u6j2puX5Bij6PuNdzcTvaqOo=", "rmi325ty2roqtkuk"), &a.ctx) == "announce_info_hash");
	CHECK(failureOf(tracker, announce(std::string(40, 'a'), peerId('a')), &a.ctx) == "announce_info_hash");
	CHECK(failureOf(tracker, announce(IH1, "rmi325ty2roqtkuk"), &a.ctx) == "announce_peer_id");
	CHECK(failureOf(tracker, answerMessage(IH1, peerId('a'), "short", "s"), &a.ctx) == "answer_to_peer_id");
}

#define RUN(t) do { if (std::getenv("OWT_TEST_VERBOSE")) std::cerr << #t << std::endl; t(); } while (0)

int main() {
	RUN(testMalformedInputNeverCrashes);
	RUN(testAnnounceResponse);
	RUN(testOffersAreDistributed);
	RUN(testRandomPeersAreUniform);
	RUN(testAnswerIsRouted);
	RUN(testReconnectWithSamePeerId);
	RUN(testStopAndDisconnectCleanUp);
	RUN(testCompletedAndScrape);
	RUN(testBinaryStrings);
	RUN(testLimits);
	RUN(testTokenBucket);
	RUN(testAddressKey);
	RUN(testMultiplePeerIdsPerConnection);
	RUN(testPausedPartialSeeds);
	RUN(testStaleMembershipsExpire);
	RUN(testOutboxQueuesMessages);
	RUN(testRelayFramesMatchJson);
	RUN(testCountersMatchWalk);
	RUN(testIncrementalCleanup);
	RUN(testAnnounceIntervalJitter);
	RUN(testFailureReasons);
	RUN(testFailureReasonMetrics);
	RUN(testFailureSampleSummary);
	RUN(testFailureSampleStore);
	RUN(testFailureDiagnosis);
	RUN(testIsLoopback);
	RUN(testAnswerIsAnAnnounce);
	RUN(testStrictProtocol);

	if (failures) {
		std::cerr << failures << " check(s) failed" << std::endl;
		return 1;
	}
	std::cout << "All tests passed" << std::endl;
	return 0;
}
