// Attribute resources follow the same visible-owner selection as entity rows.
// Large values use the existing chunked overlay path without replacing the table
// with the attributes of one oversized entity.

#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/net/Packet.hpp>
#include <engine/replication/Authority.hpp>
#include <engine/replication/Replica.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>
#include <string>

TEST_SUITE_ID("engine.replication.resourcevisibility")
TEST_DEPENDS("engine.ecs.attributes")
TEST_DEPENDS("engine.replication.stream")

namespace resource_visibility_test {
	using engine::core::Name;
	using engine::ecs::AttributeValue;
	using engine::ecs::Entity;
	using engine::ecs::Store;
	using engine::replication::ChangeDetection;

	enum class Audience : uint32_t { Public, Player, Server };
	struct Record {
		Audience Scope = Audience::Public;
		uint32_t Owner = 0;
		uint32_t Revision = 0;
	};

	struct Pair {
		explicit Pair(engine::replication::AuthoritySettings settings = {}) : Authority(settings) {
			engine::ecs::Components::Register<Record>("resource_visibility_test.Record");
			engine::ecs::RegisterAttributeComponents();
			Authority.Replicate(Name("resource_visibility_test.Record"), ChangeDetection::Signature);
			Authority.Replicate(Name("ecs.AttributeTable"), ChangeDetection::Observed, true);
			Handle = Authority.Admit();
			Authority.SetInterest([this](auto client, Entity entity, const Store &store) {
				const auto *record = store.Get<Record>(entity);
				if (record == nullptr) return false;
				return record->Scope == Audience::Public
						   ? ShowPublic
						   : record->Scope == Audience::Player && record->Owner == client.Index;
			});
		}

		Entity Make(Audience audience, uint32_t owner = 0) {
			const Entity entity = Server.Create();
			Server.Set(entity, Record{audience, owner, 0});
			return entity;
		}

		void Tick(bool deliver = true, bool acknowledge = true) {
			Authority.Publish(Server, ++Now);
			for (const auto &message : Authority.Outgoing(Handle)) {
				REQUIRE(message.size() <= engine::net::Packet::MAXIMUM_MESSAGE_BYTES);
				if (deliver) {
					INFO("tick " << Now << ", message bytes " << message.size());
					REQUIRE(Replica.Receive(Client, message) == engine::replication::ApplyStatus::Ok);
				}
			}
			Server.ClearChanges();
			if (acknowledge) {
				const auto ack = Replica.Acknowledge();
				if (!ack.empty()) REQUIRE(Authority.Receive(Handle, ack));
			}
		}

		bool Wait(const std::function<bool()> &ready, int limit = 128) {
			for (int tick = 0; tick < limit && !ready(); ++tick)
				Tick();
			return ready();
		}

		Store Server{"resource-authority"};
		Store Client{"resource-replica"};
		engine::replication::Authority Authority;
		engine::replication::Replica Replica;
		engine::replication::ClientId Handle;
		uint64_t Now = 0;
		bool ShowPublic = true;
	};

	void Score(Store &store, Entity entity, int32_t number) {
		AttributeValue value;
		value.Type = engine::ecs::PropertyType::Int32;
		value.Int32 = number;
		REQUIRE(engine::ecs::SetAttribute(store, entity, Name("Score"), value));
	}

	void Text(Store &store, Entity entity, const std::string &text) {
		AttributeValue value;
		value.Type = engine::ecs::PropertyType::String;
		value.String = text;
		REQUIRE(engine::ecs::SetAttribute(store, entity, Name("Text"), value));
	}

	bool HasScore(const Store &store, Entity entity, int32_t expected) {
		AttributeValue value;
		return engine::ecs::GetAttribute(store, entity, Name("Score"), value) &&
			   value.Type == engine::ecs::PropertyType::Int32 && value.Int32 == expected;
	}

	bool HasText(const Store &store, Entity entity, const std::string &expected) {
		AttributeValue value;
		return engine::ecs::GetAttribute(store, entity, Name("Text"), value) &&
			   value.Type == engine::ecs::PropertyType::String && value.String == expected;
	}
}

using namespace resource_visibility_test;

TEST_CASE("attribute snapshots and updates exclude hidden owners", "[replication][resources]") {
	Pair pair;
	const Entity publicOwner = pair.Make(Audience::Public);
	const Entity mine = pair.Make(Audience::Player, pair.Handle.Index);
	const Entity otherPlayer = pair.Make(Audience::Player, pair.Handle.Index + 1);
	const Entity serverOnly = pair.Make(Audience::Server);
	Score(pair.Server, publicOwner, 10);
	Score(pair.Server, mine, 20);
	Score(pair.Server, otherPlayer, 30);
	Score(pair.Server, serverOnly, 40);
	REQUIRE(pair.Wait([&] { return pair.Replica.Joined(); }));
	CHECK(HasScore(pair.Client, publicOwner, 10));
	CHECK(HasScore(pair.Client, mine, 20));
	CHECK_FALSE(pair.Client.Alive(otherPlayer));
	CHECK_FALSE(pair.Client.Alive(serverOnly));
	AttributeValue hidden;
	CHECK_FALSE(engine::ecs::GetAttribute(pair.Client, otherPlayer, Name("Score"), hidden));
	CHECK_FALSE(engine::ecs::GetAttribute(pair.Client, serverOnly, Name("Score"), hidden));

	Score(pair.Server, mine, 21);
	Score(pair.Server, otherPlayer, 31);
	Score(pair.Server, serverOnly, 41);
	REQUIRE(pair.Wait([&] { return HasScore(pair.Client, mine, 21); }));
	CHECK_FALSE(engine::ecs::GetAttribute(pair.Client, otherPlayer, Name("Score"), hidden));
	CHECK_FALSE(engine::ecs::GetAttribute(pair.Client, serverOnly, Name("Score"), hidden));

	pair.ShowPublic = false;
	REQUIRE(pair.Wait([&] {
		return !pair.Client.Alive(publicOwner) &&
			   !engine::ecs::GetAttribute(pair.Client, publicOwner, Name("Score"), hidden);
	}));
	CHECK(HasScore(pair.Client, mine, 21));
	pair.ShowPublic = true;
	REQUIRE(pair.Wait([&] {
		return pair.Client.Alive(publicOwner) && HasScore(pair.Client, publicOwner, 10);
	}));
}

TEST_CASE(
	"oversized attribute updates preserve every visible owner and stop when unchanged",
	"[replication][resources]"
) {
	engine::replication::AuthoritySettings settings;
	settings.ChunkBytes = 256;
	Pair pair(settings);
	const Entity owner = pair.Make(Audience::Player, pair.Handle.Index);
	const Entity neighbor = pair.Make(Audience::Public);
	const Entity secret = pair.Make(Audience::Server);
	const std::string initial(4096, 'a');
	const std::string updated(8192, 'b');
	Text(pair.Server, owner, initial);
	Score(pair.Server, neighbor, 17);
	Text(pair.Server, secret, std::string(16384, 's'));
	REQUIRE(pair.Wait([&] { return pair.Replica.Joined(); }));
	CHECK(HasText(pair.Client, owner, initial));
	CHECK(HasScore(pair.Client, neighbor, 17));
	AttributeValue hidden;
	CHECK_FALSE(engine::ecs::GetAttribute(pair.Client, secret, Name("Text"), hidden));
	for (int tick = 0; tick < 8; ++tick)
		pair.Tick();
	const auto before = pair.Replica.Stats().Prefaces;
	Text(pair.Server, owner, updated);
	REQUIRE(pair.Wait([&] { return HasText(pair.Client, owner, updated); }));
	CHECK(pair.Replica.Stats().Prefaces > before);
	CHECK(HasScore(pair.Client, neighbor, 17));
	CHECK_FALSE(engine::ecs::GetAttribute(pair.Client, secret, Name("Text"), hidden));

	const auto completed = pair.Replica.Stats().Prefaces;
	for (int tick = 0; tick < 12; ++tick) {
		pair.Tick();
		CHECK(pair.Authority.Outgoing(pair.Handle).empty());
	}
	CHECK(pair.Replica.Stats().Prefaces == completed);

	pair.ShowPublic = false;
	REQUIRE(pair.Wait([&] {
		return !engine::ecs::GetAttribute(pair.Client, neighbor, Name("Score"), hidden);
	}));
	CHECK(HasText(pair.Client, owner, updated));
	pair.ShowPublic = true;
	REQUIRE(pair.Wait([&] { return HasScore(pair.Client, neighbor, 17); }));
	CHECK(HasText(pair.Client, owner, updated));
}

TEST_CASE(
	"a fresh snapshot without attributes permits an equal oversized resource to return",
	"[replication][resources]"
) {
	engine::replication::AuthoritySettings settings;
	settings.ChunkBytes = 256;
	settings.ResnapshotAfterTicks = 8;
	Pair pair(settings);
	const Entity owner = pair.Make(Audience::Player, pair.Handle.Index);
	const std::string text(4096, 'r');
	Text(pair.Server, owner, text);
	REQUIRE(pair.Wait([&] { return pair.Replica.Joined(); }));
	pair.Server.RemoveResource<engine::ecs::AttributeTable>();
	const auto snapshots = pair.Replica.Stats().Snapshots;
	for (int tick = 0; tick < 32 && pair.Replica.Stats().Snapshots == snapshots; ++tick) {
		pair.Server.GetMutable<Record>(owner)->Revision++;
		pair.Tick(true, false);
	}
	REQUIRE(pair.Replica.Stats().Snapshots > snapshots);
	pair.Tick();
	const auto prefaced = pair.Replica.Stats().Prefaces;
	Text(pair.Server, owner, text);
	REQUIRE(pair.Wait([&] { return pair.Replica.Stats().Prefaces > prefaced; }));
	CHECK(HasText(pair.Client, owner, text));
}
