#pragma once

// arch-waiver public-header: a fixture leaf. Nothing outside the fixture
// includes anything, so every fixture that is not about this rule says so once.

namespace world {

	struct Payload {
		std::vector<uint8_t> Bytes;
	};

	// arch-crossing - every field of this is a copy, which is the rule.
	struct Envelope {
		uint64_t Sequence = 0;
		Payload Body;
	};
}
