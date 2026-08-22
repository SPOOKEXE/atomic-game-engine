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

TEST_CASE("reordering keeps resident slots and only changes indices", "[render][residency]") {
	InstanceResidency rows;
	rows.BeginFrame();
	const uint32_t first = rows.Upsert(Key(1), Row(1.0f));
	const uint32_t second = rows.Upsert(Key(2), Row(2.0f));
	rows.EndFrame();
	REQUIRE(rows.DirtyCount() == 2);

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
}
