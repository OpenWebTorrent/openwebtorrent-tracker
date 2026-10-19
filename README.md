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
| `--announce-interval` | `120` | Announce interval sent to clients, in seconds. Peers that don't announce for 2 intervals are dropped |
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

Tracker stats are available at `/stats.json`.

### Protocol notes

- A connection can announce with several peer IDs (libtorrent shares one
  connection between torrents). Announcing a peer ID from another connection
  moves that peer ID, and only that one, to the new connection.
- [BEP 21](https://www.bittorrent.org/beps/bep_0021.html) partial seeds:
  announces with `event: "paused"` count as incomplete but not as
  downloaders. Scrape responses include a `downloaders` field.

## Tests

```sh
# Unit tests for the tracker logic
make test

# Integration tests against the built binary (requires Node.js 18+)
cd tests/node && npm install && npm test
```

## License

MIT. Copyright (c) [Alex](https://github.com/alxhotel)
