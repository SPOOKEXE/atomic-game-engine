// Priority refinement over a moving multi-component world.
//
// **Every entity has four changed rows, so the benchmark makes the reuse
// boundary visible.** The priority and refinement hooks answer per entity;
// `Authority` still orders and packs per component row. A full budget keeps
// all rows in the refinement window, which makes a tick ask refinement to
// identify the same entity four times before the memoisation returns its
// answer. This is the hot path for a server where transforms, motion, visual
// state and ownership move together.

#include <engine/core/Bytes.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.replication.bench.priority-refinement")

using engine::core::ByteWriter;
using engine::core::Name;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::replication::Authority;
using engine::replication::AuthoritySettings;
using engine::replication::ClientId;
using engine::replication::WriteMessage;

namespace priority_refinement_bench {

	constexpr size_t ENTITIES = 2000;
	constexpr size_t SLOTS = 4;
	constexpr size_t CLIENTS = 32;

	struct Value {
		float Field[4] = {};
	};

	template <size_t Index> struct Row : Value {};

	const std::vector<Name> &SlotNames() {
		static const std::vector<Name> names = [] {
			std::vector<Name> made;
			made.reserve(SLOTS);
			for (size_t slot = 0; slot < SLOTS; slot++) {
				made.emplace_back("engine.bench.priority_refinement.Row" + std::to_string(slot));
			}
			return made;
		}();
		return names;
	}

	template <size_t... Index> void RegisterRows(std::index_sequence<Index...>) {
		(Components::Register<Row<Index>>(SlotNames()[Index].Text()), ...);
	}

	void Prepare() {
		static const bool once = [] {
			RegisterRows(std::make_index_sequence<SLOTS>{});
			return true;
		}();
		(void)once;
	}

	struct Fixture {
		Store World;
		Authority Server;
		std::vector<ClientId> Clients;
		std::vector<Entity> Entities;
		std::vector<engine::ecs::ComponentId> Ids;
		uint64_t Tick = 1;
		size_t ScoreCalls = 0;
		size_t RefinementCalls = 0;

		Fixture() : World("engine.bench.priority-refinement") {}

		void Acknowledge() {
			ByteWriter writer;
			WriteMessage(writer, engine::replication::Applied{Tick});
			for (const ClientId client : Clients) {
				Server.Receive(client, writer.Bytes());
			}
		}

		void Step() {
			for (size_t entityIndex = 0; entityIndex < Entities.size(); entityIndex++) {
				for (size_t slot = 0; slot < Ids.size(); slot++) {
					const Value moved{
						{static_cast<float>(Tick),
						 static_cast<float>(entityIndex),
						 static_cast<float>(slot),
						 0.0f}
					};
					World.SetComponent(Entities[entityIndex], Ids[slot], &moved);
				}
			}

			Server.Publish(World, Tick);
			Acknowledge();
			World.ClearChanges();
			Tick++;
		}
	};

	Fixture &FixtureOf() {
		static std::unique_ptr<Fixture> fixture;
		if (fixture) {
			return *fixture;
		}

		Prepare();

		AuthoritySettings settings;
		settings.BytesPerTick = 180 * 1024;
		settings.ChunkBytes = 1024;
		settings.MessagesPerTick = 256;
		settings.ChunksPerTick = 256;
		settings.JoinsPerTick = 0;
		settings.PriorityRefinementFactor = 1;
		settings.ParallelClientThreshold = SIZE_MAX;

		fixture = std::make_unique<Fixture>();
		fixture->Server = Authority(settings);

		for (const Name name : SlotNames()) {
			fixture->Server.Replicate(name);
			fixture->Ids.push_back(Components::Find(name));
		}
		fixture->Server.SetInterest([](ClientId, Entity, const Store &) { return true; });
		Fixture *const counted = fixture.get();
		fixture->Server.SetPriority([counted](ClientId client, Entity entity) {
			counted->ScoreCalls++;
			return 1.0f / static_cast<float>(1 + ((entity.Id + client.Index) % 1024));
		});
		fixture->Server.SetPriorityRefinement([counted](ClientId, Entity, float hint) {
			counted->RefinementCalls++;
			return hint * 0.5f;
		});

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
		for (size_t client = 0; client < CLIENTS; client++) {
			fixture->Clients.push_back(fixture->Server.Admit());
		}
		for (int warm = 0; warm < 8; warm++) {
			fixture->Step();
		}
		if (fixture->ScoreCalls == 0 || fixture->RefinementCalls == 0) {
			std::abort();
		}

		// Counters prove that the warm-up reached the priority path, then the
		// timed body uses the production hooks without tally overhead.
		fixture->Server.SetPriority([](ClientId client, Entity entity) {
			return 1.0f / static_cast<float>(1 + ((entity.Id + client.Index) % 1024));
		});
		fixture->Server.SetPriorityRefinement([](ClientId, Entity, float hint) { return hint * 0.5f; });

		return *fixture;
	}

	void Publish() {
		FixtureOf().Step();
	}

	// Prints one steady-state phase sample before the benchmark consumes its
	// timed samples. Authority already records these phases for the server
	// metrics endpoint, so this reads existing instrumentation only.
	void PrintPhaseProfile() {
		static const bool printed = [] {
			FixtureOf();
			engine::core::Metrics::Clear();
			Publish();

			for (const engine::core::Histogram &histogram : engine::core::Metrics::Snapshot().Histograms) {
				const std::string_view name = histogram.Name.Text();
				if (!name.starts_with("replication.publish.")) {
					continue;
				}
				std::cout << "priority-refinement-phase\t" << name << '\t'
						  << histogram.Mean / static_cast<double>(CLIENTS) << "\tns/item\n";
			}

			engine::core::Metrics::Clear();
			return true;
		}();
		(void)printed;
	}
}

using namespace priority_refinement_bench;

BENCH_PER_ITEM("Priority refinement · 2k entities × 4 rows · 32 clients", CLIENTS) {
	PrintPhaseProfile();
	Publish();
}
