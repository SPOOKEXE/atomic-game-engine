#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <vector>

TEST_SUITE_ID("engine.ecs.store")

using Catch::Approx;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;

namespace {
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
	store.Set<Position>(entity, Position { 3.0f, 4.0f });

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
	store.Set<Position>(entity, Position { 1.0f, 1.0f });

	store.GetMutable<Position>(entity)->X = 9.0f;

	REQUIRE(store.Get<Position>(entity)->X == Approx(9.0f));
}

TEST_CASE("removing a component leaves the entity", "[ecs]") {
	Store store("test");

	const Entity entity = store.Create();
	store.Set<Position>(entity, Position {});
	store.Set<Tag>(entity, Tag { 7 });

	store.Remove<Position>(entity);

	REQUIRE_FALSE(store.Has<Position>(entity));
	REQUIRE(store.Has<Tag>(entity));
	REQUIRE(store.Alive(entity));
}

TEST_CASE("Each visits only entities carrying every component named", "[ecs]") {
	Store store("test");

	const Entity both = store.Create();
	store.Set<Position>(both, Position { 0.0f, 0.0f });
	store.Set<Velocity>(both, Velocity { 1.0f, 2.0f });

	const Entity positionOnly = store.Create();
	store.Set<Position>(positionOnly, Position { 5.0f, 5.0f });

	const Entity velocityOnly = store.Create();
	store.Set<Velocity>(velocityOnly, Velocity { 9.0f, 9.0f });

	std::vector<Entity> visited;
	store.Each<Position, Velocity>([&](Entity entity, Position &, Velocity &) {
		visited.push_back(entity);
	});

	REQUIRE(visited.size() == 1);
	REQUIRE(visited[0] == both);
}

TEST_CASE("Each writes through to the store", "[ecs]") {
	Store store("test");

	for (int index = 0; index < 100; index++) {
		const Entity entity = store.Create();
		store.Set<Position>(entity, Position { 0.0f, 0.0f });
		store.Set<Velocity>(entity, Velocity { 1.0f, 0.0f });
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

TEST_CASE("destroying during iteration is deferred, not a crash", "[ecs]") {
	Store store("test");

	std::vector<Entity> created;
	for (int index = 0; index < 50; index++) {
		const Entity entity = store.Create();
		store.Set<Tag>(entity, Tag { index });
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
		store.Set<Position>(entity, Position {});
		if (index < 3) {
			store.Set<Velocity>(entity, Velocity {});
		}
	}

	REQUIRE(store.CountMatching<Position>() == 8);
	REQUIRE(store.CountMatching<Position, Velocity>() == 3);
}

TEST_CASE("a named entity keeps its name", "[ecs]") {
	Store store("test");

	const Entity entity = store.Create("camera");
	REQUIRE(store.Alive(entity));
	REQUIRE(std::string(store.Native().entity(entity.Id).name()) == "camera");
}

TEST_CASE("a store rebound to another thread accepts that thread", "[ecs]") {
	Store store("test");

	// Constructing on the loader thread and ticking on a worker is the
	// ordinary case for a world, not a violation.
	bool succeeded = false;
	std::thread worker([&] {
		store.BindToCallingThread();
		const Entity entity = store.Create();
		store.Set<Tag>(entity, Tag { 1 });
		succeeded = store.Has<Tag>(entity);
	});
	worker.join();

	REQUIRE(succeeded);
	REQUIRE_FALSE(store.IsOnOwningThread());
}
