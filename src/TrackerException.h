#ifndef OWT_TRACKER_EXCEPTION_H
#define OWT_TRACKER_EXCEPTION_H

#include <cstddef>
#include <exception>

namespace owt {

// Every protocol error the tracker reports to clients. The code is counted per
// reason in the metrics; the message is the "failure reason" sent to the client.
// Some messages are shared by several codes so the metrics can tell apart where
// the error happened (e.g. a bad info_hash in an announce vs. an answer).
// Codes are never reused: a removed reason's key disappears from the metrics.
#define OWT_FAILURE_REASONS(X) \
	X(NotObject,             "not_object",              "message must be a JSON object") \
	X(ActionNotString,       "action_not_string",       "action field must be a string") \
	X(EventNotString,        "event_not_string",        "event field must be a string") \
	X(UnknownAction,         "unknown_action",          "unknown action") \
	X(AnnounceInfoHash,      "announce_info_hash",      "info_hash field is missing or wrong") \
	X(AnnouncePeerId,        "announce_peer_id",        "peer_id field is missing or wrong") \
	X(OffersNotArray,        "offers_not_array",        "announce: offers field is not an array") \
	X(TooManyTorrents,       "too_many_torrents",       "announce: too many torrents on this connection") \
	X(BadOffer,              "bad_offer",               "announce: wrong offer item format") \
	X(BadOfferSdp,           "bad_offer_sdp",           "announce: wrong offer item field format") \
	X(AnswerInfoHash,        "answer_info_hash",        "info_hash field is missing or wrong") \
	X(AnswerPeerId,          "answer_peer_id",          "peer_id field is missing or wrong") \
	X(AnswerToPeerId,        "answer_to_peer_id",       "to_peer_id field is missing or wrong") \
	X(BadAnswer,             "bad_answer",              "answer: answer field is missing or wrong") \
	X(BadOfferId,            "bad_offer_id",            "answer: offer_id field is missing or wrong") \
	X(AnswerPeerIdTaken,     "answer_peer_id_taken",    "answer: peer_id belongs to another connection") \
	X(StopInfoHash,          "stop_info_hash",          "info_hash field is missing or wrong") \
	X(StopPeerId,            "stop_peer_id",            "peer_id field is missing or wrong") \
	X(FullScrapeDisabled,    "full_scrape_disabled",    "scrape: full scrape is disabled") \
	X(TooManyScrapeHashes,   "too_many_scrape_hashes",  "scrape: too many info_hash values") \
	X(BadScrapeInfoHash,     "bad_scrape_info_hash",    "scrape: info_hash field is wrong")

enum class FailureReason : unsigned char {
#define OWT_FAILURE_ENUM(name, key, message) name,
	OWT_FAILURE_REASONS(OWT_FAILURE_ENUM)
#undef OWT_FAILURE_ENUM
};

#define OWT_FAILURE_COUNT(name, key, message) + 1
constexpr size_t FAILURE_REASON_COUNT = 0 OWT_FAILURE_REASONS(OWT_FAILURE_COUNT);
#undef OWT_FAILURE_COUNT

// Short stable identifier, used as the metrics key
constexpr const char* failureReasonKey(FailureReason reason) {
	constexpr const char* keys[] = {
#define OWT_FAILURE_KEY(name, key, message) key,
		OWT_FAILURE_REASONS(OWT_FAILURE_KEY)
#undef OWT_FAILURE_KEY
	};
	return keys[static_cast<size_t>(reason)];
}

// The "failure reason" text sent to the client
constexpr const char* failureReasonMessage(FailureReason reason) {
	constexpr const char* messages[] = {
#define OWT_FAILURE_MESSAGE(name, key, message) message,
		OWT_FAILURE_REASONS(OWT_FAILURE_MESSAGE)
#undef OWT_FAILURE_MESSAGE
	};
	return messages[static_cast<size_t>(reason)];
}

class TrackerException : public std::exception {

public:
	explicit TrackerException(FailureReason reason) : reason_(reason) {}
	FailureReason reason() const noexcept { return reason_; }
	virtual char const *what() const noexcept { return failureReasonMessage(reason_); }

private:
	FailureReason reason_;

};

};

#endif // OWT_TRACKER_EXCEPTION_H
