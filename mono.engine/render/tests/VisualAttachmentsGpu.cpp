// Real-device proof that visual shader attachments stay dormant until selected.

#include "RenderFixture.hpp"

#include <engine/core/Name.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>

TEST_SUITE_ID("engine.render.visualattachmentsgpu")
TEST_DEPENDS("engine.graph.rendergraph")

namespace {
	using namespace engine;
	using namespace engine::render::test;

	constexpr std::string_view PASS_BLOCK = R"glsl(
layout(set = 3, binding = 0, std140) uniform Pass {
	mat4 ViewProjection;
	mat4 InverseViewProjection;
	vec4 Target;
	vec4 View;
	uvec4 RenderFeatures;
} pass;
)glsl";

	std::string RasterSource(std::string_view body, bool sampled) {
		std::string source = R"glsl(#version 450
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
)glsl";
		if (sampled) {
			source += "layout(set = 2, binding = 0) uniform sampler2D sourceImage;\n";
		}
		source += PASS_BLOCK;
		source += "\nvoid main() {\n";
		source += body;
		source += "\n}\n";
		return source;
	}

	std::string ComputeSource() {
		return R"glsl(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform sampler2D sourceImage;
layout(set = 1, binding = 0, rgba16f) writeonly uniform image2D targetImage;
layout(set = 2, binding = 0, std140) uniform Pass {
	mat4 ViewProjection;
	mat4 InverseViewProjection;
	vec4 Target;
	vec4 View;
	uvec4 RenderFeatures;
} pass;
void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, ivec2(pass.Target.xy)))) {
		return;
	}
	imageStore(targetImage, pixel, vec4(0.1, 0.8, 0.1, 1.0));
}
)glsl";
	}

	graph::ResourceId AddImage(
		graph::RenderGraph &graph, const char *name, graph::ResourceKind kind, graph::ResourceFormat format
	) {
		const graph::ResourceId resource =
			graph.AddResource({.Name = core::Name(name), .Kind = kind, .Format = format});
		REQUIRE(resource.IsValid());
		return resource;
	}

	void AddRaster(
		graph::RenderGraph &graph,
		const char *name,
		std::string source,
		std::initializer_list<graph::ResourceId> reads,
		graph::ResourceId write,
		bool attachment = false
	) {
		graph::Node node;
		node.Name = core::Name(name);
		node.Kind = core::Name("raster");
		node.Scope = graph::NodeScope::View;
		node.Reads.assign(reads);
		node.Writes = {write};
		node.Parameters.push_back({core::Name("source"), std::move(source)});
		if (attachment) {
			node.Parameters.push_back({core::Name("attachment"), "visual"});
		}
		REQUIRE(graph.AddNode(std::move(node)).IsValid());
	}

	std::array<uint8_t, 3> Centre(const CapturedImage &image) {
		const size_t offset = (image.Height / 2) * image.RowStrideBytes + (image.Width / 2) * 4;
		return {
			static_cast<uint8_t>(image.Bytes[offset]),
			static_cast<uint8_t>(image.Bytes[offset + 1]),
			static_cast<uint8_t>(image.Bytes[offset + 2]),
		};
	}
}

TEST_CASE(
	"visual raster and compute attachments activate independently", "[render][gpu][visual-attachment][.]"
) {
	using namespace engine;
	using namespace engine::render::test;

	FixtureDevice fixture;
	fixture.Initialise();

	graph::RenderGraph pipeline;
	const graph::ResourceId source =
		AddImage(pipeline, "attachment-source", graph::ResourceKind::Colour, graph::ResourceFormat::RGBA16F);
	const graph::ResourceId post =
		AddImage(pipeline, "attachment-post", graph::ResourceKind::Colour, graph::ResourceFormat::RGBA16F);
	const graph::ResourceId computed = AddImage(
		pipeline, "attachment-compute", graph::ResourceKind::Storage, graph::ResourceFormat::RGBA16F
	);
	const graph::ResourceId final = AddImage(
		pipeline, "attachment-final", graph::ResourceKind::Colour, graph::ResourceFormat::RGBA8_SRGB
	);

	AddRaster(
		pipeline,
		"attachment-source-pass",
		RasterSource("\toutColour = vec4(0.2, 0.4, 0.6, 1.0);", false),
		{},
		source
	);
	AddRaster(
		pipeline,
		"attachment-post-pass",
		RasterSource("\toutColour = vec4(vec3(1.0) - texture(sourceImage, inUv).rgb, 1.0);", true),
		{source},
		post,
		true
	);
	graph::Node compute;
	compute.Name = core::Name("attachment-compute-pass");
	compute.Kind = core::Name("dispatch");
	compute.Scope = graph::NodeScope::View;
	compute.Reads = {post};
	compute.Writes = {computed};
	compute.Parameters = {
		{core::Name("source"), ComputeSource()},
		{core::Name("attachment"), "visual"},
		{core::Name("uniforms"), "view"},
	};
	REQUIRE(pipeline.AddNode(std::move(compute)).IsValid());
	AddRaster(
		pipeline,
		"attachment-final-pass",
		RasterSource("\toutColour = texture(sourceImage, inUv);", true),
		{computed},
		final
	);

	const core::Name boundaryKind("attachment-test-boundary");
	graph::NodeKindSpec boundary;
	boundary.Kind = boundaryKind;
	boundary.Scope = graph::NodeScope::Frame;
	boundary.Queue = graph::ExecutionQueue::Cpu;
	boundary.Category = graph::NodeCategory::Output;
	boundary.Inputs.push_back({.Name = core::Name("image"), .Kind = graph::ResourceKind::Texture});
	REQUIRE(graph::RegisterNodeKind(std::move(boundary)));
	REQUIRE(fixture.Render.InstallNodeHandler(boundaryKind, [](const graph::RunContext &) { return true; }));
	graph::Node sink;
	sink.Name = boundaryKind;
	sink.Kind = boundaryKind;
	sink.Scope = graph::NodeScope::Frame;
	sink.Reads = {final};
	REQUIRE(pipeline.AddNode(std::move(sink)).IsValid());
	const core::Name pipelineName("attachment-test");
	REQUIRE(fixture.Render.SetPipeline(pipelineName, pipeline));

	render::SceneTarget target{33, 33};
	render::View view;
	view.World = 733;
	view.WorldName = core::Name("attachment-test-world");
	view.Pipeline = pipelineName;
	view.Target = &target;
	render::OverlayImage overlay;
	const auto renderCentre = [&] {
		const render::FrameResult frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		CHECK(frame.Ran(core::Name("attachment-post-pass")));
		CHECK(frame.Ran(core::Name("attachment-compute-pass")));
		return Centre(CaptureResource(
			fixture.Render,
			core::Name("attachment-final"),
			view.Slot,
			target.Width,
			target.Height,
			ImageFormat::Rgba8Unorm
		));
	};

	const std::array<uint8_t, 3> inactive = renderCentre();
	CHECK(inactive[2] > inactive[1]);
	CHECK(inactive[1] > inactive[0]);

	scene::DrawInstance selected;
	selected.Source = 1;
	selected.Effects.Count = 1;
	selected.Effects.Attachments[0] = {
		.Node = core::Name("attachment-post-pass"),
		.Stage = scene::RenderEffectStage::PostProcess,
		.Enabled = true,
	};
	view.Instances = std::span(&selected, 1);
	const std::array<uint8_t, 3> postOnly = renderCentre();
	CHECK(postOnly[0] > postOnly[1]);
	CHECK(postOnly[1] > postOnly[2]);

	selected.Effects.Attachments[0].Node = core::Name("attachment-compute-pass");
	selected.Effects.Attachments[0].Stage = scene::RenderEffectStage::Compute;
	const std::array<uint8_t, 3> computeOnly = renderCentre();
	CHECK(computeOnly[1] > computeOnly[0] + 80);
	CHECK(computeOnly[1] > computeOnly[2] + 80);

	CHECK(fixture.Render.RemovePipeline(pipelineName));
	CHECK(fixture.Render.ResourceTexture(core::Name("attachment-final"), view.Slot) == nullptr);
}
