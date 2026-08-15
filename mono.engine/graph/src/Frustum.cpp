#include <engine/graph/Frustum.hpp>

#include <cmath>

namespace engine::graph {

	namespace {
		// One plane from a row combination, normalised.
		//
		// The normalisation divides all four components by the normal's length,
		// which is what turns the algebraic inequality into a metric one - so
		// `SignedDistance` reports metres rather than an arbitrary scale.
		//
		// A degenerate row - a projection with a zero field of view, or an
		// identity matrix where two rows cancel - would divide by zero. Left as
		// the zero plane instead, which accepts everything: the conservative
		// direction, and the one that shows up as "culling did nothing" rather
		// than as a world full of holes.
		Plane Normalised(float x, float y, float z, float w) {
			const float length = std::sqrt(x * x + y * y + z * z);
			if (length <= 0.0f) {
				return Plane{core::Vector3{0.0f, 0.0f, 0.0f}, 1.0f};
			}

			return Plane{core::Vector3{x / length, y / length, z / length}, w / length};
		}
	}

	Frustum Frustum::FromViewProjection(const glm::mat4 &viewProjection) {
		// glm is column-major, so `m[column][row]`. Written out as rows below,
		// because the extraction is stated in terms of matrix *rows* everywhere
		// it is described and transcribing it in the other layout is how a sign
		// ends up on the wrong plane.
		const auto row = [&viewProjection](int index) {
			return glm::vec4{
				viewProjection[0][index],
				viewProjection[1][index],
				viewProjection[2][index],
				viewProjection[3][index],
			};
		};

		const glm::vec4 x = row(0);
		const glm::vec4 y = row(1);
		const glm::vec4 z = row(2);
		const glm::vec4 w = row(3);

		Frustum frustum;

		// **Near is `z` alone, not `w + z`.** Vulkan's clip volume is
		// `0 ≤ z ≤ w` where OpenGL's is `-w ≤ z ≤ w`, and this engine pins the
		// former with `GLM_FORCE_DEPTH_ZERO_TO_ONE`. The OpenGL form here would
		// put the near plane behind the camera and clip the whole scene.
		frustum.Planes[Near] = Normalised(z.x, z.y, z.z, z.w);
		frustum.Planes[Far] = Normalised(w.x - z.x, w.y - z.y, w.z - z.z, w.w - z.w);

		frustum.Planes[Left] = Normalised(w.x + x.x, w.y + x.y, w.z + x.z, w.w + x.w);
		frustum.Planes[Right] = Normalised(w.x - x.x, w.y - x.y, w.z - x.z, w.w - x.w);
		frustum.Planes[Bottom] = Normalised(w.x + y.x, w.y + y.y, w.z + y.z, w.w + y.w);
		frustum.Planes[Top] = Normalised(w.x - y.x, w.y - y.y, w.z - y.z, w.w - y.w);

		return frustum;
	}

	bool Frustum::Intersects(const core::AABB &box) const {
		for (const Plane &plane : Planes) {
			// The **positive vertex**: the corner furthest along the inward
			// normal. If even that corner is outside, every corner is, and the
			// box is rejected - which is what makes this test never reject
			// something visible.
			const core::Vector3 furthest{
				plane.Normal.X >= 0.0f ? box.Maximum.X : box.Minimum.X,
				plane.Normal.Y >= 0.0f ? box.Maximum.Y : box.Minimum.Y,
				plane.Normal.Z >= 0.0f ? box.Maximum.Z : box.Minimum.Z,
			};

			if (plane.SignedDistance(furthest) < 0.0f) {
				return false;
			}
		}
		return true;
	}

	bool Frustum::Intersects(const core::Vector3 &centre, float radius) const {
		for (const Plane &plane : Planes) {
			if (plane.SignedDistance(centre) < -radius) {
				return false;
			}
		}
		return true;
	}

	bool Frustum::Contains(const core::Vector3 &point) const {
		for (const Plane &plane : Planes) {
			if (plane.SignedDistance(point) < 0.0f) {
				return false;
			}
		}
		return true;
	}
}
