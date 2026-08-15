// Change signals, and where they are allowed to fire.
//
// The whole design is one decision: a callback runs at a phase boundary rather
// than at the moment of assignment. Every case below is either that decision or
// something that follows from it.

#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.ecs.signals")

using engine::ecs::Entity;
using engine::ecs::Store;

namespace signals_test {
	struct Health {
		int Value = 0;
	};
	struct Shield {
		int Value = 0;
	};
	struct Marker {
		int Value = 0;
	};
}

using namespace signals_test;

TEST_CASE("a signal fires at the boundary, not at the write", "[ecs]") {
	Store store("signals");

	std::vector<Entity> heard;
	store.OnChanged<Health>([&heard](Store &, Entity entity, const Health &) { heard.push_back(entity); });

	const Entity entity = store.Create();
	store.Set<Health>(entity, Health{10});

	// Nothing yet. This is the property the whole design exists for: a script
	// mutating the world from inside a loop over it is what a mid-batch
	// callback allows.
	REQUIRE(heard.empty());

	REQUIRE(store.FlushSignals() == 1);
	REQUIRE(heard.size() == 1);
	REQUIRE(heard[0] == entity);
}

TEST_CASE("three writes in one tick signal once, with the value it ended at", "[ecs]") {
	Store store("signals");

	std::vector<int> values;
	store.OnChanged<Health>([&values](Store &, Entity, const Health &health) {
		values.push_back(health.Value);
	});

	const Entity entity = store.Create();
	store.Set<Health>(entity, Health{1});
	store.Set<Health>(entity, Health{2});
	store.Set<Health>(entity, Health{3});

	REQUIRE(store.FlushSignals() == 1);
	REQUIRE(values == std::vector<int>{3});
}

TEST_CASE("a listener sees the current value, not one captured at the write", "[ecs]") {
	// Which is what makes the coalescing above meaningful rather than a lie:
	// the one signal reports the world as it is at the boundary.
	Store store("signals");

	int seen = 0;
	store.OnChanged<Health>([&seen](Store &, Entity, const Health &health) { seen = health.Value; });

	const Entity entity = store.Create();
	store.Set<Health>(entity, Health{1});
	store.GetMutable<Health>(entity)->Value = 42;

	store.FlushSignals();
	REQUIRE(seen == 42);
}

TEST_CASE("a flush with nothing changed fires nothing", "[ecs]") {
	Store store("signals");

	size_t calls = 0;
	store.OnChanged<Health>([&calls](Store &, Entity, const Health &) { calls++; });

	const Entity entity = store.Create();
	store.Set<Health>(entity, Health{1});
	REQUIRE(store.FlushSignals() == 1);

	// Bits are not cleared by firing - a replication delta and a render
	// invalidation read the same bits, and one consumer clearing them out from
	// under another is the bug that shape avoids. So this fires again until
	// something clears.
	REQUIRE(store.FlushSignals() == 1);

	store.ClearChanges();
	REQUIRE(store.FlushSignals() == 0);
	REQUIRE(calls == 2);
}

TEST_CASE("listening to a type observes it, so a signal is never silent", "[ecs]") {
	// A signal on a type nothing records would never fire, and the silence
	// would look like a bug in the listener rather than a missing `Observe`.
	Store store("signals");
	REQUIRE_FALSE(store.Observed<Shield>());

	store.OnChanged<Shield>([](Store &, Entity, const Shield &) {});
	REQUIRE(store.Observed<Shield>());
}

TEST_CASE("several listeners on one type all hear it", "[ecs]") {
	Store store("signals");

	int first = 0;
	int second = 0;
	store.OnChanged<Health>([&first](Store &, Entity, const Health &) { first++; });
	store.OnChanged<Health>([&second](Store &, Entity, const Health &) { second++; });

	store.Set<Health>(store.Create(), Health{1});
	REQUIRE(store.FlushSignals() == 2);
	REQUIRE(first == 1);
	REQUIRE(second == 1);
}

TEST_CASE("listeners on different types each hear only their own", "[ecs]") {
	Store store("signals");

	std::vector<std::string> heard;
	store.OnChanged<Health>([&heard](Store &, Entity, const Health &) { heard.emplace_back("health"); });
	store.OnChanged<Shield>([&heard](Store &, Entity, const Shield &) { heard.emplace_back("shield"); });

	const Entity entity = store.Create();
	store.Set<Health>(entity, Health{1});

	store.FlushSignals();
	REQUIRE(heard == std::vector<std::string>{"health"});
}

TEST_CASE("a disconnected listener stops hearing", "[ecs]") {
	Store store("signals");

	size_t calls = 0;
	const auto connection = store.OnChanged<Health>([&calls](Store &, Entity, const Health &) { calls++; });
	REQUIRE(store.Listeners() == 1);

	const Entity entity = store.Create();
	store.Set<Health>(entity, Health{1});
	store.FlushSignals();
	REQUIRE(calls == 1);

	REQUIRE(store.Disconnect(connection));
	REQUIRE(store.Listeners() == 0);

	store.Set<Health>(entity, Health{2});
	REQUIRE(store.FlushSignals() == 0);
	REQUIRE(calls == 1);

	// Twice is not an error, but it is not a success either.
	REQUIRE_FALSE(store.Disconnect(connection));
	REQUIRE_FALSE(store.Disconnect({}));
}

TEST_CASE("a listener that writes does not fire itself", "[ecs]") {
	// The obvious infinite loop, and the reason the flush refuses to re-enter.
	// The write belongs to the next boundary.
	Store store("signals");

	size_t calls = 0;
	store.OnChanged<Health>([&calls](Store &store, Entity entity, const Health &health) {
		calls++;
		if (health.Value < 100) {
			store.GetMutable<Health>(entity)->Value = health.Value + 1;
		}
	});

	const Entity entity = store.Create();
	store.Set<Health>(entity, Health{1});

	REQUIRE(store.FlushSignals() == 1);
	REQUIRE(calls == 1);
	REQUIRE(store.Get<Health>(entity)->Value == 2);

	// And the next boundary picks it up, once.
	REQUIRE(store.FlushSignals() == 1);
	REQUIRE(calls == 2);
}

TEST_CASE("a listener may add a component without corrupting the walk", "[ecs]") {
	// A structural change moves rows between tables. The changed set is
	// collected before anything fires precisely so an iteration is not still
	// walking a table that no longer holds what it thought.
	Store store("signals");

	// Registered first, which is not incidental: listening observes the type,
	// and observing after the fact records nothing about writes that already
	// happened. `Observe` says to declare it when the world is built and this
	// is what that means in practice.
	size_t calls = 0;
	store.OnChanged<Health>([&calls](Store &store, Entity entity, const Health &health) {
		calls++;
		if (health.Value % 2 == 0) {
			store.Set<Marker>(entity, Marker{health.Value});
		}
	});

	std::vector<Entity> entities;
	for (int index = 0; index < 64; index++) {
		const Entity entity = store.Create();
		store.Set<Health>(entity, Health{index});
		entities.push_back(entity);
	}

	REQUIRE(store.FlushSignals() == 64);
	REQUIRE(calls == 64);

	size_t marked = 0;
	store.Each<const Marker>([&marked](Entity, const Marker &) { marked++; });
	REQUIRE(marked == 32);
}

TEST_CASE("a listener may destroy an entity a later listener would have seen", "[ecs]") {
	// The value is re-fetched per listener rather than captured once, so the
	// second one is told there is nothing there instead of being handed a
	// pointer into a row that has moved.
	Store store("signals");

	const Entity first = store.Create();
	const Entity second = store.Create();

	size_t later = 0;
	store.OnChanged<Health>([second](Store &store, Entity, const Health &) {
		if (store.Alive(second)) {
			store.Destroy(second);
		}
	});
	store.OnChanged<Health>([&later](Store &, Entity, const Health &) { later++; });

	store.Set<Health>(first, Health{1});
	store.Set<Health>(second, Health{2});

	store.FlushSignals();

	// The destroyed entity's second listener was skipped; the survivor's ran.
	REQUIRE_FALSE(store.Alive(second));
	REQUIRE(later == 1);
}

TEST_CASE("a listener that removes the component it listens to is not called again", "[ecs]") {
	Store store("signals");

	size_t calls = 0;
	store.OnChanged<Health>([&calls](Store &store, Entity entity, const Health &) {
		calls++;
		store.Remove<Health>(entity);
	});
	store.OnChanged<Health>([&calls](Store &, Entity, const Health &) { calls++; });

	store.Set<Health>(store.Create(), Health{1});
	store.FlushSignals();

	REQUIRE(calls == 1);
}

TEST_CASE("a store with no listeners costs a flush nothing", "[ecs]") {
	Store store("signals");
	store.Observe<Health>();

	for (int index = 0; index < 100; index++) {
		store.Set<Health>(store.Create(), Health{index});
	}

	REQUIRE(store.FlushSignals() == 0);
	REQUIRE(store.Listeners() == 0);
}

TEST_CASE("signals do not survive a snapshot, and say so by being absent", "[ecs]") {
	// A snapshot carries state, never code. A restored world has storage and no
	// behaviour until somebody registers it again - the same rule a replayed
	// universe follows, and the reason `Replayer::Restore` demands a configure
	// callback.
	Store store("signals");
	store.OnChanged<Health>([](Store &, Entity, const Health &) {});
	REQUIRE(store.Listeners() == 1);

	engine::core::ByteWriter writer;
	REQUIRE(store.Save(writer));

	Store restored("restored");
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	REQUIRE(restored.Listeners() == 0);
}

TEST_CASE("observing a tag reports the change rather than crashing", "[ecs]") {
	// **A tag has no column, so the change machinery has no bytes to hand
	// over** - and the walk indexed the column directory anyway, which is null
	// for a component with no data. It segfaulted rather than reporting a
	// change with no value.
	//
	// Nothing observed a tag until a property named one among the components it
	// reads, which `scene::Anchored` does: `.Changed` on `Mass` has to fire when
	// a part is anchored, and anchored is a tag. The crash landed three modules
	// away in a JavaScript test, which is what an unchecked null in a shared
	// walk looks like from the outside.
	struct Flagged {};

	Store store("signals.tag");
	store.Observe<Flagged>();

	const Entity entity = store.Create();
	store.Set(entity, Flagged{});

	CHECK(store.Changed<Flagged>(entity));

	// And the boundary walk that reads those bits runs over it without
	// touching the column that is not there.
	store.FlushSignals();
	store.ClearChanges();

	CHECK_FALSE(store.Changed<Flagged>(entity));
}
