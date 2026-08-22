// What a tick of `Authority::Publish` costs per client, and where parallel
// starts paying.
//
// **`Survey.cpp` measures the half no client pays for; this measures the half
// every client pays separately.** Once the survey has resolved the slots, built
// `Bearing` and gathered what moved, the rest of a publish is per client: a walk
// of the world through the host's interest predicate, a structural comparison
// against what that client already holds, a delta built out of the runs the
// survey gathered, and the packing. That is `N x entities` work, which is the
// shape `docs/ARCH_REVIEW.md` §F names at 64 to 200 clients against ten thousand
// entities.
//
// **The ladder varies the client count and nothing else**, because rule 5's
// second half is a number and not an opinion: below a crossover parallel is
// slower, and the crossover is higher than it looks. Every row here holds the
// same world, the same slots and the same number of moving entities, and the
// `serial` and `lanes` rows differ only in
// `AuthoritySettings::ParallelClientThreshold` - `SIZE_MAX` for one and zero for
// the other. Where the two lines cross is the default that setting carries, and
// the comment beside it says so.
//
// **Reported per client**, so a row is nanoseconds of publish per connected
// client per tick and the rows are directly comparable. A serial loop should
// read flat across the ladder; a parallel one should fall as the client count
// climbs past the point where the lanes fill.
//
// **The one-client rows are the control, and reading them first is how not to
// over-read the rest.** At one client both settings run identical code -
// `LanesFor` answers one lane and no batch is dispatched - so whatever those
// two rows differ by is what a fixture built onto a warmer heap is worth on
// this machine. Measured at about 30%, which is why this suite cannot say where
// between one and sixteen clients the crossover falls, and can say with
// confidence that by sixteen it is well past.
//
// **Five hundred of ten thousand entities move per tick**, which is a world in
// motion rather than one at rest or one where everything moves at once. A world
// at rest measures the interest and structure walks alone; a world where
// everything moves measures the priority sort, which has its own suite in
// `Protocol.cpp`. The delta path in between is what a running server spends its
// time on.

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.replication.bench.publish")

using engine::core::ByteWriter;
using engine::core::Name;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::replication::Authority;
using engine::replication::AuthoritySettings;
using engine::replication::ClientId;

namespace publish_bench {

	// How many entities the world holds. §F's number.
	constexpr size_t ENTITIES = 10000;

	// How many of them move on a tick.
	constexpr size_t MOVING = 500;

	// How many component types every entity carries. A part on `mono.server`
	// carries a transform, a motion, a name and a class.
	constexpr size_t SLOTS = 4;

	struct Value {
		float Field[4] = {};
	};

	// Distinct *types* rather than one type under four names, because
	// `Components::Register` keys on the type and a second name for one type is
	// what the registry aborts on.
	template <size_t Index> struct Row : Value {};

	const std::vector<Name> &SlotNames() {
		static const std::vector<Name> names = [] {
			std::vector<Name> made;
			for (size_t slot = 0; slot < SLOTS; slot++) {
				made.emplace_back("engine.bench.publish.Row" + std::to_string(slot));
			}
			return made;
		}();
		return names;
	}

	template <size_t... Index> void RegisterRows(std::index_sequence<Index...>) {
		(Components::Register<Row<Index>>(SlotNames()[Index].Text()), ...);
	}

	// Stops the job pool at exit, and **the constructor is not decoration**.
	//
	// Two things have to be true and they pull against each other. A pool
	// started at static initialisation would be running while `Survey.cpp`
	// measured `Resign` in this same binary, which spreads that suite's work
	// across workers its recorded baseline was taken without - so the pool is
	// started lazily, by this suite's first fixture. And a pool that is never
	// stopped hangs the binary in `exit`: `parallel::Jobs`' own static
	// destructor destroys condition variables its workers are still parked on,
	// and `pthread_cond_destroy` waits for the last waiter.
	//
	// A destructor alone does not fix that, because destruction order is the
	// reverse of *construction* order and the pool's static is constructed later
	// than this one - so it would be destroyed first, with its workers still on
	// it. Touching `Jobs` here constructs the pool before this object registers
	// its own destructor, which puts them back in the order the fix needs.
	// `Jobs::Stop` on a pool that was never started does nothing, which is what
	// makes the same call right in both places.
	struct PoolGuard {
		PoolGuard() {
			engine::parallel::Jobs::Stop();
		}
		~PoolGuard() {
			engine::parallel::Jobs::Stop();
		}
	};

	const PoolGuard Workers;

	// Registered once for the binary, and the job pool started with it.
	//
	// **A benchmark that never started the pool would measure the inline
	// fallback under both names**, which is the one way this suite could report
	// that parallel costs nothing and be wrong.
	void Prepare() {
		static const bool once = [] {
			RegisterRows(std::make_index_sequence<SLOTS>{});
			engine::parallel::Jobs::Start(0);
			return true;
		}();
		(void)once;
	}

	// A world, an authority over it, and a set of clients already joined.
	struct Fixture {
		Store World;
		Authority Server;
		std::vector<ClientId> Clients;
		std::vector<Entity> Entities;
		std::vector<engine::ecs::ComponentId> Ids;
		uint64_t Tick = 1;
		size_t Cursor = 0;

		Fixture() : World("engine.bench.publish") {}

		// Acknowledges `Tick` for every client, without a `Replica`.
		//
		// The wire form rather than a back door, because what an authority does
		// with an acknowledgement - retiring the unconfirmed rows it covers - is
		// half of what the steady state costs.
		void Acknowledge() {
			ByteWriter writer;
			WriteMessage(writer, engine::replication::Applied{Tick});
			for (const ClientId client : Clients) {
				Server.Receive(client, writer.Bytes());
			}
		}

		// Moves `MOVING` entities, publishes, and acknowledges.
		void Step() {
			for (size_t at = 0; at < MOVING; at++) {
				const Entity entity = Entities[(Cursor + at) % Entities.size()];
				const Value moved{{static_cast<float>(Tick), 0.0f, 0.0f, 0.0f}};
				World.SetComponent(entity, Ids[0], &moved);
			}
			Cursor = (Cursor + MOVING) % Entities.size();

			Server.Publish(World, Tick);
			Acknowledge();
			World.ClearChanges();
			Tick++;
		}
	};

	// The shape of one fixture: how many clients, and whether the per-client
	// loop is allowed to use the pool.
	struct Shape {
		size_t Clients = 0;
		bool Parallel = false;

		bool operator==(const Shape &other) const {
			return Clients == other.Clients && Parallel == other.Parallel;
		}
	};

	Fixture &FixtureOf(const Shape &shape) {
		static std::vector<std::pair<Shape, std::unique_ptr<Fixture>>> built;
		for (const auto &[made, fixture] : built) {
			if (made == shape) {
				return *fixture;
			}
		}

		Prepare();

		AuthoritySettings settings;

		// **Whole worlds a tick, because the join is not what this measures.**
		// The default drains a ten-thousand-entity blob over about seventy-five
		// ticks per client and bounds a tick to two of them, which is right for
		// a server and would make the fixture's warm-up the whole run.
		settings.ChunksPerTick = 65536;
		settings.JoinsPerTick = 0;
		settings.ParallelClientThreshold = shape.Parallel ? 0 : SIZE_MAX;

		auto fixture = std::make_unique<Fixture>();
		fixture->Server = Authority(settings);

		for (const Name name : SlotNames()) {
			fixture->Server.Replicate(name);
			fixture->Ids.push_back(Components::Find(name));
		}

		// **A predicate that says yes to everything, and not no predicate at
		// all.** An empty `Interest` skips the call, and what a real host pays
		// for the visibility walk is exactly that call - `mono.server`'s is two
		// binary searches - so measuring without one would measure a loop
		// nobody runs.
		fixture->Server.SetInterest([](ClientId, Entity, const Store &) { return true; });

		const Value blank{};
		for (size_t index = 0; index < ENTITIES; index++) {
			const Entity entity = fixture->World.Create();
			for (const engine::ecs::ComponentId id : fixture->Ids) {
				fixture->World.SetComponent(entity, id, &blank);
			}
			fixture->Entities.push_back(entity);
		}

		for (const engine::ecs::ComponentId id : fixture->Ids) {
			fixture->World.ObserveComponent(id);
		}

		for (size_t at = 0; at < shape.Clients; at++) {
			fixture->Clients.push_back(fixture->Server.Admit());
		}

		// Joined and settled before anything is timed: the first publish takes a
		// snapshot per client and the second is the first steady-state tick.
		for (int warm = 0; warm < 8; warm++) {
			fixture->Step();
		}

		built.emplace_back(shape, std::move(fixture));
		return *built.back().second;
	}

	void Publish(const Shape &shape) {
		FixtureOf(shape).Step();
	}
}

using namespace publish_bench;

// --- the serial loop, across the ladder -------------------------------------
//
// Flat is what a serial loop looks like: one client's publish costs what it
// costs and the tick is that times the population. A row that climbs with the
// client count is per-client work that is not per client.

BENCH_PER_ITEM("Publish · 10k entities · 1 client · serial", 1) {
	Publish(Shape{1, false});
}

BENCH_PER_ITEM("Publish · 10k entities · 2 clients · serial", 2) {
	Publish(Shape{2, false});
}

BENCH_PER_ITEM("Publish · 10k entities · 4 clients · serial", 4) {
	Publish(Shape{4, false});
}

BENCH_PER_ITEM("Publish · 10k entities · 16 clients · serial", 16) {
	Publish(Shape{16, false});
}

BENCH_PER_ITEM("Publish · 10k entities · 64 clients · serial", 64) {
	Publish(Shape{64, false});
}

BENCH_PER_ITEM("Publish · 10k entities · 128 clients · serial", 128) {
	Publish(Shape{128, false});
}

BENCH_PER_ITEM("Publish · 10k entities · 200 clients · serial", 200) {
	Publish(Shape{200, false});
}

// --- the same ladder through the job pool -----------------------------------
//
// The crossover is the row where these stop being worse than the serial row
// above them. Below it the batch dispatch and a cold lane cost more than the
// work they spread; above it the cost per client falls towards the serial cost
// over the lane count.

BENCH_PER_ITEM("Publish · 10k entities · 1 client · lanes", 1) {
	Publish(Shape{1, true});
}

BENCH_PER_ITEM("Publish · 10k entities · 2 clients · lanes", 2) {
	Publish(Shape{2, true});
}

BENCH_PER_ITEM("Publish · 10k entities · 4 clients · lanes", 4) {
	Publish(Shape{4, true});
}

BENCH_PER_ITEM("Publish · 10k entities · 16 clients · lanes", 16) {
	Publish(Shape{16, true});
}

BENCH_PER_ITEM("Publish · 10k entities · 64 clients · lanes", 64) {
	Publish(Shape{64, true});
}

BENCH_PER_ITEM("Publish · 10k entities · 128 clients · lanes", 128) {
	Publish(Shape{128, true});
}

BENCH_PER_ITEM("Publish · 10k entities · 200 clients · lanes", 200) {
	Publish(Shape{200, true});
}
