#include <discord/Activity.hpp>

namespace discord {

	namespace {
		// Whether a byte continues a UTF-8 sequence rather than starting one.
		bool Continues(unsigned char byte) {
			return (byte & 0xC0u) == 0x80u;
		}
	}

	bool Activity::IsEmpty() const {
		return Details.empty() && State.empty() && LargeImage.empty() && StartedUnixSeconds == 0 &&
			   PartySize == 0 && Buttons.empty();
	}

	std::string Clamp(std::string_view text, size_t limit) {
		if (limit == 0) {
			return {};
		}

		// **Walked rather than measured first.** The overwhelming case is text
		// well under the limit, and this way that case is one pass which stops
		// at the end rather than two passes over the whole string.
		size_t characters = 0;
		size_t index = 0;
		while (index < text.size()) {
			if (!Continues(static_cast<unsigned char>(text[index]))) {
				if (characters == limit) {
					// `index` is the start of the character that would be the
					// one too many, so cutting here keeps exactly `limit` and
					// never splits a sequence.
					return std::string(text.substr(0, index));
				}
				characters++;
			}
			index++;
		}
		return std::string(text);
	}

	std::string Fill(std::string_view pattern, const Facts &facts, size_t limit) {
		std::string built;
		built.reserve(pattern.size());

		size_t index = 0;
		while (index < pattern.size()) {
			const char here = pattern[index];
			if (here != '{') {
				built.push_back(here);
				index++;
				continue;
			}

			const size_t close = pattern.find('}', index + 1);
			if (close == std::string_view::npos) {
				// **A brace with no partner is literal.** A template is a thing
				// somebody types, and a typo should cost them a stray character
				// rather than the rest of the line.
				built.push_back(here);
				index++;
				continue;
			}

			const std::string_view token = pattern.substr(index + 1, close - index - 1);
			for (const auto &[name, value] : facts) {
				if (name == token) {
					built += value;
					break;
				}
			}

			// An unknown token contributes nothing and is not an error. See the
			// header: showing somebody `{capacity}` on their own profile is
			// worse than showing them a gap.
			index = close + 1;
		}

		return Clamp(built, limit);
	}
}
