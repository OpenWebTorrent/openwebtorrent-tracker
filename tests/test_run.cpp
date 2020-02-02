#include "WebtorrentTracker.h"

int main() {
	owt::WebtorrentTracker tracker(9001);
	tracker.run();
}
