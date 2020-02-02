#ifndef OWT_UTILS_H
#define OWT_UTILS_H

#include <string>
#include <iostream>

namespace owt {
namespace Utils {
namespace Logger {
	enum LogLevel {
		LOG_NOTHING,
		LOG_CRITICAL,
		LOG_ERROR,
		LOG_WARNING,
		LOG_INFO,
		LOG_DEBUG
	};

	LogLevel LOG_LEVEL;

	inline void set_log_level(std::string level) {
		if (level == "LOG_NOTHING")
			owt::Utils::Logger::LOG_LEVEL = owt::Utils::Logger::LogLevel::LOG_NOTHING;
		else if (level == "LOG_CRITICAL")
			owt::Utils::Logger::LOG_LEVEL = owt::Utils::Logger::LogLevel::LOG_CRITICAL;
		else if (level == "LOG_ERROR")
			owt::Utils::Logger::LOG_LEVEL = owt::Utils::Logger::LogLevel::LOG_ERROR;
		else if (level == "LOG_WARNING")
			owt::Utils::Logger::LOG_LEVEL = owt::Utils::Logger::LogLevel::LOG_WARNING;
		else if (level == "LOG_INFO")
			owt::Utils::Logger::LOG_LEVEL = owt::Utils::Logger::LogLevel::LOG_INFO;
		else if (level == "LOG_DEBUG")
			owt::Utils::Logger::LOG_LEVEL = owt::Utils::Logger::LogLevel::LOG_DEBUG;
		else
			std::cerr << "[OWT] " << __FILE__ << ":" << __LINE__ << " Error: Unknown log level" << std::endl;
	}
};
};
};

#define EOUT(content) std::cerr << content << std::endl

#define DOUT(content) \
	if (owt::Utils::Logger::LOG_LEVEL >= owt::Utils::Logger::LogLevel::LOG_DEBUG) \
		std::cout << content << std::endl

#define IOUT(content) \
	if (owt::Utils::Logger::LOG_LEVEL >= owt::Utils::Logger::LogLevel::LOG_INFO) \
		std::cout << "Info: "<< content << std::endl

#define COUT(content) \
	std::cout << content << std::endl

#endif // OWT_UTILS_H
