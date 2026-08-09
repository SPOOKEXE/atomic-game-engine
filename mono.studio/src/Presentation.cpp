#include <studio/Presentation.hpp>

namespace studio {

	float PresentationAlpha(bool advancing, engine::world::WorldState state, float accumulator) {
		return advancing && engine::world::Ticks(state) ? accumulator : 1.0f;
	}
}
