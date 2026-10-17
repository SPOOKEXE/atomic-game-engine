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

#include <engine/core/Bytes.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Defaults.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.replication.recoverywalk")
// The delta path the walk feeds.
TEST_DEPENDS("engine.replication.stream")

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

	struct Retained {
		float X = 0.0f;
	};

	void RegisterTypes() {
		static bool once = [] {
			engine::ecs::Components::Register<Tally>("recovery_walk_test.Tally");
			return true;
		}();
		(void)once;
	}

	// The immutable host policy may first be consumed by another suite.
	[[maybe_unused]] const bool SceneRegistered = [] {
		engine::scene::RegisterSceneClasses();
		return true;
	}();

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

		void Tick(bool acknowledge = true) {
			Now++;
			Authority_.Publish(Server, Now);
			for (const std::vector<std::byte> &message : Authority_.Outgoing(Handle)) {
				Replica_.Receive(Client, message);
			}
			Server.ClearChanges();

			if (acknowledge) {
				const std::vector<std::byte> ack = Replica_.Acknowledge();
				if (!ack.empty()) {
					Authority_.Receive(Handle, ack);
				}
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

TEST_CASE("recovery retires removed components and destroyed entities", "[replication][recovery]") {
	static const bool registered = [] {
		engine::ecs::Components::Register<Retained>("recovery_walk_test.Retained");
		return true;
	}();
	(void)registered;

	Pair pair(1);
	pair.Authority_.Replicate(Name("recovery_walk_test.Retained"));
	const std::vector<Entity> made = pair.Fill(3);
	for (const Entity entity : made) {
		pair.Server.Set(entity, Retained{5.0f});
	}
	REQUIRE(pair.Join());

	// Withhold acknowledgements so removing a component leaves a real recovery entry.
	for (const Entity entity : made) {
		pair.Server.Set(entity, Tally{7.0f});
	}
	pair.Tick(false);
	const Entity removed = made[0];
	const Entity destroyed = made[1];
	const Entity surviving = made[2];
	pair.Server.Remove<Tally>(removed);
	pair.Server.Destroy(destroyed);
	const Entity replacement = pair.Server.Create();
	REQUIRE(replacement != destroyed);
	pair.Server.Set(replacement, Tally{11.0f});
	pair.Server.Set(replacement, Retained{13.0f});
	pair.Server.Set(surviving, Tally{17.0f});

	const auto tally = engine::ecs::Components::Of<Tally>();
	CHECK(pair.Server.Alive(removed));
	CHECK(pair.Server.GetComponent(removed, tally) == nullptr);
	CHECK(pair.Server.GetComponent(destroyed, tally) == nullptr);
	REQUIRE(pair.Server.GetComponent(replacement, tally) != nullptr);

	for (int tick = 0; tick < 12; tick++) {
		pair.Tick(false);
		for (const auto &bytes : pair.Authority_.Outgoing(pair.Handle)) {
			engine::core::ByteReader reader(bytes);
			engine::replication::Message message;
			REQUIRE(engine::replication::ReadMessage(reader, message));
			if (message.Kind != engine::replication::MessageKind::Delta) continue;
			for (const auto &component : message.Delta.Components) {
				for (const Entity entity : component.Entities) {
					CHECK(entity != destroyed);
					if (component.Component == Name("recovery_walk_test.Tally")) CHECK(entity != removed);
				}
			}
		}
	}

	CHECK_FALSE(pair.Client.Alive(destroyed));
	REQUIRE(pair.Client.Get<Retained>(removed) != nullptr);
	CHECK(pair.Client.Get<Retained>(removed)->X == 5.0f);
	REQUIRE(pair.Client.Get<Tally>(replacement) != nullptr);
	CHECK(pair.Client.Get<Tally>(replacement)->X == 11.0f);
	REQUIRE(pair.Client.Get<Tally>(surviving) != nullptr);
	CHECK(pair.Client.Get<Tally>(surviving)->X == 17.0f);
}

TEST_CASE("transport-refused recovery rows retain their turn", "[replication][recovery]") {
	RegisterTypes();
	Store server("refused-recovery-server");
	Store client("refused-recovery-client");
	AuthoritySettings settings;
	settings.ChunkBytes = 256;
	settings.MessagesPerTick = 8;
	settings.RecoveryRowsPerTick = 32;
	Authority authority(settings);
	authority.Replicate(Name("recovery_walk_test.Tally"));
	server.Observe<Tally>();
	const ClientId handle = authority.Admit();
	Replica replica;
	uint64_t now = 0;
	for (int attempt = 0; attempt < 32 && !replica.Joined(); ++attempt) {
		authority.Publish(server, ++now);
		for (const auto &message : authority.Outgoing(handle))
			replica.Receive(client, message);
		authority.Receive(handle, replica.Acknowledge());
	}
	REQUIRE(replica.Joined());
	std::vector<Entity> made;
	for (int index = 0; index < 128; ++index) {
		const auto entity = server.Create();
		server.Set<Tally>(entity, {7.0f});
		made.push_back(entity);
	}
	size_t refused = 0;
	// Keep multipart ACKs incomplete, as on a saturated datagram link.
	// Only the first delta fits, while structural messages still arrive.
	for (int tick = 0; tick < 64; ++tick) {
		authority.Publish(server, ++now);
		bool acceptedDelta = false;
		const auto messages = authority.Outgoing(handle);
		for (size_t index = 0; index < messages.size(); ++index) {
			const auto kind = engine::replication::PeekMessageKind(messages[index]);
			if (kind == engine::replication::MessageKind::Delta && acceptedDelta) {
				authority.Unsent(handle, index);
				++refused;
				continue;
			}
			if (kind == engine::replication::MessageKind::Delta) acceptedDelta = true;
			replica.Receive(client, messages[index]);
		}
		server.ClearChanges();
	}
	REQUIRE(refused > 0);
	for (const auto entity : made) {
		REQUIRE(client.Alive(entity));
		const auto *value = client.Get<Tally>(entity);
		REQUIRE(value != nullptr);
		CHECK(value->X == 7.0f);
	}
}

TEST_CASE("dirty transforms beyond one column chunk reach their replicas", "[replication][recovery]") {
	bool completeParts = false;
	SECTION("the observed Transform alone") {}
	SECTION("complete Part creation competes with movement") {
		completeParts = true;
	}
	engine::scene::RegisterSceneClasses();
	Store server("many-transform-server");
	Store client("many-transform-client");
	AuthoritySettings settings;
	settings.MessagesPerTick = 8;
	Authority authority(settings);
	if (completeParts) {
		bool transformRegistered = false;
		for (const auto &component : engine::replication::DefaultReplicatedComponents()) {
			transformRegistered |= component.Name == "scene.Transform";
			authority.Replicate(Name(component.Name), component.Detection, component.Resource);
		}
		REQUIRE(transformRegistered);
	} else {
		authority.Replicate(Name("scene.Transform"));
	}
	server.Observe<engine::scene::Transform>();
	const auto handle = authority.Admit();
	Replica replica;
	uint64_t now = 0;
	for (int attempt = 0; attempt < 32 && !replica.Joined(); ++attempt) {
		authority.Publish(server, ++now);
		for (const auto &message : authority.Outgoing(handle))
			replica.Receive(client, message);
		authority.Receive(handle, replica.Acknowledge());
	}
	REQUIRE(replica.Joined());
	std::vector<Entity> made;
	for (int index = 0; index < 1536; ++index) {
		const auto entity =
			completeParts ? server.CreateInstance(engine::scene::PartClass()) : server.Create();
		server.Set<engine::scene::Transform>(entity, {});
		made.push_back(entity);
	}
	size_t initialPayload = 0;
	if (completeParts) {
		for (const auto &component : engine::replication::DefaultReplicatedComponents()) {
			const auto id = engine::ecs::Components::Find(Name(component.Name));
			const auto *value = server.GetComponent(made.front(), id);
			if (value == nullptr) continue;
			const auto &descriptor = engine::ecs::Components::Describe(id);
			engine::core::ByteWriter writer;
			if (descriptor.Size != 0) {
				if (descriptor.Wire.Present())
					descriptor.Wire.Write(writer, value, 1);
				else
					descriptor.Write(writer, value, 1);
			}
			initialPayload += sizeof(uint64_t) + writer.Bytes().size();
		}
	}
	INFO("one complete Part initial value payload bytes=" << initialPayload);
	std::string progress;
	for (int tick = 0; tick < 256; ++tick) {
		for (size_t index = 0; index < made.size(); ++index) {
			server.Set<engine::scene::Transform>(
				made[index],
				{engine::core::CFrame(
					engine::core::Vector3{static_cast<float>(index % 100), static_cast<float>(tick % 2), 0.0f}
				)}
			);
		}
		size_t changed = 0;
		server.EachChangedRuns(
			engine::ecs::Components::Of<engine::scene::Transform>(),
			[&](const Entity *, void *, size_t rows) { changed += rows; }
		);
		REQUIRE(changed == made.size());
		authority.Publish(server, ++now);
		size_t emittedTransforms = 0;
		for (const auto &message : authority.Outgoing(handle)) {
			engine::core::ByteReader reader(message);
			engine::replication::Message decoded;
			if (engine::replication::ReadMessage(reader, decoded) &&
				decoded.Kind == engine::replication::MessageKind::Delta)
				for (const auto &component : decoded.Delta.Components)
					if (component.Component == Name("scene.Transform"))
						emittedTransforms += component.Entities.size();
			replica.Receive(client, message);
		}
		authority.Receive(handle, replica.Acknowledge());
		server.ClearChanges();
		if ((tick + 1) % 32 == 0) {
			size_t present = 0;
			for (const auto entity : made)
				present += client.Has<engine::scene::Transform>(entity);
			progress += " tick=" + std::to_string(tick + 1) + " transforms=" + std::to_string(present) +
						" emitted=" + std::to_string(emittedTransforms) +
						" deferred=" + std::to_string(authority.Stats().Deferred) +
						" oldest=" + std::to_string(authority.Stats().Stalest) +
						" oversized=" + std::to_string(authority.Stats().Oversized) +
						" prefaces=" + std::to_string(replica.Stats().Prefaces);
		}
	}
	size_t received = 0;
	for (const auto entity : made)
		received += client.Has<engine::scene::Transform>(entity);
	INFO(progress);
	CHECK(received == made.size());
}

TEST_CASE("initial values arrive before higher-priority continuing changes", "[replication][recovery]") {
	RegisterTypes();
	Store server("initial-priority-server");
	Store client("initial-priority-client");
	AuthoritySettings settings;
	settings.ChunkBytes = 192;
	settings.MessagesPerTick = 1;
	settings.BytesPerTick = 192;
	settings.RecoveryRowsPerTick = 8;
	Authority authority(settings);
	authority.Replicate(Name("recovery_walk_test.Tally"));
	server.Observe<Tally>();
	const auto handle = authority.Admit();
	Replica replica;
	std::vector<Entity> existing;
	for (int index = 0; index < 8; ++index) {
		const auto entity = server.Create();
		server.Set<Tally>(entity, {});
		existing.push_back(entity);
	}
	uint64_t now = 0;
	const auto deliver = [&] {
		authority.Publish(server, ++now);
		for (const auto &message : authority.Outgoing(handle))
			replica.Receive(client, message);
		authority.Receive(handle, replica.Acknowledge());
		server.ClearChanges();
	};
	for (int attempt = 0; attempt < 32 && !replica.Joined(); ++attempt)
		deliver();
	REQUIRE(replica.Joined());
	authority.SetPriority([last = existing.back()](ClientId, Entity entity) {
		return entity.Id <= last.Id ? 100.0f : 0.0f;
	});
	std::vector<Entity> created;
	for (int index = 0; index < 8; ++index) {
		const auto entity = server.Create();
		server.Set<Tally>(entity, {7.0f});
		created.push_back(entity);
	}
	for (int tick = 0; tick < 8; ++tick) {
		for (const auto entity : existing)
			server.Set<Tally>(entity, {static_cast<float>(tick)});
		deliver();
	}
	for (const auto entity : created) {
		const auto *value = client.Get<Tally>(entity);
		REQUIRE(value != nullptr);
		CHECK(value->X == 7.0f);
	}
}

TEST_CASE("continuing births preserve the deadline of an ordinary update", "[replication][recovery]") {
	RegisterTypes();
	Store server("birth-pressure-server");
	Store client("birth-pressure-client");
	AuthoritySettings settings;
	settings.ChunkBytes = 192;
	settings.MessagesPerTick = 2;
	settings.BytesPerTick = 384;
	settings.RecoveryRowsPerTick = 512;
	settings.StarvationTicks = 8;
	Authority authority(settings);
	authority.Replicate(Name("recovery_walk_test.Tally"));
	server.Observe<Tally>();
	const Entity existing = server.Create();
	server.Set<Tally>(existing, {});
	const auto handle = authority.Admit();
	Replica replica;
	uint64_t now = 0;
	const auto deliver = [&] {
		authority.Publish(server, ++now);
		for (const auto &message : authority.Outgoing(handle))
			replica.Receive(client, message);
		authority.Receive(handle, replica.Acknowledge());
		server.ClearChanges();
	};
	for (int attempt = 0; attempt < 32 && !replica.Joined(); ++attempt)
		deliver();
	REQUIRE(replica.Joined());

	std::vector<Entity> births;
	uint64_t firstUpdate = 0;
	// A structure message leaves one value packet, holding two Tally rows.
	// Four births every tick therefore keep initial values over capacity.
	for (uint64_t tick = 1; tick <= 40; ++tick) {
		for (int index = 0; index < 4; ++index) {
			const Entity entity = server.Create();
			server.Set<Tally>(entity, {1000.0f});
			births.push_back(entity);
		}
		server.Set<Tally>(existing, {static_cast<float>(tick)});
		deliver();
		const auto *value = client.Get<Tally>(existing);
		REQUIRE(value != nullptr);
		if (value->X > 0.0f && firstUpdate == 0) firstUpdate = tick;
	}
	CHECK(authority.Stats().Deferred > 0);
	REQUIRE(firstUpdate > 0);
	CHECK(firstUpdate <= settings.StarvationTicks + 3);
	CHECK(client.Get<Tally>(existing)->X >= 16.0f);
	for (const Entity entity : births)
		CHECK(client.Alive(entity));
}
