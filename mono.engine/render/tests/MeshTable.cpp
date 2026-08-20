// Device-free checks for the mesh table's host side.
//
// **The half of the table that does not need a GPU is the half that had the
// bug.** `Add` accumulates on the host and hands out ranges into the shared
// buffers; `Flush` sends what is new. Whether the ranges are right and whether
// a flush is a delta are both decidable with no device at all, and neither was
// covered - which is how `Renderer::AddMesh` came to flush on every arrival
// while the header two files away said a burst costs one upload.
//
// What genuinely needs a device - that the bytes land where the ranges say - is
// checked by running the client, as `AGENTS.md` requires for `Renderer.hpp`.

#include <engine/assets/Builtin.hpp>
#include <engine/core/Name.hpp>
#include <engine/render/MeshTable.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

TEST_SUITE_ID("engine.render.meshtable")
TEST_DEPENDS("engine.assets.builtin")
TEST_DEPENDS("engine.core.name")

using Catch::Approx;
using engine::assets::BuiltinMesh;
using engine::assets::MakeBuiltin;
using engine::assets::MeshData;
using engine::core::Name;
using engine::render::MeshEntry;
using engine::render::MeshTable;

namespace {
	// A mesh with a box that is not the unit one, so `Extent` and `Centre` are
	// distinguishable from their defaults.
	MeshData Offset(const MeshData &source, float shift) {
		MeshData moved = source;
		for (engine::assets::MeshVertex &vertex : moved.Vertices) {
			vertex.Position[0] += shift;
		}
		moved.ComputeBounds();
		return moved;
	}
}

TEST_CASE("adding a mesh registers it without touching the device", "[render][meshtable]") {
	// **The property the whole batching argument rests on.** `Add` is host-only,
	// so a table with no device still answers `Has` and `Resolve` - and a caller
	// that admits a hundred meshes has done no device work until it flushes.
	MeshTable table;
	const MeshData cube = MakeBuiltin(BuiltinMesh::Cube);

	REQUIRE(table.Add(Name("test.Cube"), cube));

	CHECK(table.Has(Name("test.Cube")));
	CHECK(table.Count() == 1);
	CHECK(table.UploadCount() == 0);
	CHECK(table.PendingVertexCount() == cube.Vertices.size());
	CHECK(table.PendingIndexCount() == cube.Indices.size());
}

TEST_CASE("a burst of arrivals is one pending delta, not one per mesh", "[render][meshtable]") {
	// **The regression this suite exists for.** `Renderer::AddMesh` used to call
	// `Flush` itself, and because a copy pass cannot write part of a cycled
	// buffer that meant re-sending the whole table per mesh: N arrivals moved
	// O(N^2) bytes. Nothing asserted the documented behaviour, so the header and
	// the code disagreed for three releases.
	MeshTable table;
	const MeshData cube = MakeBuiltin(BuiltinMesh::Cube);
	const MeshData plane = MakeBuiltin(BuiltinMesh::Plane);
	const MeshData sphere = MakeBuiltin(BuiltinMesh::Sphere);

	REQUIRE(table.Add(Name("test.Cube"), cube));
	REQUIRE(table.Add(Name("test.Plane"), plane));
	REQUIRE(table.Add(Name("test.Sphere"), sphere));

	CHECK(table.Count() == 3);
	CHECK(table.UploadCount() == 0);
	CHECK(
		table.PendingVertexCount() == cube.Vertices.size() + plane.Vertices.size() + sphere.Vertices.size()
	);
	CHECK(table.PendingIndexCount() == cube.Indices.size() + plane.Indices.size() + sphere.Indices.size());
}

TEST_CASE("each mesh is placed after the one before it", "[render][meshtable]") {
	// The arithmetic a draw call depends on. `VertexOffset` is added to every
	// index before it names a vertex, so the second mesh's indices are the
	// file's own and the offset is what moves them into the shared buffer.
	MeshTable table;
	const MeshData cube = MakeBuiltin(BuiltinMesh::Cube);
	const MeshData plane = MakeBuiltin(BuiltinMesh::Plane);

	REQUIRE(table.Add(Name("test.Cube"), cube));
	REQUIRE(table.Add(Name("test.Plane"), plane));

	const MeshEntry &first = table.Resolve(Name("test.Cube"));
	const MeshEntry &second = table.Resolve(Name("test.Plane"));

	CHECK(first.Whole.VertexOffset == 0);
	CHECK(first.Whole.FirstIndex == 0);
	CHECK(first.Whole.IndexCount == cube.Indices.size());

	CHECK(second.Whole.VertexOffset == static_cast<int32_t>(cube.Vertices.size()));
	CHECK(second.Whole.FirstIndex == static_cast<uint32_t>(cube.Indices.size()));
	CHECK(second.Whole.IndexCount == plane.Indices.size());
}

TEST_CASE("replacing a mesh appends rather than rewriting", "[render][meshtable]") {
	// **Why the delta is safe.** A replacement never touches a byte the device
	// already has, so everything below the upload mark stays byte-identical -
	// which is also what keeps a range a frame in flight is drawing from valid.
	// The table grows and the entry repoints; nothing is edited in place.
	MeshTable table;
	const MeshData cube = MakeBuiltin(BuiltinMesh::Cube);

	REQUIRE(table.Add(Name("test.Cube"), cube));
	const int32_t before = table.Resolve(Name("test.Cube")).Whole.VertexOffset;

	REQUIRE(table.Add(Name("test.Cube"), cube));

	CHECK(table.Count() == 1);
	CHECK(table.Resolve(Name("test.Cube")).Whole.VertexOffset != before);
	CHECK(table.PendingVertexCount() == cube.Vertices.size() * 2);
}

TEST_CASE("an unknown name resolves to the fallback rather than nothing", "[render][meshtable]") {
	// `Resolve` is on the draw loop's hot path and never returns null, so an
	// unregistered mesh has to come back as something drawable. Without a device
	// the fallback is the default entry, which is what a caller gets before
	// `Initialise` has registered the built-ins.
	MeshTable table;
	REQUIRE(table.Add(Name("test.Cube"), MakeBuiltin(BuiltinMesh::Cube)));

	CHECK_FALSE(table.Has(Name("test.Missing")));
	CHECK(table.Resolve(Name("test.Missing")).Whole.IndexCount == 0);
	CHECK_FALSE(table.Has(Name()));
	CHECK(table.Resolve(Name()).Whole.IndexCount == 0);
}

TEST_CASE("an invalid mesh or name is refused", "[render][meshtable]") {
	MeshTable table;

	CHECK_FALSE(table.Add(Name(), MakeBuiltin(BuiltinMesh::Cube)));

	MeshData broken = MakeBuiltin(BuiltinMesh::Cube);
	broken.Indices.push_back(static_cast<uint32_t>(broken.Vertices.size() + 100));
	CHECK_FALSE(table.Add(Name("test.Broken"), broken));

	CHECK(table.Count() == 0);
	CHECK(table.PendingVertexCount() == 0);
}

TEST_CASE("the entry carries the mesh's own box", "[render][meshtable]") {
	// **What turns `MeshPart.Size` into a size rather than a multiplier.** The
	// renderer scales by `HalfExtent / Extent`, so a model authored off-centre
	// has to bring its own centre with it or it sits at an offset nobody can see
	// the cause of.
	MeshTable table;
	const MeshData shifted = Offset(MakeBuiltin(BuiltinMesh::Cube), 10.0f);

	REQUIRE(table.Add(Name("test.Shifted"), shifted));

	const MeshEntry &entry = table.Resolve(Name("test.Shifted"));
	CHECK(entry.Centre.X == Approx((shifted.Minimum.X + shifted.Maximum.X) * 0.5f));
	CHECK(entry.Extent.X == Approx((shifted.Maximum.X - shifted.Minimum.X) * 0.5f));
	CHECK(entry.Extent.Y == Approx((shifted.Maximum.Y - shifted.Minimum.Y) * 0.5f));
}
