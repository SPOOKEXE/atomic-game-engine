#include <engine/replication/Priority.hpp>

#include <algorithm>
#include <cmath>

namespace engine::replication {

	float DistancePriority::operator()(ClientId client, ecs::Entity entity) const {
		if (!Viewpoint || !Position) {
			// A scorer missing half of itself scores everything the same, which
			// leaves the rotation in charge — the behaviour `Authority` has
			// without a hook at all, and the safe thing for a host that wired
			// one accessor and forgot the other.
			return 0.0f;
		}

		core::Vector3 eye;
		core::Vector3 at;
		if (!Viewpoint(client, eye) || !Position(entity, at)) {
			return 0.0f;
		}

		const float dx = at.X - eye.X;
		const float dy = at.Y - eye.Y;
		const float dz = at.Z - eye.Z;
		const float squared = dx * dx + dy * dy + dz * dz;

		// **The square root is taken, unlike every other distance comparison in
		// this engine.** Elsewhere the squared value is compared against another
		// squared value and the root is a monotonic step nothing needs; here the
		// result is a *score on a linear ramp*, so squaring it would bend the
		// ramp into the inverse-square shape the header rejects.
		//
		// Guarded against a falloff of zero or a non-finite accessor result,
		// because both come from a caller and neither should produce an
		// undefined ordering.
		if (!std::isfinite(squared) || !std::isfinite(FalloffMetres) || FalloffMetres <= 0.0f) {
			return 0.0f;
		}

		const float distance = std::sqrt(squared);
		return std::clamp(1.0f - distance / FalloffMetres, 0.0f, 1.0f);
	}
}
