// What a driver tick costs, and how it scales with the shape of the universe.
//
// The target from `v02v03v04.md` is mixed: one or two worlds at 50k–500k
// entities, plus tens to hundreds at 1k–10k. Both shapes are here, because they
// stress different things - the large one measures the inner loop and the small
// one measures the *per-world* overhead, which is the cost that a hundred
// worlds multiply.

#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Bench.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/Universe.hpp>

#include <memory>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.world.bench.barrier")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::testing::Consume;
using engine::world::ExecutionMode;
using engine::world::Postbox;
using engine::world::Universe;
using engine::world::UniverseSettings;
using engine::world::WorldId;
using engine::world::WorldSettings;

namespace barrier_bench {
	struct Position {
		float X = 0.0f;
	};
	struct Velocity {
		float X = 1.0f;
	};

	struct Pool {
		Pool() {
			engine::parallel::Jobs::Start(0);
			engine::world::RegisterMailboxTypes();
		}
		~Pool() {
			engine::parallel::Jobs::Stop();
		}
	};
	const Pool Workers;

	// A universe of `worlds` worlds with `entities` each, built once.
	//
	// Lazily, because a universe binds its driver thread on construction and
	// the thread that runs a namespace static is not necessarily the one that
	// runs the body.
	Universe &UniverseOf(size_t worlds, size_t entities, ExecutionMode mode) {
		struct Key {
			size_t Worlds;
			size_t Entities;
			ExecutionMode Mode;
		};
		static std::vector<std::pair<Key, std::unique_ptr<Universe>>> built;

		for (auto &[key, universe] : built) {
			if (key.Worlds == worlds && key.Entities == entities && key.Mode == mode) {
				return *universe;
			}
		}

		UniverseSettings settings;
		settings.Mode = mode;

		auto universe = std::make_unique<Universe>(settings);
		for (size_t index = 0; index < worlds; index++) {
			WorldSettings world;
			world.Name = Name("bench.world." + std::to_string(index));
			world.TickRate = 60.0;

			const WorldId id = universe->Create(world);
			universe->Enter(id, [entities](Store &store, Scheduler &systems) {
				for (size_t entity = 0; entity < entities; entity++) {
					const Entity made = store.Create();
					store.Set<Position>(made, Position{static_cast<float>(entity)});
					store.Set<Velocity>(made, Velocity{1.0f});
				}
				systems.Add("integrate", Phase::Simulation, [](Store &world) {
					world.Each<Position, const Velocity>(
						[](Entity, Position &position, const Velocity &velocity) { position.X += velocity.X; }
					);
				});
			});
		}

		built.emplace_back(Key{worlds, entities, mode}, std::move(universe));
		return *built.back().second;
	}

	// A universe whose worlds all talk on one topic. What a barrier costs when
	// there is traffic to route rather than none.
	Universe &ChattyUniverse(size_t worlds, ExecutionMode mode = ExecutionMode::WorldParallel) {
		static std::vector<std::pair<std::pair<size_t, ExecutionMode>, std::unique_ptr<Universe>>> built;

		for (auto &[key, held] : built) {
			if (key.first == worlds && key.second == mode) {
				return *held;
			}
		}

		UniverseSettings settings;
		settings.Mode = mode;
		auto universe = std::make_unique<Universe>(settings);

		for (size_t index = 0; index < worlds; index++) {
			WorldSettings world;
			world.Name = Name("bench.chat." + std::to_string(index));
			world.TickRate = 60.0;

			const WorldId id = universe->Create(world);
			universe->Enter(id, [](Store &store, Scheduler &systems) {
				systems.Add("chat", Phase::PreSimulation, [](Store &world) {
					Postbox box(world);
					if (world.Time().Tick <= 1) {
						box.Subscribe("bench.topic");
						return;
					}
					box.Publish("bench.topic");
				});
			});
		}

		// Two ticks, so the subscriptions have taken effect before anything is
		// measured. Measuring the tick that sets a benchmark up is measuring
		// the setup.
		universe->Tick(1.0f / 60.0f);
		universe->Tick(1.0f / 60.0f);

		built.emplace_back(std::make_pair(worlds, mode), std::move(universe));
		return *built.back().second;
	}
}

using namespace barrier_bench;

// --- the large shape: a few big worlds ---------------------------------------

BENCH("Tick · 1 world of 100k", 20) {
	Universe &universe = UniverseOf(1, 100'000, ExecutionMode::WorldParallel);
	for (int pass = 0; pass < 20; pass++) {
		universe.Tick(1.0f / 60.0f);
	}
}

BENCH("Tick · 2 worlds of 100k, parallel", 20) {
	Universe &universe = UniverseOf(2, 100'000, ExecutionMode::WorldParallel);
	for (int pass = 0; pass < 20; pass++) {
		universe.Tick(1.0f / 60.0f);
	}
}

BENCH("Tick · 2 worlds of 100k, serial", 20) {
	// The switch changes no result, only where the parallelism is taken. What
	// it costs is the number that says whether `WorldSerial` is a sensible
	// default for a dedicated host.
	Universe &universe = UniverseOf(2, 100'000, ExecutionMode::WorldSerial);
	for (int pass = 0; pass < 20; pass++) {
		universe.Tick(1.0f / 60.0f);
	}
}

// --- the small shape: many little worlds -------------------------------------
//
// Per-world overhead is what a hundred worlds multiply. These three sizes are
// the same total entity count in different numbers of worlds, so the difference
// between them is the overhead and nothing else.

BENCH("Tick · 4 worlds of 100k, parallel", 20) {
	// A few large worlds is half the stated target scale, and it is the case a
	// blanket "not worth dispatching below N" rule would quietly ruin: four
	// world ticks are four expensive things, not four cheap ones.
	Universe &universe = UniverseOf(4, 100'000, ExecutionMode::WorldParallel);
	for (int pass = 0; pass < 20; pass++) {
		universe.Tick(1.0f / 60.0f);
	}
}

BENCH("Tick · 4 worlds of 100k, serial", 20) {
	Universe &universe = UniverseOf(4, 100'000, ExecutionMode::WorldSerial);
	for (int pass = 0; pass < 20; pass++) {
		universe.Tick(1.0f / 60.0f);
	}
}

BENCH("Tick · 10 worlds of 2k", 50) {
	Universe &universe = UniverseOf(10, 2'000, ExecutionMode::WorldParallel);
	for (int pass = 0; pass < 50; pass++) {
		universe.Tick(1.0f / 60.0f);
	}
}

BENCH("Tick · 100 worlds of 200", 50) {
	Universe &universe = UniverseOf(100, 200, ExecutionMode::WorldParallel);
	for (int pass = 0; pass < 50; pass++) {
		universe.Tick(1.0f / 60.0f);
	}
}

BENCH("Tick · 200 worlds of 100", 20) {
	Universe &universe = UniverseOf(200, 100, ExecutionMode::WorldParallel);
	for (int pass = 0; pass < 20; pass++) {
		universe.Tick(1.0f / 60.0f);
	}
}

// --- the barrier itself ------------------------------------------------------

BENCH("Tick · 50 quiet worlds, no entities", 200) {
	Universe &universe = UniverseOf(50, 0, ExecutionMode::WorldParallel);
	for (int pass = 0; pass < 200; pass++) {
		universe.Tick(1.0f / 60.0f);
	}
}

BENCH("Tick · 50 quiet worlds, serial", 200) {
	// The same thing with the job system taken out of the measurement.
	//
	// **The parallel version above cannot see a change to the barrier.** Its
	// samples vary by a quarter to a half because thread wake-up latency
	// dominates fifty empty worlds, and a measurement whose noise is larger
	// than its subject measures the scheduler. This one runs the identical
	// bookkeeping on the calling thread, so what it reports is the per-world
	// cost of a barrier and nothing else - which is the number the storage and
	// resource paths are actually judged on.
	Universe &universe = UniverseOf(50, 0, ExecutionMode::WorldSerial);
	for (int pass = 0; pass < 200; pass++) {
		universe.Tick(1.0f / 60.0f);
	}
}

BENCH("Tick · 200 suspended worlds, serial", 50) {
	// The barrier on its own. A suspended world is still walked by the router
	// and still given an inbox and a budget; what it does not do is tick.
	//
	// Against the case below, the difference is `World::Tick` and the
	// difference alone - which is the only way to tell "the barrier costs too
	// much per world" from "an empty tick costs too much", and those want
	// opposite fixes.
	static Universe *suspended = nullptr;
	if (suspended == nullptr) {
		// Its own universe, not the one the active case caches: suspending that
		// one would silently change what the benchmark below measures.
		suspended = &UniverseOf(201, 0, ExecutionMode::WorldSerial);
		for (const WorldId id : suspended->Worlds()) {
			suspended->SetState(id, engine::world::WorldState::Suspended);
		}
	}
	for (int pass = 0; pass < 50; pass++) {
		suspended->Tick(1.0f / 60.0f);
	}
}

BENCH("Tick · 200 quiet worlds, serial", 50) {
	// Four times the worlds, same emptiness. Against the case above this is
	// the per-world term on its own, with every fixed cost of a barrier
	// divided out.
	Universe &universe = UniverseOf(200, 0, ExecutionMode::WorldSerial);
	for (int pass = 0; pass < 50; pass++) {
		universe.Tick(1.0f / 60.0f);
	}
}

BENCH("Tick · 50 worlds all publishing, serial", 50) {
	// The delivery path with the scheduler taken out, which is the only shape
	// that can see a change to how an inbox is handed over. Fifty publishes
	// fanned to forty-nine subscribers each is 2450 deliveries a tick, so the
	// per-world inbox handover is the measurement rather than a rounding error
	// beside thread wake-up.
	Universe &universe = ChattyUniverse(50, ExecutionMode::WorldSerial);
	for (int pass = 0; pass < 50; pass++) {
		universe.Tick(1.0f / 60.0f);
	}
}

BENCH("Tick · 50 worlds all publishing to each other", 50) {
	// Fifty publishes routed to forty-nine subscribers each, sorted by
	// `(From, Sequence)`. This is the barrier doing the work it exists for, and
	// the number to watch if the routing ever changes shape.
	Universe &universe = ChattyUniverse(50);
	for (int pass = 0; pass < 50; pass++) {
		universe.Tick(1.0f / 60.0f);
	}
}
