#include <exception>
#include <iostream>
#include <cxxopts.hpp>

#include "Utils.h"
#include "WebtorrentTracker.h"

int main(int argc, char** argv) {
	cxxopts::Options options("openwebtorrent-tracker", "An open webtorrent tracker");

	options.add_options()
		("p,port", "Port", cxxopts::value<int>()->default_value("8000"))
		("k,ssl-key", "SSL key", cxxopts::value<std::string>()->default_value("./localhost-key.pem"))
		("c,ssl-cert", "SSL certificate", cxxopts::value<std::string>()->default_value("./localhost.pem"))
		("d,debug", "Debug", cxxopts::value<bool>()->default_value("false"))
		("h,help", "Print usage")
	;

	auto result = options.parse(argc, argv);

	// Help
	if (result.count("help")) {
		std::cout << options.help() << std::endl;
		return 0;
	}

	// Port
	int port = result["port"].as<int>();
	if (port <= 0 || port > 65535) {
		std::cerr << "Port must be between 0 and 65535" << std::endl;
		return 1;
	}

	// Key & Cert
	std::string key = result["ssl-key"].as<std::string>();
	std::string cert = result["ssl-cert"].as<std::string>();

	bool debug = result["debug"].as<bool>();
	if (debug) {
		owt::Utils::Logger::set_log_level("LOG_DEBUG");
	}

	owt::WebtorrentTracker tracker(port, key, cert);

	try {
		tracker.run();

		return 0;
	} catch (std::runtime_error e) {
		std::cout << "Error: " << e.what() << std::endl;

		return 1;
	}
}
