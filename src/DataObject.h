#ifndef OWT_DATA_OBJECT_H
#define OWT_DATA_OBJECT_H

#include <json.hpp>

using namespace nlohmann;

namespace owt {

	struct ServerStats {
		std::string server;
		int websocketCount;
	};

	static void to_json(json& j, const ServerStats& value) {
		j = json{{"server", value.server}, {"websocketCount", value.websocketCount}};
	}

	static void from_json(const json& j, ServerStats& value) {
		j.at("server").get_to(value.server);
		j.at("websocketCount").get_to(value.websocketCount);
	}

	struct ProcessMemory {
		long int rss;
		int heapTotal;
		int heapUsed;
		int external;
	};

	static void to_json(json& j, const ProcessMemory& value) {
		j = json{
			{"rss", value.rss},
			{"heapTotal", value.heapTotal},
			{"heapUsed", value.heapUsed},
			{"external", value.external}
		};
	}

	static void from_json(const json& j, ProcessMemory& value) {
		j.at("rss").get_to(value.rss);
		j.at("heapTotal").get_to(value.heapTotal);
		j.at("heapUsed").get_to(value.heapUsed);
		j.at("external").get_to(value.external);
	}

};

#endif //OWT_DATA_OBJECT_H
