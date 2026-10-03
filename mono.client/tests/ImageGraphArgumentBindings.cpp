#include <engine/ecs/Store.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/SourceArgumentHost.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/TextureTable.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <client/ImageGraphRuntime.hpp>
#include <filesystem>
#include <fstream>

TEST_SUITE_ID("client.imagegraph_argument_bindings")
TEST_DEPENDS("engine.imagegraph.source_argument")
TEST_DEPENDS("engine.scene.imagegraphbinding")
TEST_DEPENDS("engine.render.liveimagepublisher")

namespace {
	using namespace engine::imagegraph;
	const engine::core::Name GRAPH{"argument-binding"}, OUTPUT{"image"}, TEXTURE{"argument-binding-texture"},
		OWNER{"argument-binding-owner"};
	struct GraphFile {
		std::filesystem::path Assets =
			std::filesystem::temp_directory_path() / "atomic-argument-binding-fixture";
		GraphFile(bool transform = false) {
			std::filesystem::remove_all(Assets);
			std::filesystem::create_directories(Assets / "imagegraphs");
			Document document;
			document.FormatVersion = 9;
			document.Nodes = {
				{"argument",
				 "pc.argument",
				 "",
				 {},
				 {{"tag", std::string{"width"}}, {"type", EnumValue{1}}, {"default_value", 2.}}},
				{"dimension", "pc.vector2", "", {}, {{"x", 2.0}, {"y", 1.0}}},
				{"solid",
				 "pc.solid",
				 "",
				 {},
				 {{"dimension", Vector2{2, 1}},
				  {"dimension_unit", EnumValue{0}},
				  {"color", Colour{31, 47, 59, 255}},
				  {"attribute_color_depth", EnumValue{3}}}}
			};
			document.Links = {
				{"argument", "value", "dimension", "x"}, {"dimension", "vector", "solid", "dimension"}
			};
			document.Outputs = {{"image", "solid", "surface_out"}};
			if (transform) {
				document.Nodes.push_back(
					{"transform",
					 "image.transform_3d",
					 "",
					 {},
					 {{"position", Vector3{}},
					  {"anchor", Vector3{}},
					  {"rotation", Quaternion{}},
					  {"scale", Vector3{1, 1, 1}},
					  {"texture_tiling", Vector2{1, 1}},
					  {"projection", EnumValue{1}},
					  {"fov", 45.0},
					  {"view_range", Vector2{.001, 10}},
					  {"depth_range", Vector2{0, 1}}}}
				);
				document.Links.push_back({"solid", "surface_out", "transform", "surface"});
				document.Outputs = {{"image", "transform", "rendered"}};
			}
			std::ofstream output(client::ImageGraphDocumentPath(Assets, GRAPH));
			output << Write(document);
		}
		~GraphFile() {
			std::error_code ignored;
			std::filesystem::remove_all(Assets, ignored);
		}
	};
	void Bind(engine::ecs::Store &store) {
		engine::scene::ImageGraphBinding binding;
		binding.Graph = GRAPH;
		binding.Output = OUTPUT;
		binding.Texture = TEXTURE;
		binding.TickPolicy = engine::scene::ImageGraphTickPolicy::Fixed;
		binding.FixedTick = 0;
		REQUIRE(engine::scene::SetImageGraphBinding(store, store.Create(), binding));
	}
}

TEST_CASE(
	"Actual cached binding reads replacement arguments at the same tick before device publication",
	"[client][source_argument][argument_binding]"
) {
	GraphFile file;
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("argument-idle-binding");
	Bind(store);
	engine::render::Renderer renderer;
	client::ImageGraphRuntime runtime;
	Diagnostic diagnostic;
	const std::string_view outside[] = {"width=999999"};
	REQUIRE(runtime.PrepareArguments({{}, {}, outside, {}}, renderer, diagnostic) == Status::Ok);
	const auto tick = store.Time().Tick;
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.LastError() == "dimensions exceed native limits");
	CHECK(runtime.DocumentParses() == 1);
	CHECK(runtime.CachedDocumentCount() == 1);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.LastError() == "dimensions exceed native limits");
	CHECK(runtime.DocumentParses() == 1);

	const auto generation = runtime.ArgumentGeneration();
	const std::string_view invalid[] = {"width=3tail"};
	CHECK(runtime.PrepareArguments({{}, {}, invalid, {}}, renderer, diagnostic) == Status::InvalidValue);
	CHECK(runtime.ArgumentGeneration() == generation);
	CHECK(runtime.CachedDocumentCount() == 1);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.LastError() == "dimensions exceed native limits");
	CHECK(runtime.DocumentParses() == 1);

	const std::string_view valid[] = {"width=3"};
	REQUIRE(runtime.PrepareArguments({{}, {}, valid, {}}, renderer, diagnostic) == Status::Ok);
	CHECK(runtime.ArgumentGeneration() == generation + 1);
	CHECK(runtime.CachedDocumentCount() == 0);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.LastError() == "image graph texture upload failed");
	CHECK(runtime.DocumentParses() == 2);
	CHECK(runtime.CachedDocumentCount() == 1);
	CHECK(store.Time().Tick == tick);
	uint32_t width = 0, height = 0;
	CHECK_FALSE(renderer.TextureSize(TEXTURE, width, height, OWNER));

	CHECK(runtime.PrepareArguments({{}, {}, outside, {}}, renderer, diagnostic, 1) == Status::LimitExceeded);
	CHECK(runtime.ArgumentGeneration() == generation + 1);
	CHECK(runtime.CachedDocumentCount() == 1);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.LastError() == "image graph texture upload failed");
	CHECK(runtime.DocumentParses() == 2);

	REQUIRE(runtime.PrepareArguments({{}, {}, outside, {}}, renderer, diagnostic) == Status::Ok);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.LastError() == "dimensions exceed native limits");
	CHECK(runtime.DocumentParses() == 3);
	CHECK(store.Time().Tick == tick);
	runtime.Clear(renderer);
	CHECK(runtime.CachedDocumentCount() == 0);
}

TEST_CASE(
	"Published cached binding replaces same-tick pixels and retires the prior argument generation",
	"[client][source_argument][argument_binding][gpu][.]"
) {
	GraphFile file;
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("argument-device-binding");
	Bind(store);
	REQUIRE(SDL_Init(SDL_INIT_VIDEO));
	struct Video {
		~Video() {
			SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}
	} video;
	engine::render::Renderer renderer;
	REQUIRE(renderer.Initialise(nullptr));
	client::ImageGraphRuntime runtime;
	struct Retire {
		client::ImageGraphRuntime &Runtime;
		engine::render::Renderer &Renderer;
		~Retire() {
			Runtime.Clear(Renderer);
			Renderer.Shutdown();
		}
	} retire{runtime, renderer};
	const auto tick = store.Time().Tick;
	const auto path = client::ImageGraphDocumentPath(file.Assets, GRAPH);
	const auto modified = std::filesystem::last_write_time(path);
	const auto fileBytes = std::filesystem::file_size(path);
	const auto copy = [&](uint32_t expectedWidth) {
		engine::assets::TextureData image;
		REQUIRE(
			renderer.CopyTexture(TEXTURE, image, 1024, OWNER) == engine::render::TextureCopyStatus::Copied
		);
		CHECK(image.Width == expectedWidth);
		CHECK(image.Height == 1);
		REQUIRE(image.Pixels.size() == expectedWidth * 4);
		for (size_t pixel = 0; pixel < expectedWidth; ++pixel) {
			CHECK(image.Pixels[pixel * 4] == std::byte{31});
			CHECK(image.Pixels[pixel * 4 + 1] == std::byte{47});
			CHECK(image.Pixels[pixel * 4 + 2] == std::byte{59});
			CHECK(image.Pixels[pixel * 4 + 3] == std::byte{255});
		}
	};
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	copy(2);
	CHECK(runtime.DocumentParses() == 1);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.DocumentParses() == 1);

	Diagnostic diagnostic;
	const auto generation = runtime.ArgumentGeneration();
	const std::string_view three[] = {"width=3"};
	REQUIRE(runtime.PrepareArguments({{}, {}, three, {}}, renderer, diagnostic) == Status::Ok);
	CHECK(runtime.ArgumentGeneration() == generation + 1);
	CHECK(runtime.CachedDocumentCount() == 0);
	uint32_t width = 0, height = 0;
	CHECK_FALSE(renderer.TextureSize(TEXTURE, width, height, OWNER));
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	copy(3);
	CHECK(runtime.DocumentParses() == 2);

	const std::string_view malformed[] = {"width=4tail"};
	CHECK(runtime.PrepareArguments({{}, {}, malformed, {}}, renderer, diagnostic) == Status::InvalidValue);
	CHECK(runtime.ArgumentGeneration() == generation + 1);
	CHECK(runtime.CachedDocumentCount() == 1);
	copy(3);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.DocumentParses() == 2);

	const std::string_view four[] = {"width=4"};
	CHECK(runtime.PrepareArguments({{}, {}, four, {}}, renderer, diagnostic, 1) == Status::LimitExceeded);
	CHECK(runtime.ArgumentGeneration() == generation + 1);
	copy(3);
	REQUIRE(runtime.PrepareArguments({{}, {}, four, {}}, renderer, diagnostic) == Status::Ok);
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	copy(4);
	CHECK(runtime.DocumentParses() == 3);
	REQUIRE(runtime.PrepareArguments({}, renderer, diagnostic) == Status::Ok);
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	copy(2);
	CHECK(runtime.DocumentParses() == 4);
	CHECK(store.Time().Tick == tick);
	CHECK(std::filesystem::last_write_time(path) == modified);
	CHECK(std::filesystem::file_size(path) == fileBytes);
}

TEST_CASE(
	"Argument replacement cancels actual queued binding work while stale generations cannot cancel its "
	"successor",
	"[client][source_argument][argument_binding]"
) {
	GraphFile file(true);
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("argument-queued-binding");
	Bind(store);
	engine::render::Renderer renderer;
	client::ImageGraphRuntime runtime;
	Diagnostic diagnostic;
	const std::string_view three[] = {"width=3"};
	REQUIRE(runtime.PrepareArguments({{}, {}, three, {}}, renderer, diagnostic) == Status::Ok);
	const auto generation = runtime.ArgumentGeneration();
	const auto tick = store.Time().Tick;
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	CHECK(runtime.LastError().empty());
	CHECK(runtime.DocumentParses() == 1);
	CHECK(runtime.CachedDocumentCount() == 1);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.DocumentParses() == 1);
	const std::string_view malformed[] = {"width=4tail"};
	CHECK(runtime.PrepareArguments({{}, {}, malformed, {}}, renderer, diagnostic) == Status::InvalidValue);
	CHECK(runtime.ArgumentGeneration() == generation);
	CHECK(runtime.CachedDocumentCount() == 1);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.DocumentParses() == 1);

	const std::string_view four[] = {"width=4"};
	REQUIRE(runtime.PrepareArguments({{}, {}, four, {}}, renderer, diagnostic) == Status::Ok);
	CHECK(runtime.ArgumentGeneration() == generation + 1);
	CHECK(runtime.CachedDocumentCount() == 0);
	CHECK_FALSE(renderer.CancelTransformImage3D(OWNER, TEXTURE, 1));
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	CHECK(runtime.LastError().empty());
	CHECK(runtime.DocumentParses() == 2);
	CHECK_FALSE(renderer.CancelTransformImage3D(OWNER, TEXTURE, 1));

	const std::string_view five[] = {"width=5"};
	CHECK(runtime.PrepareArguments({{}, {}, five, {}}, renderer, diagnostic, 1) == Status::LimitExceeded);
	CHECK(runtime.ArgumentGeneration() == generation + 1);
	CHECK(runtime.CachedDocumentCount() == 1);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.DocumentParses() == 2);
	CHECK_FALSE(renderer.CancelTransformImage3D(OWNER, TEXTURE, 1));
	CHECK(renderer.CancelTransformImage3D(OWNER, TEXTURE, 2));
	CHECK(store.Time().Tick == tick);
	runtime.Clear(renderer);
	CHECK_FALSE(renderer.CancelTransformImage3D(OWNER, TEXTURE, 2));
}
