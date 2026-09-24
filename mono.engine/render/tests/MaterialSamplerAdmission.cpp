#include "MaterialSamplerAdmission.hpp"

#include "RenderFixture.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

TEST_SUITE_ID("engine.render.materialsampleradmission")
TEST_DEPENDS("engine.render.shadercompiler")

TEST_CASE("material sampler declarations match named draw slots", "[render][shader-sampler]") {
	using namespace engine::render;
	ShaderCapabilities capabilities;
	capabilities.Resources.push_back({"colourMap", ShaderResourceKind::SampledTexture, 2, 2});
	capabilities.Resources.push_back({"normalMap", ShaderResourceKind::SampledTexture, 2, 4});
	CHECK_FALSE(AdmitMaterialSamplers(capabilities).has_value());
	capabilities.Resources[1].Name = "customImage";
	const auto wrongName = AdmitMaterialSamplers(capabilities);
	REQUIRE(wrongName.has_value());
	CHECK(wrongName->find("customImage") != std::string::npos);
	CHECK(wrongName->find("set 2 binding 4") != std::string::npos);
	CHECK(wrongName->find("normalMap") != std::string::npos);
	capabilities.Resources[1].Name = "normalMap";
	capabilities.Resources[1].Binding = 10;
	CHECK(AdmitMaterialSamplers(capabilities).has_value());
	capabilities.Resources[1].Binding = 4;
	capabilities.Resources[1].Set = 1;
	CHECK(AdmitMaterialSamplers(capabilities).has_value());
	capabilities.Resources[1].Set = 2;
	capabilities.Resources[1].Kind = ShaderResourceKind::SeparateTexture;
	CHECK(AdmitMaterialSamplers(capabilities).has_value());
	capabilities.Resources[1] = capabilities.Resources[0];
	CHECK(AdmitMaterialSamplers(capabilities).has_value());
}

TEST_CASE(
	"hosted material shader rejects a mismatched edit and retains its pipeline",
	"[render][shader-sampler][gpu][.]"
) {
	using namespace engine::render;
	test::FixtureDevice fixture;
	fixture.Initialise();
	ShaderCompiler compiler;
	const auto compile = [&](std::string_view sampler) {
		return compiler.Compile(
			"#version 450\nlayout(location=0) out vec4 colour;\n"
			"layout(set=2,binding=2) uniform sampler2D " +
				std::string(sampler) + ";\nvoid main(){colour=texture(" + std::string(sampler) +
				",vec2(0.5));}\n",
			ShaderStage::Fragment,
			"named-material.frag"
		);
	};
	const auto accepted = compile("colourMap");
	const auto mismatched = compile("otherMap");
	REQUIRE_FALSE(accepted.Failed);
	REQUIRE_FALSE(mismatched.Failed);
	const engine::core::Name name("named-material-admission");
	REQUIRE(fixture.Render.AddMaterialShader(name, accepted.SpirV));
	const uint64_t acceptedRevision = fixture.Render.ResourceRevision();
	CHECK_FALSE(fixture.Render.AddMaterialShader(name, mismatched.SpirV));
	CHECK(fixture.Render.HasShader(name));
	CHECK(fixture.Render.ResourceRevision() == acceptedRevision);
}
