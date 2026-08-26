#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>

#include <algorithm>
#include <cmath>

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
		if (mesh.TriangleBounds.empty() || !mesh.Bounds.Overlaps(box)) {
			return 0;
		}

		size_t written = 0;
		for (size_t triangle = 0; triangle < mesh.TriangleBounds.size(); triangle++) {
			if (!mesh.TriangleBounds[triangle].Overlaps(box)) {
				continue;
			}
			if (written >= out.size()) {
				// **The caller gets a partial answer and cannot tell.** Every
				// triangle past this point is a contact that will not be
				// generated, so a body rests on the first `out.size()` of them
				// and passes through the rest. Rate-limited because a narrow
				// phase asks this thousands of times a frame, and counted so a
				// headless run reports it with no overlay open.
				core::Metrics::Count("collision.mesh.overlap.truncated", 1.0);
				ENGINE_WARN_EVERY(
					1.0,
					"overlap buffer of {} filled at triangle {} of {}; contacts past it are lost",
					out.size(),
					triangle,
					mesh.TriangleBounds.size()
				);
				break;
			}
			out[written++] = static_cast<uint32_t>(triangle);
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
