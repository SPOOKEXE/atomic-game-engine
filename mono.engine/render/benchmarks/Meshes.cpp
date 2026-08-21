// Admitting geometry to the renderer, and reclaiming the slots it stops using.
//
// **The half of the GPU that a benchmark can measure honestly.** Everything
// past the transfer belongs to the driver, the display and whatever else is on
// the machine - `Overlay.cpp` says so, and this file draws the same line. What
// is on this side of it is real work and quite a lot of it: the table keeps a
// host copy of every vertex and index, decides where in the shared buffers each
// mesh's run lives, tracks which spans have changed since the last transfer,
// and hands a freed run back once no frame in flight can still be reading it.
// That is a suballocator, a dirty-span tracker and a deferred free list, and
// none of them need a device to run or to be wrong.
//
// **The rows are about admission, not about drawing.** A frame draws from the
// table; a *load* fills it. So the shapes here are the ones a load has:
// hundreds of meshes arriving in a burst, meshes arriving into a table that is
// already large, and the same name being written repeatedly - which is what a
// script that rebuilds geometry does and is the case the free list exists for.
//
// **The reclaim rows are the ones worth watching.** Replacing a name gives its
// old run back, and the run only becomes reusable once `DEFERRED_FRAMES` have
// passed - counted in `Flush` calls, whether or not a flush sent anything. Two
// things can go wrong and neither shows up as a failure: a table that never
// reclaims grows without bound until a mesh is refused, and a table that
// reclaims by walking a long free list makes every admission slower than the
// last. The rows that replace names over a fragmented table are what tell those
// apart.
//
// No device is created and none is needed. `Add` accumulates on the host,
// `Flush` advances the table's clock and only then asks a device to send
// anything, so the whole of the accounting runs with `Device` left null - which
// is the same seam `tests/MeshTable.cpp` checks correctness at. What genuinely
// needs a device, that the bytes land where the ranges say, is checked by
// running the client.

#include <engine/assets/Builtin.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/core/Name.hpp>
#include <engine/render/MeshTable.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.render.bench.meshes")

using engine::assets::MeshData;
using engine::assets::MeshVertex;
using engine::core::Name;
using engine::render::MeshTable;
using engine::testing::Consume;

namespace mesh_bench {
	// Meshes admitted per sample. A level load is this order; a burst inside
	// one frame is a tenth of it.
	constexpr size_t MESHES = 1000;

	// A prop, in vertices. Small enough that a thousand of them is a table a
	// real scene has rather than one nothing could hold.
	constexpr size_t VERTICES = 256;

	// A grid mesh of `vertices` vertices and the triangles to match, built
	// once per size and shared. Building geometry inside a measured body would
	// measure the builder.
	const MeshData &Geometry(size_t vertices) {
		struct Built {
			size_t Vertices;
			MeshData Data;
		};
		// A deque, so a reference handed out earlier survives a later size
		// being built.
		static std::deque<Built> built;

		for (const Built &entry : built) {
			if (entry.Vertices == vertices) {
				return entry.Data;
			}
		}

		MeshData data;
		data.Vertices.reserve(vertices);
		for (size_t index = 0; index < vertices; index++) {
			const float along = static_cast<float>(index % 16) * 0.1f;
			const float across = static_cast<float>(index / 16) * 0.1f;
			data.Vertices.push_back(
				MeshVertex{
					{along, across, along * across},
					{0.0f, 1.0f, 0.0f},
					{along, across},
				}
			);
		}
		// One triangle per three vertices, which keeps the index count
		// proportional to the vertex count and the runs a realistic shape.
		data.Indices.reserve(vertices);
		for (size_t index = 0; index + 2 < vertices; index += 3) {
			data.Indices.push_back(static_cast<uint32_t>(index));
			data.Indices.push_back(static_cast<uint32_t>(index + 1));
			data.Indices.push_back(static_cast<uint32_t>(index + 2));
		}
		data.ComputeBounds();

		built.push_back(Built{vertices, std::move(data)});
		return built.back().Data;
	}

	// `count` distinct names, interned once. `Name` interning is
	// `engine.core.bench.names`' subject and would otherwise show up in every
	// row here.
	const std::vector<Name> &Names(size_t count) {
		static std::vector<Name> names;
		while (names.size() < count) {
			names.push_back(Name("mesh." + std::to_string(names.size())));
		}
		return names;
	}

	// Fills a table with `count` meshes and flushes once, returning what it
	// holds.
	//
	// **Returns a count so a row can say `static const size_t ready = Fill(...)`
	// and have the setup run exactly once.** A `MeshTable` owns device buffers
	// and is neither copyable nor movable, so the usual build-and-return-a-local
	// idiom is not available to it; a function-local static filled by its
	// initialiser is.
	size_t Fill(MeshTable &table, size_t count) {
		const std::vector<Name> &names = Names(count);
		for (size_t index = 0; index < count; index++) {
			table.Add(names[index], Geometry(VERTICES));
		}
		table.Flush();
		return table.Count();
	}

	// Three sizes in rotation, so that replacing any of them leaves a freed run
	// the next request does not fit.
	size_t SizeOf(size_t index) {
		return 64 + index % 3 * 192;
	}

	// Fills a table with meshes of mixed size, which is what fragments a free
	// list.
	size_t FillMixed(MeshTable &table, size_t count) {
		const std::vector<Name> &names = Names(count);
		for (size_t index = 0; index < count; index++) {
			table.Add(names[index], Geometry(SizeOf(index)));
		}
		table.Flush();
		return table.Count();
	}
}

using namespace mesh_bench;

// --- filling an empty table ---------------------------------------------------

BENCH("Add · 1000 meshes into an empty table, one flush", 1) {
	// The level load. One flush at the end, because a burst of arrivals is
	// supposed to cost one transfer rather than one each - which is what
	// `UploadCount` exists to assert and what this row prices.
	MeshTable table;
	const std::vector<Name> &names = Names(MESHES);
	for (size_t index = 0; index < MESHES; index++) {
		Consume(table.Add(names[index], Geometry(VERTICES)));
	}
	table.Flush();
	Consume(table.Count());
}

BENCH("Add · 1000 meshes into an empty table, flushed after each", 1) {
	// The same thousand meshes admitted the wrong way: one flush per arrival,
	// which is what `Renderer::AddMesh` did before v0.11.
	//
	// **It measures the same as the row above, and that is the useful result.**
	// With no device attached a flush advances the generation counter and
	// returns, so what this row proves is that none of the batching win is
	// host-side: every byte of it is the transfer. Which means `UploadCount` is
	// the thing to assert about batching - a CPU profile would show nothing at
	// all, and the regression that put this back would be invisible to one.
	MeshTable table;
	const std::vector<Name> &names = Names(MESHES);
	for (size_t index = 0; index < MESHES; index++) {
		Consume(table.Add(names[index], Geometry(VERTICES)));
		table.Flush();
	}
	Consume(table.Count());
}

BENCH("Add · 10,000 meshes into an empty table, one flush", 1) {
	// **Whether admission degrades with what is already there**, asked the only
	// way that gives a stable answer: ten times the meshes into a table built
	// from nothing, so every sample measures the same thing. Divide by ten and
	// compare against the thousand-mesh row - equal is a claim that costs the
	// same at any table size, and materially worse is a walk that grew with the
	// table.
	//
	// A growing table measured across samples was the obvious way to write this
	// and is the wrong one: the minimum sample would be the small table and the
	// spread would be the growth, which reports a stable figure for a cost that
	// is not stable.
	MeshTable table;
	const std::vector<Name> &names = Names(10'000);
	for (size_t index = 0; index < 10'000; index++) {
		Consume(table.Add(names[index], Geometry(VERTICES)));
	}
	table.Flush();
	Consume(table.Count());
}

BENCH("Add · 1000 32-vertex meshes into an empty table, one flush", 1) {
	// The same question with an eighth of the bytes.
	//
	// **Two curves rather than one, because there are two candidate causes and
	// a single curve cannot tell them apart.** Admitting a mesh copies its
	// vertices into a shared host buffer *and* does per-mesh bookkeeping - a
	// free-list claim, a dirty span, a map insert. If the pair of small-mesh
	// rows climbs as steeply as the pair above, the cost is the bookkeeping and
	// is a real algorithmic problem; if it climbs far less, the cost is the
	// bytes and the answer is a reserve rather than a rewrite.
	//
	// **As measured it is the bytes.** Ten times the small meshes costs about
	// ten times as much, so the per-mesh bookkeeping is flat in the table size;
	// ten times the 256-vertex meshes costs far more than ten times, and eighty
	// megabytes of host vertices grown by `resize` is enough to account for it -
	// each growth zeroes the new tail and then copies, and the copies add up to
	// several times the buffer. A table told up front how large it will get
	// would not pay that, and nothing tells it today.
	MeshTable table;
	const std::vector<Name> &names = Names(MESHES);
	for (size_t index = 0; index < MESHES; index++) {
		Consume(table.Add(names[index], Geometry(32)));
	}
	table.Flush();
	Consume(table.Count());
}

BENCH("Add · 10,000 32-vertex meshes into an empty table, one flush", 1) {
	MeshTable table;
	const std::vector<Name> &names = Names(10'000);
	for (size_t index = 0; index < 10'000; index++) {
		Consume(table.Add(names[index], Geometry(32)));
	}
	table.Flush();
	Consume(table.Count());
}

// --- reclaiming ---------------------------------------------------------------

BENCH("Add · 1000 names replaced in place, flushed between", 1) {
	// **What a script that rebuilds a mesh every frame costs.** Replacing a
	// name hands its old run back, and the run becomes reusable once enough
	// flushes have passed that no frame in flight can still be reading it. Done
	// right the table holds steady; done wrong it grows by a mesh per rewrite
	// until an admission is refused, and nothing reports that until it happens.
	static MeshTable table;
	static const size_t ready = Fill(table, MESHES);
	Consume(ready);
	const std::vector<Name> &names = Names(MESHES);
	for (size_t index = 0; index < MESHES; index++) {
		Consume(table.Add(names[index], Geometry(VERTICES)));
		table.Flush();
	}
	Consume(table.FreeVertexCount());
}

BENCH("Add · 1000 names replaced in place with no flush at all", 1) {
	// The same rewrites with the table's clock stopped. Nothing ages, so
	// nothing is reusable and every replacement takes fresh host slots - the
	// failure mode the header names in one sentence: a caller that stops
	// flushing stops reclaiming.
	//
	// It is here as the *other* end of the row above rather than as a case
	// anybody should hit. Read the two together: the difference is what the
	// deferred free list buys.
	//
	// Expect a wide spread. Each sample leaves the host buffers larger than the
	// last because nothing was ever reclaimed, so the samples are not measuring
	// the same table - which is the point, and is why this is the one row here
	// whose minimum is the honest figure and whose maximum is the warning.
	static MeshTable table;
	static const size_t ready = Fill(table, MESHES);
	Consume(ready);
	const std::vector<Name> &names = Names(MESHES);
	for (size_t index = 0; index < MESHES; index++) {
		Consume(table.Add(names[index], Geometry(VERTICES)));
	}
	Consume(table.HostVertexCount());
}

BENCH("Add · 1000 replacements over a fragmented free list", 1) {
	// **Fragmentation, which is the failure the two rows above cannot show.**
	// Meshes of three different sizes replaced in a rotation leave freed runs
	// that do not fit the next request, so the claim has to walk. A free list
	// that is searched linearly turns a load into something quadratic in the
	// number of rewrites, and it does it silently: every mesh still arrives and
	// every frame still draws.
	static MeshTable table;
	static const size_t ready = FillMixed(table, MESHES);
	Consume(ready);

	// Each name is rewritten at the *next* size in the rotation, so the run it
	// gives back is never the size of the run it now needs.
	static size_t shift = 1;
	const std::vector<Name> &names = Names(MESHES);
	for (size_t index = 0; index < MESHES; index++) {
		Consume(table.Add(names[index], Geometry(SizeOf(index + shift))));
		table.Flush();
	}
	shift++;
	Consume(table.FreeVertexCount());
}

// --- what a frame does --------------------------------------------------------

BENCH("Resolve · 100k lookups by name in a 10,000 mesh table", 100'000) {
	// Once per drawn instance per frame, which at a hundred thousand instances
	// is a hundred thousand times at the display's rate rather than the tick's.
	static MeshTable table;
	static const size_t ready = Fill(table, 10'000);
	Consume(ready);
	const std::vector<Name> &names = Names(10'000);
	size_t indices = 0;
	for (size_t lookup = 0; lookup < 100'000; lookup++) {
		indices += table.Resolve(names[lookup % 10'000]).Whole.IndexCount;
	}
	Consume(indices);
}

BENCH("Flush · 100k quiet frames", 100'000) {
	// **The table's clock, and it ticks whether or not anything changed.** A
	// game that is not loading anything calls this every frame and it must cost
	// nothing, because a counter that only moved when something was sent would
	// leave a table nobody is writing to unable to ever reuse a run.
	static MeshTable table;
	static const size_t ready = Fill(table, 10'000);
	Consume(ready);
	for (size_t frame = 0; frame < 100'000; frame++) {
		Consume(table.Flush());
	}
	Consume(table.Count());
}
