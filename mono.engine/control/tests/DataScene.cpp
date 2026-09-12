#include <engine/control/Surface.hpp>
#include <engine/control/features/DataScene.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.control.datascene")
TEST_DEPENDS("engine.script.datascene")

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
		store.Set<Camera>(camera, Camera{});
		store.Set<Transform>(camera, Transform{CFrame{Vector3::Zero}});
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
