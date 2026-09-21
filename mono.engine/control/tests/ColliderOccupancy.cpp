#include <engine/control/Surface.hpp>
#include <engine/control/features/DataFactory.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/AuthoredAffordance.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
	json Call(
		Surface &surface,
		const json &arguments,
		bool &failed,
		std::string_view name = "get_collider_occupancy"
	) {
		const json request{
			{"jsonrpc", "2.0"},
			{"id", 1},
			{"method", "tools/call"},
			{"params", {{"name", name}, {"arguments", arguments}}},
		};
		const json reply = json::parse(surface.Answer(request.dump()));
		const json &result = reply.at("result");
		failed = result.value("isError", false);
		return json::parse(result.at("content").at(0).at("text").get<std::string>());
	}

	json Tools(Surface &surface) {
		const json request{
			{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}, {"params", json::object()}
		};
		return json::parse(surface.Answer(request.dump())).at("result").at("tools");
	}

	struct Fixture {
		Universe Worlds;
		DataFactorySession Session;
		WorldId Id;
		Surface Control{"test", "test"};

		Fixture() : Session(Worlds) {
			Session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) {
				return true;
			});
			Id = Create();
			Control.Enable(std::array{engine::control::features::DataFactory(Session)});
			Worlds.Enter(Id, [](engine::ecs::Store &) {
				engine::scene::RegisterSceneComponents();
				engine::physics::RegisterPhysicsComponents();
			});
		}

		WorldId Create() {
			const engine::world::DataFactoryWorldRequest request{
				.Operation = engine::world::DataFactoryWorldOperation::Create,
				.InstanceId = "occupancy",
				.TickRate = 60.0,
				.OperationId = "occupancy-create",
			};
			REQUIRE(Session.CreateWorld(request).Status == DataFactoryStatus::Ok);
			return Worlds.Find(Name("occupancy"));
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

		json Request(
			const json &probes = json::array(
				{{{"name", "origin"},
				  {"minimum_metres", {-0.5, -0.5, -0.5}},
				  {"maximum_metres", {0.5, 0.5, 0.5}}}}
			)
		) {
			const auto before = Session.Inspect("occupancy");
			REQUIRE(
				Session.Pause("occupancy", DataFactoryPauseScope::AllSystems, before.Clock.Tick).Status ==
				DataFactoryStatus::Ok
			);
			std::string snapshot;
			REQUIRE(Session.Snapshot("occupancy", snapshot).Status == DataFactoryStatus::Ok);
			const auto current = Session.Inspect("occupancy");
			return {
				{"schema_version", "collider-occupancy/v1"},
				{"world_id", "occupancy"},
				{"lifecycle",
				 {{"tick", current.Clock.Tick},
				  {"world_epoch", current.WorldEpoch},
				  {"world_version", current.WorldVersion}}},
				{"snapshot_id", snapshot},
				{"probes", probes}
			};
		}

		json BevRequest(uint8_t rows = 2, uint8_t columns = 2) {
			const auto before = Session.Inspect("occupancy");
			REQUIRE(
				Session.Pause("occupancy", DataFactoryPauseScope::AllSystems, before.Clock.Tick).Status ==
				DataFactoryStatus::Ok
			);
			std::string snapshot;
			REQUIRE(Session.Snapshot("occupancy", snapshot).Status == DataFactoryStatus::Ok);
			const auto current = Session.Inspect("occupancy");
			return {
				{"schema_version", "collider-bev/v1"},
				{"world_id", "occupancy"},
				{"lifecycle",
				 {{"tick", current.Clock.Tick},
				  {"world_epoch", current.WorldEpoch},
				  {"world_version", current.WorldVersion}}},
				{"snapshot_id", snapshot},
				{"xz_bounds_metres", {{"minimum", {0.0, 0.0}}, {"maximum", {2.0, 2.0}}}},
				{"y_minimum_metres", -1.0},
				{"y_maximum_metres", 1.0},
				{"rows", rows},
				{"columns", columns},
			};
		}

		json FilledRequest(uint8_t layers = 2, uint8_t rows = 2, uint8_t columns = 2) {
			const auto before = Session.Inspect("occupancy");
			REQUIRE(
				Session.Pause("occupancy", DataFactoryPauseScope::AllSystems, before.Clock.Tick).Status ==
				DataFactoryStatus::Ok
			);
			std::string snapshot;
			REQUIRE(Session.Snapshot("occupancy", snapshot).Status == DataFactoryStatus::Ok);
			const auto current = Session.Inspect("occupancy");
			return {
				{"schema_version", "filled-occupancy/v1"},
				{"world_id", "occupancy"},
				{"lifecycle",
				 {{"tick", current.Clock.Tick},
				  {"world_epoch", current.WorldEpoch},
				  {"world_version", current.WorldVersion}}},
				{"snapshot_id", snapshot},
				{"minimum_metres", {-2.0, -3.0, -5.0}},
				{"maximum_metres", {2.0, 3.0, 5.0}},
				{"columns", columns},
				{"rows", rows},
				{"layers", layers},
			};
		}

		json NavmeshRequest() {
			json request = FilledRequest(1, 1, 1);
			request["schema_version"] = "authored-navmesh-path/v1";
			request.erase("minimum_metres");
			request.erase("maximum_metres");
			request.erase("columns");
			request.erase("rows");
			request.erase("layers");
			request["start_metres"] = {-0.5, 0.5, 0.0};
			request["goal_metres"] = {2.5, 0.5, 0.0};
			return request;
		}
	};
}

TEST_CASE("authored navmesh MCP is snapshot fenced and keeps unknown routes honest", "[control][navmesh]") {
	Fixture fixture;
	fixture.Worlds.Enter(fixture.Id, [](engine::ecs::Store &store) {
		auto part = [](Vector3 position) {
			return engine::scene::PartDesc{
				CFrame{position}, {2, 1, 2}, {}, {}, {}, engine::scene::ShapeKind::Box, false
			};
		};
		const Entity left = engine::scene::MakePart(store, part({0, 0, 0}));
		const Entity right = engine::scene::MakePart(store, part({2, 0, 0}));
		store.Set<engine::scene::AuthoredAffordance>(
			left, {Name("nav/left"), engine::scene::AuthoredAffordanceKind::Walkable, true}
		);
		store.Set<engine::scene::AuthoredAffordance>(
			right, {Name("nav/right"), engine::scene::AuthoredAffordanceKind::Walkable, true}
		);
		PreparePhysicsWorld(store, 4.0f);
		SyncBroadphase(store);
	});
	json request = fixture.NavmeshRequest();
	bool failed = false;
	const json reply = Call(fixture.Control, request, failed, "get_authored_navmesh_path");
	CHECK_FALSE(failed);
	CHECK(reply.at("status") == "path_found");
	CHECK(reply.at("snapshot_id") == request.at("snapshot_id"));
	json stale = request;
	stale["lifecycle"]["world_version"] = stale["lifecycle"]["world_version"].get<uint64_t>() + 1;
	const json rejected = Call(fixture.Control, stale, failed, "get_authored_navmesh_path");
	CHECK(failed);
	CHECK(rejected.dump().find("version_conflict") != std::string::npos);
	json missing = request;
	missing["snapshot_id"] = "missing";
	Call(fixture.Control, missing, failed, "get_authored_navmesh_path");
	CHECK(failed);
}

TEST_CASE(
	"filled occupancy MCP reports only wholly contained analytic cells", "[control][filled-occupancy]"
) {
	Fixture fixture;
	fixture.Worlds.Enter(fixture.Id, [](engine::ecs::Store &store) {
		PreparePhysicsWorld(store, 4.0f);
		const Entity collider = store.Create();
		store.Set(collider, Transform{CFrame{Vector3{-1.0f, -1.5f, -2.5f}}});
		Collider shape;
		shape.Extent = {1.0f, 1.5f, 2.5f};
		store.Set(collider, shape);
		SyncBroadphase(store);
	});
	bool failed = false;
	const json reply = Call(fixture.Control, fixture.FilledRequest(), failed, "get_filled_occupancy");
	INFO(reply.dump());
	CHECK_FALSE(failed);
	CHECK(reply.at("schema_version") == "filled-occupancy/v1");
	CHECK(reply.at("cell_order") == "y_then_z_then_x");
	REQUIRE(reply.at("cells").size() == 8);
	CHECK(reply.at("cells").at(0).at("state") == "filled");
	CHECK(reply.at("cells").at(0).at("filled") == true);
	CHECK(reply.at("cells").at(4).at("layer") == 1);
	CHECK(reply.at("cells").at(4).at("row") == 0);
	CHECK(reply.at("cells").at(4).at("column") == 0);
	CHECK(reply.at("cells").at(4).at("minimum_metres") == json::array({-2.0, 0.0, -5.0}));
	CHECK(reply.at("cells").at(4).at("maximum_metres") == json::array({0.0, 3.0, 0.0}));
	CHECK(reply.at("cells").at(4).at("state") == "unknown");
	CHECK(reply.at("cells").at(4).at("reason") == "unproven_coverage");
	CHECK(reply.at("cells").at(7).at("state") == "unknown");
	CHECK(reply.at("cells").at(7).at("filled").is_null());
}

TEST_CASE(
	"signed distance MCP fences lifecycle and preserves conservative samples", "[control][signed-distance]"
) {
	Fixture fixture;
	const json tools = Tools(fixture.Control);
	const auto found = std::find_if(tools.begin(), tools.end(), [](const json &tool) {
		return tool.at("name") == "get_signed_distance_field";
	});
	REQUIRE(found != tools.end());
	CHECK(
		found->at("inputSchema").at("properties").at("schema_version").at("const") ==
		"signed-distance-field/v1"
	);
	CHECK(found->at("inputSchema").at("properties").at("columns").at("maximum") == 4);
	fixture.Worlds.Enter(fixture.Id, [](engine::ecs::Store &store) {
		PreparePhysicsWorld(store, 4.0f);
		const Entity collider = store.Create();
		store.Set(collider, Transform{CFrame{}});
		store.Set(collider, Collider{});
		engine::ecs::AttributeValue id;
		id.Type = engine::ecs::PropertyType::String;
		id.String = "sdf/box";
		REQUIRE(engine::ecs::SetAttribute(store, collider, Name("DataFactoryId"), id));
		SyncBroadphase(store);
	});
	json request = fixture.FilledRequest(2, 2, 2);
	request["schema_version"] = "signed-distance-field/v1";
	bool failed = false;
	const json reply = Call(fixture.Control, request, failed, "get_signed_distance_field");
	CHECK_FALSE(failed);
	CHECK(reply.at("sign_convention") == "negative_inside_zero_surface_positive_outside");
	CHECK(reply.at("units") == "metres");
	CHECK(reply.at("cell_order") == "y_then_z_then_x");
	REQUIRE(reply.at("samples").size() == 8);
	CHECK(reply.at("samples").at(0).at("layer") == 0);
	CHECK(reply.at("samples").at(4).at("layer") == 1);
	CHECK(reply.at("samples").at(4).at("row") == 0);
	CHECK(reply.at("samples").at(4).at("column") == 0);
	CHECK(reply.at("samples").at(0).at("state") == "known");
	CHECK(reply.at("samples").at(0).at("witness_id") == "sdf/box");

	json collapsed = request;
	collapsed["minimum_metres"][0] = 1.0e30;
	collapsed["maximum_metres"][0] = 1.0e30 + 1.0e20;
	const json invalid = Call(fixture.Control, collapsed, failed, "get_signed_distance_field");
	CHECK(failed);
	json collapsedCentres = request;
	collapsedCentres["minimum_metres"][0] = 1.0000001192092896;
	collapsedCentres["maximum_metres"][0] = 1.0000003576278687;
	collapsedCentres["columns"] = 2;
	failed = false;
	Call(fixture.Control, collapsedCentres, failed, "get_signed_distance_field");
	CHECK(failed);
	CHECK(invalid.dump().find("validation_failed") != std::string::npos);
	for (const char *field : {"tick", "world_epoch", "world_version"}) {
		json mismatched = request;
		mismatched["lifecycle"][field] = mismatched["lifecycle"][field].get<uint64_t>() + 1;
		const json rejected = Call(fixture.Control, mismatched, failed, "get_signed_distance_field");
		CHECK(failed);
		CHECK(rejected.dump().find("version_conflict") != std::string::npos);
	}
}

TEST_CASE("collider BEV MCP discovery advertises the strict grid schema", "[control][collider-bev]") {
	Fixture fixture;
	const json tools = Tools(fixture.Control);
	const auto found = std::find_if(tools.begin(), tools.end(), [](const json &tool) {
		return tool.at("name") == "get_collider_bev";
	});
	REQUIRE(found != tools.end());
	const json &schema = found->at("inputSchema");
	CHECK(schema.at("additionalProperties") == false);
	CHECK(schema.at("properties").at("schema_version").at("const") == "collider-bev/v1");
	CHECK(schema.at("properties").at("rows").at("minimum") == 1);
	CHECK(schema.at("properties").at("rows").at("maximum") == 8);
	CHECK(schema.at("properties").at("columns").at("minimum") == 1);
	CHECK(schema.at("properties").at("columns").at("maximum") == 8);
	CHECK(schema.at("properties").at("xz_bounds_metres").at("additionalProperties") == false);
}

TEST_CASE("collider BEV MCP emits z-major per-cell AABBs and contact states", "[control][collider-bev]") {
	Fixture fixture;
	fixture.Worlds.Enter(fixture.Id, [](engine::ecs::Store &store) {
		PreparePhysicsWorld(store, 4.0f);
		const Entity collider = store.Create();
		store.Set(collider, Transform{CFrame{Vector3{1.5f, 0.0f, 1.5f}}});
		store.Set(collider, Collider{});
		SyncBroadphase(store);
	});
	bool failed = false;
	const json reply = Call(fixture.Control, fixture.BevRequest(), failed, "get_collider_bev");
	INFO(reply.dump());
	CHECK_FALSE(failed);
	CHECK(reply.at("schema_version") == "collider-bev/v1");
	CHECK(reply.at("row_order") == "z_major_then_x");
	REQUIRE(reply.at("cells").size() == 4);
	const json &first = reply.at("cells").at(0);
	CHECK(first.at("row") == 0);
	CHECK(first.at("column") == 0);
	CHECK(first.at("minimum_metres") == json{0.0, -1.0, 0.0});
	CHECK(first.at("maximum_metres") == json{1.0, 1.0, 1.0});
	CHECK(first.at("state") == "empty");
	const json &last = reply.at("cells").at(3);
	CHECK(last.at("row") == 1);
	CHECK(last.at("column") == 1);
	CHECK(last.at("minimum_metres") == json{1.0, -1.0, 1.0});
	CHECK(last.at("maximum_metres") == json{2.0, 1.0, 2.0});
	CHECK(last.at("state") == "occupied");
	CHECK(last.at("occupied") == true);
}

TEST_CASE("collider BEV MCP preserves asymmetric finite float endpoints", "[control][collider-bev]") {
	Fixture fixture;
	fixture.PrepareEmptyPhysics();
	const float minimumX = -1.0e30f;
	const float maximumX = 1.0f;
	json request = fixture.BevRequest(1, 1);
	request["xz_bounds_metres"] = {{"minimum", {minimumX, -1.0f}}, {"maximum", {maximumX, 1.0f}}};
	bool failed = false;
	const json reply = Call(fixture.Control, request, failed, "get_collider_bev");
	CHECK_FALSE(failed);
	const json &cell = reply.at("cells").at(0);
	CHECK(cell.at("minimum_metres").at(0) == minimumX);
	CHECK(cell.at("maximum_metres").at(0) == maximumX);
}

TEST_CASE(
	"collider BEV MCP rejects collapsed cells and reports unprepared cells unknown", "[control][collider-bev]"
) {
	Fixture fixture;
	bool failed = false;
	const json unknown = Call(fixture.Control, fixture.BevRequest(), failed, "get_collider_bev");
	CHECK_FALSE(failed);
	CHECK(unknown.at("cells").at(0).at("state") == "unknown");
	CHECK(unknown.at("cells").at(0).at("reason") == "physics_unprepared");

	Fixture collapsed;
	json request = collapsed.BevRequest();
	request["xz_bounds_metres"]["minimum"][0] = 1.0e30;
	request["xz_bounds_metres"]["maximum"][0] = 1.0e30 + 1.0e20;
	const json invalid = Call(collapsed.Control, request, failed, "get_collider_bev");
	CHECK(failed);
	CHECK(invalid.dump().find("validation_failed") != std::string::npos);

	Fixture stale;
	request = stale.BevRequest();
	stale.Worlds.Enter(stale.Id, [](engine::ecs::Store &store) { store.Create(); });
	const json staleReply = Call(stale.Control, request, failed, "get_collider_bev");
	CHECK(failed);
	CHECK(staleReply.dump().find("stale_snapshot") != std::string::npos);

	Fixture foreign;
	request = foreign.BevRequest();
	WorldSettings foreignSettings;
	foreignSettings.Name = Name("foreign");
	REQUIRE(foreign.Worlds.Create(foreignSettings).IsValid());
	request["world_id"] = "foreign";
	const json foreignReply = Call(foreign.Control, request, failed, "get_collider_bev");
	CHECK(failed);
	CHECK(foreignReply.dump().find("validation_failed") != std::string::npos);

	Fixture malformed;
	request = malformed.BevRequest();
	request["lifecycle"] = json::array();
	const json malformedReply = Call(malformed.Control, request, failed, "get_collider_bev");
	CHECK(failed);
	CHECK(malformedReply.dump().find("validation_failed") != std::string::npos);

	Fixture denormal;
	request = denormal.BevRequest();
	request["y_minimum_metres"] = 0.0;
	request["y_maximum_metres"] = std::numeric_limits<float>::denorm_min();
	const json denormalReply = Call(denormal.Control, request, failed, "get_collider_bev");
	CHECK(failed);
	CHECK(denormalReply.dump().find("validation_failed") != std::string::npos);
}

TEST_CASE(
	"collider BEV MCP keeps only globally unique unnamed witness identities", "[control][collider-bev]"
) {
	Fixture fixture;
	Entity collider;
	fixture.Worlds.Enter(fixture.Id, [&](engine::ecs::Store &store) {
		PreparePhysicsWorld(store, 4.0f);
		collider = store.Create();
		store.Set(collider, Transform{CFrame{Vector3{1.5f, 0.0f, 1.5f}}});
		store.Set(collider, Collider{});
		engine::ecs::AttributeValue value;
		value.Type = engine::ecs::PropertyType::String;
		value.String = "unnamed/unique";
		REQUIRE(engine::ecs::SetAttribute(store, collider, Name("DataFactoryId"), value));
		SyncBroadphase(store);
	});
	bool failed = false;
	json reply = Call(fixture.Control, fixture.BevRequest(), failed, "get_collider_bev");
	CHECK_FALSE(failed);
	CHECK(reply.at("cells").at(3).at("witness_identity_available") == true);
	CHECK(reply.at("cells").at(3).at("witness_id") == "unnamed/unique");

	fixture.Worlds.Enter(fixture.Id, [](engine::ecs::Store &store) {
		const Entity duplicate = store.Create();
		engine::ecs::AttributeValue value;
		value.Type = engine::ecs::PropertyType::String;
		value.String = "unnamed/unique";
		REQUIRE(engine::ecs::SetAttribute(store, duplicate, Name("DataFactoryId"), value));
	});
	reply = Call(fixture.Control, fixture.BevRequest(), failed, "get_collider_bev");
	CHECK_FALSE(failed);
	CHECK(reply.at("cells").at(3).at("witness_identity_available") == false);
	CHECK(reply.at("cells").at(3).at("witness_id").is_null());
}

TEST_CASE(
	"collider BEV MCP fences each lifecycle revision and foreign snapshots", "[control][collider-bev]"
) {
	Fixture fixture;
	bool failed = false;
	json request = fixture.BevRequest();
	for (const char *field : {"tick", "world_epoch", "world_version"}) {
		json mismatched = request;
		mismatched["lifecycle"][field] = mismatched["lifecycle"][field].get<uint64_t>() + 1;
		const json reply = Call(fixture.Control, mismatched, failed, "get_collider_bev");
		CHECK(failed);
		CHECK(reply.dump().find("version_conflict") != std::string::npos);
	}

	Fixture foreign;
	json foreignRequest = foreign.BevRequest();
	foreignRequest = foreign.BevRequest();
	foreignRequest = foreign.BevRequest();
	request = fixture.BevRequest();
	request["snapshot_id"] = foreignRequest.at("snapshot_id");
	const json reply = Call(fixture.Control, request, failed, "get_collider_bev");
	CHECK(failed);
	CHECK(reply.dump().find("stale_snapshot") != std::string::npos);
}

TEST_CASE("collider BEV MCP maps incomplete physics answers to unknown cells", "[control][collider-bev]") {
	const auto requireUnknown = [](const json &reply, std::string_view reason) {
		const json &cell = reply.at("cells").at(3);
		CHECK(cell.at("state") == "unknown");
		CHECK(cell.at("occupied").is_null());
		CHECK(cell.at("complete") == false);
		CHECK(cell.at("reason") == reason);
		CHECK(cell.at("witness_id").is_null());
		CHECK(cell.at("witness_identity_available") == false);
	};

	Fixture stale;
	stale.Worlds.Enter(stale.Id, [](engine::ecs::Store &store) {
		PreparePhysicsWorld(store, 4.0f);
		const Entity collider = store.Create();
		store.Set(collider, Transform{CFrame{Vector3{1.5f, 0.0f, 1.5f}}});
		store.Set(collider, Collider{});
		SyncBroadphase(store);
		store.Set(collider, Transform{CFrame{Vector3{1.6f, 0.0f, 1.6f}}});
	});
	bool failed = false;
	json reply = Call(stale.Control, stale.BevRequest(), failed, "get_collider_bev");
	CHECK_FALSE(failed);
	requireUnknown(reply, "physics_stale");

	Fixture baked;
	baked.Worlds.Enter(baked.Id, [](engine::ecs::Store &store) {
		PreparePhysicsWorld(store, 4.0f);
		const Entity collider = store.Create();
		store.Set(collider, Transform{CFrame{Vector3{1.5f, 0.0f, 1.5f}}});
		Collider shape;
		shape.Shape = engine::scene::ShapeKind::Hull;
		store.Set(collider, shape);
		SyncBroadphase(store);
	});
	reply = Call(baked.Control, baked.BevRequest(), failed, "get_collider_bev");
	CHECK_FALSE(failed);
	requireUnknown(reply, "baked_geometry_uncertain");

	Fixture overflow;
	overflow.Worlds.Enter(overflow.Id, [](engine::ecs::Store &store) {
		PreparePhysicsWorld(store, 4.0f);
		for (size_t index = 0; index <= engine::physics::QUERY_CANDIDATE_LIMIT; ++index) {
			const Entity collider = store.Create();
			store.Set(collider, Transform{CFrame{Vector3{1.5f, 0.0f, 1.5f}}});
			store.Set(collider, Collider{});
		}
		SyncBroadphase(store);
	});
	reply = Call(overflow.Control, overflow.BevRequest(), failed, "get_collider_bev");
	CHECK_FALSE(failed);
	requireUnknown(reply, "candidate_overflow");
}

TEST_CASE(
	"collider occupancy MCP preserves an unlabelled positive witness", "[control][collider-occupancy]"
) {
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
		many.push_back(
			{{"name", "probe" + std::to_string(index)},
			 {"minimum_metres", {-1.0, -1.0, -1.0}},
			 {"maximum_metres", {1.0, 1.0, 1.0}}}
		);
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
	const json probes = json::array(
		{{{"name", "decimal"}, {"minimum_metres", {0.1, 0.2, 0.3}}, {"maximum_metres", {2.9, 3.8, 4.7}}}}
	);
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

TEST_CASE(
	"collider occupancy MCP withholds a witness ID duplicated by a non-collider instance",
	"[control][collider-occupancy]"
) {
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
