// The replication seam: applying authoritative state to a world that is
// already running.
//
// `v02v03.md` §2.12 reserves exactly this and builds nothing else. A client
// holding a replica receives state every tick and reconciles against what it
// already has — same entity, new values, no destroy-and-recreate, because
// recreating would reset everything the client predicted and turn every
// correction into a visible pop.

#include <engine/core/Bytes.hpp>
#include <engine/core/Random.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <vector>

TEST_SUITE_ID("engine.ecs.replication")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Random;
using engine::ecs::ApplyMode;
using engine::ecs::Entity;
using engine::ecs::Store;

namespace replication_test {
	struct Spot {
		float X = 0.0f;
	};
	struct Drift {
		float X = 0.0f;
	};
	struct Owned {
		int Value = 0;
	};

	// A handle stored inside a component, which is the case the directory has
	// to be reproduced exactly for.
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

	std::map<uint64_t, float> SpotsIn(Store &store) {
		std::map<uint64_t, float> found;
		store.Each<const Spot>([&found](Entity entity, const Spot &spot) {
			found.emplace(entity.Id, spot.X);
		});
		return found;
	}
}

using namespace replication_test;

TEST_CASE("applying to an empty world brings everything across", "[ecs]") {
	Store authority("authority");
	for (int index = 0; index < 16; index++) {
		authority.Set<Spot>(authority.Create(), Spot{static_cast<float>(index)});
	}

	Store replica("replica");
	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));

	REQUIRE(SpotsIn(replica) == SpotsIn(authority));
}

TEST_CASE("applying updates entities in place rather than recreating them", "[ecs]") {
	// The property the whole seam exists for. A destroy-and-recreate would give
	// the entity a new generation, and every handle the client held — including
	// ones inside its own components — would go stale on every correction.
	Store authority("authority");
	const Entity tracked = authority.Create();
	authority.Set<Spot>(tracked, Spot{1.0f});

	Store replica("replica");
	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));
	REQUIRE(replica.Alive(tracked));

	authority.GetMutable<Spot>(tracked)->X = 99.0f;
	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));

	// Same handle, new value.
	REQUIRE(replica.Alive(tracked));
	REQUIRE(replica.Get<Spot>(tracked)->X == 99.0f);
}

TEST_CASE("a handle stored inside a component survives every correction", "[ecs]") {
	Store authority("authority");
	const Entity first = authority.Create();
	const Entity second = authority.Create();
	authority.Set<Spot>(first, Spot{1.0f});
	authority.Set<Spot>(second, Spot{2.0f});
	authority.Set<Target>(first, Target{second});

	Store replica("replica");
	for (int correction = 0; correction < 10; correction++) {
		authority.GetMutable<Spot>(second)->X = static_cast<float>(correction);
		REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));

		const Entity pointed = replica.Get<Target>(first)->Other;
		REQUIRE(pointed == second);
		REQUIRE(replica.Alive(pointed));
		REQUIRE(replica.Get<Spot>(pointed)->X == static_cast<float>(correction));
	}
}

TEST_CASE("authoritative mode removes what the sender no longer has", "[ecs]") {
	// The sender is saying "this is the whole world", so anything else the
	// receiver believes is stale.
	Store authority("authority");
	std::vector<Entity> entities;
	for (int index = 0; index < 8; index++) {
		const Entity entity = authority.Create();
		authority.Set<Spot>(entity, Spot{static_cast<float>(index)});
		entities.push_back(entity);
	}

	Store replica("replica");
	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));
	REQUIRE(replica.CountMatching<Spot>() == 8);

	authority.Destroy(entities[3]);
	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));

	REQUIRE(replica.CountMatching<Spot>() == 7);
	REQUIRE_FALSE(replica.Alive(entities[3]));
}

// Builds a snapshot that mentions only some of a world's entities, the way a
// delta or an interest-filtered update would. Loading and then destroying keeps
// every surviving entity's index and generation exactly as the authority has
// them, which is the property that makes the result a *partial view of that
// world* rather than a different world.
static std::vector<std::byte>
PartialOf(Store &authority, const std::vector<Entity> &all, const std::vector<Entity> &keep) {
	Store partial("partial");
	const std::vector<std::byte> whole = SnapshotOf(authority);
	ByteReader reader(whole);
	REQUIRE(partial.Load(reader));

	for (const Entity entity : all) {
		if (std::find(keep.begin(), keep.end(), entity) == keep.end()) {
			partial.Destroy(entity);
		}
	}
	return SnapshotOf(partial);
}

TEST_CASE("overlay mode leaves what the sender did not mention", "[ecs]") {
	// For a delta, or the slice of a world one client can perceive. Removing
	// everything outside the part that was sent is the failure this mode
	// exists to avoid.
	Store authority("authority");
	std::vector<Entity> entities;
	for (int index = 0; index < 5; index++) {
		const Entity entity = authority.Create();
		authority.Set<Spot>(entity, Spot{static_cast<float>(index)});
		entities.push_back(entity);
	}

	Store replica("replica");
	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));
	REQUIRE(replica.CountMatching<Spot>() == 5);

	// One entity moved. That is all the sender says.
	authority.GetMutable<Spot>(entities[2])->X = 42.0f;
	REQUIRE(ApplyTo(replica, PartialOf(authority, entities, {entities[2]}), ApplyMode::Overlay));

	REQUIRE(replica.CountMatching<Spot>() == 5);
	REQUIRE(replica.Get<Spot>(entities[2])->X == 42.0f);
	REQUIRE(replica.Get<Spot>(entities[0])->X == 0.0f);
	REQUIRE(replica.Get<Spot>(entities[4])->X == 4.0f);
}

TEST_CASE("the same partial snapshot read as authoritative empties the rest", "[ecs]") {
	// The mode is the whole difference, on identical bytes. Sending a delta and
	// reading it as full state is the mistake `ApplyMode` exists to make
	// impossible to commit silently.
	Store authority("authority");
	std::vector<Entity> entities;
	for (int index = 0; index < 5; index++) {
		const Entity entity = authority.Create();
		authority.Set<Spot>(entity, Spot{static_cast<float>(index)});
		entities.push_back(entity);
	}

	const std::vector<std::byte> partial = PartialOf(authority, entities, {entities[2]});

	Store replica("replica");
	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));
	REQUIRE(ApplyTo(replica, partial, ApplyMode::Authoritative));

	REQUIRE(replica.CountMatching<Spot>() == 1);
	REQUIRE(replica.Alive(entities[2]));
	REQUIRE_FALSE(replica.Alive(entities[0]));
}

TEST_CASE("two stores allocate the same indices, and apply cannot tell them apart", "[ecs]") {
	// **A constraint the replication version has to solve, recorded here
	// rather than discovered later.**
	//
	// Entity identity is an index plus a generation, and two independently
	// built stores both start at index 0 generation 1. So an entity a replica
	// created for itself — a predicted projectile, say — can collide exactly
	// with one the authority created, and `Apply` is right to treat them as the
	// same entity: it has nothing to distinguish them by.
	//
	// The fix belongs with replication, not here: locally predicted entities
	// need an index range the authority never allocates from. `ecs/docs/TODO.md`
	// carries it with that trigger. This case pins the current behaviour so the
	// day it changes, something says so.
	//
	// **What has changed is that a replica can no longer walk into this by
	// accident** — `SetAdoptOnly` refuses to mint, and every store a
	// `replication::Connector` writes into has it set. This case reaches the
	// collision by *not* setting it, which is what makes it still the pinned
	// behaviour of the storage rather than of a replica.
	Store authority("authority");
	const Entity theirs = authority.Create();
	authority.Set<Spot>(theirs, Spot{1.0f});

	Store replica("replica");
	const Entity mine = replica.Create();
	replica.Set<Owned>(mine, Owned{7});

	// The same handle, from two stores that have never met.
	REQUIRE(mine == theirs);

	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Overlay));

	// The authority's version won, because as far as the storage is concerned
	// there was only ever one entity.
	REQUIRE(replica.Has<Spot>(mine));
	REQUIRE_FALSE(replica.Has<Owned>(mine));
}

TEST_CASE("a component the sender dropped is dropped here too", "[ecs]") {
	// Otherwise a replica accumulates state the authority no longer believes
	// in, and nothing ever removes it.
	Store authority("authority");
	const Entity entity = authority.Create();
	authority.Set<Spot>(entity, Spot{1.0f});
	authority.Set<Drift>(entity, Drift{2.0f});

	Store replica("replica");
	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));
	REQUIRE(replica.Has<Drift>(entity));

	authority.Remove<Drift>(entity);
	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));

	REQUIRE(replica.Has<Spot>(entity));
	REQUIRE_FALSE(replica.Has<Drift>(entity));
}

TEST_CASE("an entity destroyed and recreated by the sender is a different entity", "[ecs]") {
	// Matched by index *and* generation. Matching on the index alone would let
	// a recycled slot arrive wearing the old entity's identity.
	Store authority("authority");
	const Entity original = authority.Create();
	authority.Set<Spot>(original, Spot{1.0f});

	Store replica("replica");
	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));

	authority.Destroy(original);
	const Entity replacement = authority.Create(); // reuses the index
	authority.Set<Spot>(replacement, Spot{2.0f});
	REQUIRE(replacement != original);

	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));

	REQUIRE(replica.Alive(replacement));
	REQUIRE_FALSE(replica.Alive(original));
	REQUIRE(replica.Get<Spot>(replacement)->X == 2.0f);
}

TEST_CASE("the clock comes across, so a replica knows which tick it holds", "[ecs]") {
	Store authority("authority");
	authority.AdvanceTick(1.0f / 60.0f);
	authority.AdvanceTick(1.0f / 60.0f);
	authority.AdvanceTick(1.0f / 60.0f);

	Store replica("replica");
	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));

	REQUIRE(replica.Time().Tick == 3);
	REQUIRE(replica.Time().Elapsed == authority.Time().Elapsed);
}

TEST_CASE("a corrupt snapshot leaves the live world as it was", "[ecs]") {
	// A replica that lost its world to a bad packet would be worse off than one
	// that ignored it.
	Store replica("replica");
	const Entity kept = replica.Create();
	replica.Set<Spot>(kept, Spot{5.0f});

	std::vector<std::byte> rubbish(64);
	for (size_t index = 0; index < rubbish.size(); index++) {
		rubbish[index] = static_cast<std::byte>(Random::Bits(1u, static_cast<uint32_t>(index)));
	}

	ByteReader reader(rubbish);
	REQUIRE_FALSE(replica.Apply(reader, ApplyMode::Authoritative));

	REQUIRE(replica.Alive(kept));
	REQUIRE(replica.Get<Spot>(kept)->X == 5.0f);
	REQUIRE(replica.CountMatching<Spot>() == 1);
}

TEST_CASE("applying the same snapshot twice changes nothing the second time", "[ecs]") {
	// Idempotence. A replica that received a duplicate packet must not drift.
	Store authority("authority");
	for (int index = 0; index < 12; index++) {
		authority.Set<Spot>(authority.Create(), Spot{static_cast<float>(index)});
	}
	const std::vector<std::byte> snapshot = SnapshotOf(authority);

	Store replica("replica");
	REQUIRE(ApplyTo(replica, snapshot, ApplyMode::Authoritative));
	const auto once = SpotsIn(replica);

	REQUIRE(ApplyTo(replica, snapshot, ApplyMode::Authoritative));
	REQUIRE(SpotsIn(replica) == once);
}

TEST_CASE("a replica converges however far it has drifted", "[ecs][fuzz]") {
	// The reconciliation loop in miniature: the authority ticks, the replica
	// predicts something else, and one correction has to bring them level —
	// whatever the replica did in between.
	Store authority("authority");
	std::vector<Entity> entities;
	for (int index = 0; index < 64; index++) {
		const Entity entity = authority.Create();
		authority.Set<Spot>(entity, Spot{static_cast<float>(index)});
		entities.push_back(entity);
	}

	Store replica("replica");
	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));

	size_t diverged = 0;
	for (uint32_t round = 0; round < 200; round++) {
		// The authority moves.
		authority.Each<Spot>([round](Entity, Spot &spot) { spot.X += static_cast<float>(round % 3); });

		// The replica predicts badly, and sometimes invents or destroys.
		replica.Each<Spot>([](Entity, Spot &spot) { spot.X *= 1.5f; });
		if (Random::Bits(round, 41) % 4 == 0) {
			replica.Set<Spot>(replica.Create(), Spot{-1.0f});
		}
		if (Random::Bits(round, 42) % 4 == 0 && !entities.empty()) {
			replica.Destroy(entities[Random::Bits(round, 43) % entities.size()]);
		}

		// One correction brings it level.
		if (!ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative)) {
			diverged++;
			continue;
		}
		if (SpotsIn(replica) != SpotsIn(authority)) {
			diverged++;
		}
	}

	REQUIRE(diverged == 0);
}

TEST_CASE("an adopt-only store refuses to mint and still adopts", "[ecs]") {
	// The guard standing in for the index range until v0.4 builds it. A replica
	// that only adopts cannot collide with its authority, and this is what makes
	// "only adopts" a property rather than a hope.
	Store replica("replica");
	replica.SetAdoptOnly(true);
	REQUIRE(replica.AdoptOnly());

	REQUIRE(replica.Create() == engine::ecs::NULL_ENTITY);
	REQUIRE(replica.Create("named") == engine::ecs::NULL_ENTITY);

	// Adopting is the whole point and is untouched: this refuses minting, not
	// receiving.
	Store authority("authority");
	const Entity theirs = authority.Create();
	authority.Set<Spot>(theirs, Spot{4.0f});

	REQUIRE(replica.CreateAt(theirs));
	REQUIRE(replica.Alive(theirs));
	REQUIRE(ApplyTo(replica, SnapshotOf(authority), ApplyMode::Authoritative));
	REQUIRE(replica.Get<Spot>(theirs)->X == 4.0f);
}

TEST_CASE("adopt-only is a switch, not a one-way door", "[ecs]") {
	// A world promoted out of being a replica — single-player taking over a
	// session, a test building a fixture — has to be able to mint again.
	Store store("world");
	store.SetAdoptOnly(true);
	REQUIRE(store.Create() == engine::ecs::NULL_ENTITY);

	store.SetAdoptOnly(false);
	const Entity entity = store.Create();
	REQUIRE(entity != engine::ecs::NULL_ENTITY);
	REQUIRE(store.Alive(entity));
}
