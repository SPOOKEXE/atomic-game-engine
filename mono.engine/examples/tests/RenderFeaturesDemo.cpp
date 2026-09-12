// The checked-in graph recipe and Luau scene remain one runnable example.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/examples/RenderFeaturesDemo.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/LevelOfDetail.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/RenderFeatures.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>

TEST_SUITE_ID("engine.examples.renderfeaturesdemo")
TEST_DEPENDS("engine.graph.pipelinedocument")
TEST_DEPENDS("engine.scripthost.scripting")

namespace {
	using engine::core::Name;
	using engine::ecs::Entity;
	using engine::ecs::Store;

	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();

		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
		}

		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};

	Entity Child(Store &store, const char *name) {
		return store.FindFirstChild(engine::scene::WorkspaceOf(store), name);
	}

	bool HasNode(const engine::graph::RenderGraph &graph, Name name) {
		for (uint32_t value = 1; value <= graph.Count(); ++value) {
			const auto *node = graph.Find(engine::graph::NodeId{value});
			if (node != nullptr && node->Name == name) {
				return true;
			}
		}
		return false;
	}
}

TEST_CASE("the render features pipeline asset matches and builds its recipe", "[examples][graph]") {
	const StagedAssets assets;
	engine::graph::RegisterRenderNodeKinds();
	const engine::graph::PipelineDocument expected = engine::examples::RenderFeaturesDemoPipeline();

	const std::filesystem::path path =
		engine::core::Paths::Assets() / "examples" / "pipelines" / "RenderFeatures.pipeline";
	std::ifstream input(path, std::ios::binary);
	REQUIRE(input);
	const std::string text(std::istreambuf_iterator<char>(input), {});

	engine::graph::PipelineDocument loaded;
	Name offender;
	REQUIRE(engine::graph::Read(text, loaded, offender) == engine::graph::PipelineDocumentStatus::Ok);
	CHECK(engine::graph::Write(loaded) == engine::graph::Write(expected));

	engine::graph::RenderGraph graph;
	REQUIRE(engine::graph::Build(loaded, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
	CHECK(HasNode(graph, Name("demo-compute")));
	CHECK(HasNode(graph, Name("demo-post")));
	CHECK(HasNode(graph, Name("demo-fxaa")));
}

TEST_CASE("the tracing pipeline assets match their graph recipes", "[examples][graph][tracing]") {
	const StagedAssets assets;
	engine::graph::RegisterRenderNodeKinds();
	for (const auto &[name, expected, trace] : {
			 std::tuple{
				 "RaytraceDemo.pipeline", engine::graph::RaytraceDemoDocument(), Name("screen-raytrace")
			 },
			 std::tuple{
				 "PathtraceDemo.pipeline",
				 engine::graph::PathtraceDemoDocument(),
				 Name("progressive-pathtrace")
			 },
		 }) {
		const std::filesystem::path path = engine::core::Paths::Assets() / "examples" / "pipelines" / name;
		std::ifstream input(path, std::ios::binary);
		REQUIRE(input);
		const std::string text(std::istreambuf_iterator<char>(input), {});

		engine::graph::PipelineDocument loaded;
		Name offender;
		REQUIRE(engine::graph::Read(text, loaded, offender) == engine::graph::PipelineDocumentStatus::Ok);
		CHECK(engine::graph::Write(loaded) == engine::graph::Write(expected));

		engine::graph::RenderGraph graph;
		REQUIRE(engine::graph::Build(loaded, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
		CHECK(HasNode(graph, trace));
		CHECK(HasNode(graph, Name("adaptive-tessellation")));
		CHECK(HasNode(graph, Name("indirect-light")));
	}
}

TEST_CASE("the compositor pipeline asset matches its graph recipe", "[examples][graph][compositor]") {
	const StagedAssets assets;
	engine::graph::RegisterRenderNodeKinds();
	const std::filesystem::path path =
		engine::core::Paths::Assets() / "examples" / "pipelines" / "CompositorDemo.pipeline";
	std::ifstream input(path, std::ios::binary);
	REQUIRE(input);
	const std::string text(std::istreambuf_iterator<char>(input), {});

	engine::graph::PipelineDocument loaded;
	Name offender;
	REQUIRE(engine::graph::Read(text, loaded, offender) == engine::graph::PipelineDocumentStatus::Ok);
	CHECK(engine::graph::Write(loaded) == engine::graph::Write(engine::graph::CompositorDemoDocument()));

	engine::graph::RenderGraph graph;
	REQUIRE(engine::graph::Build(loaded, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
	for (const char *name :
		 {"grade-exposure", "grade-hsv", "mix-original", "frame-transform", "blur-x", "blur-y"}) {
		CHECK(HasNode(graph, Name(name)));
	}
}

TEST_CASE("the render features scene authors policies attachments and LOD fallback", "[examples][scene]") {
	const StagedAssets assets;
	Store store("render_features_demo");
	engine::ecs::Scheduler systems;
	std::string error;
	INFO(error);
	REQUIRE(
		engine::examples::LoadScene(
			store, systems, engine::examples::ExamplePath("RenderFeaturesDemo.luau"), error
		)
	);

	const Entity part = Child(store, "RenderFeatureLodMesh");
	REQUIRE(part != engine::ecs::NULL_ENTITY);
	const auto *automatic = store.Get<engine::scene::AutoMeshLOD>(part);
	const auto *custom = store.Get<engine::scene::CustomMeshLOD>(part);
	const auto *effects = store.Get<engine::scene::RenderEffects>(part);
	const auto *visual = store.Get<engine::scene::Visual>(part);
	REQUIRE(automatic != nullptr);
	REQUIRE(custom != nullptr);
	REQUIRE(effects != nullptr);
	REQUIRE(visual != nullptr);

	const engine::scene::LevelOfDetail resolved = engine::scene::ResolveMeshLOD(automatic, custom);
	CHECK(resolved.Meshes[0] == Name("engine.Plane"));
	CHECK(resolved.Meshes[1] == Name("engine.Cylinder"));
	CHECK(resolved.Meshes[2] == Name("engine.CornerWedge"));
	CHECK(effects->Attachments[0].Node == Name("demo-compute"));
	CHECK(effects->Attachments[1].Node == Name("demo-post"));
	CHECK(
		(visual->RenderFeatures.Enable &
		 engine::scene::FeatureBit(engine::scene::RenderFeature::ComputeEffects)) != 0
	);

	const auto *active = store.Resource<engine::scene::ActiveCamera>();
	REQUIRE(active != nullptr);
	const auto *camera = store.Get<engine::scene::Camera>(active->Entity);
	REQUIRE(camera != nullptr);
	const engine::scene::WorldLighting lighting = engine::scene::LightingOf(store);
	CHECK(
		(lighting.RenderFeatures.Enable & engine::scene::FeatureBit(engine::scene::RenderFeature::Shadows)) !=
		0
	);
	CHECK(
		(camera->RenderFeatures.Disable &
		 engine::scene::FeatureBit(engine::scene::RenderFeature::Reflections)) != 0
	);
}
