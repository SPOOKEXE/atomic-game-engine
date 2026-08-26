#pragma once

// arch-waiver public-header: a fixture leaf. Nothing outside the fixture
// includes anything, so every fixture that is not about this rule says so once.

#include <alpha/Health.hpp>

namespace beta {

	// Reads the component out of the store rather than keeping one, so there is
	// nothing here for the ECS-copy rule to find.
	class Panel {
	  public:
		void Draw(const alpha::Health &health);
	};

	// An argument list with no behaviour. A component travelling to a call is a
	// value, and the rule says so by not firing here.
	struct DrawRequest {
		alpha::Health Snapshot;
	};
}
