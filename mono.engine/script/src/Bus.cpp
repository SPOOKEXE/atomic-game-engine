// The topic table and the status word, which are a map and a switch.
//
// `Bus.hpp` carries the argument for why either exists here; there is nothing
// below that is not obvious from the header.
//
// @tier L9 · shared

#include <engine/script/Bus.hpp>
#include <engine/script/Vocabulary.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace engine::script {

	void TopicSubscriptions::Add(std::string_view topic, CallbackRef callback) {
		Topics[std::string(topic)].push_back(callback);
	}

	std::span<const CallbackRef> TopicSubscriptions::Listeners(std::string_view topic) const {
		const auto found = Topics.find(std::string(topic));
		if (found == Topics.end()) {
			return {};
		}
		return found->second;
	}

	const char *DescribeStatus(world::BusStatus status) {
		switch (status) {
		case world::BusStatus::Ok:
			return "Ok";
		case world::BusStatus::NotFound:
			return "NotFound";
		case world::BusStatus::Conflict:
			return "Conflict";
		case world::BusStatus::OverBudget:
			return "OverBudget";
		case world::BusStatus::NoSuchWorld:
			return "NoSuchWorld";
		case world::BusStatus::Unsupported:
			return "Unsupported";
		case world::BusStatus::NoSuchChannel:
			return "NoSuchChannel";
		case world::BusStatus::Overflow:
			return "Overflow";
		case world::BusStatus::WorldNotReady:
			return "WorldNotReady";
		case world::BusStatus::TooManyChannels:
			return "TooManyChannels";
		}
		return "Unknown";
	}

	std::span<const std::string_view> BusStatusWords() {
		static const std::vector<std::string_view> WORDS = [] {
			// **Walked rather than listed, so the switch above is the only
			// place a status is spelled.** `DescribeStatus` answers `"Unknown"`
			// for anything it does not recognise, which is exactly the end of
			// the enum - and a status appended to `world::BusStatus` without a
			// case here is a `-Wswitch` error rather than a word this walk
			// silently stops before.
			//
			// The cast is defined: `BusStatus` has a fixed `uint8_t` underlying
			// type, so every value in that range is a valid enumeration value
			// whether or not it is a named one.
			std::vector<std::string_view> words;
			for (uint32_t ordinal = 0; ordinal <= UINT8_MAX; ordinal++) {
				const std::string_view word =
					DescribeStatus(static_cast<world::BusStatus>(static_cast<uint8_t>(ordinal)));
				if (word == "Unknown") {
					break;
				}
				words.push_back(word);
			}

			// Last, and a member rather than a fallback: a script handed one by
			// a process built against a newer enum has to be able to compare
			// against it.
			words.emplace_back("Unknown");
			return words;
		}();
		return WORDS;
	}
}
