// What an `AssetKind::Mesh`'s bytes are.
//
// The refusal cases are most of this file, for `Texture.cpp`'s reason: every
// byte reaching `Mesh::Read` came off a disk or a wire, so a count an attacker
// chose must cost a comparison rather than an allocation. The two that are
// specific to geometry are an index naming a vertex that is not there and a
// submesh run reaching past the indices - both are reads past the end of a
// buffer in whatever consumes the mesh, and neither is visible in the file.

#include <engine/assets/Mesh.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <vector>

TEST_SUITE_ID("engine.assets.mesh")

using engine::assets::Mesh;
using engine::assets::MeshData;
using engine::assets::MeshVertex;
using engine::assets::Submesh;
using engine::core::ByteReader;
using engine::core::ByteWriter;

namespace {
	MeshVertex At(float x, float y, float z) {
		MeshVertex vertex{};
		vertex.Position[0] = x;
		vertex.Position[1] = y;
		vertex.Position[2] = z;
		vertex.Normal[1] = 1.0f;
		vertex.TexCoord[0] = x;
		vertex.TexCoord[1] = z;
		return vertex;
	}

	// One triangle, which is the smallest thing the format calls a mesh.
	MeshData Triangle() {
		MeshData data;
		data.Vertices = {At(0.0f, 0.0f, 0.0f), At(1.0f, 0.0f, 0.0f), At(0.0f, 0.0f, 1.0f)};
		data.Indices = {0, 1, 2};
		data.ComputeBounds();
		return data;
	}

	std::vector<std::byte> Written(const MeshData &data) {
		ByteWriter writer;
		REQUIRE(Mesh::Write(writer, data));
		const std::span<const std::byte> bytes = writer.Bytes();
		return std::vector<std::byte>(bytes.begin(), bytes.end());
	}
}

TEST_CASE("a mesh round-trips", "[assets][mesh]") {
	MeshData source = Triangle();
	source.JointCount = 2;
	source.Vertices[0].Joints[0] = 1;
	source.Vertices[0].Weights[0] = 65535;
	source.Submeshes.push_back(Submesh{0, 3, "skin", "characters/fox_body"});
	REQUIRE(source.IsValid());

	const std::vector<std::byte> bytes = Written(source);

	MeshData read;
	ByteReader reader(bytes);
	REQUIRE(Mesh::Read(reader, read));

	CHECK(read.Vertices.size() == 3);
	CHECK(read.Indices.size() == 3);
	REQUIRE(read.Submeshes.size() == 1);
	CHECK(read.Submeshes[0].FirstIndex == 0);
	CHECK(read.Submeshes[0].IndexCount == 3);
	CHECK(read.Submeshes[0].Material == "skin");
	CHECK(read.Submeshes[0].Texture == "characters/fox_body");
	CHECK(read.Vertices[1].Position[0] == 1.0f);
	CHECK(read.Vertices[2].TexCoord[1] == 1.0f);
	CHECK(read.JointCount == 2);
	CHECK(read.Vertices[0].Joints[0] == 1);
	CHECK(read.Vertices[0].Weights[0] == 65535);
}

TEST_CASE("legacy vertices read as unskinned", "[assets][mesh]") {
	const MeshData source = Triangle();
	ByteWriter writer;
	writer.WriteUInt32(Mesh::MAGIC);
	writer.WriteUInt16(Mesh::LEGACY_VERSION);
	writer.WriteUInt32(3);
	writer.WriteUInt32(3);
	writer.WriteUInt32(0);
	for (const MeshVertex &vertex : source.Vertices) {
		for (const float coordinate : vertex.Position) {
			writer.WriteFloat(coordinate);
		}
		for (const float coordinate : vertex.Normal) {
			writer.WriteFloat(coordinate);
		}
		for (const float coordinate : vertex.TexCoord) {
			writer.WriteFloat(coordinate);
		}
	}
	for (const uint32_t index : source.Indices) {
		writer.WriteUInt32(index);
	}

	MeshData read;
	ByteReader reader(writer.Bytes());
	REQUIRE(Mesh::Read(reader, read));
	CHECK(read.JointCount == 0);
	CHECK(read.Vertices[0].Weights[0] == 0);
}

TEST_CASE("invalid skinning influences are refused", "[assets][mesh]") {
	MeshData data = Triangle();
	data.JointCount = 2;
	data.Vertices[0].Weights[0] = 65535;

	SECTION("a weighted joint is outside the palette") {
		data.Vertices[0].Joints[0] = 2;
		CHECK_FALSE(data.IsValid());
	}

	SECTION("normalized weights do not sum exactly") {
		data.Vertices[0].Joints[0] = 1;
		data.Vertices[0].Weights[0] = 32767;
		CHECK_FALSE(data.IsValid());
	}

	SECTION("the palette exceeds the runtime ceiling") {
		data.JointCount = Mesh::MAXIMUM_JOINTS + 1;
		CHECK_FALSE(data.IsValid());
	}
}

TEST_CASE("bounds are derived from the vertices and never read", "[assets][mesh]") {
	MeshData source = Triangle();

	// A publisher lying about the box is the case this exists for: the bounds
	// are what a frustum test uses, so a zero box makes a mesh disappear from
	// every view and a huge one makes it draw from everywhere. Neither is
	// visible in the file, and neither survives a round-trip because nothing
	// about the bounds is written.
	source.Minimum = engine::core::Vector3(-1000.0f, -1000.0f, -1000.0f);
	source.Maximum = engine::core::Vector3(1000.0f, 1000.0f, 1000.0f);

	// Held in a local rather than passed inline: `ByteReader` borrows and does
	// not own, so a temporary vector would be freed before the first read.
	const std::vector<std::byte> bytes = Written(source);

	MeshData read;
	ByteReader reader(bytes);
	REQUIRE(Mesh::Read(reader, read));

	CHECK(read.Minimum.X == 0.0f);
	CHECK(read.Maximum.X == 1.0f);
	CHECK(read.Maximum.Z == 1.0f);
	CHECK(read.Maximum.Y == 0.0f);
}

TEST_CASE("an empty mesh gets a zero box rather than an inverted one", "[assets][mesh]") {
	MeshData data;
	data.ComputeBounds();

	// The fold starts from infinity, so the honest answer for no vertices is
	// an inverted box - which propagates into a world AABB and makes every
	// containment test answer nonsense rather than "empty".
	CHECK(data.Minimum.X == 0.0f);
	CHECK(data.Maximum.X == 0.0f);
	CHECK(std::isfinite(data.Minimum.Y));
}

TEST_CASE("a mesh with no submeshes is valid", "[assets][mesh]") {
	const MeshData source = Triangle();
	CHECK(source.IsValid());

	const std::vector<std::byte> bytes = Written(source);

	MeshData read;
	ByteReader reader(bytes);
	REQUIRE(Mesh::Read(reader, read));
	CHECK(read.Submeshes.empty());
}

TEST_CASE("an index past the vertices is refused", "[assets][mesh]") {
	MeshData data = Triangle();
	data.Indices = {0, 1, 3};

	CHECK_FALSE(data.IsValid());

	ByteWriter writer;
	CHECK_FALSE(Mesh::Write(writer, data));
	CHECK(writer.Empty());
}

TEST_CASE("an index count that is not a multiple of three is refused", "[assets][mesh]") {
	MeshData data = Triangle();
	data.Indices = {0, 1};
	CHECK_FALSE(data.IsValid());
}

TEST_CASE("a submesh reaching past the indices is refused", "[assets][mesh]") {
	MeshData data = Triangle();

	SECTION("past the end") {
		data.Submeshes.push_back(Submesh{0, 6, "", ""});
		CHECK_FALSE(data.IsValid());
	}

	SECTION("wrapping to land back inside") {
		// Both fields fit in 32 bits and their sum does not. Added in 32 the
		// end lands at 2, which is inside the buffer and passes - which is why
		// the check is done in 64.
		data.Submeshes.push_back(Submesh{0xFFFFFFFEu, 4, "", ""});
		CHECK_FALSE(data.IsValid());
	}

	SECTION("a run that is not whole triangles") {
		data.Submeshes.push_back(Submesh{0, 2, "", ""});
		CHECK_FALSE(data.IsValid());
	}
}

TEST_CASE("a non-finite coordinate is refused", "[assets][mesh]") {
	SECTION("a NaN position") {
		MeshData data = Triangle();
		data.Vertices[0].Position[1] = std::numeric_limits<float>::quiet_NaN();
		CHECK_FALSE(data.IsValid());
	}

	SECTION("an infinite normal") {
		MeshData data = Triangle();
		data.Vertices[2].Normal[0] = std::numeric_limits<float>::infinity();
		CHECK_FALSE(data.IsValid());
	}

	SECTION("a NaN texture coordinate") {
		MeshData data = Triangle();
		data.Vertices[1].TexCoord[0] = std::numeric_limits<float>::quiet_NaN();
		CHECK_FALSE(data.IsValid());
	}
}

TEST_CASE("a wrong magic or version is refused", "[assets][mesh]") {
	std::vector<std::byte> bytes = Written(Triangle());

	SECTION("the magic") {
		bytes[0] = std::byte{0};
		MeshData read;
		ByteReader reader(bytes);
		CHECK_FALSE(Mesh::Read(reader, read));
	}

	SECTION("the version") {
		bytes[4] = std::byte{99};
		MeshData read;
		ByteReader reader(bytes);
		CHECK_FALSE(Mesh::Read(reader, read));
	}
}

TEST_CASE("a count past what the bytes hold allocates nothing", "[assets][mesh]") {
	// The decompression bomb: four million vertices declared over a header and
	// a triangle. Nothing about the file says it is not there except the
	// comparison against what is left.
	ByteWriter writer;
	writer.WriteUInt32(Mesh::MAGIC);
	writer.WriteUInt16(Mesh::VERSION);
	writer.WriteUInt32(Mesh::MAXIMUM_VERTICES);
	writer.WriteUInt32(3);
	writer.WriteUInt32(0);

	MeshData read;
	ByteReader reader(writer.Bytes());
	CHECK_FALSE(Mesh::Read(reader, read));
	CHECK(read.Vertices.empty());
}

TEST_CASE("a count past the ceiling is refused before anything else", "[assets][mesh]") {
	ByteWriter writer;
	writer.WriteUInt32(Mesh::MAGIC);
	writer.WriteUInt16(Mesh::VERSION);
	writer.WriteUInt32(Mesh::MAXIMUM_VERTICES + 1);
	writer.WriteUInt32(3);
	writer.WriteUInt32(0);

	MeshData read;
	ByteReader reader(writer.Bytes());
	CHECK_FALSE(Mesh::Read(reader, read));
}

TEST_CASE("a truncated mesh is refused rather than half read", "[assets][mesh]") {
	const std::vector<std::byte> bytes = Written(Triangle());

	for (size_t length = 1; length < bytes.size(); length++) {
		MeshData read;
		ByteReader reader(std::span<const std::byte>(bytes.data(), length));
		CHECK_FALSE(Mesh::Read(reader, read));
		CHECK(read.Vertices.empty());
	}
}

TEST_CASE("a refused mesh leaves the destination alone", "[assets][mesh]") {
	MeshData held = Triangle();
	held.Submeshes.push_back(Submesh{0, 3, "kept", ""});

	std::vector<std::byte> bytes = Written(Triangle());
	bytes[0] = std::byte{0};

	ByteReader reader(bytes);
	CHECK_FALSE(Mesh::Read(reader, held));

	// A caller reusing one across a load loop must not act on a mixture of the
	// last good mesh and a bad one.
	REQUIRE(held.Submeshes.size() == 1);
	CHECK(held.Submeshes[0].Material == "kept");
	CHECK(held.Vertices.size() == 3);
}

TEST_CASE("an over-long material name is refused", "[assets][mesh]") {
	MeshData data = Triangle();
	data.Submeshes.push_back(Submesh{0, 3, std::string(Mesh::MAXIMUM_MATERIAL_BYTES + 1, 'x'), ""});

	CHECK_FALSE(data.IsValid());

	ByteWriter writer;
	CHECK_FALSE(Mesh::Write(writer, data));
}

TEST_CASE("an empty mesh is not a mesh", "[assets][mesh]") {
	MeshData data;
	CHECK_FALSE(data.IsValid());

	ByteWriter writer;
	CHECK_FALSE(Mesh::Write(writer, data));

	data.Vertices = {At(0.0f, 0.0f, 0.0f)};
	CHECK_FALSE(data.IsValid());
}
