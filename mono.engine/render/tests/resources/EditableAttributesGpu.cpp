// Real-device proof that an editable mesh's free channels reach a ShaderScript.

#include "../RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/render/resources/ShaderCompiler.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>

TEST_SUITE_ID("engine.render.editableattributesgpu")

namespace {
	using namespace engine;
	using namespace engine::render::test;

	assets::MeshData Quad(bool channel) {
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
			{{0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
			{{0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
			{{-0.5f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
		};
		mesh.Indices = {0, 1, 2, 0, 2, 3};
		if (channel) mesh.Attributes[2].assign(4, {0.0f, 1.0f, 0.0f, 1.0f});
		mesh.ComputeBounds();
		return mesh;
	}

	// Red and green of the centre pixel of an RGBA8 capture.
	std::array<int, 2> Centre(const CapturedImage &image) {
		const size_t at = (image.Height / 2) * image.RowStrideBytes + (image.Width / 2) * 4;
		return {std::to_integer<int>(image.Bytes[at]), std::to_integer<int>(image.Bytes[at + 1])};
	}
}

TEST_CASE(
	"a ShaderScript reads an editable mesh attribute channel", "[render][gpu][editable-attributes][.]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	render::ShaderCompiler compiler;
	const auto compiled = compiler.Compile(
		"#version 450\nlayout(location = 13) in vec4 inAttribute2;\nlayout(location = 0) out vec4 colour;\n"
		"void main() { colour = vec4(inAttribute2.rgb * 4.0, 1.0); }\n",
		render::ShaderStage::Fragment,
		"channels.frag"
	);
	INFO(compiled.Error);
	REQUIRE_FALSE(compiled.Failed);
	const core::Name shader("channels.shader");
	REQUIRE(fixture.Render.AddShader(shader, compiled.SpirV));
	REQUIRE(fixture.Render.AddMesh(core::Name("channels.plain"), Quad(false)));
	REQUIRE(fixture.Render.AddMesh(core::Name("channels.carrying"), Quad(true)));

	scene::DrawInstance instance;
	instance.Source = 1;
	instance.Frame.Position = {0.0f, 0.0f, -2.0f};
	instance.HalfExtent = {2.0f, 2.0f, 0.01f};
	instance.Tint = {1.0f, 1.0f, 1.0f};
	instance.CastShadow = false;
	instance.Shader = shader;
	render::SceneTarget target{33, 25};
	render::View view;
	view.World = 4411;
	view.WorldName = core::Name("channels.world");
	view.Target = &target;
	view.Instances = std::span(&instance, 1);
	render::OverlayImage overlay;
	const auto draw = [&](const char *mesh) {
		instance.Mesh = core::Name(mesh);
		view.Damage.Objects = true;
		fixture.Render.Render(std::span(&view, 1), overlay, nullptr, false);
		return Centre(CaptureResource(
			fixture.Render, core::Name("tonemapped"), 0, target.Width, target.Height, ImageFormat::Rgba8Unorm
		));
	};

	const auto carrying = draw("channels.carrying");
	CHECK(carrying[1] > 128);
	CHECK(carrying[0] < 32);
	// A mesh with no channels reads zeros through the same shader.
	const auto plain = draw("channels.plain");
	CHECK(plain[1] < 32);
}
