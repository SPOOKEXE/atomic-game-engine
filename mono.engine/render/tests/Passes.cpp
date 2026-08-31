// Device-free checks for the renderer's graph installation boundary.
//
// The renderer no longer has a parallel enum or fixed pass list. A pipeline is
// accepted only when every enabled node has a backend implementation. The
// default output path belongs to the engine's default graph, not this boundary.

#include "EnvironmentModes.hpp"

#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.render.passes")

// Graph implementation changes must re-run the backend acceptance checks.
TEST_DEPENDS("engine.graph.rendergraph")

using engine::core::Name;
using engine::graph::RenderGraph;
using engine::render::FrameResult;
using engine::render::Renderer;

namespace {
	RenderGraph DefaultGraph() {
		RenderGraph graph;
		Name offender;
		REQUIRE(
			engine::graph::Build(engine::graph::DefaultPbrDocument(), graph, offender) ==
			engine::graph::PipelineDocumentStatus::Ok
		);
		return graph;
	}

	engine::graph::PipelineDocument
	WithSetting(const engine::graph::PipelineDocument &source, Name node, Name key, std::string value) {
		engine::graph::PipelineDocument configured;
		bool found = false;
		for (const engine::graph::Edit &edit : source.Edits()) {
			configured.Record(edit);
			if (edit.Kind != engine::graph::EditKind::AddNode || edit.Name != node) {
				continue;
			}
			engine::graph::Edit setting;
			setting.Kind = engine::graph::EditKind::Set;
			setting.Key = key;
			setting.Value = std::move(value);
			configured.Record(std::move(setting));
			found = true;
		}
		REQUIRE(found);
		return configured;
	}
}

TEST_CASE("named render graphs compile before entering the runtime cache", "[render][graph]") {
	Renderer renderer;
	const Name first("main#7");
	const Name second("main#2");

	REQUIRE(renderer.SetPipeline(first, DefaultGraph()));
	REQUIRE(renderer.SetPipeline(second, DefaultGraph()));
	CHECK(renderer.Pipelines() == std::vector<Name>{second, first});

	RenderGraph unsupported = DefaultGraph();
	const engine::graph::ResourceId storage = unsupported.AddResource(
		{.Name = Name("custom-storage"), .Kind = engine::graph::ResourceKind::Storage}
	);
	engine::graph::Node node;
	node.Name = Name("custom-compute");
	node.Kind = Name("unsupported-test-node");
	node.Writes = {storage};
	node.Scope = engine::graph::NodeScope::Frame;
	REQUIRE(unsupported.AddNode(node).IsValid());

	// A refusal keeps the complete graph already installed under this key.
	CHECK_FALSE(renderer.SetPipeline(first, unsupported));
	CHECK(renderer.Pipelines() == std::vector<Name>{second, first});

	CHECK(renderer.RemovePipeline(first));
	CHECK_FALSE(renderer.RemovePipeline(first));
	CHECK(renderer.Pipelines() == std::vector<Name>{second});

	renderer.ResetPipelines();
	CHECK(renderer.Pipelines().empty());
}

TEST_CASE("the default PBR graph compiles into the graph backend", "[render][graph]") {
	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("Default PBR#1"), DefaultGraph()));
	CHECK(renderer.Pipelines() == std::vector<Name>{Name("Default PBR#1")});
}

TEST_CASE("built-in capability fallbacks compile into the graph backend", "[render][graph]") {
	Renderer renderer;
	for (const auto &[name, document] : {
			 std::pair{Name("Tier B#1"), engine::graph::DefaultPbrTierBDocument()},
			 std::pair{Name("Tier C#1"), engine::graph::DefaultForwardTierCDocument()},
		 }) {
		RenderGraph graph;
		Name offender;
		REQUIRE(engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
		CHECK(renderer.SetPipeline(name, graph));
	}
}

TEST_CASE("default PBR stages are not mandatory backend policy", "[render][graph]") {
	RenderGraph graph;
	const engine::graph::ResourceId display = graph.AddResource(
		{.Name = Name("display"), .Kind = engine::graph::ResourceKind::Colour, .External = true}
	);
	REQUIRE(display.IsValid());

	engine::graph::Node present;
	present.Name = Name("minimal-present");
	present.Kind = Name("present");
	present.Reads = {display};
	present.Scope = engine::graph::NodeScope::Frame;
	REQUIRE(graph.AddNode(present).IsValid());

	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("minimal#1"), graph));
}

TEST_CASE("authored raster compute and inspection nodes are repeatable backend work", "[render][graph]") {
	RenderGraph graph;
	const engine::graph::ResourceId source = graph.AddResource(
		{.Name = Name("source"), .Kind = engine::graph::ResourceKind::Texture, .External = true}
	);
	const engine::graph::ResourceId first =
		graph.AddResource({.Name = Name("first"), .Kind = engine::graph::ResourceKind::Colour});
	const engine::graph::ResourceId second =
		graph.AddResource({.Name = Name("second"), .Kind = engine::graph::ResourceKind::Colour});
	const engine::graph::ResourceId storage =
		graph.AddResource({.Name = Name("storage"), .Kind = engine::graph::ResourceKind::Storage});
	REQUIRE(source.IsValid());
	REQUIRE(first.IsValid());
	REQUIRE(second.IsValid());
	REQUIRE(storage.IsValid());

	engine::graph::Node raster;
	raster.Name = Name("first-raster");
	raster.Kind = Name("raster");
	raster.Reads = {source};
	raster.Writes = {first};
	raster.Scope = engine::graph::NodeScope::View;
	REQUIRE(graph.AddNode(raster).IsValid());
	raster.Name = Name("second-raster");
	raster.Reads = {first};
	raster.Writes = {second};
	REQUIRE(graph.AddNode(raster).IsValid());

	engine::graph::Node dispatch;
	dispatch.Name = Name("compute");
	dispatch.Kind = Name("dispatch");
	dispatch.Reads = {second};
	dispatch.Writes = {storage};
	dispatch.Scope = engine::graph::NodeScope::View;
	REQUIRE(graph.AddNode(dispatch).IsValid());

	for (const char *kind : {"viewer", "capture", "viewer"}) {
		engine::graph::Node sink;
		sink.Name = Name(std::string(kind) + std::to_string(graph.Count()));
		sink.Kind = Name(kind);
		sink.Reads = {storage};
		sink.Scope = engine::graph::NodeScope::Frame;
		REQUIRE(graph.AddNode(sink).IsValid());
	}

	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("authored#1"), graph));
}

TEST_CASE("authored compute can be scoped once per world", "[render][graph]") {
	RenderGraph graph;
	const engine::graph::ResourceId storage =
		graph.AddResource({.Name = Name("world-storage"), .Kind = engine::graph::ResourceKind::Storage});
	REQUIRE(storage.IsValid());

	engine::graph::Node dispatch;
	dispatch.Name = Name("world-compute");
	dispatch.Kind = Name("dispatch");
	dispatch.Writes = {storage};
	dispatch.Scope = engine::graph::NodeScope::World;
	REQUIRE(graph.AddNode(dispatch).IsValid());

	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("world-compute#1"), graph));
}

TEST_CASE("blit owns its target format at the installation boundary", "[render][graph]") {
	RenderGraph graph;
	const engine::graph::ResourceId source = graph.AddResource(
		{.Name = Name("source"),
		 .Kind = engine::graph::ResourceKind::Texture,
		 .Format = engine::graph::ResourceFormat::RGBA16F,
		 .External = true}
	);
	const engine::graph::ResourceId target = graph.AddResource(
		{.Name = Name("target"),
		 .Kind = engine::graph::ResourceKind::Colour,
		 .Format = engine::graph::ResourceFormat::RGB10A2,
		 .External = true}
	);
	REQUIRE(source.IsValid());
	REQUIRE(target.IsValid());

	engine::graph::Node blit;
	blit.Name = Name("convert");
	blit.Kind = Name("blit");
	blit.Reads = {source};
	blit.Writes = {target};
	blit.Scope = engine::graph::NodeScope::View;
	blit.Parameters.push_back({.Key = Name("format"), .Value = "RGB10A2"});
	REQUIRE(graph.AddNode(blit).IsValid());

	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("conversion#1"), graph));

	blit.Name = Name("mismatch");
	blit.Parameters.front().Value = "RGBA8";
	RenderGraph mismatched;
	const engine::graph::ResourceId mismatchSource = mismatched.AddResource(
		{.Name = Name("source"), .Kind = engine::graph::ResourceKind::Texture, .External = true}
	);
	const engine::graph::ResourceId mismatchTarget = mismatched.AddResource(
		{.Name = Name("target"),
		 .Kind = engine::graph::ResourceKind::Colour,
		 .Format = engine::graph::ResourceFormat::RGB10A2,
		 .External = true}
	);
	blit.Reads = {mismatchSource};
	blit.Writes = {mismatchTarget};
	REQUIRE(mismatched.AddNode(blit).IsValid());
	CHECK_FALSE(renderer.SetPipeline(Name("conversion#2"), mismatched));
}

TEST_CASE("environment enabled switches select only their authored GPU mode", "[render][environment]") {
	engine::scene::Environment environment;
	environment.Skybox = engine::scene::SkyboxSource::Textures;
	CHECK(engine::render::EnvironmentModesOf(environment).Skybox == 1);
	environment.Textures.Enabled = false;
	CHECK(engine::render::EnvironmentModesOf(environment).Skybox == 0);

	environment.Skybox = engine::scene::SkyboxSource::Compute;
	CHECK(engine::render::EnvironmentModesOf(environment).Skybox == 2);
	environment.SkyCompute.Enabled = false;
	CHECK(engine::render::EnvironmentModesOf(environment).Skybox == 0);

	environment.HasClouds = true;
	CHECK(engine::render::EnvironmentModesOf(environment).Clouds == 1);
	environment.HasCloudCompute = true;
	CHECK(engine::render::EnvironmentModesOf(environment).Clouds == 2);
	environment.CloudVolume.Shader = engine::scene::CloudComputeShader::Voxel;
	CHECK(
		engine::render::EnvironmentShadersOf(environment).Clouds ==
		static_cast<uint32_t>(engine::scene::CloudComputeShader::Voxel)
	);
	environment.CloudVolume.Enabled = false;
	CHECK(engine::render::EnvironmentModesOf(environment).Clouds == 0);
	CHECK(engine::render::EnvironmentShadersOf(environment).Clouds == 0);
	CHECK(
		engine::render::EnvironmentCloudComputeOf(environment).Shader ==
		engine::scene::CloudComputeShader::Cumulus
	);
	environment.CloudLayer.Enabled = false;
	CHECK(engine::render::EnvironmentModesOf(environment).Clouds == 0);

	environment.HasAtmosphere = true;
	CHECK(engine::render::EnvironmentModesOf(environment).Atmosphere == 1);
	environment.HasAtmosphereCompute = true;
	environment.AirCompute.Shader = engine::scene::AtmosphereProceduralShader::Alien;
	CHECK(engine::render::EnvironmentModesOf(environment).Atmosphere == 2);
	CHECK(
		engine::render::EnvironmentShadersOf(environment).Atmosphere ==
		static_cast<uint32_t>(engine::scene::AtmosphereProceduralShader::Alien)
	);
	environment.AirCompute.Enabled = false;
	CHECK(engine::render::EnvironmentModesOf(environment).Atmosphere == 0);
	CHECK(engine::render::EnvironmentShadersOf(environment).Atmosphere == 0);
	CHECK(
		engine::render::EnvironmentAtmosphereComputeOf(environment).Shader ==
		engine::scene::AtmosphereProceduralShader::Earth
	);
}

TEST_CASE("optional default nodes can be disabled at the backend boundary", "[render][graph]") {
	engine::graph::PipelineDocument document = engine::graph::DefaultPbrDocument();
	engine::graph::Edit disabled;
	disabled.Kind = engine::graph::EditKind::Enable;
	disabled.Name = Name("shadow");
	disabled.Enabled = false;
	document.Record(disabled);
	disabled.Name = Name("ssao");
	document.Record(disabled);
	disabled.Name = Name("mirror-capture");
	document.Record(disabled);

	RenderGraph graph;
	Name offender;
	REQUIRE(engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("unshadowed#1"), graph));
}

TEST_CASE("backend queue and overlap controls describe the work each node records", "[render][graph]") {
	engine::graph::PipelineDocument document = engine::graph::DefaultPbrDocument();
	document = WithSetting(document, Name("cull-frustum"), Name("queue"), "cpu");
	document = WithSetting(document, Name("upload-instances"), Name("queue"), "transfer");
	document = WithSetting(document, Name("gbuffer"), Name("queue"), "graphics");
	document = WithSetting(document, Name("ssao"), Name("async"), "allow");

	RenderGraph graph;
	Name offender;
	REQUIRE(engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("queued#1"), graph));
}

TEST_CASE("entity culling nodes compose and may repeat in an authored graph", "[render][graph][cull]") {
	RenderGraph graph;
	const auto resource = [&graph](const char *name, engine::graph::ResourceKind kind) {
		return graph.AddResource({.Name = Name(name), .Kind = kind});
	};
	const engine::graph::ResourceId camera = resource("camera", engine::graph::ResourceKind::Camera);
	const engine::graph::ResourceId all = resource("all", engine::graph::ResourceKind::Entities);
	const engine::graph::ResourceId tagged = resource("tagged", engine::graph::ResourceKind::Entities);
	const engine::graph::ResourceId retagged = resource("retagged", engine::graph::ResourceKind::Entities);
	const engine::graph::ResourceId near = resource("near", engine::graph::ResourceKind::Entities);
	const engine::graph::ResourceId ordered = resource("ordered", engine::graph::ResourceKind::Entities);

	const auto node = [&graph](
						  const char *name,
						  const char *kind,
						  std::vector<engine::graph::ResourceId> reads,
						  std::vector<engine::graph::ResourceId> writes,
						  std::vector<engine::graph::NodeParameter> parameters = {}
					  ) {
		engine::graph::Node value;
		value.Name = Name(name);
		value.Kind = Name(kind);
		value.Reads = std::move(reads);
		value.Writes = std::move(writes);
		value.Parameters = std::move(parameters);
		REQUIRE(graph.AddNode(value).IsValid());
	};
	node("camera", "camera", {}, {camera});
	node("entities", "entities", {}, {all});
	node("characters", "filter-tag", {all}, {tagged}, {{Name("mask"), "0x3"}});
	node("players", "filter-tag", {tagged}, {retagged}, {{Name("mask"), "0x1"}});
	node("nearby", "cull-distance", {retagged, camera}, {near}, {{Name("radius"), "128.5"}});
	node("order", "order-draw", {near, camera}, {ordered});

	Renderer renderer;
	CHECK(renderer.SetPipeline(Name("filtered#1"), graph));
}

TEST_CASE("culling hints cannot be attached to a pass that ignores entity filters", "[render][graph][cull]") {
	Renderer renderer;
	const auto install = [&](Name pipeline, Name node, std::string value) {
		const engine::graph::PipelineDocument document =
			WithSetting(engine::graph::DefaultPbrDocument(), node, Name("culling"), std::move(value));
		RenderGraph graph;
		Name offender;
		REQUIRE(engine::graph::Build(document, graph, offender) == engine::graph::PipelineDocumentStatus::Ok);
		return renderer.SetPipeline(pipeline, graph);
	};

	CHECK_FALSE(install(Name("ignored-cull#1"), Name("gbuffer"), "none"));
	CHECK(install(Name("unculled#1"), Name("cull-frustum"), "none"));

	// Accepted since the backend grew its depth pyramid and indirect draw
	// path: the default document carries the gbuffer pass whose early phase
	// seeds the pyramid.
	CHECK(install(Name("occlusion#1"), Name("cull-frustum"), "occlusion"));
}

TEST_CASE("occlusion culling is refused without the pass that seeds its pyramid", "[render][graph][cull]") {
	RenderGraph graph;
	const auto resource = [&graph](const char *name, engine::graph::ResourceKind kind) {
		return graph.AddResource({.Name = Name(name), .Kind = kind});
	};
	const engine::graph::ResourceId camera = resource("camera", engine::graph::ResourceKind::Camera);
	const engine::graph::ResourceId all = resource("all", engine::graph::ResourceKind::Entities);
	const engine::graph::ResourceId visible = resource("visible", engine::graph::ResourceKind::Entities);
	const engine::graph::ResourceId ordered = resource("ordered", engine::graph::ResourceKind::Entities);

	const auto node = [&graph](
						  const char *name,
						  const char *kind,
						  std::vector<engine::graph::ResourceId> reads,
						  std::vector<engine::graph::ResourceId> writes,
						  std::vector<engine::graph::NodeParameter> parameters = {}
					  ) {
		engine::graph::Node value;
		value.Name = Name(name);
		value.Kind = Name(kind);
		value.Reads = std::move(reads);
		value.Writes = std::move(writes);
		value.Parameters = std::move(parameters);
		REQUIRE(graph.AddNode(value).IsValid());
	};
	node("camera", "camera", {}, {camera});
	node("entities", "entities", {}, {all});
	node("cull", "cull-frustum", {all, camera}, {visible}, {{Name("culling"), "occlusion"}});
	node("order", "order-draw", {visible, camera}, {ordered});

	// No gbuffer pass, so nothing writes the depth the pyramid reduces - the
	// hint authored a cull nothing can feed.
	Renderer renderer;
	CHECK_FALSE(renderer.SetPipeline(Name("blind-occlusion#1"), graph));
}

TEST_CASE("a frame result reports authored node names", "[render]") {
	FrameResult result;
	CHECK_FALSE(result.Ran(Name("geometry")));

	result.Nodes = {Name("geometry"), Name("ao.quality"), Name("present")};
	CHECK(result.Ran(Name("geometry")));
	CHECK(result.Ran(Name("ao.quality")));
	CHECK(result.Ran(Name("present")));
	CHECK_FALSE(result.Ran(Name("shadow")));
}

TEST_CASE("a batched frame result adds counters and de-duplicates nodes", "[render][batch]") {
	FrameResult frame;
	frame.DrawCalls = 2;
	frame.Triangles = 12;
	frame.Nodes = {Name("shadow"), Name("gbuffer")};

	FrameResult view;
	view.Presented = true;
	view.DrawCalls = 3;
	view.Triangles = 24;
	view.SurfaceInstances = 2;
	view.SurfacePasses = 1;
	view.PortalPasses = 4;
	view.RibbonVertices = 22;
	view.Particles = 8;
	view.Culled = 7;
	view.ScheduledReadBytes = 1024;
	view.ScheduledWriteBytes = 2048;
	view.QueueTransferBytes = 512;
	view.UploadedBytes = 4096;
	view.UploadCommandBuffers = 3;
	view.ComputeDispatches = 4;
	view.AsyncComputeCommandBuffers = 2;
	view.ConcurrentWaves = 2;
	view.Nodes = {Name("gbuffer"), Name("tonemap"), Name("present")};

	frame.Accumulate(view);

	CHECK(frame.Presented);
	CHECK(frame.DrawCalls == 5);
	CHECK(frame.Triangles == 36);
	CHECK(frame.SurfaceInstances == 2);
	CHECK(frame.SurfacePasses == 1);
	CHECK(frame.PortalPasses == 4);
	CHECK(frame.RibbonVertices == 22);
	CHECK(frame.Particles == 8);
	CHECK(frame.Culled == 7);
	CHECK(frame.ScheduledReadBytes == 1024);
	CHECK(frame.ScheduledWriteBytes == 2048);
	CHECK(frame.QueueTransferBytes == 512);
	CHECK(frame.UploadedBytes == 4096);
	CHECK(frame.UploadCommandBuffers == 3);
	CHECK(frame.ComputeDispatches == 4);
	CHECK(frame.AsyncComputeCommandBuffers == 2);
	CHECK(frame.ConcurrentWaves == 2);
	CHECK(
		frame.Nodes == std::vector<Name>{Name("shadow"), Name("gbuffer"), Name("tonemap"), Name("present")}
	);
}

TEST_CASE("a renderer returns the complete lighting state it was given", "[render][batch]") {
	Renderer renderer;
	engine::scene::WorldLighting lighting;
	lighting.Direction = {0.0f, -2.0f, 0.0f};
	lighting.Ambient = {0.1f, 0.2f, 0.3f};
	lighting.OutdoorAmbient = {0.4f, 0.5f, 0.6f};
	lighting.Direct = {0.7f, 0.8f, 0.9f};
	lighting.FogColor = {0.11f, 0.22f, 0.33f};
	lighting.FogStart = 12.0f;
	lighting.FogEnd = 48.0f;
	lighting.EnvironmentState.Skybox = engine::scene::SkyboxSource::Compute;
	lighting.EnvironmentState.SkyCompute.Seed = 83;
	renderer.SetLighting(lighting);

	const engine::scene::WorldLighting current = renderer.CurrentLighting();
	CHECK(current.Direction == (engine::core::Vector3{0.0f, -1.0f, 0.0f}));
	CHECK(current.Ambient == lighting.Ambient);
	CHECK(current.OutdoorAmbient == lighting.OutdoorAmbient);
	CHECK(current.Direct == lighting.Direct);
	CHECK(current.FogColor == lighting.FogColor);
	CHECK(current.FogStart == 12.0f);
	CHECK(current.FogEnd == 48.0f);
	CHECK(current.EnvironmentState.Skybox == engine::scene::SkyboxSource::Compute);
	CHECK(current.EnvironmentState.SkyCompute.Seed == 83);
}

// The other half of D00016's neighbourhood: a decision that was recorded and
// not enforced.
//
// v0.7 decided that a studio with several viewports draws them one after
// another - the passes share one command buffer and one device, so parallel
// recording would serialise at submit and would cost the ordering that makes a
// docked viewport show *this* frame. That went into `ROADMAP.md` and nothing
// checked it, which is the shape of every stale claim this repository has found.
//
// **No device is created here.** The owner is claimed by the constructor and
// re-claimed by `Initialise`, precisely so the contract can be exercised on a
// build machine with no GPU. What cannot be asserted is the abort itself -
// `RequireOwningThread` calls `std::abort`, on purpose, and a test that survived
// it would be testing something else.
TEST_CASE("a renderer is owned by one thread", "[render]") {
	engine::render::Renderer renderer;

	CHECK(renderer.IsOnOwningThread());

	bool ownedElsewhere = true;
	std::thread other([&renderer, &ownedElsewhere] { ownedElsewhere = renderer.IsOnOwningThread(); });
	other.join();

	// The assertion the contract is made of: a second thread is not the owner,
	// so `Render` from a worker is refused rather than racing the frame the main
	// thread is recording.
	CHECK_FALSE(ownedElsewhere);

	// And the owner is unchanged by having been asked from elsewhere - the check
	// is a comparison and never a claim.
	CHECK(renderer.IsOnOwningThread());
}
