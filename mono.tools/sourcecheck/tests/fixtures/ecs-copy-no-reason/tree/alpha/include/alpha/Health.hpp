#pragma once

// A registered component, and the enumeration it declares a field with.

namespace alpha {

	enum class Stance : uint8_t {
		Standing,
		Crouched,
	};

	struct Health {
		float Current = 100.0f;
		Stance Posture = Stance::Standing;
	};
}
