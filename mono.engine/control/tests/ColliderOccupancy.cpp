#include <engine/control/Surface.hpp>
#include <engine/control/features/DataFactory.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <nlohmann/json.hpp>
#include <string>

TEST_SUITE_ID("engine.control.collider-occupancy")
TEST_DEPENDS("engine.physics.query")
TEST_DEPENDS("engine.world.data-factory")

using engine::control::Surface;
using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::physics::PreparePhysicsWorld;
using engine::physics::SyncBroadphase;
using engine::scene::Collider;
using engine::scene::Transform;
using engine::world::DataFactoryPauseScope;
using engine::world::DataFactorySession;
using engine::world::DataFactoryStatus;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;
using nlohmann::json;

namespace {
	json Call(Surface &surface, const json &arguments, bool &failed) {
		const json request{
			{"jsonrpc", "2.0"},
			{"id", 1},
			{"method", "tools/call"},
			{"params", {{"name", "get_collider_occupancy"}, {"arguments", arguments}}},
		};
		const json reply = json::parse(surface.Answer(request.dump()));
		const json &result = reply.at("result");
		failed = result.value("isError", false);
		return json::parse(result.at("content").at(0).at("text").get<std::string>());
	}

	struct Fixture {
		Universe Worlds;
		WorldId Id;
		DataFactorySession Session;
		Surface Control{"test", "test"};

		Fixture() : Id(Create()), Session(Worlds) {
			Session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
			Control.Enable(std::array{engine::control::features::DataFactory(Session)});
			Worlds.Enter(Id, [](engine::ecs::Store &) {
				engine::scene::RegisterSceneComponents();
				engine::physics::RegisterPhysicsComponents();
			});
		}

		WorldId Create() {
			WorldSettings settings;
			settings.Name = Name("occupancy");
			return Worlds.Create(settings);
		}

		Entity AddPrimitiveCollider() {
			Entity entity;
			Worlds.Enter(Id, [&](engine::ecs::Store &store) {
				PreparePhysicsWorld(store, 4.0f);
				entity = store.Create();
				store.Set(entity, Transform{CFrame{}});
				store.Set(entity, Collider{});
				SyncBroadphase(store);
			});
			return entity;
		}

		void PrepareEmptyPhysics() {
			Worlds.Enter(Id, [](engine::ecs::Store &store) {
				PreparePhysicsWorld(store, 4.0f);
				SyncBroadphase(store);
			});
		}

		json Request(const json &probes = json::array({{{"name", "origin"}, {"minimum_metres", {-0.5, -0.5, -0.5}}, {"maximum_metres", {0.5, 0.5, 0.5}}}})) {
			const auto before = Session.Inspect("occupancy");
			REQUIRE(Session.Pause("occupancy", DataFactoryPauseScope::AllSystems, before.Clock.Tick).Status == DataFactoryStatus::Ok);
			std::string snapshot;
			REQUIRE(Session.Snapshot("occupancy", snapshot).Status == DataFactoryStatus::Ok);
			const auto current = Session.Inspect("occupancy");
			return {{"schema_version", "collider-occupancy/v1"}, {"world_id", "occupancy"}, {"lifecycle", {{"tick", current.Clock.Tick}, {"world_epoch", current.WorldEpoch}, {"world_version", current.WorldVersion}}}, {"snapshot_id", snapshot}, {"probes", probes}};
		}
	};
}

TEST_CASE("collider occupancy MCP preserves an unlabelled positive witness", "[control][collider-occupancy]") {
	Fixture fixture;
	fixture.AddPrimitiveCollider();
	bool failed = false;
	const json reply = Call(fixture.Control, fixture.Request(), failed);
	INFO(reply.dump());
	CHECK_FALSE(failed);
	CHECK(reply.at("schema_version") == "collider-occupancy/v1");
	CHECK(reply.at("world_id") == "occupancy");
	REQUIRE(reply.at("probes").size() == 1);
	const json &probe = reply.at("probes").at(0);
	CHECK(probe.at("name") == "origin");
	CHECK(probe.at("available") == true);
	CHECK(probe.at("overlap_found") == true);
	CHECK(probe.at("witness_identity_available") == false);
	CHECK(probe.at("witness_id").is_null());
	CHECK(probe.at("complete") == true);
	CHECK(probe.at("reason").is_null());
}

TEST_CASE("collider occupancy MCP refuses stale lifecycle fences", "[control][collider-occupancy]") {
	Fixture fixture;
	json request = fixture.Request();
	bool failed = false;
	request["lifecycle"]["world_version"] = request["lifecycle"]["world_version"].get<uint64_t>() + 1;
	const json revision = Call(fixture.Control, request, failed);
	CHECK(failed);
	CHECK(revision.dump().find("version_conflict") != std::string::npos);

	request = fixture.Request();
	fixture.Worlds.Enter(fixture.Id, [](engine::ecs::Store &store) { store.Create(); });
	const json stale = Call(fixture.Control, request, failed);
	CHECK(failed);
	CHECK(stale.dump().find("stale_snapshot") != std::string::npos);
}

TEST_CASE("collider occupancy MCP validates strict bounded probes", "[control][collider-occupancy]") {
	Fixture fixture;
	bool failed = false;
	json malformed = fixture.Request();
	malformed["probes"][0]["maximum_metres"] = json::array({-0.5, 0.5, 0.5});
	const json invalid = Call(fixture.Control, malformed, failed);
	CHECK(failed);
	CHECK(invalid.dump().find("validation_failed") != std::string::npos);

	json many = json::array();
	for (size_t index = 0; index < 33; ++index)
		many.push_back({{"name", "probe" + std::to_string(index)}, {"minimum_metres", {-1.0, -1.0, -1.0}}, {"maximum_metres", {1.0, 1.0, 1.0}}});
	const json limited = Call(fixture.Control, fixture.Request(many), failed);
	CHECK(failed);
	CHECK(limited.dump().find("validation_failed") != std::string::npos);
}

TEST_CASE("collider occupancy MCP marks an unprepared index unknown", "[control][collider-occupancy]") {
	Fixture fixture;
	bool failed = false;
	const json reply = Call(fixture.Control, fixture.Request(), failed);
	CHECK_FALSE(failed);
	const json &probe = reply.at("probes").at(0);
	CHECK(probe.at("available") == false);
	CHECK(probe.at("overlap_found").is_null());
	CHECK(probe.at("complete") == false);
	CHECK(probe.at("reason") == "physics_unprepared");
}

TEST_CASE("collider occupancy MCP returns a complete negative Boolean row", "[control][collider-occupancy]") {
	Fixture fixture;
	fixture.PrepareEmptyPhysics();
	bool failed = false;
	const json reply = Call(fixture.Control, fixture.Request(), failed);
	CHECK_FALSE(failed);
	const json &probe = reply.at("probes").at(0);
	CHECK(probe.at("available") == true);
	CHECK(probe.at("overlap_found") == false);
	CHECK(probe.at("complete") == true);
	CHECK(probe.at("reason").is_null());
}

TEST_CASE("collider occupancy MCP echoes caller decimal bounds exactly", "[control][collider-occupancy]") {
	Fixture fixture;
	fixture.PrepareEmptyPhysics();
	const json probes = json::array({{{"name", "decimal"}, {"minimum_metres", {0.1, 0.2, 0.3}}, {"maximum_metres", {2.9, 3.8, 4.7}}}});
	bool failed = false;
	const json reply = Call(fixture.Control, fixture.Request(probes), failed);
	CHECK_FALSE(failed);
	const json &probe = reply.at("probes").at(0);
	CHECK(probe.at("minimum_metres") == probes.at(0).at("minimum_metres"));
	CHECK(probe.at("maximum_metres") == probes.at(0).at("maximum_metres"));
}

TEST_CASE("collider occupancy MCP withholds an ambiguous witness identity", "[control][collider-occupancy]") {
	Fixture fixture;
	const Entity first = fixture.AddPrimitiveCollider();
	fixture.Worlds.Enter(fixture.Id, [&](engine::ecs::Store &store) {
		engine::ecs::AttributeValue value;
		value.Type = engine::ecs::PropertyType::String;
		value.String = "shared/collider";
		store.Set(first, engine::ecs::InstanceName{Name("First")});
		REQUIRE(engine::ecs::SetAttribute(store, first, Name("DataFactoryId"), value));
		const Entity duplicate = store.Create();
		store.Set(duplicate, Transform{CFrame{Vector3{5.0f, 0.0f, 0.0f}}});
		store.Set(duplicate, Collider{});
		store.Set(duplicate, engine::ecs::InstanceName{Name("Second")});
		REQUIRE(engine::ecs::SetAttribute(store, duplicate, Name("DataFactoryId"), value));
		SyncBroadphase(store);
	});
	bool failed = false;
	const json reply = Call(fixture.Control, fixture.Request(), failed);
	CHECK_FALSE(failed);
	const json &probe = reply.at("probes").at(0);
	CHECK(probe.at("overlap_found") == true);
	CHECK(probe.at("witness_identity_available") == false);
	CHECK(probe.at("witness_id").is_null());
	CHECK(probe.at("reason").is_null());
}

TEST_CASE("collider occupancy MCP withholds a witness ID duplicated by a non-collider instance", "[control][collider-occupancy]") {
	Fixture fixture;
	const Entity collider = fixture.AddPrimitiveCollider();
	fixture.Worlds.Enter(fixture.Id, [&](engine::ecs::Store &store) {
		engine::ecs::AttributeValue value;
		value.Type = engine::ecs::PropertyType::String;
		value.String = "shared/instance";
		store.Set(collider, engine::ecs::InstanceName{Name("Collider")});
		REQUIRE(engine::ecs::SetAttribute(store, collider, Name("DataFactoryId"), value));
		const Entity folder = store.Create();
		store.Set(folder, engine::ecs::InstanceName{Name("Folder")});
		REQUIRE(engine::ecs::SetAttribute(store, folder, Name("DataFactoryId"), value));
		SyncBroadphase(store);
	});
	bool failed = false;
	const json reply = Call(fixture.Control, fixture.Request(), failed);
	CHECK_FALSE(failed);
	CHECK(reply.at("probes").at(0).at("overlap_found") == true);
	CHECK(reply.at("probes").at(0).at("witness_identity_available") == false);
	CHECK(reply.at("probes").at(0).at("witness_id").is_null());
}

TEST_CASE("collider occupancy MCP suppresses malformed UTF-8 witness IDs", "[control][collider-occupancy]") {
	Fixture fixture;
	const Entity collider = fixture.AddPrimitiveCollider();
	fixture.Worlds.Enter(fixture.Id, [&](engine::ecs::Store &store) {
		engine::ecs::AttributeValue value;
		value.Type = engine::ecs::PropertyType::String;
		value.String = std::string("bad\xC0\xAF", 5);
		store.Set(collider, engine::ecs::InstanceName{Name("Collider")});
		REQUIRE(engine::ecs::SetAttribute(store, collider, Name("DataFactoryId"), value));
		SyncBroadphase(store);
	});
	bool failed = false;
	const json reply = Call(fixture.Control, fixture.Request(), failed);
	CHECK_FALSE(failed);
	CHECK(reply.at("probes").at(0).at("overlap_found") == true);
	CHECK(reply.at("probes").at(0).at("witness_identity_available") == false);
	CHECK(reply.at("probes").at(0).at("witness_id").is_null());
}
