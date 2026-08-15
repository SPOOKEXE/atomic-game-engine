// The query builder: what it matches, what it refuses, and the plan behind it.
//
// `Store::Each<Ts...>` answers "every entity carrying all of these" and could
// say nothing else, so a system that wanted "and not a wall" said it as a branch
// inside the body - over rows the loop should never have visited, once per row
// per tick. `Query` adds the two terms that were missing, and both are answered
// per archetype.
//
// The last case here is a regression rather than a feature: building the filter
// meant reading the plan cache, and the cache was keying two term orders to one
// plan while binding columns in the order of whichever ran first.

#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

TEST_SUITE_ID("engine.ecs.query")

using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::Store;

namespace query_test {
	struct Position {
		float X = 0.0f;
	};
	struct Velocity {
		float X = 0.0f;
	};
	struct Solid {};
	struct Anchored {};
	struct Hidden {};

	// Entities in visit order, so a case can assert on the set and on the fact
	// that two runs agree about it.
	std::vector<Entity> Visited(auto &&selection) {
		std::vector<Entity> seen;
		selection.Each([&seen](Entity entity, Position &) { seen.push_back(entity); });
		return seen;
	}
}

using namespace query_test;

TEST_CASE("a query with no filters is Each", "[ecs][query]") {
	Store store("query.plain");

	const Entity first = store.Create();
	const Entity second = store.Create();
	store.Set(first, Position{1.0f});
	store.Set(second, Position{2.0f});

	std::vector<Entity> direct;
	store.Each<Position>([&direct](Entity entity, Position &) { direct.push_back(entity); });

	auto selection = store.Query<Position>();
	const std::vector<Entity> built = Visited(selection);

	CHECK(built == direct);
	CHECK(built.size() == 2);
}

TEST_CASE("With matches a component without handing it over", "[ecs][query]") {
	// The half that is easy to overlook: `Solid` is a tag, so naming it in the
	// bound list would force the body to take a parameter it cannot read.
	Store store("query.with");

	const Entity solid = store.Create();
	store.Set(solid, Position{1.0f});
	store.Set(solid, Solid{});

	const Entity bare = store.Create();
	store.Set(bare, Position{2.0f});

	auto selection = store.Query<Position>().With<Solid>();
	const std::vector<Entity> seen = Visited(selection);

	REQUIRE(seen.size() == 1);
	CHECK(seen[0] == solid);
}

TEST_CASE("Without refuses an entity carrying the term", "[ecs][query]") {
	Store store("query.without");

	const Entity moving = store.Create();
	store.Set(moving, Position{1.0f});

	const Entity wall = store.Create();
	store.Set(wall, Position{2.0f});
	store.Set(wall, Anchored{});

	auto selection = store.Query<Position>().Without<Anchored>();
	const std::vector<Entity> seen = Visited(selection);

	REQUIRE(seen.size() == 1);
	CHECK(seen[0] == moving);
}

TEST_CASE("With and Without compose, and each may name several", "[ecs][query]") {
	Store store("query.both");

	const Entity wanted = store.Create();
	store.Set(wanted, Position{1.0f});
	store.Set(wanted, Velocity{});
	store.Set(wanted, Solid{});

	// Carries the required pair and an excluded term with it.
	const Entity hidden = store.Create();
	store.Set(hidden, Position{2.0f});
	store.Set(hidden, Velocity{});
	store.Set(hidden, Solid{});
	store.Set(hidden, Hidden{});

	// Missing one of the required pair.
	const Entity partial = store.Create();
	store.Set(partial, Position{3.0f});
	store.Set(partial, Solid{});

	auto selection = store.Query<Position>().With<Solid, Velocity>().Without<Anchored, Hidden>();
	const std::vector<Entity> seen = Visited(selection);

	REQUIRE(seen.size() == 1);
	CHECK(seen[0] == wanted);
}

TEST_CASE("a filter that excludes everything visits nothing", "[ecs][query]") {
	Store store("query.empty");

	const Entity wall = store.Create();
	store.Set(wall, Position{1.0f});
	store.Set(wall, Anchored{});

	auto selection = store.Query<Position>().Without<Anchored>();
	CHECK(Visited(selection).empty());
	CHECK(store.Query<Position>().Without<Anchored>().Count() == 0);
}

TEST_CASE("Count agrees with what Each visits", "[ecs][query]") {
	Store store("query.count");

	for (int index = 0; index < 5; index++) {
		const Entity entity = store.Create();
		store.Set(entity, Position{static_cast<float>(index)});
		if (index % 2 == 0) {
			store.Set(entity, Anchored{});
		}
	}

	auto selection = store.Query<Position>().Without<Anchored>();
	CHECK(store.Query<Position>().Without<Anchored>().Count() == Visited(selection).size());
	CHECK(store.Query<Position>().Without<Anchored>().Count() == 2);
}

TEST_CASE("a table created after the plan is filtered too", "[ecs][query]") {
	// The plan is topped up rather than rebuilt, so a filter has to be applied
	// to the tables that arrive later as well as the ones that were there. A
	// top-up that only tested the bound terms would let a wall in through the
	// back door.
	Store store("query.later");

	const Entity moving = store.Create();
	store.Set(moving, Position{1.0f});

	CHECK(store.Query<Position>().Without<Anchored>().Count() == 1);

	const Entity wall = store.Create();
	store.Set(wall, Position{2.0f});
	store.Set(wall, Anchored{});

	CHECK(store.Query<Position>().Without<Anchored>().Count() == 1);
	CHECK(store.Query<Position>().Count() == 2);
}

TEST_CASE("the batch and parallel paths take the same filter", "[ecs][query]") {
	// One filter, four ways of walking what it matched. A path that resolved
	// its own terms would be a second definition of the query.
	Store store("query.paths");

	for (int index = 0; index < 4; index++) {
		const Entity entity = store.Create();
		store.Set(entity, Position{1.0f});
		if (index >= 2) {
			store.Set(entity, Anchored{});
		}
	}

	size_t batched = 0;
	store.Query<Position>().Without<Anchored>().EachBatch([&batched](size_t rows, Position *) {
		batched += rows;
	});
	CHECK(batched == 2);

	size_t walked = 0;
	store.Query<Position>().Without<Anchored>().EachParallel([&walked](Entity, Position &) { walked++; });
	CHECK(walked == 2);

	const size_t visited =
		store.Query<Position>().Without<Anchored>().EachBatchParallel([](size_t, size_t, Position *) {});
	CHECK(visited == 2);
}

TEST_CASE("two term orders over one component set bind their own columns", "[ecs][query]") {
	// **A regression, and it was silent.** The plan cache keyed on the *sorted*
	// term ids so that `Each<A, B>` and `Each<B, A>` would share one plan - but
	// a plan's column positions are recorded in the caller's term order, so the
	// second of those two got the first's bindings and read an `A` where it had
	// asked for a `B`. The types line up, the loop runs, and the numbers belong
	// to somebody else.
	Store store("query.order");

	const Entity entity = store.Create();
	store.Set(entity, Position{11.0f});
	store.Set(entity, Velocity{22.0f});

	float position = 0.0f;
	float velocity = 0.0f;
	store.Each<Position, Velocity>([&](Entity, Position &first, Velocity &second) {
		position = first.X;
		velocity = second.X;
	});
	CHECK(position == 11.0f);
	CHECK(velocity == 22.0f);

	// The same set, named the other way round. Whichever of the two runs first,
	// both have to be right.
	float swappedPosition = 0.0f;
	float swappedVelocity = 0.0f;
	store.Each<Velocity, Position>([&](Entity, Velocity &first, Position &second) {
		swappedVelocity = first.X;
		swappedPosition = second.X;
	});
	CHECK(swappedPosition == 11.0f);
	CHECK(swappedVelocity == 22.0f);
}

TEST_CASE("a filter and a bare query over one set do not share a plan", "[ecs][query]") {
	// The other half of the key: `Query<Position>()` and
	// `Query<Position>().Without<Anchored>()` name the same bound term, so a key
	// built from the bound terms alone would hand the second the first's
	// matches.
	Store store("query.keys");

	const Entity moving = store.Create();
	store.Set(moving, Position{1.0f});

	const Entity wall = store.Create();
	store.Set(wall, Position{2.0f});
	store.Set(wall, Anchored{});

	CHECK(store.Query<Position>().Count() == 2);
	CHECK(store.Query<Position>().Without<Anchored>().Count() == 1);
	CHECK(store.Query<Position>().With<Anchored>().Count() == 1);

	// And asking again in the other order still answers the same, which is what
	// says the two plans are two rather than one that was overwritten.
	CHECK(store.Query<Position>().Count() == 2);
	CHECK(store.Query<Position>().Without<Anchored>().Count() == 1);
}

TEST_CASE("naming a filter term twice is the same filter", "[ecs][query]") {
	// The sets are unioned and their order is not read, so two spellings of one
	// filter share a plan rather than building two.
	Store store("query.repeat");

	const Entity moving = store.Create();
	store.Set(moving, Position{1.0f});

	const Entity wall = store.Create();
	store.Set(wall, Position{2.0f});
	store.Set(wall, Anchored{});
	store.Set(wall, Hidden{});

	CHECK(store.Query<Position>().Without<Anchored, Hidden>().Count() == 1);
	CHECK(store.Query<Position>().Without<Hidden>().Without<Anchored>().Count() == 1);
}
