// The checked-in graph recipe and Luau scene remain one runnable example.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/LevelOfDetail.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/RenderFeatures.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
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

	const engine::graph::Node *FindNode(const engine::graph::RenderGraph &graph, Name name) {
		for (uint32_t value = 1; value <= graph.Count(); ++value) {
			const auto *node = graph.Find(engine::graph::NodeId{value});
			if (node != nullptr && node->Name == name) {
				return node;
			}
		}
		return nullptr;
	}

	const engine::graph::ResourceDesc *FindResource(const engine::graph::RenderGraph &graph, Name name) {
		for (uint32_t value = 1; value <= graph.ResourceCount(); ++value) {
			const auto *resource = graph.FindResource(engine::graph::ResourceId{value});
			if (resource != nullptr && resource->Name == name) return resource;
		}
		return nullptr;
	}

	bool HasPort(
		const engine::graph::RenderGraph &graph,
		const engine::graph::Node &node,
		Name resource,
		Name port,
		bool read
	) {
		const auto &resources = read ? node.Reads : node.Writes;
		const auto &ports = read ? node.ReadPorts : node.WritePorts;
		for (size_t index = 0; index < resources.size(); ++index) {
			const auto *description = graph.FindResource(resources[index]);
			if (description != nullptr && description->Name == resource && index < ports.size() &&
				ports[index] == port)
				return true;
		}
		return false;
	}

	engine::graph::PipelineDocument LoadPipeline(std::string_view name) {
		const std::filesystem::path path =
			engine::core::Paths::Assets() / "examples" / "pipelines" / std::string(name);
		std::ifstream input(path, std::ios::binary);
		REQUIRE(input);
		const std::string text(std::istreambuf_iterator<char>(input), {});
		engine::graph::PipelineDocument document;
		Name offender;
		REQUIRE(engine::graph::Read(text, document, offender) == engine::graph::PipelineDocumentStatus::Ok);
		return document;
	}
}

TEST_CASE("the render features pipeline runs its authored attachment and output chain", "[examples][graph]") {
	const StagedAssets assets;
	engine::graph::RegisterRenderNodeKinds();
	const auto document = LoadPipeline("RenderFeatures.pipeline");
	engine::graph::RenderGraph graph;
	Name offender;
	REQUIRE(engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
	engine::graph::CompiledGraph compiled;
	REQUIRE(graph.Compile(compiled, offender) == engine::graph::GraphStatus::Ok);

	const auto *compute = FindNode(graph, Name("demo-compute"));
	const auto *post = FindNode(graph, Name("demo-post"));
	const auto *fxaa = FindNode(graph, Name("demo-fxaa"));
	const auto *output = FindNode(graph, Name("demo-output"));
	REQUIRE(compute != nullptr);
	REQUIRE(post != nullptr);
	REQUIRE(fxaa != nullptr);
	REQUIRE(output != nullptr);
	CHECK(compute->Kind == Name("dispatch"));
	CHECK(HasPort(graph, *compute, Name("tonemapped"), Name("source"), true));
	CHECK(HasPort(graph, *compute, Name("demo-computed"), Name("target"), false));
	CHECK(HasPort(graph, *post, Name("demo-computed"), Name("source"), true));
	CHECK(HasPort(graph, *fxaa, Name("demo-post"), Name("source"), true));
	CHECK(HasPort(graph, *output, Name("demo-composed"), Name("image"), true));
	CHECK(output->Scope == engine::graph::NodeScope::Frame);
	const auto *defaultOutput = FindNode(graph, Name("output-image"));
	REQUIRE(defaultOutput != nullptr);
	CHECK_FALSE(defaultOutput->Enabled);
}

TEST_CASE(
	"the tracing pipeline assets keep geometry lighting and history wired", "[examples][graph][tracing]"
) {
	const StagedAssets assets;
	engine::graph::RegisterRenderNodeKinds();
	for (const auto &[asset, traceName, kind] : {
			 std::tuple{"RaytraceDemo.pipeline", "screen-raytrace", "raytrace"},
			 std::tuple{"PathtraceDemo.pipeline", "progressive-pathtrace", "pathtrace"},
		 }) {
		const auto document = LoadPipeline(asset);
		engine::graph::RenderGraph graph;
		Name offender;
		REQUIRE(engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
		engine::graph::CompiledGraph compiled;
		REQUIRE(graph.Compile(compiled, offender) == engine::graph::GraphStatus::Ok);

		const auto *tessellation = FindNode(graph, Name("adaptive-tessellation"));
		const auto *geometry = FindNode(graph, Name("tessellated-geometry"));
		const auto *illumination = FindNode(graph, Name("indirect-light"));
		const auto *trace = FindNode(graph, Name(traceName));
		REQUIRE(tessellation != nullptr);
		REQUIRE(geometry != nullptr);
		REQUIRE(illumination != nullptr);
		REQUIRE(trace != nullptr);
		CHECK(tessellation->Kind == Name("tessellate"));
		CHECK(geometry->Kind == Name("tessellated-draw"));
		CHECK(geometry->Reads.size() == 3);
		CHECK(illumination->Kind == Name("global-illumination"));
		CHECK(HasPort(graph, *illumination, Name("tessellated-depth"), Name("depth"), true));
		CHECK(trace->Kind == Name(kind));
		CHECK(HasPort(graph, *trace, Name("indirect"), Name("indirect"), true));
		if (trace->Kind == Name("pathtrace")) {
			CHECK(HasPort(graph, *trace, Name("linear-depth"), Name("depth"), true));
			CHECK(HasPort(graph, *trace, Name("visible-entities"), Name("entities"), true));
			CHECK(HasPort(graph, *trace, Name("coarse-instances"), Name("instances"), true));
			CHECK(HasPort(graph, *trace, Name("path-radiance"), Name("radiance"), false));
			const auto *history = FindResource(graph, Name("path-history"));
			REQUIRE(history != nullptr);
			CHECK(history->Lifetime == engine::graph::ResourceLifetime::History);
			CHECK(HasPort(graph, *trace, Name("path-history"), Name("history"), true));
			const auto *store = FindNode(graph, Name("path-history-store"));
			REQUIRE(store != nullptr);
			CHECK(HasPort(graph, *store, Name("path-radiance"), Name("source"), true));
			CHECK(HasPort(graph, *store, Name("path-history"), Name("target"), false));
		} else {
			CHECK(HasPort(graph, *trace, Name("tessellated-depth"), Name("depth"), true));
			CHECK(HasPort(graph, *trace, Name("tessellated-colour"), Name("scene"), true));
			CHECK(HasPort(graph, *trace, Name("reflections"), Name("reflection"), false));
		}
		engine::graph::PipelineDocument reloaded;
		REQUIRE(
			engine::graph::Read(engine::graph::Write(document), reloaded, offender) ==
			engine::graph::PipelineDocumentStatus::Ok
		);
		CHECK(engine::graph::Write(reloaded) == engine::graph::Write(document));
	}
}

TEST_CASE("the compositor pipeline asset preserves the image pass order", "[examples][graph][compositor]") {
	const StagedAssets assets;
	engine::graph::RegisterRenderNodeKinds();
	const auto document = LoadPipeline("CompositorDemo.pipeline");
	engine::graph::RenderGraph graph;
	Name offender;
	REQUIRE(engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
	engine::graph::CompiledGraph compiled;
	REQUIRE(graph.Compile(compiled, offender) == engine::graph::GraphStatus::Ok);
	const auto position = [&](const char *name) {
		for (size_t index = 0; index < compiled.PerView.size(); ++index) {
			if (graph.Find(compiled.PerView[index])->Name == Name(name)) return index;
		}
		return compiled.PerView.size();
	};
	CHECK(position("grade-exposure") < position("grade-hsv"));
	CHECK(position("grade-hsv") < position("mix-original"));
	CHECK(position("mix-original") < position("frame-transform"));
	CHECK(position("frame-transform") < position("blur-x"));
	CHECK(position("blur-x") < position("blur-y"));
	CHECK(position("blur-y") < position("compositor-tonemap"));
	const auto *exposure = FindNode(graph, Name("grade-exposure"));
	const auto *blur = FindNode(graph, Name("blur-y"));
	REQUIRE(exposure != nullptr);
	REQUIRE(blur != nullptr);
	CHECK(exposure->Kind == Name("exposure-grade"));
	CHECK(HasPort(graph, *exposure, Name("lens-b"), Name("source"), true));
	CHECK(blur->Kind == Name("blur"));
	CHECK(HasPort(graph, *blur, Name("blur-horizontal"), Name("source"), true));
	REQUIRE_FALSE(compiled.Final.empty());
	CHECK(graph.Find(compiled.Final.back())->Name == Name("compositor-output-image"));
}

TEST_CASE("the render features scene authors policies attachments and LOD fallback", "[examples][scene]") {
	const StagedAssets assets;
	Store store("render_features_demo");
	engine::ecs::Scheduler systems;
	engine::script::RuntimeLimits limits;
	limits.Role = engine::script::HostRole::OfBoth();
	std::string error;
	INFO(error);
	REQUIRE(
		engine::examples::LoadScene(
			store, systems, engine::examples::ExamplePath("RenderFeaturesDemo.luau"), error, nullptr, &limits
		)
	);

	const Entity part = Child(store, "RenderFeatureLodMesh");
	REQUIRE(part != engine::ecs::NULL_ENTITY);
	const auto *automatic = store.Get<engine::scene::LODAuto>(part);
	const auto *custom = store.Get<engine::scene::LODCustom>(part);
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
