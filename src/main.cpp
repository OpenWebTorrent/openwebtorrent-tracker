#include "Utils.h"
#include "WebtorrentTracker.h"

int main() {
	// TODO: implement CLI options

	//owt::Utils::Logger::set_log_level("LOG_DEBUG");

	owt::WebtorrentTracker tracker(8000, "./localhost-key.pem", "./localhost.pem");
	//owt::WebtorrentTracker tracker(8000);
	tracker.run();
}
