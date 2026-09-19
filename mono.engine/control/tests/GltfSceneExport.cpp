#include <engine/assets/Builtin.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/control/Surface.hpp>
#include <engine/control/features/DataScene.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/EditableMesh.hpp>
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

TEST_CASE("glTF scene export embeds editable image maps as PNG buffer views", "[control][gltf]") {
	engine::world::Universe universe;
	const auto world = World(universe, "gltf.textures");
	engine::control::Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});
	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneClasses();
		const auto image = store.CreateInstance(engine::scene::EditableImageClass(), "Pixel");
		REQUIRE(engine::scene::ResizeEditableImage(store, image, 1, 1));
		REQUIRE(
			engine::scene::EditableImageFromBuffer(
				store,
				image,
				std::vector<std::byte>{std::byte{12}, std::byte{34}, std::byte{56}, std::byte{255}}
			)
		);
		const auto part = engine::scene::MakePart(store, {});
		Identify(store, part, "export/textured");
		store.Set(
			part,
			engine::scene::SurfaceAppearance{
				.ColourMap = engine::scene::EditableImageContentName(store, image),
				.Mode = engine::scene::AlphaMode::Transparency,
			}
		);
	});
	bool failed = false;
	const json reply = Call(
		surface, "export_gltf_scene", {{"instance_id", "gltf.textures"}, {"options", json::object()}}, failed
	);
	REQUIRE_FALSE(failed);
	const auto bytes = Decode(reply.at("data").get<std::string>());
	const json document = GltfJson(bytes);
	REQUIRE(document.at("images").size() == 1);
	CHECK(document.at("images").at(0).at("mimeType") == "image/png");
	CHECK(document.at("materials").at(0).at("alphaMode") == "MASK");
	CHECK(document.at("materials").at(0).at("pbrMetallicRoughness").at("baseColorTexture").at("index") == 0);
	const auto &view =
		document.at("bufferViews").at(document.at("images").at(0).at("bufferView").get<size_t>());
	CHECK_FALSE(view.contains("target"));
	const size_t jsonBytes = static_cast<size_t>(std::to_integer<uint8_t>(bytes[12])) |
							 (static_cast<size_t>(std::to_integer<uint8_t>(bytes[13])) << 8) |
							 (static_cast<size_t>(std::to_integer<uint8_t>(bytes[14])) << 16) |
							 (static_cast<size_t>(std::to_integer<uint8_t>(bytes[15])) << 24);
	const size_t binaryStart = 20 + jsonBytes + 8;
	const size_t pngStart = binaryStart + view.at("byteOffset").get<size_t>();
	REQUIRE(pngStart + view.at("byteLength").get<size_t>() <= bytes.size());
	CHECK(std::to_integer<uint8_t>(bytes[pngStart]) == 0x89);
	CHECK(std::to_integer<char>(bytes[pngStart + 1]) == 'P');
	CHECK(std::to_integer<char>(bytes[pngStart + 2]) == 'N');
	CHECK(std::to_integer<char>(bytes[pngStart + 3]) == 'G');
}

TEST_CASE("glTF scene export uses a host's resident mesh source", "[control][gltf]") {
	engine::world::Universe universe;
	const auto world = World(universe, "gltf.delivered");
	bool sourceCalled = false;
	bool textureCalled = false;
	engine::control::Surface surface("test", "test");
	surface.Enable(
		std::array{engine::control::features::DataScene(
			universe,
			{},
			nullptr,
			[&](std::string_view owner, std::string_view name, engine::assets::MeshData &out) {
				sourceCalled = true;
				CHECK(owner == "gltf.delivered");
				CHECK(name == "content/delivered.amesh");
				out = engine::assets::MakeBuiltin(engine::assets::BuiltinMesh::Cube);
				engine::assets::Submesh red;
				red.FirstIndex = 0;
				red.IndexCount = 6;
				red.Texture = "content/delivered.atex";
				red.BaseColour[0] = 0.25f;
				out.Submeshes.push_back(red);
				engine::assets::Submesh blue;
				blue.FirstIndex = 6;
				blue.IndexCount = static_cast<uint32_t>(out.Indices.size()) - 6;
				blue.BaseColour[2] = 0.5f;
				out.Submeshes.push_back(blue);
				return engine::script::GltfMeshSourceStatus::Available;
			},
			[&](std::string_view owner, std::string_view name, engine::assets::TextureData &out) {
				textureCalled = true;
				CHECK(owner == "gltf.delivered");
				CHECK(name == "content/delivered.atex");
				out.Width = out.Height = 1;
				out.Format = engine::assets::TextureFormat::RGBA8;
				out.Pixels = {std::byte{128}, std::byte{64}, std::byte{32}, std::byte{255}};
				return engine::script::GltfTextureSourceStatus::Available;
			}
		)}
	);
	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneClasses();
		const auto part = engine::scene::MakePart(store, {});
		Identify(store, part, "export/delivered");
		auto visual = *store.Get<engine::scene::Visual>(part);
		visual.Mesh = engine::core::Name("content/delivered.amesh");
		store.Set(part, visual);
	});
	bool failed = false;
	const json reply = Call(
		surface, "export_gltf_scene", {{"instance_id", "gltf.delivered"}, {"options", json::object()}}, failed
	);
	REQUIRE_FALSE(failed);
	CHECK(sourceCalled);
	CHECK(textureCalled);
	CHECK(reply.at("unavailable").empty());
	const json document = GltfJson(Decode(reply.at("data").get<std::string>()));
	CHECK(document.at("nodes").at(0).at("extras").at("engine_stable_id") == "export/delivered");
	CHECK(document.at("meshes").size() == 1);
	const auto &primitives = document.at("meshes").at(0).at("primitives");
	REQUIRE(primitives.size() == 2);
	CHECK(document.at("accessors").at(primitives.at(0).at("indices").get<size_t>()).at("count") == 6);
	const auto &secondIndices = document.at("accessors").at(primitives.at(1).at("indices").get<size_t>());
	CHECK(secondIndices.at("count") == 30);
	CHECK(secondIndices.at("byteOffset") == 24);
	CHECK(document.at("materials").at(0).at("pbrMetallicRoughness").at("baseColorFactor").at(0) == 0.25f);
	CHECK(document.at("materials").at(0).at("pbrMetallicRoughness").at("baseColorTexture").at("index") == 0);
	CHECK_FALSE(document.at("materials").at(1).at("pbrMetallicRoughness").contains("baseColorTexture"));
	CHECK(document.at("images").size() == 1);
	CHECK(document.at("materials").at(1).at("pbrMetallicRoughness").at("baseColorFactor").at(2) == 0.5f);
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

TEST_CASE("glTF scene export retains and releases large GLB resources", "[control][gltf]") {
	engine::world::Universe universe;
	const auto world = World(universe, "gltf.large");
	engine::control::Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe)});
	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneClasses();
		const auto editable = store.CreateInstance(engine::scene::EditableMeshClass(), "LargeMesh");
		engine::scene::EditableMesh geometry;
		for (uint32_t index = 0; index < 1200; ++index) {
			geometry.Positions.push_back({static_cast<float>(index), static_cast<float>(index % 7), 0});
			geometry.Normals.push_back({0, 0, 1});
			geometry.UVs.push_back({static_cast<float>(index % 8) / 8.0f, 0});
			geometry.Indices.push_back(index);
		}
		store.Set(editable, geometry);
		const auto part = engine::scene::MakePart(
			store,
			{.Frame = {},
			 .Size = {1, 1, 1},
			 .Material = {},
			 .Mesh = engine::scene::EditableMeshContentName(store, editable),
			 .Class = {}}
		);
		Identify(store, part, "export/large");
	});
	const json request{{"instance_id", "gltf.large"}, {"options", json::object()}};
	bool failed = false;
	const json first = Call(surface, "export_gltf_scene", request, failed);
	REQUIRE_FALSE(failed);
	CHECK(first.at("encoding") == "resource");
	CHECK(first.at("data").is_null());
	REQUIRE(first.at("byte_length").get<size_t>() > 40 * 1024);
	const std::string resource = first.at("resource_id").get<std::string>();
	const std::string digest = first.at("hash").get<std::string>();
	std::vector<std::byte> bytes;
	const size_t total = first.at("byte_length").get<size_t>();
	for (size_t begin = 0; begin < total; begin += 4096) {
		const size_t end = std::min(begin + 4096, total);
		const json chunk = Call(
			surface,
			"get_gltf_scene_chunk",
			{{"instance_id", "gltf.large"},
			 {"resource_id", resource},
			 {"hash", digest},
			 {"byte_begin", begin},
			 {"byte_end", end},
			 {"options", json::object()}},
			failed
		);
		REQUIRE_FALSE(failed);
		const auto part = Decode(chunk.at("base64").get<std::string>());
		bytes.insert(bytes.end(), part.begin(), part.end());
	}
	CHECK(bytes.size() == total);
	CHECK(engine::assets::Hasher::Of(bytes).ToHex() == digest);
	CHECK(GltfJson(bytes).at("nodes").at(0).at("extras").at("engine_stable_id") == "export/large");

	Call(
		surface,
		"get_gltf_scene_chunk",
		{{"instance_id", "gltf.large"},
		 {"resource_id", resource},
		 {"hash", "wrong"},
		 {"byte_begin", 0},
		 {"byte_end", 1},
		 {"options", json::object()}},
		failed
	);
	CHECK(failed);
	const json second = Call(surface, "export_gltf_scene", request, failed);
	REQUIRE_FALSE(failed);
	CHECK(second.at("encoding") == "resource");
	Call(surface, "export_gltf_scene", request, failed);
	CHECK(failed);
	const json released = Call(
		surface, "release_gltf_scene", {{"instance_id", "gltf.large"}, {"resource_id", resource}}, failed
	);
	REQUIRE_FALSE(failed);
	CHECK(released.at("status") == "released");
	Call(
		surface,
		"get_gltf_scene_chunk",
		{{"instance_id", "gltf.large"},
		 {"resource_id", resource},
		 {"hash", digest},
		 {"byte_begin", 0},
		 {"byte_end", 1},
		 {"options", json::object()}},
		failed
	);
	CHECK(failed);
	Call(surface, "export_gltf_scene", request, failed);
	CHECK_FALSE(failed);
}

TEST_CASE("glTF resource reads keep their source lifecycle fence", "[control][gltf]") {
	engine::world::Universe universe;
	engine::world::DataFactorySession session(universe);
	session.SetPauseParticipant(
		[](engine::world::WorldId, engine::world::DataFactoryPauseScope, bool, std::string &) { return true; }
	);
	const engine::world::DataFactoryWorldRequest create{
		.Operation = engine::world::DataFactoryWorldOperation::Create,
		.InstanceId = "gltf.fenced",
		.TickRate = 60.0,
		.OperationId = "gltf-fenced-create",
	};
	REQUIRE(session.CreateWorld(create).Status == engine::world::DataFactoryStatus::Ok);
	const auto world = universe.Find(engine::core::Name("gltf.fenced"));
	REQUIRE(world.IsValid());
	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneClasses();
		for (uint32_t index = 0; index < 30; ++index) {
			const auto part = engine::scene::MakePart(store, {});
			Identify(store, part, "export/fenced/" + std::to_string(index));
		}
	});
	engine::control::Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataScene(universe, {}, &session)});
	const auto current = session.Inspect("gltf.fenced");
	REQUIRE(current.Status == engine::world::DataFactoryStatus::Ok);
	const json options{
		{"expected_tick", current.Clock.Tick},
		{"expected_world_epoch", current.WorldEpoch},
		{"expected_world_version", current.WorldVersion},
	};
	bool failed = false;
	const json exported =
		Call(surface, "export_gltf_scene", {{"instance_id", "gltf.fenced"}, {"options", options}}, failed);
	REQUIRE_FALSE(failed);
	REQUIRE(exported.at("encoding") == "resource");
	const std::string resource = exported.at("resource_id").get<std::string>();
	REQUIRE(
		session.CommitExternalMutation("gltf.fenced", current.Clock.Tick, current.WorldVersion).Status ==
		engine::world::DataFactoryStatus::Ok
	);
	const json stale = Call(
		surface,
		"get_gltf_scene_chunk",
		{{"instance_id", "gltf.fenced"},
		 {"resource_id", resource},
		 {"hash", exported.at("hash")},
		 {"byte_begin", 0},
		 {"byte_end", 1},
		 {"options", options}},
		failed
	);
	CHECK(failed);
	CHECK(stale.at("status") == "version_conflict");
	const json released = Call(
		surface, "release_gltf_scene", {{"instance_id", "gltf.fenced"}, {"resource_id", resource}}, failed
	);
	CHECK_FALSE(failed);
	CHECK(released.at("status") == "released");
}

TEST_CASE("raw scene extract preserves portable mesh and source texture sections", "[control][rawscene]") {
	engine::world::Universe universe;
	const auto world = World(universe, "raw.scene");
	engine::control::Surface surface("test", "test");
	const std::array<std::byte, 4> sourcePixels{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
	const engine::script::GltfTextureSource source =
		[&](std::string_view, std::string_view name, engine::assets::TextureData &out) {
			if (name != "content/raw.atex") return engine::script::GltfTextureSourceStatus::Missing;
			out.Width = 1;
			out.Height = 1;
			out.Format = engine::assets::TextureFormat::RGBA8_LINEAR;
			out.Pixels.assign(sourcePixels.begin(), sourcePixels.end());
			return engine::script::GltfTextureSourceStatus::Available;
		};
	surface.Enable(std::array{engine::control::features::DataScene(universe, {}, nullptr, {}, source)});
	universe.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneClasses();
		const auto part = engine::scene::MakePart(
			store,
			{.Frame = {},
			 .Size = {1, 1, 1},
			 .Material = {},
			 .Mesh = engine::core::Name(engine::assets::BuiltinName(engine::assets::BuiltinMesh::Cube)),
			 .Class = {}}
		);
		Identify(store, part, "raw/cube");
		const auto light = store.CreateInstance(
			engine::ecs::Classes::Find(engine::core::Name("PointLight")), "raw light"
		);
		REQUIRE(store.SetParent(light, part));
		engine::scene::Light point;
		point.Colour = {0.25f, 0.5f, 1.0f};
		point.Brightness = 3.0f;
		point.Range = 12.0f;
		store.Set(light, point);
		Identify(store, light, "raw/light");
		store.Set(
			part,
			engine::scene::SurfaceAppearance{
				.ColourMap = engine::core::Name("content/raw.atex"), .Mode = engine::scene::AlphaMode::Overlay
			}
		);
	});
	bool failed = false;
	const json began = Call(
		surface,
		"begin_raw_scene_extract",
		{{"instance_id", "raw.scene"}, {"options", json::object()}},
		failed
	);
	REQUIRE_FALSE(failed);
	CHECK(began.at("schema_version") == "raw-scene/v2");
	const json &mesh = began.at("manifest").at("meshes").at(0);
	CHECK(mesh.at("vertex_section").at("stride") == 48);
	CHECK(mesh.at("vertex_section").at("attributes").at(3).at("name") == "joints0");
	const json &texture = began.at("manifest").at("textures").at(0);
	CHECK(texture.at("format") == "rgba8_unorm");
	CHECK(texture.at("color_space") == "linear");
	CHECK(began.at("manifest").at("nodes").at(0).at("material").at("source_alpha_mode") == "overlay");
	const json &light = began.at("manifest").at("lights").at(0);
	CHECK(light.at("stable_id") == "raw/light");
	CHECK(light.at("kind") == "point");
	CHECK(light.at("brightness_unit") == "engine_brightness");
	CHECK(light.at("brightness") == 3.0f);
	const auto end = began.at("byte_length").get<uint64_t>();
	const json chunk = Call(
		surface,
		"get_raw_scene_chunk",
		{{"instance_id", "raw.scene"},
		 {"resource_id", began.at("resource_id")},
		 {"hash", began.at("hash")},
		 {"byte_begin", 0},
		 {"byte_end", end},
		 {"options", json::object()}},
		failed
	);
	REQUIRE_FALSE(failed);
	const std::vector<std::byte> bytes = Decode(chunk.at("base64").get<std::string>());
	CHECK(bytes.size() == end);
	CHECK(engine::assets::Hasher::Of(bytes).ToHex() == began.at("hash").get<std::string>());
	const size_t textureOffset = texture.at("byte_offset").get<size_t>();
	CHECK(std::equal(sourcePixels.begin(), sourcePixels.end(), bytes.begin() + textureOffset));
	const json released = Call(
		surface,
		"release_raw_scene_extract",
		{{"instance_id", "raw.scene"}, {"resource_id", began.at("resource_id")}},
		failed
	);
	CHECK_FALSE(failed);
	CHECK(released.at("status") == "released");
	Call(
		surface,
		"get_raw_scene_chunk",
		{{"instance_id", "raw.scene"},
		 {"resource_id", began.at("resource_id")},
		 {"hash", began.at("hash")},
		 {"byte_begin", 0},
		 {"byte_end", 1},
		 {"options", json::object()}},
		failed
	);
	CHECK(failed);
}
