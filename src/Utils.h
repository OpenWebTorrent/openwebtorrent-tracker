#ifndef OWT_UTILS_H
#define OWT_UTILS_H

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <iostream>
#include <arpa/inet.h>

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

	inline LogLevel LOG_LEVEL = LogLevel::LOG_NOTHING;

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

	// Number of code points in a UTF-8 string if all of them are <= U+00FF
	// (a JS "binary string"), or -1 otherwise.
	inline int binaryStringLength(const std::string& value) {
		int length = 0;
		for (size_t i = 0; i < value.size(); i++) {
			unsigned char c = value[i];
			if (c >= 0x80) {
				// U+0080..U+00FF are encoded as 0xC2/0xC3 followed by a continuation byte
				if ((c != 0xC2 && c != 0xC3) || i + 1 >= value.size() || (value[i + 1] & 0xC0) != 0x80) {
					return -1;
				}
				i++;
			}
			length++;
		}
		return length;
	}

	// Key used to count connections per client. IPv4-mapped IPv6 addresses
	// are treated as IPv4, other IPv6 addresses are grouped by /64 since a
	// single host usually owns a whole /64. Takes the raw 4 or 16 address bytes.
	inline std::string addressKey(std::string_view binary) {
		char text[INET6_ADDRSTRLEN] = {};
		const unsigned char* b = (const unsigned char*) binary.data();

		if (binary.size() == 16) {
			static const unsigned char v4Mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
			if (std::equal(b, b + 12, v4Mapped)) {
				return addressKey(binary.substr(12));
			}
			unsigned char prefix[16] = {};
			std::copy(b, b + 8, prefix);
			inet_ntop(AF_INET6, prefix, text, sizeof(text));
			return std::string(text) + "/64";
		}
		if (binary.size() == 4) {
			inet_ntop(AF_INET, b, text, sizeof(text));
			return text;
		}
		return "";
	}

	// Same as above for a textual address (e.g. from X-Forwarded-For)
	inline std::string addressKeyFromText(std::string_view address) {
		std::string text(address);
		unsigned char binary[16];
		if (inet_pton(AF_INET, text.c_str(), binary) == 1) {
			return addressKey(std::string_view((char*) binary, 4));
		}
		if (inet_pton(AF_INET6, text.c_str(), binary) == 1) {
			return addressKey(std::string_view((char*) binary, 16));
		}
		return "";
	}

	// Classic token bucket: allows `burst` messages at once, refilled at
	// `rate` messages per second. A rate of 0 disables limiting.
	class TokenBucket {
	public:
		using Clock = std::chrono::steady_clock;

		TokenBucket() = default;

		TokenBucket(double rate, double burst, Clock::time_point now = Clock::now())
			: rate(rate), burst(std::max(burst, 1.0)), tokens(std::max(burst, 1.0)), last(now) {}

		bool consume(Clock::time_point now = Clock::now()) {
			if (this->rate <= 0) {
				return true;
			}

			double elapsed = std::chrono::duration<double>(now - this->last).count();
			this->tokens = std::min(this->burst, this->tokens + elapsed * this->rate);
			this->last = now;

			if (this->tokens < 1) {
				return false;
			}
			this->tokens -= 1;
			return true;
		}

	private:
		double rate = 0;
		double burst = 1;
		double tokens = 1;
		Clock::time_point last;
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
