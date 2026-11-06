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
| `/status` | Status page: health checks, current values and charts (connections, peers, messages, CPU per thread, lock contention, churn, rejected traffic, bandwidth, memory), and protocol errors by reason |
| `/stats.json` | Torrent, peer and connection counts, computed live (original format) |
| `/stats/metrics.json` | Current metrics, refreshed every 5 seconds. `failureReasons` lists each protocol error (`reason`, the `message` sent to clients, `total` since start, and `windowCount` / `windowRate` over the last 15 minutes) |
| `/debug/failures.json` | Last 20 rejected requests per failure reason, summarized. Loopback only (404 otherwise). See [Debugging protocol errors](#debugging-protocol-errors) |
| `/stats/history.json?range=15m\|1h\|6h\|24h\|7d` | Metric history for charts. Kept in memory: 5-second samples for 1 hour, 1-minute samples for 7 days |

The status page and the `/stats/metrics.json`, `/stats/history.json` and
`/debug/failures.json` endpoints are marked `noindex, nofollow` (an
`X-Robots-Tag` header, plus a `<meta name="robots">` tag on the page) so
search engines don't index them. `/stats.json` keeps its original headers.

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
- An answer also counts as an announce (see below).

## Compatibility with bittorrent-tracker

The WebTorrent tracker protocol has no formal specification. Its reference is
the [bittorrent-tracker](https://github.com/webtorrent/bittorrent-tracker)
server used by WebTorrent, and this tracker follows it. Everything below was
checked against bittorrent-tracker 11.2.3's source:

- `info_hash`, `peer_id` and `to_peer_id` are **20-byte binary strings** (the
  BitTorrent hash and peer ID, one character per byte).
- `event` is `started`, `stopped`, `completed`, `paused` or `update`; absent
  or `empty` mean `update`. Anything else is an error.
- An answer is handled as an announce (an `update`): if its sender isn't in
  the swarm yet, it's added (as a leecher), then the answer is relayed to
  `to_peer_id`. No announce reply is sent for an answer. qBittorrent 5.2/5.3
  and BitTorrent 7.11 (libtorrent) rely on this: they receive an offer on one
  connection and send the answer on a new connection before announcing there.
  Earlier versions of this tracker rejected those answers (`answer: peer_id
  is not in the swarm on this connection`, ~19/s, about 3% of all answers,
  measured 2026-11-05), as the original Node tracker did.

### Where this tracker differs

These don't change anything for a client that follows the protocol:

- **Several peer IDs per connection** are fully supported (see "Protocol
  notes"). bittorrent-tracker accepts them too, but when the connection
  closes it only removes the first peer ID it saw; the others linger until
  they expire.
- **An answer never moves a peer ID to another connection.** If an answer's
  `peer_id` belongs to another live connection, the answer is relayed but
  nothing is registered: the peer stays on its connection. bittorrent-tracker
  also relays it and keeps the peer's original socket when the peer is in the
  swarm; when it isn't, bittorrent-tracker adds it on the answering socket,
  which we don't. Only an announce moves a peer ID, so reconnecting clients
  keep their identity. Relaying such an answer is harmless: the recipient
  only accepts an answer to an `offer_id` it sent to that peer. Clients that
  need this (seen 2026-11-06, ~1.5/s): qBittorrent answering on a new
  connection while its old one is still open, and an app (no Origin or
  User-Agent) that answers with the placeholder peer ID
  `01234567890123456789` from every instance.
- **Server limits**: messages per second, torrents per connection, info
  hashes per scrape, and no full scrape by default (see "Limits").

### Not supported, on purpose

Decided on 2026-11-06. These clients get a `failure reason` for each such
request; their connections stay open. The numbers were measured on
tracker.openwebtorrent.com on 2026-11-05 with
[`/debug/failures.json`](#debugging-protocol-errors).

- **Identifiers that aren't 20-byte binary strings.** The original Node
  tracker (replaced on 2026-11-04) accepted any string, so some non-WebTorrent
  clients had been using it:
  - About 3,850 clients of one Go application (`User-Agent:
    Go-http-client/1.1`, no `Origin`), ~32 announces/s, half of all rejected
    requests. It uses the tracker as a WebRTC meeting point for its own
    rooms: `info_hash` is a base64 SHA-256 (44 characters) and `peer_id` 16
    random characters. These aren't BitTorrent v2 torrents: the first and last
    20 bytes of their hashes matched no swarm.
  - A few clients sending a 20-byte hash as 40 hex characters (a Go client
    with peer ID prefix `-PI0100-`, and a Node.js client), ~0.3/s.

  Reasons: `announce_info_hash`, `announce_peer_id`, `answer_*`, `stop_*`.
- **Unknown events.** [Gopeed](https://gopeed.com) 1.9.30 (peer ID prefix
  `-GP1930-`) announces `started`, then re-announces with
  `event: "<regular>"`. Those fail (`unknown_event`), so its peers expire from
  their swarms after two announce intervals; ~0.5 requests/s.

### Failure reasons

Every rejected request gets a reply with `failure reason` (the connection
stays open) and is counted under one reason in `/stats/metrics.json`
(`failureReasons`) and on `/status`. The message is what the client receives.

| Reason | Message |
| --- | --- |
| `not_object` | message must be a JSON object |
| `action_not_string` | action field must be a string |
| `event_not_string` | event field must be a string |
| `unknown_event` | unknown announce event |
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
| `stop_info_hash` | info_hash field is missing or wrong |
| `stop_peer_id` | peer_id field is missing or wrong |
| `full_scrape_disabled` | scrape: full scrape is disabled |
| `too_many_scrape_hashes` | scrape: too many info_hash values |
| `bad_scrape_info_hash` | scrape: info_hash field is wrong |

Removed: `answer_not_in_swarm` (answers now count as announces, see above)
and `answer_peer_id_taken` (answers with another connection's peer ID are
relayed, see "Where this tracker differs").

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
  (e.g. `-qB5300-` is qBittorrent 5.3.0). SDP is never stored: it contains
  users' IP addresses.
- `client`: the connection's `Origin | User-Agent`.
- `connectionAgeSeconds`: how long the connection had been open.
- `tracker`: tracker state at that moment: which connection owns the
  `peer_id` (`this`, `other` or `none`), whether the sender and `to_peer_id`
  are in the swarm, and how many torrents and peer IDs the connection has.

When a reason grows on `/status`, read its samples: group them by `client`
and `peer_id` prefix to find who sends them, and compare the fields with what
the protocol expects. That's how the findings above were made.

## Tests

```sh
# Unit tests for the tracker logic
make test

# Integration tests against the built binary (requires Node.js 18+)
cd tests/node && npm install && npm test
```

## License

MIT. Copyright (c) [Alex](https://github.com/alxhotel)
