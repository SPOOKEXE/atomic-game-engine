#pragma once

// What a view can see, as six planes.
//
// **Culling is the first optimisation that changes what is correct.** Every
// other one makes the same picture faster; this one makes a *different* picture
// and is only allowed because the difference is invisible. So the direction of
// the error matters more than the speed: a test that wrongly says "visible"
// costs a draw, and one that wrongly says "hidden" is a hole in the world.
// Everything below is biased that way — the box test is conservative, and it
// says "visible" whenever it is not certain.
//
// **Planes come out of the view-projection matrix rather than out of the
// camera.** Gribb and Hartmann's extraction: each clip-space inequality
// `-w ≤ x ≤ w` is a row combination of the matrix, and the six of them are the
// six planes. Deriving from the matrix rather than from a field of view and an
// aspect ratio means the frustum cannot disagree with what was actually
// projected — which is the bug that produces geometry popping at the screen
// edge on one machine and not another.
//
// `scene::ResolveCamera` is where that matrix comes from, and it is the one
// place the engine decides what a camera's matrices are. A frustum built from
// anything else would be a second answer to the same question.
//
// @tier L9 · shared

#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Vector3.hpp>

#include <glm/mat4x4.hpp>

#include <array>

namespace engine::graph {

	// One side of a frustum: the plane `Normal · p + Distance = 0`.
	//
	// The normal points **inward**, so a point is inside the half-space when
	// the signed distance is non-negative. Inward rather than outward because
	// every test then reads as "how far in is it", and a sign convention that
	// flips per plane is the thing that makes a culling bug take an afternoon.
	//
	// @since v0.6
	struct Plane {
		// The inward-facing normal. Normalised by `Frustum::FromViewProjection`.
		core::Vector3 Normal;

		// The plane's offset along that normal.
		float Distance = 0.0f;

		// The signed distance from the plane to a point; positive is inside.
		//
		// @param point The point to measure.
		// @return The signed distance.
		constexpr float SignedDistance(const core::Vector3 &point) const {
			return Normal.Dot(point) + Distance;
		}
	};

	// The six planes of a view.
	//
	// @since v0.6
	struct Frustum {
		// Which side each plane is, in a fixed order.
		//
		// **Near first and far second**, and the order is not arbitrary: the
		// box test rejects on the first plane that excludes, and depth is what
		// rejects most often in a scene that reaches past its far plane. The
		// side planes follow because a wide field of view rejects less.
		enum Side : size_t {
			Near,
			Far,
			Left,
			Right,
			Bottom,
			Top,
			COUNT,
		};

		// The planes, indexed by `Side`.
		std::array<Plane, COUNT> Planes;

		// Extracts the six planes from a view-projection matrix.
		//
		// **Gribb–Hartmann**, and the normalisation is not optional here even
		// though a sign test would not need it: `DistanceTo` reports a real
		// distance, and a caller comparing one against a radius would otherwise
		// be comparing against an arbitrary scale.
		//
		// Assumes the Vulkan convention this engine pins everywhere —
		// `GLM_FORCE_DEPTH_ZERO_TO_ONE`, so the near plane is `z ≥ 0` rather
		// than `z ≥ -w`. A frustum built for the OpenGL convention would clip
		// everything in front of the camera, which reads as a renderer that
		// draws nothing rather than as a convention mismatch.
		//
		// @param viewProjection `Projection * View`, as `scene::CameraMatrices`
		//                       carries it.
		// @return The frustum.
		static Frustum FromViewProjection(const glm::mat4 &viewProjection);

		// Reports whether a box is at least partly inside.
		//
		// **Conservative: a `true` may be wrong and a `false` may not.** The
		// test is the standard positive-vertex one — for each plane, the corner
		// furthest along the inward normal — which never rejects a box that is
		// actually visible. It can accept a box that is outside all six planes
		// but outside no single one, which happens for large boxes near a
		// corner; that costs a draw call and nothing else.
		//
		// @param box The world-space box.
		// @return `true` when the box may be visible.
		bool Intersects(const core::AABB &box) const;

		// Reports whether a sphere is at least partly inside.
		//
		// @param centre The sphere's centre, in world space.
		// @param radius Its radius.
		// @return `true` when the sphere may be visible.
		bool Intersects(const core::Vector3 &centre, float radius) const;

		// Reports whether a point is inside every plane.
		//
		// @param point The world-space point.
		// @return `true` when the point is inside.
		bool Contains(const core::Vector3 &point) const;
	};
}
