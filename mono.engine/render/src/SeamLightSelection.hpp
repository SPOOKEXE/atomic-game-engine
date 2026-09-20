#pragma once

// The bounded spill volume and ranking shared by portal-light capture and
// deferred-light binding. Keeping this in one place prevents the capture budget
// from producing a field the final pass cannot select.

#include <engine/core/types/AABB.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/Frustum.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace engine::render {

	struct SeamLightProjector {
		core::Vector3 Centre;
		core::Vector3 Outward;
		core::Vector3 First;
		core::Vector3 Second;
		float Range = 0.0f;
		size_t Index = 0;
	};

	// Encloses the shader's one-sided expanding window. At the far plane each
	// half-axis has grown by `Range`, matching `SeamSpill`'s 45-degree spread.
	inline core::AABB SeamLightSpillBounds(const SeamLightProjector &projector) {
		const float firstLength = std::max(projector.First.Magnitude(), 1.0e-4f);
		const float secondLength = std::max(projector.Second.Magnitude(), 1.0e-4f);
		const core::Vector3 farFirst = projector.First * (1.0f + projector.Range / firstLength);
		const core::Vector3 farSecond = projector.Second * (1.0f + projector.Range / secondLength);
		const core::Vector3 half{
			std::abs(projector.Outward.X) * projector.Range * 0.5f + std::abs(farFirst.X) +
				std::abs(farSecond.X),
			std::abs(projector.Outward.Y) * projector.Range * 0.5f + std::abs(farFirst.Y) +
				std::abs(farSecond.Y),
			std::abs(projector.Outward.Z) * projector.Range * 0.5f + std::abs(farFirst.Z) +
				std::abs(farSecond.Z),
		};
		return core::AABB::FromCentre(projector.Centre + projector.Outward * (projector.Range * 0.5f), half);
	}

	inline bool SeamLightMayAffect(const SeamLightProjector &projector, const graph::Frustum &frustum) {
		return frustum.Intersects(SeamLightSpillBounds(projector));
	}

	// Estimates the part of a field which falls inside the final camera. The
	// capture budget must value that spill region, rather than the doorway's
	// location or a broad receiver such as the Tunnels floor.
	inline float
	SeamLightScreenCoverage(const SeamLightProjector &projector, const glm::mat4 &viewProjection) {
		const core::AABB bounds = SeamLightSpillBounds(projector);
		float minimumX = std::numeric_limits<float>::infinity();
		float minimumY = std::numeric_limits<float>::infinity();
		float maximumX = -std::numeric_limits<float>::infinity();
		float maximumY = -std::numeric_limits<float>::infinity();
		size_t projectedCorners = 0;
		for (size_t corner = 0; corner < 8; corner++) {
			const core::Vector3 point{
				corner & 1 ? bounds.Maximum.X : bounds.Minimum.X,
				corner & 2 ? bounds.Maximum.Y : bounds.Minimum.Y,
				corner & 4 ? bounds.Maximum.Z : bounds.Minimum.Z,
			};
			const glm::vec4 clip = viewProjection * glm::vec4{point.X, point.Y, point.Z, 1.0f};
			// A screen rectangle cannot safely omit an edge crossing the eye plane.
			// Treat it as full coverage, retaining a field instead of underestimating
			// a spill that clips through the viewport.
			if (!std::isfinite(clip.w) || clip.w <= 1.0e-5f) return 1.0f;
			const float x = clip.x / clip.w;
			const float y = clip.y / clip.w;
			if (!std::isfinite(x) || !std::isfinite(y)) continue;
			minimumX = std::min(minimumX, x);
			minimumY = std::min(minimumY, y);
			maximumX = std::max(maximumX, x);
			maximumY = std::max(maximumY, y);
			++projectedCorners;
		}
		// A degenerate transform has no usable projected corners. Retaining it as
		// full coverage is conservative for capture and lets the normal readiness
		// checks decide whether the field can be sampled.
		if (projectedCorners == 0) return 1.0f;
		const float width = std::max(0.0f, std::min(maximumX, 1.0f) - std::max(minimumX, -1.0f));
		const float height = std::max(0.0f, std::min(maximumY, 1.0f) - std::max(minimumY, -1.0f));
		return width * height * 0.25f;
	}

	inline float SeamLightBoundsDistanceSquared(const core::AABB &left, const core::AABB &right) {
		const auto axisDistance =
			[](float leftMinimum, float leftMaximum, float rightMinimum, float rightMaximum) {
				return std::max({leftMinimum - rightMaximum, rightMinimum - leftMaximum, 0.0f});
			};
		const core::Vector3 delta{
			axisDistance(left.Minimum.X, left.Maximum.X, right.Minimum.X, right.Maximum.X),
			axisDistance(left.Minimum.Y, left.Maximum.Y, right.Minimum.Y, right.Maximum.Y),
			axisDistance(left.Minimum.Z, left.Maximum.Z, right.Minimum.Z, right.Maximum.Z),
		};
		return delta.Dot(delta);
	}

	inline float SeamLightInfluenceDistanceSquared(
		const SeamLightProjector &projector,
		std::span<const scene::DrawInstance> receivers,
		const core::Vector3 &eye
	) {
		const core::AABB spill = SeamLightSpillBounds(projector);
		float nearest = receivers.empty() ? SeamLightBoundsDistanceSquared(spill, core::AABB{eye, eye})
										  : std::numeric_limits<float>::infinity();
		for (const scene::DrawInstance &receiver : receivers) {
			nearest = std::min(nearest, SeamLightBoundsDistanceSquared(spill, graph::BoundsOf(receiver)));
		}
		return nearest;
	}

	// DrawOrder retains the culled rows into the source instance array. Ranking
	// against it avoids treating offscreen pieces of a large mesh as receivers.
	inline float SeamLightInfluenceDistanceSquared(
		const SeamLightProjector &projector,
		std::span<const scene::DrawInstance> instances,
		std::span<const uint32_t> receiverIndices,
		const core::Vector3 &eye
	) {
		const core::AABB spill = SeamLightSpillBounds(projector);
		float nearest = receiverIndices.empty() ? SeamLightBoundsDistanceSquared(spill, core::AABB{eye, eye})
												: std::numeric_limits<float>::infinity();
		for (const uint32_t receiverIndex : receiverIndices) {
			if (receiverIndex < instances.size()) {
				nearest = std::min(
					nearest, SeamLightBoundsDistanceSquared(spill, graph::BoundsOf(instances[receiverIndex]))
				);
			}
		}
		return nearest;
	}

	// A field that still reaches the camera keeps its capture slot. Replacing a
	// live field on a tiny rank change makes its whole spill disappear for a
	// frame, while retiring it after it leaves the frustum is visually harmless.
	inline bool SeamLightCaptureBefore(
		bool leftReady,
		float leftDistance,
		float leftCoverage,
		size_t leftIndex,
		bool rightReady,
		float rightDistance,
		float rightCoverage,
		size_t rightIndex
	) {
		if (leftReady != rightReady) return leftReady;
		if (leftDistance != rightDistance) return leftDistance < rightDistance;
		if (leftCoverage != rightCoverage) return leftCoverage > rightCoverage;
		return leftIndex < rightIndex;
	}

}
