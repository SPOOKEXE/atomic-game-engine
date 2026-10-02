#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/TextureTable.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <client/ImageGraphRuntime.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("client.imagegraphruntime")
TEST_DEPENDS("engine.imagegraph.document")
TEST_DEPENDS("engine.imagegraphphysics.rigid_graph")
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
	engine::imagegraph::Document RigidGraph() {
		using namespace engine::imagegraph;
		Document document;
		document.FormatVersion = 9;
		document.Project = ProjectSettings{};
		document.Project->SurfaceWidth = document.Project->SurfaceHeight = 32;
		document.Nodes = {
			{"owner",
			 "pc.rigid_group_inline",
			 "",
			 {},
			 {{"dimension", Vector2{32, 32}},
			  {"dimension_unit", EnumValue{0}},
			  {"simulation_scale", 16.},
			  {"strength", 10.},
			  {"use_wall", true},
			  {"walls", int64_t{2}}}},
			{"texture",
			 "image.solid",
			 "rigid",
			 {},
			 {{"width", int64_t{4}}, {"height", int64_t{4}}, {"colour", Colour{255, 80, 20, 255}}}},
			{"body",
			 "pc.rigid_object",
			 "rigid",
			 {},
			 {{"spawn", true},
			  {"spawn_frame", int64_t{0}},
			  {"spawn_position", Vector2{16, 6}},
			  {"spawn_position_unit", EnumValue{0}},
			  {"fix_rotation", true}}},
			{"render", "pc.rigid_render", "rigid", {}, {{"timestep", 100.}, {"round_position", true}}}
		};
		document.Nodes.back().DynamicInputs = {{"object_0", ValueType::Rigid, std::nullopt}};
		Group group{"rigid", "Rigid native contact study"};
		group.OwnerNodeId = "owner";
		document.Groups = {group};
		document.Links = {{"texture", "image", "body", "texture"}, {"body", "object", "render", "object_0"}};
		document.Outputs = {{"final", "render", "surface_out"}};
		return document;
	}
}

TEST_CASE(
	"Client rigid sampling seeks played frames and repeats reset deterministically",
	"[client][imagegraph][rigid]"
) {
	GraphFile file;
	{
		std::ofstream out(client::ImageGraphDocumentPath(file.Assets, GRAPH));
		out << engine::imagegraph::Write(RigidGraph());
	}
	const auto first = client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 0);
	INFO(first.Diagnostic.Message);
	REQUIRE(first.Status == engine::imagegraph::Status::Ok);
	CHECK(first.Animated);
	CHECK(first.Image.Width == 32);
	CHECK(first.Image.Height == 32);
	CHECK(std::any_of(first.Image.Pixels.begin(), first.Image.Pixels.end(), [](uint8_t value) {
		return value != 0;
	}));
	const auto later = client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 12);
	INFO(later.Diagnostic.Message);
	REQUIRE(later.Status == engine::imagegraph::Status::Ok);
	CHECK(later.Image.Pixels != first.Image.Pixels);
	const auto repeated = client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 12);
	REQUIRE(repeated.Status == engine::imagegraph::Status::Ok);
	CHECK(repeated.Image == later.Image);
	const auto reset = client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 0);
	REQUIRE(reset.Status == engine::imagegraph::Status::Ok);
	CHECK(reset.Image == first.Image);
	CHECK(
		client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 4097).Status ==
		engine::imagegraph::Status::LimitExceeded
	);
}

TEST_CASE(
	"Client rigid replay cache avoids provider work and keeps last good pixels on refusal",
	"[client][imagegraph][rigid]"
) {
	using namespace engine::imagegraph;
	struct CountingProvider final : SourceRigidProvider {
		engine::imagegraphphysics::RigidProvider Physics;
		size_t Calls = 0;
		Status Replay(
			const SourceRigidHistory &history,
			uint64_t tick,
			std::optional<SourceRigidEventPosition> position,
			uint64_t bytes,
			SourceRigidSnapshot &output,
			Diagnostic &diagnostic
		) override {
			++Calls;
			return Physics.Replay(history, tick, position, bytes, output, diagnostic);
		}
	} provider;
	const auto document = RigidGraph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	request.Tick = 12;
	request.RigidProvider = &provider;
	request.RigidPlaying = request.RigidFrameProgress = true;
	REQUIRE(host.Prepare(document, plan, 1, 0, request, diagnostic, Limits::MaximumEvaluationBytes, "final"));
	REQUIRE(host.Output("final"));
	const auto last = *host.Output("final");
	REQUIRE(request.RigidReplay);
	const auto journal = *request.RigidReplay;
	const auto calls = provider.Calls;
	REQUIRE(calls > 0);
	REQUIRE(host.Prepare(document, plan, 1, 0, request, diagnostic, Limits::MaximumEvaluationBytes, "final"));
	CHECK(provider.Calls == calls);
	CHECK(*request.RigidReplay == journal);
	CHECK(*host.Output("final") == last);
	request.Tick = 4097;
	CHECK_FALSE(
		host.Prepare(document, plan, 1, 0, request, diagnostic, Limits::MaximumEvaluationBytes, "final")
	);
	CHECK(diagnostic.Code == Status::LimitExceeded);
	CHECK(provider.Calls == calls);
	CHECK(*request.RigidReplay == journal);
	CHECK(*host.Output("final") == last);
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

TEST_CASE(
	"Persisted native feedback evaluates prior generations through client load adapter",
	"[client][imagegraph][feedback]"
) {
	GraphFile file;
	engine::imagegraph::Document document;
	document.Nodes = {
		{"prior", "image.captured", "", {}, {{"source_id", std::string{"feedback:final"}}}},
		{"invert", "image.invert", "", {}, {{"include_alpha", false}}}
	};
	document.Links = {{"prior", "image", "invert", "image"}};
	document.Outputs = {{"final", "invert", "image"}};
	{
		std::ofstream out(client::ImageGraphDocumentPath(file.Assets, GRAPH));
		out << engine::imagegraph::Write(document);
	}
	for (uint64_t tick : {0, 1, 3, 0}) {
		const auto frame = client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, tick);
		INFO(frame.Diagnostic.Message);
		REQUIRE(frame.Status == engine::imagegraph::Status::Ok);
		CHECK(frame.Animated);
		CHECK(frame.Image.Pixels.front() == (tick % 2 ? 0 : 255));
	}
	const auto refused = client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 4097);
	CHECK(refused.Status == engine::imagegraph::Status::LimitExceeded);
}

TEST_CASE(
	"Living feedback study retains accumulated output and repeats reset", "[client][imagegraph][feedback]"
) {
	GraphFile file;
	const auto source = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
						"mono.engine/examples/assets/imagegraphs/Composer-Feedback-And-Fluid.graph";
	REQUIRE(std::filesystem::exists(source));
	std::filesystem::copy_file(
		source,
		client::ImageGraphDocumentPath(file.Assets, GRAPH),
		std::filesystem::copy_options::overwrite_existing
	);
	const engine::core::Name output("accumulated");
	const auto initial = client::LoadImageGraphFrame(file.Assets, GRAPH, output, 0);
	INFO(initial.Diagnostic.Message);
	REQUIRE(initial.Status == engine::imagegraph::Status::Ok);
	const auto middle = client::LoadImageGraphFrame(file.Assets, GRAPH, output, 30);
	INFO(middle.Diagnostic.Message);
	REQUIRE(middle.Status == engine::imagegraph::Status::Ok);
	const auto late = client::LoadImageGraphFrame(file.Assets, GRAPH, output, 60);
	INFO(late.Diagnostic.Message);
	REQUIRE(late.Status == engine::imagegraph::Status::Ok);
	CHECK(initial.Image.Hash != middle.Image.Hash);
	CHECK(middle.Image.Hash != late.Image.Hash);
	const auto reset = client::LoadImageGraphFrame(file.Assets, GRAPH, output, 0);
	REQUIRE(reset.Status == engine::imagegraph::Status::Ok);
	CHECK(reset.Image == initial.Image);
}

TEST_CASE("saved Lua surface graphs use the native client host", "[client][imagegraph]") {
	GraphFile file;
	{
		std::ofstream output(client::ImageGraphDocumentPath(file.Assets, GRAPH));
		output << "imagegraph 1\n"
				  "node \"pixels\" \"pc.lua_surface\" \"\" 0 0\n"
				  "value 0 \"pixels\" \"output_dimension\" v 3 2\n"
				  "value 0 \"pixels\" \"lua_code\" s \"clear() setColor(colorCreateRGB(255,0,0)) "
				  "drawPixel(1,0)\"\n"
				  "output \"final\" \"pixels\" \"surface_out\"\n";
	}
	const auto frame = client::LoadImageGraphFrame(file.Assets, GRAPH, OUTPUT, 0);
	INFO(frame.Diagnostic.Message);
	REQUIRE(frame.Status == engine::imagegraph::Status::Ok);
	CHECK(frame.Animated);
	CHECK(frame.Image.Width == 3);
	CHECK(frame.Image.Height == 2);
	engine::imagegraph::SurfacePixel sample{};
	REQUIRE(engine::imagegraph::LoadSurfacePixel(frame.Image, 1, 0, sample));
	CHECK(sample[0] == 1);
	CHECK(sample[1] == 0);
	CHECK(sample[3] == 1);
}

TEST_CASE(
	"live Lua globals persist across ticks and reset with graph replacement", "[client][imagegraph][gpu][.]"
) {
	GraphFile file;
	const auto path = client::ImageGraphDocumentPath(file.Assets, GRAPH);
	{
		std::ofstream output(path);
		output << "imagegraph 1\n"
				  "node \"pixels\" \"pc.lua_surface\" \"\" 0 0\n"
				  "value 0 \"pixels\" \"output_dimension\" v 1 1\n"
				  "value 0 \"pixels\" \"lua_code\" s \"count=(count or 0)+1 clear() "
				  "setColor(colorCreateRGB(count,0,0)) drawPixel(0,0)\"\n"
				  "output \"final\" \"pixels\" \"surface_out\"\n";
	}
	engine::scene::RegisterSceneComponents();
	engine::ecs::Store store("imagegraph-lua-world");
	engine::scene::ImageGraphBinding selector;
	selector.Graph = GRAPH;
	selector.Output = OUTPUT;
	selector.Texture = TEXTURE;
	selector.ColorSpace = engine::scene::ImageGraphColorSpace::Linear;
	selector.TickPolicy = engine::scene::ImageGraphTickPolicy::World;
	REQUIRE(engine::scene::SetImageGraphBinding(store, store.Create(), selector));
	REQUIRE(SDL_Init(SDL_INIT_VIDEO));
	engine::render::Renderer renderer;
	REQUIRE(renderer.Initialise(nullptr, 1, true));
	client::ImageGraphRuntime runtime;
	auto red = [&] {
		engine::assets::TextureData texture;
		REQUIRE(
			renderer.CopyTexture(TEXTURE, texture, 1024, OWNER) == engine::render::TextureCopyStatus::Copied
		);
		REQUIRE(texture.Pixels.size() == 4);
		return std::to_integer<unsigned>(texture.Pixels[0]);
	};
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	CHECK(red() == 1);
	store.AdvanceTick(1.0f / 60.0f);
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	CHECK(red() == 2);
	std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(1));
	runtime.BeginFrame();
	REQUIRE(runtime.Refresh(store, renderer, OWNER, file.Assets) == 1);
	CHECK(red() == 1);
	runtime.Clear(renderer);
	renderer.Shutdown();
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

TEST_CASE(
	"material map colour space conflicts reject before graph evaluation",
	"[client][imagegraph][sink-admission]"
) {
	engine::scene::RegisterSceneComponents();
	using Appearance = engine::scene::SurfaceAppearance;
	const std::array<engine::core::Name Appearance::*, 8> maps{
		&Appearance::ColourMap,
		&Appearance::EmissiveMap,
		&Appearance::NormalMap,
		&Appearance::RoughnessMap,
		&Appearance::OcclusionMap,
		&Appearance::HeightMap,
		&Appearance::MetalnessMap,
		&Appearance::PackedPbrMap
	};
	for (size_t index = 0; index < maps.size(); ++index) {
		CAPTURE(index);
		engine::ecs::Store store("headless-material-sink");
		const auto part = store.Create();
		Appearance appearance;
		appearance.*maps[index] = TEXTURE;
		store.Set(part, appearance);
		const auto binding = store.Create();
		engine::scene::ImageGraphBinding selector;
		selector.Graph = GRAPH;
		selector.Output = OUTPUT;
		selector.Texture = TEXTURE;
		selector.ColorSpace = index < 2 ? engine::scene::ImageGraphColorSpace::Linear
										: engine::scene::ImageGraphColorSpace::Display;
		REQUIRE(engine::scene::SetImageGraphBinding(store, binding, selector));
		engine::render::Renderer renderer;
		client::ImageGraphRuntime runtime;
		CHECK(runtime.Refresh(store, renderer, OWNER, "absent-assets") == 0);
		CHECK(runtime.LastError().find("colour space") != std::string::npos);
		CHECK(runtime.DocumentParses() == 0);
		// A selector cannot satisfy a display map and a numeric map sharing its name.
		appearance.ColourMap = TEXTURE;
		appearance.NormalMap = TEXTURE;
		store.Set(part, appearance);
		selector.ColorSpace = engine::scene::ImageGraphColorSpace::Display;
		REQUIRE(engine::scene::SetImageGraphBinding(store, binding, selector));
		runtime.BeginFrame();
		CHECK(runtime.Refresh(store, renderer, OWNER, "absent-assets") == 0);
		CHECK(runtime.LastError().find("colour space") != std::string::npos);
		CHECK(runtime.DocumentParses() == 0);
	}
}

TEST_CASE(
	"incomplete skybox groups reject atomically before graph evaluation",
	"[client][imagegraph][sink-admission]"
) {
	engine::scene::RegisterSceneComponents();
	engine::scene::RegisterSceneClasses();
	for (int failure = 0; failure != 3; ++failure) {
		CAPTURE(failure);
		engine::ecs::Store store("headless-skybox-sink");
		engine::scene::InstallServices(store);
		const auto sky = store.CreateInstance(
			engine::ecs::Classes::Find(engine::core::Name("SkyboxTextures")), "GraphSky"
		);
		REQUIRE(sky != engine::ecs::NULL_ENTITY);
		REQUIRE(store.SetParent(sky, store.FindFirstRoot("Lighting")));
		const std::array<engine::core::Name, 6> names{
			engine::core::Name("front"),
			engine::core::Name("back"),
			engine::core::Name("left"),
			engine::core::Name("right"),
			engine::core::Name("up"),
			engine::core::Name("down")
		};
		auto *textures = store.GetMutable<engine::scene::SkyboxTextures>(sky);
		REQUIRE(textures);
		textures->Front = names[0];
		textures->Back = names[1];
		textures->Left = names[2];
		textures->Right = names[3];
		textures->Up = names[4];
		textures->Down = names[5];
		for (size_t i = 0; i < names.size(); ++i) {
			if (failure == 0 && i == 5) continue;
			engine::scene::ImageGraphBinding selector;
			selector.Graph = GRAPH;
			selector.Output = OUTPUT;
			selector.Texture = names[i];
			if (failure == 1 && i == 5) selector.ColorSpace = engine::scene::ImageGraphColorSpace::Linear;
			REQUIRE(engine::scene::SetImageGraphBinding(store, store.Create(), selector));
			if (failure == 2 && i == 5)
				REQUIRE(engine::scene::SetImageGraphBinding(store, store.Create(), selector));
		}
		engine::render::Renderer renderer;
		client::ImageGraphRuntime runtime;
		CHECK(runtime.Refresh(store, renderer, OWNER, "absent-assets") == 0);
		CHECK(runtime.LastError().find("six distinct valid display") != std::string::npos);
		CHECK(runtime.DocumentParses() == 0);
	}
}
