#pragma once

// arch-waiver public-header: a fixture leaf. Nothing outside the fixture
// includes anything, so every fixture that is not about this rule says so once.

namespace world {

	struct Payload {
		std::span<const uint8_t> Bytes;
	};

	// arch-crossing - what leaves a world, so every field of it is walked.
	struct Envelope {
		uint64_t Sequence = 0;
		Payload Body;
	};
}
