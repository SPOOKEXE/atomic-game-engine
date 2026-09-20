#pragma once

// Portal shadow beams are selected by the receiver geometry they can reach.
// A doorway can be outside the camera while its mapped sunlight still lands on
// visible ground, so pane distance is not a valid priority.

#include <engine/core/types/CFrame.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/Frustum.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <glm/mat4x4.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

namespace engine::render {

	struct PortalBeamProjector {
		// Maps a receiver in the far room back to the caster room.
		scene::SeamTransform Back;
		core::Vector3 PlaneNormal;
		float PlaneOffset = 0.0f;
		glm::mat4 Light{1.0f};
	};

	struct PortalBeamRank {
		uint32_t Slot = 0;
		float InfluenceDistance = std::numeric_limits<float>::infinity();
	};

	inline bool PortalBeamRanksBefore(const PortalBeamRank &left, const PortalBeamRank &right) {
		if (left.InfluenceDistance != right.InfluenceDistance)
			return left.InfluenceDistance < right.InfluenceDistance;
		return left.Slot < right.Slot;
	}

	inline float PortalBeamMaxPlaneDistance(const core::AABB &bounds, const core::Vector3 &normal) {
		const core::Vector3 corner{
			normal.X >= 0.0f ? bounds.Maximum.X : bounds.Minimum.X,
			normal.Y >= 0.0f ? bounds.Maximum.Y : bounds.Minimum.Y,
			normal.Z >= 0.0f ? bounds.Maximum.Z : bounds.Minimum.Z,
		};
		return normal.Dot(corner);
	}

	inline core::AABB
	PortalBeamMappedBounds(const PortalBeamProjector &projector, const scene::DrawInstance &receiver) {
		// `Place` already rotates the receiver's local axes. A half-extent is a
		// length on those axes, so it scales but does not rotate a second time.
		return core::OrientedBoxBounds(
			projector.Back.Place(receiver.Frame), receiver.HalfExtent * std::abs(projector.Back.Scale)
		);
	}

	// The squared distance from the eye to the closest visible receiver touched
	// by the beam, or infinity when the beam cannot touch any visible receiver.
	// `DrawOrder` is the renderer's main-view cull result, so a large offscreen
	// part does not reserve a beam merely because its source row exists.
	inline float PortalBeamInfluenceDistanceSquared(
		const PortalBeamProjector &projector,
		std::span<const scene::DrawInstance> instances,
		std::span<const uint32_t> receiverIndices,
		const core::Vector3 &eye
	) {
		const graph::Frustum frustum = graph::Frustum::FromViewProjection(projector.Light);
		float nearest = std::numeric_limits<float>::infinity();
		for (const uint32_t index : receiverIndices) {
			if (index >= instances.size()) continue;
			const scene::DrawInstance &receiver = instances[index];
			const core::AABB mapped = PortalBeamMappedBounds(projector, receiver);
			if (PortalBeamMaxPlaneDistance(mapped, projector.PlaneNormal) <= projector.PlaneOffset ||
				!frustum.Intersects(mapped)) {
				continue;
			}

			const core::AABB original = graph::BoundsOf(receiver);
			const core::Vector3 closest = original.ClosestPoint(eye);
			const core::Vector3 delta = closest - eye;
			nearest = std::min(nearest, delta.Dot(delta));
		}
		return nearest;
	}
}
