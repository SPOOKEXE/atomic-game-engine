#include <engine/core/Name.hpp>
#include <engine/scene/SurfaceTable.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.surfacetable")

using engine::core::Name;
using engine::scene::SurfaceProperties;
using engine::scene::SurfaceTable;

TEST_CASE("a registered material resolves to what was set", "[scene][surfacetable]") {
	const Name ice("surfacetable_test.Ice");

	SurfaceTable table;
	table.Set(ice, SurfaceProperties{0.02f, 0.1f});

	const SurfaceProperties *found = table.Find(ice);
	REQUIRE(found != nullptr);
	CHECK(found->Friction == 0.02f);
	CHECK(found->Restitution == 0.1f);
}

TEST_CASE("an unregistered material resolves to nothing, not a default", "[scene][surfacetable]") {
	// The whole reason there is no get-or-default. A mistyped material name
	// that silently behaves like concrete is a bug that reads as working code
	// for a month.
	SurfaceTable table;
	table.Set(Name("surfacetable_test.Concrete"), SurfaceProperties{0.8f, 0.0f});

	CHECK(table.Find(Name("surfacetable_test.Conrete")) == nullptr);
	CHECK(table.Count() == 1);
}

TEST_CASE("an invalid name never matches a row", "[scene][surfacetable]") {
	// A `Surface` that nobody set carries an invalid name. If that matched
	// anything, "no material" and "the first material" would be the same
	// value.
	SurfaceTable table;
	table.Set(Name(), SurfaceProperties{9.0f, 9.0f});

	CHECK(table.Find(Name()) == nullptr);
}

TEST_CASE("setting a material twice replaces its row", "[scene][surfacetable]") {
	// Appending instead would leave a table holding a history, where `Find`
	// returns whichever copy it reached first - so a reload would keep the old
	// values and look like the new ones had not been applied.
	const Name rubber("surfacetable_test.Rubber");

	SurfaceTable table;
	table.Set(rubber, SurfaceProperties{1.0f, 0.6f});
	table.Set(rubber, SurfaceProperties{1.2f, 0.9f});

	REQUIRE(table.Count() == 1);
	const SurfaceProperties *found = table.Find(rubber);
	REQUIRE(found != nullptr);
	CHECK(found->Restitution == 0.9f);
}

TEST_CASE("rows keep insertion order", "[scene][surfacetable]") {
	// Insertion order is program order, which is what makes two runs of one
	// scene hold an identical table and a snapshot of it byte-identical. A hash
	// map would give that up for a lookup nobody has measured.
	const Name first("surfacetable_test.Wood");
	const Name second("surfacetable_test.Metal");
	const Name third("surfacetable_test.Glass");

	SurfaceTable table;
	table.Set(first, SurfaceProperties{0.4f, 0.0f});
	table.Set(second, SurfaceProperties{0.3f, 0.1f});
	table.Set(third, SurfaceProperties{0.2f, 0.2f});

	// Replacing an existing row must not move it to the end either, or the
	// order would depend on how often a scene rewrote a material.
	table.Set(first, SurfaceProperties{0.45f, 0.0f});

	REQUIRE(table.Rows.size() == 3);
	CHECK(table.Rows[0].Material == first);
	CHECK(table.Rows[1].Material == second);
	CHECK(table.Rows[2].Material == third);
	CHECK(table.Rows[0].Properties.Friction == 0.45f);
}
