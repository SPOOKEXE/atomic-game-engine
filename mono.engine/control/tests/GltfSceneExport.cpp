#include <engine/assets/Builtin.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/control/Surface.hpp>
#include <engine/control/features/DataScene.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.control.gltfsceneexport")
TEST_DEPENDS("engine.script.gltfsceneexport")

namespace {
	using nlohmann::json;

	engine::world::WorldId World(engine::world::Universe &universe, std::string_view name) {
		engine::world::WorldSettings settings;
		settings.Name = engine::core::Name(name);
		return universe.Create(settings);
	}

	void Identify(engine::ecs::Store &store, engine::ecs::Entity entity, std::string_view id) {
		engine::ecs::AttributeValue value;
		value.Type = engine::ecs::PropertyType::String;
		value.String = id;
		REQUIRE(engine::ecs::SetAttribute(store, entity, engine::core::Name("DataFactoryId"), value));
	}

	json Call(engine::control::Surface &surface, std::string_view name, const json &arguments, bool &failed) {
		const json reply = json::parse(surface.Answer(
			json{
				{"jsonrpc", "2.0"},
				{"id", 1},
				{"method", "tools/call"},
				{"params", {{"name", name}, {"arguments", arguments}}}
			}.dump()
		));
		const json &result = reply.at("result");
		failed = result.value("isError", false);
		return json::parse(result.at("content").at(0).at("text").get<std::string>());
	}

	std::vector<std::byte> Decode(std::string_view value) {
		const auto digit = [](char character) -> uint8_t {
			if (character >= 'A' && character <= 'Z') return static_cast<uint8_t>(character - 'A');
			if (character >= 'a' && character <= 'z') return static_cast<uint8_t>(character - 'a' + 26);
			if (character >= '0' && character <= '9') return static_cast<uint8_t>(character - '0' + 52);
			return character == '+' ? 62 : 63;
		};
		std::vector<std::byte> out;
		for (size_t index = 0; index < value.size(); index += 4) {
			const uint32_t group = digit(value[index]) << 18 | digit(value[index + 1]) << 12 |
								   (value[index + 2] == '=' ? 0 : digit(value[index + 2])) << 6 |
								   (value[index + 3] == '=' ? 0 : digit(value[index + 3]));
			out.push_back(static_cast<std::byte>(group >> 16));
			if (value[index + 2] != '=') out.push_back(static_cast<std::byte>(group >> 8));
			if (value[index + 3] != '=') out.push_back(static_cast<std::byte>(group));
		}
		return out;
	}

	json GltfJson(const std::vector<std::byte> &bytes) {
		const auto word = [&bytes](size_t offset) {
			return static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[offset])) |
				   static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[offset + 1])) << 8 |
				   static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[offset + 2])) << 16 |
				   static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[offset + 3])) << 24;
		};
		REQUIRE(bytes.size() >= 20);
		REQUIRE(word(12) <= bytes.size() - 20);
		return json::parse(
			reinterpret_cast<const char *>(bytes.data() + 20),
			reinterpret_cast<const char *>(bytes.data() + 20 + word(12))
		);
	}
}

TEST_CASE("glTF scene export MCP returns a checksummed GLB with stable IDs", "[control][gltf]") {
	engine::world::Universe universe;
	const auto world = World(universe, "gltf");
	engine::control::Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});
	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneClasses();
		const auto part = engine::scene::MakePart(
			store,
			{
				.Frame = {},
				.Size = {1, 1, 1},
				.Material = {},
				.Mesh = engine::core::Name(engine::assets::BuiltinName(engine::assets::BuiltinMesh::Cube)),
				.Class = {},
			}
		);
		Identify(store, part, "export/cube");
		auto visual = *store.Get<engine::scene::Visual>(part);
		visual.Transparency = 0.5f;
		store.Set(part, visual);
		const auto unavailable = engine::scene::MakePart(
			store,
			{.Frame = {},
			 .Size = {1, 1, 1},
			 .Material = {},
			 .Mesh = engine::core::Name("content/unknown.amesh"),
			 .Class = {}}
		);
		Identify(store, unavailable, "export/unknown");
	});
	bool failed = false;
	const json reply =
		Call(surface, "export_gltf_scene", {{"instance_id", "gltf"}, {"options", json::object()}}, failed);
	CHECK_FALSE(failed);
	CHECK(reply.at("schema_version") == "gltf-scene/v1");
	CHECK(reply.at("format") == "glb");
	CHECK(reply.at("hash_algorithm") == "blake3-256");
	CHECK(reply.at("unavailable").at(0).at("reason") == "source_geometry_unavailable");
	const std::vector<std::byte> bytes = Decode(reply.at("data").get<std::string>());
	CHECK(bytes.size() == reply.at("byte_length").get<size_t>());
	REQUIRE(bytes.size() >= 12);
	CHECK(std::to_integer<char>(bytes[0]) == 'g');
	CHECK(std::to_integer<char>(bytes[1]) == 'l');
	CHECK(std::to_integer<char>(bytes[2]) == 'T');
	CHECK(std::to_integer<char>(bytes[3]) == 'F');
	const json document = GltfJson(bytes);
	CHECK(document.at("materials").at(0).at("alphaMode") == "BLEND");
	CHECK(document.at("materials").at(0).at("pbrMetallicRoughness").at("baseColorFactor").at(3) == 0.5f);
	CHECK(engine::assets::Hasher::Of(bytes).ToHex() == reply.at("hash").get<std::string>());
}

TEST_CASE("glTF scene export omits binary structures when a scene has no geometry", "[control][gltf]") {
	engine::world::Universe universe;
	World(universe, "gltf.empty");
	const auto cameraOnly = World(universe, "gltf.camera");
	engine::control::Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});
	universe.Enter(cameraOnly, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneClasses();
		const auto camera = store.Create();
		store.Set(camera, engine::ecs::InstanceName{engine::core::Name("Camera")});
		store.Set(camera, engine::scene::Transform{});
		store.Set(camera, engine::scene::Camera{});
		Identify(store, camera, "export/camera");
	});

	for (const std::string_view world : {"gltf.empty", "gltf.camera"}) {
		bool failed = false;
		const json reply =
			Call(surface, "export_gltf_scene", {{"instance_id", world}, {"options", json::object()}}, failed);
		CHECK_FALSE(failed);
		const std::vector<std::byte> bytes = Decode(reply.at("data").get<std::string>());
		const json document = GltfJson(bytes);
		CHECK_FALSE(document.contains("buffers"));
		CHECK_FALSE(document.contains("bufferViews"));
		CHECK_FALSE(document.contains("accessors"));
		CHECK_FALSE(document.contains("meshes"));
		CHECK_FALSE(document.contains("materials"));
		CHECK(bytes.size() == 20 + document.dump().size() + ((4 - document.dump().size() % 4) % 4));
		if (world == "gltf.empty") {
			CHECK_FALSE(document.contains("cameras"));
			CHECK_FALSE(document.contains("nodes"));
			CHECK_FALSE(document.at("scenes").at(0).contains("nodes"));
		} else
			REQUIRE(document.at("cameras").size() == 1);
	}
}
