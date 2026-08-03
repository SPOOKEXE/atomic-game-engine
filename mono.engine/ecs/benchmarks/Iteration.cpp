// What a tick actually spends its time on.
//
// **These are the numbers `docs/CODE_QUALITY.md` asks for.** The rule there is
// that an algorithm change carries a measurement; this is where the measurement
// comes from, so that "faster" is a figure rather than an adjective.
//
// The entity counts are chosen to sit either side of the decisions already made
// in the code. `EachParallel`'s default grain is 4096 because parallel
// iteration measured *slower* than serial below roughly 60k entities — so the
// counts here bracket that, and the day somebody changes the grain this suite
// says whether they were right.

#include <engine/core/Random.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Bench.hpp>

#include <vector>

TEST_SUITE_ID("engine.ecs.bench.iteration")

using engine::core::Random;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::testing::Consume;

namespace iteration_bench {
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
	struct Tag {
		uint32_t Value = 0;
	};

	// One world, built once and reused by every benchmark that wants that size.
	//
	// Built lazily rather than at static-initialisation time: a store binds its
	// owning thread on construction, and the thread that constructs a namespace
	// static is not necessarily the one that runs the body.
	Store &WorldOf(size_t entities, bool mixed) {
		static std::vector<std::pair<std::pair<size_t, bool>, std::unique_ptr<Store>>> built;

		for (auto &[key, store] : built) {
			if (key.first == entities && key.second == mixed) {
				return *store;
			}
		}

		auto store = std::make_unique<Store>("bench");
		for (size_t index = 0; index < entities; index++) {
			const Entity entity = store->Create();
			store->Set<Position>(entity, Position{static_cast<float>(index), 0.0f, 0.0f});
			store->Set<Velocity>(entity, Velocity{1.0f, 2.0f, 3.0f});

			// Half the rows in a second archetype, so a query has more than one
			// table to walk. A single-table world measures the inner loop and
			// nothing about the planner.
			if (mixed && index % 2 == 0) {
				store->Set<Tag>(entity, Tag{static_cast<uint32_t>(index)});
			}
		}

		built.emplace_back(std::make_pair(entities, mixed), std::move(store));
		return *built.back().second;
	}

	// Started once for the whole binary. Every parallel benchmark needs it, and
	// starting a pool inside a measured body would measure the pool.
	struct Pool {
		Pool() {
			engine::parallel::Jobs::Start(0);
		}
		~Pool() {
			engine::parallel::Jobs::Stop();
		}
	};
	const Pool Workers;
}

using namespace iteration_bench;

// --- the floor -----------------------------------------------------------------
//
// Two flat arrays and the same arithmetic, with no query, no table walk and no
// per-row dispatch. **Nothing here can be made faster by changing the ECS**, so
// the gap between this and `Each` at the same size is what the ECS costs, and
// this number is what the machine costs.
//
// Without it "500k takes 200 microseconds" is unreadable: it could be a
// hardware limit or it could be ten times more overhead than work, and those
// want completely different responses.

BENCH("control · raw arrays, 500k", 2) {
	static std::vector<Position> positions(500'000);
	static std::vector<Velocity> velocities(500'000, Velocity{1.0f, 2.0f, 3.0f});

	for (int pass = 0; pass < 2; pass++) {
		for (size_t index = 0; index < positions.size(); index++) {
			positions[index].X += velocities[index].X;
			positions[index].Y += velocities[index].Y;
			positions[index].Z += velocities[index].Z;
		}
		Consume(positions[0].X);
	}
}

BENCH("control · raw arrays, 100k", 10) {
	static std::vector<Position> positions(100'000);
	static std::vector<Velocity> velocities(100'000, Velocity{1.0f, 2.0f, 3.0f});

	for (int pass = 0; pass < 10; pass++) {
		for (size_t index = 0; index < positions.size(); index++) {
			positions[index].X += velocities[index].X;
			positions[index].Y += velocities[index].Y;
			positions[index].Z += velocities[index].Z;
		}
		Consume(positions[0].X);
	}
}

// --- the inner loop, at three sizes ------------------------------------------

BENCH("Each · 10k entities", 100) {
	Store &store = WorldOf(10'000, false);
	for (int pass = 0; pass < 100; pass++) {
		store.Each<Position, const Velocity>([](Entity, Position &position, const Velocity &velocity) {
			position.X += velocity.X;
			position.Y += velocity.Y;
			position.Z += velocity.Z;
		});
	}
}

BENCH("Each · 100k entities", 10) {
	Store &store = WorldOf(100'000, false);
	for (int pass = 0; pass < 10; pass++) {
		store.Each<Position, const Velocity>([](Entity, Position &position, const Velocity &velocity) {
			position.X += velocity.X;
			position.Y += velocity.Y;
			position.Z += velocity.Z;
		});
	}
}

BENCH("Each · 500k entities", 2) {
	Store &store = WorldOf(500'000, false);
	for (int pass = 0; pass < 2; pass++) {
		store.Each<Position, const Velocity>([](Entity, Position &position, const Velocity &velocity) {
			position.X += velocity.X;
			position.Y += velocity.Y;
			position.Z += velocity.Z;
		});
	}
}

// --- the batched form, which is the one a compiler can vectorise -------------
//
// `EachBatch` hands out arrays rather than one row at a time. Whether that is
// worth anything is exactly the question this pair answers, and it is the
// reason the batched path exists at all.

BENCH("EachBatch · 10k entities", 100) {
	// The size where a change to the *layout* is most likely to show. 10k rows of
	// two twelve-byte components is 240 KB — it fits in L2, so the loop is issue
	// bound rather than bandwidth bound, and that is the regime where a stride a
	// compiler can vectorise is worth anything at all. At 500k the same loop is
	// streaming from DRAM and a wider row is simply more bytes to move.
	Store &store = WorldOf(10'000, false);
	for (int pass = 0; pass < 100; pass++) {
		store.EachBatch<Position, const Velocity>(
			[](size_t rows, Position *position, const Velocity *velocity) {
				for (size_t row = 0; row < rows; row++) {
					position[row].X += velocity[row].X;
					position[row].Y += velocity[row].Y;
					position[row].Z += velocity[row].Z;
				}
			}
		);
	}
}

BENCH("EachBatch · 100k entities", 10) {
	Store &store = WorldOf(100'000, false);
	for (int pass = 0; pass < 10; pass++) {
		store.EachBatch<Position, const Velocity>(
			[](size_t rows, Position *position, const Velocity *velocity) {
				for (size_t row = 0; row < rows; row++) {
					position[row].X += velocity[row].X;
					position[row].Y += velocity[row].Y;
					position[row].Z += velocity[row].Z;
				}
			}
		);
	}
}

BENCH("EachBatch · 500k entities", 2) {
	Store &store = WorldOf(500'000, false);
	for (int pass = 0; pass < 2; pass++) {
		store.EachBatch<Position, const Velocity>(
			[](size_t rows, Position *position, const Velocity *velocity) {
				for (size_t row = 0; row < rows; row++) {
					position[row].X += velocity[row].X;
					position[row].Y += velocity[row].Y;
					position[row].Z += velocity[row].Z;
				}
			}
		);
	}
}

// --- the parallel form, above and below the grain ----------------------------
//
// The default grain is 4096 because parallel measured slower than serial below
// roughly 60k entities. These two counts bracket that, so the pair says whether
// that is still true — on this machine, on this compiler, today.

BENCH("EachParallel · 10k entities", 100) {
	Store &store = WorldOf(10'000, false);
	for (int pass = 0; pass < 100; pass++) {
		store.EachParallel<Position, const Velocity>(
			[](Entity, Position &position, const Velocity &velocity) { position.X += velocity.X; }
		);
	}
}

BENCH("EachParallel · 100k entities", 10) {
	// Between the two above, so the pair brackets the crossover rather than
	// merely straddling it. Waking a pool costs the same whatever the work is,
	// so the question this answers is how much work repays it.
	Store &store = WorldOf(100'000, false);
	for (int pass = 0; pass < 10; pass++) {
		store.EachParallel<Position, const Velocity>(
			[](Entity, Position &position, const Velocity &velocity) { position.X += velocity.X; }
		);
	}
}

BENCH("EachParallel · 500k entities", 2) {
	Store &store = WorldOf(500'000, false);
	for (int pass = 0; pass < 2; pass++) {
		store.EachParallel<Position, const Velocity>(
			[](Entity, Position &position, const Velocity &velocity) { position.X += velocity.X; }
		);
	}
}

// --- more than one table -----------------------------------------------------

BENCH("EachBatchParallel · 500k entities", 2) {
	// The form a vectorising compiler can actually use *and* spread across
	// workers: arrays, disjoint by construction, no atomic in the inner loop.
	// What it costs against `EachBatch` at the same size is what the job system
	// is worth on this body.
	Store &store = WorldOf(500'000, false);
	for (int pass = 0; pass < 2; pass++) {
		Consume(store.EachBatchParallel<Position, const Velocity>(
			[](size_t, size_t rows, Position *position, const Velocity *velocity) {
				for (size_t row = 0; row < rows; row++) {
					position[row].X += velocity[row].X;
					position[row].Y += velocity[row].Y;
					position[row].Z += velocity[row].Z;
				}
			}
		));
	}
}

BENCH("Each over two archetypes · 100k entities", 10) {
	Store &store = WorldOf(100'000, true);
	for (int pass = 0; pass < 10; pass++) {
		store.Each<Position, const Velocity>([](Entity, Position &position, const Velocity &velocity) {
			position.X += velocity.X;
		});
	}
}

BENCH("Each with a narrowing term · 100k entities", 10) {
	// Half the rows match. What this costs against the case above is what the
	// planner's table filtering is worth.
	Store &store = WorldOf(100'000, true);
	for (int pass = 0; pass < 10; pass++) {
		store.Each<Position, const Tag>([](Entity, Position &position, const Tag &tag) {
			position.X += static_cast<float>(tag.Value);
		});
	}
}

BENCH("CountMatching · 100k entities", 1000) {
	Store &store = WorldOf(100'000, true);
	for (int pass = 0; pass < 1000; pass++) {
		Consume(store.CountMatching<Position, Velocity>());
	}
}
