#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/spatial/LayerMask.hpp>
#include <engine/testing/Suite.hpp>

// Private: the pair list's retained capacity and the two indexes' rebuild
// counts are what several cases here assert on, and no module outside this one
// has any business reaching them.
#include "PipelineInternals.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.physics.broadphase")
// The boxes handed to the index come from here, and a wrong bound is a dropped
// pair rather than a visible failure.
TEST_DEPENDS("engine.physics.shapes")
// The pair list and the two indexes live on the resource, and its accessors are
// what these cases read.
TEST_DEPENDS("engine.physics.physicsworld")
// The index itself, and the guarantee that two rebuilds of one input iterate
// identically - which is half of why the pair list is reproducible.
TEST_DEPENDS("engine.spatial.hashgrid")
// The overlap query the pair walk is built on.
TEST_DEPENDS("engine.spatial.query")
// Layer and Mask are these, and the filtering rule is stated on both.
TEST_DEPENDS("engine.spatial.layermask")
// Transform, Collider and Motion, and the fact that a Motion is what makes a
// collider dynamic.
TEST_DEPENDS("engine.scene.components")
// The change stamps the static index's staleness is decided from.
TEST_DEPENDS("engine.ecs.changechannel")

using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::physics::BroadPhase;
using engine::physics::CandidatePair;
using engine::physics::ColliderRecord;
using engine::physics::PairAdmitted;
using engine::physics::PhysicsWorld;
using engine::physics::PipelineInternals;
using engine::physics::PreparePhysicsWorld;
using engine::physics::SyncBroadphase;
using engine::scene::Collider;
using engine::scene::Motion;
using engine::scene::ShapeKind;
using engine::scene::Transform;
using engine::spatial::LayerMask;

namespace {
	// One-metre cells, so a coordinate in a case below is also a cell index and
	// nothing here needs arithmetic to read.
	constexpr float UNIT_CELL = 1.0f;

	// Half a metre on each axis, so two colliders a metre apart just touch.
	constexpr Vector3 HALF_METRE{0.5f, 0.5f, 0.5f};

	// How a part is described to the helpers below.
	//
	// An empty label creates an unnamed entity. `Store::Create` hands back the
	// entity already holding a name rather than minting a second, so a case
	// wanting several interchangeable colliders must not name them all the
	// same thing - it would get one entity and a passing test that measured
	// nothing.
	struct Placed {
		std::string_view Label;
		Vector3 Position;
		bool Moving = true;
		LayerMask Layer = LayerMask::Only(0);
		LayerMask Mask = LayerMask::All();
	};

	Entity Place(Store &store, const Placed &placed) {
		const Entity entity = placed.Label.empty() ? store.Create() : store.Create(placed.Label);
		store.Set<Transform>(entity, Transform{CFrame{placed.Position}});

		Collider collider;
		collider.Shape = ShapeKind::Box;
		collider.Extent = HALF_METRE;
		collider.Layer = placed.Layer;
		collider.Mask = placed.Mask;
		store.Set<Collider>(entity, collider);

		// A `Motion` is what makes a collider one the world may move, and
		// therefore which of the two indexes it lands in. `MakePart` says the
		// same thing from the other end: an anchored part gets neither
		// `RigidBody` nor `Motion`.
		if (placed.Moving) {
			store.Set<Motion>(entity, Motion{});
		}
		return entity;
	}

	void Step(Store &store) {
		SyncBroadphase(store);
		BroadPhase(store);
	}

	const PhysicsWorld &WorldOf(const Store &store) {
		return *store.Resource<PhysicsWorld>();
	}

	// The pairs as label strings, so two scenes built in different orders - and
	// therefore holding different entity ids - can be compared at all.
	std::vector<std::pair<std::string, std::string>> Labelled(const Store &store) {
		std::vector<std::pair<std::string, std::string>> named;
		for (const CandidatePair &pair : WorldOf(store).Pairs()) {
			named.emplace_back(std::string(store.NameOf(pair.A)), std::string(store.NameOf(pair.B)));
		}
		// The pair list is already sorted by entity id; sorting by label is a
		// second, id-independent order so that two different insertion orders
		// are comparable at all.
		for (auto &entry : named) {
			if (entry.second < entry.first) {
				std::swap(entry.first, entry.second);
			}
		}
		std::sort(named.begin(), named.end());
		return named;
	}
}

TEST_CASE("two overlapping colliders are one candidate pair", "[physics][broadphase]") {
	Store store("broadphase.overlap");
	PreparePhysicsWorld(store, UNIT_CELL);

	const Entity left = Place(store, Placed{"left", Vector3::Zero});
	const Entity right = Place(store, Placed{"right", Vector3{0.5f, 0.0f, 0.0f}});
	Place(store, Placed{"far", Vector3{20.0f, 0.0f, 0.0f}});

	Step(store);

	const auto pairs = WorldOf(store).Pairs();
	REQUIRE(pairs.size() == 1);
	CHECK(pairs[0].A.Id == std::min(left.Id, right.Id));
	CHECK(pairs[0].B.Id == std::max(left.Id, right.Id));
}

TEST_CASE("a body is never paired with itself", "[physics][broadphase]") {
	// The dynamic query finds the querying proxy in its own index, every time.
	// Dropping it is one comparison and forgetting it is a body the solver
	// pushes against itself.
	Store store("broadphase.self");
	PreparePhysicsWorld(store, UNIT_CELL);

	Place(store, Placed{"only", Vector3::Zero});
	Step(store);

	CHECK(WorldOf(store).Pairs().empty());
}

TEST_CASE("a pair is reported once however many cells it spans", "[physics][broadphase]") {
	// A collider wider than a cell appears in several buckets. `spatial`
	// reports it from the first shared cell of the walk, and this is the case
	// that fails if that de-duplication is ever traded for a visited stamp - or
	// if the pair walk stops keeping each unordered pair exactly once.
	Store store("broadphase.spanning");
	PreparePhysicsWorld(store, UNIT_CELL);

	const Entity wide = store.Create("wide");
	store.Set<Transform>(wide, Transform{CFrame{Vector3::Zero}});
	Collider slab;
	slab.Extent = Vector3{6.0f, 0.5f, 6.0f};
	store.Set<Collider>(wide, slab);
	store.Set<Motion>(wide, Motion{});

	Place(store, Placed{"small", Vector3{2.0f, 0.0f, 2.0f}});

	Step(store);

	CHECK(WorldOf(store).Pairs().size() == 1);
}

TEST_CASE("pairs come out sorted by the smaller id then the larger", "[physics][broadphase]") {
	// **A determinism requirement, not tidiness.** Sequential impulse is
	// order-dependent, so an unsorted pair list makes the solver visit contacts
	// in a different order between runs and `just determinism` fails a long way
	// from here.
	Store store("broadphase.sorted");
	PreparePhysicsWorld(store, UNIT_CELL);

	// **The rows are laid out against the ids on purpose.** A cluster built in
	// id order comes out of the pair walk already ascending, so the sort would
	// be free to be missing and this case would still pass - it did, the first
	// time it was written. Adding the colliders in reverse makes the walk emit
	// descending ids, which nothing but the sort can put right.
	std::vector<Entity> entities;
	for (int index = 0; index < 6; index++) {
		const Entity entity = store.Create();
		store.Set<Transform>(entity, Transform{CFrame{Vector3{static_cast<float>(index) * 0.4f, 0, 0}}});
		entities.push_back(entity);
	}
	for (auto at = entities.rbegin(); at != entities.rend(); ++at) {
		Collider collider;
		collider.Extent = HALF_METRE;
		store.Set<Collider>(*at, collider);
		store.Set<Motion>(*at, Motion{});
	}

	Step(store);

	const auto pairs = WorldOf(store).Pairs();
	REQUIRE(pairs.size() > 1);
	CHECK(std::is_sorted(pairs.begin(), pairs.end()));

	for (const CandidatePair &pair : pairs) {
		CHECK(pair.A.Id < pair.B.Id);
	}
	CHECK(std::adjacent_find(pairs.begin(), pairs.end()) == pairs.end());
}

TEST_CASE("the pair list does not depend on the order colliders entered", "[physics][broadphase]") {
	// The sharp form of the determinism requirement. Both scenes create the
	// same entities in the same order, so the ids are identical; only the order
	// the rows entered the collider archetype differs, which is what changes
	// the order the index is built in and therefore the order pairs are found.
	// The resulting list has to be byte-identical, not merely equivalent.
	const auto run = [](const char *name, bool reversed) {
		Store store(name);
		PreparePhysicsWorld(store, UNIT_CELL);

		std::vector<Entity> entities;
		for (int index = 0; index < 8; index++) {
			const Entity entity = store.Create();
			store.Set<Transform>(entity, Transform{CFrame{Vector3{static_cast<float>(index) * 0.4f, 0, 0}}});
			entities.push_back(entity);
		}

		// The collider is what moves a row into the queried archetype, so
		// adding it in a different order lays the rows out differently while
		// leaving every entity id exactly where it was.
		if (reversed) {
			std::reverse(entities.begin(), entities.end());
		}
		for (const Entity entity : entities) {
			Collider collider;
			collider.Extent = HALF_METRE;
			store.Set<Collider>(entity, collider);
			store.Set<Motion>(entity, Motion{});
		}

		Step(store);

		std::vector<CandidatePair> pairs;
		for (const CandidatePair &pair : store.Resource<PhysicsWorld>()->Pairs()) {
			pairs.push_back(pair);
		}
		return pairs;
	};

	const std::vector<CandidatePair> forward = run("broadphase.order.forward", false);
	const std::vector<CandidatePair> backward = run("broadphase.order.backward", true);

	REQUIRE_FALSE(forward.empty());
	REQUIRE(forward.size() == backward.size());
	for (size_t index = 0; index < forward.size(); index++) {
		CHECK(forward[index] == backward[index]);
	}
}

TEST_CASE("the same scene built in a different creation order pairs the same", "[physics][broadphase]") {
	// Creating in a different order gives every entity a different id, so the
	// two lists cannot be compared as ids - they are compared as the names the
	// scene gave them, which is what "the same scene" means to anybody but the
	// entity directory.
	const auto run = [](const char *name, bool reversed) {
		Store store(name);
		PreparePhysicsWorld(store, UNIT_CELL);

		std::vector<Placed> scene = {
			Placed{"a", Vector3::Zero},
			Placed{"b", Vector3{0.4f, 0.0f, 0.0f}},
			Placed{"c", Vector3{0.8f, 0.0f, 0.0f}},
			Placed{"floor", Vector3{0.4f, -0.6f, 0.0f}, false},
		};
		if (reversed) {
			std::reverse(scene.begin(), scene.end());
		}
		for (const Placed &placed : scene) {
			Place(store, placed);
		}

		Step(store);
		return Labelled(store);
	};

	const auto forward = run("broadphase.creation.forward", false);
	const auto backward = run("broadphase.creation.backward", true);

	REQUIRE_FALSE(forward.empty());
	CHECK(forward == backward);
}

TEST_CASE("filtering needs both masks and not either", "[physics][broadphase]") {
	// The rule is `a.Mask ∩ b.Layer` **and** `b.Mask ∩ a.Layer`, which is what
	// `scene::Collider::Mask` already claims in its own comment - so the other
	// reading would make that documentation false rather than merely make a
	// different choice.
	const ColliderRecord seer{Entity{1}, LayerMask::Only(0), LayerMask::Only(1)};
	const ColliderRecord seen{Entity{2}, LayerMask::Only(1), LayerMask::None()};
	const ColliderRecord mutual{Entity{3}, LayerMask::Only(1), LayerMask::Only(0)};

	// A sees B and B does not see A.
	CHECK_FALSE(PairAdmitted(seer, seen));
	CHECK_FALSE(PairAdmitted(seen, seer));

	// Both directions, so the pair is admitted.
	CHECK(PairAdmitted(seer, mutual));
	CHECK(PairAdmitted(mutual, seer));
}

TEST_CASE("a one-way mask produces no pair", "[physics][broadphase]") {
	// The same rule through the whole pipeline rather than against the
	// predicate alone: two boxes on top of each other, one of which cannot see
	// the other, are not a pair.
	Store store("broadphase.oneway");
	PreparePhysicsWorld(store, UNIT_CELL);

	Place(store, Placed{"seer", Vector3::Zero, true, LayerMask::Only(0), LayerMask::Only(1)});
	Place(store, Placed{"blind", Vector3{0.2f, 0.0f, 0.0f}, true, LayerMask::Only(1), LayerMask::None()});

	Step(store);
	CHECK(WorldOf(store).Pairs().empty());

	// And the same two, once the second can see the first.
	Store mutualStore("broadphase.mutual");
	PreparePhysicsWorld(mutualStore, UNIT_CELL);

	Place(mutualStore, Placed{"seer", Vector3::Zero, true, LayerMask::Only(0), LayerMask::Only(1)});
	Place(
		mutualStore, Placed{"seen", Vector3{0.2f, 0.0f, 0.0f}, true, LayerMask::Only(1), LayerMask::Only(0)}
	);

	Step(mutualStore);
	CHECK(mutualStore.Resource<PhysicsWorld>()->Pairs().size() == 1);
}

TEST_CASE("a dynamic collider pairs with static geometry", "[physics][broadphase]") {
	Store store("broadphase.static");
	PreparePhysicsWorld(store, UNIT_CELL);

	Place(store, Placed{"crate", Vector3::Zero});
	Place(store, Placed{"floor", Vector3{0.0f, -0.6f, 0.0f}, false});

	Step(store);

	CHECK(WorldOf(store).DynamicColliders() == 1);
	CHECK(WorldOf(store).StaticColliders() == 1);
	CHECK(WorldOf(store).Pairs().size() == 1);
}

TEST_CASE("two static colliders are never a pair", "[physics][broadphase]") {
	// Only the dynamic set is queried. Two overlapping anchored parts are the
	// level author's business, and a contact the solver could not move either
	// half of costs work for nothing.
	Store store("broadphase.twostatic");
	PreparePhysicsWorld(store, UNIT_CELL);

	Place(store, Placed{"wall", Vector3::Zero, false});
	Place(store, Placed{"pillar", Vector3{0.3f, 0.0f, 0.0f}, false});

	Step(store);

	CHECK(WorldOf(store).StaticColliders() == 2);
	CHECK(WorldOf(store).Pairs().empty());
}

TEST_CASE("static geometry that has not moved is not rebuilt", "[physics][broadphase]") {
	// The whole reason there are two indexes. `spatial::HashGrid` is
	// rebuild-only by design, so "only re-insert what moved" is a decision
	// about which set to hand to `Rebuild` - and static geometry, which is most
	// of a world, must not be re-measured every tick.
	Store store("broadphase.staticstable");
	PreparePhysicsWorld(store, UNIT_CELL);

	Place(store, Placed{"crate", Vector3::Zero});
	Place(store, Placed{"floor", Vector3{0.0f, -0.6f, 0.0f}, false});

	Step(store);
	const uint64_t afterFirst = WorldOf(store).StaticRebuilds();
	REQUIRE(afterFirst == 1);

	for (int tick = 0; tick < 20; tick++) {
		Step(store);
	}

	CHECK(WorldOf(store).StaticRebuilds() == afterFirst);

	// The dynamic index, by contrast, is rebuilt every tick - which is what
	// makes the number above worth reading rather than an artefact of nothing
	// running.
	CHECK(WorldOf(store).DynamicRebuilds() == 21);
}

TEST_CASE("moving static geometry rebuilds its index", "[physics][broadphase]") {
	Store store("broadphase.staticmoved");
	PreparePhysicsWorld(store, UNIT_CELL);

	Place(store, Placed{"crate", Vector3::Zero});
	const Entity floor = Place(store, Placed{"floor", Vector3{0.0f, -0.6f, 0.0f}, false});

	Step(store);
	Step(store);
	const uint64_t settled = WorldOf(store).StaticRebuilds();

	// Written through `Set`, which is how a loader, an editor or a script moves
	// an anchored part - and what advances the change stamp the sync reads.
	store.Set<Transform>(floor, Transform{CFrame{Vector3{0.0f, -20.0f, 0.0f}}});
	Step(store);

	CHECK(WorldOf(store).StaticRebuilds() == settled + 1);
	CHECK(WorldOf(store).Pairs().empty());
}

TEST_CASE("adding static geometry rebuilds its index", "[physics][broadphase]") {
	Store store("broadphase.staticadded");
	PreparePhysicsWorld(store, UNIT_CELL);

	Place(store, Placed{"crate", Vector3::Zero});
	Step(store);
	const uint64_t settled = WorldOf(store).StaticRebuilds();

	Place(store, Placed{"floor", Vector3{0.0f, -0.6f, 0.0f}, false});
	Step(store);

	CHECK(WorldOf(store).StaticRebuilds() == settled + 1);
	CHECK(WorldOf(store).StaticColliders() == 1);
	CHECK(WorldOf(store).Pairs().size() == 1);
}

TEST_CASE("a collider that gains a motion leaves the static index", "[physics][broadphase]") {
	// The case that would otherwise pair a body with itself: for one tick the
	// entity is in the freshly built dynamic index and still in the stale
	// static one. The count check notices within the same tick, and the pair
	// walk drops a self-pair anyway.
	Store store("broadphase.becamedynamic");
	PreparePhysicsWorld(store, UNIT_CELL);

	const Entity crate = Place(store, Placed{"crate", Vector3::Zero, false});
	Step(store);
	REQUIRE(WorldOf(store).StaticColliders() == 1);

	store.Set<Motion>(crate, Motion{});
	Step(store);

	CHECK(WorldOf(store).DynamicColliders() == 1);
	CHECK(WorldOf(store).StaticColliders() == 0);
	CHECK(WorldOf(store).Pairs().empty());
}

TEST_CASE("marking the static set dirty forces one rebuild", "[physics][broadphase]") {
	// The escape hatch for a transform written through a raw column pointer,
	// which advances no per-row stamp and is therefore invisible to the change
	// channel. Calling it when nothing moved costs one rebuild; not calling it
	// when something did is a collider that collides where it used to be.
	Store store("broadphase.markdirty");
	PreparePhysicsWorld(store, UNIT_CELL);

	Place(store, Placed{"floor", Vector3::Zero, false});
	Step(store);
	const uint64_t settled = WorldOf(store).StaticRebuilds();

	store.ResourceMutable<PhysicsWorld>()->MarkStaticDirty();
	Step(store);
	CHECK(WorldOf(store).StaticRebuilds() == settled + 1);

	Step(store);
	CHECK(WorldOf(store).StaticRebuilds() == settled + 1);
}

TEST_CASE("the pair list is cleared and not freed", "[physics][broadphase]") {
	// `v02v03v04.md`'s allocation table states it for exactly these lists: a
	// steady scene stops allocating after its first tick.
	Store store("broadphase.capacity");
	PreparePhysicsWorld(store, UNIT_CELL);

	for (int index = 0; index < 8; index++) {
		Place(store, Placed{{}, Vector3{static_cast<float>(index) * 0.4f, 0.0f, 0.0f}});
	}
	Step(store);

	PhysicsWorld &world = *store.ResourceMutable<PhysicsWorld>();
	const size_t capacity = PipelineInternals::Pairs(world).capacity();
	const void *storage = PipelineInternals::Pairs(world).data();
	REQUIRE(capacity > 0);

	// Move everything apart, so the next tick produces no pairs at all.
	store.Each<Transform>([](Entity, Transform &transform) {
		transform.Frame.Position = transform.Frame.Position * 100.0f;
	});
	store.MarkAllChanged<Transform>();
	Step(store);

	CHECK(world.Pairs().empty());
	CHECK(PipelineInternals::Pairs(world).capacity() == capacity);
	CHECK(static_cast<const void *>(PipelineInternals::Pairs(world).data()) == storage);
}

TEST_CASE("an empty world produces no pairs and no candidates", "[physics][broadphase]") {
	Store store("broadphase.empty");
	PreparePhysicsWorld(store, UNIT_CELL);

	Step(store);

	CHECK(WorldOf(store).DynamicColliders() == 0);
	CHECK(WorldOf(store).StaticColliders() == 0);
	CHECK(WorldOf(store).Pairs().empty());
}
