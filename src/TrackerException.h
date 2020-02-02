#ifndef OWT_TRACKER_EXCEPTION_H
#define OWT_TRACKER_EXCEPTION_H

#include <exception>
#include <string>

namespace owt {

class TrackerException : public std::exception {

public:
	TrackerException(std::string const &message) : msg_(message) {}
	virtual char const *what() const noexcept { return msg_.c_str(); }

private:
	std::string msg_;

};

};

#endif // OWT_TRACKER_EXCEPTION_H
