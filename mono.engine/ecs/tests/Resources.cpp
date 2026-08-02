// World-scoped state: resources, and the clock that is one.
//
// The property most of these are about is separation. A resource is reachable
// by name and unreachable by query, and a component is the other way round. If
// that ever stops being true the failure is silent — a loop quietly gains a row
// that is not an entity — so it is asserted rather than documented.

#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.ecs.resources")
TEST_DEPENDS("engine.ecs.store")

using Catch::Approx;
using engine::ecs::Entity;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::ecs::WorldTime;

namespace resources_test {
	struct Gravity {
		float Metres = 9.81f;
	};

	struct Score {
		int Points = 0;
	};

	// Deliberately usable as both, which is the case that has to stay safe.
	struct Position {
		float X = 0.0f;
	};

	struct DrawList {
		std::vector<int> Items;
	};
}

using namespace resources_test;

TEST_CASE("a resource is null until it is set", "[resources]") {
	Store store("test");

	REQUIRE_FALSE(store.HasResource<Gravity>());
	REQUIRE(store.Resource<Gravity>() == nullptr);

	store.SetResource(Gravity{1.62f});

	REQUIRE(store.HasResource<Gravity>());
	REQUIRE(store.Resource<Gravity>()->Metres == Approx(1.62f));
}

TEST_CASE("setting a resource twice replaces it rather than adding one", "[resources]") {
	Store store("test");

	store.SetResource(Score{1});
	store.SetResource(Score{2});

	REQUIRE(store.Resource<Score>()->Points == 2);
}

TEST_CASE("a resource is mutable in place", "[resources]") {
	Store store("test");
	store.SetResource(Score{10});

	store.ResourceMutable<Score>()->Points += 5;

	REQUIRE(store.Resource<Score>()->Points == 15);
}

TEST_CASE("a removed resource is gone", "[resources]") {
	Store store("test");
	store.SetResource(Gravity{});
	store.RemoveResource<Gravity>();

	REQUIRE_FALSE(store.HasResource<Gravity>());
	REQUIRE(store.Resource<Gravity>() == nullptr);
}

TEST_CASE("a resource holding a container keeps its capacity", "[resources]") {
	Store store("test");
	store.SetResource(DrawList{});

	auto *list = store.ResourceMutable<DrawList>();
	list->Items.reserve(256);
	const auto reserved = list->Items.capacity();

	list->Items.push_back(1);
	list->Items.clear();

	// Clearing and refilling is what the render's draw list does every frame.
	// If the resource were copied in and out on each access, the capacity would
	// not survive and a steady scene would allocate forever.
	REQUIRE(store.Resource<DrawList>()->Items.capacity() == reserved);
}

// --- the separation from components ---------------------------------------

TEST_CASE("a resource is not visited by a query for the same type", "[resources]") {
	Store store("test");

	const Entity entity = store.Create();
	store.Set<Position>(entity, Position{1.0f});
	store.SetResource(Position{99.0f});

	int visited = 0;
	float sum = 0.0f;
	store.Each<const Position>([&](Entity, const Position &position) {
		visited++;
		sum += position.X;
	});

	// One entity has a Position. The world also has one, and it is not an
	// entity — so a loop over positions sees exactly the entity's.
	REQUIRE(visited == 1);
	REQUIRE(sum == Approx(1.0f));
}

TEST_CASE("a resource is not counted as an entity", "[resources]") {
	Store store("test");

	for (int index = 0; index < 5; index++) {
		store.Set<Position>(store.Create(), Position{});
	}
	store.SetResource(Position{});
	store.SetResource(Gravity{});

	REQUIRE(store.CountMatching<Position>() == 5);
}

TEST_CASE("a cached count query is a live view, not a snapshot", "[resources]") {
	Store store("test");

	REQUIRE(store.CountMatching<Position>() == 0);

	for (int index = 0; index < 3; index++) {
		store.Set<Position>(store.Create(), Position{});
	}

	// The query is built on the first call and kept. Entities created after it
	// still have to be counted, or a system calling this every tick would read
	// whatever the world looked like the first time anybody asked.
	REQUIRE(store.CountMatching<Position>() == 3);
}

TEST_CASE("a resource survives entities being created around it", "[resources]") {
	Store store("test");
	store.SetResource(Score{7});

	for (int index = 0; index < 64; index++) {
		store.Set<Position>(store.Create(), Position{});
	}

	REQUIRE(store.Resource<Score>()->Points == 7);
}

// --- the clock -------------------------------------------------------------

TEST_CASE("a new world has a clock at zero", "[resources][time]") {
	Store store("test");

	REQUIRE(store.Time().Tick == 0);
	REQUIRE(store.Time().Elapsed == Approx(0.0));
	REQUIRE(store.Time().Alpha == Approx(0.0f));
}

TEST_CASE("advancing a tick moves elapsed and the tick count", "[resources][time]") {
	Store store("test");

	store.AdvanceTick(0.25f);
	store.AdvanceTick(0.25f);

	REQUIRE(store.Time().Tick == 2);
	REQUIRE(store.Time().Elapsed == Approx(0.5));
	REQUIRE(store.Time().Delta == Approx(0.25f));
}

TEST_CASE("the frame delta and the tick delta are separate fields", "[resources][time]") {
	Store store("test");

	store.AdvanceTick(1.0f / 60.0f);
	store.SetFrame(1.0f / 240.0f, 0.5f);

	// The whole reason the clock is a resource rather than a parameter: a
	// system asking for the tick delta cannot be handed the frame's, because
	// they are not the same question.
	REQUIRE(store.Time().Delta == Approx(1.0f / 60.0f));
	REQUIRE(store.Time().FrameDelta == Approx(1.0f / 240.0f));
	REQUIRE(store.Time().Alpha == Approx(0.5f));
}

TEST_CASE("setting a frame does not advance the simulation", "[resources][time]") {
	Store store("test");
	store.AdvanceTick(0.1f);

	store.SetFrame(0.004f, 0.9f);
	store.SetFrame(0.004f, 0.95f);

	REQUIRE(store.Time().Tick == 1);
	REQUIRE(store.Time().Elapsed == Approx(0.1));
}

TEST_CASE("a system reads the clock out of the world it is handed", "[resources][time]") {
	Store store("test");
	Scheduler scheduler;

	double seenElapsed = 0.0;
	float seenDelta = 0.0f;
	scheduler.Add("read-clock", Phase::Simulation, [&](Store &tickStore) {
		seenElapsed = tickStore.Time().Elapsed;
		seenDelta = tickStore.Time().Delta;
	});

	scheduler.Tick(store, 0.5f);

	// The tick's clock, not the previous one's: Tick advances time before it
	// runs anything, so a system sees the step it is simulating.
	REQUIRE(seenElapsed == Approx(0.5));
	REQUIRE(seenDelta == Approx(0.5f));
}
