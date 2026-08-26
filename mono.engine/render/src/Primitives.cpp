#include "Primitives.hpp"

#include <algorithm>
#include <cmath>

namespace engine::render {

	AtlasQuadrant BeamQuadrant(uint32_t index, uint32_t resolution) {
		const uint32_t slot = index % 4u;
		const auto half = static_cast<float>(resolution / 2u);

		AtlasQuadrant quadrant;
		quadrant.X = static_cast<float>(slot % 2u) * half;
		quadrant.Y = static_cast<float>(slot / 2u) * half;
		quadrant.Width = half;
		quadrant.Height = half;

		// The lookup window is the same rectangle in 0..1, and it is derived from
		// the texel one rather than written out again - a beam whose viewport and
		// whose window disagreed would draw into one quadrant and sample another.
		quadrant.Window = glm::vec4{
			0.5f,
			0.5f,
			static_cast<float>(slot % 2u) * 0.5f,
			static_cast<float>(slot / 2u) * 0.5f,
		};
		return quadrant;
	}

	bool CanvasFacesViewer(const core::Vector3 &normal, const core::Vector3 &toViewer) {
		return normal.Dot(toViewer) > 0.0f;
	}

	float CanvasPixelsPerStud(
		const glm::mat4 &viewProjection,
		const core::Vector3 &anchor,
		const core::Vector3 &up,
		float canvasHeight
	) {
		const glm::vec4 projectedAnchor = viewProjection * glm::vec4{anchor.X, anchor.Y, anchor.Z, 1.0f};
		const core::Vector3 oneUp = anchor + up;
		const glm::vec4 projectedUp = viewProjection * glm::vec4{oneUp.X, oneUp.Y, oneUp.Z, 1.0f};

		// **`abs` on the w as well as on the difference.** A point behind the eye
		// projects with a negative w, and dividing by it flips the sign of a
		// height that is about to be divided into a size.
		const float perStud = std::abs(
								  projectedUp.y / std::max(std::abs(projectedUp.w), 1.0e-5f) -
								  projectedAnchor.y / std::max(std::abs(projectedAnchor.w), 1.0e-5f)
							  ) *
							  canvasHeight * 0.5f;
		return std::max(perStud, 1.0e-5f);
	}

	SpatialQuad BillboardQuad(
		const core::Vector3 &anchor,
		const core::Vector3 &right,
		const core::Vector3 &up,
		const core::Vector3 &toCamera,
		const core::Vector2 &studs,
		const core::Vector2 &pixels,
		float pixelsPerStud,
		const core::Vector3 &towardsDefault
	) {
		const float usable = std::max(pixelsPerStud, 1.0e-5f);
		const core::Vector2 worldSize{
			studs.X + pixels.X / usable,
			studs.Y + pixels.Y / usable,
		};

		SpatialQuad quad;

		// **`AxisY` runs down the image**, because a canvas is laid out in
		// interface pixels and `interface_spatial.vert` reads `v` from the top.
		// That is why `Normal` is carried separately rather than derived from the
		// axes: their cross product points away from the eye.
		quad.AxisX = right * worldSize.X;
		quad.AxisY = up * -worldSize.Y;
		quad.Origin = anchor - quad.AxisX * 0.5f - quad.AxisY * 0.5f;

		const float distance = toCamera.Magnitude();
		quad.Normal = distance > 0.0f ? toCamera / distance : towardsDefault;
		return quad;
	}
}
