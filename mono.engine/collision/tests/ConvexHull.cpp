#include <engine/collision/ConvexHull.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

TEST_SUITE_ID("engine.collision.convexhull")
// The points, the planes and the bound are all core arithmetic.
TEST_DEPENDS("engine.core.types.vector3")
TEST_DEPENDS("engine.core.types.aabb")

using Catch::Approx;
using engine::collision::BuildConvexHull;
using engine::collision::ConvexHull;
using engine::collision::HullFace;
using engine::collision::MAXIMUM_HULL_POINTS;
using engine::collision::SupportDistance;
using engine::collision::SupportPoint;
using engine::core::Vector3;

namespace {
	// The eight corners of an axis-aligned box, in an order that is not the
	// order a hull builder would like.
	std::vector<Vector3> BoxCorners(float half) {
		return {
			Vector3{half, half, half},
			Vector3{-half, -half, -half},
			Vector3{half, -half, half},
			Vector3{-half, half, -half},
			Vector3{half, half, -half},
			Vector3{-half, -half, half},
			Vector3{half, -half, -half},
			Vector3{-half, half, half},
		};
	}

	// Whether every point lies on or inside every face's plane, which is the
	// definition of the answer being a hull of the input.
	bool Encloses(const ConvexHull &hull, const std::vector<Vector3> &points, float slack) {
		for (const Vector3 &point : points) {
			for (const HullFace &face : hull.Faces) {
				if (face.Normal.Dot(point) - face.Offset > slack) {
					return false;
				}
			}
		}
		return true;
	}
}

TEST_CASE("a box hulls to eight corners and six faces", "[convexhull]") {
	// The case that says the merge works. Quickhull produces twelve triangles
	// for a box; six quads is what comes out only if coplanar facets are joined,
	// and it is what a visualiser and a manifold both want.
	const std::vector<Vector3> corners = BoxCorners(1.0f);
	const ConvexHull hull = BuildConvexHull(corners);

	CHECK(hull.Solid());
	CHECK(hull.Points.size() == 8);
	CHECK(hull.Faces.size() == 6);
	for (const HullFace &face : hull.Faces) {
		CHECK(face.IndexCount == 4);
	}

	CHECK(hull.Bounds.Minimum.X == Approx(-1.0f));
	CHECK(hull.Bounds.Maximum.Z == Approx(1.0f));
}

TEST_CASE("interior points are discarded", "[convexhull]") {
	// **Most of what the builder is for.** A support query is a scan, so the
	// cost of every narrow-phase question is the length of `Points` - and a
	// baked mesh is mostly interior vertices.
	std::vector<Vector3> cloud = BoxCorners(1.0f);
	for (int step = 0; step < 200; step++) {
		const auto drift = static_cast<float>(step) * 0.004f - 0.4f;
		cloud.push_back(Vector3{drift, drift * 0.5f, -drift * 0.25f});
	}

	const ConvexHull hull = BuildConvexHull(cloud);
	CHECK(hull.Points.size() == 8);
	CHECK(Encloses(hull, cloud, 1e-3f));
}

TEST_CASE("a support query answers the cloud, not the hull's own corners", "[convexhull]") {
	// The property everything else rests on: the support of a point set and the
	// support of its hull are the same number, so a coarse build costs a worse
	// manifold and never a missed contact.
	const std::vector<Vector3> cloud = BoxCorners(2.0f);
	const ConvexHull hull = BuildConvexHull(cloud);

	const Vector3 directions[] = {
		Vector3{1.0f, 0.0f, 0.0f},
		Vector3{0.0f, -1.0f, 0.0f},
		Vector3{1.0f, 1.0f, 1.0f},
		Vector3{-0.3f, 0.7f, -0.2f},
	};

	for (const Vector3 &direction : directions) {
		float furthest = -std::numeric_limits<float>::infinity();
		for (const Vector3 &point : cloud) {
			furthest = std::max(furthest, point.Dot(direction));
		}
		CHECK(SupportDistance(hull, direction) == Approx(furthest));
		CHECK(SupportPoint(hull, direction).Dot(direction) == Approx(furthest));
	}
}

TEST_CASE("every face points outward", "[convexhull]") {
	// A face wound the wrong way is a plane whose "outside" is the inside, and
	// the symptom is a shape that repels along one face and swallows along
	// another - which reads as the solver being unstable rather than as a
	// winding mistake.
	const ConvexHull hull = BuildConvexHull(BoxCorners(1.5f));
	REQUIRE(hull.Solid());

	Vector3 centre;
	for (const Vector3 &point : hull.Points) {
		centre = centre + point;
	}
	centre = centre * (1.0f / static_cast<float>(hull.Points.size()));

	for (const HullFace &face : hull.Faces) {
		CHECK(face.Normal.Dot(centre) < face.Offset);

		// And the winding agrees with the stored normal, which is what a
		// triangle fan for drawing relies on.
		const Vector3 &a = hull.Points[hull.Loops[face.FirstIndex]];
		const Vector3 &b = hull.Points[hull.Loops[face.FirstIndex + 1]];
		const Vector3 &c = hull.Points[hull.Loops[face.FirstIndex + 2]];
		CHECK((b - a).Cross(c - a).Unit().Dot(face.Normal) > 0.9f);
	}
}

TEST_CASE("a tetrahedron keeps its four triangles", "[convexhull]") {
	// The merge must not join faces that only nearly share a plane. Four
	// triangles at real angles to each other is the case that would fail if the
	// plane test compared normals without comparing offsets, or compared
	// neither closely enough.
	const std::vector<Vector3> cloud{
		Vector3{0.0f, 0.0f, 0.0f},
		Vector3{1.0f, 0.0f, 0.0f},
		Vector3{0.0f, 1.0f, 0.0f},
		Vector3{0.0f, 0.0f, 1.0f},
	};

	const ConvexHull hull = BuildConvexHull(cloud);
	CHECK(hull.Solid());
	CHECK(hull.Points.size() == 4);
	CHECK(hull.Faces.size() == 4);
}

TEST_CASE("two parallel faces are not merged into one", "[convexhull]") {
	// The half of the plane test that is easy to leave out. The top and the
	// bottom of a box have normals that differ only in sign, so a merge keyed on
	// `abs(dot)` joins them - and the result is one face whose boundary is two
	// disjoint squares, which does not chain and falls back to triangles.
	const ConvexHull hull = BuildConvexHull(BoxCorners(1.0f));
	REQUIRE(hull.Faces.size() == 6);

	size_t upward = 0;
	size_t downward = 0;
	for (const HullFace &face : hull.Faces) {
		upward += face.Normal.Y > 0.9f ? 1 : 0;
		downward += face.Normal.Y < -0.9f ? 1 : 0;
	}
	CHECK(upward == 1);
	CHECK(downward == 1);
}

TEST_CASE("a flat cloud keeps its points and grows no faces", "[convexhull]") {
	// **The ordinary case, not the hostile one.** A baked mesh is very often a
	// single quad or a plane, and the contract is that the result still answers
	// every support query exactly - `Solid()` is how a caller asks whether there
	// is a face to draw or a plane to separate along.
	const std::vector<Vector3> flat{
		Vector3{-1.0f, 0.0f, -1.0f},
		Vector3{1.0f, 0.0f, -1.0f},
		Vector3{1.0f, 0.0f, 1.0f},
		Vector3{-1.0f, 0.0f, 1.0f},
		Vector3{0.0f, 0.0f, 0.0f},
	};

	const ConvexHull hull = BuildConvexHull(flat);
	CHECK_FALSE(hull.Solid());
	CHECK(hull.Faces.empty());
	CHECK(hull.Points.size() == 5);
	CHECK(SupportDistance(hull, Vector3{1.0f, 0.0f, 0.0f}) == Approx(1.0f));
	CHECK(SupportDistance(hull, Vector3{0.0f, 1.0f, 0.0f}) == Approx(0.0f));
	CHECK(hull.Bounds.Maximum.Y == Approx(0.0f));
}

TEST_CASE("a line, a point and nothing all build something answerable", "[convexhull]") {
	// Each of these reaches the builder from a real file, and each of them used
	// to be a division by a zero-length edge.
	const std::vector<Vector3> line{Vector3{-1.0f, 0.0f, 0.0f}, Vector3{1.0f, 0.0f, 0.0f}};
	const ConvexHull straight = BuildConvexHull(line);
	CHECK_FALSE(straight.Solid());
	CHECK(SupportDistance(straight, Vector3{1.0f, 0.0f, 0.0f}) == Approx(1.0f));

	const std::vector<Vector3> one{Vector3{3.0f, 4.0f, 5.0f}};
	const ConvexHull dot = BuildConvexHull(one);
	CHECK(dot.Points.size() == 1);
	CHECK(SupportPoint(dot, Vector3{-1.0f, -1.0f, -1.0f}) == Vector3{3.0f, 4.0f, 5.0f});

	const ConvexHull nothing = BuildConvexHull({});
	CHECK(nothing.Points.empty());
	CHECK(SupportPoint(nothing, Vector3{1.0f, 0.0f, 0.0f}) == Vector3::Zero);
	CHECK(SupportDistance(nothing, Vector3{1.0f, 0.0f, 0.0f}) == 0.0f);
}

TEST_CASE("coincident points are welded before anything counts them", "[convexhull]") {
	// A mesh whose vertices were split for texture seams has three copies of
	// every corner. A builder that treated them as three points would spend its
	// seed search deciding they are the same one, and would report a box as
	// twenty-four corners.
	std::vector<Vector3> seams;
	for (const Vector3 &corner : BoxCorners(1.0f)) {
		seams.push_back(corner);
		seams.push_back(corner);
		seams.push_back(corner);
	}

	const ConvexHull hull = BuildConvexHull(seams);
	CHECK(hull.Points.size() == 8);
	CHECK(hull.Faces.size() == 6);
}

TEST_CASE("a near-coincident pair welds across a cell boundary", "[convexhull]") {
	// **The case the weld grid can get wrong and the scan could not.** Cells are
	// one weld distance across, so two points closer together than that can
	// still land in different cells - and a grid that only looked in its own
	// cell would keep both. Each corner here is snapped onto a cell edge and its
	// twin sits a quarter of a weld below it, which puts the pair in adjacent
	// cells on two axes at once.
	//
	// A flat cloud is used deliberately: a coplanar set has no faces, so every
	// surviving point is a corner and `Points.size()` is the welded count
	// itself rather than something a hull build could have discarded for its
	// own reasons.
	const float weld = engine::collision::HULL_WELD_DISTANCE;
	const std::vector<Vector3> quad{
		Vector3{-1.0f, 0.0f, -1.0f},
		Vector3{1.0f, 0.0f, -1.0f},
		Vector3{1.0f, 0.0f, 1.0f},
		Vector3{-1.0f, 0.0f, 1.0f},
	};

	std::vector<Vector3> pairs;
	for (const Vector3 &corner : quad) {
		const Vector3 onEdge{
			std::floor(corner.X / weld) * weld,
			0.0f,
			std::floor(corner.Z / weld) * weld,
		};
		pairs.push_back(onEdge);
		pairs.push_back(Vector3{onEdge.X - weld * 0.25f, 0.0f, onEdge.Z - weld * 0.25f});
	}

	const ConvexHull hull = BuildConvexHull(pairs);
	CHECK(hull.Points.size() == 4);
}

TEST_CASE("points past the weld distance are both kept", "[convexhull]") {
	// The other side of the same boundary. Welding is a distance test and the
	// grid only decides which candidates to test, so a pair further apart than
	// the weld has to survive - otherwise the grid would be rounding geometry to
	// its own cells, which is a collider that does not match the model.
	const float weld = engine::collision::HULL_WELD_DISTANCE;
	const std::vector<Vector3> spread{
		Vector3{-1.0f, 0.0f, -1.0f},
		Vector3{1.0f, 0.0f, -1.0f},
		Vector3{1.0f, 0.0f, 1.0f},
		Vector3{-1.0f, 0.0f, 1.0f},
		Vector3{-1.0f + weld * 4.0f, 0.0f, -1.0f},
	};

	const ConvexHull hull = BuildConvexHull(spread);
	CHECK(hull.Points.size() == 5);
}

TEST_CASE("a large distinct cloud still welds", "[convexhull]") {
	// **The size the quadratic weld could not survive.** Every point here is
	// distinct, so the scan it replaced compared each against everything kept
	// before it - twenty thousand points is two hundred million distance tests,
	// which measured at over a tenth of a second inside the frame a model
	// arrived in. This case is here so the shape of that loop cannot come back
	// unnoticed; the assertion is on the answer, and the runner's timing on the
	// suite is what shows the cost.
	std::vector<Vector3> cloud;
	cloud.reserve(20000);
	for (uint32_t index = 0; index < 20000; index++) {
		// A deterministic spiral on a sphere - no two points coincide, and the
		// hull of it is the sphere.
		const float t = static_cast<float>(index) / 20000.0f;
		const float z = 1.0f - 2.0f * t;
		const float radius = std::sqrt(std::max(0.0f, 1.0f - z * z));
		const float angle = static_cast<float>(index) * 2.399963f;
		cloud.push_back(Vector3{radius * std::cos(angle), radius * std::sin(angle), z});
	}

	const ConvexHull hull = BuildConvexHull(cloud);
	CHECK(hull.Points.size() > 3);
	CHECK(hull.Points.size() <= MAXIMUM_HULL_POINTS);
	CHECK(SupportDistance(hull, Vector3{1.0f, 0.0f, 0.0f}) == Approx(1.0f).margin(0.05f));
}

TEST_CASE("a point that is not a number never reaches the build", "[convexhull]") {
	// One infinity makes every plane test meaningless: the offset becomes a NaN,
	// every point compares "not outside" against it, and the result is a hull
	// that swallows the world without anything having failed.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float huge = std::numeric_limits<float>::infinity();

	std::vector<Vector3> cloud = BoxCorners(1.0f);
	cloud.push_back(Vector3{nan, 0.0f, 0.0f});
	cloud.push_back(Vector3{0.0f, huge, 0.0f});

	const ConvexHull hull = BuildConvexHull(cloud);
	CHECK(hull.Points.size() == 8);
	CHECK(hull.Bounds.Maximum.Y == Approx(1.0f));
}

TEST_CASE("a cloud past the point cap stops growing", "[convexhull]") {
	// A support query is linear in the point count and runs several times per
	// contact per iteration, so an unbounded hull is not a detailed collider, it
	// is a frame that does not finish.
	std::vector<Vector3> sphere;
	for (int index = 0; index < 4000; index++) {
		// A deterministic spiral over the sphere - every point is a corner of
		// its own hull, which is the worst case the cap exists for.
		const auto step = static_cast<float>(index);
		const float y = 1.0f - 2.0f * step / 3999.0f;
		const float radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
		const float angle = step * 2.39996f;
		sphere.push_back(Vector3{radius * std::cos(angle), y, radius * std::sin(angle)});
	}

	const ConvexHull hull = BuildConvexHull(sphere);
	CHECK(hull.Points.size() <= MAXIMUM_HULL_POINTS);
	CHECK(hull.Solid());
}

TEST_CASE("two builds of one cloud agree", "[convexhull]") {
	// **Deterministic, which is what lets a baked hull be compared between
	// machines.** Quickhull is usually written to take the furthest outside
	// point next, which makes the result depend on a floating-point maximum;
	// this one takes them in input order for exactly that reason.
	std::vector<Vector3> cloud;
	for (int index = 0; index < 300; index++) {
		const auto step = static_cast<float>(index);
		cloud.push_back(Vector3{std::sin(step), std::cos(step * 1.7f), std::sin(step * 0.3f)});
	}

	const ConvexHull first = BuildConvexHull(cloud);
	const ConvexHull second = BuildConvexHull(cloud);

	REQUIRE(first.Points.size() == second.Points.size());
	for (size_t index = 0; index < first.Points.size(); index++) {
		CHECK(first.Points[index] == second.Points[index]);
	}
	REQUIRE(first.Loops.size() == second.Loops.size());
	for (size_t index = 0; index < first.Loops.size(); index++) {
		CHECK(first.Loops[index] == second.Loops[index]);
	}
}

TEST_CASE("the hull encloses the cloud it was built from", "[convexhull]") {
	// The one property that makes the whole thing a hull rather than a shape
	// near one. Checked over a cloud with no symmetry, because a box passes this
	// even when the horizon walk is wrong.
	std::vector<Vector3> cloud;
	for (int index = 0; index < 120; index++) {
		const auto step = static_cast<float>(index);
		cloud.push_back(
			Vector3{
				std::sin(step * 0.7f) * 2.0f,
				std::cos(step * 1.1f) * 0.5f,
				std::sin(step * 1.9f + 1.0f) * 1.25f,
			}
		);
	}

	const ConvexHull hull = BuildConvexHull(cloud);
	REQUIRE(hull.Solid());
	CHECK(Encloses(hull, cloud, 1e-3f));
}
