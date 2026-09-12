// The join from a world's saved render documents to the renderer's compiled
// runtime cache.

#include <engine/core/Paths.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Capabilities.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <client/Client.hpp>
#include <client/Scene.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

TEST_SUITE_ID("client.scene.worldpipelines")
TEST_DEPENDS("engine.graph.pipelinedocument")
TEST_DEPENDS("engine.render.passes")

using engine::core::Name;
using engine::graph::PipelineSet;
using engine::render::Renderer;

namespace {
	struct TemporaryPipelineFile {
		std::filesystem::path Path;

		explicit TemporaryPipelineFile(std::string_view text) {
			Path =
				std::filesystem::temp_directory_path() /
				("atomic-render-pipeline-" +
				 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".pipeline");
			std::ofstream output(Path, std::ios::binary);
			output << text;
		}

		~TemporaryPipelineFile() {
			std::error_code error;
			std::filesystem::remove(Path, error);
		}
	};
}

TEST_CASE("a command-line pipeline selects a parsed demo profile", "[client][pipeline]") {
	const std::filesystem::path path =
		engine::core::Paths::Base().parent_path() / "assets/examples/pipelines/RenderFeatures.pipeline";
	PipelineSet profiles;
	Name selected;
	std::string error;

	REQUIRE(client::LoadRenderPipelineFile(path, profiles, selected, error));
	CHECK(error.empty());
	CHECK(selected == Name(client::COMMAND_LINE_RENDER_PIPELINE));
	REQUIRE(profiles.Find(selected) != nullptr);

	Renderer renderer;
	CHECK(
		client::InstallRenderingProfiles(profiles, renderer, 2, selected) ==
		Name("Command Line Render Pipeline#2")
	);
}

TEST_CASE("a malformed command-line pipeline refuses before client startup", "[client][pipeline]") {
	const TemporaryPipelineFile malformed("renderpipeline 3\nnode broken\n");
	PipelineSet profiles;
	Name selected("previous");
	std::string error;

	CHECK_FALSE(client::LoadRenderPipelineFile(malformed.Path, profiles, selected, error));
	CHECK(error.starts_with("not a render pipeline document"));
	CHECK(profiles.Count() == 0);
	CHECK(selected == Name("previous"));

	client::Options options;
	options.RenderPipelineFile = malformed.Path;
	client::Client client;
	CHECK_FALSE(client.Initialise(options));
}

TEST_CASE("a game keeps its embedded rendering profiles", "[client][pipeline]") {
	client::Options options;
	options.GameFile = "game.agame";
	options.RenderPipelineFile = "demo.pipeline";
	client::Client client;

	CHECK_FALSE(client.Initialise(options));
}

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

TEST_CASE("capability tiers fall through with reasons and runnable documents", "[client][pipeline]") {
	engine::render::DeviceCaps caps;
	caps.HasIndirectDraws = true;
	caps.Formats = {
		engine::graph::ResourceFormat::RGBA8,
		engine::graph::ResourceFormat::RGBA8_SRGB,
		engine::graph::ResourceFormat::RGB10A2,
		engine::graph::ResourceFormat::RGBA16F,
		engine::graph::ResourceFormat::R32F,
		engine::graph::ResourceFormat::D24S8,
		engine::graph::ResourceFormat::D32F,
	};
	const engine::render::PipelineTierDecision decision = engine::render::ChooseDefaultPipeline(caps);
	CHECK(decision.Tier == engine::render::DefaultPipelineTier::B);
	REQUIRE(decision.Fallthrough.size() == 1);
	CHECK(decision.Fallthrough.front().Cause.Status == engine::render::CapabilityStatus::MissingCompute);

	for (const auto &[document, expected, omitted] : {
			 std::tuple{engine::graph::DefaultPbrTierBDocument(), Name("deferred-lighting"), Name("ssao")},
			 std::tuple{engine::graph::DefaultForwardTierCDocument(), Name("forward"), Name("gbuffer")},
		 }) {
		engine::graph::RenderGraph graph;
		Name offender;
		REQUIRE(engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
		bool foundExpected = false;
		bool foundOmitted = false;
		for (uint32_t value = 1; value <= graph.Count(); value++) {
			const engine::graph::Node *node = graph.Find(engine::graph::NodeId{value});
			foundExpected = foundExpected || (node != nullptr && node->Kind == expected);
			foundOmitted = foundOmitted || (node != nullptr && node->Kind == omitted);
		}
		CHECK(foundExpected);
		CHECK_FALSE(foundOmitted);
	}
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

namespace {
	// The default document with a `raytrace` node inserted after the linear
	// depth it reads. Its storage target matches the compute node contract, so
	// this checks profile installation can admit the built-in screen-space pass.
	engine::graph::PipelineDocument RaytracedDocument() {
		using engine::graph::Edit;
		using engine::graph::EditKind;

		const engine::graph::PipelineDocument stock = engine::graph::DefaultPbrDocument();

		engine::graph::PipelineDocument document;
		const auto record = [&document](Edit edit) { document.Record(std::move(edit)); };

		// Replayed rather than appended, because a `Reads` edit binds to the
		// node above it and the document ends with the frame-scoped tail - a
		// raytrace node recorded after `present` fails scheduling before the
		// backend ever sees its kind, which is the wrong refusal to pin.
		bool inserted = false;
		for (const Edit &edit : stock.Edits()) {
			record(edit);

			const bool linearDepthWritten =
				edit.Kind == EditKind::Writes && edit.Target == Name("linear-depth");
			if (!inserted && linearDepthWritten) {
				inserted = true;

				Edit traced;
				traced.Kind = EditKind::AddResource;
				traced.Name = Name("traced");
				traced.Resource = engine::graph::ResourceKind::Storage;
				traced.Format = engine::graph::ResourceFormat::RGBA16F;
				record(std::move(traced));

				Edit node;
				node.Kind = EditKind::AddNode;
				node.Name = Name("raytrace");
				node.NodeKind = Name("raytrace");
				node.Scope = engine::graph::NodeScope::View;
				record(std::move(node));

				const auto touches = [&record](EditKind kind, const char *target, const char *port) {
					Edit edit;
					edit.Kind = kind;
					edit.Target = Name(target);
					edit.Key = Name(port);
					record(std::move(edit));
				};
				touches(EditKind::Reads, "linear-depth", "depth");
				touches(EditKind::Reads, "normal", "normal");
				touches(EditKind::Reads, "material", "material");
				touches(EditKind::Writes, "traced", "reflection");
			}
		}
		REQUIRE(inserted);

		return document;
	}
}

TEST_CASE("a raytrace node installs through the screen-space compute backend", "[client][pipeline]") {
	const engine::graph::PipelineDocument document = RaytracedDocument();

	// The document is legal and its kind has a compute handler. Device-free
	// installation proves saved profiles no longer fall back because the handler
	// table lacks the node.
	engine::graph::RenderGraph graph;
	Name offender;
	REQUIRE(engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);

	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("raytraced"), graph));
	CHECK(renderer.Pipelines() == std::vector<Name>{Name("raytraced")});

	// A selected saved profile keeps its authored screen-space tracing pass.
	PipelineSet profiles;
	REQUIRE(profiles.Set(Name("Raytraced"), document));
	REQUIRE(profiles.Set(Name("Default PBR"), engine::graph::DefaultPbrDocument()));

	CHECK(client::InstallRenderingProfiles(profiles, renderer, 7, Name("Raytraced")) == Name("Raytraced#7"));
	CHECK(renderer.Pipelines() == std::vector<Name>{Name("Raytraced#7"), Name("raytraced")});
}
