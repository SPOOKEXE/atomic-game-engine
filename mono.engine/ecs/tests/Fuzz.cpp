// The ECS checked against a model that is obviously correct.
//
// Every other suite in this module tests one type against what its header
// promises. This one tests the *whole* store against a `std::map` — slow,
// stupid, and impossible to get subtly wrong — by driving both with the same
// random operation stream and comparing them after every step.
//
// That is the test worth having for hand-written storage. An archetype graph
// with swap-back removal, a generational directory and a deferral queue has
// interactions no per-type suite reaches: an entity moved between tables by one
// operation while another operation holds its old row, a destroy that displaces
// the entity a later step is about to touch, a snapshot taken mid-churn. Those
// only show up when the operations are interleaved by something with no
// intuition about which orders are interesting.
//
// `core::Random` rather than a standard generator, so a failure reproduces from
// its seed on any machine — which is the whole reason that type exists.

#include <engine/core/Bytes.hpp>
#include <engine/core/Random.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.ecs.fuzz")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Random;
using engine::ecs::Entity;
using engine::ecs::Store;

namespace fuzz_test {
	// Eight distinct component types, so the archetype graph has 256 reachable
	// shapes rather than a handful. Templated on an index so they are genuinely
	// different types with the same layout — which is also the case that would
	// catch a registry keying on size instead of identity.
	template <int Index> struct Slot {
		int64_t Value = 0;
	};

	using C0 = Slot<0>;
	using C1 = Slot<1>;
	using C2 = Slot<2>;
	using C3 = Slot<3>;
	using C4 = Slot<4>;
	using C5 = Slot<5>;
	using C6 = Slot<6>;
	using C7 = Slot<7>;

	constexpr int COMPONENT_COUNT = 8;

	// What the store is supposed to contain: entity to component to value.
	//
	// An ordered map on purpose. Comparing two unordered containers means
	// sorting them anyway, and a failure message that lists entities in a
	// stable order is one somebody can read.
	using Model = std::map<uint64_t, std::map<int, int64_t>>;

	// The store operations, dispatched by index so the fuzz loop can pick one
	// by number and the same number always means the same thing.
	void SetSlot(Store &store, Entity entity, int slot, int64_t value) {
		switch (slot) {
		case 0:
			store.Set<C0>(entity, C0{value});
			break;
		case 1:
			store.Set<C1>(entity, C1{value});
			break;
		case 2:
			store.Set<C2>(entity, C2{value});
			break;
		case 3:
			store.Set<C3>(entity, C3{value});
			break;
		case 4:
			store.Set<C4>(entity, C4{value});
			break;
		case 5:
			store.Set<C5>(entity, C5{value});
			break;
		case 6:
			store.Set<C6>(entity, C6{value});
			break;
		default:
			store.Set<C7>(entity, C7{value});
			break;
		}
	}

	void RemoveSlot(Store &store, Entity entity, int slot) {
		switch (slot) {
		case 0:
			store.Remove<C0>(entity);
			break;
		case 1:
			store.Remove<C1>(entity);
			break;
		case 2:
			store.Remove<C2>(entity);
			break;
		case 3:
			store.Remove<C3>(entity);
			break;
		case 4:
			store.Remove<C4>(entity);
			break;
		case 5:
			store.Remove<C5>(entity);
			break;
		case 6:
			store.Remove<C6>(entity);
			break;
		default:
			store.Remove<C7>(entity);
			break;
		}
	}

	bool HasSlot(const Store &store, Entity entity, int slot) {
		switch (slot) {
		case 0:
			return store.Has<C0>(entity);
		case 1:
			return store.Has<C1>(entity);
		case 2:
			return store.Has<C2>(entity);
		case 3:
			return store.Has<C3>(entity);
		case 4:
			return store.Has<C4>(entity);
		case 5:
			return store.Has<C5>(entity);
		case 6:
			return store.Has<C6>(entity);
		default:
			return store.Has<C7>(entity);
		}
	}

	// The value, or a sentinel that no operation ever writes, so "absent" and
	// "present and zero" cannot be confused.
	constexpr int64_t MISSING = -999'999;

	int64_t GetSlot(const Store &store, Entity entity, int slot) {
		const auto read = [](const auto *value) { return value == nullptr ? MISSING : value->Value; };
		switch (slot) {
		case 0:
			return read(store.Get<C0>(entity));
		case 1:
			return read(store.Get<C1>(entity));
		case 2:
			return read(store.Get<C2>(entity));
		case 3:
			return read(store.Get<C3>(entity));
		case 4:
			return read(store.Get<C4>(entity));
		case 5:
			return read(store.Get<C5>(entity));
		case 6:
			return read(store.Get<C6>(entity));
		default:
			return read(store.Get<C7>(entity));
		}
	}

	// Every entity the store thinks carries `slot`, by iteration rather than by
	// lookup — so a query that disagrees with Has/Get is caught.
	std::map<uint64_t, int64_t> IterateSlot(Store &store, int slot) {
		std::map<uint64_t, int64_t> found;
		const auto collect = [&found](Entity entity, int64_t value) { found.emplace(entity.Id, value); };

		switch (slot) {
		case 0:
			store.Each<C0>([&](Entity e, C0 &c) { collect(e, c.Value); });
			break;
		case 1:
			store.Each<C1>([&](Entity e, C1 &c) { collect(e, c.Value); });
			break;
		case 2:
			store.Each<C2>([&](Entity e, C2 &c) { collect(e, c.Value); });
			break;
		case 3:
			store.Each<C3>([&](Entity e, C3 &c) { collect(e, c.Value); });
			break;
		case 4:
			store.Each<C4>([&](Entity e, C4 &c) { collect(e, c.Value); });
			break;
		case 5:
			store.Each<C5>([&](Entity e, C5 &c) { collect(e, c.Value); });
			break;
		case 6:
			store.Each<C6>([&](Entity e, C6 &c) { collect(e, c.Value); });
			break;
		default:
			store.Each<C7>([&](Entity e, C7 &c) { collect(e, c.Value); });
			break;
		}
		return found;
	}

	size_t CountSlot(Store &store, int slot) {
		switch (slot) {
		case 0:
			return store.CountMatching<C0>();
		case 1:
			return store.CountMatching<C1>();
		case 2:
			return store.CountMatching<C2>();
		case 3:
			return store.CountMatching<C3>();
		case 4:
			return store.CountMatching<C4>();
		case 5:
			return store.CountMatching<C5>();
		case 6:
			return store.CountMatching<C6>();
		default:
			return store.CountMatching<C7>();
		}
	}

	// The number of mismatches between the store and the model.
	//
	// Returned rather than asserted per entity, because a fuzz run that has
	// diverged usually diverges in thousands of places and a REQUIRE per row
	// buries the first one.
	struct Divergence {
		size_t Missing = 0; // model has it, store does not
		size_t Extra = 0;	// store has it, model does not
		size_t WrongValue = 0;
		size_t WrongCount = 0;
		size_t DeadAlive = 0; // store says a destroyed entity is alive

		size_t Total() const {
			return Missing + Extra + WrongValue + WrongCount + DeadAlive;
		}
	};

	Divergence Compare(Store &store, const Model &model, const std::vector<Entity> &retired) {
		Divergence divergence;

		// Every entity the model holds is alive, carries what the model says,
		// and carries nothing the model does not.
		for (const auto &[id, slots] : model) {
			const Entity entity{id};
			if (!store.Alive(entity)) {
				divergence.Missing++;
				continue;
			}

			for (int slot = 0; slot < COMPONENT_COUNT; slot++) {
				const auto expected = slots.find(slot);
				const bool shouldHave = expected != slots.end();
				const bool has = HasSlot(store, entity, slot);

				if (shouldHave && !has) {
					divergence.Missing++;
				} else if (!shouldHave && has) {
					divergence.Extra++;
				} else if (shouldHave && GetSlot(store, entity, slot) != expected->second) {
					divergence.WrongValue++;
				}
			}
		}

		// Nothing destroyed reads as alive, whatever its index was reused for.
		for (const Entity entity : retired) {
			if (store.Alive(entity)) {
				divergence.DeadAlive++;
			}
		}

		// Iteration agrees with lookup, and the count agrees with both.
		for (int slot = 0; slot < COMPONENT_COUNT; slot++) {
			std::map<uint64_t, int64_t> expected;
			for (const auto &[id, slots] : model) {
				const auto at = slots.find(slot);
				if (at != slots.end()) {
					expected.emplace(id, at->second);
				}
			}

			const std::map<uint64_t, int64_t> found = IterateSlot(store, slot);
			if (found != expected) {
				divergence.WrongCount++;
			}
			if (CountSlot(store, slot) != expected.size()) {
				divergence.WrongCount++;
			}
		}

		return divergence;
	}

	// Drives a store and a model through the same operations.
	//
	// `salt` varies the stream between cases so that two calls exercise
	// different interleavings without either being unreproducible.
	Divergence Churn(Store &store, uint32_t steps, uint32_t salt, bool snapshotting) {
		Model model;
		std::vector<Entity> alive;
		std::vector<Entity> retired;

		for (uint32_t step = 0; step < steps; step++) {
			const uint32_t roll = Random::Bits(step, salt) % 100;

			if (roll < 25 || alive.empty()) {
				const Entity entity = store.Create();
				alive.push_back(entity);
				model[entity.Id];
			} else if (roll < 35) {
				const size_t at = Random::Bits(step, salt + 1) % alive.size();
				const Entity entity = alive[at];

				store.Destroy(entity);
				model.erase(entity.Id);
				retired.push_back(entity);
				alive.erase(alive.begin() + static_cast<long>(at));
			} else if (roll < 75) {
				const Entity entity = alive[Random::Bits(step, salt + 2) % alive.size()];
				const int slot = static_cast<int>(Random::Bits(step, salt + 3) % COMPONENT_COUNT);
				const auto value = static_cast<int64_t>(Random::Bits(step, salt + 4));

				SetSlot(store, entity, slot, value);
				model[entity.Id][slot] = value;
			} else if (roll < 92) {
				const Entity entity = alive[Random::Bits(step, salt + 5) % alive.size()];
				const int slot = static_cast<int>(Random::Bits(step, salt + 6) % COMPONENT_COUNT);

				RemoveSlot(store, entity, slot);
				model[entity.Id].erase(slot);
			} else if (snapshotting) {
				// A snapshot in the middle of churn, restored over the top of
				// the world it came from. Anything the storage got wrong about
				// identity shows up as the next comparison failing.
				ByteWriter writer;
				if (store.Save(writer)) {
					ByteReader reader(writer.Bytes());
					REQUIRE(store.Load(reader));
				}
			}
		}

		return Compare(store, model, retired);
	}
}

using namespace fuzz_test;

TEST_CASE("the store matches a model under random churn", "[ecs][fuzz]") {
	// The headline case. Twenty thousand interleaved operations over eight
	// component types, which reaches archetypes no hand-written case would
	// think to build.
	Store store("fuzz");
	const Divergence divergence = Churn(store, 20'000, 1, false);

	REQUIRE(divergence.Missing == 0);
	REQUIRE(divergence.Extra == 0);
	REQUIRE(divergence.WrongValue == 0);
	REQUIRE(divergence.WrongCount == 0);
	REQUIRE(divergence.DeadAlive == 0);
}

TEST_CASE("the store matches a model across snapshots taken mid-churn", "[ecs][fuzz]") {
	// The same stream with a save-and-restore folded in. A snapshot that
	// preserved values but not identity passes the previous case and fails
	// this one.
	Store store("fuzz");
	const Divergence divergence = Churn(store, 10'000, 7, true);

	REQUIRE(divergence.Total() == 0);
}

TEST_CASE("many independent streams all match", "[ecs][fuzz]") {
	// Different salts reach different interleavings. One long run explores one
	// path through the state space; several shorter ones explore several.
	size_t failures = 0;

	for (uint32_t salt = 100; salt < 120; salt++) {
		Store store("fuzz");
		if (Churn(store, 1'000, salt, false).Total() != 0) {
			failures++;
		}
	}

	REQUIRE(failures == 0);
}

TEST_CASE("a world driven only by adds and removes stays consistent", "[ecs][fuzz]") {
	// No destroys, so entities live the whole run and every archetype
	// transition is an add or a remove. This is the path that exercises the
	// archetype graph hardest — the same entity walks the lattice repeatedly.
	Store store("fuzz");
	Model model;
	std::vector<Entity> entities;

	for (int index = 0; index < 64; index++) {
		const Entity entity = store.Create();
		entities.push_back(entity);
		model[entity.Id];
	}

	for (uint32_t step = 0; step < 20'000; step++) {
		const Entity entity = entities[Random::Bits(step, 31) % entities.size()];
		const int slot = static_cast<int>(Random::Bits(step, 32) % COMPONENT_COUNT);

		if (Random::Bits(step, 33) % 2 == 0) {
			const auto value = static_cast<int64_t>(Random::Bits(step, 34));
			SetSlot(store, entity, slot, value);
			model[entity.Id][slot] = value;
		} else {
			RemoveSlot(store, entity, slot);
			model[entity.Id].erase(slot);
		}
	}

	REQUIRE(Compare(store, model, {}).Total() == 0);
}

TEST_CASE("every archetype in the lattice is reachable and correct", "[ecs][fuzz]") {
	// Exhaustive rather than random: build one entity for each of the 256
	// subsets of eight components and check every one holds exactly its subset.
	// A bug in the merge walk that only fires for a particular overlap shows up
	// here and nowhere else.
	Store store("lattice");
	std::vector<Entity> entities;

	for (int mask = 0; mask < 256; mask++) {
		const Entity entity = store.Create();
		entities.push_back(entity);
		for (int slot = 0; slot < COMPONENT_COUNT; slot++) {
			if ((mask & (1 << slot)) != 0) {
				SetSlot(store, entity, slot, mask * 100 + slot);
			}
		}
	}

	size_t wrong = 0;
	for (int mask = 0; mask < 256; mask++) {
		for (int slot = 0; slot < COMPONENT_COUNT; slot++) {
			const bool expected = (mask & (1 << slot)) != 0;
			if (HasSlot(store, entities[mask], slot) != expected) {
				wrong++;
			} else if (expected && GetSlot(store, entities[mask], slot) != mask * 100 + slot) {
				wrong++;
			}
		}
	}

	REQUIRE(wrong == 0);

	// 255, not 256. Mask zero is the entity with no components, and an entity
	// with no components occupies no table at all — it is a directory slot, and
	// a row is what a component buys. The empty set is reachable and interned;
	// nothing ever builds a table for it.
	REQUIRE(store.TableCount() == 255);

	// And a query for one component matches exactly the masks containing it.
	for (int slot = 0; slot < COMPONENT_COUNT; slot++) {
		REQUIRE(CountSlot(store, slot) == 128);
	}
}

TEST_CASE("removing components walks back down the lattice", "[ecs][fuzz]") {
	// The other direction, which uses different archetype-graph edges.
	Store store("lattice");
	std::vector<Entity> entities;

	for (int mask = 0; mask < 256; mask++) {
		const Entity entity = store.Create();
		entities.push_back(entity);
		for (int slot = 0; slot < COMPONENT_COUNT; slot++) {
			SetSlot(store, entity, slot, mask * 100 + slot);
		}
	}

	for (int mask = 0; mask < 256; mask++) {
		for (int slot = 0; slot < COMPONENT_COUNT; slot++) {
			if ((mask & (1 << slot)) == 0) {
				RemoveSlot(store, entities[mask], slot);
			}
		}
	}

	size_t wrong = 0;
	for (int mask = 0; mask < 256; mask++) {
		for (int slot = 0; slot < COMPONENT_COUNT; slot++) {
			if (HasSlot(store, entities[mask], slot) != ((mask & (1 << slot)) != 0)) {
				wrong++;
			}
		}
	}

	REQUIRE(wrong == 0);
}

TEST_CASE("structural changes inside Each land after it, exactly once", "[ecs][fuzz]") {
	// Deferral is the subsystem with the least intuitive failure mode: a
	// command applied twice, or applied against an entity another command
	// already destroyed, or one that displaces a row a later command names.
	Store store("defer");

	std::vector<Entity> entities;
	for (int index = 0; index < 500; index++) {
		const Entity entity = store.Create();
		store.Set<C0>(entity, C0{index});
		entities.push_back(entity);
	}

	size_t visited = 0;
	store.Each<C0>([&](Entity entity, C0 &value) {
		visited++;

		// Every kind of structural change, from inside the loop.
		if (value.Value % 3 == 0) {
			store.Destroy(entity);
		} else if (value.Value % 3 == 1) {
			store.Set<C1>(entity, C1{value.Value * 2});
		} else {
			store.Remove<C0>(entity);
		}
	});

	// The loop saw the world as it was, not as its own changes made it.
	REQUIRE(visited == 500);

	size_t alive = 0;
	size_t promoted = 0;
	size_t stripped = 0;
	for (int index = 0; index < 500; index++) {
		const Entity entity = entities[static_cast<size_t>(index)];
		if (index % 3 == 0) {
			if (!store.Alive(entity)) {
				alive++;
			}
		} else if (index % 3 == 1) {
			if (store.Alive(entity) && store.Has<C1>(entity) && store.Get<C1>(entity)->Value == index * 2) {
				promoted++;
			}
		} else if (store.Alive(entity) && !store.Has<C0>(entity)) {
			stripped++;
		}
	}

	REQUIRE(alive == 167); // destroyed
	REQUIRE(promoted == 167);
	REQUIRE(stripped == 166);
}

TEST_CASE("a destroy inside Each does not disturb the rows still to come", "[ecs][fuzz]") {
	// Swap-back removal moves the last row into the hole. If a destroy were
	// applied immediately, the loop would skip whichever entity got moved —
	// and it would skip a different one every run.
	Store store("defer");

	for (int index = 0; index < 1'000; index++) {
		store.Set<C0>(store.Create(), C0{index});
	}

	std::set<int64_t> seen;
	store.Each<C0>([&](Entity entity, C0 &value) {
		seen.insert(value.Value);
		store.Destroy(entity);
	});

	REQUIRE(seen.size() == 1'000);
	REQUIRE(store.CountMatching<C0>() == 0);
}

TEST_CASE("nested Each defers to the outermost loop", "[ecs][fuzz]") {
	Store store("defer");
	for (int index = 0; index < 50; index++) {
		store.Set<C0>(store.Create(), C0{index});
	}

	size_t inner = 0;
	store.Each<C0>([&](Entity outer, C0 &) {
		store.Each<C0>([&](Entity, C0 &) { inner++; });
		store.Set<C1>(outer, C1{1});
	});

	// The inner loop saw all fifty every time, because nothing landed until the
	// outer one finished.
	REQUIRE(inner == 50 * 50);
	REQUIRE(store.CountMatching<C0, C1>() == 50);
}

TEST_CASE("entity handles survive arbitrary churn around them", "[ecs][fuzz]") {
	// One entity is held across ten thousand operations that move it between
	// tables and displace it repeatedly. Its handle must never stop resolving.
	Store store("handles");

	const Entity held = store.Create();
	store.Set<C0>(held, C0{12345});

	std::vector<Entity> others;
	size_t lost = 0;

	for (uint32_t step = 0; step < 10'000; step++) {
		const uint32_t roll = Random::Bits(step, 91) % 4;

		if (roll == 0) {
			const Entity entity = store.Create();
			store.Set<C0>(entity, C0{static_cast<int64_t>(step)});
			others.push_back(entity);
		} else if (roll == 1 && !others.empty()) {
			const size_t at = Random::Bits(step, 92) % others.size();
			store.Destroy(others[at]);
			others.erase(others.begin() + static_cast<long>(at));
		} else if (roll == 2) {
			store.Set<C1>(held, C1{static_cast<int64_t>(step)});
		} else {
			store.Remove<C1>(held);
		}

		if (!store.Alive(held) || store.Get<C0>(held) == nullptr || store.Get<C0>(held)->Value != 12345) {
			lost++;
		}
	}

	REQUIRE(lost == 0);
}

TEST_CASE("batch and row iteration agree under churn", "[ecs][fuzz]") {
	// Three ways of walking the same query must produce the same multiset.
	// They take different paths through the storage, and a table boundary
	// handled wrongly by one of them shows up as a count that does not match.
	Store store("iteration");

	for (uint32_t step = 0; step < 4'000; step++) {
		const Entity entity = store.Create();
		store.Set<C0>(entity, C0{static_cast<int64_t>(step)});
		if (step % 3 == 0) {
			store.Set<C1>(entity, C1{0});
		}
		if (step % 5 == 0) {
			store.Set<C2>(entity, C2{0});
		}
		if (step % 7 == 0) {
			store.Destroy(entity);
		}
	}

	std::vector<int64_t> byRow;
	store.Each<C0>([&](Entity, C0 &value) { byRow.push_back(value.Value); });

	std::vector<int64_t> byBatch;
	store.EachBatch<C0>([&](size_t rows, C0 *values) {
		for (size_t row = 0; row < rows; row++) {
			byBatch.push_back(values[row].Value);
		}
	});

	std::vector<int64_t> byParallel;
	store.EachParallel<C0>([&](Entity, C0 &) {}, 64); // just has to not corrupt anything
	store.Each<C0>([&](Entity, C0 &value) { byParallel.push_back(value.Value); });

	std::sort(byRow.begin(), byRow.end());
	std::sort(byBatch.begin(), byBatch.end());
	std::sort(byParallel.begin(), byParallel.end());

	REQUIRE(byRow == byBatch);
	REQUIRE(byRow == byParallel);
	REQUIRE(byRow.size() == store.CountMatching<C0>());
}

TEST_CASE("EachBatchParallel indexes a packed output correctly", "[ecs][fuzz]") {
	// The contract that makes it safe to parallelise: `first` is the row's
	// index in the iteration as a whole, so disjoint workers fill disjoint
	// slices of one array with no atomics.
	Store store("packed");

	constexpr size_t COUNT = 20'000;
	for (size_t index = 0; index < COUNT; index++) {
		const Entity entity = store.Create();
		store.Set<C0>(entity, C0{static_cast<int64_t>(index)});
		if (index % 4 == 0) {
			store.Set<C1>(entity, C1{0}); // several tables, so several batches
		}
	}

	std::vector<int64_t> packed(COUNT, MISSING);
	const size_t visited = store.EachBatchParallel<C0>(
		[&](size_t first, size_t rows, C0 *values) {
			for (size_t row = 0; row < rows; row++) {
				packed[first + row] = values[row].Value;
			}
		},
		256
	);

	REQUIRE(visited == COUNT);
	REQUIRE(std::count(packed.begin(), packed.end(), MISSING) == 0);

	// Every value arrived exactly once, wherever it landed.
	std::sort(packed.begin(), packed.end());
	for (size_t index = 0; index < COUNT; index++) {
		if (packed[index] != static_cast<int64_t>(index)) {
			FAIL("packed output lost or duplicated a row at " << index);
		}
	}
}

TEST_CASE("change tracking matches a model under churn", "[ecs][fuzz]") {
	Store store("changes");
	store.Observe<C0>();

	std::vector<Entity> entities;
	for (int index = 0; index < 200; index++) {
		const Entity entity = store.Create();
		store.Set<C0>(entity, C0{index});
		entities.push_back(entity);
	}

	size_t wrong = 0;
	for (uint32_t round = 0; round < 200; round++) {
		store.ClearChanges();

		std::set<uint64_t> expected;
		for (uint32_t write = 0; write < 20; write++) {
			const Entity entity = entities[Random::Bits(round, write + 200) % entities.size()];
			store.Set<C0>(entity, C0{static_cast<int64_t>(round)});
			expected.insert(entity.Id);
		}

		std::set<uint64_t> reported;
		store.EachChanged<C0>([&](Entity entity, C0 &) { reported.insert(entity.Id); });

		if (reported != expected) {
			wrong++;
		}
	}

	REQUIRE(wrong == 0);
}

TEST_CASE("a world survives being cleared and rebuilt repeatedly", "[ecs][fuzz]") {
	// Clear has to leave the store as usable as a fresh one — including its
	// query plans, which hold table indices that no longer exist.
	Store store("cycles");

	for (uint32_t cycle = 0; cycle < 50; cycle++) {
		Model model;
		std::vector<Entity> alive;

		for (uint32_t step = 0; step < 200; step++) {
			const Entity entity = store.Create();
			alive.push_back(entity);
			model[entity.Id];

			const int slot = static_cast<int>(Random::Bits(cycle * 200 + step, 61) % COMPONENT_COUNT);
			const auto value = static_cast<int64_t>(step);
			SetSlot(store, entity, slot, value);
			model[entity.Id][slot] = value;
		}

		REQUIRE(Compare(store, model, {}).Total() == 0);
		store.Clear();
		REQUIRE(store.TableCount() == 0);
	}
}
