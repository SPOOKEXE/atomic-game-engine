// The meshes the engine ships with.
//
// **The winding check is the one that matters**, and it is here rather than
// beside each generator because it is the same property for all six. A face
// wound the wrong way is culled when you look at it and drawn when you cannot,
// so a solid renders as an open shell showing its own interior - which reads as
// the renderer dropping triangles rather than as a winding bug. It shipped
// exactly once, in the cube, and the check that caught it is the ancestor of
// this file.
//
// The stronger property is below it: every closed built-in is a **manifold**,
// meaning every edge is shared by exactly two triangles and those two traverse
// it in opposite directions. That single check subsumes "no hole", "no
// duplicated face", "no face wound backwards relative to its neighbour" and
// "no missing pole triangle" - all of which are ways a generated mesh goes
// wrong that a picture of it would not show.

#include <engine/assets/Builtin.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

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
	// every generator duplicates vertices on purpose - a cube corner is three
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
		BuiltinMesh::SkinnedWedge,
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

TEST_CASE("the skinned wedge is fully bound to its only joint", "[assets][builtin]") {
	const MeshData data = MakeBuiltin(BuiltinMesh::SkinnedWedge);

	REQUIRE(data.JointCount == 1);
	for (const MeshVertex &vertex : data.Vertices) {
		CHECK(vertex.Joints[0] == 0);
		CHECK(vertex.Weights[0] == 65535);
		CHECK(vertex.Weights[1] == 0);
		CHECK(vertex.Weights[2] == 0);
		CHECK(vertex.Weights[3] == 0);
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

// --- the sheets --------------------------------------------------------------

TEST_CASE("the built-in checker is a valid, tiling, two-colour sheet", "[assets][builtin]") {
	using engine::assets::BuiltinTexture;
	using engine::assets::TextureFormat;

	const engine::assets::TextureData sheet = MakeBuiltin(BuiltinTexture::Checker);

	REQUIRE(sheet.IsValid());
	REQUIRE(sheet.Format == TextureFormat::RGBA8);
	REQUIRE(sheet.Width == sheet.Height);
	REQUIRE_FALSE(sheet.IsFlipbook());

	const auto at = [&](uint32_t x, uint32_t y) {
		const size_t offset = (static_cast<size_t>(y) * sheet.Width + x) * 4;
		return std::array<uint8_t, 4>{
			static_cast<uint8_t>(sheet.Pixels[offset]),
			static_cast<uint8_t>(sheet.Pixels[offset + 1]),
			static_cast<uint8_t>(sheet.Pixels[offset + 2]),
			static_cast<uint8_t>(sheet.Pixels[offset + 3]),
		};
	};

	// Two colours and no more. A gradient here would mean the check arithmetic
	// had picked up the pixel index rather than the cell index.
	std::map<std::array<uint8_t, 4>, size_t> seen;
	for (uint32_t y = 0; y < sheet.Height; y++) {
		for (uint32_t x = 0; x < sheet.Width; x++) {
			seen[at(x, y)]++;
		}
	}
	REQUIRE(seen.size() == 2);

	// Half each, which is what makes it read as a checkerboard rather than as
	// a pattern with a bias - and what the cell size dividing the side buys.
	for (const auto &[colour, count] : seen) {
		INFO(static_cast<int>(colour[0]));
		REQUIRE(count == static_cast<size_t>(sheet.Width) * sheet.Height / 2);
	}

	// Opaque throughout: an author's chosen sheet must not quietly be a
	// stencil.
	REQUIRE(at(0, 0)[3] == 0xFF);

	// **The corners tile.** The sheet repeats across a surface, so the column
	// past the right edge is column zero again - if the two edges held the same
	// colour the seam would show as a double-width check.
	REQUIRE(at(0, 0) != at(sheet.Width - 1, 0));
	REQUIRE(at(0, 0) != at(0, sheet.Height - 1));
}

TEST_CASE("the built-in checker arrives with its mip chain", "[assets][builtin]") {
	// **The sheet an author puts on a wall while deciding where the wall goes is
	// also the sheet they see tiled across a floor from across the map**, so the
	// one built-in texture in the engine was the one shimmering worst. It had no
	// chain until v0.15 because the box filter lived a tier above this module -
	// `assets/Resample.hpp` carries what moved and why.
	using engine::assets::BuiltinTexture;
	using engine::assets::MipLevelCount;

	const engine::assets::TextureData sheet = MakeBuiltin(BuiltinTexture::Checker);

	REQUIRE(sheet.IsValid());
	REQUIRE(sheet.LevelCount() == MipLevelCount(sheet.Width, sheet.Height));

	// **The smallest level is the mean of the two colours, not one of them.** A
	// chain of the right length built by copying rather than filtering would pass
	// the count above and still alias, so the assertion that matters is a number
	// neither check holds: the checks divide the side, so exactly half the sheet
	// is each colour and a single texel is their average.
	const std::vector<std::byte> &smallest = sheet.Mips.back();
	REQUIRE(smallest.size() == 4);
	CHECK(static_cast<int>(smallest[0]) == (0xE8 + 0x96) / 2);
	CHECK(static_cast<int>(smallest[1]) == (0x8A + 0x96) / 2);
	CHECK(static_cast<int>(smallest[2]) == (0xB0 + 0x9B) / 2);
	CHECK(static_cast<int>(smallest[3]) == 0xFF);
}

TEST_CASE("a built-in texture name round-trips and is namespaced", "[assets][builtin]") {
	using engine::assets::BUILTIN_TEXTURE_COUNT;
	using engine::assets::BuiltinTexture;

	for (uint8_t index = 0; index < BUILTIN_TEXTURE_COUNT; index++) {
		const auto texture = static_cast<BuiltinTexture>(index);
		const std::string name(BuiltinName(texture));
		INFO(name);

		REQUIRE(name.rfind("engine.", 0) == 0);

		BuiltinTexture parsed = BuiltinTexture::Checker;
		REQUIRE(BuiltinFromName(name, parsed));
		REQUIRE(parsed == texture);

		// **A mesh name is not a texture name and the two overloads must not
		// answer for each other.** They share a namespace prefix and a lookup
		// that walked the wrong table would resolve `engine.Cube` to a sheet.
		BuiltinMesh asMesh = BuiltinMesh::Cube;
		REQUIRE_FALSE(BuiltinFromName(name, asMesh));
	}

	BuiltinTexture parsed = BuiltinTexture::Checker;
	REQUIRE_FALSE(BuiltinFromName("engine.Cube", parsed));
	REQUIRE_FALSE(BuiltinFromName("", parsed));
}
