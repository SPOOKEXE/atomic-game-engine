// The bounded re-offer work for rows a client has not acknowledged.
//
// The fixture joins and acknowledges its initial snapshot before timing. It then
// changes every row once and intentionally withholds subsequent acknowledgements,
// so each publish builds both changed rows and the capped recovery selection. This
// measures authority publish work under recovery pressure; it does not claim to
// measure bytes delivered by a transport.

#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

TEST_SUITE_ID("engine.replication.bench.recovery-rows")

namespace recovery_rows_bench {

	constexpr size_t ENTITIES = 512;
	constexpr size_t RECOVERY_ROWS = 128;

	struct Value {
		float Fields[8] = {};
	};

	struct Fixture {
		engine::ecs::Store Server{"engine.bench.recovery-rows.server"};
		engine::ecs::Store Client{"engine.bench.recovery-rows.client"};
		engine::replication::Authority Authority_;
		engine::replication::Replica Replica_;
		engine::replication::ClientId Handle;
		std::vector<engine::ecs::Entity> Rows;
		uint64_t Tick = 0;

		Fixture() {
			using namespace engine;
			static const bool registered = [] {
				ecs::Components::Register<Value>("engine.bench.recovery_rows.Value");
				return true;
			}();
			(void)registered;

			replication::AuthoritySettings settings;
			settings.RecoveryRowsPerTick = RECOVERY_ROWS;
			settings.BytesPerTick = 64 * 1024;
			settings.MessagesPerTick = 64;
			Authority_ = replication::Authority(settings);
			Authority_.Replicate(core::Name("engine.bench.recovery_rows.Value"));
			Server.Observe<Value>();
			Handle = Authority_.Admit();
			Rows.reserve(ENTITIES);
			for (size_t index = 0; index < ENTITIES; index++) {
				const ecs::Entity entity = Server.Create();
				Server.Set(entity, Value{});
				Rows.push_back(entity);
			}
			while (!Replica_.Joined()) {
				Publish(true);
			}
			for (const ecs::Entity entity : Rows) {
				Server.Set(entity, Value{{static_cast<float>(entity.Id)}});
			}
		}

		void Publish(bool acknowledge) {
			Tick++;
			Authority_.Publish(Server, Tick);
			for (const std::vector<std::byte> &message : Authority_.Outgoing(Handle)) {
				Replica_.Receive(Client, message);
			}
			Server.ClearChanges();
			if (acknowledge) {
				const std::vector<std::byte> acknowledgement = Replica_.Acknowledge();
				if (!acknowledgement.empty()) Authority_.Receive(Handle, acknowledgement);
			}
		}

		void Step() {
			Publish(false);
		}
	};

	Fixture &FixtureOf() {
		static std::unique_ptr<Fixture> fixture = std::make_unique<Fixture>();
		return *fixture;
	}
}

BENCH_PER_ITEM("Recovery rows · 512 outstanding rows · cap 128", recovery_rows_bench::RECOVERY_ROWS) {
	recovery_rows_bench::FixtureOf().Step();
}
