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
	}

	core::AABB BoundsOfAll(std::span<const scene::DrawInstance> instances) {
		// A pass over the whole draw list rather than the culled set, which
		// is deliberate and is why it is worth a row of its own: it does not
		// get cheaper when the camera turns away.
		ENGINE_PROFILE_CAT("graph.light-bounds", core::ProfileCategory::Render);

		if (instances.empty()) {
			// A unit box rather than an inverted one. Nothing here accumulates
			// from an empty sentinel — `AABB.hpp` says so and says why — and a
			// caller handed an inverted box would fit a light to a volume that
			// contains nothing.
			return core::AABB::FromCentre(core::Vector3::Zero, core::Vector3::One);
		}

		core::AABB bounds = BoundsOf(instances[0]);
		for (size_t index = 1; index < instances.size(); index++) {
			bounds = bounds.Union(BoundsOf(instances[index]));
		}
		return bounds;
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
