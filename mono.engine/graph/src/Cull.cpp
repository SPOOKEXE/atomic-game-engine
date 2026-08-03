#include <engine/graph/Cull.hpp>

namespace engine::graph {

	core::AABB BoundsOf(const scene::DrawInstance &instance) {
		return core::AABB::FromOrientedBox(instance.Frame, instance.HalfExtent);
	}

	size_t Cull(
		std::span<const scene::DrawInstance> instances, const Frustum &frustum, std::vector<uint32_t> &visible
	) {
		// Sized to the worst case once rather than grown as it fills. The worst
		// case is "everything is visible", which is also the common case for a
		// camera framing its own scene — so a reserve that assumed otherwise
		// would reallocate on exactly the frames that matter.
		visible.clear();
		visible.reserve(instances.size());

		for (size_t index = 0; index < instances.size(); index++) {
			if (frustum.Intersects(BoundsOf(instances[index]))) {
				visible.push_back(static_cast<uint32_t>(index));
			}
		}
		return visible.size();
	}
}
