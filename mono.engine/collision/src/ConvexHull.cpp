#include <engine/collision/ConvexHull.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace engine::collision {

	namespace {
		// Whether every component of a point is a real number.
		//
		// One infinity makes every plane test below meaningless - a face's
		// offset becomes a NaN, every point compares "not outside" against it,
		// and the result is a hull that swallows the world without anything
		// failing.
		bool Finite(const core::Vector3 &point) {
			return std::isfinite(point.X) && std::isfinite(point.Y) && std::isfinite(point.Z);
		}

		// One triangle of the growing hull.
		//
		// **Triangles throughout the build and polygons only at the end.** A
		// quickhull that merged coplanar faces as it went would have to keep
		// them convex through every horizon cut, which is where the numerically
		// awkward cases live; merging once, at the end, over faces that are
		// already final is a pass with nothing to undo.
		struct Facet {
			uint32_t Vertex[3] = {0, 0, 0};
			core::Vector3 Normal;
			float Offset = 0.0f;

			// Whether this facet is still part of the hull. Facets are retired
			// rather than erased, because every other facet is referred to by
			// index and compacting the array would invalidate all of them.
			bool Live = true;
		};

		// The distance of a point above a facet's plane. Positive is outside.
		float Above(const Facet &facet, const core::Vector3 &point) {
			return facet.Normal.Dot(point) - facet.Offset;
		}

		// Builds a facet from three vertex indices, wound so that `interior`
		// lies behind it.
		//
		// **The interior point is what fixes the winding, and it has to be a
		// point strictly inside the seed tetrahedron.** Deriving the outward
		// direction from the winding instead needs the winding to already be
		// right, which is the thing being decided.
		Facet MakeFacet(
			std::span<const core::Vector3> points,
			uint32_t first,
			uint32_t second,
			uint32_t third,
			const core::Vector3 &interior
		) {
			Facet facet;
			facet.Vertex[0] = first;
			facet.Vertex[1] = second;
			facet.Vertex[2] = third;

			const core::Vector3 &a = points[first];
			const core::Vector3 &b = points[second];
			const core::Vector3 &c = points[third];
			facet.Normal = (b - a).Cross(c - a).Unit();
			facet.Offset = facet.Normal.Dot(a);

			if (facet.Normal.Dot(interior) > facet.Offset) {
				facet.Normal = facet.Normal * -1.0f;
				facet.Offset = -facet.Offset;
				std::swap(facet.Vertex[1], facet.Vertex[2]);
			}
			return facet;
		}

		// The input with non-finite points dropped and coincident ones welded.
		//
		// **Welded first, because every count below is a count of *distinct*
		// points.** A mesh whose vertices were split for texture seams has three
		// copies of every corner, and a builder that treated them as three
		// points would spend its whole degeneracy budget deciding they are the
		// same one.
		//
		// Quadratic in the surviving count and bounded by it: the cap is applied
		// to the welded set, so a million-vertex mesh welds against at most
		// `MAXIMUM_HULL_POINTS * 4` survivors before the scan stops growing.
		std::vector<core::Vector3> Distinct(std::span<const core::Vector3> points, float weld) {
			std::vector<core::Vector3> kept;
			const float squared = weld * weld;

			for (const core::Vector3 &point : points) {
				if (!Finite(point)) {
					continue;
				}

				bool duplicate = false;
				for (const core::Vector3 &seen : kept) {
					if ((seen - point).MagnitudeSquared() <= squared) {
						duplicate = true;
						break;
					}
				}
				if (!duplicate) {
					kept.push_back(point);
				}
			}
			return kept;
		}

		// The bound of a point set, or an empty box for an empty one.
		core::AABB BoundOf(std::span<const core::Vector3> points) {
			if (points.empty()) {
				return core::AABB{};
			}

			core::Vector3 minimum = points[0];
			core::Vector3 maximum = points[0];
			for (const core::Vector3 &point : points) {
				minimum = core::Vector3{
					std::min(minimum.X, point.X),
					std::min(minimum.Y, point.Y),
					std::min(minimum.Z, point.Z),
				};
				maximum = core::Vector3{
					std::max(maximum.X, point.X),
					std::max(maximum.Y, point.Y),
					std::max(maximum.Z, point.Z),
				};
			}
			return core::AABB{minimum, maximum};
		}

		// The four points of a non-degenerate starting tetrahedron.
		//
		// The two furthest apart on the widest axis, then the point furthest
		// from that line, then the point furthest from that plane. Returns false
		// when any stage finds nothing far enough away, which is exactly the
		// "all collinear" and "all coplanar" cases the header promises.
		bool SeedTetrahedron(std::span<const core::Vector3> points, float tolerance, uint32_t (&seed)[4]) {
			if (points.size() < 4) {
				return false;
			}

			// **By extent on each axis with the index breaking a tie**, so the
			// seed is a function of the point set rather than of whichever
			// candidate the comparison happened to see first.
			size_t lowest = 0;
			size_t highest = 0;
			float widest = -1.0f;
			for (int axis = 0; axis < 3; axis++) {
				size_t low = 0;
				size_t high = 0;
				for (size_t index = 1; index < points.size(); index++) {
					const float value = axis == 0	? points[index].X
										: axis == 1 ? points[index].Y
													: points[index].Z;
					const float least = axis == 0 ? points[low].X : axis == 1 ? points[low].Y : points[low].Z;
					const float most = axis == 0   ? points[high].X
									   : axis == 1 ? points[high].Y
												   : points[high].Z;
					if (value < least) {
						low = index;
					}
					if (value > most) {
						high = index;
					}
				}

				const float span = (points[high] - points[low]).Magnitude();
				if (span > widest) {
					widest = span;
					lowest = low;
					highest = high;
				}
			}

			if (!(widest > tolerance)) {
				return false;
			}

			// The point furthest from the line through the first two.
			const core::Vector3 along = (points[highest] - points[lowest]).Unit();
			size_t third = 0;
			float furthest = -1.0f;
			for (size_t index = 0; index < points.size(); index++) {
				const core::Vector3 offset = points[index] - points[lowest];
				const float distance = (offset - along * offset.Dot(along)).Magnitude();
				if (distance > furthest) {
					furthest = distance;
					third = index;
				}
			}
			if (!(furthest > tolerance)) {
				return false;
			}

			// The point furthest from the plane through the first three.
			const core::Vector3 normal =
				(points[highest] - points[lowest]).Cross(points[third] - points[lowest]).Unit();
			const float plane = normal.Dot(points[lowest]);
			size_t fourth = 0;
			float deepest = -1.0f;
			for (size_t index = 0; index < points.size(); index++) {
				const float distance = std::abs(normal.Dot(points[index]) - plane);
				if (distance > deepest) {
					deepest = distance;
					fourth = index;
				}
			}
			if (!(deepest > tolerance)) {
				return false;
			}

			seed[0] = static_cast<uint32_t>(lowest);
			seed[1] = static_cast<uint32_t>(highest);
			seed[2] = static_cast<uint32_t>(third);
			seed[3] = static_cast<uint32_t>(fourth);
			return true;
		}

		// Turns the live facets into faces, merging ones that share a plane.
		//
		// **Merged by walking each face's boundary rather than by re-triangulating
		// a point set.** A merged group's outline is the edges that appear once
		// among its facets; chaining those gives the loop directly, in the
		// winding the facets already had, with no second convexity decision to
		// get wrong.
		//
		// A group whose boundary does not chain into one closed loop is emitted
		// as its individual triangles instead. That happens when a merge would
		// have produced a face with a hole or two disjoint pieces, which a
		// convex hull cannot have - so reaching it means the plane test grouped
		// two genuinely different faces, and falling back to triangles is a
		// coarser answer rather than a wrong one.
		void EmitFaces(const std::vector<Facet> &facets, float tolerance, ConvexHull &hull) {
			std::vector<uint32_t> group;
			std::vector<bool> taken(facets.size(), false);

			const auto emitTriangle = [&hull](const Facet &facet) {
				HullFace face;
				face.FirstIndex = static_cast<uint32_t>(hull.Loops.size());
				face.IndexCount = 3;
				face.Normal = facet.Normal;
				face.Offset = facet.Offset;
				hull.Loops.push_back(facet.Vertex[0]);
				hull.Loops.push_back(facet.Vertex[1]);
				hull.Loops.push_back(facet.Vertex[2]);
				hull.Faces.push_back(face);
			};

			for (size_t index = 0; index < facets.size(); index++) {
				if (!facets[index].Live || taken[index]) {
					continue;
				}

				// Everything on this facet's plane. **Both the normal and the
				// offset**, because two parallel faces on opposite sides of a
				// box agree on the first and must not be merged.
				group.clear();
				group.push_back(static_cast<uint32_t>(index));
				taken[index] = true;

				for (size_t other = index + 1; other < facets.size(); other++) {
					if (!facets[other].Live || taken[other]) {
						continue;
					}
					if (facets[other].Normal.Dot(facets[index].Normal) < 1.0f - tolerance) {
						continue;
					}
					if (std::abs(facets[other].Offset - facets[index].Offset) > tolerance) {
						continue;
					}
					group.push_back(static_cast<uint32_t>(other));
					taken[other] = true;
				}

				if (group.size() == 1) {
					emitTriangle(facets[index]);
					continue;
				}

				// The boundary is the directed edges that have no opposite.
				std::vector<std::pair<uint32_t, uint32_t>> edges;
				for (uint32_t member : group) {
					const Facet &facet = facets[member];
					for (int corner = 0; corner < 3; corner++) {
						edges.emplace_back(facet.Vertex[corner], facet.Vertex[(corner + 1) % 3]);
					}
				}

				std::vector<std::pair<uint32_t, uint32_t>> boundary;
				for (const auto &edge : edges) {
					const bool shared =
						std::find(edges.begin(), edges.end(), std::pair{edge.second, edge.first}) !=
						edges.end();
					if (!shared) {
						boundary.push_back(edge);
					}
				}

				// Chain them. A convex face's boundary is one closed loop; if it
				// does not close, the group was not one face.
				std::vector<uint32_t> loop;
				bool closed = !boundary.empty();
				if (closed) {
					loop.push_back(boundary[0].first);
					uint32_t at = boundary[0].second;
					std::vector<bool> used(boundary.size(), false);
					used[0] = true;

					for (size_t step = 1; step < boundary.size(); step++) {
						bool found = false;
						for (size_t candidate = 0; candidate < boundary.size(); candidate++) {
							if (used[candidate] || boundary[candidate].first != at) {
								continue;
							}
							loop.push_back(at);
							at = boundary[candidate].second;
							used[candidate] = true;
							found = true;
							break;
						}
						if (!found) {
							closed = false;
							break;
						}
					}
					closed = closed && at == loop.front() && loop.size() >= 3;
				}

				if (!closed) {
					for (uint32_t member : group) {
						emitTriangle(facets[member]);
					}
					continue;
				}

				HullFace face;
				face.FirstIndex = static_cast<uint32_t>(hull.Loops.size());
				face.IndexCount = static_cast<uint32_t>(loop.size());
				face.Normal = facets[index].Normal;
				face.Offset = facets[index].Offset;
				hull.Loops.insert(hull.Loops.end(), loop.begin(), loop.end());
				hull.Faces.push_back(face);
			}
		}
	}

	core::Vector3 SupportPoint(const ConvexHull &hull, const core::Vector3 &direction) {
		if (hull.Points.empty()) {
			return core::Vector3::Zero;
		}

		size_t best = 0;
		float furthest = hull.Points[0].Dot(direction);
		for (size_t index = 1; index < hull.Points.size(); index++) {
			const float reach = hull.Points[index].Dot(direction);
			if (reach > furthest) {
				furthest = reach;
				best = index;
			}
		}
		return hull.Points[best];
	}

	float SupportDistance(const ConvexHull &hull, const core::Vector3 &direction) {
		if (hull.Points.empty()) {
			return 0.0f;
		}
		return SupportPoint(hull, direction).Dot(direction);
	}

	ConvexHull BuildConvexHull(std::span<const core::Vector3> points, float tolerance) {
		const float epsilon = tolerance > 0.0f ? tolerance : HULL_WELD_DISTANCE;

		ConvexHull hull;
		const std::vector<core::Vector3> distinct = Distinct(points, HULL_WELD_DISTANCE);
		if (distinct.empty()) {
			return hull;
		}

		uint32_t seed[4] = {0, 0, 0, 0};
		if (!SeedTetrahedron(distinct, epsilon, seed)) {
			// Flat, straight or a single place. Every point is a corner, there
			// are no faces, and every support query is still exact - which is
			// the contract the header states rather than an excuse.
			hull.Points.assign(distinct.begin(), distinct.end());
			if (hull.Points.size() > MAXIMUM_HULL_POINTS) {
				hull.Points.resize(MAXIMUM_HULL_POINTS);
			}
			hull.Bounds = BoundOf(hull.Points);
			return hull;
		}

		// The seed tetrahedron. Its centroid is strictly inside it, which is
		// what every facet's winding is fixed against.
		const core::Vector3 interior =
			(distinct[seed[0]] + distinct[seed[1]] + distinct[seed[2]] + distinct[seed[3]]) * 0.25f;

		std::vector<Facet> facets;
		facets.push_back(MakeFacet(distinct, seed[0], seed[1], seed[2], interior));
		facets.push_back(MakeFacet(distinct, seed[0], seed[1], seed[3], interior));
		facets.push_back(MakeFacet(distinct, seed[0], seed[2], seed[3], interior));
		facets.push_back(MakeFacet(distinct, seed[1], seed[2], seed[3], interior));

		std::vector<bool> inside(distinct.size(), false);
		for (uint32_t corner : seed) {
			inside[corner] = true;
		}

		size_t corners = 4;
		std::vector<uint32_t> visible;
		std::vector<std::pair<uint32_t, uint32_t>> horizon;

		// **Input order, so the hull is a function of the cloud.** Quickhull is
		// usually written to take the furthest outside point next, which
		// converges in fewer rounds and makes the result depend on a
		// floating-point maximum - two builds of one cloud on two machines could
		// then differ, which is the thing `just determinism` exists to prevent.
		for (size_t index = 0; index < distinct.size(); index++) {
			if (inside[index] || corners >= MAXIMUM_HULL_POINTS) {
				continue;
			}

			const core::Vector3 &point = distinct[index];

			visible.clear();
			for (size_t facet = 0; facet < facets.size(); facet++) {
				if (facets[facet].Live && Above(facets[facet], point) > epsilon) {
					visible.push_back(static_cast<uint32_t>(facet));
				}
			}
			if (visible.empty()) {
				continue;
			}

			// The horizon is the boundary of the visible region: the directed
			// edges of visible facets whose opposite belongs to a facet that is
			// not visible.
			horizon.clear();
			for (uint32_t facet : visible) {
				for (int corner = 0; corner < 3; corner++) {
					const uint32_t from = facets[facet].Vertex[corner];
					const uint32_t to = facets[facet].Vertex[(corner + 1) % 3];

					bool shared = false;
					for (uint32_t other : visible) {
						if (other == facet) {
							continue;
						}
						for (int edge = 0; edge < 3; edge++) {
							if (facets[other].Vertex[edge] == to &&
								facets[other].Vertex[(edge + 1) % 3] == from) {
								shared = true;
								break;
							}
						}
						if (shared) {
							break;
						}
					}
					if (!shared) {
						horizon.emplace_back(from, to);
					}
				}
			}

			if (horizon.empty()) {
				// Every facet saw the point, which means the point is not
				// outside a closed hull at all - a plane test that disagreed
				// with itself. Skipping it keeps the hull closed, which is the
				// invariant everything below depends on.
				continue;
			}

			for (uint32_t facet : visible) {
				facets[facet].Live = false;
			}
			for (const auto &edge : horizon) {
				facets.push_back(
					MakeFacet(distinct, edge.first, edge.second, static_cast<uint32_t>(index), interior)
				);
			}

			inside[index] = true;
			corners++;
		}

		// The hull's points are the ones its live facets name, in first-use
		// order - so the array is compact and the indices below refer to it
		// rather than to the welded cloud.
		std::vector<uint32_t> slotOf(distinct.size(), UINT32_MAX);
		for (Facet &facet : facets) {
			if (!facet.Live) {
				continue;
			}
			for (uint32_t &corner : facet.Vertex) {
				if (slotOf[corner] == UINT32_MAX) {
					slotOf[corner] = static_cast<uint32_t>(hull.Points.size());
					hull.Points.push_back(distinct[corner]);
				}
				corner = slotOf[corner];
			}
		}

		EmitFaces(facets, epsilon, hull);
		hull.Bounds = BoundOf(hull.Points);
		return hull;
	}
}
