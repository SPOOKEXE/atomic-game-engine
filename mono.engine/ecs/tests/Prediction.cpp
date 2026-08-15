// Locally predicted entities: the reserved index range, and promotion.
//
// A replica may not mint an *authoritative* entity - the index is the
// authority's to hand out, and two independently built stores both start at
// index 0 generation 1, so one minted here would collide exactly with one the
// server minted. What it may do is mint from the high half of the index space,
// which the authority never allocates from. `tests/Replication.cpp` pins the
// collision itself; this pins the way out of it.
//
// Promotion is the primitive and not the policy. Nothing here decides *when* a
// prediction is promoted, because nothing predicts a spawn yet - see
// `Store::Promote` and `ecs/AGENTS.md`.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/SparseSet.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

TEST_SUITE_ID("engine.ecs.prediction")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::ecs::ApplyMode;
using engine::ecs::Classes;
using engine::ecs::ClassId;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::Hierarchy;
using engine::ecs::NULL_ENTITY;
using engine::ecs::SparseSet;
using engine::ecs::Store;

namespace prediction_test {
	struct Spot {
		float X = 0.0f;
	};
	struct Guess {
		int Value = 0;
	};

	// A handle stored inside a component - the case promotion cannot fix up and
	// says so.
	struct Target {
		Entity Other;
	};

	std::vector<std::byte> SnapshotOf(Store &store) {
		ByteWriter writer;
		REQUIRE(store.Save(writer));
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}

	bool ApplyTo(Store &store, const std::vector<std::byte> &bytes, ApplyMode mode) {
		ByteReader reader(bytes);
		return store.Apply(reader, mode);
	}

	// The handle a server would hand over: minted in the authority's store, at
	// an index this replica has not already filled. Promoting onto an index that
	// is already live here is refused, and rightly - so a test that wants the
	// promotion to happen has to name a free one, exactly as a real caller must.
	Entity ServerHandleFor(Store &replica, Store &authority) {
		for (int attempt = 0; attempt < 64; attempt++) {
			const Entity entity = authority.Create();
			if (!replica.Alive(entity)) {
				return entity;
			}
		}
		return NULL_ENTITY;
	}

	// A minimal class, so the hierarchy fix-up has a tree to work on.
	ClassId NodeClass() {
		static const ClassId node = [] {
			const std::array<engine::ecs::ComponentId, 1> members{Components::Of<Spot>()};
			return Classes::Register("prediction_test.Node", {}, members);
		}();
		return node;
	}
}

using namespace prediction_test;

// --- the range -------------------------------------------------------------

TEST_CASE("an authority never allocates in the predicted range", "[ecs]") {
	// Over a spawn-and-despawn churn, not only fresh mints: getting the fresh
	// path right and the recycling path wrong is the failure that appears in a
	// world which has been running a while and nowhere else.
	Store authority("authority");

	std::vector<Entity> live;
	size_t predicted = 0;

	for (int step = 0; step < 20'000; step++) {
		if (!live.empty() && step % 3 == 0) {
			authority.Destroy(live.back());
			live.pop_back();
			continue;
		}

		const Entity entity = authority.Create();
		REQUIRE(entity != NULL_ENTITY);
		if (Store::IsPredicted(entity)) {
			predicted++;
		}
		live.push_back(entity);
	}

	REQUIRE(predicted == 0);
}

TEST_CASE("a replica mints in the predicted range and cannot collide", "[ecs]") {
	// **The exact scenario `tests/Replication.cpp` reaches, now taken the safe
	// way.** There, a replica mints with `Create` and gets index 0 generation 1
	// - the same handle the authority minted - so `Apply` merges two different
	// entities into one and the replica's own component is gone. Here it mints
	// with `CreatePredicted`, and both survive.
	Store authority("authority");
	const Entity theirs = authority.Create();
	authority.Set<Spot>(theirs, Spot{1.0f});

	Store replica("replica");
	replica.SetAdoptOnly(true);
	const Entity mine = replica.CreatePredicted();
	replica.Set<Guess>(mine, Guess{7});

	// Different handles, from two stores that have never met.
	REQUIRE(mine != theirs);
	REQUIRE(Store::IsPredicted(mine));
	REQUIRE_FALSE(Store::IsPredicted(theirs));

	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Overlay));

	// The authority's entity arrived and the prediction is untouched. Under the
	// old behaviour there was one entity here carrying `Spot` and not `Guess`.
	REQUIRE(replica.Alive(theirs));
	REQUIRE(replica.Has<Spot>(theirs));
	REQUIRE(replica.Alive(mine));
	REQUIRE(replica.Get<Guess>(mine)->Value == 7);
	REQUIRE_FALSE(replica.Has<Spot>(mine));
}

TEST_CASE("an authoritative correction does not delete a prediction", "[ecs]") {
	// `Authoritative` mode destroys what the sender did not mention, and a
	// snapshot from a server can never mention a predicted entity - the
	// authority allocates nothing from that range. Sweeping them would delete
	// every prediction on the first correction, which is every tick.
	Store authority("authority");
	for (int index = 0; index < 4; index++) {
		authority.Set<Spot>(authority.Create(), Spot{static_cast<float>(index)});
	}

	Store replica("replica");
	replica.SetAdoptOnly(true);
	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));

	const Entity mine = replica.CreatePredicted();
	replica.Set<Guess>(mine, Guess{3});

	for (int correction = 0; correction < 5; correction++) {
		REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));
		REQUIRE(replica.Alive(mine));
		REQUIRE(replica.Get<Guess>(mine)->Value == 3);
	}

	REQUIRE(replica.CountMatching<Spot>() == 4);
}

TEST_CASE("a freed predicted index is reused inside the predicted range only", "[ecs]") {
	Store replica("replica");
	replica.SetAdoptOnly(true);

	const Entity first = replica.CreatePredicted();
	const Entity second = replica.CreatePredicted();
	replica.Destroy(first);
	replica.Destroy(second);

	const Entity third = replica.CreatePredicted();
	const Entity fourth = replica.CreatePredicted();

	REQUIRE(Store::IsPredicted(third));
	REQUIRE(Store::IsPredicted(fourth));

	// Recycled slots, so the handles differ from the retired ones only by
	// generation - and the retired ones stay dead.
	REQUIRE(third != second);
	REQUIRE(fourth != first);
	REQUIRE_FALSE(replica.Alive(first));
	REQUIRE_FALSE(replica.Alive(second));
	REQUIRE(replica.Alive(third));
	REQUIRE(replica.Alive(fourth));
}

TEST_CASE("every entity walk sees both regions", "[ecs]") {
	// `EachEntity` walks the directory rather than the tables, so it is the one
	// place a second region is missed silently: a world with no predictions
	// behaves identically either way.
	Store store("world");
	const Entity theirs = store.Create();
	const Entity mine = store.CreatePredicted();

	std::vector<Entity> seen;
	store.EachEntity([&seen](Entity entity) { seen.push_back(entity); });

	REQUIRE(seen.size() == 2);
	REQUIRE(seen[0] == theirs);
	REQUIRE(seen[1] == mine);
}

// --- what adopt-only now means ---------------------------------------------

TEST_CASE("an adopt-only store refuses authoritative mints and allows predicted ones", "[ecs]") {
	// The distinction the range buys, drawn at the call site: `Create` is the
	// authority's to make and `CreatePredicted` is not.
	Store replica("replica");
	replica.SetAdoptOnly(true);

	REQUIRE(replica.Create() == NULL_ENTITY);
	REQUIRE(replica.Create("named") == NULL_ENTITY);
	REQUIRE(replica.Find("named") == NULL_ENTITY);

	const Entity mine = replica.CreatePredicted();
	REQUIRE(mine != NULL_ENTITY);
	REQUIRE(replica.Alive(mine));
	REQUIRE(Store::IsPredicted(mine));
}

TEST_CASE("CreateInstance honours adopt-only", "[ecs]") {
	// **The hole this closes.** `Store::Create` checked the flag and
	// `Store::CreateInstance` did not, so a replica could mint a colliding
	// entity through the instance path - which is the path `scene::MakePart`
	// uses, and which grew a guard of its own to work around it.
	Store replica("replica");
	replica.SetAdoptOnly(true);

	REQUIRE(replica.CreateInstance(NodeClass()) == NULL_ENTITY);
	REQUIRE(replica.CreateInstance(NodeClass(), "named") == NULL_ENTITY);
}

TEST_CASE("CloneInstance honours adopt-only", "[ecs]") {
	// The third minting path, and the one easiest to forget: a clone is a mint.
	Store world("world");
	const Entity source = world.CreateInstance(NodeClass(), "source");
	REQUIRE(source != NULL_ENTITY);

	world.SetAdoptOnly(true);
	REQUIRE(world.CloneInstance(source) == NULL_ENTITY);
}

// --- promotion -------------------------------------------------------------

TEST_CASE("promotion keeps the row and changes the handle", "[ecs]") {
	// The whole point: a promotion that rebuilt the entity would throw away
	// what the client predicted, which is the state the prediction existed to
	// have.
	Store replica("replica");
	replica.SetAdoptOnly(true);

	const Entity mine = replica.CreatePredicted();
	replica.Set<Spot>(mine, Spot{4.5f});
	replica.Set<Guess>(mine, Guess{9});

	Store authority("authority");
	const Entity theirs = authority.Create();

	REQUIRE(replica.Promote(mine, theirs));

	REQUIRE(replica.Alive(theirs));
	REQUIRE_FALSE(replica.Alive(mine));
	REQUIRE(replica.Get<Spot>(theirs)->X == 4.5f);
	REQUIRE(replica.Get<Guess>(theirs)->Value == 9);
	REQUIRE_FALSE(Store::IsPredicted(theirs));

	// One entity, not two: the row was renamed rather than copied.
	REQUIRE(replica.CountMatching<Spot>() == 1);

	// And a query hands back the new handle, because the row's own copy of it
	// moved too. Leaving that would give a body a handle the directory calls
	// dead.
	Entity visited = NULL_ENTITY;
	replica.Each<Spot>([&visited](Entity entity, Spot &) { visited = entity; });
	REQUIRE(visited == theirs);
}

TEST_CASE("a promoted entity then takes authoritative state", "[ecs]") {
	// What promotion is for. Before it, the server's snapshot creates a second
	// entity beside the prediction; after it, the same snapshot corrects the row
	// the client already has.
	Store authority("authority");
	const Entity theirs = authority.Create();
	authority.Set<Spot>(theirs, Spot{10.0f});

	Store replica("replica");
	replica.SetAdoptOnly(true);
	const Entity mine = replica.CreatePredicted();
	replica.Set<Spot>(mine, Spot{4.5f});

	REQUIRE(replica.Promote(mine, theirs));
	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));

	REQUIRE(replica.CountMatching<Spot>() == 1);
	REQUIRE(replica.Get<Spot>(theirs)->X == 10.0f);
}

TEST_CASE("promotion moves the name with the entity", "[ecs]") {
	// The store's name map is keyed by index, so a promotion that left it alone
	// would have `Find` answer with a handle the directory calls dead - the
	// quietest possible failure, since `NameOf` and `Find` both guard on
	// liveness and would simply stop finding anything.
	Store replica("replica");
	const Entity mine = replica.CreatePredicted("beacon");
	REQUIRE(mine != NULL_ENTITY);
	REQUIRE(Store::IsPredicted(mine));
	REQUIRE(replica.Find("beacon") == mine);

	Store authority("authority");
	const Entity theirs = authority.Create();

	REQUIRE(replica.Promote(mine, theirs));

	REQUIRE(replica.Find("beacon") == theirs);
	REQUIRE(replica.NameOf(theirs) == "beacon");
	REQUIRE(replica.NameOf(mine).empty());
}

TEST_CASE("a named predicted mint refuses nothing a named authoritative one allows", "[ecs]") {
	// The two named paths share a body, and this is the pair of behaviours that
	// body has to keep: an empty name mints unnamed, and a name already taken
	// hands back what holds it rather than a second entity under one name.
	Store replica("replica");
	replica.SetAdoptOnly(true);

	const Entity unnamed = replica.CreatePredicted("");
	REQUIRE(unnamed != NULL_ENTITY);
	REQUIRE(replica.NameOf(unnamed).empty());

	const Entity first = replica.CreatePredicted("shot");
	REQUIRE(replica.CreatePredicted("shot") == first);
}

TEST_CASE("promotion relinks the instance tree around it", "[ecs]") {
	// `Hierarchy` is the one place `ecs` itself stores entity handles inside
	// components, so it is the one place promotion can fix up without guessing.
	Store world("world");

	const Entity parent = world.CreateInstance(NodeClass(), "parent");
	const Entity before = world.CreateInstance(NodeClass(), "before");
	const Entity after = world.CreateInstance(NodeClass(), "after");
	const Entity child = world.CreateInstance(NodeClass(), "child");

	// A predicted instance is not a thing `Store` mints today - the class path
	// is authoritative - so the node is built by hand out of the same
	// components, which is what the tree is made of anyway.
	const Entity guess = world.CreatePredicted();
	world.Set<Hierarchy>(guess, Hierarchy{});
	world.Set<Spot>(guess, Spot{1.0f});

	REQUIRE(world.SetParent(before, parent));
	REQUIRE(world.SetParent(guess, parent));
	REQUIRE(world.SetParent(after, parent));
	REQUIRE(world.SetParent(child, guess));

	Store authority("authority");
	const Entity theirs = ServerHandleFor(world, authority);

	REQUIRE(world.Promote(guess, theirs));

	// The parent's child list, the siblings either side, and the child's parent
	// all name the promoted handle now.
	std::vector<Entity> children;
	world.EachChild(parent, [&children](Entity node) { children.push_back(node); });
	REQUIRE(children.size() == 3);
	REQUIRE(children[0] == before);
	REQUIRE(children[1] == theirs);
	REQUIRE(children[2] == after);

	REQUIRE(world.ParentOf(child) == theirs);
	REQUIRE(world.ParentOf(theirs) == parent);
}

TEST_CASE("promotion refuses everything it cannot do safely", "[ecs]") {
	Store world("world");
	const Entity mine = world.CreatePredicted();
	world.Set<Spot>(mine, Spot{1.0f});

	Store authority("authority");
	const Entity theirs = authority.Create();
	const Entity another = authority.Create();

	// Not a prediction.
	REQUIRE_FALSE(world.Promote(theirs, another));

	// Not live.
	const Entity dead = world.CreatePredicted();
	world.Destroy(dead);
	REQUIRE_FALSE(world.Promote(dead, theirs));

	// Into the predicted range, which would leave it exactly as unsafe as it
	// started having claimed to fix it.
	Store other("other");
	const Entity alsoPredicted = other.CreatePredicted();
	REQUIRE_FALSE(world.Promote(mine, alsoPredicted));
	REQUIRE_FALSE(world.Promote(mine, NULL_ENTITY));

	// Onto an index that is already live here.
	const Entity occupant = world.Create();
	REQUIRE_FALSE(world.Promote(mine, occupant));

	// None of the refusals moved anything.
	REQUIRE(world.Alive(mine));
	REQUIRE(world.Get<Spot>(mine)->X == 1.0f);
	REQUIRE(world.CountMatching<Spot>() == 1);
}

TEST_CASE("promotion is refused from inside an iteration", "[ecs]") {
	// The id array an `Each` is holding a pointer into is exactly what promotion
	// rewrites, so a loop would hand a body the new handle for rows it had
	// already visited under the old one.
	Store world("world");
	const Entity mine = world.CreatePredicted();
	world.Set<Spot>(mine, Spot{1.0f});

	Store authority("authority");
	const Entity theirs = authority.Create();

	bool refused = false;
	world.Each<Spot>([&](Entity, Spot &) { refused = !world.Promote(mine, theirs); });

	REQUIRE(refused);
	REQUIRE(world.Alive(mine));
	REQUIRE_FALSE(world.Alive(theirs));
}

TEST_CASE("a handle stored in another component is left dead, never wrong", "[ecs]") {
	// **The limit of what promotion covers, pinned rather than described.**
	// Nothing in `TypeDescriptor` says which of a component's bytes are entity
	// handles, so a handle a game stored in a field of its own keeps the
	// predicted value. What matters is that it reads as *dead* - the predicted
	// index's generation is bumped as it is freed - rather than naming whatever
	// the replica predicts next.
	Store world("world");

	const Entity mine = world.CreatePredicted();
	world.Set<Spot>(mine, Spot{1.0f});

	const Entity watcher = world.Create();
	world.Set<Target>(watcher, Target{mine});

	Store authority("authority");
	const Entity theirs = ServerHandleFor(world, authority);
	REQUIRE(world.Promote(mine, theirs));

	// Not rewritten, and honest about it.
	const Entity stored = world.Get<Target>(watcher)->Other;
	REQUIRE(stored == mine);
	REQUIRE_FALSE(world.Alive(stored));

	// And it stays dead once the index is recycled, which is the property that
	// makes "not covered" safe rather than merely undocumented.
	const Entity reissued = world.CreatePredicted();
	REQUIRE(reissued != stored);
	REQUIRE_FALSE(world.Alive(stored));
}

TEST_CASE("a promoted entity survives a save and restore", "[ecs]") {
	// Promotion must not weaken what `Save`/`Load` promises: the directory comes
	// back exactly, so a handle stored inside a component is still the same
	// entity afterwards.
	Store world("world");

	const Entity mine = world.CreatePredicted();
	world.Set<Spot>(mine, Spot{2.5f});

	Store authority("authority");
	const Entity theirs = authority.Create();
	REQUIRE(world.Promote(mine, theirs));

	const Entity pointer = world.Create();
	world.Set<Target>(pointer, Target{theirs});

	ByteWriter writer;
	REQUIRE(world.Save(writer));

	Store restored("restored");
	ByteReader reader(writer.Bytes());
	REQUIRE(restored.Load(reader));

	REQUIRE(restored.Alive(theirs));
	REQUIRE_FALSE(restored.Alive(mine));
	REQUIRE(restored.Get<Spot>(theirs)->X == 2.5f);
	REQUIRE(restored.Get<Target>(pointer)->Other == theirs);
	REQUIRE(restored.Alive(restored.Get<Target>(pointer)->Other));
}
