// The join from a world's saved render documents to the renderer's compiled
// runtime cache.

#include <engine/ecs/Store.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/Scene.hpp>
#include <string>
#include <vector>

TEST_SUITE_ID("client.scene.worldpipelines")
TEST_DEPENDS("engine.graph.pipelinedocument")
TEST_DEPENDS("engine.render.passes")

using engine::core::Name;
using engine::ecs::Store;
using engine::graph::PipelineSet;
using engine::render::Renderer;

TEST_CASE("a world's pipelines are qualified, selected, and replaced", "[client][pipeline]") {
	engine::graph::RegisterPipelineComponents();

	Store store("client.worldpipelines");
	PipelineSet first;
	REQUIRE(first.Set(Name("main"), engine::graph::DefaultPbrDocument()));
	REQUIRE(first.Set(Name("reflection"), engine::graph::DefaultPbrDocument()));
	store.SetResource(first);

	Renderer renderer;
	CHECK(client::InstallWorldPipelines(store, renderer, 17) == Name("main#17"));
	CHECK(renderer.Pipelines() == std::vector<Name>{Name("main#17"), Name("reflection#17")});

	PipelineSet replacement;
	REQUIRE(replacement.Set(Name("cinematic"), engine::graph::DefaultPbrDocument()));
	store.SetResource(replacement);

	CHECK(client::InstallWorldPipelines(store, renderer, 17) == Name("cinematic#17"));
	CHECK(renderer.Pipelines() == std::vector<Name>{Name("cinematic#17")});
}

TEST_CASE("worlds with the same authored name keep separate runtime keys", "[client][pipeline]") {
	engine::graph::RegisterPipelineComponents();

	Store first("client.worldpipelines.first");
	Store second("client.worldpipelines.second");
	PipelineSet pipelines;
	REQUIRE(pipelines.Set(Name("main"), engine::graph::DefaultPbrDocument()));
	first.SetResource(pipelines);
	second.SetResource(pipelines);

	Renderer renderer;
	CHECK(client::InstallWorldPipelines(first, renderer, 4) == Name("main#4"));
	CHECK(client::InstallWorldPipelines(second, renderer, 9) == Name("main#9"));
	CHECK(renderer.Pipelines() == std::vector<Name>{Name("main#4"), Name("main#9")});
}

TEST_CASE("a world with no pipelines installs the engine default graph", "[client][pipeline]") {
	Store store("client.worldpipelines.empty");
	Renderer renderer;

	CHECK(client::InstallWorldPipelines(store, renderer, 3) == Name("Default PBR#3"));
	CHECK(renderer.Pipelines() == std::vector<Name>{Name("Default PBR#3")});
	const PipelineSet *saved = store.Resource<PipelineSet>();
	REQUIRE(saved != nullptr);
	CHECK(saved->Find(Name("Default PBR")) != nullptr);
}
