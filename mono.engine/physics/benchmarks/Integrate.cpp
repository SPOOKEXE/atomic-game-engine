// Where the serial/parallel crossover for `IntegrateMotion` comes from.
//
// **The old number does not carry forward.** `Store::EachParallel`'s header
// records a crossover near sixty to eighty thousand rows, measured over an
// integration step of three float multiply-adds against the previous backing
// store. This body carries a whole `core::CFrame`: a position add, a quaternion
// product and a normalise. `v02v03v04.md` §3.6 asks for the measurement to be
// re-taken for exactly that reason, and this is where it is taken.
//
// The counts bracket the answer rather than land on it, so the day the body
// changes this suite says which side of the crossover it moved to.
//
// Both halves run the same `IntegrateOne` the system runs, out of the module's
// private header. A benchmark with its own copy of the arithmetic measures the
// copy, and the copy is the one that stops being updated.

#include "Integration.hpp"

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.physics.bench.integrate")

using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::physics::INTEGRATE_GRAIN;
using engine::physics::IntegrateMotion;
using engine::physics::IntegrateOne;
using engine::scene::Motion;
using engine::scene::Transform;
using engine::testing::Consume;

namespace integrate_bench {
	// Sixty hertz, the rate the physics is tuned against.
	constexpr float TICK = 1.0f / 60.0f;

	// Started once for the whole binary. Starting a pool inside a measured body
	// would measure the pool.
	struct Pool {
		Pool() {
			engine::parallel::Jobs::Start(0);
		}
		~Pool() {
			engine::parallel::Jobs::Stop();
		}
	};
	const Pool Workers;

	// One world of `entities` moving, tumbling parts, built once and reused.
	//
	// Lazily rather than at static-initialisation time: a store binds its
	// owning thread on construction, and the thread that constructs a namespace
	// static is not necessarily the one that runs the body.
	Store &WorldOf(size_t entities) {
		static std::vector<std::pair<size_t, std::unique_ptr<Store>>> built;

		for (auto &[size, store] : built) {
			if (size == entities) {
				return *store;
			}
		}

		engine::scene::RegisterSceneComponents();

		auto store = std::make_unique<Store>("physics.bench.integrate");
		store->AdvanceTick(TICK);

		for (size_t index = 0; index < entities; index++) {
			const auto offset = static_cast<float>(index);
			const Entity entity = store->Create();
			store->Set<Transform>(entity, Transform{CFrame{Vector3{offset, 0.0f, -offset}}});

			// Every row spins. A world where the angular velocity is zero would
			// measure a quaternion product against a zero vector, which is the
			// same instruction count and a different branch predictor's day —
			// and it would flatter the parallel case for the wrong reason.
			store->Set<Motion>(entity, Motion{Vector3{1.0f, 0.0f, -1.0f}, Vector3{0.3f, 1.1f, -0.7f}});
		}

		built.emplace_back(entities, std::move(store));
		return *built.back().second;
	}

	// The serial half, written against the same per-row step the system uses.
	void Serial(Store &store) {
		const float delta = store.Time().Delta;
		store.Each<Transform, const Motion>([delta](Entity, Transform &transform, const Motion &motion) {
			IntegrateOne(transform, motion, delta);
		});
	}
}

using namespace integrate_bench;

// --- serial, at four sizes ---------------------------------------------------

BENCH("Each · 1000 entities", 500) {
	Store &store = WorldOf(1000);
	for (int pass = 0; pass < 500; pass++) {
		Serial(store);
		Consume(store.Time().Delta);
	}
}

BENCH("Each · 4000 entities", 200) {
	Store &store = WorldOf(4000);
	for (int pass = 0; pass < 200; pass++) {
		Serial(store);
		Consume(store.Time().Delta);
	}
}

BENCH("Each · 20000 entities", 50) {
	Store &store = WorldOf(20000);
	for (int pass = 0; pass < 50; pass++) {
		Serial(store);
		Consume(store.Time().Delta);
	}
}

BENCH("Each · 100000 entities", 20) {
	Store &store = WorldOf(100000);
	for (int pass = 0; pass < 20; pass++) {
		Serial(store);
		Consume(store.Time().Delta);
	}
}

// --- the system, which is EachParallel at the measured grain -----------------
//
// Read these against the rows above. Where the parallel figure is larger, the
// system is paying for a handover it cannot repay, and `INTEGRATE_GRAIN` is
// the knob — it sets both the range size and, through
// `Jobs::MINIMUM_GRAINS`, the count below which the span runs inline anyway.

BENCH("IntegrateMotion · 1000 entities", 500) {
	Store &store = WorldOf(1000);
	for (int pass = 0; pass < 500; pass++) {
		IntegrateMotion(store);
		Consume(store.Time().Delta);
	}
}

BENCH("IntegrateMotion · 4000 entities", 200) {
	Store &store = WorldOf(4000);
	for (int pass = 0; pass < 200; pass++) {
		IntegrateMotion(store);
		Consume(store.Time().Delta);
	}
}

BENCH("IntegrateMotion · 20000 entities", 50) {
	Store &store = WorldOf(20000);
	for (int pass = 0; pass < 50; pass++) {
		IntegrateMotion(store);
		Consume(store.Time().Delta);
	}
}

BENCH("IntegrateMotion · 100000 entities", 20) {
	Store &store = WorldOf(100000);
	for (int pass = 0; pass < 20; pass++) {
		IntegrateMotion(store);
		Consume(store.Time().Delta);
	}
}

// --- what the grain is worth -------------------------------------------------
//
// The default grain is a guess about a body that does almost nothing, and this
// one is not that body. Two rows at 20000 entities, where the choice matters
// most: below `INTEGRATE_GRAIN * Jobs::MINIMUM_GRAINS` the whole span runs
// inline, so the default grain of 4096 refuses to dispatch anything under
// 32768 rows at all.

BENCH("EachParallel at the default grain · 20000 entities", 50) {
	Store &store = WorldOf(20000);
	const float delta = store.Time().Delta;
	for (int pass = 0; pass < 50; pass++) {
		store.EachParallel<Transform, const Motion>([delta](
														Entity, Transform &transform, const Motion &motion
													) { IntegrateOne(transform, motion, delta); });
		Consume(delta);
	}
}

BENCH("EachParallel at the physics grain · 20000 entities", 50) {
	Store &store = WorldOf(20000);
	const float delta = store.Time().Delta;
	for (int pass = 0; pass < 50; pass++) {
		store.EachParallel<Transform, const Motion>(
			[delta](Entity, Transform &transform, const Motion &motion) {
				IntegrateOne(transform, motion, delta);
			},
			INTEGRATE_GRAIN
		);
		Consume(delta);
	}
}
