#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numeric>

namespace engine::collision {

	namespace {
		// Whether every component of a point is a real number. One infinity in a
		// vertex makes the normal of every triangle using it a NaN, and a
		// contact normal of NaN is a velocity of NaN is a body that leaves the
		// world.
		bool FiniteVertex(const core::Vector3 &point) {
			return std::isfinite(point.X) && std::isfinite(point.Y) && std::isfinite(point.Z);
		}

		// How small a triangle may be before it has no usable normal.
		//
		// **Twice the area, because that is what the cross product gives** and
		// taking the square root to compare against a length would be a square
		// root per triangle at build time to make the constant read nicer.
		//
		// A square micrometre. Below it the normal is dominated by the float
		// error in the subtraction rather than by the geometry.
		constexpr float DEGENERATE_DOUBLE_AREA = 1e-12f;

		core::AABB BoundOfTriangle(const core::Vector3 &a, const core::Vector3 &b, const core::Vector3 &c) {
			return core::AABB{
				core::Vector3{
					std::min({a.X, b.X, c.X}),
					std::min({a.Y, b.Y, c.Y}),
					std::min({a.Z, b.Z, c.Z}),
				},
				core::Vector3{
					std::max({a.X, b.X, c.X}),
					std::max({a.Y, b.Y, c.Y}),
					std::max({a.Z, b.Z, c.Z}),
				},
			};
		}

		float Component(const core::Vector3 &value, size_t axis) {
			return axis == 0 ? value.X : (axis == 1 ? value.Y : value.Z);
		}
	}

	TriangleMesh
	BuildTriangleMesh(std::span<const core::Vector3> vertices, std::span<const uint32_t> indices) {
		TriangleMesh mesh;
		mesh.Vertices.assign(vertices.begin(), vertices.end());

		const size_t triangles = indices.size() / 3;
		mesh.Indices.reserve(triangles * 3);
		mesh.TriangleBounds.reserve(triangles);

		// A trailing partial triangle is dropped by the division above and is
		// always a caller that built its index buffer wrong.
		if (indices.size() % 3 != 0) {
			ENGINE_WARN(
				"{} indices is not a whole number of triangles; {} ignored",
				indices.size(),
				indices.size() % 3
			);
		}

		// Three reasons a triangle does not survive, kept apart because they
		// mean different things: an index past the end is a corrupt buffer, a
		// non-finite vertex is an importer that let a NaN through, and a sliver
		// is ordinary baked geometry.
		size_t outOfRange = 0;
		size_t nonFinite = 0;
		size_t degenerate = 0;

		for (size_t triangle = 0; triangle < triangles; triangle++) {
			const uint32_t first = indices[triangle * 3];
			const uint32_t second = indices[triangle * 3 + 1];
			const uint32_t third = indices[triangle * 3 + 2];

			// An index past the end is the one failure that is not merely a bad
			// answer - it is a read of somebody else's memory.
			if (first >= vertices.size() || second >= vertices.size() || third >= vertices.size()) {
				outOfRange++;
				continue;
			}

			const core::Vector3 &a = vertices[first];
			const core::Vector3 &b = vertices[second];
			const core::Vector3 &c = vertices[third];
			if (!FiniteVertex(a) || !FiniteVertex(b) || !FiniteVertex(c)) {
				nonFinite++;
				continue;
			}

			// **Area rather than "are two corners equal".** A sliver whose three
			// corners are distinct and collinear is degenerate in exactly the
			// way that matters, and comparing corners misses it.
			if ((b - a).Cross(c - a).MagnitudeSquared() <= DEGENERATE_DOUBLE_AREA) {
				degenerate++;
				continue;
			}

			mesh.Indices.push_back(first);
			mesh.Indices.push_back(second);
			mesh.Indices.push_back(third);
			mesh.TriangleBounds.push_back(BoundOfTriangle(a, b, c));
		}

		// **The whole mesh's bound is the union of the triangles that survived**,
		// not of the vertices - a vertex no live triangle names is not part of
		// this collider and a bound that included it would hand the broad phase
		// a shape larger than the surface.
		if (!mesh.TriangleBounds.empty()) {
			mesh.Bounds = mesh.TriangleBounds[0];
			for (const core::AABB &bound : mesh.TriangleBounds) {
				mesh.Bounds = mesh.Bounds.Union(bound);
			}

			// Release-derived bench preset, 8192-triangle terrain chunk: 984 us full-sort
			// build versus 858 us with cached centroids and median partitions.
			constexpr size_t LEAF_TRIANGLES = 4;
			mesh.HierarchyTriangles.resize(mesh.TriangleBounds.size());
			std::iota(mesh.HierarchyTriangles.begin(), mesh.HierarchyTriangles.end(), uint32_t{0});
			if (mesh.TriangleBounds.size() <= LEAF_TRIANGLES) {
				TriangleBvhNode node;
				node.Bounds = mesh.Bounds;
				node.Count = static_cast<uint32_t>(mesh.TriangleBounds.size());
				mesh.Hierarchy.push_back(node);
			} else {
				// A balanced binary split needs no more than this complete leaf level.
				const size_t leaves =
					std::bit_ceil((mesh.TriangleBounds.size() + LEAF_TRIANGLES - 1) / LEAF_TRIANGLES);
				mesh.Hierarchy.reserve(leaves * 2 - 1);
				std::vector<core::Vector3> centroids;
				centroids.reserve(mesh.TriangleBounds.size());
				for (const auto &bound : mesh.TriangleBounds)
					centroids.push_back((bound.Minimum + bound.Maximum) * 0.5f);
				const auto less = [&](size_t axis, uint32_t left, uint32_t right) {
					const float a = Component(centroids[left], axis), b = Component(centroids[right], axis);
					return a != b ? a < b : left < right;
				};
				const auto build = [&](auto &&self, size_t begin, size_t end, size_t parentAxis) -> uint32_t {
					const uint32_t nodeIndex = static_cast<uint32_t>(mesh.Hierarchy.size());
					mesh.Hierarchy.emplace_back();

					TriangleBvhNode &node = mesh.Hierarchy[nodeIndex];
					if (end - begin <= LEAF_TRIANGLES) {
						node.Bounds = mesh.TriangleBounds[mesh.HierarchyTriangles[begin]];
						for (size_t at = begin + 1; at < end; ++at)
							node.Bounds = node.Bounds.Union(mesh.TriangleBounds[mesh.HierarchyTriangles[at]]);
						// Median partitioning leaves each leaf unordered. Restore its parent's
						// total order so the hierarchy retains the full-sort builder's triangle order.
						if (parentAxis < 3)
							std::sort(
								mesh.HierarchyTriangles.begin() + begin,
								mesh.HierarchyTriangles.begin() + end,
								[&](uint32_t left, uint32_t right) { return less(parentAxis, left, right); }
							);
						node.First = static_cast<uint32_t>(begin);
						node.Count = static_cast<uint32_t>(end - begin);
						return nodeIndex;
					}

					core::AABB centroidBounds{
						centroids[mesh.HierarchyTriangles[begin]],
						centroids[mesh.HierarchyTriangles[begin]],
					};
					for (size_t at = begin + 1; at < end; at++) {
						const core::Vector3 centroid = centroids[mesh.HierarchyTriangles[at]];
						centroidBounds = centroidBounds.Union(core::AABB{centroid, centroid});
					}

					const core::Vector3 extent = centroidBounds.Size();
					const size_t axis =
						extent.X >= extent.Y && extent.X >= extent.Z ? 0 : (extent.Y >= extent.Z ? 1 : 2);
					const size_t middle = begin + (end - begin) / 2;
					std::nth_element(
						mesh.HierarchyTriangles.begin() + begin,
						mesh.HierarchyTriangles.begin() + middle,
						mesh.HierarchyTriangles.begin() + end,
						[&](uint32_t left, uint32_t right) { return less(axis, left, right); }
					);
					const uint32_t left = self(self, begin, middle, axis);
					const uint32_t right = self(self, middle, end, axis);
					mesh.Hierarchy[nodeIndex].Left = left;
					mesh.Hierarchy[nodeIndex].Right = right;
					mesh.Hierarchy[nodeIndex].Bounds =
						mesh.Hierarchy[left].Bounds.Union(mesh.Hierarchy[right].Bounds);
					return nodeIndex;
				};
				build(build, 0, mesh.TriangleBounds.size(), 3);
			}
		}

		// A corrupt index buffer is a warning because it is a bug upstream. A
		// dropped sliver is not: baked meshes are full of them, so the count
		// only appears at `debug` beside the shape of what was kept.
		if (outOfRange != 0 || nonFinite != 0) {
			ENGINE_WARN(
				"{} triangle(s) had an index past {} vertices and {} had a non-finite corner",
				outOfRange,
				vertices.size(),
				nonFinite
			);
		}
		ENGINE_DEBUG(
			"mesh: {} of {} triangles kept over {} vertices, {} degenerate",
			mesh.TriangleBounds.size(),
			triangles,
			vertices.size(),
			degenerate
		);

		return mesh;
	}

	size_t OverlapTriangles(const TriangleMesh &mesh, const core::AABB &box, std::span<uint32_t> out) {
		// The whole-mesh bound first, so a query nowhere near the terrain costs
		// one box test rather than one per triangle.
		if (mesh.Hierarchy.empty() || !mesh.Bounds.Overlaps(box)) {
			return 0;
		}

		size_t written = 0;
		bool overflowed = false;
		std::array<uint32_t, 64> stack{};
		size_t stackSize = 1;
		stack[0] = 0;
		while (stackSize != 0) {
			const TriangleBvhNode &node = mesh.Hierarchy[stack[--stackSize]];
			if (!node.Bounds.Overlaps(box)) {
				continue;
			}
			if (!node.Leaf()) {
				stack[stackSize++] = node.Right;
				stack[stackSize++] = node.Left;
				continue;
			}
			for (size_t offset = 0; offset < node.Count; offset++) {
				const uint32_t triangle = mesh.HierarchyTriangles[node.First + offset];
				if (!mesh.TriangleBounds[triangle].Overlaps(box)) {
					continue;
				}
				if (written < out.size()) {
					out[written++] = triangle;
				} else {
					overflowed = true;
					if (!out.empty()) {
						auto greatest = std::max_element(out.begin(), out.end());
						if (triangle < *greatest) {
							*greatest = triangle;
						}
					}
				}
			}
		}
		std::sort(out.begin(), out.begin() + static_cast<long>(written));
		if (overflowed) {
			core::Metrics::Count("collision.mesh.overlap.truncated", 1.0);
			ENGINE_WARN_EVERY(
				1.0,
				"overlap buffer of {} filled while querying {} triangles",
				out.size(),
				mesh.TriangleBounds.size()
			);
		}
		return written;
	}

	core::Vector3 ClosestPointOnTriangle(const Triangle &triangle, const core::Vector3 &point) {
		const core::Vector3 ab = triangle.B - triangle.A;
		const core::Vector3 ac = triangle.C - triangle.A;
		const core::Vector3 ap = point - triangle.A;

		// Outside the vertex region of A.
		const float d1 = ab.Dot(ap);
		const float d2 = ac.Dot(ap);
		if (d1 <= 0.0f && d2 <= 0.0f) {
			return triangle.A;
		}

		// Outside the vertex region of B.
		const core::Vector3 bp = point - triangle.B;
		const float d3 = ab.Dot(bp);
		const float d4 = ac.Dot(bp);
		if (d3 >= 0.0f && d4 <= d3) {
			return triangle.B;
		}

		// Inside the edge region of AB.
		const float vc = d1 * d4 - d3 * d2;
		if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
			const float denominator = d1 - d3;
			const float along = denominator != 0.0f ? d1 / denominator : 0.0f;
			return triangle.A + ab * along;
		}

		// Outside the vertex region of C.
		const core::Vector3 cp = point - triangle.C;
		const float d5 = ab.Dot(cp);
		const float d6 = ac.Dot(cp);
		if (d6 >= 0.0f && d5 <= d6) {
			return triangle.C;
		}

		// Inside the edge region of AC.
		const float vb = d5 * d2 - d1 * d6;
		if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
			const float denominator = d2 - d6;
			const float along = denominator != 0.0f ? d2 / denominator : 0.0f;
			return triangle.A + ac * along;
		}

		// Inside the edge region of BC.
		const float va = d3 * d6 - d5 * d4;
		if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
			const float denominator = (d4 - d3) + (d5 - d6);
			const float along = denominator != 0.0f ? (d4 - d3) / denominator : 0.0f;
			return triangle.B + (triangle.C - triangle.B) * along;
		}

		// Inside the face region. **Written as a division of the barycentric
		// sum rather than as two normalised weights**, because the sum is the
		// one quantity that can be zero here - a triangle with no area, which
		// `BuildTriangleMesh` drops but a caller passing a `Triangle` directly
		// may not have.
		const float denominator = va + vb + vc;
		if (denominator == 0.0f) {
			return triangle.A;
		}
		const float v = vb / denominator;
		const float w = vc / denominator;
		return triangle.A + ab * v + ac * w;
	}
}
