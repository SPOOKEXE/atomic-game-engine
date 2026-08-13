// What the solver costs per contact, which is the number that decides
// `SOLVER_ITERATIONS`.
//
// `v02v03v04.md` §3.6 asks for exactly this figure. It is the cost side of the
// decision; the accuracy side — how far a stack sinks and whether it settles at
// all — is in `Solver.hpp` beside the constant, because that is where somebody
// changing the number will be looking.
//
// **The iteration count is compiled in**, so the per-iteration column of that
// table is taken by rebuilding this suite with a different `SOLVER_ITERATIONS`
// rather than by a loop here. A runtime knob would be a second way to say what
// the constant already says, and the one that got left switched would be the
// one that shipped.
//
// Every row reports per call to `Solve`. The contact counts the per-contact
// figure divides by are 3520 for 200 stacks of four, 18040 for 800 of four and
// 10688 for 200 of twelve — four points per box-on-box face contact, and
// `tests/Solver.cpp` is where that is pinned rather than assumed.
//
// What it measured, in the `bench` preset, on a 24-thread machine, at the
// sixteen iterations that shipped. Minimum sample, spread beside it:
//
// | Row | Cost | Per contact |
// |---|---|---|
// | 200 stacks of 4 | 1785 us ± 226 | 0.51 us |
// | 800 stacks of 4 | 10058 us ± 916 | 0.56 us |
// | 200 stacks of 12 | 5852 us ± 507 | 0.55 us |
// | 200 stacks of 4, cache emptied | 1619 us ± 114 | 0.46 us |
// | `Publish`, 200 stacks of 4 | 26 us ± 262 | |
//
// **Flat across scene size and across stack height**, which is what makes it a
// per-contact figure worth quoting: a scene's solver budget is a
// multiplication. The larger scenes sit a tenth above the smallest because the
// row array outgrows the cache the sixteen sweeps walk it from, not because a
// contact costs more. `Publish` is two orders of magnitude cheaper because it is
// one pass over the bodies and a merge over the pairs, with no iteration at
// all.
//
// The warm start does not show up in the cost — the cache row is inside the
// spread of the row above it — which is the point: a binary search over a
// sorted array is free next to sixteen sweeps. What it buys is in `Solver.hpp`.

#include <engine/core/Random.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Solver.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/testing/Bench.hpp>

// Private: the impulse cache is what the warm-start row below empties, and
// emptying it is the only way to measure a cold solve without deleting the
// feature to find out what it was worth.
#include "PipelineInternals.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.physics.bench.solver")

using engine::core::CFrame;
using engine::core::Random;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::physics::BroadPhase;
using engine::physics::NarrowPhase;
using engine::physics::PhysicsWorld;
using engine::physics::PipelineInternals;
using engine::physics::PreparePhysicsWorld;
using engine::physics::Publish;
using engine::physics::Solve;
using engine::physics::SyncBroadphase;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::RigidBody;
using engine::scene::ShapeKind;
using engine::scene::Transform;
using engine::testing::Consume;

namespace solver_bench {
	// How many contacts one row of the table is divided by.
	size_t PointCount(const Store &store) {
		size_t points = 0;
		for (const auto &manifold : store.Resource<PhysicsWorld>()->Manifolds()) {
			points += manifold.PointCount;
		}
		return points;
	}

	// Keeps the scene awake for the length of a measurement.
	//
	// **A benchmark that let the solver sleep would measure the sleeping.** The
	// rows below call `Solve` two hundred times without a tick of gravity
	// between them, so after thirty calls every body has been still for half a
	// simulated second and drops out of the solve entirely — and the figure
	// stops moving when `SOLVER_ITERATIONS` changes, which is exactly the
	// signal that says the measurement is measuring nothing.
	void KeepAwake(Store &store) {
		PipelineInternals::Resting(*store.ResourceMutable<PhysicsWorld>()).clear();
	}

	// A grid of boxes resting in stacks, which is the case a solver is
	// actually judged on: every contact carries the weight of the ones above
	// it, so the iteration count is what decides whether the bottom of a stack
	// hears about the top.
	//
	// Deterministic and built lazily, for the reasons the broad-phase suite
	// gives.
	Store &StackedWorld(size_t columns, size_t height) {
		static std::vector<std::pair<std::pair<size_t, size_t>, std::unique_ptr<Store>>> built;
		for (auto &[key, store] : built) {
			if (key.first == columns && key.second == height) {
				return *store;
			}
		}

		auto store = std::make_unique<Store>("physics.bench.solver");
		PreparePhysicsWorld(*store, 4.0f);

		// One wide floor, anchored, so every stack has something to press on.
		const Entity floor = store->Create();
		store->Set<Transform>(floor, Transform{CFrame{Vector3{0.0f, -1.0f, 0.0f}}});
		Collider ground;
		ground.Extent = Vector3{128.0f, 1.0f, 128.0f};
		store->Set<Collider>(floor, ground);

		for (size_t column = 0; column < columns; column++) {
			const auto seed = static_cast<uint32_t>(column);
			const float x = Random::Range(seed, 3, -48.0f, 48.0f);
			const float z = Random::Range(seed, 5, -48.0f, 48.0f);

			for (size_t level = 0; level < height; level++) {
				const Entity entity = store->Create();
				store->Set<Transform>(
					entity, Transform{CFrame{Vector3{x, 0.5f + static_cast<float>(level) * 0.999f, z}}}
				);

				Collider collider;
				collider.Extent = Vector3{0.5f, 0.5f, 0.5f};
				store->Set<Collider>(entity, collider);
				store->Set<Motion>(entity, Motion{});
				store->Set<RigidBody>(entity, RigidBody{});
			}
		}

		// Several ticks before anything is measured, so the warm start is warm
		// and the rows below measure a steady scene rather than its first
		// tick — which is the one tick a game never spends most of its time in.
		for (int tick = 0; tick < 10; tick++) {
			store->AdvanceTick(1.0f / 60.0f);
			SyncBroadphase(*store);
			BroadPhase(*store);
			NarrowPhase(*store);
			Solve(*store);
			Publish(*store);
		}

		built.emplace_back(std::make_pair(columns, height), std::move(store));
		return *built.back().second;
	}
}

using namespace solver_bench;

// --- how many contacts each row below is divided by ---------------------------

BENCH("Contacts · 200 stacks of 4, counted", 1) {
	Store &store = StackedWorld(200, 4);
	Consume(PointCount(store));
}

// --- the number §3.6 asks for -------------------------------------------------

BENCH("Solve · 200 stacks of 4", 200) {
	Store &store = StackedWorld(200, 4);
	for (int pass = 0; pass < 200; pass++) {
		KeepAwake(store);
		Solve(store);
		Consume(PointCount(store));
	}
}

BENCH("Solve · 800 stacks of 4", 50) {
	Store &store = StackedWorld(800, 4);
	for (int pass = 0; pass < 50; pass++) {
		KeepAwake(store);
		Solve(store);
		Consume(PointCount(store));
	}
}

BENCH("Solve · 200 stacks of 12", 100) {
	Store &store = StackedWorld(200, 12);
	for (int pass = 0; pass < 100; pass++) {
		KeepAwake(store);
		Solve(store);
		Consume(PointCount(store));
	}
}

// --- the scale where the answer stops being arithmetic ------------------------
//
// Ten thousand bodies, which is `examples/Cube.luau` and roughly the largest
// pile worth calling a scene. It is here because the rows above cannot see what
// this one measures: at 3520 contacts the whole row array sits in cache and the
// cost is the arithmetic, and at ten thousand bodies it does not and the cost is
// the walk. An optimisation that trades bytes for instructions wins the rows
// above and can lose this one, so both have to be read together.
//
// **This row needs a quiet machine and the ones above do not.** It walks about
// ten megabytes sixteen times, so it is competing for L3 with whatever else is
// running: measured against a loaded machine it wandered between 25 and 39 ms
// for one unchanged binary, where the small rows held to a few per cent. Take
// its figure only from a run with nothing else on the box, and compare
// alternatives by running them alternately rather than one after the other.
//
// Around 25 ms per call for roughly 40000 contact points, which is 0.6 us each
// — a fifth above the small rows, and that gap is the cache rather than the
// arithmetic.

BENCH("Solve · 500 stacks of 20", 10) {
	Store &store = StackedWorld(500, 20);
	for (int pass = 0; pass < 10; pass++) {
		KeepAwake(store);
		Solve(store);
		Consume(PointCount(store));
	}
}

// --- what the warm start is worth ---------------------------------------------
//
// The same solve with the cache emptied first, which is what every tick would
// look like if the impulses were not carried. The gap is not the cost of the
// lookup — that is a binary search over a sorted array — it is the iterations
// the warm start saves by starting the search at the answer.

BENCH("Solve · 200 stacks of 4, cache emptied each tick", 200) {
	Store &store = StackedWorld(200, 4);
	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	for (int pass = 0; pass < 200; pass++) {
		KeepAwake(store);
		PipelineInternals::ImpulseCache(world).clear();
		Solve(store);
		Consume(PointCount(store));
	}
}

// --- the rest of the chain, for context ---------------------------------------
//
// The four steps at pile scale, so the share each one takes is a measurement
// rather than an extrapolation from three differently-shaped scenes. This is
// the breakdown to read before optimising anything: it says which step a big
// world's physics budget is actually in, and the answer is not evenly split.

BENCH("SyncBroadphase · 500 stacks of 20", 10) {
	Store &store = StackedWorld(500, 20);
	for (int pass = 0; pass < 10; pass++) {
		SyncBroadphase(store);
		Consume(store.Resource<PhysicsWorld>()->CellSize());
	}
}

BENCH("BroadPhase · 500 stacks of 20", 10) {
	Store &store = StackedWorld(500, 20);
	for (int pass = 0; pass < 10; pass++) {
		BroadPhase(store);
		Consume(store.Resource<PhysicsWorld>()->Pairs().size());
	}
}

BENCH("NarrowPhase · 500 stacks of 20", 10) {
	Store &store = StackedWorld(500, 20);
	for (int pass = 0; pass < 10; pass++) {
		NarrowPhase(store);
		Consume(PointCount(store));
	}
}

BENCH("Publish · 200 stacks of 4", 200) {
	Store &store = StackedWorld(200, 4);
	for (int pass = 0; pass < 200; pass++) {
		Publish(store);
		Consume(store.Resource<PhysicsWorld>()->Events().size());
	}
}
