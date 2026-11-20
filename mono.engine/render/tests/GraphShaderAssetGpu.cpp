// A cooked shader asset registered with `Renderer::AddGraphShader` drives a raster
// graph node that names it, and dropping it stops the node resolving.

#include "RenderFixture.hpp"

#include <engine/assets/Shader.hpp>
#include <engine/core/Name.hpp>
#include <engine/graph/RenderGraph.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <span>
#include <string>

TEST_SUITE_ID("engine.render.graphshaderassetgpu")
TEST_DEPENDS("engine.render.shadercompiler")

namespace {
	using namespace engine;

	// Writes a fixed colour; its red channel is what the test reads back.
	constexpr std::string_view FILL = R"glsl(#version 450
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
void main() {
	outColour = vec4(1.0, 0.0, 0.0, 1.0);
}
)glsl";

	assets::ShaderData Cooked(std::string_view source, render::ShaderStage stage, const char *spelling) {
		render::ShaderCompiler compiler;
		const auto compiled = compiler.Compile(source, stage, "graph-shader-asset");
		REQUIRE_FALSE(compiled.Failed);
		assets::ShaderPayload payload;
		payload.Backend = "spirv";
		payload.EntryPoint = "main";
		payload.Bytes.resize(compiled.SpirV.size() * sizeof(uint32_t));
		std::memcpy(payload.Bytes.data(), compiled.SpirV.data(), payload.Bytes.size());
		assets::ShaderVariant variant;
		variant.Name = "graph";
		variant.Stage = spelling;
		variant.Payloads.push_back(std::move(payload));
		assets::ShaderData data;
		data.Variants.push_back(std::move(variant));
		return data;
	}
}

TEST_CASE("a registered shader asset drives a raster graph node", "[render][gpu][.]") {
	using namespace engine::render::test;

	FixtureDevice fixture;
	fixture.Initialise();

	const core::Name shader("fill.shader");
	std::string failure;
	assets::ShaderData unusable;
	assets::ShaderVariant geometry;
	geometry.Name = "x";
	geometry.Stage = "geometry";
	unusable.Variants.push_back(geometry);
	CHECK_FALSE(fixture.Render.AddGraphShader(shader, unusable, failure));
	CHECK_FALSE(failure.empty());
	REQUIRE(fixture.Render.AddGraphShader(
		shader, Cooked(FILL, render::ShaderStage::Fragment, "fragment"), failure
	));
	CHECK(fixture.Render.HasGraphShader(shader));

	graph::RenderGraph pipeline;
	const graph::ResourceId output = pipeline.AddResource({
		.Name = core::Name("asset-fill"),
		.Kind = graph::ResourceKind::Colour,
		.Format = graph::ResourceFormat::RGBA8,
	});
	REQUIRE(output.IsValid());
	graph::Node node;
	node.Name = core::Name("asset-fill-pass");
	node.Kind = core::Name("raster");
	node.Scope = graph::NodeScope::View;
	node.Writes = {output};
	node.Parameters.push_back({core::Name("shader"), std::string(shader.Text())});
	REQUIRE(pipeline.AddNode(std::move(node)).IsValid());
	REQUIRE(fixture.Render.SetPipeline(core::Name("asset-fill-test"), pipeline));

	render::SceneTarget target{16, 16};
	render::View view;
	view.World = 412;
	view.WorldName = core::Name("asset-fill-world");
	view.Pipeline = core::Name("asset-fill-test");
	view.Target = &target;
	render::OverlayImage overlay;
	const render::FrameResult frame = fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
	REQUIRE(frame.Ran(core::Name("asset-fill-pass")));
	const CapturedImage image = CaptureResource(
		fixture.Render,
		core::Name("asset-fill"),
		view.Slot,
		target.Width,
		target.Height,
		ImageFormat::Rgba8Unorm
	);
	REQUIRE(image.Bytes.size() >= 4);
	CHECK(static_cast<uint8_t>(image.Bytes[0]) == 255);
	CHECK(static_cast<uint8_t>(image.Bytes[1]) == 0);

	CHECK(fixture.Render.DropGraphShader(shader));
	CHECK_FALSE(fixture.Render.HasGraphShader(shader));
	CHECK_FALSE(fixture.Render.DropGraphShader(shader));
}
