#include <array>
#include <unified/Arrangement.hpp>

namespace unified {

	namespace {
		// One axis's names, in enumerator order.
		//
		// **Parsed against the same table they are printed from**, so a name
		// that comes out of `Name` goes back in through `ParseArrangement` and
		// there is nowhere for the two spellings to drift apart.
		constexpr std::array<std::string_view, 3> TRANSPORTS{"direct", "loopback", "lossy"};
		constexpr std::array<std::string_view, 2> CONTENTS{"", "relayed"};
		constexpr std::array<std::string_view, 2> DISCOVERIES{"", "advertised"};
	}

	std::string_view Name(Transport carrying) {
		return TRANSPORTS[static_cast<size_t>(carrying)];
	}

	std::string_view Name(Content serving) {
		return CONTENTS[static_cast<size_t>(serving)];
	}

	std::string_view Name(Discovery finding) {
		return DISCOVERIES[static_cast<size_t>(finding)];
	}

	std::string Arrangement::Name() const {
		// The transport is always spelled, because there is no transport that
		// means "none" - even `direct` is a choice about where the seam is.
		std::string name(unified::Name(Carrying));

		if (Serving != Content::None) {
			name += '+';
			name += unified::Name(Serving);
		}
		if (Finding != Discovery::None) {
			name += '+';
			name += unified::Name(Finding);
		}
		return name;
	}

	std::optional<Arrangement> ParseArrangement(std::string_view name) {
		Arrangement arrangement;
		bool namedTransport = false;
		bool namedContent = false;
		bool namedDiscovery = false;

		size_t start = 0;
		while (start <= name.size()) {
			const size_t plus = name.find('+', start);
			const std::string_view field =
				name.substr(start, plus == std::string_view::npos ? std::string_view::npos : plus - start);

			// An empty field is `direct++relayed` or a trailing `+`. Refused
			// rather than skipped: it is a typo either way, and a parser that
			// forgives one runs something the caller did not ask for.
			if (field.empty()) {
				return std::nullopt;
			}

			bool matched = false;
			for (size_t index = 0; index < TRANSPORTS.size() && !matched; index++) {
				if (field != TRANSPORTS[index]) {
					continue;
				}
				if (namedTransport) {
					return std::nullopt;
				}
				arrangement.Carrying = static_cast<Transport>(index);
				namedTransport = true;
				matched = true;
			}
			// Index one upwards: the zeroth entry of each optional axis is the
			// empty name, which no field can equal.
			for (size_t index = 1; index < CONTENTS.size() && !matched; index++) {
				if (field != CONTENTS[index]) {
					continue;
				}
				if (namedContent) {
					return std::nullopt;
				}
				arrangement.Serving = static_cast<Content>(index);
				namedContent = true;
				matched = true;
			}
			for (size_t index = 1; index < DISCOVERIES.size() && !matched; index++) {
				if (field != DISCOVERIES[index]) {
					continue;
				}
				if (namedDiscovery) {
					return std::nullopt;
				}
				arrangement.Finding = static_cast<Discovery>(index);
				namedDiscovery = true;
				matched = true;
			}

			if (!matched) {
				return std::nullopt;
			}
			if (plus == std::string_view::npos) {
				break;
			}
			start = plus + 1;
		}

		// A name that mentioned no transport is `relayed`, which describes half
		// an arrangement. `direct+relayed` is the thing it probably meant and
		// guessing at it is how a matrix silently runs eleven of its twelve.
		if (!namedTransport) {
			return std::nullopt;
		}
		return arrangement;
	}

	std::vector<Arrangement> AllArrangements() {
		std::vector<Arrangement> all;
		all.reserve(TRANSPORTS.size() * CONTENTS.size() * DISCOVERIES.size());

		for (size_t carrying = 0; carrying < TRANSPORTS.size(); carrying++) {
			for (size_t serving = 0; serving < CONTENTS.size(); serving++) {
				for (size_t finding = 0; finding < DISCOVERIES.size(); finding++) {
					all.push_back(
						Arrangement{
							.Carrying = static_cast<Transport>(carrying),
							.Serving = static_cast<Content>(serving),
							.Finding = static_cast<Discovery>(finding),
						}
					);
				}
			}
		}
		return all;
	}
}
