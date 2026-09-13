#include <engine/control/Surface.hpp>
#include <engine/control/features/DataScene.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.control.datascene")
TEST_DEPENDS("engine.scripthost.datasceneservice")

using engine::control::Surface;
using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::AttributeValue;
using engine::ecs::Entity;
using engine::ecs::InstanceName;
using engine::ecs::PropertyType;
using engine::scene::ActiveCamera;
using engine::scene::Camera;
using engine::scene::Transform;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;
using nlohmann::json;

namespace {
	json Call(Surface &surface, std::string_view name, const json &arguments, bool &failed) {
		const json request{
			{"jsonrpc", "2.0"},
			{"id", 1},
			{"method", "tools/call"},
			{"params", {{"name", name}, {"arguments", arguments}}}
		};
		const json reply = json::parse(surface.Answer(request.dump()));
		const json &result = reply.at("result");
		failed = result.value("isError", false);
		return json::parse(result.at("content").at(0).at("text").get<std::string>());
	}

	WorldId World(Universe &universe, std::string_view name) {
		WorldSettings settings;
		settings.Name = Name(name);
		return universe.Create(settings);
	}

	void Identify(engine::ecs::Store &store, Entity entity, std::string_view id) {
		AttributeValue value;
		value.Type = PropertyType::String;
		value.String = id;
		REQUIRE(engine::ecs::SetAttribute(store, entity, Name("DataFactoryId"), value));
	}
}

TEST_CASE("data-scene MCP tools use stable scene and camera identifiers", "[control][datascene]") {
	Universe universe;
	const WorldId world = World(universe, "scene");
	Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});

	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneComponents();
		const Entity camera = store.Create();
		store.Set<InstanceName>(camera, InstanceName{Name("Camera")});
		Camera calibration;
		calibration.FieldOfViewRadians = 1.0f;
		calibration.NearPlane = 0.25f;
		calibration.FarPlane = 400.0f;
		calibration.ImageWidth = 640;
		calibration.ImageHeight = 360;
		store.Set<Camera>(camera, calibration);
		CFrame cameraFrame{Vector3{1.0f, 2.0f, 3.0f}};
		cameraFrame.QuaternionW = 1.0005f;
		store.Set<Transform>(camera, Transform{cameraFrame});
		Identify(store, camera, "fixture/camera");
		store.SetResource(ActiveCamera{camera});
	});

	bool failed = false;
	const json camera = Call(
		surface,
		"get_camera_rendering_data",
		{{"instance_id", "scene"}, {"options", {{"camera_id", "fixture/camera"}}}},
		failed
	);
	INFO(camera.dump());
	CHECK_FALSE(failed);
	CHECK(camera.at("id") == "fixture/camera");
	CHECK(camera.at("world_from_camera").at("position") == json::array({1.0, 2.0, 3.0}));
	CHECK(camera.at("camera_from_world").at("position") == json::array({-1.0, -2.0, -3.0}));
	CHECK(camera.at("world_from_camera").at("rotation") == json::array({0.0, 0.0, 0.0, 1.0}));
	CHECK(camera.at("vertical_fov_radians") == 1.0);
	CHECK(camera.at("near_metres") == 0.25);
	CHECK(camera.at("far_metres") == 400.0);
	CHECK(camera.at("requested_width") == 640);
	CHECK(camera.at("requested_height") == 360);
	CHECK(camera.at("requested_resolution_available") == true);
	CHECK(camera.at("projection_available") == false);
	CHECK(camera.at("crop").at("convention") == "normalized_full_view_left_top_width_height");
	CHECK(camera.at("lens_distortion").at("available") == false);
	CHECK(camera.at("temporal_jitter").at("available") == false);
	CHECK(camera.at("units").at("world") == "metres");
	CHECK(camera.at("camera_axes") == "x_right_y_up_negative_z_forward");
	CHECK(camera.at("clip_depth_range") == "zero_to_one");

	universe.Enter(world, [](engine::ecs::Store &store) {
		const auto *active = store.Resource<ActiveCamera>();
		REQUIRE(active != nullptr);
		Camera inherited = *store.Get<Camera>(active->Entity);
		inherited.ImageWidth = 0;
		inherited.ImageHeight = 0;
		store.Set<Camera>(active->Entity, inherited);
	});
	const json inherited = Call(
		surface, "get_camera_rendering_data", {{"instance_id", "scene"}, {"options", json::object()}}, failed
	);
	CHECK_FALSE(failed);
	CHECK(inherited.at("requested_resolution_available") == false);
	CHECK(inherited.at("requested_resolution_reason") == "host_viewport_resolves_at_capture");

	universe.Enter(world, [](engine::ecs::Store &store) {
		const auto *active = store.Resource<ActiveCamera>();
		REQUIRE(active != nullptr);
		Camera invalid = *store.Get<Camera>(active->Entity);
		invalid.ImageWidth = 640;
		store.Set<Camera>(active->Entity, invalid);
	});
	const json invalid = Call(
		surface, "get_camera_rendering_data", {{"instance_id", "scene"}, {"options", json::object()}}, failed
	);
	CHECK(failed);
	CHECK(invalid.at("status") == "invalid_camera_calibration");

	const json unknown = Call(
		surface, "get_scene_snapshot", {{"instance_id", "missing"}, {"options", json::object()}}, failed
	);
	CHECK(failed);
	CHECK(unknown.contains("error"));

	const json malformed = Call(surface, "get_scene_snapshot", {{"instance_id", "scene"}}, failed);
	CHECK(failed);
	CHECK(malformed.contains("error"));
}

TEST_CASE("data-scene MCP snapshot refuses duplicate stable identifiers", "[control][datascene]") {
	Universe universe;
	const WorldId world = World(universe, "duplicates");
	Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});

	universe.Enter(world, [](engine::ecs::Store &store) {
		for (int index = 0; index < 2; index++) {
			const Entity entity = store.Create();
			store.Set<InstanceName>(entity, InstanceName{Name("Part")});
			Identify(store, entity, "fixture/duplicate");
		}
	});

	bool failed = false;
	const json reply = Call(
		surface, "get_scene_snapshot", {{"instance_id", "duplicates"}, {"options", json::object()}}, failed
	);
	INFO(reply.dump());
	CHECK(failed);
	CHECK(reply.at("status") == "identity_conflict");
}

TEST_CASE("data-scene MCP queries prepared collider geometry", "[control][datascene]") {
	Universe universe;
	const WorldId world = World(universe, "queries");
	Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});

	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		engine::physics::PreparePhysicsWorld(store);
		engine::scene::PartDesc partDesc;
		partDesc.Frame = CFrame{Vector3::Zero};
		partDesc.Size = Vector3{2.0f, 2.0f, 2.0f};
		const Entity part = engine::scene::MakePart(store, partDesc);
		Identify(store, part, "query/box");
		engine::ecs::Scheduler scheduler;
		engine::physics::RegisterPhysicsSystems(scheduler);
		scheduler.Tick(store, 1.0 / 60.0);
	});

	bool failed = false;
	const json ray = Call(
		surface,
		"raycast",
		{{"instance_id", "queries"},
		 {"options", {{"origin", {-3, 0, 0}}, {"direction", {2, 0, 0}}, {"max_distance_metres", 10}}}},
		failed
	);
	INFO(ray.dump());
	CHECK_FALSE(failed);
	CHECK(ray.at("id") == "query/box");
	CHECK(ray.at("provenance") == "physics_exact_collider_raycast");
	const float large = std::numeric_limits<float>::max();
	const json largeDirection = Call(
		surface,
		"raycast",
		{{"instance_id", "queries"},
		 {"options",
		  {{"origin", {-3, -3, 0}}, {"direction", {large, large, 0}}, {"max_distance_metres", 10}}}},
		failed
	);
	INFO(largeDirection.dump());
	CHECK_FALSE(failed);
	CHECK(largeDirection.at("id") == "query/box");

	const json aabb = Call(
		surface,
		"overlap_aabb",
		{{"instance_id", "queries"}, {"options", {{"minimum", {-1, -1, -1}}, {"maximum", {1, 1, 1}}}}},
		failed
	);
	INFO(aabb.dump());
	CHECK_FALSE(failed);
	CHECK(aabb.at("ids") == json::array({"query/box"}));

	const json obb = Call(
		surface,
		"overlap_obb",
		{{"instance_id", "queries"},
		 {"options",
		  {{"center", {0, 0, 0}}, {"orientation_xyzw", {0, 0, 0, 1}}, {"half_extent", {1, 1, 1}}}}},
		failed
	);
	INFO(obb.dump());
	CHECK_FALSE(failed);
	CHECK(obb.at("ids") == json::array({"query/box"}));
	const json tinyQuaternion = Call(
		surface,
		"overlap_obb",
		{{"instance_id", "queries"},
		 {"options",
		  {{"center", {0, 0, 0}}, {"orientation_xyzw", {0, 0, 0, 1e-40}}, {"half_extent", {1, 1, 1}}}}},
		failed
	);
	INFO(tinyQuaternion.dump());
	CHECK_FALSE(failed);
	CHECK(tinyQuaternion.at("ids") == json::array({"query/box"}));

	const json zeroDirection = Call(
		surface,
		"raycast",
		{{"instance_id", "queries"},
		 {"options", {{"origin", {0, 0, 0}}, {"direction", {0, 0, 0}}, {"max_distance_metres", 10}}}},
		failed
	);
	CHECK(failed);
	CHECK(zeroDirection.at("status") == "invalid_raycast_query");

	const json unknown = Call(
		surface,
		"overlap_aabb",
		{{"instance_id", "queries"},
		 {"options", {{"minimum", {-1, -1, -1}}, {"maximum", {1, 1, 1}}, {"extra", true}}}},
		failed
	);
	CHECK(failed);
	CHECK(unknown.contains("error"));
}
