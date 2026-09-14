#include <engine/control/Surface.hpp>
#include <engine/control/features/DataFactory.hpp>
#include <engine/control/features/DataScene.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/physics/BodyMotion.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Welds.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Constraints.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/script/EventNarratives.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <numbers>

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
using engine::world::DataFactoryPauseScope;
using engine::world::DataFactorySession;
using engine::world::DataFactoryStatus;
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

TEST_CASE(
	"data-scene MCP exposes source lighting metadata and unavailable contribution evidence",
	"[control][datascene]"
) {
	Universe universe;
	const WorldId world = World(universe, "lighting");
	Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});

	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneComponents();
		engine::scene::RegisterSceneClasses();
		const Entity parent = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Lamp");
		store.Set<Transform>(parent, Transform{CFrame(Vector3{2.0f, 3.0f, 4.0f})});
		const Entity bulb = store.CreateInstance(engine::ecs::Classes::Find(Name("PointLight")), "Bulb");
		REQUIRE(store.SetParent(bulb, parent));
		engine::scene::Light light;
		light.Colour = engine::core::Color3{0.5f, 0.25f, 0.125f};
		light.Brightness = 4.0f;
		store.Set(bulb, light);
		Identify(store, bulb, "lighting/bulb");
	});

	bool failed = false;
	const json snapshot = Call(
		surface, "get_scene_snapshot", {{"instance_id", "lighting"}, {"options", json::object()}}, failed
	);
	INFO(snapshot.dump());
	CHECK_FALSE(failed);
	const json &lighting = snapshot.at("lighting_observation");
	CHECK(lighting.at("schema_version") == "lighting-observation/v1");
	CHECK(lighting.at("resolved_global").at("provenance").at("lighting_service") == "engine_defaults");
	REQUIRE(lighting.at("local_lights").size() == 1);
	CHECK(lighting.at("local_lights").at(0).at("id") == "lighting/bulb");
	CHECK(lighting.at("local_lights").at(0).at("source_stage_eligible") == true);
	CHECK(lighting.at("local_lights").at(0).at("renderer_rgb").at("r") == 2.0);
	CHECK(
		lighting.at("local_light_coverage") == "identified_source_rows_before_portal_copies_and_camera_cap"
	);
	CHECK(lighting.at("identified_local_light_count") == 1);
	CHECK(lighting.at("omitted_unidentified_local_light_count") == 0);
	CHECK(lighting.at("view_selection").at("available") == false);
	CHECK(lighting.at("portal_copies").at("available") == false);
	CHECK(lighting.at("per_pixel_contribution").at("available") == false);
	CHECK(lighting.at("shadow_factor").at("available") == false);
	CHECK(lighting.at("shadow_caster").at("available") == false);
	CHECK(lighting.at("shadow_receiver").at("available") == false);
	CHECK(lighting.at("photometric_units").at("available") == false);
}

TEST_CASE("data-scene MCP reports the Sun resource override provenance", "[control][datascene]") {
	Universe universe;
	const WorldId world = World(universe, "sun_override");
	Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});

	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::Sun sun;
		sun.Direction = Vector3{0.0f, 3.0f, 4.0f};
		sun.Ambient = engine::core::Color3{0.2f, 0.3f, 0.4f};
		store.SetResource(sun);
	});

	bool failed = false;
	const json snapshot = Call(
		surface, "get_scene_snapshot", {{"instance_id", "sun_override"}, {"options", json::object()}}, failed
	);
	INFO(snapshot.dump());
	CHECK_FALSE(failed);
	const json &global = snapshot.at("lighting_observation").at("resolved_global");
	CHECK(global.at("direction_world_towards").at("x") == Catch::Approx(0.0));
	CHECK(global.at("direction_world_towards").at("y") == Catch::Approx(0.6));
	CHECK(global.at("direction_world_towards").at("z") == Catch::Approx(0.8));
	CHECK(global.at("ambient_rgb").at("r") == Catch::Approx(0.2));
	CHECK(global.at("ambient_rgb").at("g") == Catch::Approx(0.3));
	CHECK(global.at("ambient_rgb").at("b") == Catch::Approx(0.4));
	CHECK(global.at("provenance").at("lighting_service") == "engine_defaults");
	CHECK(global.at("provenance").at("sun_override") == "sun_resource_override");
	CHECK(global.at("provenance").at("direction") == "sun_resource_override");
	CHECK(global.at("provenance").at("ambient") == "sun_resource_override");
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
		const Entity visible = store.Create();
		store.Set<InstanceName>(visible, InstanceName{Name("Visible")});
		store.Set<Transform>(visible, Transform{CFrame{Vector3{0.0f, 0.0f, -4.0f}}});
		store.Set<engine::scene::Bounds>(visible, {{0.5f, 0.5f, 0.5f}});
		Identify(store, visible, "fixture/object-visible");
		const Entity behind = store.Create();
		store.Set<InstanceName>(behind, InstanceName{Name("Behind")});
		store.Set<Transform>(behind, Transform{CFrame{Vector3{0.0f, 0.0f, 4.0f}}});
		store.Set<engine::scene::Bounds>(behind, {{0.5f, 0.5f, 0.5f}});
		Identify(store, behind, "fixture/object-behind");
		const Entity inside = store.Create();
		store.Set<InstanceName>(inside, InstanceName{Name("Inside")});
		store.Set<Transform>(inside, Transform{CFrame{Vector3{1.0f, 2.0f, 3.0f}}});
		store.Set<engine::scene::Bounds>(inside, {{1.0f, 1.0f, 1.0f}});
		Identify(store, inside, "fixture/object-inside");
	});

	bool failed = false;
	const json camera = Call(
		surface,
		"get_camera_rendering_data",
		{{"instance_id", "scene"}, {"options", {{"camera_id", "fixture/camera"}, {"object_limit", 3}}}},
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
	CHECK(camera.at("requested_width").is_number_integer());
	CHECK(camera.at("requested_height").is_number_integer());
	CHECK(camera.at("near_metres").is_number_float());
	CHECK(camera.at("requested_resolution_available") == true);
	CHECK(camera.at("projection_available") == false);
	const json &observations = camera.at("object_observations");
	REQUIRE(observations.size() == 3);
	CHECK(observations[0].at("id") == "fixture/object-behind");
	CHECK(observations[1].at("id") == "fixture/object-inside");
	CHECK(observations[2].at("id") == "fixture/object-visible");
	CHECK(observations[0].at("projected_bounds").at("available") == false);
	CHECK(observations[0].at("projected_bounds").at("xyxy_pixels").is_null());
	CHECK(observations[0].at("projected_bounds").at("clipped_to_image").is_null());
	CHECK(observations[0].at("authored_visible").at("value").is_null());
	CHECK(observations[1].at("projected_bounds").at("available") == true);
	CHECK(observations[1].at("projected_bounds").at("clipped_to_image") == true);
	CHECK(observations[2].at("projected_bounds").at("available") == true);
	CHECK(observations[2].at("projected_bounds").at("xyxy_pixels").size() == 4);
	CHECK(observations[2].at("frustum_intersection").at("value") == true);
	CHECK(observations[2].at("occlusion").at("available") == false);
	CHECK(observations[2].at("occlusion").at("reason") == "requires_capture_visibility_evidence");
	const json calibrationOnly = Call(
		surface,
		"get_camera_rendering_data",
		{{"instance_id", "scene"}, {"options", {{"camera_id", "fixture/camera"}}}},
		failed
	);
	CHECK_FALSE(failed);
	CHECK(calibrationOnly.at("object_observations").empty());
	CHECK(camera.at("crop").at("convention") == "normalized_full_view_left_top_width_height");
	CHECK(camera.at("lens_distortion").at("available") == false);
	CHECK(camera.at("temporal_jitter").at("available") == false);
	CHECK(camera.at("units").at("world") == "metres");
	CHECK(camera.at("camera_axes") == "x_right_y_up_negative_z_forward");
	CHECK(camera.at("clip_depth_range") == "zero_to_one");

	const json snapshot =
		Call(surface, "get_scene_snapshot", {{"instance_id", "scene"}, {"options", json::object()}}, failed);
	INFO(snapshot.dump());
	CHECK_FALSE(failed);
	CHECK(snapshot.at("unlabelled_instances").is_number_integer());
	CHECK(snapshot.at("physics_observations").at("schema_version") == "physics-observation/v1");
	CHECK(snapshot.at("physics_observations").at("world_prepared") == false);
	CHECK(snapshot.at("physics_observations").at("contacts").at("available") == false);
	CHECK(snapshot.at("physics_observations").at("impulses").at("available") == false);
	CHECK(snapshot.at("physics_observations").at("forces").at("available") == true);
	CHECK(snapshot.at("physics_observations").at("torques").at("available") == true);
	CHECK(snapshot.at("contacts").empty());
	CHECK(snapshot.at("contact_events").empty());
	const json &snapshotEntities = snapshot.at("entities");
	const auto visibleRecord =
		std::find_if(snapshotEntities.begin(), snapshotEntities.end(), [](const json &record) {
			return record.at("id") == "fixture/object-visible";
		});
	REQUIRE(visibleRecord != snapshotEntities.end());
	CHECK(visibleRecord->at("size_metres") == json{{"x", 1.0}, {"y", 1.0}, {"z", 1.0}});
	CHECK(visibleRecord->at("world_from_object").at("position") == json::array({0.0, 0.0, -4.0}));
	CHECK(visibleRecord->at("parent_from_object").at("available") == false);

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

TEST_CASE("camera object observations reject diagonal screen-bound false positives", "[control][datascene]") {
	Universe universe;
	const WorldId world = World(universe, "diagonal");
	Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});
	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneComponents();
		const Entity camera = store.Create();
		store.Set<InstanceName>(camera, InstanceName{Name("Camera")});
		Camera lens;
		lens.FieldOfViewRadians = std::numbers::pi_v<float> / 2.0f;
		lens.NearPlane = 0.1f;
		lens.FarPlane = 10.0f;
		lens.ImageWidth = 100;
		lens.ImageHeight = 100;
		store.Set<Camera>(camera, lens);
		store.Set<Transform>(camera, Transform{});
		Identify(store, camera, "fixture/diagonal-camera");
		store.SetResource(ActiveCamera{camera});
		const Entity thin = store.Create();
		store.Set<InstanceName>(thin, InstanceName{Name("Thin")});
		CFrame frame = CFrame::Angles(0.0f, 0.0f, -std::numbers::pi_v<float> / 4.0f);
		frame.Position = {1.5f, 1.5f, -1.0f};
		store.Set<Transform>(thin, Transform{frame});
		store.Set<engine::scene::Bounds>(thin, {{1.4f, 0.01f, 0.001f}});
		Identify(store, thin, "fixture/diagonal-thin");
	});
	bool failed = false;
	const json result = Call(
		surface,
		"get_camera_rendering_data",
		{{"instance_id", "diagonal"}, {"options", {{"object_limit", 1}}}},
		failed
	);
	CHECK_FALSE(failed);
	const json &observation = result.at("object_observations").at(0);
	CHECK(observation.at("frustum_intersection").at("value") == false);
	CHECK(observation.at("projected_bounds").at("available") == false);
}

TEST_CASE("data-scene refuses finite geometry whose derived corners overflow", "[control][datascene]") {
	Universe universe;
	const WorldId world = World(universe, "overflow");
	Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});
	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneComponents();
		const Entity camera = store.Create();
		store.Set<Camera>(camera, Camera{});
		store.Set<Transform>(camera, Transform{});
		Identify(store, camera, "fixture/overflow-camera");
		store.SetResource(ActiveCamera{camera});
		const Entity object = store.CreateInstance(engine::scene::PartClass(), "Overflow");
		Identify(store, object, "fixture/overflow");
		store.Set<Transform>(
			object, Transform{CFrame{Vector3{std::numeric_limits<float>::max(), 0.0f, 0.0f}}}
		);
		store.Set<engine::scene::Bounds>(object, {{std::numeric_limits<float>::max(), 1.0f, 1.0f}});
	});
	bool failed = false;
	const json result = Call(
		surface,
		"get_camera_rendering_data",
		{{"instance_id", "overflow"}, {"options", {{"object_limit", 1}}}},
		failed
	);
	CHECK(failed);
	CHECK(result.at("status") == "invalid_spatial_data");
}

TEST_CASE(
	"data-scene derives parent-from-object from translated rotated world frames", "[control][datascene]"
) {
	Universe universe;
	const WorldId world = World(universe, "parent-frame");
	Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});
	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneComponents();
		const Entity parent = store.CreateInstance(engine::scene::PartClass(), "Parent");
		const Entity child = store.CreateInstance(engine::scene::PartClass(), "Child");
		Identify(store, parent, "fixture/parent");
		Identify(store, child, "fixture/child");
		REQUIRE(store.SetParent(child, parent));
		CFrame parentFrame = CFrame::Angles(0.0f, 0.0f, std::numbers::pi_v<float> / 2.0f);
		parentFrame.Position = {3.0f, 4.0f, 0.0f};
		const CFrame local{Vector3{2.0f, 0.0f, 0.0f}};
		store.Set<Transform>(parent, Transform{parentFrame});
		store.Set<Transform>(child, Transform{parentFrame * local});
		store.Set<engine::scene::Bounds>(child, {{1.0f, 1.0f, 1.0f}});
	});
	bool failed = false;
	const json snapshot = Call(
		surface, "get_scene_snapshot", {{"instance_id", "parent-frame"}, {"options", json::object()}}, failed
	);
	CHECK_FALSE(failed);
	const auto child =
		std::find_if(snapshot.at("entities").begin(), snapshot.at("entities").end(), [](const json &row) {
			return row.at("id") == "fixture/child";
		});
	REQUIRE(child != snapshot.at("entities").end());
	CHECK(child->at("parent_from_object").at("available") == true);
	CHECK(
		std::abs(child->at("parent_from_object").at("frame").at("position").at(0).get<float>() - 2.0f) <
		0.001f
	);
}

TEST_CASE("rolled camera projects an object along its look vector", "[control][datascene]") {
	Universe universe;
	const WorldId world = World(universe, "rolled-camera");
	Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});
	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneComponents();
		const Entity camera = store.Create();
		Camera lens;
		lens.ImageWidth = 640;
		lens.ImageHeight = 360;
		store.Set<Camera>(camera, lens);
		const CFrame cameraFrame = CFrame::Angles(0.0f, 0.0f, std::numbers::pi_v<float> / 4.0f);
		store.Set<Transform>(camera, Transform{cameraFrame});
		Identify(store, camera, "fixture/rolled-camera");
		store.SetResource(ActiveCamera{camera});
		const Entity object = store.CreateInstance(engine::scene::PartClass(), "Object");
		Identify(store, object, "fixture/rolled-object");
		CFrame objectFrame;
		objectFrame.Position = cameraFrame.LookVector() * 4.0f + cameraFrame.RightVector() * 0.25f;
		store.Set<Transform>(object, Transform{objectFrame});
		store.Set<engine::scene::Bounds>(object, {{0.25f, 0.25f, 0.25f}});
	});
	bool failed = false;
	const json result = Call(
		surface,
		"get_camera_rendering_data",
		{{"instance_id", "rolled-camera"}, {"options", {{"object_limit", 1}}}},
		failed
	);
	CHECK_FALSE(failed);
	const json &observation = result.at("object_observations").at(0);
	CHECK(observation.at("frustum_intersection").at("value") == true);
	CHECK(observation.at("projected_bounds").at("available") == true);
}

TEST_CASE("camera projection keeps extreme finite depth denominators truthful", "[control][datascene]") {
	Universe universe;
	const WorldId world = World(universe, "extreme-projection");
	Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});
	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneComponents();
		const Entity camera = store.Create();
		Camera lens;
		lens.FieldOfViewRadians = 1.0f;
		lens.NearPlane = 1.0f;
		lens.FarPlane = std::numeric_limits<float>::max();
		lens.ImageWidth = std::numeric_limits<uint32_t>::max();
		lens.ImageHeight = 1;
		store.Set<Camera>(camera, lens);
		store.Set<Transform>(camera, Transform{});
		Identify(store, camera, "fixture/extreme-camera");
		store.SetResource(ActiveCamera{camera});
		const Entity object = store.CreateInstance(engine::scene::PartClass(), "Object");
		Identify(store, object, "fixture/extreme-object");
		store.Set<Transform>(
			object,
			Transform{CFrame{Vector3{
				std::numeric_limits<float>::max() / 4.0f, 0.0f, -std::numeric_limits<float>::max() / 2.0f
			}}}
		);
		store.Set<engine::scene::Bounds>(object, {{1.0f, 1.0f, 1.0f}});
	});
	bool failed = false;
	const json result = Call(
		surface,
		"get_camera_rendering_data",
		{{"instance_id", "extreme-projection"}, {"options", {{"object_limit", 1}}}},
		failed
	);
	CHECK_FALSE(failed);
	const json &projected = result.at("object_observations").at(0).at("projected_bounds");
	CHECK(projected.at("available") == false);
	CHECK(projected.at("xyxy_pixels").is_null());
}

TEST_CASE("camera projection preserves a clipped UINT32_MAX right edge", "[control][datascene]") {
	Universe universe;
	const WorldId world = World(universe, "wide-edge");
	Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});
	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneComponents();
		const Entity camera = store.Create();
		Camera lens;
		lens.ImageWidth = std::numeric_limits<uint32_t>::max();
		lens.ImageHeight = std::numeric_limits<uint32_t>::max();
		store.Set<Camera>(camera, lens);
		store.Set<Transform>(camera, Transform{});
		Identify(store, camera, "fixture/wide-camera");
		store.SetResource(ActiveCamera{camera});
		const Entity object = store.CreateInstance(engine::scene::PartClass(), "Edge");
		Identify(store, object, "fixture/wide-edge");
		store.Set<Transform>(object, Transform{CFrame{Vector3{2.0f, 0.0f, -4.0f}}});
		store.Set<engine::scene::Bounds>(object, {{2.0f, 1.0f, 1.0f}});
	});
	bool failed = false;
	const json result = Call(
		surface,
		"get_camera_rendering_data",
		{{"instance_id", "wide-edge"}, {"options", {{"object_limit", 1}}}},
		failed
	);
	CHECK_FALSE(failed);
	const json &bounds = result.at("object_observations").at(0).at("projected_bounds");
	REQUIRE(bounds.at("available") == true);
	CHECK(bounds.at("clipped_to_image") == true);
	CHECK(bounds.at("xyxy_pixels").at(2).get<double>() <= std::numeric_limits<uint32_t>::max());
}

TEST_CASE("data-scene MCP exposes read-only script-declared event narratives", "[control][datascene]") {
	Universe universe;
	const WorldId world = World(universe, "narratives");
	Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});
	const json unavailableReply = json::parse(surface.Answer(
		json{
			{"jsonrpc", "2.0"},
			{"id", 1},
			{"method", "tools/call"},
			{"params",
			 {{"name", "get_event_narratives"},
			  {"arguments", {{"instance_id", "narratives"}, {"options", json::object()}}}}}
		}.dump()
	));
	const json &unavailableResult = unavailableReply.at("result");
	CHECK_FALSE(unavailableResult.value("isError", true));
	const json unavailable = json::parse(unavailableResult.at("content").at(0).at("text").get<std::string>());
	CHECK(unavailable.at("status") == "unavailable");
	CHECK(unavailable.at("version").is_number());
	CHECK(unavailable.at("version").get<double>() == 1.0);
	bool failed = false;
	universe.Enter(world, [](engine::ecs::Store &store) {
		store.SetResource(engine::script::EventNarratives{engine::script::ScriptValue{}});
	});
	const json corrupt = Call(
		surface, "get_event_narratives", {{"instance_id", "narratives"}, {"options", json::object()}}, failed
	);
	CHECK(failed);
	CHECK(corrupt.at("status") == "invalid_event_narratives");

	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::script::ScriptValue version(engine::script::ValueTag::Number);
		version.Number = engine::script::EVENT_NARRATIVE_SCHEMA_VERSION;
		engine::script::ScriptValue records(engine::script::ValueTag::Array);
		engine::script::ScriptValue bundle(engine::script::ValueTag::Map);
		bundle.Entries = {{"version", std::move(version)}, {"records", std::move(records)}};
		REQUIRE(std::string_view(engine::script::SetEventNarratives(store, bundle).Status) == "ok");
	});
	const json narratives = Call(
		surface, "get_event_narratives", {{"instance_id", "narratives"}, {"options", json::object()}}, failed
	);
	CHECK_FALSE(failed);
	CHECK(narratives.at("status") == "ok");
	CHECK(narratives.at("schema_version") == "event-narrative/v1");
	CHECK(narratives.at("records") == json::array());

	const json invalid = Call(
		surface,
		"get_event_narratives",
		{{"instance_id", "narratives"}, {"options", {{"extra", true}}}},
		failed
	);
	CHECK(failed);
	CHECK(invalid.contains("error"));
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
		engine::scene::InstallServices(store);
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

TEST_CASE(
	"data-scene snapshot reports solver observations and persistent applied loads", "[control][datascene]"
) {
	Universe universe;
	const WorldId world = World(universe, "physics_observation");
	Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});
	Entity body;

	universe.Enter(world, [&](engine::ecs::Store &store) {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
		const Entity workspace = engine::scene::InstallServices(store);
		engine::physics::PreparePhysicsWorld(store);
		engine::scene::PartDesc staticDescription;
		staticDescription.Frame = CFrame{Vector3::Zero};
		staticDescription.Size = Vector3{2.0f, 2.0f, 2.0f};
		const Entity floor = engine::scene::MakePart(store, staticDescription);
		Identify(store, floor, "physics/floor");

		engine::scene::PartDesc dynamicDescription = staticDescription;
		dynamicDescription.Frame = CFrame{Vector3{0.0f, 0.5f, 0.0f}};
		body = engine::scene::MakePart(store, dynamicDescription);
		Identify(store, body, "physics/body");
		store.Set<engine::scene::Simulated>(body, {});
		engine::scene::Motion motion;
		motion.Linear = Vector3{0.0f, -2.0f, 0.0f};
		store.Set<engine::scene::Motion>(body, motion);
		REQUIRE(engine::physics::SetAppliedForce(store, body, Vector3{6.0f, 0.0f, 0.0f}));
		REQUIRE(engine::physics::SetAppliedTorque(store, body, Vector3{0.0f, 3.0f, 0.0f}));
		engine::scene::Humanoid humanoid;
		humanoid.RootPart = body;
		humanoid.MoveDirection = Vector3{1.0f, 0.0f, 0.0f};
		humanoid.WalkSpeed = 4.5f;
		store.Set<engine::scene::Humanoid>(body, humanoid);
		// This collider shares the observed body's contact, but lacks a factory
		// id. The snapshot must report only contact pairs with both endpoints in
		// its explicit stable-id subset.
		engine::scene::MakePart(store, dynamicDescription);

		engine::scene::PartDesc assemblyDescription = staticDescription;
		assemblyDescription.Frame = CFrame{Vector3{20.0f, 0.0f, 0.0f}};
		const Entity unlabelledAssemblyRoot = engine::scene::MakePart(store, assemblyDescription);
		assemblyDescription.Frame = CFrame{Vector3{22.0f, 0.0f, 0.0f}};
		const Entity assemblyLeaf = engine::scene::MakePart(store, assemblyDescription);
		Identify(store, assemblyLeaf, "physics/assembly-leaf");
		REQUIRE(workspace != engine::ecs::NULL_ENTITY);
		store.SetParent(unlabelledAssemblyRoot, workspace);
		store.SetParent(assemblyLeaf, workspace);
		REQUIRE(store.IsDescendantOf(unlabelledAssemblyRoot, workspace));
		REQUIRE(store.IsDescendantOf(assemblyLeaf, workspace));
		engine::scene::JointInstance joint;
		joint.Part0 = unlabelledAssemblyRoot;
		joint.Part1 = assemblyLeaf;
		store.Set<engine::scene::JointInstance>(assemblyLeaf, joint);

		engine::scene::CameraController cameraController;
		cameraController.Distance = 7.0f;
		cameraController.MinimumDistance = 1.0f;
		cameraController.MaximumDistance = 30.0f;
		store.SetResource(cameraController);
		engine::scene::ControllerState controllers;
		controllers.Slots[0].Connected = true;
		controllers.Slots[0].Mapped = true;
		controllers.Slots[0].Buttons = 3;
		controllers.Slots[0].PressedButtons = 1;
		controllers.Slots[0].Axes[0] = 0.25f;
		store.SetResource(controllers);
		engine::ecs::Scheduler scheduler;
		engine::physics::RegisterPhysicsSystems(scheduler);
		scheduler.Tick(store, 1.0 / 60.0);
		engine::physics::SolveRigidJoints(store);
	});

	bool failed = false;
	const json snapshot = Call(
		surface,
		"get_scene_snapshot",
		{{"instance_id", "physics_observation"}, {"options", json::object()}},
		failed
	);
	INFO(snapshot.dump());
	CHECK_FALSE(failed);
	CHECK(snapshot.at("physics_observations").at("schema_version") == "physics-observation/v1");
	CHECK(snapshot.at("physics_observations").at("world_prepared") == true);
	CHECK(snapshot.at("physics_observations").at("contacts").at("available") == true);
	CHECK(snapshot.at("physics_observations").at("impulses").at("available") == true);
	CHECK(
		snapshot.at("physics_observations").at("contacts").at("coverage") ==
		"identified_endpoints_in_explicit_subset"
	);
	CHECK(snapshot.at("physics_observations").at("impulses").at("skipped_manifolds") == "excluded");
	CHECK(snapshot.at("physics_observations").at("forces").at("available") == true);
	CHECK(snapshot.at("physics_observations").at("torques").at("available") == true);
	CHECK(snapshot.at("physics_observations").at("forces").at("source") == "persistent_applied_load");
	CHECK(snapshot.at("physics_observations").at("forces").at("units") == "N");
	CHECK(snapshot.at("physics_observations").at("torques").at("units") == "N*m");
	CHECK(snapshot.at("physics_observations").at("forces").at("coordinate_space") == "world");
	CHECK(
		snapshot.at("physics_observations").at("forces").at("timing") ==
		"applied_once_per_completed_physics_step"
	);
	REQUIRE(snapshot.at("forces").size() == 1);
	CHECK(snapshot.at("forces").at(0).at("id") == "physics/body");
	CHECK(snapshot.at("forces").at(0).at("force_world_newtons").at("x") == 6.0);
	REQUIRE(snapshot.at("torques").size() == 1);
	CHECK(snapshot.at("torques").at(0).at("id") == "physics/body");
	CHECK(snapshot.at("torques").at(0).at("torque_world_newton_metres").at("y") == 3.0);
	REQUIRE(snapshot.at("contacts").is_array());
	REQUIRE(snapshot.at("contact_events").is_array());
	bool contactObserved = false;
	for (const auto &contact : snapshot.at("contacts")) {
		if (contact.at("a_id") != "physics/floor" || contact.at("b_id") != "physics/body") {
			continue;
		}
		REQUIRE_FALSE(contact.at("points").empty());
		const auto &point = contact.at("points").at(0);
		CHECK(point.at("normal_impulse_newton_seconds").get<float>() > 0.0f);
		CHECK(point.at("friction_impulse_newton_seconds").at("tangent0_world_direction").is_object());
		CHECK(point.at("world_impulse_newton_seconds").is_object());
		CHECK(point.at("world_impulse_applied_to") == "b_from_a");
		contactObserved = true;
	}
	CHECK(contactObserved);
	bool beganObserved = false;
	for (const auto &event : snapshot.at("contact_events")) {
		beganObserved |= event.at("a_id") == "physics/floor" && event.at("b_id") == "physics/body" &&
						 event.at("phase") == "began";
	}
	CHECK(beganObserved);
	CHECK(
		snapshot.at("physics_observations")
			.at("contacts")
			.at("omitted_unidentified_endpoints")
			.get<size_t>() >= 1
	);
	for (const auto &entity : snapshot.at("entities")) {
		if (entity.at("id") == "physics/body") {
			CHECK(entity.at("physics").at("sleeping").is_boolean());
			CHECK(entity.at("physics").at("sleep_state") == "awake");
			CHECK(entity.at("physics").at("awake") == true);
			CHECK(entity.at("physics").at("assembly_available") == true);
			CHECK(entity.at("physics").at("has_rigid_assembly") == false);
			CHECK(entity.at("physics").at("assembly_root_has_stable_id") == false);
			CHECK(entity.at("physics").at("units").at("impulse") == "N*s");
			CHECK(entity.at("humanoid_controller").at("root_part_id") == "physics/body");
			CHECK(entity.at("humanoid_controller").at("walk_speed_mps") == 4.5);
		}
		if (entity.at("id") == "physics/assembly-leaf") {
			CHECK(entity.at("physics").at("has_rigid_assembly") == true);
			CHECK(entity.at("physics").at("assembly_root_has_stable_id") == false);
			REQUIRE(entity.at("joints").size() == 1);
			CHECK(entity.at("joints").at(0).at("kind") == "joint_instance");
			CHECK(entity.at("joints").at(0).at("part1_id") == "physics/assembly-leaf");
		}
	}
	CHECK(snapshot.at("camera_controller").at("distance_metres") == 7.0);
	CHECK(snapshot.at("controllers").at(0).at("connected") == true);
	CHECK(snapshot.at("controllers").at(0).at("axes").at(0) == 0.25);

	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::ecs::Scheduler scheduler;
		engine::physics::RegisterPhysicsSystems(scheduler);
		scheduler.Tick(store, 1.0 / 60.0);
	});
	const json persisted = Call(
		surface,
		"get_scene_snapshot",
		{{"instance_id", "physics_observation"}, {"options", json::object()}},
		failed
	);
	INFO(persisted.dump());
	CHECK_FALSE(failed);
	bool persistedObserved = false;
	for (const auto &event : persisted.at("contact_events")) {
		persistedObserved |= event.at("a_id") == "physics/floor" && event.at("b_id") == "physics/body" &&
							 event.at("phase") == "persisted";
	}
	CHECK(persistedObserved);

	universe.Enter(world, [&](engine::ecs::Store &store) {
		store.GetMutable<Transform>(body)->Frame.Position = Vector3{0.0f, 10.0f, 0.0f};
		engine::ecs::Scheduler scheduler;
		engine::physics::RegisterPhysicsSystems(scheduler);
		scheduler.Tick(store, 1.0 / 60.0);
	});
	const json ended = Call(
		surface,
		"get_scene_snapshot",
		{{"instance_id", "physics_observation"}, {"options", json::object()}},
		failed
	);
	INFO(ended.dump());
	CHECK_FALSE(failed);
	bool endedObserved = false;
	for (const auto &event : ended.at("contact_events")) {
		endedObserved |= event.at("a_id") == "physics/floor" && event.at("b_id") == "physics/body" &&
						 event.at("phase") == "ended";
	}
	CHECK(endedObserved);
}

TEST_CASE(
	"data-factory read observations require and preserve their inspected lifecycle revision",
	"[control][datascene][data-factory]"
) {
	Universe universe;
	Universe decoy;
	DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) { return true; });
	const engine::world::DataFactoryWorldRequest create{
		.Operation = engine::world::DataFactoryWorldOperation::Create,
		.InstanceId = "fenced",
		.TickRate = 60.0,
		.OperationId = "fenced-create",
	};
	REQUIRE(session.CreateWorld(create).Status == DataFactoryStatus::Ok);
	const WorldId fenced = universe.Find(Name("fenced"));
	REQUIRE(fenced.IsValid());
	universe.Enter(fenced, [](engine::ecs::Store &) { engine::scene::RegisterSceneComponents(); });
	const WorldId foreign = World(universe, "foreign");
	REQUIRE(foreign.IsValid());
	REQUIRE(World(decoy, "fenced").IsValid());

	Surface surface("test", "test");
	surface.Enable(
		std::array{
			engine::control::features::DataFactory(session),
			// The supplied universe is deliberately a same-name decoy. Factory reads
			// must resolve through the session-owned universe instead.
			engine::control::features::DataScene(decoy, {}, &session),
		}
	);
	const auto current = session.Inspect("fenced");
	REQUIRE(current.Status == DataFactoryStatus::Ok);
	const json revision{
		{"expected_tick", current.Clock.Tick},
		{"expected_world_epoch", current.WorldEpoch},
		{"expected_world_version", current.WorldVersion},
	};

	bool failed = false;
	const json accepted =
		Call(surface, "get_scene_snapshot", {{"instance_id", "fenced"}, {"options", revision}}, failed);
	INFO(accepted.dump());
	CHECK_FALSE(failed);
	CHECK(accepted.at("schema_version") == "data-scene/v1");

	// Every read entry point receives the same decoy universe. Valid requests
	// may be unavailable for scene-specific reasons, but none may look up that
	// decoy and report the owned same-name world as missing.
	auto checkSessionUniverse = [&](std::string_view tool, json options) {
		for (const auto &[name, value] : revision.items())
			options[name] = value;
		const json response = Call(
			surface, std::string(tool), {{"instance_id", "fenced"}, {"options", std::move(options)}}, failed
		);
		INFO(tool << ": " << response.dump());
		CHECK(response.dump().find("no scene called") == std::string::npos);
	};
	checkSessionUniverse("get_camera_rendering_data", json::object());
	checkSessionUniverse("get_capture_channels", json::object());
	checkSessionUniverse("get_resources", json::object());
	checkSessionUniverse("get_event_narratives", json::object());
	checkSessionUniverse(
		"raycast", {{"origin", {0.0, 0.0, 0.0}}, {"direction", {0.0, 1.0, 0.0}}, {"max_distance_metres", 1.0}}
	);
	checkSessionUniverse("overlap_aabb", {{"minimum", {0.0, 0.0, 0.0}}, {"maximum", {1.0, 1.0, 1.0}}});
	checkSessionUniverse(
		"overlap_obb",
		{{"center", {0.0, 0.0, 0.0}},
		 {"orientation_xyzw", {0.0, 0.0, 0.0, 1.0}},
		 {"half_extent", {1.0, 1.0, 1.0}}}
	);

	const json missing =
		Call(surface, "get_scene_snapshot", {{"instance_id", "fenced"}, {"options", json::object()}}, failed);
	INFO(missing.dump());
	CHECK(failed);
	CHECK(missing.at("error").get<std::string>().find("expected lifecycle revision") != std::string::npos);

	REQUIRE(
		session.CommitExternalMutation("fenced", current.Clock.Tick, current.WorldVersion).Status ==
		DataFactoryStatus::Ok
	);
	const json stale =
		Call(surface, "get_scene_snapshot", {{"instance_id", "fenced"}, {"options", revision}}, failed);
	INFO(stale.dump());
	CHECK(failed);
	CHECK(stale.at("status") == "version_conflict");
	CHECK(stale.at("expected_world_version") == current.WorldVersion);
	CHECK(stale.at("current_world_version") == current.WorldVersion + 1);

	const size_t namesBeforeHostileRead = Name::Count();
	const json hostile = Call(
		surface,
		"get_scene_snapshot",
		{{"instance_id", "hostile-unowned-world"}, {"options", revision}},
		failed
	);
	INFO(hostile.dump());
	CHECK(failed);
	CHECK(hostile.at("status") == "validation_failed");
	CHECK(Name::Count() == namesBeforeHostileRead);
	CHECK_FALSE(Name::Exists("hostile-unowned-world"));

	const json crossWorld =
		Call(surface, "get_scene_snapshot", {{"instance_id", "foreign"}, {"options", revision}}, failed);
	INFO(crossWorld.dump());
	CHECK(failed);
	CHECK(crossWorld.at("status") == "validation_failed");
	CHECK(crossWorld.at("detail").get<std::string>().find("not owned") != std::string::npos);

	const json audit = Call(surface, "data_factory_operation_audit", json::object(), failed);
	INFO(audit.dump());
	CHECK_FALSE(failed);
	CHECK(audit.at("entries").empty());
}

TEST_CASE("compatibility data-scene reads reject factory revision fields", "[control][datascene]") {
	Universe universe;
	REQUIRE(World(universe, "compatibility").IsValid());
	Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});
	bool failed = false;
	const json response = Call(
		surface,
		"get_scene_snapshot",
		{{"instance_id", "compatibility"}, {"options", {{"expected_tick", 0}}}},
		failed
	);
	INFO(response.dump());
	CHECK(failed);
	CHECK(response.at("error").get<std::string>().find("unknown option") != std::string::npos);
	const auto tool =
		std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &item) {
			return item.Name == "get_scene_snapshot";
		});
	REQUIRE(tool != surface.Registered().end());
	const json schema = tool->Schema();
	CHECK_FALSE(schema["properties"]["options"].contains("expected_tick"));
}
