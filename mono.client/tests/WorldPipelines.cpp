// The join from a world's saved render documents to the renderer's compiled
// runtime cache.

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
using engine::graph::PipelineSet;
using engine::render::Renderer;

TEST_CASE("universe profiles are qualified, selected, and replaced", "[client][pipeline]") {
	PipelineSet first;
	REQUIRE(first.Set(Name("main"), engine::graph::DefaultPbrDocument()));
	REQUIRE(first.Set(Name("reflection"), engine::graph::DefaultPbrDocument()));

	Renderer renderer;
	CHECK(client::InstallRenderingProfiles(first, renderer, 17, Name("reflection")) == Name("reflection#17"));
	CHECK(renderer.Pipelines() == std::vector<Name>{Name("reflection#17")});

	PipelineSet replacement;
	REQUIRE(replacement.Set(Name("cinematic"), engine::graph::DefaultPbrDocument()));

	CHECK(
		client::InstallRenderingProfiles(replacement, renderer, 17, Name("cinematic")) == Name("cinematic#17")
	);
	CHECK(renderer.Pipelines() == std::vector<Name>{Name("cinematic#17")});
}

TEST_CASE("worlds with the same authored name keep separate runtime keys", "[client][pipeline]") {
	PipelineSet pipelines;
	REQUIRE(pipelines.Set(Name("main"), engine::graph::DefaultPbrDocument()));

	Renderer renderer;
	CHECK(client::InstallRenderingProfiles(pipelines, renderer, 4, Name("main")) == Name("main#4"));
	CHECK(client::InstallRenderingProfiles(pipelines, renderer, 9, Name("main")) == Name("main#9"));
	CHECK(renderer.Pipelines() == std::vector<Name>{Name("main#4"), Name("main#9")});
}

TEST_CASE("an empty profile library installs the engine default graph", "[client][pipeline]") {
	PipelineSet profiles;
	Renderer renderer;

	CHECK(
		client::InstallRenderingProfiles(profiles, renderer, 3, Name("Default PBR")) == Name("Default PBR#3")
	);
	CHECK(renderer.Pipelines() == std::vector<Name>{Name("Default PBR#3")});
}

TEST_CASE("a missing selection falls back to Default PBR", "[client][pipeline]") {
	PipelineSet profiles;
	REQUIRE(profiles.Set(Name("Cinematic"), engine::graph::DefaultPbrDocument()));
	REQUIRE(profiles.Set(Name("Default PBR"), engine::graph::DefaultPbrDocument()));
	Renderer renderer;

	CHECK(
		client::InstallRenderingProfiles(profiles, renderer, 12, Name("Missing")) == Name("Default PBR#12")
	);
}
