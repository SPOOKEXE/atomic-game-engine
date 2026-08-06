#include <engine/replication/Priority.hpp>

#include <algorithm>
#include <cmath>

namespace engine::replication {

	float DistancePriority::operator()(ClientId client, ecs::Entity entity) const {
		if (!Viewpoint || !Position) {
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

		if (!std::isfinite(squared) || !std::isfinite(FalloffMetres) || FalloffMetres <= 0.0f) {
			return 0.0f;
		}

		const float distance = std::sqrt(squared);
		const float score = std::clamp(1.0f - distance / FalloffMetres, 0.0f, 1.0f);

		if (!Blocked || score <= OcclusionFloor) {
			return score;
		}
		if (!Blocked(client, entity)) {
			return score;
		}

		const float kept = std::isfinite(HiddenFraction) ? std::clamp(HiddenFraction, 0.0f, 1.0f) : 0.0f;
		return score * kept;
	}
}
