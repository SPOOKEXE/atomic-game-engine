#include <engine/ecs/Entity.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/LayerMask.hpp>
#include <engine/testing/Suite.hpp>

// Private: the buffers a case here inspects for retained capacity, and the
// grids whose cell size it reads back.
#include "PipelineInternals.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

TEST_SUITE_ID("engine.physics.physicsworld")
// Both indexes are hash grids, and the cell size a world is constructed with is
// resolved by that constructor's own rules.
TEST_DEPENDS("engine.spatial.hashgrid")
// The record's two masks are these, and swapping them is the mistake the named
// type exists to narrow.
TEST_DEPENDS("engine.spatial.layermask")
// The manifold and event lists this resource holds.
TEST_DEPENDS("engine.physics.contacts")

using Catch::Approx;
using engine::ecs::Entity;
using engine::physics::CandidatePair;
using engine::physics::ColliderRecord;
using engine::physics::PhysicsWorld;
using engine::physics::PipelineInternals;
using engine::spatial::HashGrid;
using engine::spatial::LayerMask;

TEST_CASE("a fresh world starts with the static index stale", "[physics][physicsworld]") {
	// The first `SyncBroadphase` has to build the static index, and it has
	// nothing to compare against to work that out. Starting clean would leave a
	// world whose anchored geometry is in no index at all until something
	// happened to change — a floor nothing collides with, and no diagnostic.
	const PhysicsWorld world;

	CHECK(world.StaticDirty());
	CHECK(world.StaticRebuilds() == 0);
	CHECK(world.DynamicRebuilds() == 0);
	CHECK(world.DynamicColliders() == 0);
	CHECK(world.StaticColliders() == 0);
	CHECK(world.Pairs().empty());
}

TEST_CASE("a world takes the cell size it was given", "[physics][physicsworld]") {
	const PhysicsWorld sized{2.0f};
	CHECK(sized.CellSize() == Approx(2.0f));

	// Both indexes, not just the one a query happened to hit. Two grids at
	// different spacings would answer the same question two different ways.
	PhysicsWorld mutableWorld{2.0f};
	CHECK(PipelineInternals::DynamicIndex(mutableWorld).CellSize() == Approx(2.0f));
	CHECK(PipelineInternals::StaticIndex(mutableWorld).CellSize() == Approx(2.0f));

	// A cell size at or below zero is refused by the grid in favour of its own
	// default, because the alternative is a division by zero reaching every
	// later query as a NaN. That is what makes zero usable as "the default" at
	// the `PreparePhysicsWorld` boundary.
	const PhysicsWorld defaulted{0.0f};
	CHECK(defaulted.CellSize() == Approx(HashGrid::DEFAULT_CELL_SIZE));
}

TEST_CASE("marking the static set dirty is sticky until something clears it", "[physics][physicsworld]") {
	PhysicsWorld world;
	PipelineInternals::StaticStale(world) = false;
	REQUIRE_FALSE(world.StaticDirty());

	world.MarkStaticDirty();
	CHECK(world.StaticDirty());

	// Only the sync clears it, and only after it has actually rebuilt. A reader
	// clearing the flag would be a rebuild that never happened.
	CHECK(world.StaticDirty());
}

TEST_CASE("a fresh world has no bodies and nothing asleep", "[physics][physicsworld]") {
	// This case replaces the one that used to say nothing produced a manifold
	// or an event. Something does now — `NarrowPhase` and `Publish` — so the
	// warning it carried would be false, and a stale warning is worse than
	// none. What is still worth pinning is the *starting* state: a world that
	// has not ticked reports no contacts because it has not looked, and a
	// consumer must not read the empty sleeping set as "nothing can sleep".
	const PhysicsWorld world;

	CHECK(world.Manifolds().empty());
	CHECK(world.Events().empty());
	CHECK(world.Bodies().empty());
	CHECK(world.SleepingBodies() == 0);
	CHECK_FALSE(world.Sleeping(Entity{1}));
}

TEST_CASE("the contact buffers are cleared rather than freed", "[physics][physicsworld]") {
	// `v02v03v04.md`'s allocation table names the pair, contact and manifold
	// lists together: capacity retained across ticks. Only the pair list has a
	// producer today, so this is what pins the property for the other two
	// before the narrow phase arrives and has to keep it.
	PhysicsWorld world;

	PipelineInternals::Manifolds(world).resize(16);
	PipelineInternals::Events(world).resize(16);
	const size_t manifolds = PipelineInternals::Manifolds(world).capacity();
	const size_t events = PipelineInternals::Events(world).capacity();

	PipelineInternals::Manifolds(world).clear();
	PipelineInternals::Events(world).clear();

	CHECK(PipelineInternals::Manifolds(world).capacity() == manifolds);
	CHECK(PipelineInternals::Events(world).capacity() == events);
}

TEST_CASE("a pair orders by the smaller id then the larger", "[physics][physicsworld]") {
	// The ordering the solver visits contacts in, so it is the ordering two
	// runs of one scene have to agree on.
	const CandidatePair low{Entity{1}, Entity{2}};
	const CandidatePair sameFirst{Entity{1}, Entity{9}};
	const CandidatePair high{Entity{3}, Entity{4}};

	CHECK(low < sameFirst);
	CHECK(sameFirst < high);
	CHECK_FALSE(high < low);
	CHECK_FALSE(low < low);
	CHECK(low == CandidatePair{Entity{1}, Entity{2}});
	CHECK_FALSE(low == sameFirst);

	// Sorting a shuffled list has to produce exactly that order, which is what
	// `BroadPhase` relies on rather than on the walk happening to be tidy.
	std::vector<CandidatePair> pairs = {high, low, sameFirst};
	std::sort(pairs.begin(), pairs.end());
	CHECK(pairs[0] == low);
	CHECK(pairs[1] == sameFirst);
	CHECK(pairs[2] == high);
}

TEST_CASE("a record keeps the two masks apart", "[physics][physicsworld]") {
	// The layer a collider is on and the set it is tested against are the same
	// width and mean opposite things. A record that stored them the wrong way
	// round compiles and filters plausibly wrongly for the rest of the
	// project's life, which is the whole reason `spatial::LayerMask` is a type.
	const ColliderRecord record{Entity{7}, LayerMask::Only(2), LayerMask::Only(3)};

	CHECK(record.Owner == Entity{7});
	CHECK(record.Layer == LayerMask::Only(2));
	CHECK(record.Mask == LayerMask::Only(3));
	CHECK_FALSE(record.Layer == record.Mask);
}
