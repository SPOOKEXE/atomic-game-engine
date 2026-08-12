#include <engine/core/Profiling.hpp>
#include <engine/graph/Cull.hpp>

namespace engine::graph {

	core::AABB BoundsOf(const scene::DrawInstance &instance) {
		return core::AABB::FromOrientedBox(instance.Frame, instance.HalfExtent);
	}

	size_t Cull(
		std::span<const scene::DrawInstance> instances, const Frustum &frustum, std::vector<uint32_t> &visible
	) {
		ENGINE_PROFILE_CAT("graph.cull", core::ProfileCategory::Render);

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

	size_t VisibleSurfaces(
		std::span<const scene::DrawInstance> instances,
		const Frustum &camera,
		std::span<const SurfaceEye> surfaces,
		std::span<bool> visible
	) {
		ENGINE_PROFILE_CAT("graph.surface visibility", core::ProfileCategory::Render);

		for (size_t slot = 0; slot < visible.size(); slot++) {
			visible[slot] = false;
		}

		if (surfaces.empty()) {
			return 0;
		}

		// The box each slot's pane occupies, unioned over every instance naming
		// it — which is one instance in every scene anybody has built, and a
		// union rather than an assignment because nothing forbids two.
		//
		// **Kept between calls rather than sized per frame.** This runs once per
		// viewport per frame in the middle of the render path, and two vectors
		// built and thrown away there is two allocations a frame for sixteen
		// boxes. Thread-local because a host may draw two panels from two
		// threads, which is the same reason `scene`'s own scratch is.
		static thread_local std::vector<core::AABB> panes;
		static thread_local std::vector<uint8_t> present;
		panes.assign(visible.size(), core::AABB{});
		present.assign(visible.size(), 0u);

		for (const scene::DrawInstance &instance : instances) {
			if (instance.Surface < 0 || static_cast<size_t>(instance.Surface) >= visible.size()) {
				continue;
			}

			const auto slot = static_cast<size_t>(instance.Surface);
			const core::AABB box = BoundsOf(instance);
			panes[slot] = present[slot] != 0u ? panes[slot].Union(box) : box;
			present[slot] = 1u;
		}

		size_t seen = 0;
		for (const SurfaceEye &eye : surfaces) {
			if (eye.Index < 0 || static_cast<size_t>(eye.Index) >= visible.size()) {
				continue;
			}

			const auto slot = static_cast<size_t>(eye.Index);
			visible[slot] = present[slot] != 0u && camera.Intersects(panes[slot]);
			seen += visible[slot] ? 1u : 0u;
		}

		// **Read from a snapshot, so the answer cannot depend on the order the
		// surfaces happen to arrive in.** Marking as it walked would let one
		// pane's newly granted visibility grant another's within the same sweep,
		// which is the closure this deliberately is not.
		static thread_local std::vector<uint8_t> onScreen;
		onScreen.assign(visible.begin(), visible.end());

		for (const SurfaceEye &eye : surfaces) {
			if (eye.Index < 0 || static_cast<size_t>(eye.Index) >= visible.size()) {
				continue;
			}

			const auto slot = static_cast<size_t>(eye.Index);
			if (onScreen[slot] != 0u || present[slot] == 0u) {
				continue;
			}

			for (const SurfaceEye &other : surfaces) {
				if (other.Index == eye.Index || other.Index < 0 ||
					static_cast<size_t>(other.Index) >= visible.size() ||
					onScreen[static_cast<size_t>(other.Index)] == 0u) {
					continue;
				}

				if (Frustum::FromViewProjection(other.ViewProjection).Intersects(panes[slot])) {
					visible[slot] = true;
					seen++;
					break;
				}
			}
		}

		return seen;
	}
}
