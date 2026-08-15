// What the narrow phase costs per pair, by shape combination.
//
// `v02v03v04.md` §3.6 asks for it by name, and by shape combination rather than
// as one number, because the six pairs are not one algorithm: two of them are a
// handful of subtractions, one is a fifteen-axis search with a polygon clip, and
// the cylinder pairs add a longer axis list on top of the same clip. A single
// average over a mixed scene would hide the one that matters.
//
// Each row measures **one call to the pair function** - no store, no index, no
// broad phase - so the number is the geometry and nothing else. The scene rows
// at the end put it back in context.
//
// What it measured, in the `bench` preset, on a 24-thread machine. Minimum
// sample per call, with the spread beside it:
//
// | Pair | Touching | Separated |
// |---|---|---|
// | sphere against sphere | 15 ns ± 0 | |
// | sphere against cylinder | 46 ns ± 0 | |
// | box against sphere | 56 ns ± 4 | |
// | cylinder against cylinder | 163 ns ± 14 | 72 ns ± 58 |
// | box against box | 295 ns ± 155 | 67 ns ± 18 |
// | box against cylinder | 389 ns ± 176 | |
//
// **The two analytic pairs are twenty times cheaper than the two that clip**,
// which is the shape to expect: a sphere against anything is a closest point
// and a subtraction, and everything else is an axis search followed by a
// polygon clip. Box-cylinder is the most expensive of the six because its axis
// list is twenty-three long against box-box's fifteen - the price of a shape
// with no faces to enumerate.
//
// Saying no is three to four times cheaper than saying yes, because a
// separating axis ends the search and no manifold is built. That is the common
// case: a broad phase hands over boxes that overlap, and most of the shapes
// inside them do not.
//
// **`ShapeInstance` resolving its frame axes once is what these numbers are
// standing on.** The axis search asks for a projection radius twice per
// candidate, and a `CFrame` holds a quaternion - so deriving the three world
// axes inside that question made box-box rotate the same six vectors ninety
// times per pair. It was a third of the cost of every row that clips.
//
// In a world, over colliders every third one of which is a different shape:
// 187 us ± 57 for 2000 and 3140 us ± 516 for 8000. That climbs faster than the
// count because the scene volume is fixed, so density rises with it and the
// pair count is quadratic in density - the same reason the broad-phase suite
// beside this one climbs.

#include <engine/core/Random.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/testing/Bench.hpp>

// Private: the point of this suite is the pair functions themselves, and they
// are `src/`'s. Measuring them through the store would measure the store.
#include "ContactPairs.hpp"
#include "ShapeSupport.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.physics.bench.narrowphase")

using engine::core::CFrame;
using engine::core::Random;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::physics::BroadPhase;
using engine::physics::ContactBetween;
using engine::physics::NarrowPhase;
using engine::physics::PhysicsWorld;
using engine::physics::PreparePhysicsWorld;
using engine::physics::ShapeInstance;
using engine::physics::SyncBroadphase;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::ShapeKind;
using engine::scene::Transform;
using engine::testing::Consume;

namespace narrowphase_bench {
	// A quarter turn about two axes, so no pair is measured in the
	// axis-aligned case a real scene almost never presents.
	const CFrame TILTED = CFrame::Angles(0.35f, 0.6f, 0.0f);

	ShapeInstance Of(ShapeKind shape, const Vector3 &position, const Vector3 &extent, bool tilted) {
		return ShapeInstance{
			CFrame{position, tilted ? TILTED.Rotation() : CFrame{}.Rotation()}, extent, shape
		};
	}

	// Overlapping by a tenth of a metre along X, which is the resting-contact
	// depth a real scene spends its time in.
	constexpr float TOUCHING = 0.9f;

	const Vector3 UNIT_BOX{0.5f, 0.5f, 0.5f};
	const Vector3 UNIT_SPHERE{0.5f, 0.0f, 0.0f};
	const Vector3 UNIT_CYLINDER{0.5f, 0.5f, 0.0f};
}

using namespace narrowphase_bench;

// --- one pair at a time -------------------------------------------------------

BENCH("Pair · box against box, touching", 20000) {
	const ShapeInstance first = Of(ShapeKind::Box, Vector3::Zero, UNIT_BOX, true);
	const ShapeInstance second = Of(ShapeKind::Box, Vector3{TOUCHING, 0.0f, 0.0f}, UNIT_BOX, false);
	for (int pass = 0; pass < 20000; pass++) {
		Consume(ContactBetween(first, second).PointCount);
	}
}

BENCH("Pair · box against sphere, touching", 20000) {
	const ShapeInstance first = Of(ShapeKind::Box, Vector3::Zero, UNIT_BOX, true);
	const ShapeInstance second = Of(ShapeKind::Sphere, Vector3{TOUCHING, 0.0f, 0.0f}, UNIT_SPHERE, false);
	for (int pass = 0; pass < 20000; pass++) {
		Consume(ContactBetween(first, second).PointCount);
	}
}

BENCH("Pair · box against cylinder, touching", 20000) {
	const ShapeInstance first = Of(ShapeKind::Box, Vector3::Zero, UNIT_BOX, true);
	const ShapeInstance second = Of(ShapeKind::Cylinder, Vector3{TOUCHING, 0.0f, 0.0f}, UNIT_CYLINDER, false);
	for (int pass = 0; pass < 20000; pass++) {
		Consume(ContactBetween(first, second).PointCount);
	}
}

BENCH("Pair · sphere against sphere, touching", 20000) {
	const ShapeInstance first = Of(ShapeKind::Sphere, Vector3::Zero, UNIT_SPHERE, false);
	const ShapeInstance second = Of(ShapeKind::Sphere, Vector3{TOUCHING, 0.0f, 0.0f}, UNIT_SPHERE, false);
	for (int pass = 0; pass < 20000; pass++) {
		Consume(ContactBetween(first, second).PointCount);
	}
}

BENCH("Pair · sphere against cylinder, touching", 20000) {
	const ShapeInstance first = Of(ShapeKind::Sphere, Vector3::Zero, UNIT_SPHERE, false);
	const ShapeInstance second = Of(ShapeKind::Cylinder, Vector3{TOUCHING, 0.0f, 0.0f}, UNIT_CYLINDER, true);
	for (int pass = 0; pass < 20000; pass++) {
		Consume(ContactBetween(first, second).PointCount);
	}
}

BENCH("Pair · cylinder against cylinder, touching", 20000) {
	const ShapeInstance first = Of(ShapeKind::Cylinder, Vector3::Zero, UNIT_CYLINDER, true);
	const ShapeInstance second = Of(ShapeKind::Cylinder, Vector3{TOUCHING, 0.0f, 0.0f}, UNIT_CYLINDER, false);
	for (int pass = 0; pass < 20000; pass++) {
		Consume(ContactBetween(first, second).PointCount);
	}
}

// --- the rejection path -------------------------------------------------------
//
// Most candidate pairs are boxes that overlap and shapes that do not, so the
// cost of saying no is the one a busy scene pays most often. It should be
// cheaper than saying yes: a separating axis ends the search and no manifold is
// built.

BENCH("Pair · box against box, separated", 20000) {
	const ShapeInstance first = Of(ShapeKind::Box, Vector3::Zero, UNIT_BOX, true);
	const ShapeInstance second = Of(ShapeKind::Box, Vector3{1.4f, 0.0f, 0.0f}, UNIT_BOX, false);
	for (int pass = 0; pass < 20000; pass++) {
		Consume(ContactBetween(first, second).Touching);
	}
}

BENCH("Pair · cylinder against cylinder, separated", 20000) {
	const ShapeInstance first = Of(ShapeKind::Cylinder, Vector3::Zero, UNIT_CYLINDER, true);
	const ShapeInstance second = Of(ShapeKind::Cylinder, Vector3{1.4f, 0.0f, 0.0f}, UNIT_CYLINDER, false);
	for (int pass = 0; pass < 20000; pass++) {
		Consume(ContactBetween(first, second).Touching);
	}
}

// --- in a world ---------------------------------------------------------------

namespace narrowphase_bench {
	// A slab of colliders, one shape kind in three, packed closely enough that
	// a good fraction of the candidate pairs really touch.
	//
	// Deterministic through `core::Random`, which is indexed rather than
	// streamed, so two runs measure the same scene. Built lazily because a
	// store binds its owning thread on construction.
	Store &WorldOf(size_t count) {
		static std::vector<std::pair<size_t, std::unique_ptr<Store>>> built;
		for (auto &[key, store] : built) {
			if (key == count) {
				return *store;
			}
		}

		auto store = std::make_unique<Store>("physics.bench.narrowphase");
		PreparePhysicsWorld(*store, 4.0f);

		for (size_t index = 0; index < count; index++) {
			const auto seed = static_cast<uint32_t>(index);
			const Vector3 centre{
				Random::Range(seed, 3, -32.0f, 32.0f),
				Random::Range(seed, 5, -4.0f, 4.0f),
				Random::Range(seed, 7, -32.0f, 32.0f),
			};

			const Entity entity = store->Create();
			store->Set<Transform>(entity, Transform{CFrame{centre, TILTED.Rotation()}});

			Collider collider;
			collider.Shape = static_cast<ShapeKind>(index % 3);
			collider.Extent = Vector3{0.5f, 0.5f, 0.5f};
			store->Set<Collider>(entity, collider);
			store->Set<Motion>(entity, Motion{});
		}

		SyncBroadphase(*store);
		BroadPhase(*store);
		built.emplace_back(count, std::move(store));
		return *built.back().second;
	}
}

BENCH("Narrow phase · 2000 mixed colliders", 100) {
	Store &store = WorldOf(2000);
	for (int pass = 0; pass < 100; pass++) {
		NarrowPhase(store);
		Consume(store.Resource<PhysicsWorld>()->Manifolds().size());
	}
}

BENCH("Narrow phase · 8000 mixed colliders", 30) {
	Store &store = WorldOf(8000);
	for (int pass = 0; pass < 30; pass++) {
		NarrowPhase(store);
		Consume(store.Resource<PhysicsWorld>()->Manifolds().size());
	}
}
