#include <exception>
#include <iostream>
#include <cxxopts.hpp>

#include "Utils.h"
#include "WebtorrentTracker.h"

int main(int argc, char** argv) {
	cxxopts::Options options("openwebtorrent-tracker", "An open webtorrent tracker");

	options.add_options()
		("p,port", "Port", cxxopts::value<int>()->default_value("8000"))
		("H,host", "Host to listen on (default: all interfaces)", cxxopts::value<std::string>()->default_value(""))
		("k,ssl-key", "SSL key", cxxopts::value<std::string>()->default_value("./localhost-key.pem"))
		("c,ssl-cert", "SSL certificate", cxxopts::value<std::string>()->default_value("./localhost.pem"))
		("no-ssl", "Serve plain websockets (e.g. behind a TLS-terminating proxy)")
		("trust-proxy", "Use X-Forwarded-For as client address (only behind a reverse proxy)")
		("max-connections-per-ip", "Max connections per IPv4 address or IPv6 /64 (0 = unlimited)", cxxopts::value<size_t>()->default_value("0"))
		("rate-limit", "Max messages per second per connection (0 = unlimited)", cxxopts::value<double>()->default_value("50"))
		("rate-burst", "Max messages per connection in a burst", cxxopts::value<double>()->default_value("500"))
		("max-torrents-per-connection", "Max torrents a connection can announce, across all its peer IDs (0 = unlimited)", cxxopts::value<size_t>()->default_value("1000"))
		("allow-full-scrape", "Allow scrape requests without info_hash")
		("announce-interval", "Announce interval in seconds (peers are dropped after 2 intervals without announcing)", cxxopts::value<int>()->default_value("120"))
		("d,debug", "Debug", cxxopts::value<bool>()->default_value("false"))
		("h,help", "Print usage")
	;

	try {
		auto result = options.parse(argc, argv);

		// Help
		if (result.count("help")) {
			std::cout << options.help() << std::endl;
			return 0;
		}

		// Port
		int port = result["port"].as<int>();
		if (port <= 0 || port > 65535) {
			std::cerr << "Port must be between 1 and 65535" << std::endl;
			return 1;
		}

		int announceInterval = result["announce-interval"].as<int>();
		if (announceInterval <= 0) {
			std::cerr << "Announce interval must be positive" << std::endl;
			return 1;
		}

		owt::ServerOptions serverOptions;
		serverOptions.port = port;
		serverOptions.host = result["host"].as<std::string>();

		// Key & Cert
		if (!result.count("no-ssl")) {
			serverOptions.keyFile = result["ssl-key"].as<std::string>();
			serverOptions.certFile = result["ssl-cert"].as<std::string>();
		}

		// Limits
		serverOptions.trustProxy = result.count("trust-proxy") > 0;
		serverOptions.maxConnectionsPerIp = result["max-connections-per-ip"].as<size_t>();
		serverOptions.messageRate = result["rate-limit"].as<double>();
		serverOptions.messageBurst = result["rate-burst"].as<double>();
		serverOptions.tracker.maxSwarmsPerPeer = result["max-torrents-per-connection"].as<size_t>();
		serverOptions.tracker.allowFullScrape = result.count("allow-full-scrape") > 0;
		serverOptions.tracker.announceInterval = announceInterval;

		bool debug = result["debug"].as<bool>();
		if (debug) {
			owt::Utils::Logger::set_log_level("LOG_DEBUG");
		}

		owt::WebtorrentTracker tracker(serverOptions);
		tracker.run();

		return 0;
	} catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;

		return 1;
	}
}
