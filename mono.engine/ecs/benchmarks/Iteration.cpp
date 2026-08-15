// What a tick actually spends its time on.
//
// **These are the numbers `docs/CODE_QUALITY.md` asks for.** The rule there is
// that an algorithm change carries a measurement; this is where the measurement
// comes from, so that "faster" is a figure rather than an adjective.
//
// The entity counts are chosen to sit either side of the decisions already made
// in the code, so the day somebody changes the grain this suite says whether
// they were right.
//
// **Two different questions live here and they are easy to confuse.** The
// `EachParallel · N` rows call the path the way a system calls it, so below
// `DEFAULT_GRAIN * MINIMUM_GRAINS` they measure the *inline* path and say
// nothing at all about the pool. The `dispatched` ladder further down passes a
// grain small enough to force a handover, which is the only way to ask where
// the pool starts paying for itself.

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

	// The grain the `dispatched` ladder passes, and the reason it can.
	//
	// `Jobs::For` runs a span inline below `grain * MINIMUM_GRAINS`, so the
	// default of 4096 refuses to dispatch anything under 32 768 rows and no
	// count below that can be measured through the pool at all. 1024 puts the
	// floor at 8192, which is where the ladder starts - so the smallest rung is
	// exactly the marginal case `MINIMUM_GRAINS` describes: eight grains, one
	// per range, and the whole wake cost paid to hand out eight of them.
	//
	// It also flatters the pool, deliberately. A grain of 4096 at 16k rows
	// would give four ranges over twenty-three workers; 1024 gives sixteen. So
	// where this ladder says parallel still loses, no grain would have saved it.
	constexpr size_t DISPATCHED_GRAIN = 1024;

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
	// two twelve-byte components is 240 KB - it fits in L2, so the loop is issue
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

// --- the parallel form, as a system calls it ---------------------------------
//
// **These three write one field where every `Each` row above writes three, and
// that is not a fair pair.** Kept as they are because `docs/DEFERRED.md`
// D00012 quotes their numbers, and a benchmark renamed or rebodied loses its
// baseline and with it the ability to reproduce what was claimed. The control
// directly below is the row to read `EachParallel · 10k` against; the
// `dispatched` ladder is the one to read for the crossover.

BENCH("Each · one field · 10k entities", 100) {
	// The serial twin of `EachParallel · 10k entities`, body for body.
	//
	// **It exists because that row went 17.8% slower when the build moved to
	// -O3 and the obvious reading of that is wrong.** 10k rows is under the
	// dispatch floor, so no pool is involved and nothing about the job system
	// changed; what changed is what the optimiser does with a body that writes
	// one float of a twelve-byte row. Three adds are a contiguous span the
	// vectoriser takes cleanly. One add is a stride-12 read-modify-write, and
	// -O3 vectorises it into gathers that cost more than the scalar loop -O2
	// emitted. This row and that one move together; neither moves with `Each`.
	Store &store = WorldOf(10'000, false);
	for (int pass = 0; pass < 100; pass++) {
		store.Each<Position, const Velocity>([](Entity, Position &position, const Velocity &velocity) {
			position.X += velocity.X;
		});
	}
}

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

// --- the dispatch crossover ---------------------------------------------------
//
// **Where the pool starts paying for itself, which is the number every grain
// constant is a guess at.** Serial and dispatched at the same four counts, the
// same three-add body on both sides, so the pair at each rung is the whole
// answer for that count: the first rung where the dispatched row is smaller is
// the crossover.
//
// Powers of two from 8192, because 8192 is the smallest count `DISPATCHED_GRAIN`
// can hand to the pool at all. The three rungs below 32768 pass that grain
// because the default refuses to dispatch there at all; 64k and up use the
// default, which is both what a system gets and the better setting once it is
// available. The existing 500k rows are the top of the same ladder.

BENCH("Each · 8k entities", 200) {
	Store &store = WorldOf(8'192, false);
	for (int pass = 0; pass < 200; pass++) {
		store.Each<Position, const Velocity>([](Entity, Position &position, const Velocity &velocity) {
			position.X += velocity.X;
			position.Y += velocity.Y;
			position.Z += velocity.Z;
		});
	}
}

BENCH("EachParallel dispatched · 8k entities", 200) {
	Store &store = WorldOf(8'192, false);
	for (int pass = 0; pass < 200; pass++) {
		store.EachParallel<Position, const Velocity>(
			[](Entity, Position &position, const Velocity &velocity) {
				position.X += velocity.X;
				position.Y += velocity.Y;
				position.Z += velocity.Z;
			},
			DISPATCHED_GRAIN
		);
	}
}

BENCH("Each · 16k entities", 100) {
	Store &store = WorldOf(16'384, false);
	for (int pass = 0; pass < 100; pass++) {
		store.Each<Position, const Velocity>([](Entity, Position &position, const Velocity &velocity) {
			position.X += velocity.X;
			position.Y += velocity.Y;
			position.Z += velocity.Z;
		});
	}
}

BENCH("EachParallel dispatched · 16k entities", 100) {
	Store &store = WorldOf(16'384, false);
	for (int pass = 0; pass < 100; pass++) {
		store.EachParallel<Position, const Velocity>(
			[](Entity, Position &position, const Velocity &velocity) {
				position.X += velocity.X;
				position.Y += velocity.Y;
				position.Z += velocity.Z;
			},
			DISPATCHED_GRAIN
		);
	}
}

BENCH("Each · 32k entities", 50) {
	Store &store = WorldOf(32'768, false);
	for (int pass = 0; pass < 50; pass++) {
		store.Each<Position, const Velocity>([](Entity, Position &position, const Velocity &velocity) {
			position.X += velocity.X;
			position.Y += velocity.Y;
			position.Z += velocity.Z;
		});
	}
}

BENCH("EachParallel dispatched · 32k entities", 50) {
	Store &store = WorldOf(32'768, false);
	for (int pass = 0; pass < 50; pass++) {
		store.EachParallel<Position, const Velocity>(
			[](Entity, Position &position, const Velocity &velocity) {
				position.X += velocity.X;
				position.Y += velocity.Y;
				position.Z += velocity.Z;
			},
			DISPATCHED_GRAIN
		);
	}
}

BENCH("Each · 64k entities", 25) {
	Store &store = WorldOf(65'536, false);
	for (int pass = 0; pass < 25; pass++) {
		store.Each<Position, const Velocity>([](Entity, Position &position, const Velocity &velocity) {
			position.X += velocity.X;
			position.Y += velocity.Y;
			position.Z += velocity.Z;
		});
	}
}

BENCH("EachParallel · 64k entities", 25) {
	// **The ladder switches to the default grain here and stays on it.** 64k is
	// the first rung above `DEFAULT_GRAIN * MINIMUM_GRAINS`, so it is the first
	// one a system reaches the pool at without asking, and the default is also
	// the better setting from here up: a range costs about sixty-five
	// nanoseconds to hand out whatever is in it, so 4096-row ranges pay that
	// four times less often than 1024-row ones.
	Store &store = WorldOf(65'536, false);
	for (int pass = 0; pass < 25; pass++) {
		store.EachParallel<Position, const Velocity>(
			[](Entity, Position &position, const Velocity &velocity) {
				position.X += velocity.X;
				position.Y += velocity.Y;
				position.Z += velocity.Z;
			}
		);
	}
}

BENCH("Each · 128k entities", 10) {
	Store &store = WorldOf(131'072, false);
	for (int pass = 0; pass < 10; pass++) {
		store.Each<Position, const Velocity>([](Entity, Position &position, const Velocity &velocity) {
			position.X += velocity.X;
			position.Y += velocity.Y;
			position.Z += velocity.Z;
		});
	}
}

BENCH("EachParallel · 128k entities", 10) {
	Store &store = WorldOf(131'072, false);
	for (int pass = 0; pass < 10; pass++) {
		store.EachParallel<Position, const Velocity>(
			[](Entity, Position &position, const Velocity &velocity) {
				position.X += velocity.X;
				position.Y += velocity.Y;
				position.Z += velocity.Z;
			}
		);
	}
}

BENCH("Each · 256k entities", 5) {
	Store &store = WorldOf(262'144, false);
	for (int pass = 0; pass < 5; pass++) {
		store.Each<Position, const Velocity>([](Entity, Position &position, const Velocity &velocity) {
			position.X += velocity.X;
			position.Y += velocity.Y;
			position.Z += velocity.Z;
		});
	}
}

BENCH("EachParallel · 256k entities", 5) {
	Store &store = WorldOf(262'144, false);
	for (int pass = 0; pass < 5; pass++) {
		store.EachParallel<Position, const Velocity>(
			[](Entity, Position &position, const Velocity &velocity) {
				position.X += velocity.X;
				position.Y += velocity.Y;
				position.Z += velocity.Z;
			}
		);
	}
}

// --- the same crossover for the batched pair ----------------------------------
//
// Asked separately because the batched body is the one a compiler vectorises,
// and a faster serial side moves the crossover up on its own. If the two
// ladders disagree, one `DEFAULT_GRAIN` is already serving two different
// answers inside one module.

BENCH("EachBatch · 8k entities", 200) {
	Store &store = WorldOf(8'192, false);
	for (int pass = 0; pass < 200; pass++) {
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

BENCH("EachBatchParallel dispatched · 8k entities", 200) {
	Store &store = WorldOf(8'192, false);
	for (int pass = 0; pass < 200; pass++) {
		Consume(store.EachBatchParallel<Position, const Velocity>(
			[](size_t, size_t rows, Position *position, const Velocity *velocity) {
				for (size_t row = 0; row < rows; row++) {
					position[row].X += velocity[row].X;
					position[row].Y += velocity[row].Y;
					position[row].Z += velocity[row].Z;
				}
			},
			DISPATCHED_GRAIN
		));
	}
}

BENCH("EachBatch · 16k entities", 100) {
	Store &store = WorldOf(16'384, false);
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

BENCH("EachBatchParallel dispatched · 16k entities", 100) {
	Store &store = WorldOf(16'384, false);
	for (int pass = 0; pass < 100; pass++) {
		Consume(store.EachBatchParallel<Position, const Velocity>(
			[](size_t, size_t rows, Position *position, const Velocity *velocity) {
				for (size_t row = 0; row < rows; row++) {
					position[row].X += velocity[row].X;
					position[row].Y += velocity[row].Y;
					position[row].Z += velocity[row].Z;
				}
			},
			DISPATCHED_GRAIN
		));
	}
}

BENCH("EachBatch · 32k entities", 50) {
	Store &store = WorldOf(32'768, false);
	for (int pass = 0; pass < 50; pass++) {
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

BENCH("EachBatchParallel dispatched · 32k entities", 50) {
	Store &store = WorldOf(32'768, false);
	for (int pass = 0; pass < 50; pass++) {
		Consume(store.EachBatchParallel<Position, const Velocity>(
			[](size_t, size_t rows, Position *position, const Velocity *velocity) {
				for (size_t row = 0; row < rows; row++) {
					position[row].X += velocity[row].X;
					position[row].Y += velocity[row].Y;
					position[row].Z += velocity[row].Z;
				}
			},
			DISPATCHED_GRAIN
		));
	}
}

BENCH("EachBatch · 64k entities", 25) {
	Store &store = WorldOf(65'536, false);
	for (int pass = 0; pass < 25; pass++) {
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

BENCH("EachBatchParallel · 64k entities", 25) {
	// The default grain from here up, for the reason `EachParallel · 64k` gives.
	Store &store = WorldOf(65'536, false);
	for (int pass = 0; pass < 25; pass++) {
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

BENCH("EachBatch · 128k entities", 10) {
	Store &store = WorldOf(131'072, false);
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

BENCH("EachBatchParallel · 128k entities", 10) {
	Store &store = WorldOf(131'072, false);
	for (int pass = 0; pass < 10; pass++) {
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

BENCH("EachBatch · 256k entities", 5) {
	Store &store = WorldOf(262'144, false);
	for (int pass = 0; pass < 5; pass++) {
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

BENCH("EachBatchParallel · 256k entities", 5) {
	Store &store = WorldOf(262'144, false);
	for (int pass = 0; pass < 5; pass++) {
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
