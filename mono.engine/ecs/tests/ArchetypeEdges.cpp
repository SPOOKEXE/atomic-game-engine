// Reaches this module's src/ rather than its include/, for the reason
// `tests/Archetype.cpp` gives: the edge cache is part of the storage layout, and
// publishing it to make it testable is what AGENTS.md says not to do.
#include "ArchetypeEdges.hpp"

#include <engine/ecs/ChangeChannel.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.ecs.archetypeedges")

using engine::ecs::ArchetypeEdges;
using engine::ecs::ComponentId;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::Store;

namespace archetypeedges_test {
	struct Place {
		float X = 0.0f;
	};
	struct Speed {
		float X = 0.0f;
	};
	struct Flag {
		uint32_t Value = 0;
	};

	// Non-trivial, so a row that moves between tables through a cached edge has
	// to do the same real work an uncached one does.
	struct Label {
		std::string Text;
	};
}

using namespace archetypeedges_test;

// --- the cache on its own --------------------------------------------------

TEST_CASE("an unrecorded edge is absent rather than zero", "[ecs]") {
	ArchetypeEdges edges;
	const ComponentId flag = Components::Of<Flag>();

	// Table zero is the first table any world creates, so "no edge" and "the
	// edge leads to table zero" have to be different answers.
	REQUIRE(edges.Added(0, 0, flag) == ArchetypeEdges::NO_TABLE);
	REQUIRE(edges.Removed(0, 0, flag) == ArchetypeEdges::NO_TABLE);

	edges.RecordAddition(0, 0, flag, 0);
	REQUIRE(edges.Added(0, 0, flag) == 0);
}

TEST_CASE("adding and removing are separate directions", "[ecs]") {
	ArchetypeEdges edges;
	const ComponentId flag = Components::Of<Flag>();

	edges.RecordAddition(0, 1, flag, 2);

	REQUIRE(edges.Added(0, 1, flag) == 2);
	REQUIRE(edges.Removed(0, 1, flag) == ArchetypeEdges::NO_TABLE);

	edges.RecordRemoval(0, 2, flag, 1);
	REQUIRE(edges.Removed(0, 2, flag) == 1);
}

TEST_CASE("an edge belongs to one table and one component", "[ecs]") {
	ArchetypeEdges edges;
	const ComponentId flag = Components::Of<Flag>();
	const ComponentId speed = Components::Of<Speed>();

	edges.RecordAddition(0, 1, flag, 2);

	REQUIRE(edges.Added(0, 1, speed) == ArchetypeEdges::NO_TABLE);
	REQUIRE(edges.Added(0, 3, flag) == ArchetypeEdges::NO_TABLE);
}

TEST_CASE("a moved epoch wipes what was recorded under the old one", "[ecs]") {
	ArchetypeEdges edges;
	const ComponentId flag = Components::Of<Flag>();

	edges.RecordAddition(0, 1, flag, 2);
	REQUIRE(edges.Count() == 1);

	// Not a stale answer - a wrong one. Observing a component changes which
	// tables carry DirtyBits, so the destination recorded before it is a table
	// that no longer tracks changes.
	REQUIRE(edges.Added(1, 1, flag) == ArchetypeEdges::NO_TABLE);
	REQUIRE(edges.Count() == 0);
}

TEST_CASE("forgetting drops every table's edges", "[ecs]") {
	ArchetypeEdges edges;
	edges.RecordAddition(0, 0, Components::Of<Flag>(), 1);
	edges.RecordRemoval(0, 1, Components::Of<Flag>(), 0);
	REQUIRE(edges.Count() == 2);

	edges.Forget();
	REQUIRE(edges.Count() == 0);
}

// --- the cache under a store -----------------------------------------------
//
// The cache is not observable from `Store`, and should not be: what a suite can
// check is that a world behaves identically whether a transition is the first of
// its kind or the thousandth. A wrong edge would show as a row in the wrong
// table, which is a missing component or an unreported change.

TEST_CASE("a repeated transition lands where the first one did", "[ecs]") {
	Store store("edges");

	std::vector<Entity> entities;
	for (int index = 0; index < 64; index++) {
		const Entity entity = store.Create();
		store.Set<Place>(entity, Place{static_cast<float>(index)});
		store.Set<Speed>(entity, Speed{1.0f});
		entities.push_back(entity);
	}

	// Every one of these after the first is a cache hit.
	for (const Entity entity : entities) {
		store.Set<Flag>(entity, Flag{7});
	}

	for (const Entity entity : entities) {
		REQUIRE(store.Has<Flag>(entity));
		REQUIRE(store.Get<Flag>(entity)->Value == 7);
		REQUIRE(store.Has<Place>(entity));
		REQUIRE(store.Has<Speed>(entity));
	}
	REQUIRE(store.CountMatching<Place, Speed, Flag>() == entities.size());
}

TEST_CASE("a repeated removal lands where the first one did", "[ecs]") {
	Store store("edges");

	std::vector<Entity> entities;
	for (int index = 0; index < 64; index++) {
		const Entity entity = store.Create();
		store.Set<Place>(entity, Place{static_cast<float>(index)});
		store.Set<Flag>(entity, Flag{1});
		entities.push_back(entity);
	}

	for (const Entity entity : entities) {
		store.Remove<Flag>(entity);
	}

	for (const Entity entity : entities) {
		REQUIRE_FALSE(store.Has<Flag>(entity));
		REQUIRE(store.Has<Place>(entity));
	}
	REQUIRE(store.CountMatching<Place>() == entities.size());
	REQUIRE(store.CountMatching<Flag>() == 0);
}

TEST_CASE("toggling many times keeps the values it carried", "[ecs]") {
	Store store("edges");

	const Entity entity = store.Create();
	store.Set<Place>(entity, Place{3.0f});
	store.Set<Label>(entity, Label{"kept"});

	for (int pass = 0; pass < 32; pass++) {
		store.Set<Flag>(entity, Flag{static_cast<uint32_t>(pass)});
		REQUIRE(store.Get<Flag>(entity)->Value == static_cast<uint32_t>(pass));
		store.Remove<Flag>(entity);
	}

	// The components that were only ever along for the ride came through every
	// move intact - including the one that is not trivially copyable.
	REQUIRE(store.Get<Place>(entity)->X == 3.0f);
	REQUIRE(store.Get<Label>(entity)->Text == "kept");
}

// --- the invalidation that matters -----------------------------------------
//
// **This is the case the cache could get silently wrong.** Observing a component
// gives its tables a DirtyBits column, so the table a transition should reach
// changes. An edge recorded before the `Observe` names a table with no bits, and
// a row sent there would be written without anything recording the write - a
// change that never fires, rather than a crash.

TEST_CASE("observing after a transition invalidates the edge", "[ecs]") {
	Store store("edges");

	const Entity first = store.Create();
	store.Set<Place>(first, Place{1.0f});

	// Records the Place -> Place+Flag edge, with no change tracking anywhere.
	store.Set<Flag>(first, Flag{1});
	store.Remove<Flag>(first);

	store.Observe<Flag>();

	// Same transition, but the destination now has to carry DirtyBits. Taking
	// the recorded edge would land it in the untracked table.
	store.Set<Flag>(first, Flag{2});

	REQUIRE(store.Changed<Flag>(first));

	std::vector<Entity> reported;
	store.EachChanged<Flag>([&](Entity entity, Flag &) { reported.push_back(entity); });
	REQUIRE(reported.size() == 1);
	REQUIRE(reported[0].Id == first.Id);
}

// A removal edge, by contrast, **cannot** go stale - and the reason is worth
// writing down, because "invalidate both directions" looks like the obviously
// symmetric thing to test and one half of it would be a test that cannot fail.
//
// A removal's destination set is a subset of its source set. So if observing a
// component changes the destination, that component is in the source too, and
// `ObserveRaw` migrates every table holding it - which gives the source a new
// table index. The old index is not merely stale, it is unreachable: `TableFor`
// now resolves that set through `Tracked` to the new table, so nothing ever
// enters the old one again and no edge hanging off it is ever consulted.
//
// The epoch still wipes removals along with additions. Not because this case
// needs it, but because a cache that invalidated only the direction somebody had
// worked out the argument for is one refactor away from being wrong.
TEST_CASE("removals still behave after a late observe", "[ecs]") {
	Store store("edges");

	const Entity entity = store.Create();
	store.Set<Place>(entity, Place{1.0f});
	store.Set<Speed>(entity, Speed{2.0f});

	// Records Place+Speed -> Place, before anything is observed.
	store.Remove<Speed>(entity);
	store.Set<Speed>(entity, Speed{2.0f});

	store.Observe<Place>();
	store.ClearChanges();

	store.Remove<Speed>(entity);
	store.Set<Place>(entity, Place{9.0f});

	REQUIRE(store.Changed<Place>(entity));
	REQUIRE_FALSE(store.Has<Speed>(entity));
	REQUIRE(store.Get<Place>(entity)->X == 9.0f);
}

TEST_CASE("clearing a world drops edges that named its tables", "[ecs]") {
	Store store("edges");

	const Entity before = store.Create();
	store.Set<Place>(before, Place{1.0f});
	store.Set<Flag>(before, Flag{1});

	store.Clear();

	// A surviving edge would send this row into a table index that belonged to
	// the world that has just been thrown away.
	const Entity after = store.Create();
	store.Set<Place>(after, Place{2.0f});
	store.Set<Flag>(after, Flag{2});

	REQUIRE(store.Has<Place>(after));
	REQUIRE(store.Has<Flag>(after));
	REQUIRE(store.Get<Flag>(after)->Value == 2);
	REQUIRE(store.CountMatching<Place, Flag>() == 1);
}
