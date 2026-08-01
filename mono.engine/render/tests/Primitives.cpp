#include <engine/render/Primitives.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <set>

TEST_SUITE_ID("engine.render.primitives")

using Catch::Approx;
using engine::render::CUBE_INDICES;
using engine::render::CUBE_VERTICES;
using engine::render::MeshVertex;

namespace {
	struct Vec {
		float X, Y, Z;

		Vec operator-(const Vec &other) const {
			return {X - other.X, Y - other.Y, Z - other.Z};
		}
		Vec Cross(const Vec &other) const {
			return {Y * other.Z - Z * other.Y, Z * other.X - X * other.Z, X * other.Y - Y * other.X};
		}
		float Dot(const Vec &other) const {
			return X * other.X + Y * other.Y + Z * other.Z;
		}
		float Length() const {
			return std::sqrt(Dot(*this));
		}
	};

	Vec PositionOf(const MeshVertex &vertex) {
		return {vertex.Position[0], vertex.Position[1], vertex.Position[2]};
	}
	Vec NormalOf(const MeshVertex &vertex) {
		return {vertex.Normal[0], vertex.Normal[1], vertex.Normal[2]};
	}
}

TEST_CASE("every triangle winds counter-clockwise seen from outside", "[primitives]") {
	// The test that matters, and the one whose absence let a fully inverted
	// cube ship. A face wound the wrong way is culled when you look at it and
	// drawn when you cannot — so the cube renders as an open box showing its
	// own interior, which reads as the renderer dropping triangles rather than
	// as a winding bug.
	//
	// For counter-clockwise-from-outside, (v1 - v0) x (v2 - v0) points the same
	// way as the face's declared normal.
	for (size_t triangle = 0; triangle < CUBE_INDICES.size(); triangle += 3) {
		const auto &a = CUBE_VERTICES[CUBE_INDICES[triangle]];
		const auto &b = CUBE_VERTICES[CUBE_INDICES[triangle + 1]];
		const auto &c = CUBE_VERTICES[CUBE_INDICES[triangle + 2]];

		const Vec geometric = (PositionOf(b) - PositionOf(a)).Cross(PositionOf(c) - PositionOf(a));
		const Vec declared = NormalOf(a);

		INFO("triangle " << triangle / 3);
		REQUIRE(geometric.Length() > 0.0f);
		// Same direction, not merely the same axis: a negative dot is exactly
		// the inverted case.
		REQUIRE(geometric.Dot(declared) > 0.0f);
	}
}

TEST_CASE("all three vertices of a triangle share one normal", "[primitives]") {
	// Four vertices per face precisely so each face is flat. If a triangle
	// spans two normals, corners are being shared and the cube will shade like
	// a sphere.
	for (size_t triangle = 0; triangle < CUBE_INDICES.size(); triangle += 3) {
		const Vec first = NormalOf(CUBE_VERTICES[CUBE_INDICES[triangle]]);
		for (size_t offset = 1; offset < 3; offset++) {
			const Vec other = NormalOf(CUBE_VERTICES[CUBE_INDICES[triangle + offset]]);
			REQUIRE(first.X == Approx(other.X));
			REQUIRE(first.Y == Approx(other.Y));
			REQUIRE(first.Z == Approx(other.Z));
		}
	}
}

TEST_CASE("the six faces cover the six axes exactly once", "[primitives]") {
	std::set<std::array<int, 3>> normals;
	for (const auto &vertex : CUBE_VERTICES) {
		normals.insert(
			{static_cast<int>(vertex.Normal[0]),
			 static_cast<int>(vertex.Normal[1]),
			 static_cast<int>(vertex.Normal[2])}
		);
	}

	REQUIRE(normals.size() == 6);
	for (const auto &axis :
		 {std::array<int, 3>{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}) {
		REQUIRE(normals.count(axis) == 1);
	}
}

TEST_CASE("every normal is a unit axis", "[primitives]") {
	for (const auto &vertex : CUBE_VERTICES) {
		REQUIRE(NormalOf(vertex).Length() == Approx(1.0f));
	}
}

TEST_CASE("every vertex sits on the cube's surface", "[primitives]") {
	for (const auto &vertex : CUBE_VERTICES) {
		const Vec position = PositionOf(vertex);
		REQUIRE(std::abs(position.X) == Approx(engine::render::CUBE_HALF_EXTENT));
		REQUIRE(std::abs(position.Y) == Approx(engine::render::CUBE_HALF_EXTENT));
		REQUIRE(std::abs(position.Z) == Approx(engine::render::CUBE_HALF_EXTENT));

		// And on the face its normal names.
		const Vec normal = NormalOf(vertex);
		REQUIRE(position.Dot(normal) == Approx(engine::render::CUBE_HALF_EXTENT));
	}
}

TEST_CASE("a face's four vertices are distinct and its quad is planar", "[primitives]") {
	for (size_t face = 0; face < 6; face++) {
		const size_t base = face * 4;
		for (size_t a = 0; a < 4; a++) {
			for (size_t b = a + 1; b < 4; b++) {
				const Vec difference =
					PositionOf(CUBE_VERTICES[base + a]) - PositionOf(CUBE_VERTICES[base + b]);
				REQUIRE(difference.Length() > 0.0f);
			}
		}

		// Both triangles of the quad face the same way. A quad wound
		// 0-1-2/0-2-3 with the corners in the wrong order gives a bowtie, and
		// half of it disappears.
		const Vec first = (PositionOf(CUBE_VERTICES[base + 1]) - PositionOf(CUBE_VERTICES[base]))
							  .Cross(PositionOf(CUBE_VERTICES[base + 2]) - PositionOf(CUBE_VERTICES[base]));
		const Vec second = (PositionOf(CUBE_VERTICES[base + 2]) - PositionOf(CUBE_VERTICES[base]))
							   .Cross(PositionOf(CUBE_VERTICES[base + 3]) - PositionOf(CUBE_VERTICES[base]));
		REQUIRE(first.Dot(second) > 0.0f);
	}
}

TEST_CASE("the index buffer is a closed solid", "[primitives]") {
	REQUIRE(CUBE_INDICES.size() == 36);

	// Every vertex used exactly once per triangle it belongs to, and every one
	// of the 24 used at all — an unused vertex means a face was mis-indexed.
	std::array<int, 24> uses{};
	for (const auto index : CUBE_INDICES) {
		REQUIRE(index < CUBE_VERTICES.size());
		uses[index]++;
	}
	for (const int count : uses) {
		// Two corners of each quad are shared by both triangles, two are not.
		REQUIRE(count >= 1);
		REQUIRE(count <= 2);
	}
}
