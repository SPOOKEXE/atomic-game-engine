#include <network/Enums.hpp>

// The names, and nothing else. One file so that adding a value to any of the
// three lists is a compiler warning in exactly one place - every switch here is
// unlabelled by default on purpose.

namespace network {

	const char *Describe(Reach reach) {
		switch (reach) {
		case Reach::Loopback:
			return "loopback";
		case Reach::Lan:
			return "lan";
		case Reach::Peer:
			return "peer";
		case Reach::Remote:
			return "remote";
		}
		// No default label, so adding a reach is a warning here.
		return "?";
	}

	const char *Describe(Access access) {
		switch (access) {
		case Access::Public:
			return "public";
		case Access::Private:
			return "private";
		}
		return "?";
	}

	const char *Describe(Purpose purpose) {
		switch (purpose) {
		case Purpose::Game:
			return "game";
		case Purpose::Studio:
			return "studio";
		case Purpose::Content:
			return "content";
		}
		return "?";
	}
}
