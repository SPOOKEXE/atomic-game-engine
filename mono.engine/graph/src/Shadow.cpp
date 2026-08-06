#include <engine/core/Profiling.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/Shadow.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace engine::graph {

	namespace {
		glm::vec3 ToGlm(const core::Vector3 &value) {
			return glm::vec3{value.X, value.Y, value.Z};
		}

		// The bound of nothing, in one place because two of them answer the
		// same question.
		//
		// A unit box rather than an inverted one. Nothing here accumulates from
		// an empty sentinel — `AABB.hpp` says so and says why — and a caller
		// handed an inverted box would fit a light to a volume that contains
		// nothing.
		core::AABB BoundsOfNothing() {
			return core::AABB::FromCentre(core::Vector3::Zero, core::Vector3::One);
		}
	}

	core::AABB BoundsOfAll(std::span<const scene::DrawInstance> instances) {
		// A pass over the whole draw list rather than the culled set, which
		// is deliberate and is why it is worth a row of its own: it does not
		// get cheaper when the camera turns away.
		//
		// **The renderer does not take this path; `CullAndBound` does the same
		// walk beside the frustum test.** So a profile with no `graph.light-bounds`
		// row in it has not lost the work — look for `graph.cull-bound`. This
		// stays the entry point for a caller that wants the bound alone, and it
		// is what the tests pin.
		ENGINE_PROFILE_CAT("graph.light-bounds", core::ProfileCategory::Render);

		if (instances.empty()) {
			return BoundsOfNothing();
		}

		core::AABB bounds = BoundsOf(instances[0]);
		for (size_t index = 1; index < instances.size(); index++) {
			bounds = bounds.Union(BoundsOf(instances[index]));
		}
		return bounds;
	}

	size_t CullAndBound(
		std::span<const scene::DrawInstance> instances,
		const Frustum &frustum,
		std::vector<uint32_t> &visible,
		core::AABB &bounds
	) {
		// One row where there used to be two — `graph.cull` and
		// `graph.light-bounds` — because there is now one walk. Still a pass
		// over the whole draw list that does not get cheaper when the camera
		// turns away: culling decides what is *drawn*, not what is *looked at*.
		ENGINE_PROFILE_CAT("graph.cull-bound", core::ProfileCategory::Render);

		// Sized to the worst case once rather than grown as it fills, for the
		// reason `Cull` gives: the worst case is "everything is visible", which
		// is also the common case for a camera framing its own scene.
		visible.clear();
		visible.reserve(instances.size());

		if (instances.empty()) {
			bounds = BoundsOfNothing();
			return 0;
		}

		// **The first instance is unrolled out of the loop rather than seeded
		// from an empty box**, which is the same shape `BoundsOfAll` has and for
		// the same reason: there is no empty `AABB` to accumulate from, and
		// inventing one here would be the inverted sentinel `AABB.hpp` refuses.
		core::AABB total = BoundsOf(instances[0]);
		if (frustum.Intersects(total)) {
			visible.push_back(0);
		}

		for (size_t index = 1; index < instances.size(); index++) {
			// Derived once and used twice, which is the whole point of this
			// function. The union is unconditional and the test is not: the
			// light is fitted to everything that casts, and the frustum only
			// decides what the eye draws.
			const core::AABB box = BoundsOf(instances[index]);
			total = total.Union(box);

			if (frustum.Intersects(box)) {
				visible.push_back(static_cast<uint32_t>(index));
			}
		}

		bounds = total;
		return visible.size();
	}

	glm::mat4 FitDirectionalLight(const core::AABB &bounds, const core::Vector3 &direction) {
		const float length = direction.Magnitude();
		if (length <= 0.0f) {
			// The identity shadows nothing, which is the conservative answer: a
			// caller with no light direction has a bug, and a scene rendered
			// entirely in shadow would hide it behind a symptom that looks like
			// a renderer fault.
			return glm::mat4{1.0f};
		}

		const core::Vector3 forward = direction / length;
		const glm::vec3 centre = ToGlm(bounds.Centre());

		// The radius of the scene's bounding sphere, which is what makes the
		// fit **rotation-invariant**: a box fitted axis by axis changes size as
		// the light turns, and a shadow map that resizes every frame makes its
		// edges crawl. The sphere does not care which way the light points.
		const float radius = std::max(bounds.Size().Magnitude() * 0.5f, 1.0e-3f);

		// Placed a full radius back from the centre, so nothing that casts is
		// behind the near plane. Not "far enough": exactly the distance that
		// makes the near plane touch the sphere.
		const glm::vec3 eye = centre - ToGlm(forward) * radius;

		// An up vector that is not the light direction. Straight down is the
		// ordinary case for a sun, and `lookAt` with parallel arguments
		// produces a matrix full of NaN rather than an error.
		const glm::vec3 up =
			std::abs(forward.Y) > 0.99f ? glm::vec3{0.0f, 0.0f, 1.0f} : glm::vec3{0.0f, 1.0f, 0.0f};

		const glm::mat4 view = glm::lookAt(eye, centre, up);

		// **`glm::orthoZO`, not `glm::ortho`.** The engine pins
		// `GLM_FORCE_DEPTH_ZERO_TO_ONE` for Vulkan, and the unsuffixed name
		// follows that define — but naming the convention here means this stays
		// right if the define ever moves, and the alternative is a depth
		// comparison that is half a unit out and shadows the whole scene.
		const glm::mat4 projection = glm::orthoZO(-radius, radius, -radius, radius, 0.0f, radius * 2.0f);

		return projection * view;
	}
}
