#include <engine/ecs/Store.hpp>
#include <engine/scene/Sunlight.hpp>

namespace engine::scene {

	Sun SunOf(const ecs::Store &store) {
		Sun sun;
		if (const Sun *authored = store.Resource<Sun>()) {
			sun = *authored;
		}

		// **Normalised here rather than on write.** A world setting a direction
		// is stating an aim, not a unit vector, and every consumer of this — the
		// shader's `dot`, the seam's `Rotate`, a shadow fit — wants it unit. A
		// zero vector is the one case that cannot be rescued, and it falls back
		// rather than dividing by nothing: a world with no light at all is a
		// world that turned the brightness down, which is a different edit.
		const float length = sun.Direction.Magnitude();
		sun.Direction = length > 0.0f ? sun.Direction / length : SUN_DIRECTION;

		return sun;
	}
}
