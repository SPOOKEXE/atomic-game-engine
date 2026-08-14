#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.ecs.store")

using Catch::Approx;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;

namespace store_test {
	struct Position {
		float X = 0.0f;
		float Y = 0.0f;
	};

	struct Velocity {
		float X = 0.0f;
		float Y = 0.0f;
	};

	struct Tag {
		int Value = 0;
	};
}

using namespace store_test;

TEST_CASE("a fresh store has no entities of ours", "[ecs]") {
	Store store("test");

	const Entity entity = store.Create();
	REQUIRE(store.Alive(entity));
	REQUIRE(entity);
}

TEST_CASE("the null entity is never alive", "[ecs]") {
	Store store("test");

	REQUIRE_FALSE(store.Alive(NULL_ENTITY));
	REQUIRE_FALSE(static_cast<bool>(NULL_ENTITY));
}

TEST_CASE("a destroyed entity stops being alive", "[ecs]") {
	Store store("test");

	const Entity entity = store.Create();
	store.Destroy(entity);

	REQUIRE_FALSE(store.Alive(entity));
}

TEST_CASE("a component round-trips", "[ecs]") {
	Store store("test");

	const Entity entity = store.Create();
	store.Set<Position>(entity, Position{3.0f, 4.0f});

	REQUIRE(store.Has<Position>(entity));

	const Position *position = store.Get<Position>(entity);
	REQUIRE(position != nullptr);
	REQUIRE(position->X == Approx(3.0f));
	REQUIRE(position->Y == Approx(4.0f));
}

TEST_CASE("an absent component reads as null rather than a default", "[ecs]") {
	Store store("test");

	const Entity entity = store.Create();
	REQUIRE_FALSE(store.Has<Position>(entity));
	REQUIRE(store.Get<Position>(entity) == nullptr);
}

TEST_CASE("a mutable component can be written through", "[ecs]") {
	Store store("test");

	const Entity entity = store.Create();
	store.Set<Position>(entity, Position{1.0f, 1.0f});

	store.GetMutable<Position>(entity)->X = 9.0f;

	REQUIRE(store.Get<Position>(entity)->X == Approx(9.0f));
}

TEST_CASE("removing a component leaves the entity", "[ecs]") {
	Store store("test");

	const Entity entity = store.Create();
	store.Set<Position>(entity, Position{});
	store.Set<Tag>(entity, Tag{7});

	store.Remove<Position>(entity);

	REQUIRE_FALSE(store.Has<Position>(entity));
	REQUIRE(store.Has<Tag>(entity));
	REQUIRE(store.Alive(entity));
}

TEST_CASE("Each visits only entities carrying every component named", "[ecs]") {
	Store store("test");

	const Entity both = store.Create();
	store.Set<Position>(both, Position{0.0f, 0.0f});
	store.Set<Velocity>(both, Velocity{1.0f, 2.0f});

	const Entity positionOnly = store.Create();
	store.Set<Position>(positionOnly, Position{5.0f, 5.0f});

	const Entity velocityOnly = store.Create();
	store.Set<Velocity>(velocityOnly, Velocity{9.0f, 9.0f});

	std::vector<Entity> visited;
	store.Each<Position, Velocity>([&](Entity entity, Position &, Velocity &) { visited.push_back(entity); });

	REQUIRE(visited.size() == 1);
	REQUIRE(visited[0] == both);
}

TEST_CASE("Each writes through to the store", "[ecs]") {
	Store store("test");

	for (int index = 0; index < 100; index++) {
		const Entity entity = store.Create();
		store.Set<Position>(entity, Position{0.0f, 0.0f});
		store.Set<Velocity>(entity, Velocity{1.0f, 0.0f});
	}

	constexpr float STEP = 0.5f;
	store.Each<Position, Velocity>([&](Entity, Position &position, Velocity &velocity) {
		position.X += velocity.X * STEP;
	});

	size_t moved = 0;
	store.Each<Position>([&](Entity, Position &position) {
		if (position.X == Approx(0.5f)) {
			moved++;
		}
	});

	REQUIRE(moved == 100);
}

TEST_CASE("EachBatch visits the same entities Each does", "[ecs]") {
	Store store("test");

	const Entity both = store.Create();
	store.Set<Position>(both, Position{1.0f, 2.0f});
	store.Set<Velocity>(both, Velocity{3.0f, 4.0f});

	const Entity positionOnly = store.Create();
	store.Set<Position>(positionOnly, Position{5.0f, 5.0f});

	// The batched form is an iteration strategy, not a different query. Anything
	// it matches differently from Each is a bug in one of the two.
	size_t seen = 0;
	store.EachBatch<Position, Velocity>([&](size_t rows, Position *positions, Velocity *velocities) {
		for (size_t row = 0; row < rows; row++) {
			REQUIRE(positions[row].X == Approx(1.0f));
			REQUIRE(velocities[row].Y == Approx(4.0f));
		}
		seen += rows;
	});

	REQUIRE(seen == store.CountMatching<Position, Velocity>());
	REQUIRE(seen == 1);
}

TEST_CASE("EachBatch covers every row exactly once", "[ecs]") {
	Store store("test");

	constexpr int COUNT = 500;
	for (int index = 0; index < COUNT; index++) {
		const Entity entity = store.Create();
		store.Set<Tag>(entity, Tag{index});
	}

	// Batches may divide any way the storage likes. What must hold is that
	// their concatenation is the whole set, with nothing seen twice - which is
	// the only promise a caller writing into a packed array can rely on.
	std::vector<int> found;
	store.EachBatch<Tag>([&](size_t rows, Tag *tags) {
		REQUIRE(rows > 0);
		for (size_t row = 0; row < rows; row++) {
			found.push_back(tags[row].Value);
		}
	});

	std::sort(found.begin(), found.end());
	REQUIRE(found.size() == COUNT);
	REQUIRE(std::adjacent_find(found.begin(), found.end()) == found.end());
	REQUIRE(found.front() == 0);
	REQUIRE(found.back() == COUNT - 1);
}

TEST_CASE("EachBatch writes through to the store", "[ecs]") {
	Store store("test");

	for (int index = 0; index < 100; index++) {
		const Entity entity = store.Create();
		store.Set<Position>(entity, Position{0.0f, 0.0f});
		store.Set<Velocity>(entity, Velocity{1.0f, 0.0f});
	}

	// The pointers are into the live tables, so a write through one is a write
	// to the store. That is the point of handing them out.
	constexpr float STEP = 0.5f;
	store.EachBatch<Position, Velocity>([&](size_t rows, Position *positions, Velocity *velocities) {
		for (size_t row = 0; row < rows; row++) {
			positions[row].X += velocities[row].X * STEP;
		}
	});

	size_t moved = 0;
	store.Each<Position>([&](Entity, Position &position) {
		if (position.X == Approx(0.5f)) {
			moved++;
		}
	});

	REQUIRE(moved == 100);
}

TEST_CASE("EachBatch spans more than one table", "[ecs]") {
	Store store("test");

	// Two archetypes: Tag alone, and Tag with Position. A batch is a run of
	// rows in one table, so this cannot be delivered as a single call - and a
	// caller must not assume it was.
	for (int index = 0; index < 10; index++) {
		const Entity entity = store.Create();
		store.Set<Tag>(entity, Tag{index});
		if (index % 2 == 0) {
			store.Set<Position>(entity, Position{});
		}
	}

	size_t batches = 0;
	size_t rowsSeen = 0;
	store.EachBatch<Tag>([&](size_t rows, Tag *) {
		batches++;
		rowsSeen += rows;
	});

	REQUIRE(rowsSeen == 10);
	REQUIRE(batches > 1);
}

TEST_CASE("EachBatch on nothing calls nothing", "[ecs]") {
	Store store("test");

	// Not one call with a zero row count. A body handed a count is entitled to
	// assume there is something at the pointer it came with.
	size_t calls = 0;
	store.EachBatch<Position>([&](size_t, Position *) { calls++; });

	REQUIRE(calls == 0);
}

TEST_CASE("EachBatch hands out the store's own arrays", "[ecs]") {
	Store store("test");

	for (int index = 0; index < 16; index++) {
		const Entity entity = store.Create();
		store.Set<Tag>(entity, Tag{index});
	}

	// Contiguity is the whole reason this exists: the body is meant to be a
	// loop over an array, not a walk of something that only looks like one.
	store.EachBatch<Tag>([&](size_t rows, Tag *tags) {
		for (size_t row = 1; row < rows; row++) {
			REQUIRE(&tags[row] == &tags[row - 1] + 1);
		}
	});
}

TEST_CASE("destroying during iteration is deferred, not a crash", "[ecs]") {
	Store store("test");

	std::vector<Entity> created;
	for (int index = 0; index < 50; index++) {
		const Entity entity = store.Create();
		store.Set<Tag>(entity, Tag{index});
		created.push_back(entity);
	}

	// A structural change from inside a loop is the standard way to corrupt an
	// archetype store. It has to be deferred to the end of the iteration.
	store.Each<Tag>([&](Entity entity, Tag &tag) {
		if (tag.Value % 2 == 0) {
			store.Destroy(entity);
		}
	});

	size_t alive = 0;
	for (const Entity entity : created) {
		if (store.Alive(entity)) {
			alive++;
		}
	}

	REQUIRE(alive == 25);
}

TEST_CASE("CountMatching counts the entities a query would visit", "[ecs]") {
	Store store("test");

	for (int index = 0; index < 8; index++) {
		const Entity entity = store.Create();
		store.Set<Position>(entity, Position{});
		if (index < 3) {
			store.Set<Velocity>(entity, Velocity{});
		}
	}

	REQUIRE(store.CountMatching<Position>() == 8);
	REQUIRE(store.CountMatching<Position, Velocity>() == 3);
}

TEST_CASE("a named entity keeps its name", "[ecs]") {
	Store store("test");

	const Entity entity = store.Create("camera");
	REQUIRE(store.Alive(entity));
	REQUIRE(store.NameOf(entity) == "camera");
	REQUIRE(store.Find("camera") == entity);
}

TEST_CASE("a store rebound to another thread accepts that thread", "[ecs]") {
	Store store("test");

	// Constructing on the loader thread and ticking on a worker is the
	// ordinary case for a world, not a violation.
	bool succeeded = false;
	std::thread worker([&] {
		store.BindToCallingThread();
		const Entity entity = store.Create();
		store.Set<Tag>(entity, Tag{1});
		succeeded = store.Has<Tag>(entity);
	});
	worker.join();

	REQUIRE(succeeded);
	REQUIRE_FALSE(store.IsOnOwningThread());
}

TEST_CASE("a store survives being rebound every tick", "[ecs]") {
	Store store("test");

	// What a world does once it is a range in a job batch: a different worker
	// picks it up each tick, so the handoff is the common path rather than a
	// setup step. Each thread must see its own bind, and the entity count must
	// be exactly the number of ticks - a lost handoff shows up as an abort, a
	// torn read as a miscount.
	constexpr int TICKS = 64;

	for (int tick = 0; tick < TICKS; tick++) {
		bool ownedHere = false;
		std::thread worker([&] {
			store.BindToCallingThread();
			ownedHere = store.IsOnOwningThread();
			store.Set<Tag>(store.Create(), Tag{tick});
		});
		worker.join();

		REQUIRE(ownedHere);
		REQUIRE_FALSE(store.IsOnOwningThread());
	}

	store.BindToCallingThread();
	REQUIRE(store.CountMatching<Tag>() == TICKS);
}

TEST_CASE("every live entity can be walked, components or not", "[ecs]") {
	// The primitive an interest filter wants. A query cannot express "every
	// entity" - a query is defined by the components it names - and an entity
	// carrying none is in no table and is still an entity.
	Store store("walk");

	const Entity bare = store.Create();
	const Entity carrying = store.Create();
	store.Set<Position>(carrying, Position{1.0f, 2.0f});

	const Entity gone = store.Create();
	store.Destroy(gone);

	std::vector<uint64_t> found;
	store.EachEntity([&found](Entity entity) { found.push_back(entity.Id); });

	REQUIRE(found.size() == 2);
	REQUIRE(std::find(found.begin(), found.end(), bare.Id) != found.end());
	REQUIRE(std::find(found.begin(), found.end(), carrying.Id) != found.end());
	REQUIRE(std::find(found.begin(), found.end(), gone.Id) == found.end());
}

TEST_CASE("walking every entity is deterministic and index-ordered", "[ecs]") {
	// Two runs of the same world must visit the same entities in the same
	// sequence, or nothing built on this can be compared between runs.
	const auto walk = [] {
		Store store("walk");
		std::vector<uint64_t> order;
		for (int index = 0; index < 64; index++) {
			store.Create();
		}
		store.EachEntity([&order](Entity entity) { order.push_back(entity.Id); });
		return order;
	};

	const std::vector<uint64_t> first = walk();
	const std::vector<uint64_t> second = walk();

	REQUIRE(first == second);
	REQUIRE(std::is_sorted(first.begin(), first.end()));
}

TEST_CASE("creating inside a walk is deferred, not immediate", "[ecs]") {
	Store store("walk");
	store.Create();
	store.Create();

	size_t visited = 0;
	store.EachEntity([&store, &visited](Entity) {
		visited++;
		store.Create();
	});

	// The two that existed, and not the two the body made.
	REQUIRE(visited == 2);

	size_t after = 0;
	store.EachEntity([&after](Entity) { after++; });
	REQUIRE(after == 4);
}
