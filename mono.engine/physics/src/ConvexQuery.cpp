#include "ConvexQuery.hpp"

#include "ContactPairs.hpp"

#include <engine/collision/TriangleMesh.hpp>
#include <engine/physics/Integrate.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace engine::physics {

	namespace {
		float MaximumRadius(const ShapeInstance &shape) {
			const core::AABB bounds = ShapeReach(shape);
			const core::Vector3 low = bounds.Minimum - shape.Frame.Position;
			const core::Vector3 high = bounds.Maximum - shape.Frame.Position;
			return std::max(low.Magnitude(), high.Magnitude());
		}

		// One vertex of the Minkowski difference, with the two surface points it
		// came from.
		//
		// **The witnesses travel with the difference vertex** because the answer
		// a caller wants is two points on two surfaces, and recovering them
		// afterwards from the difference alone means solving for the barycentric
		// weights a second time.
		struct Vertex {
			core::Vector3 Difference;
			core::Vector3 First;
			core::Vector3 Second;
		};

		// `support(d) = first(d) - second(-d)`, with both witnesses kept.
		//
		// The origin lies inside the set this generates exactly when the two
		// shapes overlap, which is the whole of why the difference is the thing
		// being searched rather than the shapes.
		//
		// **The direction is normalised here and that is not tidiness.**
		// `SupportPoint`'s sphere case returns `centre + direction * radius`, so
		// it is exact for a unit direction and silently scales the sphere for
		// any other - and every search direction below is a closest-point vector
		// whose length is the thing being minimised, so it is never unit. A
		// sphere that grew as the shapes approached would converge to a contact
		// that is not there. The box and cylinder cases read only the direction's
		// signs and its radial part, so they are unaffected either way.
		Vertex
		Support(const ShapeInstance &first, const ShapeInstance &second, const core::Vector3 &direction) {
			const core::Vector3 unit = direction.Unit();

			Vertex vertex;
			vertex.First = SupportPoint(first, unit);
			vertex.Second = SupportPoint(second, unit * -1.0f);
			vertex.Difference = vertex.First - vertex.Second;
			return vertex;
		}

		// The closest point of a segment to the origin, as barycentric weights.
		//
		// Written as weights rather than as a point so that the caller can apply
		// them to the witnesses as well as to the difference - the two closest
		// surface points are the same combination of the same simplex.
		void
		ClosestOnSegment(const core::Vector3 &a, const core::Vector3 &b, float &weightA, float &weightB) {
			const core::Vector3 along = b - a;
			const float length = along.MagnitudeSquared();
			if (!(length > 0.0f)) {
				weightA = 1.0f;
				weightB = 0.0f;
				return;
			}

			const float t = std::clamp(-a.Dot(along) / length, 0.0f, 1.0f);
			weightA = 1.0f - t;
			weightB = t;
		}

		// The closest point of a triangle to the origin, as barycentric weights.
		//
		// The same region walk `collision::ClosestPointOnTriangle` uses, against
		// the origin rather than an arbitrary point, and returning weights for
		// the reason above.
		void ClosestOnTriangle(
			const core::Vector3 &a, const core::Vector3 &b, const core::Vector3 &c, float (&weights)[3]
		) {
			const core::Vector3 ab = b - a;
			const core::Vector3 ac = c - a;
			const core::Vector3 ao = a * -1.0f;

			const float d1 = ab.Dot(ao);
			const float d2 = ac.Dot(ao);
			if (d1 <= 0.0f && d2 <= 0.0f) {
				weights[0] = 1.0f;
				weights[1] = 0.0f;
				weights[2] = 0.0f;
				return;
			}

			const core::Vector3 bo = b * -1.0f;
			const float d3 = ab.Dot(bo);
			const float d4 = ac.Dot(bo);
			if (d3 >= 0.0f && d4 <= d3) {
				weights[0] = 0.0f;
				weights[1] = 1.0f;
				weights[2] = 0.0f;
				return;
			}

			const float vc = d1 * d4 - d3 * d2;
			if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
				const float denominator = d1 - d3;
				const float t = denominator != 0.0f ? d1 / denominator : 0.0f;
				weights[0] = 1.0f - t;
				weights[1] = t;
				weights[2] = 0.0f;
				return;
			}

			const core::Vector3 co = c * -1.0f;
			const float d5 = ab.Dot(co);
			const float d6 = ac.Dot(co);
			if (d6 >= 0.0f && d5 <= d6) {
				weights[0] = 0.0f;
				weights[1] = 0.0f;
				weights[2] = 1.0f;
				return;
			}

			const float vb = d5 * d2 - d1 * d6;
			if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
				const float denominator = d2 - d6;
				const float t = denominator != 0.0f ? d2 / denominator : 0.0f;
				weights[0] = 1.0f - t;
				weights[1] = 0.0f;
				weights[2] = t;
				return;
			}

			const float va = d3 * d6 - d5 * d4;
			if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
				const float denominator = (d4 - d3) + (d5 - d6);
				const float t = denominator != 0.0f ? (d4 - d3) / denominator : 0.0f;
				weights[0] = 0.0f;
				weights[1] = 1.0f - t;
				weights[2] = t;
				return;
			}

			const float denominator = va + vb + vc;
			if (denominator == 0.0f) {
				weights[0] = 1.0f;
				weights[1] = 0.0f;
				weights[2] = 0.0f;
				return;
			}
			weights[1] = vb / denominator;
			weights[2] = vc / denominator;
			weights[0] = 1.0f - weights[1] - weights[2];
		}

		// Whether the origin is inside a tetrahedron, and which face it is
		// outside if it is not.
		//
		// Returns the number of faces the origin is outside; `outside` names
		// them by the vertex each face omits.
		size_t OutsideFaces(const std::array<Vertex, 4> &simplex, bool (&outside)[4]) {
			// The vertex each face omits, and the three it uses, wound so that
			// the fourth is behind it.
			constexpr int FACES[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};

			size_t count = 0;
			for (int face = 0; face < 4; face++) {
				const core::Vector3 &a = simplex[static_cast<size_t>(FACES[face][0])].Difference;
				const core::Vector3 &b = simplex[static_cast<size_t>(FACES[face][1])].Difference;
				const core::Vector3 &c = simplex[static_cast<size_t>(FACES[face][2])].Difference;
				const core::Vector3 &opposite = simplex[static_cast<size_t>(face)].Difference;

				core::Vector3 normal = (b - a).Cross(c - a);
				if (normal.Dot(opposite - a) > 0.0f) {
					normal = normal * -1.0f;
				}

				outside[face] = normal.Dot(a * -1.0f) > 0.0f;
				count += outside[face] ? 1 : 0;
			}
			return count;
		}

		// The triple product `(a x b) x a`, which is the direction in the plane
		// of `a` and `b` that is perpendicular to `a` and points toward `b`.
		//
		// The whole of the simplex walk below is written out of this one
		// expression, so having it named is the difference between reading the
		// walk and re-deriving it.
		core::Vector3 PerpendicularToward(const core::Vector3 &along, const core::Vector3 &toward) {
			return along.Cross(toward).Cross(along);
		}

		// Reduces the simplex toward the origin and picks the next search
		// direction. Returns true when the simplex is a tetrahedron containing
		// the origin.
		//
		// **The classic intersection-form walk, with the newest point last.**
		// Every case asks which Voronoi region of the newest point's features
		// the origin is in, and drops whatever the answer says cannot contain
		// it. It is written as the standard nested tests rather than as a search
		// over regions because those tests are what every reference states and a
		// clever rewrite of them is where this is traditionally wrong.
		bool ReduceToOrigin(std::array<Vertex, 4> &simplex, size_t &count, core::Vector3 &direction) {
			const auto lineCase = [&simplex, &count, &direction](const Vertex &a, const Vertex &b) {
				const core::Vector3 ab = b.Difference - a.Difference;
				const core::Vector3 ao = a.Difference * -1.0f;
				if (ab.Dot(ao) > 0.0f) {
					simplex[0] = b;
					simplex[1] = a;
					count = 2;
					direction = PerpendicularToward(ab, ao);
				} else {
					simplex[0] = a;
					count = 1;
					direction = ao;
				}
			};

			// Written as a lambda taking its three points by value, because the
			// tetrahedron case calls it with points that are about to be
			// overwritten in `simplex`.
			const auto triangleCase =
				[&simplex, &count, &direction, &lineCase](const Vertex &a, const Vertex &b, const Vertex &c) {
					const core::Vector3 ab = b.Difference - a.Difference;
					const core::Vector3 ac = c.Difference - a.Difference;
					const core::Vector3 ao = a.Difference * -1.0f;
					const core::Vector3 face = ab.Cross(ac);

					if (face.Cross(ac).Dot(ao) > 0.0f) {
						if (ac.Dot(ao) > 0.0f) {
							simplex[0] = c;
							simplex[1] = a;
							count = 2;
							direction = PerpendicularToward(ac, ao);
							return;
						}
						lineCase(a, b);
						return;
					}

					if (ab.Cross(face).Dot(ao) > 0.0f) {
						lineCase(a, b);
						return;
					}

					// Above or below the face, and the winding is arranged so that
					// the next support point is taken away from the origin's side.
					if (face.Dot(ao) > 0.0f) {
						simplex[0] = c;
						simplex[1] = b;
						simplex[2] = a;
						direction = face;
					} else {
						simplex[0] = b;
						simplex[1] = c;
						simplex[2] = a;
						direction = face * -1.0f;
					}
					count = 3;
				};

			if (count == 2) {
				lineCase(simplex[1], simplex[0]);
				return false;
			}

			if (count == 3) {
				triangleCase(simplex[2], simplex[1], simplex[0]);
				return false;
			}

			const Vertex a = simplex[3];
			const Vertex b = simplex[2];
			const Vertex c = simplex[1];
			const Vertex d = simplex[0];

			const core::Vector3 ab = b.Difference - a.Difference;
			const core::Vector3 ac = c.Difference - a.Difference;
			const core::Vector3 ad = d.Difference - a.Difference;
			const core::Vector3 ao = a.Difference * -1.0f;

			if (ab.Cross(ac).Dot(ao) > 0.0f) {
				triangleCase(a, b, c);
				return false;
			}
			if (ac.Cross(ad).Dot(ao) > 0.0f) {
				triangleCase(a, c, d);
				return false;
			}
			if (ad.Cross(ab).Dot(ao) > 0.0f) {
				triangleCase(a, d, b);
				return false;
			}
			return true;
		}

		// A tetrahedron of the Minkowski difference that contains the origin, or
		// nothing when the shapes do not overlap.
		//
		// **This is what EPA has to start from, and building one from four
		// hand-picked support directions does not work.** The obvious seed -
		// two opposed supports, one across, one off the plane - produces a
		// sliver for two boxes of the same height, with the origin sitting
		// exactly on one of its faces; EPA then expands from a face at distance
		// zero and converges on the wrong side. Measured against the exact
		// box-box pair it reported 0.27 m for an overlap of 0.70.
		bool
		OriginSimplex(const ShapeInstance &first, const ShapeInstance &second, std::array<Vertex, 4> &out) {
			core::Vector3 direction = first.Frame.Position - second.Frame.Position;
			if (direction.MagnitudeSquared() < CONVEX_EPSILON) {
				direction = core::Vector3::XAxis;
			}

			std::array<Vertex, 4> simplex{};
			simplex[0] = Support(first, second, direction);
			size_t count = 1;
			direction = simplex[0].Difference * -1.0f;

			for (size_t iteration = 0; iteration < GJK_ITERATIONS; iteration++) {
				if (direction.MagnitudeSquared() < CONVEX_EPSILON) {
					// The simplex passes through the origin, so the shapes touch.
					// Any direction continues the walk; a stated one keeps the
					// answer a function of the shapes.
					direction = core::Vector3::YAxis;
				}

				const Vertex next = Support(first, second, direction);
				if (next.Difference.Dot(direction) < 0.0f) {
					return false;
				}

				simplex[count++] = next;
				if (ReduceToOrigin(simplex, count, direction)) {
					out = simplex;
					return true;
				}
			}
			return false;
		}

		// One face of the expanding polytope.
		struct Face {
			uint32_t Vertex[3] = {0, 0, 0};

			// The outward unit normal, and how far the plane is from the origin.
			core::Vector3 Normal;
			float Distance = 0.0f;

			// Whether this face is still part of the hull. Retired rather than
			// erased, because every horizon walk refers to faces by index.
			bool Live = true;
		};

		// Builds a face from three polytope vertices, in the winding it is given.
		//
		// **It does not fix the winding, and that refusal is the whole point.**
		// An earlier version flipped the normal whenever it pointed the wrong
		// way, which produces a correct plane and a *silently reordered* vertex
		// list - and the horizon walk that grows this polytope depends on two
		// adjacent faces naming their shared edge in opposite directions. One
		// swap breaks that for every face touching it, the boundary of the
		// visible region stops being a closed loop, and the cone patches a hole
		// that is not there. The polytope then has vertices sitting outside its
		// own faces, EPA converges on one of them, and the answer is a normal
		// tilted 67 degrees off the true one at two fifths of the depth. The
		// exact box-box pair is what caught it.
		//
		// So the winding is the caller's responsibility: the seed tetrahedron is
		// wound outward once, against a point known to be inside it, and every
		// face after that is coned onto a horizon edge in the direction the
		// retired face already had - which is outward by induction.
		//
		// Returns false for a face with no area, which is a polytope that has
		// folded on itself; the caller drops it rather than dividing by zero.
		bool MakeFace(const std::vector<Vertex> &points, uint32_t a, uint32_t b, uint32_t c, Face &face) {
			const core::Vector3 &first = points[a].Difference;
			const core::Vector3 &second = points[b].Difference;
			const core::Vector3 &third = points[c].Difference;

			core::Vector3 normal = (second - first).Cross(third - first);
			const float area = normal.Magnitude();
			if (!(area > CONVEX_EPSILON)) {
				return false;
			}

			normal = normal * (1.0f / area);

			face.Vertex[0] = a;
			face.Vertex[1] = b;
			face.Vertex[2] = c;
			face.Normal = normal;

			// Floored at zero: a face through the origin is at distance zero and
			// a rounding error either side of it must not become a negative
			// depth. Two shapes of the same size at the same height put the
			// origin exactly on a seed face, so this is the ordinary case.
			face.Distance = std::max(normal.Dot(first), 0.0f);
			face.Live = true;
			return true;
		}
	}

	ConvexSeparation ClosestPoints(const ShapeInstance &first, const ShapeInstance &second) {
		ConvexSeparation answer;

		// **A direction that is a fact about the shapes rather than a constant.**
		// Starting from a fixed axis makes the first support point a function of
		// how the world happens to be oriented, and on a symmetric pair it can
		// pick two antipodal features that take several extra iterations to
		// leave. The centres are the cheapest honest guess.
		core::Vector3 direction = first.Frame.Position - second.Frame.Position;
		if (direction.MagnitudeSquared() < CONVEX_EPSILON) {
			direction = core::Vector3::XAxis;
		}

		std::array<Vertex, 4> simplex{};
		size_t count = 1;
		simplex[0] = Support(first, second, direction);

		// The closest point of the current simplex to the origin, and the
		// weights that produced it - which are also what turn the simplex's
		// witnesses into the two surface points.
		core::Vector3 closest = simplex[0].Difference;
		float weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};

		for (size_t iteration = 0; iteration < GJK_ITERATIONS; iteration++) {
			// **The search direction is toward the origin from the closest
			// point, and the termination test is against that point rather than
			// against zero.** Testing `support(d) . d <= 0` is the *intersection*
			// form of GJK, which asks "can the origin be reached at all"; used in
			// a distance search it terminates on the first step and hands back
			// the simplex it started from - which is the *furthest* pair of
			// features rather than the nearest. Two unit cubes four metres apart
			// came out five metres apart, with the witness points on their far
			// faces.
			if (closest.MagnitudeSquared() <= CONVEX_EPSILON) {
				answer.Overlapping = true;
				return answer;
			}

			const core::Vector3 toOrigin = closest * -1.0f;
			const Vertex next = Support(first, second, toOrigin);

			// No progress: the furthest the shapes reach toward the origin is no
			// nearer than the simplex already is, so nothing beyond the current
			// feature can be closer.
			const float gained = closest.Dot(closest) - closest.Dot(next.Difference);
			if (gained <= CONVEX_EPSILON * std::max(1.0f, closest.MagnitudeSquared())) {
				break;
			}

			simplex[count++] = next;

			// Reduce to the feature nearest the origin, dropping the vertices
			// that feature does not use - which is what keeps the simplex from
			// carrying a corner it has already left behind.
			std::array<Vertex, 4> kept{};
			float keptWeights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
			size_t keptCount = 0;

			if (count == 2) {
				float pair[2] = {0.0f, 0.0f};
				ClosestOnSegment(simplex[0].Difference, simplex[1].Difference, pair[0], pair[1]);
				closest = simplex[0].Difference * pair[0] + simplex[1].Difference * pair[1];
				for (size_t index = 0; index < 2; index++) {
					if (pair[index] > 0.0f) {
						keptWeights[keptCount] = pair[index];
						kept[keptCount++] = simplex[index];
					}
				}
			} else if (count == 3) {
				float triple[3] = {0.0f, 0.0f, 0.0f};
				ClosestOnTriangle(
					simplex[0].Difference, simplex[1].Difference, simplex[2].Difference, triple
				);
				closest = simplex[0].Difference * triple[0] + simplex[1].Difference * triple[1] +
						  simplex[2].Difference * triple[2];
				for (size_t index = 0; index < 3; index++) {
					if (triple[index] > 0.0f) {
						keptWeights[keptCount] = triple[index];
						kept[keptCount++] = simplex[index];
					}
				}
			} else {
				bool outside[4] = {};
				if (OutsideFaces(simplex, outside) == 0) {
					answer.Overlapping = true;
					return answer;
				}

				// The face the origin is *nearest* to, over the faces it is
				// outside of. Written as a search over the four rather than as a
				// chain of tests, because the chain is where a tetrahedron case
				// is traditionally wrong.
				constexpr int FACES[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};
				float nearest = -1.0f;

				for (int face = 0; face < 4; face++) {
					if (!outside[face]) {
						continue;
					}

					const Vertex *corners[3] = {
						&simplex[static_cast<size_t>(FACES[face][0])],
						&simplex[static_cast<size_t>(FACES[face][1])],
						&simplex[static_cast<size_t>(FACES[face][2])],
					};

					float triple[3] = {0.0f, 0.0f, 0.0f};
					ClosestOnTriangle(
						corners[0]->Difference, corners[1]->Difference, corners[2]->Difference, triple
					);
					const core::Vector3 point = corners[0]->Difference * triple[0] +
												corners[1]->Difference * triple[1] +
												corners[2]->Difference * triple[2];

					const float reachSquared = point.MagnitudeSquared();
					if (nearest >= 0.0f && reachSquared >= nearest) {
						continue;
					}

					nearest = reachSquared;
					closest = point;
					keptCount = 0;
					for (size_t index = 0; index < 3; index++) {
						if (triple[index] > 0.0f) {
							keptWeights[keptCount] = triple[index];
							kept[keptCount++] = *corners[index];
						}
					}
				}
			}

			if (keptCount == 0) {
				// Every weight was zero, which only happens for a simplex whose
				// vertices coincide. The shapes touch at a point.
				answer.Overlapping = true;
				return answer;
			}

			simplex = kept;
			count = keptCount;
			for (size_t index = 0; index < 4; index++) {
				weights[index] = index < keptCount ? keptWeights[index] : 0.0f;
			}
		}

		// The two surface points are the same combination of the same simplex
		// the closest difference point is, which is why the witnesses were
		// carried along rather than recovered afterwards.
		for (size_t index = 0; index < count; index++) {
			answer.OnFirst = answer.OnFirst + simplex[index].First * weights[index];
			answer.OnSecond = answer.OnSecond + simplex[index].Second * weights[index];
		}
		answer.Distance = closest.Magnitude();
		answer.Overlapping = answer.Distance <= CONVEX_EPSILON;
		return answer;
	}

	ConvexPenetration PenetrationBetween(const ShapeInstance &first, const ShapeInstance &second) {
		ConvexPenetration answer;

		// **The tetrahedron comes from the intersection walk, not from four
		// chosen directions.** See `OriginSimplex`, which carries the
		// measurement that made this the second attempt.
		std::array<Vertex, 4> seed{};
		if (!OriginSimplex(first, second, seed)) {
			// The shapes do not overlap. Reported as "no penetration" rather
			// than as a failure - `ContactBetween` asks this of every pair whose
			// *boxes* touch, and most of those do not.
			return answer;
		}

		std::vector<Vertex> points;
		points.reserve(EPA_FACES);
		points.push_back(seed[0]);
		points.push_back(seed[1]);
		points.push_back(seed[2]);
		points.push_back(seed[3]);

		std::vector<Face> faces;
		faces.reserve(EPA_FACES);
		const auto addFace = [&faces, &points](uint32_t a, uint32_t b, uint32_t c) {
			Face face;
			if (MakeFace(points, a, b, c, face)) {
				faces.push_back(face);
			}
		};

		// **The four seed faces, wound outward once, against the vertex each one
		// omits.** From here on the winding maintains itself: every face is
		// coned onto a horizon edge in the direction the retired face had, so it
		// faces the same way its neighbour did. See `MakeFace`.
		const auto seedFace = [&points, &addFace](uint32_t a, uint32_t b, uint32_t c, uint32_t away) {
			const core::Vector3 normal = (points[b].Difference - points[a].Difference)
											 .Cross(points[c].Difference - points[a].Difference);
			if (normal.Dot(points[away].Difference - points[a].Difference) > 0.0f) {
				addFace(a, c, b);
			} else {
				addFace(a, b, c);
			}
		};

		seedFace(0, 1, 2, 3);
		seedFace(0, 1, 3, 2);
		seedFace(0, 2, 3, 1);
		seedFace(1, 2, 3, 0);
		if (faces.size() < 4) {
			return answer;
		}

		std::vector<std::pair<uint32_t, uint32_t>> horizon;

		// **The best plane found so far, so that running out of budget answers
		// with it rather than with nothing.** The polytope is an inner
		// approximation of the Minkowski difference at every step, so its
		// nearest face is always a real - if shallow - separating plane. A
		// version that fell out of the loop and returned the default said "not
		// overlapping" for two spheres that plainly were, which is a contact
		// dropped rather than a depth under-reported.
		ConvexPenetration best;

		for (size_t iteration = 0; iteration < EPA_ITERATIONS; iteration++) {
			// The live face nearest the origin. Its plane is the current best
			// answer, and the search is over when nothing reaches past it.
			size_t nearest = faces.size();
			float closest = 0.0f;
			for (size_t index = 0; index < faces.size(); index++) {
				if (!faces[index].Live) {
					continue;
				}
				if (nearest == faces.size() || faces[index].Distance < closest) {
					nearest = index;
					closest = faces[index].Distance;
				}
			}
			if (nearest == faces.size()) {
				return best;
			}

			const core::Vector3 normal = faces[nearest].Normal;
			best.Normal = normal;
			best.Depth = closest;
			best.Overlapping = true;

			const Vertex next = Support(first, second, normal);
			const float reach = next.Difference.Dot(normal);

			if (reach - closest <= EPA_TOLERANCE || faces.size() + 8 > EPA_FACES) {
				return best;
			}

			// **A support point the polytope already holds stops the search, and
			// it is a real stopping rule rather than a guard.** A box has eight
			// support points however many directions it is asked from, so a
			// polytope of one runs out of new vertices; coning to a repeat would
			// build faces with no area. When the furthest point in this
			// direction is already a vertex, the polytope reaches as far that
			// way as the Minkowski difference does, so the current plane is on
			// its boundary and is the answer.
			//
			// **It is only sound because the polytope is convex**, which is what
			// `MakeFace` refusing to reorder its vertices buys. With a broken
			// winding a vertex can sit outside a face, and then this rule fires
			// on a face that is nowhere near the boundary.
			bool repeated = false;
			for (const Vertex &held : points) {
				if ((held.Difference - next.Difference).MagnitudeSquared() <= CONVEX_EPSILON) {
					repeated = true;
					break;
				}
			}
			if (repeated) {
				return best;
			}

			// Every face the new point can see comes off, and the boundary of
			// what came off is coned to the new point - the same horizon walk
			// the hull builder does, over a polytope instead of a hull.
			// **Coplanar counts as visible, and that is the case this gets wrong
			// otherwise.** A support point often lands exactly on a neighbouring
			// face's plane - two boxes offset along one axis put it there
			// routinely - and coning past a face the point is *on* builds a
			// second face in the same plane. Two coplanar faces have no
			// well-defined shared edge, the next horizon walk cannot tell which
			// of them owns it, and the patch that follows leaves an inward-wound
			// face at a negative distance, which then looks like the nearest
			// face and answers a depth of zero. Retiring the coplanar face
			// instead merges the region and the cone comes out convex.
			horizon.clear();
			for (Face &face : faces) {
				if (!face.Live || face.Normal.Dot(next.Difference) - face.Distance < -EPA_TOLERANCE) {
					continue;
				}
				face.Live = false;
				for (int corner = 0; corner < 3; corner++) {
					horizon.emplace_back(face.Vertex[corner], face.Vertex[(corner + 1) % 3]);
				}
			}

			// An edge that appears in both directions is interior to the removed
			// region and is not part of its boundary.
			const auto interior = [&horizon](const std::pair<uint32_t, uint32_t> &edge) {
				return std::find(horizon.begin(), horizon.end(), std::pair{edge.second, edge.first}) !=
					   horizon.end();
			};

			const auto point = static_cast<uint32_t>(points.size());
			points.push_back(next);

			size_t added = 0;
			for (const auto &edge : horizon) {
				if (interior(edge)) {
					continue;
				}
				const size_t before = faces.size();
				addFace(edge.first, edge.second, point);
				added += faces.size() > before ? 1 : 0;
			}

			if (added == 0) {
				// Nothing could be coned, so the polytope has stopped being one.
				// The last good plane is still the best answer available.
				return best;
			}
		}

		// The iteration budget, which for a curved shape is the ordinary way out
		// rather than a failure. See `EPA_ITERATIONS`.
		return best;
	}

	namespace {
		// The sweep against one convex shape, which is what conservative
		// advancement is written against.
		//
		// Split out at v0.19 so that `SweepConvex` can route a triangle mesh to
		// the walk below without recursing into itself.
		ConvexSweep SweepConvexOnly(
			const ShapeInstance &moving, const core::Vector3 &motion, const ShapeInstance &fixed
		) {
			ConvexSweep answer;

			const float travel = motion.Magnitude();
			if (!(travel > CONVEX_EPSILON)) {
				// No motion is an overlap question, and answering it here rather
				// than dividing by the travel keeps the caller from having to ask
				// two different functions depending on how fast something is going.
				const ConvexSeparation still = ClosestPoints(moving, fixed);
				if (still.Overlapping || still.Distance <= SWEEP_SKIN) {
					answer.Hit = true;
					answer.Fraction = 0.0f;
					answer.Position = still.OnSecond;
					answer.Normal = (still.OnFirst - still.OnSecond).Unit();
				}
				return answer;
			}

			const core::Vector3 direction = motion * (1.0f / travel);

			// The shape is re-placed rather than the query being told about the
			// motion, because a support function takes a frame and this is the one
			// thing a frame can express exactly.
			ShapeInstance advanced = moving;
			float covered = 0.0f;

			for (size_t advance = 0; advance < SWEEP_ADVANCES; advance++) {
				// **The position moves and the rotation does not**, which is the
				// stated limit of this sweep. Built by copying the frame and
				// replacing its position rather than by composing two frames,
				// because composing would apply the rotation to the offset and walk
				// the shape along a line that is not the motion.
				core::CFrame placed = moving.Frame;
				placed.Position = moving.Frame.Position + direction * covered;
				advanced = ShapeInstance{placed, moving.Extent, moving.Shape, moving.Hull, moving.Mesh};

				const ConvexSeparation gap = ClosestPoints(advanced, fixed);
				if (gap.Overlapping || gap.Distance <= SWEEP_SKIN) {
					answer.Hit = true;
					answer.Fraction = std::clamp(covered / travel, 0.0f, 1.0f);
					answer.Position = gap.OnSecond;

					// **From the fixed shape toward the mover**, which is the
					// direction a contact would push it and the opposite of the
					// direction it was travelling in. Taken from the two closest
					// points rather than from the motion, because a glancing hit
					// pushes sideways.
					const core::Vector3 apart = gap.OnFirst - gap.OnSecond;
					answer.Normal =
						apart.MagnitudeSquared() > CONVEX_EPSILON ? apart.Unit() : direction * -1.0f;
					return answer;
				}

				// **How fast the gap can possibly close**, which is the whole of
				// conservative advancement: the two closest points approach at most
				// at the speed of the motion projected onto the line between them,
				// so the gap cannot vanish before that much of the motion is spent.
				const core::Vector3 toward = (gap.OnSecond - gap.OnFirst).Unit();
				const float closing = direction.Dot(toward);
				if (closing <= CONVEX_EPSILON) {
					// Moving away from the nearest feature, or along it. Nothing
					// ahead can bring these two together.
					return answer;
				}

				// Every step is a *lower* bound on the time of impact, so the walk
				// approaches a contact from before it and never steps past one.
				covered += gap.Distance / closing;
				if (covered > travel) {
					return answer;
				}
			}

			// The advance budget, which a grazing approach reaches. See
			// `SWEEP_ADVANCES` for why answering "no hit" is the conservative half
			// there and would not be for a head-on approach.
			return answer;
		}

		ConvexSweep SweepConvexMotionOnly(
			const ShapeInstance &first,
			const core::Vector3 &firstLinear,
			const core::Vector3 &firstAngular,
			const ShapeInstance &second,
			const core::Vector3 &secondLinear,
			const core::Vector3 &secondAngular,
			float seconds
		) {
			ConvexSweep answer;
			if (!(seconds > 0.0f)) {
				return SweepConvexOnly(first, core::Vector3::Zero, second);
			}

			const float firstRadius = MaximumRadius(first);
			const float secondRadius = MaximumRadius(second);
			const float angularBound =
				firstAngular.Magnitude() * firstRadius + secondAngular.Magnitude() * secondRadius;
			const core::Vector3 relativeLinear = firstLinear - secondLinear;
			float elapsed = 0.0f;
			core::Vector3 lastNormal = core::Vector3::YAxis;
			core::Vector3 lastPosition;
			float lastClosing = 0.0f;

			for (size_t advance = 0; advance < MOTION_SWEEP_ADVANCES; advance++) {
				const core::CFrame firstFrame = Advanced(first.Frame, firstLinear, firstAngular, elapsed);
				const core::CFrame secondFrame = Advanced(second.Frame, secondLinear, secondAngular, elapsed);
				const ShapeInstance placedFirst{
					firstFrame, first.Extent, first.Shape, first.Hull, first.Mesh
				};
				const ShapeInstance placedSecond{
					secondFrame, second.Extent, second.Shape, second.Hull, second.Mesh
				};

				ConvexSeparation gap = ClosestPoints(placedFirst, placedSecond);
				if (gap.Overlapping && placedFirst.Shape == scene::ShapeKind::Box &&
					placedSecond.Shape == scene::ShapeKind::Box &&
					!ContactBetween(placedFirst, placedSecond).Touching) {
					const SeparatedContact exact =
						SeparatedBetween(placedFirst, placedSecond, std::numeric_limits<float>::max());
					if (exact.Found) {
						gap.OnFirst = exact.OnFirst;
						gap.OnSecond = exact.OnSecond;
						gap.Distance = exact.Distance;
						gap.Overlapping = false;
					}
				}
				if (gap.Overlapping) {
					answer.Hit = true;
					answer.Fraction = std::clamp(elapsed / seconds, 0.0f, 1.0f);
					answer.Position = gap.OnSecond;
					answer.Normal = lastNormal;
					answer.ClosingSpeed = lastClosing;
					return answer;
				}

				lastNormal = (gap.OnFirst - gap.OnSecond).Unit();
				lastPosition = gap.OnSecond;
				const core::Vector3 toward = (gap.OnSecond - gap.OnFirst).Unit();
				const core::Vector3 firstPointVelocity =
					firstLinear + firstAngular.Cross(gap.OnFirst - firstFrame.Position);
				const core::Vector3 secondPointVelocity =
					secondLinear + secondAngular.Cross(gap.OnSecond - secondFrame.Position);
				lastClosing = std::max((firstPointVelocity - secondPointVelocity).Dot(toward), 0.0f);
				if (gap.Distance <= SWEEP_SKIN) {
					if (lastClosing > CONVEX_EPSILON) {
						elapsed += (gap.Distance + SWEEP_SKIN) / lastClosing;
						if (elapsed <= seconds) {
							continue;
						}
					}
					answer.Hit = true;
					answer.Fraction = std::clamp(elapsed / seconds, 0.0f, 1.0f);
					answer.Position = gap.OnSecond;
					answer.Normal = lastNormal;
					answer.ClosingSpeed = lastClosing;
					return answer;
				}
				const float closingBound = relativeLinear.Dot(toward) + angularBound;
				if (closingBound <= CONVEX_EPSILON) {
					return answer;
				}

				elapsed += gap.Distance / closingBound;
				if (elapsed > seconds) {
					return answer;
				}
			}

			answer.Hit = true;
			answer.Fraction = std::clamp(elapsed / seconds, 0.0f, 1.0f);
			answer.Position = lastPosition;
			answer.Normal = lastNormal;
			answer.ConservativeFallback = true;
			answer.ClosingSpeed = lastClosing;
			return answer;
		}

		// The sweep against a triangle mesh, one triangle at a time.
		//
		// **A soup is not convex, so the walk above has no answer for one.**
		// `Support` returns zero reach for `ShapeKind::Mesh` - `ShapeSupport.cpp`
		// says so and calls the case unreachable - which means a sweep against
		// mesh terrain separated from everything and reported no hit at all.
		// What that cost is everything built on sweeping: `SweepFastBodies`
		// could not stop a bullet at a terrain wall and `ClipCharacterVelocity`
		// could not stop a walk at a mountainside, on any world whose ground is
		// a mesh.
		//
		// **The same shape of answer the contact path already gives.**
		// `MeshPair` gathers the triangles a body's box overlaps and pairs
		// against each as a three-point hull; this gathers the triangles the
		// *swept* box overlaps and sweeps against each. One gather, one convex
		// query per triangle, and the earliest hit wins.
		//
		// **Ties broken by triangle index**, for the reason `SweepFastBodies`
		// gives about entity ids: two triangles sharing an edge answer the same
		// fraction for a body arriving at that edge, and which of them a walk
		// reached first is a property of the mesh's build rather than of the
		// scene. The lower index is the one that is the same on two runs.
		ConvexSweep
		SweepMeshShape(const ShapeInstance &moving, const core::Vector3 &motion, const ShapeInstance &mesh) {
			ConvexSweep answer;
			if (mesh.Mesh == nullptr) {
				return answer;
			}

			// The whole of the motion, not just where it starts: a body that
			// crosses six triangles in one step has to be tested against all
			// six, and a gather around the starting pose alone would find the
			// ones it is already standing on.
			core::CFrame ended = moving.Frame;
			ended.Position = moving.Frame.Position + motion;
			const ShapeInstance arrived{ended, moving.Extent, moving.Shape, moving.Hull, moving.Mesh};

			const core::AABB world = ShapeReach(moving).Union(ShapeReach(arrived));
			const core::Vector3 corners[8] = {
				core::Vector3{world.Minimum.X, world.Minimum.Y, world.Minimum.Z},
				core::Vector3{world.Maximum.X, world.Minimum.Y, world.Minimum.Z},
				core::Vector3{world.Minimum.X, world.Maximum.Y, world.Minimum.Z},
				core::Vector3{world.Maximum.X, world.Maximum.Y, world.Minimum.Z},
				core::Vector3{world.Minimum.X, world.Minimum.Y, world.Maximum.Z},
				core::Vector3{world.Maximum.X, world.Minimum.Y, world.Maximum.Z},
				core::Vector3{world.Minimum.X, world.Maximum.Y, world.Maximum.Z},
				core::Vector3{world.Maximum.X, world.Maximum.Y, world.Maximum.Z},
			};

			// Into the mesh's own space, because its triangles are in that space
			// and moving one box there is cheaper than moving every triangle out
			// of it - `MeshPair` makes the same trade for the same reason.
			core::AABB local;
			for (size_t index = 0; index < 8; index++) {
				const core::Vector3 point = ToLocalPoint(mesh.Frame, corners[index]);
				const core::AABB one{point, point};
				local = index == 0 ? one : local.Union(one);
			}

			std::array<uint32_t, MAXIMUM_MESH_TRIANGLES> reached{};
			const size_t count = collision::OverlapTriangles(*mesh.Mesh, local, reached);

			// Reused across the walk, so a body over sixty triangles allocates
			// its three points once rather than sixty times.
			collision::ConvexHull triangle;
			uint32_t nearest = 0;

			for (size_t at = 0; at < count; at++) {
				FillTriangleHull(*mesh.Mesh, reached[at], triangle);
				const ShapeInstance placed{
					mesh.Frame, core::Vector3::Zero, scene::ShapeKind::Hull, &triangle, nullptr
				};

				ConvexSweep hit = SweepConvexOnly(moving, motion, placed);
				if (!hit.Hit) {
					continue;
				}

				// **The triangle's own plane, not the direction between the two
				// closest points.** A body that is already touching the mesh -
				// which for a character walking on terrain is every tick - gives
				// a separation of nothing, and the general sweep answers that
				// with the reverse of the motion, because between two convex
				// shapes there is nothing better to say. Against a soup there
				// is: a triangle is a plane and its normal is the direction the
				// surface pushes.
				//
				// What the made-up normal cost was a character walking three
				// studs from its spawn and stopping against what looked like a
				// vertical wall - the ground it was standing on, reported as
				// `(-1, 0, 0)` because that is where it had come from.
				//
				// **Flipped to face the approach**, so a soup resists whichever
				// side a body arrives from. A one-sided rule would let anything
				// under a floor rise through it, and a heightfield's underside
				// is where a body that has already gone wrong ends up.
				const collision::Triangle corners = mesh.Mesh->TriangleAt(reached[at]);
				const core::Vector3 plane =
					mesh.Frame.VectorToWorldSpace((corners.B - corners.A).Cross(corners.C - corners.A));
				if (plane.MagnitudeSquared() > CONVEX_EPSILON) {
					const core::Vector3 outward = plane.Unit();
					hit.Normal = outward.Dot(motion) > 0.0f ? outward * -1.0f : outward;
				}

				if (!answer.Hit || hit.Fraction < answer.Fraction ||
					(hit.Fraction == answer.Fraction && reached[at] < nearest)) {
					answer = hit;
					nearest = reached[at];
				}
			}

			return answer;
		}
	}

	ConvexSweep
	SweepConvex(const ShapeInstance &moving, const core::Vector3 &motion, const ShapeInstance &fixed) {
		// **A mesh is the one kind that is not a convex query.** Everything else
		// - box, sphere, cylinder, hull - has a support function, and
		// conservative advancement is written against exactly that. See
		// `SweepMeshShape`.
		//
		// A *moving* mesh is not routed here and is not an oversight: nothing in
		// this engine sweeps one. `SweepFastBodies` sweeps bodies that move fast
		// and `ClipCharacterVelocity` sweeps a character, and both of those are
		// boxes - a triangle soup is the shape of a world rather than of a thing
		// in it. Should one ever move, it arrives here as its own bound, which
		// `ShapeInstance` states as the behaviour for a kind with nothing to
		// answer with.
		if (fixed.Shape == scene::ShapeKind::Mesh) {
			return SweepMeshShape(moving, motion, fixed);
		}

		return SweepConvexOnly(moving, motion, fixed);
	}

	ConvexSweep SweepConvexMotion(
		const ShapeInstance &first,
		const core::Vector3 &firstLinear,
		const core::Vector3 &firstAngular,
		const ShapeInstance &second,
		const core::Vector3 &secondLinear,
		const core::Vector3 &secondAngular,
		float seconds
	) {
		if (second.Shape != scene::ShapeKind::Mesh) {
			return SweepConvexMotionOnly(
				first, firstLinear, firstAngular, second, secondLinear, secondAngular, seconds
			);
		}

		ConvexSweep answer;
		if (second.Mesh == nullptr || secondLinear.MagnitudeSquared() > CONVEX_EPSILON ||
			secondAngular.MagnitudeSquared() > CONVEX_EPSILON) {
			return answer;
		}

		const core::CFrame ended = Advanced(first.Frame, firstLinear, firstAngular, seconds);
		const ShapeInstance arrived{ended, first.Extent, first.Shape, first.Hull, first.Mesh};
		core::AABB world = ShapeReach(first).Union(ShapeReach(arrived));
		const float angularReach = firstAngular.Magnitude() * MaximumRadius(first) * seconds;
		const core::Vector3 margin{angularReach, angularReach, angularReach};
		world = core::AABB{world.Minimum - margin, world.Maximum + margin};

		const core::Vector3 corners[8] = {
			{world.Minimum.X, world.Minimum.Y, world.Minimum.Z},
			{world.Maximum.X, world.Minimum.Y, world.Minimum.Z},
			{world.Minimum.X, world.Maximum.Y, world.Minimum.Z},
			{world.Maximum.X, world.Maximum.Y, world.Minimum.Z},
			{world.Minimum.X, world.Minimum.Y, world.Maximum.Z},
			{world.Maximum.X, world.Minimum.Y, world.Maximum.Z},
			{world.Minimum.X, world.Maximum.Y, world.Maximum.Z},
			{world.Maximum.X, world.Maximum.Y, world.Maximum.Z},
		};
		core::AABB local;
		for (size_t index = 0; index < 8; index++) {
			const core::Vector3 point = ToLocalPoint(second.Frame, corners[index]);
			const core::AABB one{point, point};
			local = index == 0 ? one : local.Union(one);
		}

		std::array<uint32_t, MAXIMUM_MESH_TRIANGLES> reached{};
		const size_t count = collision::OverlapTriangles(*second.Mesh, local, reached);
		collision::ConvexHull triangle;
		uint32_t nearest = 0;
		for (size_t at = 0; at < count; at++) {
			FillTriangleHull(*second.Mesh, reached[at], triangle);
			const ShapeInstance placed{
				second.Frame, core::Vector3::Zero, scene::ShapeKind::Hull, &triangle, nullptr
			};
			const ConvexSweep hit = SweepConvexMotionOnly(
				first, firstLinear, firstAngular, placed, core::Vector3::Zero, core::Vector3::Zero, seconds
			);
			if (hit.Hit && (!answer.Hit || hit.Fraction < answer.Fraction ||
							(hit.Fraction == answer.Fraction && reached[at] < nearest))) {
				answer = hit;
				nearest = reached[at];
			}
		}
		return answer;
	}
}
