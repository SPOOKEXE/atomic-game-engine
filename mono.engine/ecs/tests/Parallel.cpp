#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.ecs.parallel")
TEST_DEPENDS("engine.ecs.store")
TEST_DEPENDS("engine.parallel.jobs")

// EachBatchParallel hands every slice the index its rows occupy in the whole
// iteration, which is the only thing that makes a packed output array safe to
// fill from several threads at once. These cases are about that index.

using Catch::Approx;
using engine::ecs::Entity;
using engine::ecs::Store;

namespace parallel_test {
	struct Position {
		float X = 0.0f;
	};

	struct Velocity {
		float X = 1.0f;
	};

	struct Marker {
		int Value = 0;
	};

	struct Pool {
		explicit Pool(unsigned workers) {
			engine::parallel::Jobs::Start(workers);
		}
		~Pool() {
			engine::parallel::Jobs::Stop();
		}
	};

	void Fill(Store &store, size_t count) {
		for (size_t index = 0; index < count; index++) {
			const Entity entity = store.Create();
			store.Set<Position>(entity, Position{0.0f});
			store.Set<Velocity>(entity, Velocity{static_cast<float>(index)});
		}
	}
}

using namespace parallel_test;

TEST_CASE("every entity is visited exactly once", "[parallel]") {
	Pool pool{4};
	Store store("test");
	constexpr size_t COUNT = 10'000;
	Fill(store, COUNT);

	std::vector<std::atomic<int>> visits(COUNT);
	std::atomic<size_t> total{0};

	store.EachParallel<const Velocity>([&](Entity, const Velocity &velocity) {
		visits[static_cast<size_t>(velocity.X)].fetch_add(1, std::memory_order_relaxed);
		total.fetch_add(1, std::memory_order_relaxed);
	});

	REQUIRE(total.load() == COUNT);
	for (size_t index = 0; index < COUNT; index++) {
		REQUIRE(visits[index].load() == 1);
	}
}

TEST_CASE("writes through a component reference land", "[parallel]") {
	Pool pool{4};
	Store store("test");
	Fill(store, 5'000);

	store.EachParallel<Position, const Velocity>([](Entity, Position &position, const Velocity &velocity) {
		position.X = velocity.X * 2.0f;
	});

	size_t checked = 0;
	store.Each<const Position, const Velocity>(
		[&](Entity, const Position &position, const Velocity &velocity) {
			REQUIRE(position.X == Approx(velocity.X * 2.0f));
			checked++;
		}
	);

	REQUIRE(checked == 5'000);
}

TEST_CASE("the parallel result equals the serial one", "[parallel]") {
	Pool pool{4};

	// The property that matters. A parallel pass that is faster and different
	// is not an optimisation.
	Store serial("serial");
	Store parallel("parallel");
	Fill(serial, 4'096);
	Fill(parallel, 4'096);

	const auto step = [](Position &position, const Velocity &velocity) {
		position.X += velocity.X * 0.5f + 1.0f;
	};

	for (int tick = 0; tick < 8; tick++) {
		serial.Each<Position, const Velocity>([&](Entity, Position &position, const Velocity &velocity) {
			step(position, velocity);
		});
		parallel.EachParallel<Position, const Velocity>(
			[&](Entity, Position &position, const Velocity &velocity) { step(position, velocity); }
		);
	}

	std::vector<float> serialResults;
	std::vector<float> parallelResults;
	serial.Each<const Position>([&](Entity, const Position &p) { serialResults.push_back(p.X); });
	parallel.Each<const Position>([&](Entity, const Position &p) { parallelResults.push_back(p.X); });

	REQUIRE(serialResults.size() == parallelResults.size());
	for (size_t index = 0; index < serialResults.size(); index++) {
		REQUIRE(serialResults[index] == Approx(parallelResults[index]));
	}
}

TEST_CASE("work actually reaches more than one thread", "[parallel]") {
	Pool pool{4};
	Store store("test");
	Fill(store, 50'000);

	// Retried for the reason Jobs' equivalent is: the calling thread drains
	// ranges too, so one dispatch staying on one thread is allowed. Failing
	// every attempt is not.
	constexpr int ATTEMPTS = 25;
	size_t widest = 0;

	for (int attempt = 0; attempt < ATTEMPTS && widest <= 1; attempt++) {
		std::mutex guard;
		std::set<std::thread::id> threads;

		store.EachParallel<const Position>(
			[&](Entity, const Position &) {
				std::lock_guard lock(guard);
				threads.insert(std::this_thread::get_id());
			},
			512
		);

		widest = std::max(widest, threads.size());
	}

	REQUIRE(widest > 1);
}

TEST_CASE("a small set runs inline rather than paying for a handover", "[parallel]") {
	Pool pool{4};
	Store store("test");
	Fill(store, 8);

	std::mutex guard;
	std::set<std::thread::id> threads;

	store.EachParallel<const Position>(
		[&](Entity, const Position &) {
			std::lock_guard lock(guard);
			threads.insert(std::this_thread::get_id());
		},
		256
	);

	REQUIRE(threads.size() == 1);
	REQUIRE(*threads.begin() == std::this_thread::get_id());
}

TEST_CASE("it works with no job pool at all", "[parallel]") {
	engine::parallel::Jobs::Stop();

	// A unit test, the asset cooker and a headless tool all run without a
	// pool. Every path has to work single-threaded.
	Store store("test");
	Fill(store, 500);

	std::atomic<size_t> visited{0};
	store.EachParallel<const Position>([&](Entity, const Position &) { visited.fetch_add(1); });

	REQUIRE(visited.load() == 500);
}

TEST_CASE("an empty match visits nothing", "[parallel]") {
	Pool pool{2};
	Store store("test");
	Fill(store, 100);

	bool called = false;
	store.EachParallel<const Marker>([&](Entity, const Marker &) { called = true; });

	REQUIRE_FALSE(called);
}

TEST_CASE("entities from several archetypes are all visited", "[parallel]") {
	Pool pool{4};
	Store store("test");

	// Two archetypes: with and without Marker. Tables are walked in order and
	// each is partitioned, so both have to be covered.
	for (size_t index = 0; index < 3'000; index++) {
		const Entity entity = store.Create();
		store.Set<Position>(entity, Position{});
		store.Set<Velocity>(entity, Velocity{1.0f});
		if (index % 3 == 0) {
			store.Set<Marker>(entity, Marker{1});
		}
	}

	std::atomic<size_t> visited{0};
	store.EachParallel<const Position>([&](Entity, const Position &) {
		visited.fetch_add(1, std::memory_order_relaxed);
	});

	REQUIRE(visited.load() == 3'000);
}

TEST_CASE("the entity handed to the body is the right one", "[parallel]") {
	Pool pool{4};
	Store store("test");

	// The entity comes from the iterator's array while the components come
	// from the column, and they are indexed separately. An off-by-one between
	// them would pair every entity with its neighbour's data.
	std::vector<Entity> created;
	for (int index = 0; index < 2'000; index++) {
		const Entity entity = store.Create();
		store.Set<Marker>(entity, Marker{index});
		created.push_back(entity);
	}

	std::mutex guard;
	std::vector<std::pair<Entity, int>> seen;
	store.EachParallel<const Marker>(
		[&](Entity entity, const Marker &marker) {
			std::lock_guard lock(guard);
			seen.emplace_back(entity, marker.Value);
		},
		64
	);

	REQUIRE(seen.size() == 2'000);
	for (const auto &[entity, value] : seen) {
		REQUIRE(created[static_cast<size_t>(value)] == entity);
	}
}

// ---------------------------------------------------------------------------
// EachBatchParallel
// ---------------------------------------------------------------------------

TEST_CASE("a batched parallel slice knows where its rows land", "[parallel]") {
	Pool pool{4};
	Store store("test");
	constexpr size_t COUNT = 10'000;
	Fill(store, COUNT);

	// The whole contract, exercised the way a caller uses it: every slice
	// writes into its own range of one packed array, with no atomic and no
	// locking, and every slot is filled exactly once.
	std::vector<float> output(COUNT, -1.0f);
	const size_t visited = store.EachBatchParallel<const Velocity>(
		[&output](size_t first, size_t rows, const Velocity *velocities) {
			for (size_t row = 0; row < rows; row++) {
				output[first + row] = velocities[row].X;
			}
		}
	);

	REQUIRE(visited == COUNT);
	for (const float value : output) {
		REQUIRE(value >= 0.0f);
	}
}

TEST_CASE("the slices cover the output exactly once between them", "[parallel]") {
	Pool pool{4};
	Store store("test");
	constexpr size_t COUNT = 10'000;
	Fill(store, COUNT);

	// Two slices overlapping is a race that shows up as a torn draw list once
	// in a thousand frames. Counting writes per slot catches it deterministically.
	std::vector<std::atomic<int>> writes(COUNT);
	for (auto &slot : writes) {
		slot.store(0, std::memory_order_relaxed);
	}

	store.EachBatchParallel<const Velocity>([&writes](size_t first, size_t rows, const Velocity *) {
		for (size_t row = 0; row < rows; row++) {
			writes[first + row].fetch_add(1, std::memory_order_relaxed);
		}
	});

	for (const auto &slot : writes) {
		REQUIRE(slot.load(std::memory_order_relaxed) == 1);
	}
}

TEST_CASE("the batched parallel order is the batched serial order", "[parallel]") {
	Pool pool{4};
	Store store("test");
	constexpr size_t COUNT = 5'000;
	Fill(store, COUNT);

	// Determinism is the reason `first` exists rather than an atomic cursor. A
	// draw list that reshuffles between frames is one that cannot be compared
	// between frames, and a recorded run stops replaying.
	std::vector<float> serial;
	serial.reserve(COUNT);
	store.EachBatch<const Velocity>([&serial](size_t rows, const Velocity *velocities) {
		for (size_t row = 0; row < rows; row++) {
			serial.push_back(velocities[row].X);
		}
	});

	std::vector<float> parallel(COUNT, -1.0f);
	store.EachBatchParallel<const Velocity>(
		[&parallel](size_t first, size_t rows, const Velocity *velocities) {
			for (size_t row = 0; row < rows; row++) {
				parallel[first + row] = velocities[row].X;
			}
		}
	);

	REQUIRE(serial == parallel);
}

TEST_CASE("batched parallel spans archetypes without colliding", "[parallel]") {
	Pool pool{4};
	Store store("test");

	// Two tables, so `first` has to carry across the boundary rather than
	// restarting — the bug that silently overwrites the first table's output
	// with the second table's.
	constexpr size_t WITH_MARKER = 3'000;
	constexpr size_t WITHOUT = 4'000;
	for (size_t index = 0; index < WITH_MARKER + WITHOUT; index++) {
		const Entity entity = store.Create();
		store.Set<Velocity>(entity, Velocity{1.0f});
		if (index < WITH_MARKER) {
			store.Set<Marker>(entity, Marker{1});
		}
	}

	std::vector<int> writes(WITH_MARKER + WITHOUT, 0);
	const size_t visited =
		store.EachBatchParallel<const Velocity>([&writes](size_t first, size_t rows, const Velocity *) {
			for (size_t row = 0; row < rows; row++) {
				writes[first + row]++;
			}
		});

	REQUIRE(visited == WITH_MARKER + WITHOUT);
	for (const int count : writes) {
		REQUIRE(count == 1);
	}
}

TEST_CASE("batched parallel visits nothing when nothing matches", "[parallel]") {
	Pool pool{4};
	Store store("test");

	size_t calls = 0;
	const size_t visited =
		store.EachBatchParallel<const Marker>([&calls](size_t, size_t, const Marker *) { calls++; });

	REQUIRE(visited == 0);
	REQUIRE(calls == 0);
}

TEST_CASE("batched parallel works with no job pool at all", "[parallel]") {
	Store store("test");
	constexpr size_t COUNT = 1'000;
	Fill(store, COUNT);

	// Jobs::For runs inline when there is nobody to hand work to. A headless
	// test run and a server that never started the pool are the same case.
	std::vector<float> output(COUNT, -1.0f);
	const size_t visited = store.EachBatchParallel<const Velocity>(
		[&output](size_t first, size_t rows, const Velocity *velocities) {
			for (size_t row = 0; row < rows; row++) {
				output[first + row] = velocities[row].X;
			}
		}
	);

	REQUIRE(visited == COUNT);
	REQUIRE(output.front() >= 0.0f);
	REQUIRE(output.back() >= 0.0f);
}
