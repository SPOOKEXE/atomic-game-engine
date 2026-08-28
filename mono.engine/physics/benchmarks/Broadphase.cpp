// What the broad phase costs per 1000 colliders.
//
// The figure splits in two: `spatial/benchmarks/HashGrid.cpp` holds the half of
// it that belongs to the index - the rebuild alone, over bare
// proxies. This is the other half: what a *world* pays, which is the pass that
// derives every world box from a `Transform` and a `Collider`, the rebuild, and
// then the pair walk over it.
//
// Everything is reported per iteration, so a scene of 1000 colliders and one of
// 8000 are directly comparable and the per-thousand figure is a division
// anybody can do in their head.
//
// The scene is a slab rather than a cube: colliders spread wide on X and Z and
// thin on Y, which is what a world of rooms and floors looks like and is the
// case a uniform grid is either good or bad at. The static/dynamic split is
// four to one, because most of a world does not move - and the whole reason
// there are two indexes is that the static four fifths are not re-measured.
//
// What it measured, in the `bench` preset, on a 24-thread machine. The figures
// are the minimum sample per iteration, with the spread beside them:
//
// | Row, 4000 colliders at 4 m | Cost |
// |---|---|
// | Sync only | 27.8 us ± 2.2 |
// | Sync with the static index rebuilt too | 163 us ± 5 |
// | Pairs only | 194 us ± 9 |
// | Sync + pairs | 232 us ± 45 |
//
// **The second index is worth 135 microseconds a tick at four thousand
// colliders**, which is the whole of its justification and is five times the
// cost of the sync that remains.
//
// Per thousand colliders the total is 16 us at 1000, 58 us at 4000 and 172 us
// at 16000. That climbs because the scene volume is fixed, so the *density*
// rises with the count and the pair walk is quadratic in it - a bigger world
// with the same spacing would not.
//
// ## Cell size, at three densities - v0.11
//
// The note that stood here said this scene would prefer 8 m to the 4 m
// `spatial` chose, and stopped, because one scene at one density is not a
// reason to move another module's default. That measurement was taken:
//
// | Colliders | 2 m | 4 m | 8 m | 16 m |
// |---|---|---|---|---|
// | 1000 | 26.4 us | 15.6 us | **12.9 us** | 13.0 us |
// | 4000 | 517 us | 231 us | **179 us** | 208 us |
// | 16000 | 4443 us | 2651 us | **2544 us** | |
//
// **Eight metres wins at every density, and the minimum is bracketed** - 16 m
// is worse than 8 m at 4000, so these rows are not still walking down a curve.
// Against the 4 m default that is 17% at a thousand colliders, 23% at four
// thousand and 4% at sixteen.
//
// **And the default still should not change**, which is the useful half.
// `spatial/benchmarks/HashGrid.cpp` measures the same grid under the queries it
// exists for, and they disagree with this one: at 4000 colliders `OverlapBox`
// is fastest at 4 m and costs 42% more at 8 m, and a short raycast is fastest
// at 4 m too. Only the rebuild and long raycasts want bigger cells. So 4 m is
// right for the query shapes `spatial` chose it against, 8 m is right for a
// pair walk, and no single number is right for both - which is precisely why
// `PreparePhysicsWorld` takes one.
//
// ## The world measures itself - v0.12
//
// The note that stood here said "the number to pass is 8 m, and there is
// nowhere to pass it from". There is now: a world prepared with no cell size
// calls `spatial::SuggestCellSize` on its own colliders, and the `measured`
// rows are what that picks.
//
// | Colliders | 4 m default | 8 m by hand | measured |
// |---|---|---|---|
// | 1000 | 15.5 us | 12.7 us | **12.8 us** |
// | 4000 | 227 us | 185 us | **178 us** |
// | 16000 | 2.59 ms | 2.46 ms | **2.49 ms** |
//
// **It lands on the hand-picked row at every density**, which is the assertion
// these three exist to make - 22% off the default at four thousand colliders,
// 17% at a thousand, 4% at sixteen. The heuristic is twice the mean widest axis
// quantised to a power of two, and this scene's median collider is two metres
// across, so it chooses 8 m for the reason a person would have.
//
// **An author who names a size still gets it.** `PreparePhysicsWorld(store,
// 4.0f)` is configured and never re-measured - which is what the fixed rows
// above still are, and why they are still here to be compared against.

#include <engine/core/Random.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Components.hpp>
#include <engine/spatial/LayerMask.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.physics.bench.broadphase")

using engine::core::CFrame;
using engine::core::Random;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::physics::BroadPhase;
using engine::physics::PhysicsWorld;
using engine::physics::PreparePhysicsWorld;
using engine::physics::SyncBroadphase;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::Transform;
using engine::spatial::LayerMask;
using engine::testing::Consume;

namespace broadphase_bench {
	// Half the width of the slab the scene fills, in metres.
	constexpr float SCENE_HALF_WIDTH = 128.0f;

	// Half its height. A world is wide and shallow.
	constexpr float SCENE_HALF_HEIGHT = 8.0f;

	// Half the median collider's edge, so the median collider is two metres
	// across - the same scene `spatial/benchmarks/HashGrid.cpp` measures the
	// index against, so the two suites are readable side by side.
	constexpr float MEDIAN_EXTENT = 1.0f;

	// One in five colliders can move. The rest are the world.
	constexpr size_t DYNAMIC_IN = 5;

	// A world of `count` colliders, built once and reused.
	//
	// Deterministic through `core::Random`, which is indexed rather than
	// streamed - so two runs measure the same scene and a difference between
	// them is the code. Lazily rather than at static-initialisation time,
	// because a store binds its owning thread on construction.
	Store &WorldOf(size_t count, float cellSize) {
		static std::vector<std::pair<std::pair<size_t, float>, std::unique_ptr<Store>>> built;

		for (auto &[key, store] : built) {
			if (key.first == count && key.second == cellSize) {
				return *store;
			}
		}

		auto store = std::make_unique<Store>("physics.bench.broadphase");
		PreparePhysicsWorld(*store, cellSize);

		for (size_t index = 0; index < count; index++) {
			const auto seed = static_cast<uint32_t>(index);
			const Vector3 centre{
				Random::Range(seed, 3, -SCENE_HALF_WIDTH, SCENE_HALF_WIDTH),
				Random::Range(seed, 5, -SCENE_HALF_HEIGHT, SCENE_HALF_HEIGHT),
				Random::Range(seed, 7, -SCENE_HALF_WIDTH, SCENE_HALF_WIDTH),
			};

			const Entity entity = store->Create();
			store->Set<Transform>(entity, Transform{CFrame{centre}});

			// Spread around the median rather than all one size, so the index
			// is not measured against colliders that all fit one cell exactly.
			const float extent = MEDIAN_EXTENT * Random::Range(seed, 11, 0.4f, 2.5f);
			Collider collider;
			collider.Extent = Vector3{extent, extent, extent};
			collider.Layer = LayerMask::Only(static_cast<uint32_t>(index % 4));
			collider.Mask = LayerMask::All();
			store->Set<Collider>(entity, collider);

			if (index % DYNAMIC_IN == 0) {
				store->Set<Motion>(entity, Motion{Vector3{1.0f, 0.0f, 0.0f}, Vector3::Zero});
			}
		}

		// One sync before anything is measured, so the static index is already
		// built and the rows below measure the steady state rather than the
		// first tick.
		SyncBroadphase(*store);
		BroadPhase(*store);

		built.emplace_back(std::make_pair(count, cellSize), std::move(store));
		return *built.back().second;
	}

	size_t PairCount(const Store &store) {
		return store.Resource<PhysicsWorld>()->Pairs().size();
	}
}

using namespace broadphase_bench;

// --- the whole per-tick cost, at three sizes ---------------------------------
//
// Sync plus pairs, which is what a world actually pays each tick. Divide by the
// collider count for the figure §3.6 asks for.

BENCH("Sync + pairs · 1000 colliders, 4m cells", 200) {
	Store &store = WorldOf(1000, 4.0f);
	for (int pass = 0; pass < 200; pass++) {
		SyncBroadphase(store);
		BroadPhase(store);
		Consume(PairCount(store));
	}
}

BENCH("Sync + pairs · 4000 colliders, 4m cells", 50) {
	Store &store = WorldOf(4000, 4.0f);
	for (int pass = 0; pass < 50; pass++) {
		SyncBroadphase(store);
		BroadPhase(store);
		Consume(PairCount(store));
	}
}

BENCH("Sync + pairs · 16000 colliders, 4m cells", 20) {
	Store &store = WorldOf(16000, 4.0f);
	for (int pass = 0; pass < 20; pass++) {
		SyncBroadphase(store);
		BroadPhase(store);
		Consume(PairCount(store));
	}
}

// --- the two halves apart ----------------------------------------------------
//
// Which of the two is worth optimising is not obvious from the total: the sync
// is a linear pass and a rebuild, and the pair walk is a query per dynamic
// collider against two indexes plus a sort.

BENCH("Sync only · 4000 colliders, 4m cells", 50) {
	Store &store = WorldOf(4000, 4.0f);
	for (int pass = 0; pass < 50; pass++) {
		SyncBroadphase(store);
		Consume(store.Resource<PhysicsWorld>()->DynamicColliders());
	}
}

BENCH("Pairs only · 4000 colliders, 4m cells", 50) {
	Store &store = WorldOf(4000, 4.0f);
	for (int pass = 0; pass < 50; pass++) {
		BroadPhase(store);
		Consume(PairCount(store));
	}
}

// --- what the static index is worth ------------------------------------------
//
// The same scene with the static set marked dirty every tick, which is what a
// single index would cost - every anchored collider re-measured and re-hashed
// for a world that did not move. The gap between this and "Sync only" above is
// the whole justification for the second grid.

BENCH("Sync with the static index rebuilt too · 4000 colliders", 50) {
	Store &store = WorldOf(4000, 4.0f);
	for (int pass = 0; pass < 50; pass++) {
		store.ResourceMutable<PhysicsWorld>()->MarkStaticDirty();
		SyncBroadphase(store);
		Consume(store.Resource<PhysicsWorld>()->StaticColliders());
	}
}

// --- how the total moves with cell size --------------------------------------
//
// `spatial` chose four metres from the index alone, over bare proxies. This is
// the same question asked by a world: smaller cells cost more to build and
// return fewer candidates to reject, and the pair walk is where the second half
// is paid.

BENCH("Sync + pairs · 4000 colliders, 2m cells", 50) {
	Store &store = WorldOf(4000, 2.0f);
	for (int pass = 0; pass < 50; pass++) {
		SyncBroadphase(store);
		BroadPhase(store);
		Consume(PairCount(store));
	}
}

// **The row the three above exist to be compared against.** A world prepared
// with no cell size measures one from its own colliders - `SuggestCellSize` -
// and this is what that costs against the hand-picked numbers beside it. If it
// does not land on the 8 m row, the heuristic is wrong for the scene the rest of
// this file measures, and that is the thing worth failing on.
BENCH("Sync + pairs · 4000 colliders, measured", 50) {
	Store &store = WorldOf(4000, 0.0f);
	for (int pass = 0; pass < 50; pass++) {
		SyncBroadphase(store);
		BroadPhase(store);
		Consume(PairCount(store));
	}
}

BENCH("Sync + pairs · 1000 colliders, measured", 200) {
	Store &store = WorldOf(1000, 0.0f);
	for (int pass = 0; pass < 200; pass++) {
		SyncBroadphase(store);
		BroadPhase(store);
		Consume(PairCount(store));
	}
}

BENCH("Sync + pairs · 16000 colliders, measured", 10) {
	Store &store = WorldOf(16000, 0.0f);
	for (int pass = 0; pass < 10; pass++) {
		SyncBroadphase(store);
		BroadPhase(store);
		Consume(PairCount(store));
	}
}

BENCH("Sync + pairs · 4000 colliders, 8m cells", 50) {
	Store &store = WorldOf(4000, 8.0f);
	for (int pass = 0; pass < 50; pass++) {
		SyncBroadphase(store);
		BroadPhase(store);
		Consume(PairCount(store));
	}
}

// --- and whether one cell size can be right for every world -------------------
//
// **The measurement the note above asked for and did not have.** The rows at
// 4000 said this scene would prefer 8 m to the 4 m `spatial` chose, and stopped
// there - one scene at one density is not a reason to move a default another
// module measured. So the same ladder is run at a quarter of the count and at
// four times it.
//
// The scene volume is fixed, so collider *density* rises with the count. The
// two halves of the cost move against each other: smaller cells cost more to
// build and hand back fewer candidates to reject, larger cells cost less to
// build and hand back more. Where the two cross is a function of density, so
// the interesting result is not which cell size wins but whether the *same* one
// wins at all three counts.

BENCH("Sync + pairs · 1000 colliders, 2m cells", 200) {
	Store &store = WorldOf(1000, 2.0f);
	for (int pass = 0; pass < 200; pass++) {
		SyncBroadphase(store);
		BroadPhase(store);
		Consume(PairCount(store));
	}
}

BENCH("Sync + pairs · 1000 colliders, 8m cells", 200) {
	Store &store = WorldOf(1000, 8.0f);
	for (int pass = 0; pass < 200; pass++) {
		SyncBroadphase(store);
		BroadPhase(store);
		Consume(PairCount(store));
	}
}

BENCH("Sync + pairs · 1000 colliders, 16m cells", 200) {
	Store &store = WorldOf(1000, 16.0f);
	for (int pass = 0; pass < 200; pass++) {
		SyncBroadphase(store);
		BroadPhase(store);
		Consume(PairCount(store));
	}
}

// **Sixteen metres at 4000 as well**, because if eight beats four the next
// question is whether the curve has turned yet or the rows are still walking
// down it. A minimum that has not been bracketed on both sides is not a
// minimum.
BENCH("Sync + pairs · 4000 colliders, 16m cells", 50) {
	Store &store = WorldOf(4000, 16.0f);
	for (int pass = 0; pass < 50; pass++) {
		SyncBroadphase(store);
		BroadPhase(store);
		Consume(PairCount(store));
	}
}

BENCH("Sync + pairs · 16000 colliders, 2m cells", 20) {
	Store &store = WorldOf(16000, 2.0f);
	for (int pass = 0; pass < 20; pass++) {
		SyncBroadphase(store);
		BroadPhase(store);
		Consume(PairCount(store));
	}
}

BENCH("Sync + pairs · 16000 colliders, 8m cells", 20) {
	Store &store = WorldOf(16000, 8.0f);
	for (int pass = 0; pass < 20; pass++) {
		SyncBroadphase(store);
		BroadPhase(store);
		Consume(PairCount(store));
	}
}
