# OpenWebtorrent Tracker

Fast and simple webtorrent tracker written in C++.

## Requirements
- C++ 20 compiler (GCC 10+ or Clang 11+)
- CMake 3.10+
- OpenSSL 1.1+
- ZLib

```sh
# Debian / Ubuntu
sudo apt-get install build-essential cmake libssl-dev zlib1g-dev
```

## Build

First, install dependencies:

```sh
make deps
```

Then, build the project:

```sh
make build
```

## Usage

You can run it with the command below:

```sh
./build/openwebtorrent-tracker \
	--port [PORT] \
	--host [HOST] \
	--ssl-key [PATH] \
	--ssl-cert [PATH]
```

| Option | Default | Description |
| --- | --- | --- |
| `-p, --port` | `8000` | Port to listen on |
| `-H, --host` | all interfaces | Address to bind to |
| `-k, --ssl-key` | `./localhost-key.pem` | SSL private key |
| `-c, --ssl-cert` | `./localhost.pem` | SSL certificate |
| `--no-ssl` | | Serve plain `ws://`, e.g. behind a TLS-terminating reverse proxy |
| `-t, --threads` | `0` | Worker threads; `0` = one per CPU core. Falls back to the `OWT_THREADS` environment variable |
| `--announce-interval` | `120` | Announce interval sent to clients, in seconds. Peers that don't announce for 2 intervals are dropped |
| `--announce-jitter` | `10` | Randomize each reply's interval by up to this percent (0-50), so clients that connected together (e.g. after a restart) don't keep announcing in lockstep |
| `-d, --debug` | | Log every message |

### Limits

| Option | Default | Description |
| --- | --- | --- |
| `--rate-limit` | `50` | Messages per second per connection (`0` = unlimited). Connections that exceed it are closed with code 1008 |
| `--rate-burst` | `500` | Messages a connection can send at once before the rate limit applies |
| `--max-torrents-per-connection` | `1000` | Torrents a single connection can announce, across all its peer IDs (`0` = unlimited) |
| `--max-connections-per-ip` | `0` | Simultaneous connections per IPv4 address or IPv6 /64 (`0` = unlimited). Extra connections get `429` |
| `--allow-full-scrape` | | Allow `scrape` without `info_hash`, which returns every torrent |
| `--trust-proxy` | | Use the last `X-Forwarded-For` entry as the client address |

When running behind a reverse proxy, every connection comes from the proxy's
address: pass `--trust-proxy` (and make sure the proxy sets `X-Forwarded-For`)
before enabling `--max-connections-per-ip`. Never use `--trust-proxy` when the
tracker is directly reachable, since clients could then forge their address.

### Monitoring

| Path | Description |
| --- | --- |
| `/status` | Status page: health checks, current values and charts (connections, peers, messages, CPU per thread, lock contention, churn, rejected traffic, bandwidth, memory), protocol errors by reason, and requests accepted outside the protocol |
| `/stats.json` | Torrent, peer and connection counts, computed live (original format) |
| `/stats/metrics.json` | Current metrics, refreshed every 5 seconds. `failureReasons` lists each protocol error (`reason`, the `message` sent to clients, `total` since start, and `windowCount` / `windowRate` over the last 15 minutes); `nonStandard` counts requests accepted [outside the WebTorrent protocol](#deviations-from-the-webtorrent-protocol), in the same format |
| `/debug/failures.json` | Last 20 rejected requests per failure reason, summarized. Loopback only (404 otherwise). See [Debugging protocol errors](#debugging-protocol-errors) |
| `/stats/history.json?range=15m\|1h\|6h\|24h\|7d` | Metric history for charts. Kept in memory: 5-second samples for 1 hour, 1-minute samples for 7 days |

### Threads

One thread accepts connections and hands them round robin to the worker
threads, each with its own event loop. Workers parse, serialize and send in
parallel; the shared tracker state (swarms, peer IDs) is protected by a single
lock held only while it's updated. Messages for a peer on another thread are
handed to that thread's loop. Messages on one connection are processed in
order; messages on different connections have no ordering guarantee.

### Protocol notes

- A connection can announce with several peer IDs (libtorrent shares one
  connection between torrents). Announcing a peer ID from another connection
  moves that peer ID, and only that one, to the new connection.
- [BEP 21](https://www.bittorrent.org/beps/bep_0021.html) partial seeds:
  announces with `event: "paused"` count as incomplete but not as
  downloaders. Scrape responses include a `downloaders` field.
- The tracker accepts some requests the WebTorrent protocol doesn't allow,
  listed in the next section.

## Deviations from the WebTorrent protocol

The WebTorrent tracker protocol has no formal specification. Its reference is
the [bittorrent-tracker](https://github.com/webtorrent/bittorrent-tracker)
server used by WebTorrent (checked against version 11.2.3): a JSON message per
WebSocket frame, with `info_hash`, `peer_id` and `to_peer_id` as **20-byte
binary strings** (the BitTorrent hash and peer ID, one character per byte),
`event` one of `started`, `stopped`, `completed`, `paused` or `update`
(absent or `empty` mean `update`), and WebRTC offers/answers relayed between
peers of the same swarm.

This tracker **deliberately implements more than that**, so that clients
which worked with the original OpenWebtorrent tracker (the Node.js
`pool-tracker`, replaced on 2026-11-04) keep working. Each deviation below is
counted separately (`nonStandard` in `/stats/metrics.json`, and the "Accepted
outside the WebTorrent protocol" table on `/status`), so we can see who still
relies on it and drop it if that ever reaches zero. Protocol-conforming
clients behave exactly as they would on a strict tracker.

The numbers below were measured on tracker.openwebtorrent.com on 2026-11-05
with [`/debug/failures.json`](#debugging-protocol-errors), while these
requests were still rejected.

### 1. Identifiers are opaque strings of 1–64 bytes

| | |
| --- | --- |
| **Protocol** | `info_hash`, `peer_id` and `to_peer_id` are 20-byte binary strings; bittorrent-tracker rejects other lengths (`invalid info_hash`, `invalid peer_id`) |
| **This tracker** | Any non-empty string up to 64 bytes (UTF-8, as received) is accepted as an opaque key, in announces, answers, stops and scrapes |
| **Counter** | `non_standard_ids`: accepted announce, answer or stop with at least one identifier that isn't a 20-byte binary string |
| **Before** | The original Node tracker accepted any string. The C++ tracker required 20 bytes from #28 (2026-10-18) until this change, so these clients failed from the cutover on 2026-11-04 |

**Who relies on it:** about 3,850 clients sending ~32 announces/s, which was
half of all rejected requests. Nearly all are one application
(`User-Agent: Go-http-client/1.1`, no `Origin`) that uses the tracker as a
WebRTC rendezvous for its own rooms: `info_hash` is a base64-encoded SHA-256
(44 characters, e.g. `JhMOjx9NZsExpIDm5Y1u6j2puX5Bij6PuNdzcTvaqOo=`) and
`peer_id` is 16 random characters. These are not BitTorrent v2 torrents: the
first and last 20 bytes of the hashes matched no swarm. About 1% send a
regular 20-byte hash written as 40 hex characters (a Go client with peer ID
prefix `-PI0100-`, and a Node.js client).

**Consequences:**
- Keys are compared byte for byte. A hash in hex and the same hash in binary
  are **different swarms**: no conversion is attempted, because replies,
  offers and answers must carry each client's `info_hash` exactly as it sent
  it. That's also how the original tracker behaved.
- Replies echo the identifiers unchanged.
- The 64-byte limit fits base64 (44) and hex (64) SHA-256 and bounds memory
  per peer; longer, empty or non-string identifiers are still rejected
  (`announce_info_hash`, `announce_peer_id`, ...).

### 2. Unknown events are handled as `update`

| | |
| --- | --- |
| **Protocol** | `event` is `started`, `stopped`, `completed`, `paused` or `update`; absent or `empty` mean `update`. bittorrent-tracker rejects anything else with `invalid event` |
| **This tracker** | Any other string is handled like `update`: the peer stays in (or joins) the swarm, gets an announce reply, and its offers are relayed. A non-string `event` is still an error (`event_not_string`) |
| **Counter** | `unknown_events` |
| **Before** | Rejected with `unknown announce event`, by both the original Node tracker and the C++ tracker (`empty` included; it's now a regular update and isn't counted) |

**Who relies on it:** [Gopeed](https://gopeed.com) 1.9.30 (peer ID prefix
`-GP1930-`, `Go-http-client/1.1`) announces `started` correctly, then sends
`event: "<regular>"` on every re-announce. Those failed, so its peers
silently expired from their swarms after two announce intervals (~0.5
requests/s).

### 3. An answer can come from a peer that hasn't announced on its connection

| | |
| --- | --- |
| **Protocol** | Peers announce a torrent, then exchange offers and answers with other peers of that swarm on the same connection |
| **This tracker** | If an answer's `peer_id` hasn't announced that torrent on this connection, and **no other live connection owns that peer ID**, the sender is added to the swarm (as a leecher, like a regular update without `left`) and the answer is relayed. No announce reply is sent for an answer |
| **Still rejected** | A `peer_id` owned by another live connection: `answer_peer_id_taken` (`answer: peer_id belongs to another connection`). The torrent limit applies (`too_many_torrents`). If the torrent has no swarm, the answer is dropped silently, since nobody can receive it |
| **Counter** | `answers_without_announce`: answers that added their sender to a swarm |
| **Before** | Rejected with `answer: peer_id is not in the swarm on this connection` (reason `answer_not_in_swarm`), by both the original Node tracker and the C++ tracker |

**Who relies on it:** qBittorrent 5.2.4 and 5.3.0 and BitTorrent 7.11
(libtorrent's WebTorrent support), ~19 answers/s, about 3% of all answers.
They receive an offer on one connection, then send the answer on a
**new** connection (0–10 s old) before announcing there, with a peer ID that
no connection owns any more. Each rejected answer was a failed WebRTC
connection between that client and a browser peer. bittorrent-tracker
handles an answer as an announce (an `update`, which adds a peer that isn't in
the swarm yet), so these clients work there. Unlike bittorrent-tracker, we
don't let an answer take over a peer ID that another connection is using.

**Safety:** answering grants nothing an announce doesn't: any client can join
any swarm by announcing. Offers are routed to a specific peer with a random
`offer_id`, so a stray answer is ignored by its recipient. The one protection
that matters is kept: a peer ID in use on another connection can't be taken
over by answering. (Announcing *does* move a peer ID to a new connection, by
design, so reconnecting clients keep their identity.) Memberships added by an
answer expire after two announce intervals unless the client announces, and
are removed when its connection closes, like any other.

### Failure reasons

Every rejected request gets a reply with `failure reason` (the connection
stays open) and is counted under one reason in `/stats/metrics.json`
(`failureReasons`). The message is what the client receives.

| Reason | Message |
| --- | --- |
| `not_object` | message must be a JSON object |
| `action_not_string` | action field must be a string |
| `event_not_string` | event field must be a string |
| `unknown_action` | unknown action |
| `announce_info_hash` | info_hash field is missing or wrong |
| `announce_peer_id` | peer_id field is missing or wrong |
| `offers_not_array` | announce: offers field is not an array |
| `too_many_torrents` | announce: too many torrents on this connection |
| `bad_offer` | announce: wrong offer item format |
| `bad_offer_sdp` | announce: wrong offer item field format |
| `answer_info_hash` | info_hash field is missing or wrong |
| `answer_peer_id` | peer_id field is missing or wrong |
| `answer_to_peer_id` | to_peer_id field is missing or wrong |
| `bad_answer` | answer: answer field is missing or wrong |
| `bad_offer_id` | answer: offer_id field is missing or wrong |
| `answer_peer_id_taken` | answer: peer_id belongs to another connection |
| `stop_info_hash` | info_hash field is missing or wrong |
| `stop_peer_id` | peer_id field is missing or wrong |
| `full_scrape_disabled` | scrape: full scrape is disabled |
| `too_many_scrape_hashes` | scrape: too many info_hash values |
| `bad_scrape_info_hash` | scrape: info_hash field is wrong |

"Missing or wrong" identifiers are those that are absent, not strings, empty
or longer than 64 bytes. Removed reasons: `unknown_event` (see deviation 2)
and `answer_not_in_swarm` (see deviation 3).

## Debugging protocol errors

`/debug/failures.json` keeps the last 20 rejected requests for each failure
reason (at most one per reason every 100 ms, so a flood costs nothing). It
answers only on loopback, so run it on the server:

```sh
curl -s http://127.0.0.1/debug/failures.json
```

Each sample has:

- `request`: a summary of the request, never the request itself. Its keys
  and, for each known field, the type, size in bytes, length as a binary
  string (`binaryChars`, -1 if it isn't one), whether it's all hex, and a
  48-byte preview. The `peer_id` prefix usually names the client and version
  (e.g. `-qB5300-` is qBittorrent 5.3.0). SDP is never
  stored: it contains users' IP addresses.
- `client`: the connection's `Origin | User-Agent`.
- `connectionAgeSeconds`: how long the connection had been open.
- `tracker`: tracker state at that moment: which connection owns the
  `peer_id` (`this`, `other` or `none`), whether the sender and `to_peer_id`
  are in the swarm, and how many torrents and peer IDs the connection has.

When a reason grows on `/status`, read its samples: group them by `client`
and `peer_id` prefix to find who sends them, and compare the fields with what
the code expects. That's how the deviations above were found.

## Tests

```sh
# Unit tests for the tracker logic
make test

# Integration tests against the built binary (requires Node.js 18+)
cd tests/node && npm install && npm test
```

## License

MIT. Copyright (c) [Alex](https://github.com/alxhotel)
