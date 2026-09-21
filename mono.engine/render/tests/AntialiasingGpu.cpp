// Real-device proof that every authored antialiasing choice executes and changes a hard edge.

#include "RenderFixture.hpp"

#include <engine/core/Name.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.render.antialiasinggpu")
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

	std::string EdgeShader(float rowOffset) {
		return std::string(R"glsl(#version 450
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
)glsl") + std::string(PASS_BLOCK) +
			   "\nvoid main() {\n\tfloat edge = step(inUv.x, inUv.y + " + std::to_string(rowOffset) +
			   " * pass.Target.w);\n\toutColour = vec4(vec3(edge), 1.0);\n}\n";
	}

	std::string ZeroVelocityShader() {
		return std::string(R"glsl(#version 450
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outVelocity;
)glsl") + std::string(PASS_BLOCK) +
			   R"glsl(
void main() {
	outVelocity = vec4(0.0);
}
)glsl";
	}

	graph::ResourceId AddTexture(
		graph::RenderGraph &graph,
		const char *name,
		graph::ResourceFormat format = graph::ResourceFormat::RGBA8_SRGB
	) {
		const graph::ResourceId resource = graph.AddResource({
			.Name = core::Name(name),
			.Kind = graph::ResourceKind::Colour,
			.Format = format,
		});
		REQUIRE(resource.IsValid());
		return resource;
	}

	void
	AddRaster(graph::RenderGraph &graph, const char *name, std::string source, graph::ResourceId output) {
		graph::Node node;
		node.Name = core::Name(name);
		node.Kind = core::Name("raster");
		node.Scope = graph::NodeScope::View;
		node.Writes = {output};
		node.Parameters.push_back({core::Name("source"), std::move(source)});
		REQUIRE(graph.AddNode(std::move(node)).IsValid());
	}

	void AddAntialiasing(
		graph::RenderGraph &graph,
		const char *name,
		const char *kind,
		std::initializer_list<graph::ResourceId> reads,
		std::initializer_list<graph::ResourceId> writes
	) {
		graph::Node node;
		node.Name = core::Name(name);
		node.Kind = core::Name(kind);
		node.Scope = graph::NodeScope::View;
		node.Reads.assign(reads);
		node.Writes.assign(writes);
		REQUIRE(graph.AddNode(std::move(node)).IsValid());
	}

	size_t PartialPixels(const CapturedImage &image) {
		size_t count = 0;
		for (uint32_t y = 0; y < image.Height; ++y) {
			for (uint32_t x = 0; x < image.Width; ++x) {
				const size_t offset = y * image.RowStrideBytes + x * 4;
				const uint8_t value = static_cast<uint8_t>(image.Bytes[offset]);
				count += value > 4 && value < 251;
			}
		}
		return count;
	}

	bool SamePixels(const CapturedImage &left, const CapturedImage &right) {
		for (uint32_t y = 0; y < left.Height; ++y) {
			const auto leftRow = std::span(left.Bytes).subspan(y * left.RowStrideBytes, left.Width * 4);
			const auto rightRow = std::span(right.Bytes).subspan(y * right.RowStrideBytes, right.Width * 4);
			if (!std::equal(leftRow.begin(), leftRow.end(), rightRow.begin())) {
				return false;
			}
		}
		return true;
	}
}

TEST_CASE("fxaa taa and smaa execute as selectable graph nodes", "[render][gpu][antialiasing][.]") {
	using namespace engine;
	using namespace engine::render::test;

	FixtureDevice fixture;
	fixture.Initialise();

	graph::RenderGraph pipeline;
	const graph::ResourceId source = AddTexture(pipeline, "aa-source");
	const graph::ResourceId history = AddTexture(pipeline, "aa-history-source");
	const graph::ResourceId velocity = AddTexture(pipeline, "aa-velocity", graph::ResourceFormat::RG16F);
	const graph::ResourceId fxaa = AddTexture(pipeline, "aa-fxaa");
	const graph::ResourceId taa = AddTexture(pipeline, "aa-taa");
	const graph::ResourceId taaHistory = AddTexture(pipeline, "aa-taa-history");
	const graph::ResourceId edges = AddTexture(pipeline, "aa-smaa-edges", graph::ResourceFormat::RG8);
	const graph::ResourceId weights = AddTexture(pipeline, "aa-smaa-weights");
	const graph::ResourceId smaa = AddTexture(pipeline, "aa-smaa");

	AddRaster(pipeline, "aa-source-pass", EdgeShader(0.0f), source);
	AddRaster(pipeline, "aa-history-pass", EdgeShader(1.0f), history);
	AddRaster(pipeline, "aa-velocity-pass", ZeroVelocityShader(), velocity);
	AddAntialiasing(pipeline, "aa-fxaa-pass", "fxaa", {source}, {fxaa});
	AddAntialiasing(pipeline, "aa-taa-pass", "taa", {source, history, velocity}, {taa, taaHistory});
	AddAntialiasing(pipeline, "aa-smaa-edge-pass", "smaa-edges", {source}, {edges});
	AddAntialiasing(pipeline, "aa-smaa-blend-pass", "smaa-blend", {edges}, {weights});
	AddAntialiasing(pipeline, "aa-smaa-resolve-pass", "smaa-resolve", {source, weights}, {smaa});

	const core::Name boundaryKind("aa-test-boundary");
	graph::NodeKindSpec boundary;
	boundary.Kind = boundaryKind;
	boundary.Scope = graph::NodeScope::Frame;
	boundary.Queue = graph::ExecutionQueue::Cpu;
	boundary.Category = graph::NodeCategory::Output;
	const std::array kept{
		std::pair{source, "source"},
		std::pair{fxaa, "fxaa"},
		std::pair{taa, "taa"},
		std::pair{taaHistory, "taa-history"},
		std::pair{edges, "edges"},
		std::pair{weights, "weights"},
		std::pair{smaa, "smaa"},
	};
	for (const auto &[resource, name] : kept) {
		(void)resource;
		boundary.Inputs.push_back({.Name = core::Name(name), .Kind = graph::ResourceKind::Texture});
	}
	REQUIRE(graph::RegisterNodeKind(std::move(boundary)));
	REQUIRE(fixture.Render.InstallNodeHandler(boundaryKind, [](const graph::RunContext &) { return true; }));
	graph::Node sink;
	sink.Name = boundaryKind;
	sink.Kind = boundaryKind;
	sink.Scope = graph::NodeScope::Frame;
	for (const auto &[resource, name] : kept) {
		(void)name;
		sink.Reads.push_back(resource);
	}
	REQUIRE(pipeline.AddNode(std::move(sink)).IsValid());
	REQUIRE(fixture.Render.SetPipeline(core::Name("aa-test"), pipeline));

	render::SceneTarget target{65, 65};
	render::View view;
	view.World = 411;
	view.WorldName = core::Name("aa-test-world");
	view.Pipeline = core::Name("aa-test");
	view.Target = &target;
	render::OverlayImage overlay;
	const render::FrameResult frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	for (const char *node : {
			 "aa-source-pass",
			 "aa-fxaa-pass",
			 "aa-taa-pass",
			 "aa-smaa-edge-pass",
			 "aa-smaa-blend-pass",
			 "aa-smaa-resolve-pass",
		 }) {
		CHECK(frame.Ran(core::Name(node)));
	}

	const auto capture = [&](const char *name) {
		return CaptureResource(
			fixture.Render, core::Name(name), view.Slot, target.Width, target.Height, ImageFormat::Rgba8Unorm
		);
	};
	const CapturedImage sourceImage = capture("aa-source");
	const CapturedImage fxaaImage = capture("aa-fxaa");
	const CapturedImage taaImage = capture("aa-taa");
	const CapturedImage taaHistoryImage = capture("aa-taa-history");
	const CapturedImage weightImage = capture("aa-smaa-weights");
	const CapturedImage smaaImage = capture("aa-smaa");

	CHECK(PartialPixels(sourceImage) == 0);
	CHECK(PartialPixels(fxaaImage) > 0);
	CHECK(PartialPixels(taaImage) > 0);
	CHECK(PartialPixels(weightImage) > 0);
	CHECK(PartialPixels(smaaImage) > 0);
	CHECK_FALSE(SamePixels(sourceImage, fxaaImage));
	CHECK_FALSE(SamePixels(sourceImage, smaaImage));
	CHECK(SamePixels(taaImage, taaHistoryImage));
}
