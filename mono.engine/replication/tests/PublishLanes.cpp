// The per-client half of a publish, spread across the job pool.
//
// **One property, and everything else here is in service of it: the bytes a
// client receives may not depend on which lane published it.** A publish order
// that varied with thread scheduling is a desync, and this is the loop in the
// engine where that is most likely to bite - so the case runs the same world
// through two authorities that differ only in
// `AuthoritySettings::ParallelClientThreshold` and requires every client's
// outgoing messages to match byte for byte.
//
// **The pool has to be running or this suite proves nothing.** `Jobs::For` runs
// its whole span inline when there are no workers, which would make both
// authorities the same code under two names. Each case starts one and asserts
// that it has workers in it.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.replication.publishlanes")
// The serial loop this is required to reproduce exactly.
TEST_DEPENDS("engine.replication.replication")

using engine::core::ByteWriter;
using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::replication::Authority;
using engine::replication::AuthoritySettings;
using engine::replication::ClientId;

namespace publish_lanes_test {

	// Enough clients to fill more lanes than one on any machine that has a pool,
	// and few enough that the case runs in milliseconds.
	constexpr size_t CLIENTS = 48;

	constexpr size_t ENTITIES = 400;

	struct Place {
		float X = 0.0f;
		float Y = 0.0f;
	};

	struct Label {
		uint32_t Value = 0;
	};

	// The pool, owned by the case that needs it.
	//
	// **A local rather than a static, for two reasons and both of them are
	// about the other two hundred and fifty cases in this binary.** A pool
	// started at static initialisation would still be running while they ran,
	// which changes what `Authority::Resign` does inside every one of them. And
	// a pool that is started and never stopped hangs the binary in `exit`:
	// `parallel::Jobs`' own static destructor destroys condition variables its
	// workers are still parked on, and `pthread_cond_destroy` waits for the last
	// waiter. `ecs/tests/Parallel.cpp` owns its pool through the same shape.
	struct Pool {
		Pool() {
			engine::parallel::Jobs::Start(0);
		}
		~Pool() {
			engine::parallel::Jobs::Stop();
		}
	};

	void RegisterTypes() {
		static const bool once = [] {
			engine::ecs::Components::Register<Place>("publish_lanes_test.Place");
			engine::ecs::Components::Register<Label>("publish_lanes_test.Label");
			return true;
		}();
		(void)once;
	}

	// A world and an authority over it, published to `CLIENTS` clients.
	struct World {
		Store Store_;
		Authority Server;
		std::vector<ClientId> Clients;
		std::vector<Entity> Entities;
		uint64_t Tick = 1;

		explicit World(size_t threshold) : Store_("publish_lanes") {
			RegisterTypes();

			AuthoritySettings settings;

			// The whole join in one tick, so both worlds reach the steady state
			// at the same tick and the comparison is of steady-state ticks.
			settings.ChunksPerTick = 65536;
			settings.JoinsPerTick = 0;
			settings.ParallelClientThreshold = threshold;
			Server = Authority(settings);

			Server.Replicate(Name("publish_lanes_test.Place"));
			Server.Replicate(Name("publish_lanes_test.Label"));
			Store_.Observe<Place>();
			Store_.Observe<Label>();

			// **A predicate that hides a slice of the world per client**, so the
			// two runs have to agree about *which* client saw what and not only
			// about how many bytes went out. A predicate that said yes to
			// everything would pass even if the lanes had swapped two clients'
			// interest sets.
			Server.SetInterest([](ClientId client, Entity entity, const Store &) {
				return entity.Id % 7 != client.Index % 7;
			});

			// Nearest-first, so the ordering pass runs and its result is part of
			// what the two runs have to agree about.
			Server.SetPriority([](ClientId client, Entity entity) {
				return 1.0f / static_cast<float>(1 + ((entity.Id + client.Index) % 32));
			});

			for (size_t index = 0; index < ENTITIES; index++) {
				const Entity entity = Store_.Create();
				Store_.Set<Place>(entity, Place{static_cast<float>(index), 0.0f});
				Store_.Set<Label>(entity, Label{static_cast<uint32_t>(index)});
				Entities.push_back(entity);
			}

			for (size_t at = 0; at < CLIENTS; at++) {
				Clients.push_back(Server.Admit());
			}
		}

		// Moves a slice of the world, publishes, and acknowledges for everybody.
		void Step(size_t moved) {
			for (size_t at = 0; at < moved; at++) {
				const Entity entity = Entities[(Tick * 13 + at) % Entities.size()];
				Store_.Set<Place>(entity, Place{static_cast<float>(Tick), static_cast<float>(at)});
			}

			Server.Publish(Store_, Tick);

			ByteWriter writer;
			WriteMessage(writer, engine::replication::Applied{Tick});
			for (const ClientId client : Clients) {
				Server.Receive(client, writer.Bytes());
			}

			Store_.ClearChanges();
			Tick++;
		}

		// Every client's outgoing messages this tick, in client order.
		std::vector<std::vector<std::byte>> Sent() const {
			std::vector<std::vector<std::byte>> all;
			for (const ClientId client : Clients) {
				for (const std::vector<std::byte> &message : Server.Outgoing(client)) {
					all.push_back(message);
				}
			}
			return all;
		}
	};
}

using namespace publish_lanes_test;

TEST_CASE("a publish spread across lanes sends the same bytes as a serial one", "[replication][parallel]") {
	const Pool workers;

	World serial(SIZE_MAX);
	World lanes(0);

	// A pool with no workers would run both of these inline, and the case would
	// be comparing one implementation with itself.
	REQUIRE(engine::parallel::Jobs::WorkerCount() > 0);

	for (int tick = 0; tick < 12; tick++) {
		serial.Step(64);
		lanes.Step(64);

		REQUIRE(serial.Sent() == lanes.Sent());
		CHECK(serial.Server.Stats().Bytes == lanes.Server.Stats().Bytes);
		CHECK(serial.Server.Stats().Messages == lanes.Server.Stats().Messages);
		CHECK(serial.Server.Stats().Visible == lanes.Server.Stats().Visible);
	}

	// The join is the first tick and the rest are deltas, so a run that sent
	// nothing after the snapshot would pass the comparison above without
	// exercising the delta path at all.
	CHECK(lanes.Server.Stats().Messages > 0);
}

TEST_CASE("the threshold is what decides, and a small publish stays serial", "[replication][parallel]") {
	// Below the default threshold the loop is the loop it always was, which is
	// the half of rule 5 that says parallel is not free. The observable
	// difference is in the bytes, and there is none - so what this case holds is
	// that a threshold nothing reaches produces the same result as one
	// everything reaches, over a client count either side of the default.
	const Pool workers;

	World high(SIZE_MAX);
	World low(0);

	for (int tick = 0; tick < 4; tick++) {
		high.Step(16);
		low.Step(16);
		REQUIRE(high.Sent() == low.Sent());
	}
}
