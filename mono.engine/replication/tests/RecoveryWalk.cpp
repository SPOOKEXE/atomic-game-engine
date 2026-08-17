// The bounded, rotating re-offer of what a client has not acknowledged.
//
// **The walk exists to lose nothing and the bound exists to stop it rebuilding
// the world.** Those pull against each other, so the cases here are about the
// join: that a bound smaller than the unconfirmed set still reaches every row,
// that a client behind a small bound still converges, and that zero means the
// unbounded walk that was here before.
//
// See `AuthoritySettings::RecoveryRowsPerTick`. At two hundred clients the walk
// was serialising two thousand rows a component to fill a link that took forty.

#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.replication.recoverywalk")
// The delta path the walk feeds.
TEST_DEPENDS("engine.replication.replication")

using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::replication::Authority;
using engine::replication::AuthoritySettings;
using engine::replication::ClientId;
using engine::replication::Replica;

namespace recovery_walk_test {
	struct Tally {
		float X = 0.0f;
	};

	void RegisterTypes() {
		static bool once = [] {
			engine::ecs::Components::Register<Tally>("recovery_walk_test.Tally");
			return true;
		}();
		(void)once;
	}

	struct Pair {
		explicit Pair(size_t recoveryRows) : Server("recovery_server"), Client("recovery_client") {
			RegisterTypes();

			AuthoritySettings settings;
			settings.RecoveryRowsPerTick = recoveryRows;
			Authority_ = Authority(settings);

			Authority_.Replicate(Name("recovery_walk_test.Tally"));
			Server.Observe<Tally>();
			Handle = Authority_.Admit();
		}

		void Tick() {
			Now++;
			Authority_.Publish(Server, Now);
			for (const std::vector<std::byte> &message : Authority_.Outgoing(Handle)) {
				Replica_.Receive(Client, message);
			}
			Server.ClearChanges();

			const std::vector<std::byte> ack = Replica_.Acknowledge();
			if (!ack.empty()) {
				Authority_.Receive(Handle, ack);
			}
		}

		bool Join(int limit = 256) {
			for (int attempt = 0; attempt < limit && !Replica_.Joined(); attempt++) {
				Tick();
			}
			return Replica_.Joined();
		}

		std::vector<Entity> Fill(int count) {
			std::vector<Entity> made;
			for (int index = 0; index < count; index++) {
				const Entity entity = Server.Create();
				Server.Set<Tally>(entity, Tally{0.0f});
				made.push_back(entity);
			}
			return made;
		}

		// How many of `entities` the client holds at the server's value.
		size_t Agreeing(const std::vector<Entity> &entities, float value) const {
			size_t agreeing = 0;
			for (const Entity entity : entities) {
				const Tally *held = Client.Get<Tally>(entity);
				if (held != nullptr && held->X == value) {
					agreeing++;
				}
			}
			return agreeing;
		}

		Store Server;
		Store Client;
		Authority Authority_;
		Replica Replica_;
		ClientId Handle;
		uint64_t Now = 0;
	};
}

using namespace recovery_walk_test;

TEST_CASE("a bound smaller than the world still reaches all of it", "[replication][recovery]") {
	// **The rotation, which is the half that makes the bound safe.** Eight rows
	// a tick against two hundred entities: a walk that restarted at the front
	// would send the same eight for ever and the other hundred and ninety two
	// would never arrive at all.
	Pair pair(8);
	const std::vector<Entity> made = pair.Fill(200);
	REQUIRE(pair.Join());

	for (const Entity entity : made) {
		pair.Server.Set<Tally>(entity, Tally{7.0f});
	}

	for (int tick = 0; tick < 200; tick++) {
		pair.Tick();
	}

	CHECK(pair.Agreeing(made, 7.0f) == made.size());
}

TEST_CASE("a bound does not lose a later change", "[replication][recovery]") {
	// A value that moves while an earlier one is still working its way through
	// the rotation. The walk re-offers what is unacknowledged rather than what
	// is old, so the newest value is what a row carries whenever it is reached.
	Pair pair(4);
	const std::vector<Entity> made = pair.Fill(80);
	REQUIRE(pair.Join());

	for (const Entity entity : made) {
		pair.Server.Set<Tally>(entity, Tally{1.0f});
	}
	for (int tick = 0; tick < 10; tick++) {
		pair.Tick();
	}

	for (const Entity entity : made) {
		pair.Server.Set<Tally>(entity, Tally{2.0f});
	}
	for (int tick = 0; tick < 200; tick++) {
		pair.Tick();
	}

	CHECK(pair.Agreeing(made, 2.0f) == made.size());
}

TEST_CASE("no bound is the walk from before there was one", "[replication][recovery]") {
	// Zero means unbounded, so a host that has never thought about this keeps
	// exactly what it had.
	Pair pair(0);
	const std::vector<Entity> made = pair.Fill(200);
	REQUIRE(pair.Join());

	for (const Entity entity : made) {
		pair.Server.Set<Tally>(entity, Tally{5.0f});
	}

	for (int tick = 0; tick < 200; tick++) {
		pair.Tick();
	}

	CHECK(pair.Agreeing(made, 5.0f) == made.size());
}

TEST_CASE("a bound reaches the world sooner than one tick per row", "[replication][recovery]") {
	// The bound is on the rebuild and not on the send, which is the whole claim.
	// Thirty two rows a tick over a hundred and sixty entities is five ticks to
	// visit all of them, so agreement should arrive in tens of ticks rather than
	// in a hundred and sixty.
	Pair pair(32);
	const std::vector<Entity> made = pair.Fill(160);
	REQUIRE(pair.Join());

	for (const Entity entity : made) {
		pair.Server.Set<Tally>(entity, Tally{3.0f});
	}

	int ticks = 0;
	while (ticks < 200 && pair.Agreeing(made, 3.0f) != made.size()) {
		pair.Tick();
		ticks++;
	}

	REQUIRE(pair.Agreeing(made, 3.0f) == made.size());
	CHECK(ticks < 40);
}
