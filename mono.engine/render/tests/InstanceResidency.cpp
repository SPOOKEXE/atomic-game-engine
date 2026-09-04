#include <engine/core/Name.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <InstanceResidency.hpp>
#include <vector>

TEST_SUITE_ID("engine.render.instanceresidency")

using engine::core::Name;
using engine::render::GpuInstance;
using engine::render::InstanceKey;
using engine::render::InstanceResidency;
using engine::render::InstanceUploadRange;
using engine::render::MeshEntry;
using engine::render::ToGpu;
using engine::scene::DrawInstance;

namespace {
	GpuInstance Row(float x) {
		GpuInstance row;
		row.Position.x = x;
		return row;
	}

	InstanceKey Key(uint64_t source, uint64_t variant = 0) {
		return InstanceKey{Name("residency.world"), source, variant, 0};
	}
}

TEST_CASE("an exact source row reuses its packed resident slot", "[render][residency]") {
	InstanceResidency rows;
	DrawInstance source;
	source.Source = 1;
	MeshEntry mesh;

	rows.BeginFrame();
	const uint32_t original = rows.Upsert(Key(1), ToGpu(source, mesh), source, mesh);
	rows.EndFrame();
	rows.AcknowledgeDirty();

	rows.BeginFrame();
	uint32_t reused = 0;
	CHECK(rows.Reuse(Key(1), source, mesh, reused));
	rows.EndFrame();
	CHECK(reused == original);
	CHECK(rows.DirtyCount() == 0);
}

TEST_CASE("probing an absent source does not touch another resident row", "[render][residency]") {
	InstanceResidency rows;
	DrawInstance source;
	MeshEntry mesh;

	rows.BeginFrame();
	rows.Upsert(Key(1), ToGpu(source, mesh), source, mesh);
	rows.EndFrame();
	rows.AcknowledgeDirty();

	rows.BeginFrame();
	uint32_t slot = 0;
	CHECK_FALSE(rows.Probe(Key(2), source, mesh, slot));
	rows.EndFrame();
	CHECK(rows.LiveCount() == 0);
}

TEST_CASE("source and mesh packing inputs invalidate exact resident reuse", "[render][residency]") {
	InstanceResidency rows;
	DrawInstance source;
	source.Source = 1;
	MeshEntry mesh;

	rows.BeginFrame();
	rows.Upsert(Key(1), ToGpu(source, mesh), source, mesh);
	rows.EndFrame();
	rows.AcknowledgeDirty();

	rows.BeginFrame();
	uint32_t slot = 0;
	source.Frame.Position.X = 4.0f;
	CHECK_FALSE(rows.Reuse(Key(1), source, mesh, slot));
	rows.Upsert(Key(1), ToGpu(source, mesh), source, mesh);
	rows.EndFrame();
	CHECK(rows.DirtyCount() == 1);
	rows.AcknowledgeDirty();

	rows.BeginFrame();
	mesh.Extent.X = 2.0f;
	CHECK_FALSE(rows.Reuse(Key(1), source, mesh, slot));
	rows.Upsert(Key(1), ToGpu(source, mesh), source, mesh);
	rows.EndFrame();
	CHECK(rows.DirtyCount() == 1);
}

TEST_CASE("a retained slot updates without changing resident identity", "[render][residency]") {
	InstanceResidency rows;
	DrawInstance source;
	source.Source = 1;
	MeshEntry mesh;

	rows.BeginFrame();
	const uint32_t slot = rows.Upsert(Key(1), ToGpu(source, mesh), source, mesh);
	rows.EndFrame();
	rows.AcknowledgeDirty();

	rows.BeginFrame();
	source.Frame.Position.X = 3.0f;
	CHECK_FALSE(rows.ProbeSlot(slot, Key(1), source, mesh));
	CHECK(rows.UpsertSlot(slot, Key(1), ToGpu(source, mesh), source, mesh) == slot);
	rows.EndFrame();
	CHECK(rows.LiveCount() == 1);
	CHECK(rows.DirtyCount() == 1);
	CHECK(rows.Row(slot).Position.x == 3.0f);
}

TEST_CASE("residency records mesh rows and staged deltas separately", "[render][residency]") {
	InstanceResidency rows;
	DrawInstance firstSource;
	firstSource.Source = 1;
	firstSource.Mesh = Name("content/first.amesh");
	DrawInstance secondSource;
	secondSource.Source = 2;
	secondSource.Mesh = Name("content/second.amesh");
	MeshEntry mesh;

	rows.BeginFrame();
	rows.Upsert(Key(1), ToGpu(firstSource, mesh), firstSource, mesh);
	rows.Upsert(Key(2), ToGpu(secondSource, mesh), secondSource, mesh);
	rows.EndFrame();
	rows.AcknowledgeDirty();

	const std::vector<engine::render::AssetInstanceRows> staged = rows.AssetRows();
	REQUIRE(staged.size() == 2);
	for (const engine::render::AssetInstanceRows &entry : staged) {
		CHECK(entry.Resident == 1);
		CHECK(entry.Staged == 1);
	}

	rows.BeginFrame();
	rows.Upsert(Key(1), ToGpu(firstSource, mesh), firstSource, mesh);
	rows.Upsert(Key(2), ToGpu(secondSource, mesh), secondSource, mesh);
	rows.EndFrame();
	rows.AcknowledgeDirty();

	const std::vector<engine::render::AssetInstanceRows> steady = rows.AssetRows();
	REQUIRE(steady.size() == 2);
	for (const engine::render::AssetInstanceRows &entry : steady) {
		CHECK(entry.Resident == 1);
		CHECK(entry.Staged == 0);
	}
}

TEST_CASE("a stale retained slot falls back to stable-key lookup", "[render][residency]") {
	InstanceResidency rows;
	DrawInstance source;
	MeshEntry mesh;

	rows.BeginFrame();
	const uint32_t first = rows.Upsert(Key(1), ToGpu(source, mesh), source, mesh);
	const uint32_t second = rows.Upsert(Key(2), ToGpu(source, mesh), source, mesh);
	rows.EndFrame();
	rows.AcknowledgeDirty();

	rows.BeginFrame();
	CHECK_FALSE(rows.ProbeSlot(first, Key(2), source, mesh));
	CHECK(rows.UpsertSlot(first, Key(2), ToGpu(source, mesh), source, mesh) == second);
	rows.EndFrame();
	CHECK(rows.LiveCount() == 1);
}

TEST_CASE("metadata changes reuse a resident GPU row", "[render][residency]") {
	InstanceResidency rows;
	DrawInstance source;
	source.Source = 1;
	MeshEntry mesh;

	rows.BeginFrame();
	const uint32_t original = rows.Upsert(Key(1), ToGpu(source, mesh), source, mesh);
	rows.EndFrame();
	rows.AcknowledgeDirty();

	rows.BeginFrame();
	uint32_t slot = 0;
	source.Texture = Name("changed.texture");
	CHECK(rows.Reuse(Key(1), source, mesh, slot));
	rows.EndFrame();
	CHECK(rows.DirtyCount() == 0);
	CHECK(slot == original);
}

TEST_CASE("reordering keeps resident slots and only changes indices", "[render][residency]") {
	InstanceResidency rows;
	rows.BeginFrame();
	const uint32_t first = rows.Upsert(Key(1), Row(1.0f));
	const uint32_t second = rows.Upsert(Key(2), Row(2.0f));
	rows.EndFrame();
	REQUIRE(rows.DirtyCount() == 2);
	REQUIRE(rows.PackedRows().size() == rows.SlotCount());
	CHECK(&rows.Row(first) == rows.PackedRows().data() + first);
	CHECK(&rows.Row(second) == rows.PackedRows().data() + second);

	rows.BeginFrame();
	CHECK(rows.Upsert(Key(2), Row(2.0f)) == second);
	CHECK(rows.Upsert(Key(1), Row(1.0f)) == first);
	rows.EndFrame();
	CHECK(rows.DirtyCount() == 0);
}

TEST_CASE("one changed entity dirties one resident row", "[render][residency]") {
	InstanceResidency rows;
	rows.BeginFrame();
	const uint32_t first = rows.Upsert(Key(1), Row(1.0f));
	const uint32_t second = rows.Upsert(Key(2), Row(2.0f));
	rows.EndFrame();

	rows.BeginFrame();
	CHECK(rows.Upsert(Key(1), Row(1.0f)) == first);
	CHECK(rows.Upsert(Key(2), Row(3.0f)) == second);
	rows.EndFrame();
	REQUIRE(rows.DirtyCount() == 1);
	const std::span<const InstanceUploadRange> ranges = rows.DirtyRanges();
	REQUIRE(ranges.size() == 1);
	CHECK(ranges[0].First == second);
	CHECK(ranges[0].Count == 1);
}

TEST_CASE("membership edits do not shift surviving rows", "[render][residency]") {
	InstanceResidency rows;
	rows.BeginFrame();
	const uint32_t first = rows.Upsert(Key(1), Row(1.0f));
	const uint32_t removed = rows.Upsert(Key(2), Row(2.0f));
	const uint32_t last = rows.Upsert(Key(3), Row(3.0f));
	rows.EndFrame();

	rows.BeginFrame();
	CHECK(rows.Upsert(Key(3), Row(3.0f)) == last);
	CHECK(rows.Upsert(Key(1), Row(1.0f)) == first);
	rows.EndFrame();
	CHECK(rows.LiveCount() == 2);
	CHECK(rows.DirtyCount() == 0);

	rows.BeginFrame();
	CHECK(rows.Upsert(Key(1), Row(1.0f)) == first);
	CHECK(rows.Upsert(Key(3), Row(3.0f)) == last);
	CHECK(rows.Upsert(Key(4), Row(4.0f)) == removed);
	rows.EndFrame();
	CHECK(rows.DirtyCount() == 1);
}

TEST_CASE("world and synthetic variant are part of row identity", "[render][residency]") {
	InstanceResidency rows;
	rows.BeginFrame();
	const uint32_t original = rows.Upsert(Key(1), Row(1.0f));
	const uint32_t portalHalf = rows.Upsert(Key(1, 7), Row(2.0f));
	const uint32_t otherWorld = rows.Upsert(InstanceKey{Name("residency.other"), 1, 0, 0}, Row(3.0f));
	rows.EndFrame();

	CHECK(original != portalHalf);
	CHECK(original != otherWorld);
	CHECK(portalHalf != otherWorld);
}

TEST_CASE("marking all resident rows dirty rebuilds a replaced device buffer", "[render][residency]") {
	InstanceResidency rows;
	rows.BeginFrame();
	rows.Upsert(Key(1), Row(1.0f));
	rows.Upsert(Key(2), Row(2.0f));
	rows.EndFrame();

	rows.BeginFrame();
	rows.Upsert(Key(1), Row(1.0f));
	rows.Upsert(Key(2), Row(2.0f));
	rows.MarkAllDirty();
	rows.EndFrame();
	CHECK(rows.DirtyCount() == 2);
}

TEST_CASE("cameras in one renderer frame share dirty rows and retain their union", "[render][residency]") {
	InstanceResidency rows;
	rows.BeginFrame(1);
	const uint32_t first = rows.Upsert(Key(1), Row(1.0f));
	REQUIRE(rows.DirtyCount() == 1);
	rows.AcknowledgeDirty();

	// A second camera in the same frame sees the first row and contributes a
	// second one. It does not reopen the residency frame or redirty the first.
	rows.BeginFrame(1);
	CHECK(rows.Upsert(Key(1), Row(1.0f)) == first);
	const uint32_t second = rows.Upsert(Key(2), Row(2.0f));
	CHECK(rows.DirtyCount() == 1);
	rows.AcknowledgeDirty();

	rows.BeginFrame(2);
	CHECK(rows.Upsert(Key(2), Row(2.0f)) == second);
	CHECK(rows.Upsert(Key(1), Row(1.0f)) == first);
	CHECK(rows.LiveCount() == 2);
	CHECK(rows.DirtyCount() == 0);
}

TEST_CASE("token residency retires rows after the whole camera batch omits them", "[render][residency]") {
	InstanceResidency rows;
	rows.BeginFrame(1);
	rows.Upsert(Key(1), Row(1.0f));
	rows.Upsert(Key(2), Row(2.0f));
	rows.AcknowledgeDirty();

	rows.BeginFrame(2);
	rows.Upsert(Key(1), Row(1.0f));
	CHECK(rows.LiveCount() == 2);

	// Retirement happens when the next renderer frame proves the row was absent
	// from every camera in frame two.
	rows.BeginFrame(3);
	CHECK(rows.LiveCount() == 1);
	CHECK(rows.PackedRows().size() == 1);
}
