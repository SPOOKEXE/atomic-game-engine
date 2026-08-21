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

TEST_CASE("a replaced run is handed back once no frame can still read it", "[render][meshtable]") {
	// **The whole of why the table stopped growing.** `Add` used to append every
	// replacement and reclaim nothing, so an `EditableMesh` a script rewrote each
	// frame walked towards `MAXIMUM_VERTICES` and was eventually refused - a mesh
	// that silently stopped following its own instance.
	MeshTable table;
	const MeshData cube = MakeBuiltin(BuiltinMesh::Cube);

	REQUIRE(table.Add(Name("test.Cube"), cube));
	const int32_t first = table.Resolve(Name("test.Cube")).Whole.VertexOffset;

	// The second write appends: the run it just replaced was freed this instant
	// and a frame in flight may still be drawing from it.
	REQUIRE(table.Add(Name("test.Cube"), cube));
	const int32_t second = table.Resolve(Name("test.Cube")).Whole.VertexOffset;
	CHECK(second != first);
	CHECK(table.FreeVertexCount() == cube.Vertices.size());

	// Three flushes is `MeshTable::DEFERRED_FRAMES`, which is the most frames
	// this engine allows in flight - so the first run is now provably nobody's.
	for (size_t frame = 0; frame < MeshTable::DEFERRED_FRAMES; frame++) {
		table.Flush();
	}

	REQUIRE(table.Add(Name("test.Cube"), cube));
	CHECK(table.Resolve(Name("test.Cube")).Whole.VertexOffset == first);

	// The table is exactly two runs wide and stays there however long this goes
	// on, which is the property that matters: bounded, not merely slower.
	CHECK(table.PendingVertexCount() <= cube.Vertices.size() * 2);
	CHECK(table.FreeVertexCount() == cube.Vertices.size());
}

TEST_CASE("a run freed this frame is not handed out again", "[render][meshtable]") {
	// The half of the rule that keeps the picture correct. Reusing storage a
	// frame in flight is reading is a mesh tearing into another mesh, and it
	// would only show on the machines whose driver runs furthest ahead.
	MeshTable table;
	const MeshData cube = MakeBuiltin(BuiltinMesh::Cube);

	REQUIRE(table.Add(Name("test.Cube"), cube));
	const int32_t first = table.Resolve(Name("test.Cube")).Whole.VertexOffset;

	// Two more writes with no flush between them. Both free a run and neither
	// may take one back.
	REQUIRE(table.Add(Name("test.Cube"), cube));
	const int32_t second = table.Resolve(Name("test.Cube")).Whole.VertexOffset;
	REQUIRE(table.Add(Name("test.Cube"), cube));
	const int32_t third = table.Resolve(Name("test.Cube")).Whole.VertexOffset;

	CHECK(second != first);
	CHECK(third != first);
	CHECK(third != second);

	// One flush short of the rule still refuses.
	for (size_t frame = 0; frame + 1 < MeshTable::DEFERRED_FRAMES; frame++) {
		table.Flush();
	}
	REQUIRE(table.Add(Name("test.Cube"), cube));
	CHECK(table.Resolve(Name("test.Cube")).Whole.VertexOffset != first);
}

TEST_CASE("runs freed beside each other become one run", "[render][meshtable]") {
	// **Without this a mesh that shrinks and grows fragments its own hole into
	// rubble.** Two eight-vertex runs side by side are unusable for a sixteen
	// vertex mesh unless they are noticed to be adjacent.
	MeshTable table;
	const MeshData cube = MakeBuiltin(BuiltinMesh::Cube);
	const MeshData plane = MakeBuiltin(BuiltinMesh::Plane);

	// Two meshes, adjacent by construction: nothing has been freed yet, so the
	// second lands immediately after the first.
	REQUIRE(table.Add(Name("test.A"), cube));
	REQUIRE(table.Add(Name("test.B"), cube));
	const int32_t base = table.Resolve(Name("test.A")).Whole.VertexOffset;
	REQUIRE(
		table.Resolve(Name("test.B")).Whole.VertexOffset == base + static_cast<int32_t>(cube.Vertices.size())
	);

	// Replace both, which frees the two runs beside each other.
	REQUIRE(table.Add(Name("test.A"), plane));
	REQUIRE(table.Add(Name("test.B"), plane));
	CHECK(table.FreeVertexCount() == cube.Vertices.size() * 2);

	for (size_t frame = 0; frame < MeshTable::DEFERRED_FRAMES; frame++) {
		table.Flush();
	}

	// A mesh needing more than either run alone, which only fits if the two were
	// merged. It lands at the first of them.
	MeshData wide = cube;
	wide.Vertices.insert(wide.Vertices.end(), cube.Vertices.begin(), cube.Vertices.end());
	for (const uint32_t index : cube.Indices) {
		wide.Indices.push_back(index + static_cast<uint32_t>(cube.Vertices.size()));
	}
	wide.ComputeBounds();
	REQUIRE(wide.Vertices.size() == cube.Vertices.size() * 2);

	REQUIRE(table.Add(Name("test.Wide"), wide));
	CHECK(table.Resolve(Name("test.Wide")).Whole.VertexOffset == base);
	CHECK(table.FreeVertexCount() == 0);
}

TEST_CASE("a mesh rewritten forever stops growing the table", "[render][meshtable]") {
	// **The property the whole change exists for, stated as a bound.** An
	// `EditableMesh` a script rewrites every frame used to add a copy of itself
	// to the table every frame - eight million vertices is about four thousand
	// rewrites of a modest chunk, so a sculpting tool or a voxel terrain reached
	// "mesh table: full, refusing" inside a minute and then silently stopped
	// following its own instance.
	//
	// The bound is `DEFERRED_FRAMES + 1` copies: one live, and the runs freed on
	// each of the frames that may still be reading them.
	MeshTable table;
	const MeshData cube = MakeBuiltin(BuiltinMesh::Cube);

	for (int frame = 0; frame < 200; frame++) {
		REQUIRE(table.Add(Name("test.Cube"), cube));
		table.Flush();
	}

	CHECK(table.Count() == 1);
	CHECK(table.HostVertexCount() <= cube.Vertices.size() * (MeshTable::DEFERRED_FRAMES + 1));
	CHECK(table.HostIndexCount() <= cube.Indices.size() * (MeshTable::DEFERRED_FRAMES + 1));

	// And the mesh still resolves to a run inside what the table holds, rather
	// than to a stale range reclamation moved out from under it.
	const MeshEntry &entry = table.Resolve(Name("test.Cube"));
	CHECK(entry.VertexCount == cube.Vertices.size());
	CHECK(static_cast<size_t>(entry.Whole.VertexOffset) + entry.VertexCount <= table.HostVertexCount());
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
