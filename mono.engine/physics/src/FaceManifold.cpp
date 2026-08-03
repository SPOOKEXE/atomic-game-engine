#include "FaceManifold.hpp"

#include "ShapeSupport.hpp"

#include <engine/core/types/Vector3.hpp>
#include <engine/physics/Contacts.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::physics {

	namespace {
		// Clipping one polygon against another can add a vertex per clipping
		// edge, so the widest intermediate is the two inputs' vertex counts
		// added. Eight and eight is the worst pair this module can produce.
		constexpr size_t MAXIMUM_CLIPPED = 2 * MAXIMUM_FEATURE_POINTS;

		// Two segment directions closer to parallel than this are treated as
		// parallel and clipped rather than crossed.
		//
		// **This is the cylinder parallel-axis degenerate case**, and it is a
		// branch rather than an epsilon nobody thought about: two barrels lying
		// side by side have no unique closest pair of points, so the crossing
		// solution collapses to an arbitrary one of the pair and the cylinder
		// rolls in place. Clipping the overlap instead gives the two points
		// that hold it.
		constexpr float PARALLEL_LIMIT = 0.9998f; // about one degree

		// Below this the contact normal is nearly in the reference face's own
		// plane, so projecting along it divides by nearly nothing.
		constexpr float PROJECTION_LIMIT = 1.0e-4f;

		// How far outside a clipping edge a vertex may lie and still be kept,
		// in metres.
		//
		// **A stack of identical boxes is the case this exists for**, and it is
		// not a rounding allowance. Two boxes of the same width resting on each
		// other present two faces whose outlines coincide *exactly*, so every
		// vertex of the incident face sits precisely on a clipping edge — and a
		// strict test puts all four of them outside, throws the whole polygon
		// away, and leaves the fallback single point holding a stack that then
		// slides apart over a couple of seconds. A tenth of a millimetre is far
		// below anything a contact cares about and far above where two exactly
		// coincident corners land after a rotation and a subtraction.
		constexpr float CLIP_TOLERANCE = 1.0e-4f;

		// A polygon under construction, with no allocation anywhere.
		struct Polygon {
			core::Vector3 Points[MAXIMUM_CLIPPED];
			size_t Count = 0;

			void Add(const core::Vector3 &point) {
				if (Count < MAXIMUM_CLIPPED) {
					Points[Count++] = point;
				}
			}
		};

		// Cuts a polygon down to the half-space behind one plane.
		//
		// `inward` must be unit length, so that `CLIP_TOLERANCE` is a distance
		// rather than a distance times an edge length.
		Polygon
		ClipAgainstPlane(const Polygon &subject, const core::Vector3 &origin, const core::Vector3 &inward) {
			Polygon result;
			if (subject.Count == 0) {
				return result;
			}

			core::Vector3 previous = subject.Points[subject.Count - 1];
			float previousSide = (origin - previous).Dot(inward);
			bool previousInside = previousSide < CLIP_TOLERANCE;

			for (size_t index = 0; index < subject.Count; index++) {
				const core::Vector3 current = subject.Points[index];
				const float side = (origin - current).Dot(inward);
				bool inside = side < CLIP_TOLERANCE;

				if (inside != previousInside) {
					const core::Vector3 edge = current - previous;
					const float denominator = edge.Dot(inward);
					if (denominator != 0.0f) {
						result.Add(previous + edge * (previousSide / denominator));
					} else {
						// The edge lies in the plane. Treating it as staying on
						// the previous side keeps the walk from adding a
						// crossing that is not one and losing the vertex.
						inside = previousInside;
					}
				}

				if (inside) {
					result.Add(current);
				}

				previous = current;
				previousSide = side;
				previousInside = inside;
			}

			return result;
		}

		// Cuts `subject` down to the prism the reference face sweeps along its
		// own normal.
		//
		// The reference's winding is what makes each edge plane point inward,
		// which is why `SupportFeature` documents the winding as load-bearing.
		Polygon ClipAgainstFace(const Polygon &subject, const SupportFeature &reference) {
			Polygon current = subject;
			for (size_t index = 0; index < reference.Count; index++) {
				const core::Vector3 from = reference.Points[index];
				const core::Vector3 to = reference.Points[(index + 1) % reference.Count];

				// Unit, because the tolerance the plane test uses is a
				// distance. Unnormalised it would be scaled by the edge's
				// length, so the same coincident-corner case would be forgiven
				// on a long edge and not on a short one.
				current = ClipAgainstPlane(current, from, reference.Plane.Cross(to - from).Unit());
				if (current.Count == 0) {
					break;
				}
			}
			return current;
		}

		Polygon PolygonOf(const SupportFeature &feature) {
			Polygon polygon;
			for (size_t index = 0; index < feature.Count; index++) {
				polygon.Add(feature.Points[index]);
			}
			return polygon;
		}

		// One candidate contact, before the four best are chosen.
		struct Candidate {
			core::Vector3 Position;
			float Penetration = 0.0f;
			size_t Source = 0;
		};

		// Keeps the four candidates that best hold the pair apart.
		//
		// The deepest one, the one furthest from it, and the two that push the
		// enclosed area widest to either side. Four points chosen this way hold
		// a face against translation and both rotations; four chosen by taking
		// the first four leave a manifold whose area collapses as soon as the
		// clip order changes.
		size_t ReduceToBudget(Candidate *candidates, size_t count, const core::Vector3 &normal) {
			if (count <= ContactManifold::MAXIMUM_POINTS) {
				return count;
			}

			size_t deepest = 0;
			for (size_t index = 1; index < count; index++) {
				if (candidates[index].Penetration > candidates[deepest].Penetration) {
					deepest = index;
				}
			}

			const core::Vector3 origin = candidates[deepest].Position;
			size_t furthest = deepest;
			float furthestDistance = -1.0f;
			for (size_t index = 0; index < count; index++) {
				const float distance = (candidates[index].Position - origin).MagnitudeSquared();
				if (distance > furthestDistance) {
					furthestDistance = distance;
					furthest = index;
				}
			}

			const core::Vector3 spread = (candidates[furthest].Position - origin).Cross(normal);
			size_t left = deepest;
			size_t right = deepest;
			float leftExtreme = 0.0f;
			float rightExtreme = 0.0f;
			for (size_t index = 0; index < count; index++) {
				const float side = spread.Dot(candidates[index].Position - origin);
				if (side < leftExtreme) {
					leftExtreme = side;
					left = index;
				} else if (side > rightExtreme) {
					rightExtreme = side;
					right = index;
				}
			}

			const size_t keep[ContactManifold::MAXIMUM_POINTS] = {deepest, furthest, left, right};
			Candidate chosen[ContactManifold::MAXIMUM_POINTS];
			size_t kept = 0;
			for (const size_t index : keep) {
				bool already = false;
				for (size_t at = 0; at < kept; at++) {
					already = already || chosen[at].Source == candidates[index].Source;
				}
				if (!already) {
					chosen[kept++] = candidates[index];
				}
			}

			for (size_t index = 0; index < kept; index++) {
				candidates[index] = chosen[index];
			}
			return kept;
		}

		ContactSolution Assemble(
			const core::Vector3 &normal,
			const Candidate *candidates,
			size_t count,
			uint8_t first,
			uint8_t second
		) {
			ContactSolution solution;
			solution.Normal = normal;
			solution.Touching = true;
			solution.PointCount = static_cast<uint8_t>(count);
			for (size_t index = 0; index < count; index++) {
				solution.Positions[index] = candidates[index].Position;
				solution.Penetrations[index] = candidates[index].Penetration;
				solution.Features[index] = ContactFeature(first, second, candidates[index].Source);
			}
			return solution;
		}

		// Where two segments come closest, as parameters along each.
		struct ClosestParameters {
			float First = 0.0f;
			float Second = 0.0f;
		};

		ClosestParameters ClosestOnSegments(
			const core::Vector3 &firstFrom,
			const core::Vector3 &firstTo,
			const core::Vector3 &secondFrom,
			const core::Vector3 &secondTo
		) {
			const core::Vector3 firstEdge = firstTo - firstFrom;
			const core::Vector3 secondEdge = secondTo - secondFrom;
			const core::Vector3 between = firstFrom - secondFrom;

			const float firstLength = firstEdge.MagnitudeSquared();
			const float secondLength = secondEdge.MagnitudeSquared();
			const float crossTerm = firstEdge.Dot(secondEdge);
			const float firstProjection = firstEdge.Dot(between);
			const float secondProjection = secondEdge.Dot(between);

			const float determinant = firstLength * secondLength - crossTerm * crossTerm;

			// A zero determinant is the parallel case, which the caller has
			// already taken another route for; clamping to the start of the
			// first segment keeps this from dividing by nothing if it is ever
			// reached from a near-degenerate one.
			float alongFirst = 0.0f;
			if (determinant > 1.0e-12f) {
				alongFirst = (crossTerm * secondProjection - secondLength * firstProjection) / determinant;
				alongFirst = alongFirst < 0.0f ? 0.0f : (alongFirst > 1.0f ? 1.0f : alongFirst);
			}

			float alongSecond =
				(crossTerm * alongFirst + secondProjection) / (secondLength > 0.0f ? secondLength : 1.0f);
			alongSecond = alongSecond < 0.0f ? 0.0f : (alongSecond > 1.0f ? 1.0f : alongSecond);
			return ClosestParameters{alongFirst, alongSecond};
		}

		// Two edges against each other: the crossed case and the parallel one.
		ContactSolution SegmentManifold(
			const SupportFeature &first,
			const SupportFeature &second,
			const core::Vector3 &normal,
			float depth
		) {
			const core::Vector3 firstEdge = first.Points[1] - first.Points[0];
			const core::Vector3 secondEdge = second.Points[1] - second.Points[0];
			const float secondLength = secondEdge.Magnitude();

			const bool parallel = std::abs(firstEdge.Unit().Dot(secondEdge.Unit())) > PARALLEL_LIMIT;
			if (!parallel || secondLength <= 0.0f) {
				const ClosestParameters closest =
					ClosestOnSegments(first.Points[0], first.Points[1], second.Points[0], second.Points[1]);
				const core::Vector3 position = second.Points[0] + secondEdge * closest.Second;
				return SinglePoint(normal, position, depth, ContactFeature(first.Id, second.Id, 0));
			}

			// Parallel: keep the stretch of the second edge that lies under the
			// first. Two points, which is what stops a cylinder resting on its
			// side from rolling.
			const core::Vector3 direction = secondEdge / secondLength;
			const float firstStart = (first.Points[0] - second.Points[0]).Dot(direction);
			const float firstEnd = (first.Points[1] - second.Points[0]).Dot(direction);
			const float low = firstStart < firstEnd ? firstStart : firstEnd;
			const float high = firstStart < firstEnd ? firstEnd : firstStart;

			const float from = low < 0.0f ? 0.0f : low;
			const float to = high > secondLength ? secondLength : high;
			if (to <= from) {
				const float middle = (from + to) * 0.5f;
				const float clamped = middle < 0.0f ? 0.0f : (middle > secondLength ? secondLength : middle);
				return SinglePoint(
					normal,
					second.Points[0] + direction * clamped,
					depth,
					ContactFeature(first.Id, second.Id, 0)
				);
			}

			Candidate candidates[2];
			candidates[0] = Candidate{second.Points[0] + direction * from, depth, 0};
			candidates[1] = Candidate{second.Points[0] + direction * to, depth, 1};
			return Assemble(normal, candidates, 2, first.Id, second.Id);
		}
	}

	ContactSolution
	SinglePoint(const core::Vector3 &normal, const core::Vector3 &position, float depth, uint32_t feature) {
		ContactSolution solution;
		solution.Normal = normal;
		solution.Touching = true;
		solution.PointCount = 1;
		solution.Positions[0] = position;
		solution.Penetrations[0] = depth < 0.0f ? 0.0f : depth;
		solution.Features[0] = feature;
		return solution;
	}

	ContactSolution ManifoldBetween(
		const ShapeInstance &first, const ShapeInstance &second, const core::Vector3 &normal, float depth
	) {
		const SupportFeature facing = FaceTowards(first, normal);
		const SupportFeature facingBack = FaceTowards(second, -normal);

		// A sphere, or a corner met exactly. One constraint is the honest
		// answer here: a ball on a floor touches at one place and is supposed
		// to be free to roll.
		if (facing.Count == 1 || facingBack.Count == 1) {
			const core::Vector3 position =
				facingBack.Count == 1 ? facingBack.Points[0] : SupportPoint(second, -normal);
			return SinglePoint(normal, position, depth, ContactFeature(facing.Id, facingBack.Id, 0));
		}

		if (facing.Count == 2 && facingBack.Count == 2) {
			return SegmentManifold(facing, facingBack, normal, depth);
		}

		// The flatter of the two faces is the reference, because clipping
		// against a face that is nearly edge-on to the contact normal projects
		// through a plane the normal barely crosses.
		const bool referenceIsFirst =
			facing.Count >= 3 && (facingBack.Count < 3 || std::abs(facing.Plane.Dot(normal)) >=
															  std::abs(facingBack.Plane.Dot(normal)));
		const SupportFeature &reference = referenceIsFirst ? facing : facingBack;
		const SupportFeature &incident = referenceIsFirst ? facingBack : facing;

		const float slope = normal.Dot(reference.Plane);
		if (std::abs(slope) < PROJECTION_LIMIT) {
			return SinglePoint(
				normal, SupportPoint(second, -normal), depth, ContactFeature(facing.Id, facingBack.Id, 0)
			);
		}

		const Polygon clipped = ClipAgainstFace(PolygonOf(incident), reference);

		Candidate candidates[MAXIMUM_CLIPPED];
		size_t count = 0;
		for (size_t index = 0; index < clipped.Count; index++) {
			const core::Vector3 point = clipped.Points[index];
			const float travel = (reference.Points[0] - point).Dot(reference.Plane) / slope;
			const float penetration = referenceIsFirst ? travel : -travel;
			if (penetration < -CONTACT_TOLERANCE) {
				continue;
			}

			candidates[count] = Candidate{
				referenceIsFirst ? point : point + normal * travel,
				penetration < 0.0f ? 0.0f : penetration,
				index,
			};
			count++;
		}

		// Nothing survived the clip, which happens when the two faces overlap
		// only outside each other's outlines — a corner resting on a face, for
		// instance. The axis still found a real overlap, so the answer is one
		// point rather than none.
		if (count == 0) {
			return SinglePoint(
				normal, SupportPoint(second, -normal), depth, ContactFeature(facing.Id, facingBack.Id, 0)
			);
		}

		count = ReduceToBudget(candidates, count, normal);
		return Assemble(normal, candidates, count, facing.Id, facingBack.Id);
	}
}
