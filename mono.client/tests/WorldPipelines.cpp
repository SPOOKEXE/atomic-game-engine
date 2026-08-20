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

namespace {
	// The default document with a `raytrace` node inserted after the linear
	// depth it reads, wired the way the catalogue entry asks and writing a
	// storage image. This is what the Studio's graph editor produces when
	// somebody drops the catalogue's "Ray trace" box into a working PBR
	// pipeline - a per-view pass among the other per-view passes, so what the
	// renderer refuses is the *kind* rather than the placement.
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

TEST_CASE("a raytrace node is refused by the backend and the profile falls back", "[client][pipeline]") {
	// **The catalogue sells a pass the backend does not run.** `raytrace` is a
	// registered `NodeKindSpec`, so the Studio's editor offers it and a saved
	// document holding one round-trips - but `BackendNodes()` in the renderer
	// has no executor for the kind, so the graph must be refused whole rather
	// than drawn with a silently missing pass. This pins where portal scenes
	// stand under raytracing today: there is no executable raytraced lighting
	// path for them to disagree with, and a world that saved one draws through
	// the default raster graph, portal lighting included.
	const engine::graph::PipelineDocument document = RaytracedDocument();

	// The document itself is legal: it replays into a graph. The refusal has
	// to come from the renderer, or the editor could never author the kind the
	// roadmap's raytrace port will eventually implement.
	engine::graph::RenderGraph graph;
	Name offender;
	REQUIRE(engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);

	Renderer renderer;
	CHECK_FALSE(renderer.SetPipeline(Name("raytraced"), graph));
	CHECK(renderer.Pipelines().empty());

	// A world whose saved profile traces still gets a frame: the install walks
	// past the refused candidate to the stock document.
	PipelineSet profiles;
	REQUIRE(profiles.Set(Name("Raytraced"), document));
	REQUIRE(profiles.Set(Name("Default PBR"), engine::graph::DefaultPbrDocument()));

	CHECK(
		client::InstallRenderingProfiles(profiles, renderer, 7, Name("Raytraced")) == Name("Default PBR#7")
	);
	CHECK(renderer.Pipelines() == std::vector<Name>{Name("Default PBR#7")});
}
