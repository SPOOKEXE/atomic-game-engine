#include "ContactPairs.hpp"

#include "FaceManifold.hpp"
#include "ShapeSupport.hpp"

#include <engine/core/types/Vector3.hpp>
#include <engine/scene/Enums.hpp>

#include <cmath>
#include <cstddef>

namespace engine::physics {

	namespace {
		// A candidate axis shorter than this is a degenerate cross product —
		// two parallel edges — and normalising it would turn float noise into a
		// direction. Skipping it is correct rather than approximate: the axis
		// it stands for does not exist for that configuration, and the
		// parallel case is covered by the face axes instead.
		constexpr float AXIS_EPSILON = 1.0e-8f;

		// How much better a cross-product axis has to be before it displaces a
		// face or cap normal.
		//
		// Two millimetres, plus two per cent of the depth. Both terms earn
		// their place: the absolute one covers a resting contact where the two
		// depths are equal to the last bit, and the relative one covers a deep
		// one where the same float noise is proportionally larger. Without the
		// bias a box at rest alternates between its face axis and an edge axis
		// from tick to tick, and the manifold alternates between four points
		// and one with it.
		constexpr float AXIS_BIAS_METRES = 0.002f;
		constexpr float AXIS_BIAS_FRACTION = 0.02f;

		// Below this two centres are on top of each other and there is no
		// direction to separate along. Every pair falls back to world up, which
		// is arbitrary and has to be *deterministic* — an arbitrary direction
		// derived from whatever was in memory is a scene that behaves
		// differently on the second run.
		constexpr float DEGENERATE_EPSILON = 1.0e-12f;

		const core::Vector3 FALLBACK_NORMAL = core::Vector3::YAxis;

		// The part of `vector` at right angles to `axis`.
		core::Vector3 Perpendicular(const core::Vector3 &vector, const core::Vector3 &axis) {
			return vector - axis * vector.Dot(axis);
		}

		// The eight corners of a box, in a fixed order.
		struct BoxCorners {
			core::Vector3 Point[8];
		};

		BoxCorners CornersOf(const ShapeInstance &box) {
			const core::Vector3 right = box.Axis[0] * box.Extent.X;
			const core::Vector3 up = box.Axis[1] * box.Extent.Y;
			const core::Vector3 forward = box.Axis[2] * box.Extent.Z;

			BoxCorners corners;
			for (size_t index = 0; index < 8; index++) {
				const float alongX = (index & 1u) != 0 ? 1.0f : -1.0f;
				const float alongY = (index & 2u) != 0 ? 1.0f : -1.0f;
				const float alongZ = (index & 4u) != 0 ? 1.0f : -1.0f;
				corners.Point[index] = box.Frame.Position + right * alongX + up * alongY + forward * alongZ;
			}
			return corners;
		}

		// The point of a cylinder's rim nearest to `target`, on whichever cap
		// faces it.
		core::Vector3 NearestRimPoint(const ShapeInstance &cylinder, const core::Vector3 &target) {
			const core::Vector3 axis = BarrelAxis(cylinder);
			const core::Vector3 offset = target - cylinder.Frame.Position;
			const float along = offset.Dot(axis);
			const core::Vector3 radial = Perpendicular(offset, axis);
			const core::Vector3 outward =
				radial.MagnitudeSquared() > DEGENERATE_EPSILON ? radial.Unit() : cylinder.Axis[0];
			const float sign = along >= 0.0f ? 1.0f : -1.0f;
			return cylinder.Frame.Position + axis * (cylinder.Extent.Y * sign) + outward * cylinder.Extent.X;
		}

		// Turns a solution's shape order round, which is the only flip in the
		// module.
		//
		// The normal reverses and every point moves from one surface to the
		// other along it — `pointOnB = pointOnA - normal * penetration` — so a
		// flipped solution still describes its points as lying on the second
		// body, which is what `ContactManifold` promises its reader.
		ContactSolution Flipped(const ContactSolution &solution) {
			ContactSolution flipped = solution;
			flipped.Normal = -solution.Normal;
			for (size_t index = 0; index < solution.PointCount; index++) {
				flipped.Positions[index] =
					solution.Positions[index] - flipped.Normal * solution.Penetrations[index];
			}
			return flipped;
		}

		// The face of `box` that faces `direction`, as a cache key.
		uint8_t BoxFaceKey(const ShapeInstance &box, const core::Vector3 &direction) {
			return FaceTowards(box, direction).Id;
		}
	}

	AxisChoice LeastOverlap(
		const ShapeInstance &first, const ShapeInstance &second, const AxisCandidate *axes, size_t count
	) {
		const core::Vector3 offset = second.Frame.Position - first.Frame.Position;

		AxisChoice primary;
		AxisChoice derived;
		float primaryDepth = 0.0f;
		float derivedDepth = 0.0f;

		for (size_t index = 0; index < count; index++) {
			const AxisCandidate &candidate = axes[index];
			const float lengthSquared = candidate.Direction.MagnitudeSquared();
			if (lengthSquared < AXIS_EPSILON) {
				continue;
			}

			// Scaled by the reciprocal rather than divided by the length:
			// `Vector3::operator/` is three divides, and this loop runs up to
			// twenty-three times per pair.
			const core::Vector3 axis = candidate.Direction * (1.0f / std::sqrt(lengthSquared));
			const float separation = offset.Dot(axis);
			const float overlap =
				ProjectionRadius(first, axis) + ProjectionRadius(second, axis) - std::abs(separation);
			if (overlap <= 0.0f) {
				// One separating axis is proof, and there is nothing further to
				// learn from the rest.
				return AxisChoice{};
			}

			// Pointing from the first shape toward the second, always. The
			// projection radius is symmetric, so the axis as given may point
			// either way and only the offset says which.
			const core::Vector3 oriented = separation >= 0.0f ? axis : -axis;

			AxisChoice &slot = candidate.Primary ? primary : derived;
			float &depth = candidate.Primary ? primaryDepth : derivedDepth;
			if (!slot.Touching || overlap < depth) {
				slot = AxisChoice{oriented, overlap, true};
				depth = overlap;
			}
		}

		if (!primary.Touching) {
			return derived;
		}
		if (derived.Touching &&
			derivedDepth < primaryDepth - AXIS_BIAS_METRES - primaryDepth * AXIS_BIAS_FRACTION) {
			return derived;
		}
		return primary;
	}

	ContactSolution BoxBox(const ShapeInstance &first, const ShapeInstance &second) {
		// Fifteen axes, and for two polytopes that set is provably complete:
		// six face normals and the nine cross products of their edge
		// directions. Nothing here is a heuristic.
		AxisCandidate axes[15];
		size_t count = 0;
		for (size_t index = 0; index < 3; index++) {
			axes[count++] = AxisCandidate{first.Axis[index], true};
		}
		for (size_t index = 0; index < 3; index++) {
			axes[count++] = AxisCandidate{second.Axis[index], true};
		}
		for (size_t left = 0; left < 3; left++) {
			for (size_t right = 0; right < 3; right++) {
				axes[count++] = AxisCandidate{first.Axis[left].Cross(second.Axis[right]), false};
			}
		}

		const AxisChoice choice = LeastOverlap(first, second, axes, count);
		if (!choice.Touching) {
			return ContactSolution{};
		}
		return ManifoldBetween(first, second, choice.Normal, choice.Depth);
	}

	ContactSolution BoxSphere(const ShapeInstance &first, const ShapeInstance &second) {
		const float radius = second.Extent.X;
		const core::Vector3 local = ToLocalPoint(first.Frame, second.Frame.Position);
		const float extent[3] = {first.Extent.X, first.Extent.Y, first.Extent.Z};
		const float centre[3] = {local.X, local.Y, local.Z};

		float clamped[3];
		bool inside = true;
		for (size_t index = 0; index < 3; index++) {
			const float low = -extent[index];
			const float high = extent[index];
			clamped[index] = centre[index] < low ? low : (centre[index] > high ? high : centre[index]);
			inside = inside && clamped[index] == centre[index];
		}

		if (!inside) {
			const core::Vector3 surface =
				first.Frame.PointToWorldSpace(core::Vector3{clamped[0], clamped[1], clamped[2]});
			const core::Vector3 offset = second.Frame.Position - surface;
			const float distanceSquared = offset.MagnitudeSquared();
			if (distanceSquared > radius * radius) {
				return ContactSolution{};
			}

			const float distance = std::sqrt(distanceSquared);
			const core::Vector3 normal = distance > DEGENERATE_EPSILON ? offset / distance : FALLBACK_NORMAL;

			// The point is on the sphere, which is the second shape — the
			// convention, and the reason it is not the box's surface point that
			// was just computed.
			return SinglePoint(
				normal,
				second.Frame.Position - normal * radius,
				radius - distance,
				ContactFeature(BoxFaceKey(first, normal), 0, 0)
			);
		}

		// The centre is inside the box, so the shortest way out is through the
		// nearest face. Every face is a candidate and the smallest wins,
		// because pushing through a further one is a longer move that would
		// also pass straight through the box.
		size_t nearest = 0;
		float shortest = extent[0] - std::abs(centre[0]);
		for (size_t index = 1; index < 3; index++) {
			const float gap = extent[index] - std::abs(centre[index]);
			if (gap < shortest) {
				shortest = gap;
				nearest = index;
			}
		}

		// The face's own axis, signed. One of the frame's three, so it is a
		// read rather than the rotation of a local unit vector.
		const float sign = centre[nearest] >= 0.0f ? 1.0f : -1.0f;
		const core::Vector3 normal = first.Axis[nearest] * sign;
		return SinglePoint(
			normal,
			second.Frame.Position - normal * radius,
			shortest + radius,
			ContactFeature(BoxFaceKey(first, normal), 0, 0)
		);
	}

	ContactSolution BoxCylinder(const ShapeInstance &first, const ShapeInstance &second) {
		const core::Vector3 barrel = BarrelAxis(second);
		const BoxCorners corners = CornersOf(first);

		// Three box faces and the cap normal are the flat contacts. The three
		// crosses are a box edge lying along the barrel. The sixteen corner
		// axes are the two curved cases: a corner pressed into the barrel, and
		// a corner pressed into the rim of a cap. See the header for what this
		// set does and does not enumerate.
		AxisCandidate axes[23];
		size_t count = 0;
		for (size_t index = 0; index < 3; index++) {
			axes[count++] = AxisCandidate{first.Axis[index], true};
		}
		axes[count++] = AxisCandidate{barrel, true};
		for (size_t index = 0; index < 3; index++) {
			axes[count++] = AxisCandidate{first.Axis[index].Cross(barrel), false};
		}
		for (size_t index = 0; index < 8; index++) {
			const core::Vector3 corner = corners.Point[index];
			axes[count++] = AxisCandidate{Perpendicular(corner - second.Frame.Position, barrel), false};
			axes[count++] = AxisCandidate{corner - NearestRimPoint(second, corner), false};
		}

		const AxisChoice choice = LeastOverlap(first, second, axes, count);
		if (!choice.Touching) {
			return ContactSolution{};
		}
		return ManifoldBetween(first, second, choice.Normal, choice.Depth);
	}

	ContactSolution SphereSphere(const ShapeInstance &first, const ShapeInstance &second) {
		const float reach = first.Extent.X + second.Extent.X;
		const core::Vector3 offset = second.Frame.Position - first.Frame.Position;
		const float distanceSquared = offset.MagnitudeSquared();
		if (distanceSquared > reach * reach) {
			return ContactSolution{};
		}

		const float distance = std::sqrt(distanceSquared);
		const core::Vector3 normal = distance > DEGENERATE_EPSILON ? offset / distance : FALLBACK_NORMAL;
		return SinglePoint(
			normal,
			second.Frame.Position - normal * second.Extent.X,
			reach - distance,
			ContactFeature(0, 0, 0)
		);
	}

	ContactSolution SphereCylinder(const ShapeInstance &first, const ShapeInstance &second) {
		const float radius = first.Extent.X;
		const float barrelRadius = second.Extent.X;
		const float halfHeight = second.Extent.Y;

		// Exact, and analytic in the cylinder's own axes: clamp along the
		// barrel, clamp across it, and the result is the nearest point of the
		// cylinder to the sphere's centre. There is no axis set here and no
		// approximation — a sphere is one number from its centre, so the
		// closest-point answer *is* the minimum-penetration answer.
		const core::Vector3 local = ToLocalPoint(second.Frame, first.Frame.Position);
		const float across = std::sqrt(local.X * local.X + local.Z * local.Z);
		const bool outside = across > barrelRadius || std::abs(local.Y) > halfHeight;

		if (outside) {
			const float scale = across > barrelRadius ? barrelRadius / across : 1.0f;
			const float clampedY =
				local.Y < -halfHeight ? -halfHeight : (local.Y > halfHeight ? halfHeight : local.Y);
			const core::Vector3 surface =
				second.Frame.PointToWorldSpace(core::Vector3{local.X * scale, clampedY, local.Z * scale});

			const core::Vector3 offset = surface - first.Frame.Position;
			const float distanceSquared = offset.MagnitudeSquared();
			if (distanceSquared > radius * radius) {
				return ContactSolution{};
			}

			const float distance = std::sqrt(distanceSquared);
			const core::Vector3 normal = distance > DEGENERATE_EPSILON ? offset / distance : FALLBACK_NORMAL;
			return SinglePoint(normal, surface, radius - distance, ContactFeature(0, 0, 0));
		}

		// The centre is inside the barrel. Out through the nearer of the cap
		// and the wall, for the same reason the box case picks its nearest
		// face.
		const float axialGap = halfHeight - std::abs(local.Y);
		const float radialGap = barrelRadius - across;

		core::Vector3 outwardLocal;
		float surfaceDistance = 0.0f;
		if (axialGap <= radialGap) {
			outwardLocal = core::Vector3{0.0f, local.Y >= 0.0f ? 1.0f : -1.0f, 0.0f};
			surfaceDistance = axialGap;
		} else if (across > DEGENERATE_EPSILON) {
			outwardLocal = core::Vector3{local.X / across, 0.0f, local.Z / across};
			surfaceDistance = radialGap;
		} else {
			// Dead on the axis with the wall nearer than either cap, which
			// needs a radial direction there is no data for. Local +X is
			// arbitrary and, unlike anything derived from the zero vector,
			// the same on every run.
			outwardLocal = core::Vector3::XAxis;
			surfaceDistance = radialGap;
		}

		const core::Vector3 outward = second.Frame.VectorToWorldSpace(outwardLocal);
		return SinglePoint(
			-outward,
			first.Frame.Position + outward * surfaceDistance,
			surfaceDistance + radius,
			ContactFeature(0, 0, 0)
		);
	}

	ContactSolution CylinderCylinder(const ShapeInstance &first, const ShapeInstance &second) {
		const core::Vector3 firstAxis = BarrelAxis(first);
		const core::Vector3 secondAxis = BarrelAxis(second);
		const core::Vector3 offset = second.Frame.Position - first.Frame.Position;

		AxisCandidate axes[11];
		size_t count = 0;

		// The two cap normals: stacked flat, and a rim pressed against a face.
		axes[count++] = AxisCandidate{firstAxis, true};
		axes[count++] = AxisCandidate{secondAxis, true};

		// **Two barrels crossed.** Degenerate exactly when the axes are
		// parallel, at which point the cross product removes itself and the
		// radial axis below is the one that answers — that is the parallel-axis
		// case, handled by construction rather than by an epsilon somewhere
		// downstream.
		axes[count++] = AxisCandidate{firstAxis.Cross(secondAxis), false};

		// **Two barrels side by side.** The direction between the axes, at
		// right angles to each. This is what holds a parallel pair apart, and
		// it is the axis a set built only from cap normals and one cross
		// product would be missing.
		axes[count++] = AxisCandidate{Perpendicular(offset, firstAxis), false};
		axes[count++] = AxisCandidate{Perpendicular(offset, secondAxis), false};

		// **The disc-edge cases**, which are the ones that go subtly wrong.
		// A rim meets a circle or a wall, never a face, so none of these
		// directions is any shape's normal and none of them appears above.
		//
		// Rim against rim: take the point of one rim nearest the other's
		// centre, then the point of the other rim nearest *that*. One
		// refinement step, from both ends, which is what turns two circles into
		// a direction without an iteration whose count would have to be a
		// documented number.
		const core::Vector3 firstRim = NearestRimPoint(first, second.Frame.Position);
		const core::Vector3 secondRim = NearestRimPoint(second, firstRim);
		axes[count++] = AxisCandidate{secondRim - firstRim, false};
		axes[count++] = AxisCandidate{NearestRimPoint(first, secondRim) - secondRim, false};

		// Rim against wall: each cap's centre against the other barrel.
		for (int side = -1; side <= 1; side += 2) {
			const auto sign = static_cast<float>(side);
			const core::Vector3 firstCap = first.Frame.Position + firstAxis * (first.Extent.Y * sign);
			const core::Vector3 secondCap = second.Frame.Position + secondAxis * (second.Extent.Y * sign);
			axes[count++] = AxisCandidate{Perpendicular(firstCap - second.Frame.Position, secondAxis), false};
			axes[count++] = AxisCandidate{Perpendicular(secondCap - first.Frame.Position, firstAxis), false};
		}

		const AxisChoice choice = LeastOverlap(first, second, axes, count);
		if (!choice.Touching) {
			return ContactSolution{};
		}
		return ManifoldBetween(first, second, choice.Normal, choice.Depth);
	}

	ContactSolution ContactBetween(const ShapeInstance &first, const ShapeInstance &second) {
		// Ordered by `ShapeKind` so that six functions cover six unordered
		// pairs instead of nine covering the ordered ones. The flip below is
		// the whole cost of that, and it is in one place.
		const bool ordered = static_cast<int>(first.Shape) <= static_cast<int>(second.Shape);
		const ShapeInstance &low = ordered ? first : second;
		const ShapeInstance &high = ordered ? second : first;

		// Six arms for six unordered pairs. The nested-switch form would need
		// nine, three of which the ordering above makes unreachable — and an
		// unreachable arm returning "not touching" is a pair that reports no
		// contact and looks like a decision.
		ContactSolution solution;
		switch (low.Shape) {
		case scene::ShapeKind::Box:
			if (high.Shape == scene::ShapeKind::Box) {
				solution = BoxBox(low, high);
			} else if (high.Shape == scene::ShapeKind::Sphere) {
				solution = BoxSphere(low, high);
			} else {
				solution = BoxCylinder(low, high);
			}
			break;

		case scene::ShapeKind::Sphere:
			if (high.Shape == scene::ShapeKind::Sphere) {
				solution = SphereSphere(low, high);
			} else {
				solution = SphereCylinder(low, high);
			}
			break;

		case scene::ShapeKind::Cylinder:
			solution = CylinderCylinder(low, high);
			break;
		}

		return ordered ? solution : Flipped(solution);
	}
}
