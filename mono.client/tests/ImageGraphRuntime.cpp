#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/TextureTable.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <client/ImageGraphRuntime.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("client.imagegraphruntime")
TEST_DEPENDS("engine.imagegraph.document")
TEST_DEPENDS("engine.scene.imagegraphbinding")
TEST_DEPENDS("engine.render.liveimagepublisher")

namespace {
	const engine::core::Name GRAPH("changing-solid");
	const engine::core::Name OUTPUT("final");
	const engine::core::Name TEXTURE("graph-live-image");
	const engine::core::Name OWNER("imagegraph-world");

	constexpr std::string_view GRAPH_TEXT = "imagegraph 1\n"
											"node \"solid\" \"image.solid\" \"\" 0 0\n"
											"value 0 \"solid\" \"width\" i 2\n"
											"value 0 \"solid\" \"height\" i 1\n"
											"value 0 \"solid\" \"colour\" c 12 34 56 255\n"
											"keyframe \"solid\" \"width\" 0 \"step\" i 2\n"
											"keyframe \"solid\" \"width\" 2 \"step\" i 3\n"
											"keyframe \"solid\" \"colour\" 0 \"step\" c 1 2 3 255\n"
											"keyframe \"solid\" \"colour\" 2 \"step\" c 4 5 6 255\n"
											"output \"final\" \"solid\" \"image\"\n";

	struct GraphFile {
		std::filesystem::path Assets =
			std::filesystem::temp_directory_path() / "atomic-imagegraph-runtime-test";
		GraphFile() {
			std::filesystem::remove_all(Assets);
			std::filesystem::create_directories(Assets / "imagegraphs");
			std::ofstream out(client::ImageGraphDocumentPath(Assets, GRAPH));
			out << GRAPH_TEXT;
		}
		~GraphFile() {
			std::filesystem::remove_all(Assets);
		}
	};
}

TEST_CASE("native graph path is a bounded single stem", "[client][imagegraph]") {
	const std::filesystem::path assets("assets");
	CHECK(client::ImageGraphDocumentPath(assets, GRAPH) == assets / "imagegraphs/changing-solid.graph");
	CHECK(client::ImageGraphDocumentPath(assets, engine::core::Name("../escape")).empty());
	CHECK(client::ImageGraphDocumentPath(assets, engine::core::Name("nested/name")).empty());
	CHECK(client::ImageGraphDocumentPath(assets, engine::core::Name("hidden.graph")).empty());
}

TEST_CASE("saved keyframes produce different frames by selected tick", "[client][imagegraph]") {
	GraphFile file;
	const auto first = client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 0);
	const auto second = client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 2);
	REQUIRE(first.Status == engine::imagegraph::Status::Ok);
	REQUIRE(second.Status == engine::imagegraph::Status::Ok);
	const auto seeded = client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 2, 7781);
	REQUIRE(seeded.Status == engine::imagegraph::Status::Ok);
	CHECK(first.Image.Width == 2);
	CHECK(second.Image.Width == 3);
	CHECK(first.Image.Pixels[0] == 1);
	CHECK(second.Image.Pixels[0] == 4);
	CHECK(first.Image.Hash != second.Image.Hash);
	CHECK(seeded.Image.Hash == second.Image.Hash);
	CHECK(
		client::LoadImageGraphFrame(file.Assets, GRAPH, engine::core::Name("missing"), 0).Status ==
		engine::imagegraph::Status::InvalidOutput
	);
}

TEST_CASE("host refuses oversized and malformed saved documents", "[client][imagegraph]") {
	GraphFile file;
	const auto path = client::ImageGraphDocumentPath(file.Assets, GRAPH);
	{
		std::ofstream oversized(path, std::ios::binary | std::ios::trunc);
		oversized.seekp(8 * 1024 * 1024);
		oversized.put('x');
	}
	CHECK(
		client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 0).Status ==
		engine::imagegraph::Status::LimitExceeded
	);
	{
		std::ofstream malformed(path, std::ios::binary | std::ios::trunc);
		malformed << "not an image graph";
	}
	CHECK(
		client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 0).Status ==
		engine::imagegraph::Status::Malformed
	);
}

TEST_CASE("world tick changes a real owner-scoped renderer texture", "[client][imagegraph][gpu][.]") {
	GraphFile file;
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("imagegraph-test-world");
	const auto entity = store.Create();
	engine::scene::ImageGraphBinding selector;
	selector.Graph = GRAPH;
	selector.Output = OUTPUT;
	selector.Texture = TEXTURE;
	selector.TickPolicy = engine::scene::ImageGraphTickPolicy::World;
	REQUIRE(engine::scene::SetImageGraphBinding(store, entity, selector));

	REQUIRE(SDL_Init(SDL_INIT_VIDEO));
	engine::render::Renderer renderer;
	REQUIRE(renderer.Initialise(nullptr));
	client::ImageGraphRuntime runtime;
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	CHECK(runtime.DocumentParses() == 1);
	uint32_t width = 0, height = 0;
	REQUIRE(renderer.TextureSize(TEXTURE, width, height, OWNER));
	CHECK(width == 2);
	CHECK(height == 1);
	CHECK(renderer.TextureHandle(TEXTURE, OWNER) != nullptr);

	store.AdvanceTick(1.0f / 60.0f);
	store.AdvanceTick(1.0f / 60.0f);
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	CHECK(runtime.DocumentParses() == 1);
	REQUIRE(renderer.TextureSize(TEXTURE, width, height, OWNER));
	CHECK(width == 3);
	CHECK(height == 1);
	const auto path = client::ImageGraphDocumentPath(file.Assets, GRAPH);
	std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(1));
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	CHECK(runtime.DocumentParses() == 2);

	// Invalid replacement leaves the last uploaded output visible.
	selector.Output = engine::core::Name("missing");
	REQUIRE(engine::scene::SetImageGraphBinding(store, entity, selector));
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	REQUIRE(renderer.TextureSize(TEXTURE, width, height, OWNER));
	CHECK(width == 3);
	engine::ecs::Store replacement("imagegraph-recreated-world");
	const auto replacementEntity = replacement.Create();
	REQUIRE(engine::scene::SetImageGraphBinding(replacement, replacementEntity, selector));
	runtime.BeginFrame();
	CHECK(runtime.Refresh(replacement, renderer, OWNER, file.Assets) == 0);
	CHECK(renderer.TextureHandle(TEXTURE, OWNER) == nullptr);
	selector.Output = OUTPUT;
	REQUIRE(engine::scene::SetImageGraphBinding(replacement, replacementEntity, selector));
	runtime.BeginFrame();
	CHECK(runtime.Refresh(replacement, renderer, OWNER, file.Assets) == 1);
	CHECK(renderer.TextureHandle(TEXTURE, OWNER) != nullptr);

	replacement.Remove<engine::scene::ImageGraphBinding>(replacementEntity);
	CHECK(runtime.Refresh(replacement, renderer, OWNER, file.Assets) == 0);
	CHECK(renderer.TextureHandle(TEXTURE, OWNER) == nullptr);
	runtime.Clear(renderer);
	renderer.Shutdown();
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

TEST_CASE("numeric material maps require linear live output", "[client][imagegraph][gpu][.]") {
	GraphFile file;
	engine::scene::RegisterSceneComponents();
	engine::gui::RegisterGuiComponents();
	engine::ecs::Store store("imagegraph-linear-map-world");
	const auto surface = store.Create();
	engine::scene::SurfaceAppearance appearance;
	appearance.NormalMap = TEXTURE;
	store.Set(surface, appearance);
	const auto sink = store.Create();
	engine::scene::ImageGraphBinding selector;
	selector.Graph = GRAPH;
	selector.Output = OUTPUT;
	selector.Texture = TEXTURE;
	selector.ColorSpace = engine::scene::ImageGraphColorSpace::Linear;
	REQUIRE(engine::scene::SetImageGraphBinding(store, sink, selector));
	REQUIRE(SDL_Init(SDL_INIT_VIDEO));
	engine::render::Renderer renderer;
	REQUIRE(renderer.Initialise(nullptr, 1, true));
	client::ImageGraphRuntime runtime;
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	engine::assets::TextureData copied;
	REQUIRE(renderer.CopyTexture(TEXTURE, copied, 1024, OWNER) == engine::render::TextureCopyStatus::Copied);
	CHECK(copied.Format == engine::assets::TextureFormat::RGBA8_LINEAR);
	selector.ColorSpace = engine::scene::ImageGraphColorSpace::Display;
	REQUIRE(engine::scene::SetImageGraphBinding(store, sink, selector));
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.LastError().find("colour space") != std::string::npos);
	REQUIRE(renderer.CopyTexture(TEXTURE, copied, 1024, OWNER) == engine::render::TextureCopyStatus::Copied);
	CHECK(copied.Format == engine::assets::TextureFormat::RGBA8_LINEAR);
	selector.ColorSpace = engine::scene::ImageGraphColorSpace::Linear;
	REQUIRE(engine::scene::SetImageGraphBinding(store, sink, selector));
	const auto picture = store.Create();
	engine::gui::Picture graphic;
	graphic.Image = TEXTURE;
	store.Set(picture, graphic);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 0);
	CHECK(runtime.LastError().find("colour space") != std::string::npos);
	store.Remove<engine::gui::Picture>(picture);
	selector.FixedTick = 2;
	REQUIRE(engine::scene::SetImageGraphBinding(store, sink, selector));
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	runtime.Clear(renderer);
	renderer.Shutdown();
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

TEST_CASE("binding budget defers work and eventually publishes every name", "[client][imagegraph][gpu][.]") {
	GraphFile file;
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("imagegraph-budget-world");
	std::vector<engine::core::Name> names;
	for (size_t index = 0; index < client::ImageGraphRuntime::MAXIMUM_CHECKS_PER_FRAME + 3; ++index) {
		const auto entity = store.Create();
		engine::scene::ImageGraphBinding selector;
		selector.Graph = engine::core::Name("graph-budget-document-" + std::to_string(index));
		{
			std::ofstream document(client::ImageGraphDocumentPath(file.Assets, selector.Graph));
			document << GRAPH_TEXT;
		}
		selector.Output = OUTPUT;
		selector.Texture = engine::core::Name("graph-budget-" + std::to_string(index));
		REQUIRE(engine::scene::SetImageGraphBinding(store, entity, selector));
		names.push_back(selector.Texture);
	}
	REQUIRE(SDL_Init(SDL_INIT_VIDEO));
	engine::render::Renderer renderer;
	REQUIRE(renderer.Initialise(nullptr));
	client::ImageGraphRuntime runtime;
	runtime.BeginFrame();
	CHECK(
		runtime.Refresh(store, renderer, OWNER, file.Assets) ==
		client::ImageGraphRuntime::MAXIMUM_CHECKS_PER_FRAME
	);
	size_t firstPass = 0;
	for (const auto name : names)
		firstPass += renderer.TextureHandle(name, OWNER) != nullptr;
	CHECK(firstPass == client::ImageGraphRuntime::MAXIMUM_CHECKS_PER_FRAME);
	runtime.BeginFrame();
	CHECK(runtime.Refresh(store, renderer, OWNER, file.Assets) == 3);
	CHECK(runtime.DocumentParses() == names.size());
	CHECK(runtime.CachedDocumentCount() == client::ImageGraphRuntime::MAXIMUM_CACHED_DOCUMENTS);
	CHECK(runtime.CachedSourceBytes() <= client::ImageGraphRuntime::MAXIMUM_CACHED_DOCUMENT_BYTES);
	for (const auto name : names)
		CHECK(renderer.TextureHandle(name, OWNER) != nullptr);
	runtime.Clear(renderer);
	renderer.Shutdown();
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
