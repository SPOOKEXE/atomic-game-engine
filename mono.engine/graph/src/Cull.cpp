#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/Cull.hpp>

#include <algorithm>
#include <cmath>

namespace engine::graph {

	core::AABB BoundsOf(const scene::DrawInstance &instance) {
		return core::OrientedBoxBounds(instance.Frame, instance.HalfExtent);
	}

	size_t Cull(
		std::span<const scene::DrawInstance> instances, const Frustum &frustum, std::vector<uint32_t> &visible
	) {
		ENGINE_PROFILE_CAT("graph.cull", core::ProfileCategory::Render);

		// Sized to the worst case once rather than grown as it fills. The worst
		// case is "everything is visible", which is also the common case for a
		// camera framing its own scene - so a reserve that assumed otherwise
		// would reallocate on exactly the frames that matter.
		visible.clear();
		visible.reserve(instances.size());

		for (size_t index = 0; index < instances.size(); index++) {
			if (frustum.Intersects(BoundsOf(instances[index]))) {
				visible.push_back(static_cast<uint32_t>(index));
			}
		}

		// Outside the loop, once per view per frame. What the frustum kept
		// against what it was offered is the number that answers "is the cull
		// doing anything" and "is it doing too much".
		core::Metrics::Count("graph.cull.tested", static_cast<double>(instances.size()));
		core::Metrics::Count("graph.cull.visible", static_cast<double>(visible.size()));
		return visible.size();
	}

	namespace {
		// How much of the screen a world box covers, as a fraction of the larger
		// viewport axis.
		//
		// **Clip space and not the frustum**, because a frustum answers "is any
		// of it in" and this needs "how much". The eight corners are projected
		// and their normalised bounding box measured; the span of normalised
		// device coordinates is two, so half the larger span is the fraction.
		//
		// **A corner at or behind the camera's plane gives one.** Its projection
		// is unbounded and its sign flips, so any number derived from it is
		// noise - and a box straddling the camera is one filling the screen,
		// which is the answer "one" already means.
		float ScreenCoverage(const glm::mat4 &camera, const core::AABB &box) {
			float left = 1.0f;
			float right = -1.0f;
			float bottom = 1.0f;
			float top = -1.0f;

			for (int corner = 0; corner < 8; corner++) {
				const glm::vec4 point(
					(corner & 1) != 0 ? box.Maximum.X : box.Minimum.X,
					(corner & 2) != 0 ? box.Maximum.Y : box.Minimum.Y,
					(corner & 4) != 0 ? box.Maximum.Z : box.Minimum.Z,
					1.0f
				);

				const glm::vec4 clip = camera * point;
				if (!(clip.w > 1.0e-4f)) {
					return 1.0f;
				}

				const float x = clip.x / clip.w;
				const float y = clip.y / clip.w;
				left = std::min(left, x);
				right = std::max(right, x);
				bottom = std::min(bottom, y);
				top = std::max(top, y);
			}

			return std::clamp(std::max(right - left, top - bottom) * 0.5f, 0.0f, 1.0f);
		}
	}

	size_t VisibleSurfaces(
		std::span<const scene::DrawInstance> instances,
		const glm::mat4 &cameraMatrix,
		std::span<const SurfaceEye> surfaces,
		std::span<bool> visible,
		std::span<float> coverage,
		size_t rounds
	) {
		ENGINE_PROFILE_CAT("graph.surface visibility", core::ProfileCategory::Render);

		const Frustum camera = Frustum::FromViewProjection(cameraMatrix);

		for (size_t slot = 0; slot < visible.size(); slot++) {
			visible[slot] = false;
		}
		for (size_t slot = 0; slot < coverage.size(); slot++) {
			coverage[slot] = 0.0f;
		}

		if (surfaces.empty()) {
			return 0;
		}

		// The box each slot's pane occupies, unioned over every instance naming
		// it - which is one instance in every scene anybody has built, and a
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

			if (slot < coverage.size() && visible[slot]) {
				coverage[slot] = ScreenCoverage(cameraMatrix, panes[slot]);
			}
		}

		// **Read from a snapshot, so the answer cannot depend on the order the
		// surfaces happen to arrive in.** Marking as it walked would let one
		// pane's newly granted visibility grant another's within the same sweep,
		// and which panes got that head start would come down to the order the
		// surfaces were gathered in.
		//
		// **One round per bounce the surface pass will draw.** A round is one
		// level of surface-seen-in-surface, and running a single one - which is
		// what this did until the mismatch below was found - culls every level
		// past the first while the pass goes on drawing them. See `Cull.hpp`.
		static thread_local std::vector<uint8_t> onScreen;

		for (size_t round = 0; round < std::max<size_t>(rounds, 1); round++) {
			onScreen.assign(visible.begin(), visible.end());
			const size_t before = seen;

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

			// **Stopped when a round grants nothing**, which is what keeps asking
			// for more rounds than the scene has depth from costing anything. It
			// is also the termination proof: `seen` only ever rises and is bounded
			// by the slot count, so a round that adds nothing is a fixpoint.
			if (seen == before) {
				ENGINE_TRACE(
					"surface visibility settled after {} round(s) with {} pane(s) seen", round + 1, seen
				);
				break;
			}
		}

		return seen;
	}

	bool VisiblePane(
		const glm::mat4 &camera,
		const core::Vector3 &centre,
		const core::Vector3 &first,
		const core::Vector3 &second
	) {
		// **The absolute half-axes summed, which is `OrientedBoxBounds` written for
		// a rectangle.** Whatever way the pane is turned, its box reaches
		// `|first| + |second|` from the centre on each world axis - and the axis
		// the rectangle has no thickness on comes out zero, which is exact rather
		// than something to guard.
		const core::Vector3 reach{
			std::abs(first.X) + std::abs(second.X),
			std::abs(first.Y) + std::abs(second.Y),
			std::abs(first.Z) + std::abs(second.Z),
		};

		return Frustum::FromViewProjection(camera).Intersects(core::AABB::FromCentre(centre, reach));
	}
}
