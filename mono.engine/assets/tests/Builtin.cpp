// The meshes the engine ships with.
//
// **The winding check is the one that matters**, and it is here rather than
// beside each generator because it is the same property for all six. A face
// wound the wrong way is culled when you look at it and drawn when you cannot,
// so a solid renders as an open shell showing its own interior — which reads as
// the renderer dropping triangles rather than as a winding bug. It shipped
// exactly once, in the cube, and the check that caught it is the ancestor of
// this file.
//
// The stronger property is below it: every closed built-in is a **manifold**,
// meaning every edge is shared by exactly two triangles and those two traverse
// it in opposite directions. That single check subsumes "no hole", "no
// duplicated face", "no face wound backwards relative to its neighbour" and
// "no missing pole triangle" — all of which are ways a generated mesh goes
// wrong that a picture of it would not show.

#include <engine/assets/Builtin.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <map>
#include <string>

TEST_SUITE_ID("engine.assets.builtin")

using Catch::Approx;
using engine::assets::BUILTIN_MESH_COUNT;
using engine::assets::BuiltinFromName;
using engine::assets::BuiltinMesh;
using engine::assets::BuiltinName;
using engine::assets::MakeBuiltin;
using engine::assets::MeshData;
using engine::assets::MeshVertex;

namespace {
	struct Vec {
		float X = 0.0f, Y = 0.0f, Z = 0.0f;

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

	// A position rounded onto a tenth-of-a-millimetre grid.
	//
	// **Positions and not indices are what identifies a corner here**, because
	// every generator duplicates vertices on purpose — a cube corner is three
	// vertices so its three faces stay flat, and a sphere's seam column is two
	// so the texture does not wrap backwards. An edge test keyed on indices
	// would report every one of those as a hole.
	//
	// The grid exists for the seam: `sin(2*pi)` is not zero in single
	// precision, so the duplicated column sits a ten-millionth of a metre from
	// the column it duplicates.
	std::array<int, 3> Grid(const Vec &position) {
		const auto quantise = [](float value) {
			return static_cast<int>(std::lround(static_cast<double>(value) * 10000.0));
		};
		return {quantise(position.X), quantise(position.Y), quantise(position.Z)};
	}

	std::string Describe(BuiltinMesh mesh) {
		return std::string(BuiltinName(mesh));
	}

	// Every built-in, for the loops below.
	constexpr std::array<BuiltinMesh, BUILTIN_MESH_COUNT> ALL{
		BuiltinMesh::Cube,
		BuiltinMesh::Plane,
		BuiltinMesh::Wedge,
		BuiltinMesh::CornerWedge,
		BuiltinMesh::Sphere,
		BuiltinMesh::Cylinder,
	};

	// The plane is the one built-in that is a surface rather than a solid, so
	// it is the one the manifold and containment checks skip.
	bool IsSolid(BuiltinMesh mesh) {
		return mesh != BuiltinMesh::Plane;
	}
}

TEST_CASE("every built-in is a valid mesh", "[assets][builtin]") {
	for (const BuiltinMesh mesh : ALL) {
		INFO(Describe(mesh));
		const MeshData data = MakeBuiltin(mesh);
		REQUIRE(data.IsValid());
		CHECK(data.Submeshes.empty());
	}
}

TEST_CASE("every triangle winds counter-clockwise seen from outside", "[assets][builtin]") {
	for (const BuiltinMesh mesh : ALL) {
		const MeshData data = MakeBuiltin(mesh);

		for (size_t triangle = 0; triangle < data.Indices.size(); triangle += 3) {
			const MeshVertex &a = data.Vertices[data.Indices[triangle]];
			const MeshVertex &b = data.Vertices[data.Indices[triangle + 1]];
			const MeshVertex &c = data.Vertices[data.Indices[triangle + 2]];

			const Vec geometric = (PositionOf(b) - PositionOf(a)).Cross(PositionOf(c) - PositionOf(a));

			INFO(Describe(mesh) << " triangle " << triangle / 3);

			// A zero-area triangle has no winding at all, which is why the
			// sphere's pole rows emit one triangle rather than two.
			REQUIRE(geometric.Length() > 0.0f);

			// Same direction, not merely the same axis: a negative dot is
			// exactly the inverted case. Averaged over the three declared
			// normals, because a smooth-shaded triangle's vertices legitimately
			// disagree with each other and with the face.
			const Vec declared{
				(NormalOf(a).X + NormalOf(b).X + NormalOf(c).X) / 3.0f,
				(NormalOf(a).Y + NormalOf(b).Y + NormalOf(c).Y) / 3.0f,
				(NormalOf(a).Z + NormalOf(b).Z + NormalOf(c).Z) / 3.0f,
			};
			REQUIRE(geometric.Dot(declared) > 0.0f);
		}
	}
}

TEST_CASE("every solid built-in is closed", "[assets][builtin]") {
	for (const BuiltinMesh mesh : ALL) {
		if (!IsSolid(mesh)) {
			continue;
		}

		const MeshData data = MakeBuiltin(mesh);

		// Directed edges, keyed by the two positions they run between. A closed
		// surface has every edge exactly twice and once in each direction; a
		// hole leaves one direction unmatched and a face wound backwards leaves
		// two edges pointing the same way.
		std::map<std::pair<std::array<int, 3>, std::array<int, 3>>, int> edges;
		for (size_t triangle = 0; triangle < data.Indices.size(); triangle += 3) {
			for (size_t corner = 0; corner < 3; corner++) {
				const auto from = Grid(PositionOf(data.Vertices[data.Indices[triangle + corner]]));
				const auto to = Grid(PositionOf(data.Vertices[data.Indices[triangle + (corner + 1) % 3]]));
				edges[{from, to}]++;
			}
		}

		for (const auto &[edge, count] : edges) {
			INFO(Describe(mesh));
			CHECK(count == 1);
			CHECK(edges.count({edge.second, edge.first}) == 1);
		}
	}
}

TEST_CASE("every built-in fits the unit box about its own origin", "[assets][builtin]") {
	// `render::Renderer` folds `DrawInstance::HalfExtent` into the model matrix
	// on the assumption that the mesh is one metre across. A generator that
	// returned a radius-one sphere would make every part twice the size it says
	// it is, and it would read as a physics bug because the collider would
	// still be right.
	for (const BuiltinMesh mesh : ALL) {
		INFO(Describe(mesh));
		const MeshData data = MakeBuiltin(mesh);

		for (const MeshVertex &vertex : data.Vertices) {
			for (int axis = 0; axis < 3; axis++) {
				REQUIRE(vertex.Position[axis] >= -0.5f - 1e-5f);
				REQUIRE(vertex.Position[axis] <= 0.5f + 1e-5f);
			}
		}

		CHECK(data.Maximum.X == Approx(0.5f).margin(1e-4));
		CHECK(data.Minimum.X == Approx(-0.5f).margin(1e-4));
		CHECK(data.Maximum.Z == Approx(0.5f).margin(1e-4));
		CHECK(data.Minimum.Z == Approx(-0.5f).margin(1e-4));

		// The plane is flat in Y on purpose, so it is the one exception.
		if (IsSolid(mesh)) {
			CHECK(data.Maximum.Y == Approx(0.5f).margin(1e-4));
			CHECK(data.Minimum.Y == Approx(-0.5f).margin(1e-4));
		} else {
			CHECK(data.Maximum.Y == Approx(0.0f).margin(1e-4));
		}
	}
}

TEST_CASE("every normal is unit length", "[assets][builtin]") {
	for (const BuiltinMesh mesh : ALL) {
		INFO(Describe(mesh));
		for (const MeshVertex &vertex : MakeBuiltin(mesh).Vertices) {
			REQUIRE(NormalOf(vertex).Length() == Approx(1.0f).margin(1e-4));
		}
	}
}

TEST_CASE("every texture coordinate is inside the sheet", "[assets][builtin]") {
	for (const BuiltinMesh mesh : ALL) {
		INFO(Describe(mesh));
		for (const MeshVertex &vertex : MakeBuiltin(mesh).Vertices) {
			REQUIRE(vertex.TexCoord[0] >= 0.0f);
			REQUIRE(vertex.TexCoord[0] <= 1.0f);
			REQUIRE(vertex.TexCoord[1] >= 0.0f);
			REQUIRE(vertex.TexCoord[1] <= 1.0f);
		}
	}
}

TEST_CASE("the cube's six faces cover the six axes exactly once", "[assets][builtin]") {
	std::map<std::array<int, 3>, int> normals;
	for (const MeshVertex &vertex : MakeBuiltin(BuiltinMesh::Cube).Vertices) {
		normals[{
			static_cast<int>(vertex.Normal[0]),
			static_cast<int>(vertex.Normal[1]),
			static_cast<int>(vertex.Normal[2])
		}]++;
	}

	REQUIRE(normals.size() == 6);
	for (const auto &[normal, count] : normals) {
		CHECK(count == 4);
	}
}

TEST_CASE("a name round-trips and an unknown one is not a built-in", "[assets][builtin]") {
	for (const BuiltinMesh mesh : ALL) {
		BuiltinMesh parsed = BuiltinMesh::Sphere;
		REQUIRE(BuiltinFromName(BuiltinName(mesh), parsed));
		CHECK(parsed == mesh);
	}

	BuiltinMesh ignored = BuiltinMesh::Cube;

	// The ordinary answer for every published mesh, and not an error.
	CHECK_FALSE(BuiltinFromName("meshes/rock", ignored));

	// Namespaced, so a game publishing content called `Sphere` cannot take over
	// the built-in one.
	CHECK_FALSE(BuiltinFromName("Sphere", ignored));
	CHECK_FALSE(BuiltinFromName("", ignored));
	CHECK(ignored == BuiltinMesh::Cube);
}

TEST_CASE("every built-in name is distinct", "[assets][builtin]") {
	std::map<std::string, int> seen;
	for (const BuiltinMesh mesh : ALL) {
		seen[std::string(BuiltinName(mesh))]++;
	}
	CHECK(seen.size() == BUILTIN_MESH_COUNT);
}

TEST_CASE("a built-in is the same geometry every call", "[assets][builtin]") {
	// What lets a client and a publisher agree about `engine.Sphere` without
	// either of them shipping it. Bitwise, not approximately: a client that
	// tessellated differently would compute a different bounding box and cull
	// differently.
	for (const BuiltinMesh mesh : ALL) {
		INFO(Describe(mesh));
		const MeshData first = MakeBuiltin(mesh);
		const MeshData second = MakeBuiltin(mesh);

		REQUIRE(first.Vertices.size() == second.Vertices.size());
		REQUIRE(first.Indices == second.Indices);
		for (size_t index = 0; index < first.Vertices.size(); index++) {
			for (int axis = 0; axis < 3; axis++) {
				REQUIRE(first.Vertices[index].Position[axis] == second.Vertices[index].Position[axis]);
				REQUIRE(first.Vertices[index].Normal[axis] == second.Vertices[index].Normal[axis]);
			}
		}
	}
}
