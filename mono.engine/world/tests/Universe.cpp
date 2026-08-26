#include <engine/core/FrameGraph.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/Random.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.world.universe")

using engine::core::Name;
using engine::core::Random;
using engine::ecs::Entity;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::parallel::Jobs;
using engine::world::ExecutionMode;
using engine::world::Isolation;
using engine::world::Presentation;
using engine::world::Universe;
using engine::world::UniverseSettings;
using engine::world::WorldId;
using engine::world::WorldSettings;
using engine::world::WorldState;
using engine::world::WorldStatus;

namespace universe_test {
	struct Counter {
		int64_t Value = 0;
	};
	struct Step {
		int64_t By = 1;
	};

	// A pool for the cases that want the parallel path to be real.
	struct Pool {
		explicit Pool(unsigned workers) {
			Jobs::Start(workers);
		}
		~Pool() {
			Jobs::Stop();
		}
	};

	WorldSettings Named(const char *name, double rate = 60.0) {
		WorldSettings settings;
		settings.Name = Name(name);
		settings.TickRate = rate;
		return settings;
	}

	// Adds `Step` to `Counter` on every entity, once per tick.
	void Advance(Store &store) {
		store.Each<Counter, const Step>([](Entity, Counter &counter, const Step &step) {
			counter.Value += step.By;
		});
	}

	// Fills a world with `count` counting entities and registers the system.
	void Populate(Universe &universe, WorldId id, int count) {
		universe.Enter(id, [count](Store &store, Scheduler &systems) {
			for (int index = 0; index < count; index++) {
				const Entity entity = store.Create();
				store.Set<Counter>(entity, Counter{0});
				store.Set<Step>(entity, Step{1});
			}
			systems.Add("advance", Phase::Simulation, Advance);
		});
	}

	// The sum of every counter in a world, which is tick count times entities
	// when everything ran the same number of times.
	int64_t Total(Universe &universe, WorldId id) {
		int64_t total = 0;
		universe.Enter(id, [&total](Store &store) {
			store.Each<const Counter>([&total](Entity, const Counter &counter) { total += counter.Value; });
		});
		return total;
	}
}

using namespace universe_test;

// --- the registry ---------------------------------------------------------

TEST_CASE("a fresh universe holds no worlds", "[world]") {
	Universe universe;
	REQUIRE(universe.Count() == 0);
	REQUIRE(universe.Worlds().empty());
	REQUIRE_FALSE(universe.Find(Name("nothing")).IsValid());
}

TEST_CASE("a world is created and found by name", "[world]") {
	Universe universe;

	WorldStatus status = WorldStatus::NoSuchWorld;
	const WorldId id = universe.Create(Named("world.lobby"), &status);

	REQUIRE(status == WorldStatus::Ok);
	REQUIRE(id.IsValid());
	REQUIRE(universe.Count() == 1);
	REQUIRE(universe.Find(Name("world.lobby")) == id);
	REQUIRE(universe.NameOf(id) == Name("world.lobby"));
	REQUIRE(universe.StateOf(id) == WorldState::Active);
}

TEST_CASE("a world selects a rendering profile by stable name", "[world]") {
	Universe universe;
	const WorldId id = universe.Create(Named("world.lit"));

	CHECK(universe.SettingsOf(id).RenderingProfile == Name("Default PBR"));
	CHECK(universe.SetRenderingProfile(id, Name("Cinematic")) == WorldStatus::Ok);
	CHECK(universe.SettingsOf(id).RenderingProfile == Name("Cinematic"));
	CHECK(universe.SetRenderingProfile(WorldId{999}, Name("Missing")) == WorldStatus::NoSuchWorld);
}

TEST_CASE("a world with no name is refused", "[world]") {
	// Everything crossing a boundary addresses a world by name, so one without
	// a name cannot be reached by anything outside itself.
	Universe universe;

	WorldStatus status = WorldStatus::Ok;
	const WorldId id = universe.Create(WorldSettings{}, &status);

	REQUIRE_FALSE(id.IsValid());
	REQUIRE(status == WorldStatus::NoName);
	REQUIRE(universe.Count() == 0);
}

TEST_CASE("a duplicate name returns the world already holding it", "[world]") {
	Universe universe;
	const WorldId first = universe.Create(Named("world.zone"));

	WorldStatus status = WorldStatus::Ok;
	const WorldId second = universe.Create(Named("world.zone"), &status);

	REQUIRE(status == WorldStatus::NameTaken);
	REQUIRE(second == first);
	REQUIRE(universe.Count() == 1);
}

TEST_CASE("destroying a world frees its slot for reuse", "[world]") {
	// A universe that creates and destroys instanced subareas all day must not
	// grow its registry forever.
	Universe universe;

	const WorldId first = universe.Create(Named("world.instance.1"));
	REQUIRE(universe.Destroy(first) == WorldStatus::Ok);
	REQUIRE(universe.Count() == 0);

	// The handle stops resolving the moment the slot is empty.
	REQUIRE_FALSE(universe.NameOf(first).IsValid());
	REQUIRE(universe.StatisticsOf(first).Ticks == 0);

	const WorldId second = universe.Create(Named("world.instance.2"));
	REQUIRE(second == first); // the slot came back
	REQUIRE(universe.Count() == 1);
}

TEST_CASE("destroying a world that does not exist is reported, not crashed", "[world]") {
	Universe universe;
	REQUIRE(universe.Destroy(WorldId{}) == WorldStatus::NoSuchWorld);
	REQUIRE(universe.Destroy(WorldId{999}) == WorldStatus::NoSuchWorld);
	REQUIRE(universe.Enter(WorldId{999}, [](Store &, Scheduler &) {}) == WorldStatus::NoSuchWorld);
}

TEST_CASE("a world has a root instance from the start", "[world]") {
	// So no system has to check whether the world has one.
	Universe universe;
	const WorldId id = universe.Create(Named("world.rooted"));

	universe.Enter(id, [](Store &store) { REQUIRE(store.Find("workspace") != engine::ecs::NULL_ENTITY); });
}

// --- entering -------------------------------------------------------------

TEST_CASE("Enter is the only way to reach a store, and it is scoped", "[world]") {
	// There is no Universe::Get(WorldId) -> Store &. A long-lived reference is
	// what makes thread-per-world and process-per-world different designs.
	Universe universe;
	const WorldId id = universe.Create(Named("world.entered"));

	bool ran = false;
	REQUIRE(universe.Enter(id, [&ran](Store &store, Scheduler &) {
		store.Set<Counter>(store.Create(), Counter{7});
		ran = true;
	}) == WorldStatus::Ok);

	REQUIRE(ran);
	REQUIRE(Total(universe, id) == 7);
}

TEST_CASE("the store-only overload reads without a scheduler", "[world]") {
	Universe universe;
	const WorldId id = universe.Create(Named("world.inspected"));
	Populate(universe, id, 4);

	size_t seen = 0;
	REQUIRE(universe.Enter(id, [&seen](Store &store) {
		seen = store.CountMatching<Counter>();
	}) == WorldStatus::Ok);

	REQUIRE(seen == 4);
}

// --- ticking --------------------------------------------------------------

TEST_CASE("a world ticks at its own rate", "[world]") {
	Universe universe;
	const WorldId fast = universe.Create(Named("world.fast", 60.0));
	const WorldId slow = universe.Create(Named("world.slow", 10.0));

	Populate(universe, fast, 1);
	Populate(universe, slow, 1);

	// One second of wall time, delivered in sixty frames.
	for (int frame = 0; frame < 60; frame++) {
		universe.Tick(1.0f / 60.0f);
	}

	// Rates are independent, which is the point: a busy zone at 60 Hz and a
	// dormant one at 10 in the same universe.
	REQUIRE(universe.StatisticsOf(fast).Ticks == 60);
	REQUIRE(universe.StatisticsOf(slow).Ticks == 10);
	REQUIRE(Total(universe, fast) == 60);
	REQUIRE(Total(universe, slow) == 10);
}

TEST_CASE("a suspended world costs nothing and does not accumulate", "[world]") {
	// The whole point of suspending: a universe of a thousand subareas pays for
	// the handful somebody is standing in. And a world resumed after an hour
	// must not owe an hour of ticks.
	Universe universe;
	const WorldId id = universe.Create(Named("world.suspended"));
	Populate(universe, id, 1);

	universe.Tick(1.0f / 60.0f);
	REQUIRE(universe.StatisticsOf(id).Ticks == 1);

	universe.SetState(id, WorldState::Suspended);
	for (int frame = 0; frame < 600; frame++) {
		universe.Tick(1.0f / 60.0f);
	}
	REQUIRE(universe.StatisticsOf(id).Ticks == 1);
	REQUIRE(universe.Statistics().Suspended == 1);

	universe.SetState(id, WorldState::Active);
	universe.Tick(1.0f / 60.0f);

	// One more tick, not six hundred.
	REQUIRE(universe.StatisticsOf(id).Ticks == 2);
}

TEST_CASE("an idle world ticks at its idle rate", "[world]") {
	Universe universe;
	WorldSettings settings = Named("world.idle", 60.0);
	settings.IdleTickRate = 6.0;
	const WorldId id = universe.Create(settings);
	Populate(universe, id, 1);

	universe.SetState(id, WorldState::Idle);
	for (int frame = 0; frame < 60; frame++) {
		universe.Tick(1.0f / 60.0f);
	}

	// Not zero: crops grow and timers expire whether or not anyone is watching.
	REQUIRE(universe.StatisticsOf(id).Ticks == 6);
}

TEST_CASE("catch-up is capped rather than unbounded", "[world]") {
	// A world far enough behind will not recover by running a hundred ticks in
	// one frame; it will only fall further behind while holding a worker.
	UniverseSettings settings;
	settings.MaximumCatchUpTicks = 4;
	Universe universe(settings);

	const WorldId id = universe.Create(Named("world.stalled"));
	Populate(universe, id, 1);

	// A ten-second frame at 60 Hz owes six hundred ticks.
	universe.Tick(10.0f);

	REQUIRE(universe.StatisticsOf(id).Ticks == 4);
	CHECK(universe.StatisticsOf(id).DroppedTicks > 500);
}

TEST_CASE("universe tuning can change between driver ticks", "[world]") {
	Universe universe;
	const WorldId id = universe.Create(Named("world.retuned"));
	Populate(universe, id, 1);

	universe.SetMode(ExecutionMode::WorldSerial);
	universe.SetMaximumCatchUpTicks(2);
	universe.SetBusBudgetPerTick(17);
	universe.SetChannelQueueLimit(5);
	universe.SetChannelsPerWorld(3);

	CHECK(universe.Settings().Mode == ExecutionMode::WorldSerial);
	CHECK(universe.Settings().MaximumCatchUpTicks == 2);
	CHECK(universe.Settings().BusBudgetPerTick == 17);
	CHECK(universe.Settings().ChannelQueueLimit == 5);
	CHECK(universe.Settings().ChannelsPerWorld == 3);

	universe.Tick(10.0f);
	CHECK(universe.StatisticsOf(id).Ticks == 2);

	universe.SetMaximumCatchUpTicks(0);
	CHECK(universe.Settings().MaximumCatchUpTicks == 1);
}

TEST_CASE("many worlds tick independently", "[world]") {
	Pool pool{4};
	Universe universe;

	std::vector<WorldId> worlds;
	for (int index = 0; index < 16; index++) {
		const WorldId id = universe.Create(Named(("world.many." + std::to_string(index)).c_str()));
		Populate(universe, id, 10);
		worlds.push_back(id);
	}

	for (int frame = 0; frame < 30; frame++) {
		universe.Tick(1.0f / 60.0f);
	}

	for (const WorldId id : worlds) {
		REQUIRE(universe.StatisticsOf(id).Ticks == 30);
		REQUIRE(Total(universe, id) == 30 * 10);
	}
}

TEST_CASE("parallel and serial execution produce the same result", "[world]") {
	// The property that makes the mode a tuning knob rather than a semantic. If
	// this ever fails, the switch is changing behaviour and is no longer a
	// setting.
	const auto run = [](ExecutionMode mode) {
		UniverseSettings settings;
		settings.Mode = mode;
		Universe universe(settings);

		std::vector<WorldId> worlds;
		for (int index = 0; index < 12; index++) {
			const WorldId id =
				universe.Create(Named(("world.mode." + std::to_string(index)).c_str(), 30.0 + index));
			Populate(universe, id, 20);
			worlds.push_back(id);
		}

		for (int frame = 0; frame < 40; frame++) {
			universe.Tick(1.0f / 60.0f);
		}

		std::vector<int64_t> totals;
		for (const WorldId id : worlds) {
			totals.push_back(Total(universe, id));
		}
		return totals;
	};

	Pool pool{4};
	REQUIRE(run(ExecutionMode::WorldParallel) == run(ExecutionMode::WorldSerial));
}

// **Which branch a tick takes, and it turns on the world count.**
//
// A `Jobs::ForWorkers` batch owns the process-wide pool, so a nested `Jobs::For`
// from an assigned task runs inline - which means handing a *lone* world to a
// lane buys no concurrency and spends the pool that everything inside the world
// would otherwise have dispatched to. `Universe::Tick` carries the argument. The
// visible half is the span it names, and that is what this pins: a name is the
// only thing a caller can see the decision through.
TEST_CASE("a lone world ticks on the driver and a pair goes to lanes", "[world]") {
	Pool pool{4};
	if (Jobs::PinnedWorkerCount() < 2) {
		SUCCEED("this platform or process affinity exposes fewer than two pinned workers");
		return;
	}

	struct RecordedTree {
		std::vector<std::string> Names;
		std::vector<std::pair<std::string, std::string>> Edges;
	};

	const auto tickWith = [](size_t worlds) {
		Universe universe;
		for (size_t index = 0; index < worlds; index++) {
			const WorldId id = universe.Create(Named(("world.branch." + std::to_string(index)).c_str()));
			universe.Enter(id, [](Store &store, Scheduler &systems) {
				const Entity entity = store.Create();
				store.Set<Counter>(entity, Counter{});
				store.Set<Step>(entity, Step{});
				systems.Add("advance", Phase::Simulation, Advance);
			});
		}

		engine::core::FrameGraph::SetEnabled(true);
		engine::core::FrameGraph::BeginFrame();
		universe.Tick(1.0f / 60.0f);
		engine::core::FrameGraph::EndFrame();

		RecordedTree recorded;
		const auto &spans = engine::core::FrameGraph::Spans();
		for (size_t index = 0; index < spans.size(); index++) {
			recorded.Names.emplace_back(spans[index].Name);
			if (spans[index].Parent < index) {
				recorded.Edges.emplace_back(spans[spans[index].Parent].Name, spans[index].Name);
			}
		}
		engine::core::FrameGraph::SetEnabled(false);
		return recorded;
	};

	const auto has = [](const std::vector<std::string> &names, std::string_view wanted) {
		return std::find(names.begin(), names.end(), wanted) != names.end();
	};
	const auto hasEdge = [](const RecordedTree &tree, std::string_view parent, std::string_view child) {
		return std::find(
				   tree.Edges.begin(), tree.Edges.end(), std::pair<std::string, std::string>{parent, child}
			   ) != tree.Edges.end();
	};

	// One world: on this thread, so everything it contains keeps its spans and
	// the pool is free for whatever it dispatches.
	const RecordedTree alone = tickWith(1);
	CHECK(has(alone.Names, "worlds (driver)"));
	CHECK_FALSE(has(alone.Names, "worlds (serial)"));
	CHECK_FALSE(has(alone.Names, "worlds (pinned workers)"));

	// Two: the lanes now have something to overlap, which is the case they are
	// for. Their spans arrive as one reported total, because a worker cannot
	// enter the driver's frame graph.
	const RecordedTree pair = tickWith(2);
	CHECK(has(pair.Names, "worlds (pinned workers)"));
	CHECK_FALSE(has(pair.Names, "worlds (serial)"));
	CHECK(hasEdge(pair, "worlds (pinned workers)", "world.branch.0"));
	CHECK(hasEdge(pair, "world.branch.0", "ecs.systems"));
	CHECK(hasEdge(pair, "ecs.systems", "pre-simulation"));
	CHECK(hasEdge(pair, "ecs.systems", "simulation"));
	CHECK(hasEdge(pair, "ecs.systems", "post-simulation"));
	CHECK(hasEdge(pair, "ecs.systems", "pre-render"));
	CHECK(hasEdge(pair, "simulation", "advance"));
}

TEST_CASE("world ticks keep stable assigned workers", "[world]") {
	Pool pool{4};
	const unsigned pinned = Jobs::PinnedWorkerCount();
	if (pinned < 2) {
		SUCCEED("this platform or process affinity exposes fewer than two pinned workers");
		return;
	}

	Universe universe;

	const std::thread::id driver = std::this_thread::get_id();
	const size_t worldCount = pinned * 2 + 1;
	std::vector<std::thread::id> owners(worldCount);
	std::vector<uint8_t> moved(worldCount, 0);

	for (size_t index = 0; index < worldCount; index++) {
		const WorldId id = universe.Create(Named(("world.thread." + std::to_string(index)).c_str()));
		universe.Enter(id, [&owners, &moved, index](Store &store, Scheduler &systems) {
			store.Set<Counter>(store.Create(), Counter{});
			systems.Add("observe", Phase::Simulation, [&owners, &moved, index](Store &) {
				const std::thread::id current = std::this_thread::get_id();
				if (owners[index] == std::thread::id{}) {
					owners[index] = current;
				} else if (owners[index] != current) {
					moved[index] = 1;
				}
			});
		});
	}

	for (int frame = 0; frame < 10; frame++) {
		universe.Tick(1.0f / 60.0f);
	}

	std::set<std::thread::id> distinct;
	for (size_t index = 0; index < worldCount; index++) {
		CHECK(moved[index] == 0);
		CHECK(owners[index] != driver);
		distinct.insert(owners[index]);
	}
	CHECK(distinct.size() == pinned);
}

// --- structural changes from inside a tick --------------------------------

TEST_CASE("creating a world from inside a tick lands at the next barrier", "[world]") {
	// A registry that grew underneath a running batch would move the very
	// worlds the batch is iterating.
	Universe universe;
	const WorldId first = universe.Create(Named("world.spawner"));

	bool spawned = false;
	universe.Enter(first, [&universe, &spawned](Store &store, Scheduler &systems) {
		store.Set<Counter>(store.Create(), Counter{});
		systems.Add("spawn", Phase::Simulation, [&universe, &spawned](Store &) {
			if (!spawned) {
				universe.Create(Named("world.spawned"));
				spawned = true;
			}
		});
	});

	universe.Tick(1.0f / 60.0f);

	REQUIRE(spawned);
	REQUIRE(universe.Count() == 2);
	REQUIRE(universe.Find(Name("world.spawned")).IsValid());
}

TEST_CASE("destroying a world from inside a tick lands at the next barrier", "[world]") {
	Universe universe;
	const WorldId doomed = universe.Create(Named("world.doomed"));
	const WorldId driver = universe.Create(Named("world.driver"));

	universe.Enter(driver, [&universe, doomed](Store &store, Scheduler &systems) {
		store.Set<Counter>(store.Create(), Counter{});
		systems.Add("destroy", Phase::Simulation, [&universe, doomed](Store &) { universe.Destroy(doomed); });
	});

	REQUIRE(universe.Count() == 2);
	universe.Tick(1.0f / 60.0f);
	REQUIRE(universe.Count() == 1);
	REQUIRE_FALSE(universe.NameOf(doomed).IsValid());
}

// --- faults ---------------------------------------------------------------

TEST_CASE("a throwing system faults one world and spares its neighbours", "[world]") {
	// The soft fault. A hard fault takes the host and cannot be caught - see
	// the module's AGENTS.md.
	Pool pool{4};
	Universe universe;

	const WorldId bad = universe.Create(Named("world.bad"));
	universe.Enter(bad, [](Store &store, Scheduler &systems) {
		store.Set<Counter>(store.Create(), Counter{});
		systems.Add("throw", Phase::Simulation, [](Store &) { throw std::runtime_error("a system failed"); });
	});

	std::vector<WorldId> good;
	for (int index = 0; index < 4; index++) {
		const WorldId id = universe.Create(Named(("world.good." + std::to_string(index)).c_str()));
		Populate(universe, id, 5);
		good.push_back(id);
	}

	for (int frame = 0; frame < 10; frame++) {
		universe.Tick(1.0f / 60.0f);
	}

	REQUIRE(universe.StateOf(bad) == WorldState::Faulted);
	REQUIRE(universe.StatisticsOf(bad).Faults >= 1);
	REQUIRE(universe.Statistics().Faulted == 1);

	// The neighbours ran every tick, exactly as if nothing had happened.
	for (const WorldId id : good) {
		REQUIRE(universe.StateOf(id) == WorldState::Active);
		REQUIRE(universe.StatisticsOf(id).Ticks == 10);
	}
}

TEST_CASE("a faulted world stops ticking until it is recovered", "[world]") {
	Universe universe;
	const WorldId id = universe.Create(Named("world.faulty"));

	bool failing = true;
	universe.Enter(id, [&failing](Store &store, Scheduler &systems) {
		store.Set<Counter>(store.Create(), Counter{});
		systems.Add("maybe", Phase::Simulation, [&failing](Store &) {
			if (failing) {
				throw std::runtime_error("still failing");
			}
		});
	});

	universe.Tick(1.0f / 60.0f);
	REQUIRE(universe.StateOf(id) == WorldState::Faulted);

	const uint64_t frozen = universe.StatisticsOf(id).Ticks;
	for (int frame = 0; frame < 10; frame++) {
		universe.Tick(1.0f / 60.0f);
	}
	REQUIRE(universe.StatisticsOf(id).Ticks == frozen);

	failing = false;
	REQUIRE(universe.Recover(id) == WorldStatus::Ok);
	REQUIRE(universe.StateOf(id) == WorldState::Active);

	universe.Tick(1.0f / 60.0f);
	REQUIRE(universe.StatisticsOf(id).Ticks > frozen);
}

TEST_CASE("a world that keeps faulting is held down", "[world]") {
	// Without the cap, a world faulting deterministically would restore and
	// re-fault forever, burning its host's budget while looking alive.
	WorldSettings settings = Named("world.hopeless");
	settings.FaultLimit = 3;

	Universe universe;
	const WorldId id = universe.Create(settings);
	universe.Enter(id, [](Store &store, Scheduler &systems) {
		store.Set<Counter>(store.Create(), Counter{});
		systems.Add("always", Phase::Simulation, [](Store &) { throw std::runtime_error("always fails"); });
	});

	for (int attempt = 0; attempt < 10; attempt++) {
		universe.Tick(1.0f / 60.0f);
		universe.Recover(id);
	}

	REQUIRE(universe.StateOf(id) == WorldState::Faulted);
	REQUIRE(universe.StatisticsOf(id).Faults >= 3);
}

TEST_CASE("SetState cannot clear a fault behind Recover's back", "[world]") {
	// Otherwise the crash-loop cap would never fire.
	Universe universe;
	const WorldId id = universe.Create(Named("world.sneaky"));
	universe.Enter(id, [](Store &store, Scheduler &systems) {
		store.Set<Counter>(store.Create(), Counter{});
		systems.Add("throw", Phase::Simulation, [](Store &) { throw std::runtime_error("x"); });
	});

	universe.Tick(1.0f / 60.0f);
	REQUIRE(universe.StateOf(id) == WorldState::Faulted);

	universe.SetState(id, WorldState::Active);
	REQUIRE(universe.StateOf(id) == WorldState::Faulted);
}

// --- presentation ---------------------------------------------------------

TEST_CASE("presentation runs only the render phase, and only when asked", "[world]") {
	Universe universe;
	const WorldId id = universe.Create(Named("world.presented"));

	size_t simulated = 0;
	size_t presented = 0;
	universe.Enter(id, [&](Store &store, Scheduler &systems) {
		store.Set<Counter>(store.Create(), Counter{});
		systems.Add("sim", Phase::Simulation, [&simulated](Store &) { simulated++; });
		systems.Add("render", Phase::PreRender, [&presented](Store &) { presented++; });
	});

	universe.Tick(1.0f / 60.0f);
	REQUIRE(simulated == 1);
	REQUIRE(presented == 0);

	REQUIRE(universe.Present(id, 1.0f / 60.0f, 0.5f) == WorldStatus::Ok);
	REQUIRE(simulated == 1);
	REQUIRE(presented == 1);

	universe.Enter(id, [](Store &store) { REQUIRE(store.Time().Alpha == 0.5f); });
}

TEST_CASE("presenting a world that does not exist is reported", "[world]") {
	Universe universe;
	REQUIRE(universe.Present(WorldId{7}, 0.016f, 0.0f) == WorldStatus::NoSuchWorld);
}

TEST_CASE("presentation keeps each world on its physical-core lane", "[world][parallel]") {
	Pool pool{4};
	if (Jobs::PinnedWorkerCount() < 2 || engine::parallel::ForceSerialCompute()) {
		SKIP("this platform could not pin two job workers to distinct physical cores");
	}

	Universe universe;
	const WorldId first = universe.Create(Named("world.present.first"));
	const WorldId second = universe.Create(Named("world.present.second"));

	std::thread::id firstTickThread;
	std::thread::id secondTickThread;
	std::thread::id firstPresentationThread;
	std::thread::id secondPresentationThread;
	std::atomic<unsigned> arrived{0};
	std::atomic<bool> release{false};

	const auto install =
		[&](WorldId world, std::thread::id &tickThread, std::thread::id &presentationThread) {
			universe.Enter(world, [&](Store &, Scheduler &systems) {
				systems.Add("record-tick-lane", Phase::Simulation, [&tickThread](Store &) {
					tickThread = std::this_thread::get_id();
				});
				systems.Add(
					"record-presentation-lane",
					Phase::PreRender,
					[&presentationThread, &arrived, &release](Store &) {
						presentationThread = std::this_thread::get_id();
						arrived.fetch_add(1, std::memory_order_release);
						while (!release.load(std::memory_order_acquire)) {
							if (arrived.load(std::memory_order_acquire) == 2) {
								release.store(true, std::memory_order_release);
							}
							std::this_thread::yield();
						}
					}
				);
			});
		};
	install(first, firstTickThread, firstPresentationThread);
	install(second, secondTickThread, secondPresentationThread);
	universe.Tick(1.0f / 60.0f);

	const std::array requests{
		Presentation{first, 1.0f / 60.0f, 0.25f},
		Presentation{second, 1.0f / 60.0f, 0.75f},
	};
	REQUIRE(universe.PresentMany(requests) == 2);
	CHECK(firstPresentationThread == firstTickThread);
	CHECK(secondPresentationThread == secondTickThread);
	CHECK(firstPresentationThread != std::this_thread::get_id());
	CHECK(secondPresentationThread != std::this_thread::get_id());
	CHECK(firstPresentationThread != secondPresentationThread);

	universe.Enter(first, [](Store &store) { CHECK(store.Time().Alpha == 0.25f); });
	universe.Enter(second, [](Store &store) { CHECK(store.Time().Alpha == 0.75f); });
}

TEST_CASE("presentation demand ignores absent, remote, and duplicate worlds", "[world]") {
	Universe universe;
	const WorldId local = universe.Create(Named("world.present.local"));
	const WorldId remote = universe.CreateRemote(Named("world.present.remote"), Name("host.present.remote"));
	CHECK(universe.Present(remote, 0.016f, 0.5f) == WorldStatus::Ok);

	size_t presented = 0;
	universe.Enter(local, [&](Store &, Scheduler &systems) {
		systems.Add("count-presentation", Phase::PreRender, [&](Store &) { presented++; });
	});

	const std::array requests{
		Presentation{local, 0.016f, 0.5f},
		Presentation{local, 0.032f, 1.0f},
		Presentation{remote, 0.016f, 0.5f},
		Presentation{WorldId{999}, 0.016f, 0.5f},
	};
	CHECK(universe.PresentMany(requests) == 1);
	CHECK(presented == 1);
	universe.Enter(local, [](Store &store) { CHECK(store.Time().Alpha == 0.5f); });
}

// --- churn ----------------------------------------------------------------

TEST_CASE("a universe survives random creation, destruction and suspension", "[world][fuzz]") {
	Pool pool{4};
	Universe universe;

	std::vector<WorldId> live;
	size_t created = 0;

	for (uint32_t step = 0; step < 2'000; step++) {
		const uint32_t roll = Random::Bits(step, 71) % 100;

		if (roll < 40 || live.empty()) {
			const WorldId id = universe.Create(Named(("world.churn." + std::to_string(created++)).c_str()));
			if (id.IsValid()) {
				Populate(universe, id, 3);
				live.push_back(id);
			}
		} else if (roll < 55) {
			const size_t at = Random::Bits(step, 72) % live.size();
			universe.Destroy(live[at]);
			live.erase(live.begin() + static_cast<long>(at));
		} else if (roll < 70) {
			const WorldId id = live[Random::Bits(step, 73) % live.size()];
			universe.SetState(id, WorldState::Suspended);
		} else if (roll < 85) {
			const WorldId id = live[Random::Bits(step, 74) % live.size()];
			universe.SetState(id, WorldState::Active);
		} else {
			universe.Tick(1.0f / 60.0f);
		}

		// The registry never disagrees with itself about what it holds.
		REQUIRE(universe.Count() == live.size());
	}

	// And every world still resolves by name.
	size_t unresolved = 0;
	for (const WorldId id : live) {
		if (universe.Find(universe.NameOf(id)) != id) {
			unresolved++;
		}
	}
	REQUIRE(unresolved == 0);
}

// Change signals, from a world's point of view.
//
// `ecs`'s own suite covers what a signal is. What only a world can show is
// *where* the boundary lands: after the simulation phases, before presentation,
// once per tick - which is the arrangement that lets a render pass still see
// what the tick changed.

namespace universe_test {
	struct Tracked {
		int Value = 0;
	};
}

TEST_CASE("a world fires change signals once a tick, after the simulation", "[world]") {
	Universe universe;
	const WorldId id = universe.Create(Named("signals.world"));

	std::vector<uint64_t> firedAt;
	universe.Enter(id, [&firedAt](Store &store, Scheduler &systems) {
		store.OnChanged<universe_test::Tracked>([&firedAt](
													Store &inner, Entity, const universe_test::Tracked &
												) { firedAt.push_back(inner.Time().Tick); });

		const Entity entity = store.Create();
		store.Set<universe_test::Tracked>(entity, universe_test::Tracked{0});

		systems.Add("bump", Phase::Simulation, [entity](Store &world) {
			world.GetMutable<universe_test::Tracked>(entity)->Value++;
		});
	});

	for (int tick = 0; tick < 5; tick++) {
		universe.Tick(1.0f / 60.0f);
	}

	// One per tick, no more: a write inside a phase does not fire where it
	// happened, and the tick's several phases share one boundary.
	REQUIRE(firedAt.size() == 5);
	REQUIRE(firedAt == std::vector<uint64_t>{1, 2, 3, 4, 5});
}

TEST_CASE("presentation still sees what the tick changed", "[world]") {
	// Changes are cleared at the *start* of a tick rather than the end, because
	// render invalidation runs in PreRender - a separate call, after. Clearing
	// at the end would hand the renderer an empty set every frame.
	Universe universe;
	const WorldId id = universe.Create(Named("signals.present"));

	size_t seenWhileDrawing = 0;
	universe.Enter(id, [&seenWhileDrawing](Store &store, Scheduler &systems) {
		store.Observe<universe_test::Tracked>();

		const Entity entity = store.Create();
		store.Set<universe_test::Tracked>(entity, universe_test::Tracked{0});

		systems.Add("bump", Phase::Simulation, [entity](Store &world) {
			world.GetMutable<universe_test::Tracked>(entity)->Value++;
		});
		systems.Add("draw", Phase::PreRender, [&seenWhileDrawing](Store &world) {
			world.EachChanged<universe_test::Tracked>([&seenWhileDrawing](Entity, universe_test::Tracked &) {
				seenWhileDrawing++;
			});
		});
	});

	universe.Tick(1.0f / 60.0f);
	universe.Present(id, 1.0f / 60.0f, 0.0f);

	REQUIRE(seenWhileDrawing == 1);
}

TEST_CASE("a world with no replication rate publishes on every tick", "[world]") {
	// The behaviour every host had before rates existed, and the one every
	// world in this repository still has.
	Universe universe;
	const WorldId id = universe.Create(Named("replication.default"));
	Populate(universe, id, 1);

	for (int frame = 0; frame < 5; frame++) {
		universe.Tick(1.0f / 60.0f);
		CHECK(universe.TakeReplicationTick(id));
	}

	CHECK(universe.StatisticsOf(id).ReplicationTicks == 5);
}

TEST_CASE("asking twice publishes once", "[world]") {
	// It is consumed, so a host that asks again in the same frame does not
	// build a second delta out of a world that has not moved.
	Universe universe;
	const WorldId id = universe.Create(Named("replication.consumed"));
	Populate(universe, id, 1);

	universe.Tick(1.0f / 60.0f);
	CHECK(universe.TakeReplicationTick(id));
	CHECK_FALSE(universe.TakeReplicationTick(id));
}

TEST_CASE("a world that has not ticked publishes nothing", "[world]") {
	Universe universe;
	const WorldId id = universe.Create(Named("replication.untouched"));
	Populate(universe, id, 1);

	CHECK_FALSE(universe.TakeReplicationTick(id));

	// And an unknown world answers the same way rather than inventing a tick.
	CHECK_FALSE(universe.TakeReplicationTick(WorldId{}));
}

TEST_CASE("a replication rate publishes on some ticks and not others", "[world]") {
	Universe universe;
	WorldSettings settings = Named("replication.slower", 60.0);
	settings.ReplicationTickRate = 20.0;
	const WorldId id = universe.Create(settings);
	Populate(universe, id, 1);

	int published = 0;
	for (int frame = 0; frame < 60; frame++) {
		universe.Tick(1.0f / 60.0f);
		published += universe.TakeReplicationTick(id) ? 1 : 0;
	}

	// A second of ticks at 60 buys twenty snapshots, and the world ran all
	// sixty ticks while doing it.
	CHECK(published == 20);
	CHECK(universe.StatisticsOf(id).Ticks == 60);
	CHECK(universe.StatisticsOf(id).ReplicationTicks == 20);
}

TEST_CASE("a replication rate above the tick rate publishes every tick", "[world]") {
	// **A world cannot publish a tick it has not run.** Asking for more is not
	// an error, and it must not build a debt the world can never pay - an
	// uncapped accumulator would climb for ever and produce this same answer
	// with a number growing behind it.
	Universe universe;
	WorldSettings settings = Named("replication.faster", 60.0);
	settings.ReplicationTickRate = 240.0;
	const WorldId id = universe.Create(settings);
	Populate(universe, id, 1);

	for (int frame = 0; frame < 30; frame++) {
		universe.Tick(1.0f / 60.0f);
		CHECK(universe.TakeReplicationTick(id));
	}
	CHECK(universe.StatisticsOf(id).ReplicationTicks == 30);
}

TEST_CASE("an idle world publishes at its idle rate", "[world]") {
	// The clock is charged in simulated seconds, so a world ticking at 6 Hz
	// cannot publish twenty times a second however loudly it was asked to. A
	// wall clock here would have published twenty snapshots of six ticks.
	Universe universe;
	WorldSettings settings = Named("replication.idle", 60.0);
	settings.IdleTickRate = 6.0;
	settings.ReplicationTickRate = 20.0;
	const WorldId id = universe.Create(settings);
	Populate(universe, id, 1);

	universe.SetState(id, WorldState::Idle);

	int published = 0;
	for (int frame = 0; frame < 60; frame++) {
		universe.Tick(1.0f / 60.0f);
		published += universe.TakeReplicationTick(id) ? 1 : 0;
	}

	CHECK(universe.StatisticsOf(id).Ticks == 6);
	CHECK(published == 6);
}

TEST_CASE("change bits survive the ticks between two published ones", "[world]") {
	// **The reason `World::Tick` does not clear on every tick once a rate is
	// set.** A property written on a tick that is not published would otherwise
	// go into a bitmap nobody ever reads, and the delta built on the next
	// published tick would carry whatever happened to move on that one tick
	// alone.
	Universe universe;
	WorldSettings settings = Named("replication.held", 60.0);
	settings.ReplicationTickRate = 20.0;
	const WorldId id = universe.Create(settings);

	Entity subject = engine::ecs::NULL_ENTITY;
	universe.Enter(id, [&subject](Store &store, Scheduler &systems) {
		store.Observe<universe_test::Tracked>();
		subject = store.Create();
		store.Set<universe_test::Tracked>(subject, universe_test::Tracked{0});

		// Writes on the first tick and never again, which is the shape of a
		// script setting a property once.
		systems.Add("bump-once", Phase::Simulation, [subject](Store &world) {
			if (world.Time().Tick == 1) {
				world.GetMutable<universe_test::Tracked>(subject)->Value++;
			}
		});
	});

	// Three ticks: the write lands on the first and the publish comes round on
	// the third.
	size_t changed = 0;
	for (int frame = 0; frame < 3; frame++) {
		universe.Tick(1.0f / 60.0f);
		if (universe.TakeReplicationTick(id)) {
			universe.Enter(id, [&changed](Store &store) {
				store.EachChanged<universe_test::Tracked>([&changed](Entity, universe_test::Tracked &) {
					changed++;
				});
			});
		}
	}

	CHECK(changed == 1);
}

TEST_CASE("a replication rate that is not a number publishes every tick", "[world]") {
	// The rate arrives from a game file and from a universe snapshot, and
	// `docs/CODE_QUALITY.md` §7 calls both hostile. `1.0 / NaN` compared
	// against an accumulator holds the world on the every-tick path anyway, so
	// the guard is a positive test and the accumulator never sees a NaN.
	Universe universe;
	WorldSettings settings = Named("replication.nan", 60.0);
	settings.ReplicationTickRate = std::numeric_limits<double>::quiet_NaN();
	const WorldId id = universe.Create(settings);
	Populate(universe, id, 1);

	for (int frame = 0; frame < 5; frame++) {
		universe.Tick(1.0f / 60.0f);
		CHECK(universe.TakeReplicationTick(id));
	}
}

TEST_CASE("an unbounded replication rate publishes every tick", "[world]") {
	Universe universe;
	WorldSettings settings = Named("replication.infinite", 60.0);
	settings.ReplicationTickRate = std::numeric_limits<double>::infinity();
	const WorldId id = universe.Create(settings);
	Populate(universe, id, 1);

	for (int frame = 0; frame < 5; frame++) {
		universe.Tick(1.0f / 60.0f);
		CHECK(universe.TakeReplicationTick(id));
	}
	CHECK(universe.StatisticsOf(id).ReplicationTicks == 5);
}

TEST_CASE("reconfiguring a world changes its rates and never its name", "[world]") {
	// **The name is the one field that cannot move.** The registry is keyed on
	// it, `Find` is what everything crossing a boundary uses, and a world whose
	// settings say one thing while the registry says another is reachable under
	// a name nobody can read off it. So `Reconfigure` takes a whole
	// `WorldSettings` and quietly keeps the name it already had.
	Universe universe;
	const WorldId id = universe.Create(Named("world.tuned", 60.0));
	REQUIRE(id.IsValid());

	WorldSettings wanted = universe.SettingsOf(id);
	wanted.Name = Name("world.renamed");
	wanted.TickRate = 30.0;
	wanted.PhysicsTickRate = 10.0;
	wanted.ReplicationTickRate = 20.0;
	wanted.FaultLimit = 9;

	REQUIRE(universe.Reconfigure(id, wanted) == WorldStatus::Ok);

	const WorldSettings applied = universe.SettingsOf(id);
	CHECK(applied.Name == Name("world.tuned"));
	CHECK(applied.TickRate == 30.0);
	CHECK(applied.PhysicsTickRate == 10.0);
	CHECK(applied.ReplicationTickRate == 20.0);
	CHECK(applied.FaultLimit == 9u);

	// Still reachable under the name it was created with, and not under the one
	// that was asked for.
	CHECK(universe.Find(Name("world.tuned")) == id);
	CHECK(!universe.Find(Name("world.renamed")).IsValid());

	CHECK(universe.Reconfigure(WorldId{999}, wanted) == WorldStatus::NoSuchWorld);
}

TEST_CASE("a rate changed between frames takes effect on the next one", "[world]") {
	// **The accumulator is kept rather than rebuilt**, which is what makes this
	// a change of rate and not a skip forward. `World::Owed` re-reads the rate
	// from the settings every frame, so `Reconfigure` writes a number and the
	// next frame is owed against it.
	Universe universe;
	const WorldId id = universe.Create(Named("world.rate", 60.0));
	Populate(universe, id, 1);

	// A tenth of a second at 60 Hz is six ticks.
	universe.Tick(1.0f / 10.0f);
	CHECK(universe.StatisticsOf(id).Ticks == 6);

	WorldSettings slower = universe.SettingsOf(id);
	slower.TickRate = 10.0;
	REQUIRE(universe.Reconfigure(id, slower) == WorldStatus::Ok);

	// The same tenth of a second at 10 Hz is one.
	universe.Tick(1.0f / 10.0f);
	CHECK(universe.StatisticsOf(id).Ticks == 7);
}
