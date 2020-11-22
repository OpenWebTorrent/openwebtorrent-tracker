#include <exception>
#include <iostream>

#include "Utils.h"
#include "WebtorrentTracker.h"

int main() {
	// TODO: implement CLI options

	//owt::Utils::Logger::set_log_level("LOG_DEBUG");

	owt::WebtorrentTracker tracker(8000, "./localhost-key.pem", "./localhost.pem");
	//owt::WebtorrentTracker tracker(8000);

	try {
		tracker.run();
	} catch (std::runtime_error e) {
		std::cout << "Error: " << e.what() << std::endl;
	}
}
