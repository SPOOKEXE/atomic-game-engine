#pragma once

// arch-waiver public-header: a fixture leaf. Nothing outside the fixture
// includes anything, so every fixture that is not about this rule says so once.

#include <alpha/Health.hpp>

namespace beta {

	// A long-lived object: it declares a function, so it is not an argument list.
	class Panel {
	  public:
		void Draw();

	  private:
		// arch-waiver ecs-copy: a fixture reason, which is what makes the waiver
		// count. The rule is switched off here and nowhere else.
		alpha::Health Bar;
	};
}
