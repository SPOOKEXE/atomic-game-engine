// What a tick of `Authority::Publish` spends before it has looked at a client.
//
// **`Survey` is the half of publishing that no client pays for and every client
// waits on.** It runs once per tick whatever the population is: it resolves the
// replicated component names, builds `Bearing` - the entities carrying any
// replicated component - and re-signs every slot whose change detection is a
// hash rather than a dirty bit. A capture of the studio's Play link had
// `FindBearing` and `Resign` filling most of the `Authority::Publish` bar, which
// is a cost that scales with the *scene* and not with the number of people in
// it, so nothing about a quiet server makes it go away.
//
// **The ladder varies declared slots against carried ones, because that ratio
// is the whole shape of `FindBearing`.** A scene declares far more replicated
// component types than any one entity carries - a world of moving parts
// replicates a transform on all of them and a dozen other types on none - so the
// interesting number is what a *miss* costs. Whether a component is present is a
// property of the archetype, so a survey that reads it from the table pays once
// per table per slot and a survey that asks each entity pays `entities x slots`
// random reads a tick.
//
// The `24 declared, 0 carried` row is where that shows: nothing matches, so the
// only work is the table test, and the row should stay at the baseline however
// many slots are declared. It was 151 ns an entity when the survey asked per
// entity. The `4 declared` and `24 declared` rows carry identical scenes and
// should read the same as each other for the same reason - a gap between them is
// a per-entity test having come back.
//
// **The signature rows vary the component's width, because `Resign` hashes.** A
// signed slot is hashed in full for every carrier every tick, and the mixing
// chain is serial - each step waits on the multiply before it - so the width of
// the value is a latency and not a throughput. The 16-byte and 64-byte rows are
// the same entities and the same slot count with four times the bytes, and if
// they cost four times as much then the hash is running a byte at a time again.
//
// **`baseline` is the harness, and every other row includes it.** Interest is
// denied here so that the rows measure `Survey` rather than delta building, and
// a client that is never told anything stays in its joining state - so each tick
// re-takes an empty join snapshot. That constant is what `baseline` reports, and
// it is the number to subtract before reading a row as the cost of the work.

#include <engine/core/Name.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.replication.bench.survey")

using engine::core::Name;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::replication::Authority;
using engine::replication::ChangeDetection;

namespace survey_bench {

	// How many narrow component types this suite registers.
	//
	// Twenty-four because that is the order of magnitude a real scene declares -
	// `mono.server` replicates transforms, motion, bounds, visuals, names,
	// classes, hierarchy, ownership and a handful of gameplay tags - and because
	// a miss ladder needs enough rungs for the misses to be the bulk of the
	// work.
	constexpr size_t SLOTS = 24;

	// How many wide component types it registers, for the signature ladder.
	constexpr size_t BROADS = 4;

	// Sixteen bytes: four floats, the shape of a position or a colour.
	constexpr size_t NARROW_BYTES = 16;

	// Sixty-four bytes: a `CFrame` with room to spare, and the width at which a
	// byte-at-a-time hash stops being invisible.
	constexpr size_t BROAD_BYTES = 64;

	struct Narrow {
		float Value[NARROW_BYTES / sizeof(float)] = {};
	};

	struct Broad {
		float Value[BROAD_BYTES / sizeof(float)] = {};
	};

	// The narrow types, distinct only in their template argument.
	//
	// Distinct *types* rather than one type under several names, because
	// `Components::Register` keys on the type and a second name for one type is
	// what the registry aborts on.
	template <size_t Index> struct Slot : Narrow {};

	template <size_t Index> struct Wide : Broad {};

	// This suite's component names, interned once.
	const std::vector<Name> &SlotNames() {
		static const std::vector<Name> names = [] {
			std::vector<Name> made;
			made.reserve(SLOTS);
			for (size_t slot = 0; slot < SLOTS; slot++) {
				made.emplace_back("engine.bench.replication.Slot" + std::to_string(slot));
			}
			return made;
		}();
		return names;
	}

	const std::vector<Name> &WideNames() {
		static const std::vector<Name> names = [] {
			std::vector<Name> made;
			made.reserve(BROADS);
			for (size_t slot = 0; slot < BROADS; slot++) {
				made.emplace_back("engine.bench.replication.Wide" + std::to_string(slot));
			}
			return made;
		}();
		return names;
	}

	template <size_t... Index> void RegisterSlots(std::index_sequence<Index...>) {
		(Components::Register<Slot<Index>>(SlotNames()[Index].Text()), ...);
	}

	template <size_t... Index> void RegisterWides(std::index_sequence<Index...>) {
		(Components::Register<Wide<Index>>(WideNames()[Index].Text()), ...);
	}

	// Registered once for the binary. Component ids are process-wide.
	void RegisterTypes() {
		static const bool once = [] {
			RegisterSlots(std::make_index_sequence<SLOTS>{});
			RegisterWides(std::make_index_sequence<BROADS>{});
			return true;
		}();
		(void)once;
	}

	// A world and an authority over it, built once per shape and reused.
	//
	// **Built outside the measured body on purpose.** Filling a store of ten
	// thousand entities is orders of magnitude more work than the tick being
	// measured, and a fixture rebuilt per sample would report the allocator.
	struct Fixture {
		Store World;
		Authority Server;
		uint64_t Tick = 1;

		Fixture() : World("engine.bench.replication") {}
	};

	// The shape of one fixture: what it declares and what its entities carry.
	struct Shape {
		size_t Entities = 0;

		// How many component types the authority is told to replicate.
		size_t Declared = 0;

		// How many of them the entities actually carry, taken from the front of
		// the declared list. The rest are the misses.
		size_t Carried = 0;

		// How the authority notices a change. `Signature` is the hashing path;
		// `Observed` reads the store's dirty bits.
		ChangeDetection Detection = ChangeDetection::Observed;

		// Whether the carried components are the wide ones, which is the axis
		// the hash cares about.
		bool Broad = false;

		bool operator==(const Shape &other) const {
			return Entities == other.Entities && Declared == other.Declared && Carried == other.Carried &&
				   Detection == other.Detection && Broad == other.Broad;
		}
	};

	Fixture &FixtureOf(const Shape &shape) {
		static std::vector<std::pair<Shape, std::unique_ptr<Fixture>>> built;
		for (const auto &[made, fixture] : built) {
			if (made == shape) {
				return *fixture;
			}
		}

		RegisterTypes();

		auto fixture = std::make_unique<Fixture>();

		const std::vector<Name> &names = shape.Broad ? WideNames() : SlotNames();
		for (size_t slot = 0; slot < shape.Declared; slot++) {
			fixture->Server.Replicate(names[slot], shape.Detection);
		}

		// **Nobody is interested in anything, so the rows measure `Survey`.**
		// Everything after it in `Publish` - the visibility walk, the delta,
		// the priority sort, the packing - is per client and per visible entity,
		// and letting it run would bury the one phase this suite is about under
		// work that has its own suite in `Protocol.cpp`.
		fixture->Server.SetInterest([](engine::replication::ClientId, Entity, const Store &) {
			return false;
		});

		// One client, because `Publish` returns immediately when there are
		// none - a server with nobody connected does not survey.
		fixture->Server.Admit();

		std::array<std::byte, BROAD_BYTES> value{};
		for (size_t index = 0; index < shape.Entities; index++) {
			const Entity entity = fixture->World.Create();
			for (size_t slot = 0; slot < shape.Carried; slot++) {
				const engine::ecs::ComponentId id = Components::Find(names[slot]);
				fixture->World.SetComponent(entity, id, value.data());
			}
		}

		built.emplace_back(shape, std::move(fixture));
		return *built.back().second;
	}

	// One tick of publishing over a fixture of the given shape.
	void Publish(const Shape &shape) {
		Fixture &fixture = FixtureOf(shape);
		fixture.Server.Publish(fixture.World, fixture.Tick++);
	}
}

using namespace survey_bench;

// --- what the harness itself costs ----------------------------------------------
//
// Nothing is replicated, so `Survey` resolves an empty list and builds an empty
// `Bearing`. What is left is one client's join snapshot of an empty view, retaken
// every tick because a denied client never acknowledges anything. Subtract this
// from every row below.

BENCH_PER_ITEM("Publish · 10k entities · baseline, nothing replicated", 10000) {
	Publish(Shape{10000, 0, 0, ChangeDetection::Observed, false});
}

// --- FindBearing, by declared slots against carried ones -------------------------
//
// The three rows carry the same components on the same entities and differ only
// in how many *other* component types the authority was told about. A survey
// that tests per entity is linear in that number; one that visits per archetype
// table is not.

BENCH_PER_ITEM("Publish · 10k entities · 4 declared, 4 carried", 10000) {
	Publish(Shape{10000, 4, 4, ChangeDetection::Observed, false});
}

BENCH_PER_ITEM("Publish · 10k entities · 24 declared, 4 carried", 10000) {
	Publish(Shape{10000, 24, 4, ChangeDetection::Observed, false});
}

// The pathological end, and not a contrived one: a scene loaded before its
// gameplay components exist, or a world of pure geometry on a server that
// replicates a dozen gameplay types, is exactly this row. Every declared slot is
// a miss on every entity.
BENCH_PER_ITEM("Publish · 10k entities · 24 declared, 0 carried", 10000) {
	Publish(Shape{10000, 24, 0, ChangeDetection::Observed, false});
}

// A tenth of the entities at the same shape, so the ladder says whether the cost
// is linear in the scene or worse.
BENCH_PER_ITEM("Publish · 1k entities · 24 declared, 4 carried", 1000) {
	Publish(Shape{1000, 24, 4, ChangeDetection::Observed, false});
}

// --- Resign, by the width of what it hashes --------------------------------------
//
// Four signed slots carried by every entity, at sixteen bytes and at sixty-four.
// The work is the same number of hashes over four times the bytes, and the two
// rows should not be four times apart: the mixing takes a word at a time, so
// four times the bytes is four times the *steps* of a chain that is eight bytes
// wide, not thirty-two times.

BENCH_PER_ITEM("Publish · 10k entities · 4 signed slots · 16-byte value", 10000) {
	Publish(Shape{10000, 4, 4, ChangeDetection::Signature, false});
}

BENCH_PER_ITEM("Publish · 10k entities · 4 signed slots · 64-byte value", 10000) {
	Publish(Shape{10000, 4, 4, ChangeDetection::Signature, true});
}

// Signed slots that nothing carries, which is the other half of `Resign`'s
// story: a slot with no carriers should cost one table lookup and touch no
// entity at all.
BENCH_PER_ITEM("Publish · 10k entities · 4 signed slots, 0 carried", 10000) {
	Publish(Shape{10000, 4, 0, ChangeDetection::Signature, false});
}
