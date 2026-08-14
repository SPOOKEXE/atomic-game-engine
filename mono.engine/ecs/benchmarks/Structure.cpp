// What it costs to change an entity's shape, rather than its values.
//
// `benchmarks/Iteration.cpp` measures the steady state: rows that already exist,
// in tables that already exist, being read and written. This file measures the
// other thing a tick does - adding and removing components, which moves a row
// from one table to another.
//
// **This is the measurement the archetype edge cache is gated on.** `Set` on a
// component an entity lacks interns `set.With(id)`: a vector allocation, a sort,
// an FNV hash and a lookup under a process-wide mutex, before anything is moved.
// Caching add-one and remove-one edges per archetype would turn that into one
// lookup. Whether that is worth doing depends on a number nobody had, and
// `ecs/docs/TODO.md` says so in as many words: *the number to have first is what
// archetype transitions cost as a fraction of a tick*.
//
// So the benchmarks below are built as a subtraction, and the pairs matter more
// than the absolute figures:
//
// - **overwrite** writes a component the entity already has. Same call, same
//   validity checks, same dirty-bit mark, no transition. This is the floor.
// - **toggle** adds a component and removes it again. Same work as overwrite
//   plus two transitions per entity.
// - **intern only** calls `With` and `Without` and moves no rows at all, which
//   splits the transition into the part an edge cache can delete and the part it
//   cannot.
//
// `toggle - overwrite` is what a transition costs. `intern only` is how much of
// that the cache could actually take back. A cache that removed the interning
// entirely would still leave the row move, so the second figure is the ceiling
// on the win and not the win.

#include <engine/ecs/ComponentSet.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Bench.hpp>

#include <memory>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.ecs.bench.structure")

using engine::ecs::ComponentId;
using engine::ecs::Components;
using engine::ecs::ComponentSet;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::testing::Consume;

namespace structure_bench {
	struct Position {
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
	};
	struct Velocity {
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
	};

	// The component that gets added and removed. Deliberately small: a
	// transition's cost is the bookkeeping plus moving the *other* columns, and
	// a large payload here would measure the memcpy of the thing being added
	// rather than the shape change.
	struct Flag {
		uint32_t Value = 0;
	};

	// A world of `entities` rows, each holding Position and Velocity and no
	// Flag, built once and reused.
	//
	// Lazily rather than at static-initialisation time, for the reason
	// `Iteration.cpp` gives: a store binds its owning thread on construction and
	// that is not necessarily the thread that runs a body.
	struct World {
		std::unique_ptr<Store> Contents;
		std::vector<Entity> Entities;
	};

	World &WorldOf(size_t entities) {
		static std::vector<std::pair<size_t, World>> built;

		for (auto &[size, world] : built) {
			if (size == entities) {
				return world;
			}
		}

		World world;
		world.Contents = std::make_unique<Store>("bench");
		world.Entities.reserve(entities);

		for (size_t index = 0; index < entities; index++) {
			const Entity entity = world.Contents->Create();
			world.Contents->Set<Position>(entity, Position{static_cast<float>(index), 0.0f, 0.0f});
			world.Contents->Set<Velocity>(entity, Velocity{1.0f, 2.0f, 3.0f});
			world.Entities.push_back(entity);
		}

		built.emplace_back(entities, std::move(world));
		return built.back().second;
	}
}

using namespace structure_bench;

// --- the floor -------------------------------------------------------------
//
// A write to a component that is already there. Everything a transition does
// except the transition: resolve the id, check the entity is alive, find the
// column, assign, mark the row dirty.

BENCH("overwrite · 10k rows", 1) {
	World &world = WorldOf(10'000);
	for (const Entity entity : world.Entities) {
		world.Contents->Set<Position>(entity, Position{1.0f, 2.0f, 3.0f});
	}
	Consume(world.Entities.size());
}

BENCH("overwrite · 100k rows", 1) {
	World &world = WorldOf(100'000);
	for (const Entity entity : world.Entities) {
		world.Contents->Set<Position>(entity, Position{1.0f, 2.0f, 3.0f});
	}
	Consume(world.Entities.size());
}

// --- the transition --------------------------------------------------------
//
// Add a component to every row, then take it away again. Two transitions per
// entity, and the world ends in the shape it started in - which is what makes
// the body repeatable, and is also the honest case: a component that goes on
// and stays on is a one-off, while a component that toggles is what a gameplay
// tick actually does.

BENCH("toggle · add and remove, 10k rows", 1) {
	World &world = WorldOf(10'000);
	for (const Entity entity : world.Entities) {
		world.Contents->Set<Flag>(entity, Flag{1});
	}
	for (const Entity entity : world.Entities) {
		world.Contents->Remove<Flag>(entity);
	}
	Consume(world.Entities.size());
}

BENCH("toggle · add and remove, 100k rows", 1) {
	World &world = WorldOf(100'000);
	for (const Entity entity : world.Entities) {
		world.Contents->Set<Flag>(entity, Flag{1});
	}
	for (const Entity entity : world.Entities) {
		world.Contents->Remove<Flag>(entity);
	}
	Consume(world.Entities.size());
}

// --- what the cache could take back ----------------------------------------
//
// The interning alone: no store, no entity, no row moved. This is the part an
// edge cache replaces with a lookup, so it bounds the win from above. The row
// move underneath it is not going anywhere.

BENCH("intern only · With then Without, 10k times", 1) {
	const ComponentId flag = Components::Of<Flag>();
	const ComponentSet &base = ComponentSet::Intern({Components::Of<Position>(), Components::Of<Velocity>()});

	for (size_t index = 0; index < 10'000; index++) {
		const ComponentSet &added = base.With(flag);
		const ComponentSet &back = added.Without(flag);
		Consume(back.Id());
	}
}

BENCH("intern only · With then Without, 100k times", 1) {
	const ComponentId flag = Components::Of<Flag>();
	const ComponentSet &base = ComponentSet::Intern({Components::Of<Position>(), Components::Of<Velocity>()});

	for (size_t index = 0; index < 100'000; index++) {
		const ComponentSet &added = base.With(flag);
		const ComponentSet &back = added.Without(flag);
		Consume(back.Id());
	}
}

// --- a tick, for scale -----------------------------------------------------
//
// The fraction the TODO asks for needs a denominator. This is the same three
// adds `Iteration.cpp` measures, repeated here so that a reader comparing a
// transition against a tick is comparing two numbers from one run on one
// machine rather than across two.

BENCH("control · Each over 10k rows", 1) {
	World &world = WorldOf(10'000);
	world.Contents->Each<Position, const Velocity>([](Entity, Position &position, const Velocity &velocity) {
		position.X += velocity.X;
		position.Y += velocity.Y;
		position.Z += velocity.Z;
	});
	Consume(world.Entities.size());
}

BENCH("control · Each over 100k rows", 1) {
	World &world = WorldOf(100'000);
	world.Contents->Each<Position, const Velocity>([](Entity, Position &position, const Velocity &velocity) {
		position.X += velocity.X;
		position.Y += velocity.Y;
		position.Z += velocity.Z;
	});
	Consume(world.Entities.size());
}
