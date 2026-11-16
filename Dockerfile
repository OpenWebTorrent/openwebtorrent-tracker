# Build: make deps && docker build -t openwebtorrent-tracker .
# Run:   docker run -d -p 8000:8000 openwebtorrent-tracker

FROM debian:trixie-slim AS build

RUN apt-get update \
	&& apt-get install -y --no-install-recommends g++ make cmake libssl-dev zlib1g-dev \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN test -f deps/uWebSockets/uSockets/src/libusockets.h \
	|| { echo "Missing dependencies: run 'make deps' before building the image" >&2; exit 1; }
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
	&& cmake --build build -j "$(nproc)" \
	&& strip build/openwebtorrent-tracker

FROM debian:trixie-slim

# tini runs as PID 1 and forwards signals: the kernel ignores SIGTERM for a
# PID 1 without a handler, so `docker stop` would wait 10 s and then kill it
RUN apt-get update \
	&& apt-get install -y --no-install-recommends libssl3t64 zlib1g tini \
	&& rm -rf /var/lib/apt/lists/* \
	&& useradd --system --no-create-home --shell /usr/sbin/nologin tracker

COPY --from=build /src/build/openwebtorrent-tracker /usr/local/bin/openwebtorrent-tracker

# Default --ssl-key / --ssl-cert paths (./localhost-key.pem, ./localhost.pem)
# are relative to this directory: mount certificates here to serve wss://
WORKDIR /etc/openwebtorrent-tracker
USER tracker
EXPOSE 8000

ENTRYPOINT ["tini", "--", "openwebtorrent-tracker"]
# TLS is usually terminated by a reverse proxy in front of the container.
# Arguments passed to `docker run` replace these, so repeat --no-ssl.
CMD ["--no-ssl"]
