#include "MaterialSamplerAdmission.hpp"

#include "RenderFixture.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

TEST_CASE("cooked material sampler admission validates the bound texture type", "[render][shader-sampler]") {
	using namespace engine::render;
	ShaderCompiler compiler;
	const auto compile = [&](std::string declaration, std::string sample) {
		return compiler.Compile(
			"#version 450\nlayout(location=0) out vec4 colour;\nlayout(set=2,binding=2) uniform " +
				declaration + ";\nvoid main(){colour=" + sample + ";}\n",
			ShaderStage::Fragment,
			"cooked-material.frag"
		);
	};
	const auto valid = compile("sampler2D colourMap", "texture(colourMap,vec2(.5))");
	REQUIRE_FALSE(valid.Failed);
	CHECK_FALSE(AdmitMaterialSamplers(valid.SpirV).has_value());
	const std::array<std::pair<std::string, std::string>, 6> incompatible{
		{{"samplerCube colourMap", "texture(colourMap,vec3(0,0,1))"},
		 {"sampler2DArray colourMap", "texture(colourMap,vec3(.5,.5,0))"},
		 {"sampler2DMS colourMap", "texelFetch(colourMap,ivec2(0),0)"},
		 {"sampler2DShadow colourMap", "vec4(texture(colourMap,vec3(.5,.5,.5)))"},
		 {"usampler2D colourMap", "vec4(texture(colourMap,vec2(.5)))"},
		 {"sampler2D colourMap[2]", "texture(colourMap[0],vec2(.5))"}}
	};
	for (const auto &[declaration, sample] : incompatible) {
		INFO(declaration);
		const auto compiled = compile(declaration, sample);
		INFO(compiled.Error);
		REQUIRE_FALSE(compiled.Failed);
		// Name and slot identity alone cannot establish compatibility with the actual draw binding.
		CHECK_FALSE(AdmitMaterialSamplers(compiled.Capabilities).has_value());
		const auto error = AdmitMaterialSamplers(compiled.SpirV);
		REQUIRE(error);
		CHECK(error->find("colourMap") != std::string::npos);
		CHECK(error->find("set 2 binding 2") != std::string::npos);
	}
}

TEST_CASE(
	"cooked material admission rejects invalid modules and unbound image writes", "[render][shader-sampler]"
) {
	using namespace engine::render;
	CHECK(AdmitMaterialSamplers(std::span<const uint32_t>{}).has_value());
	const std::array<uint32_t, 5> invalid{0, 0, 0, 0, 0};
	CHECK(AdmitMaterialSamplers(invalid).has_value());
	ShaderCompiler compiler;
	const auto vertex = compiler.Compile(
		"#version 450\nvoid main(){gl_Position=vec4(0);}", ShaderStage::Vertex, "wrong-stage.vert"
	);
	REQUIRE_FALSE(vertex.Failed);
	const auto vertexError = AdmitMaterialSamplers(vertex.SpirV);
	REQUIRE(vertexError);
	CHECK(vertexError->find("fragment") != std::string::npos);
	const auto storage = compiler.Compile(
		"#version 450\nlayout(location=0) out vec4 colour;\n"
		"layout(rgba8,set=2,binding=2) uniform image2D colourMap;\n"
		"void main(){imageStore(colourMap,ivec2(0),vec4(1));colour=vec4(1);}",
		ShaderStage::Fragment,
		"unbound-storage.frag"
	);
	REQUIRE_FALSE(storage.Failed);
	const auto error = AdmitMaterialSamplers(storage.SpirV);
	REQUIRE(error);
	CHECK(error->find("storage images") != std::string::npos);
}

TEST_CASE("material draw rejects descriptor classes absent from its bindings", "[render][shader-sampler]") {
	using namespace engine::render;
	ShaderCompiler compiler;
	const std::array<std::pair<std::string_view, std::string_view>, 4> fragments{
		{{"layout(set=2,binding=2)uniform texture2D colourTexture;layout(set=2,binding=3)uniform sampler "
		  "colourSampler;void main(){colour=texture(sampler2D(colourTexture,colourSampler),vec2(0));}",
		  "combined sampled image"},
		 {"layout(input_attachment_index=0,set=2,binding=2)uniform subpassInput colourMap;void "
		  "main(){colour=subpassLoad(colourMap);}",
		  "input attachments"},
		 {"layout(std430,set=2,binding=0)readonly buffer Data{float value;}data;void "
		  "main(){colour=vec4(data.value);}",
		  "storage buffers"},
		 {"layout(push_constant)uniform Data{float value;}data;void main(){colour=vec4(data.value);}",
		  "push constants"}}
	};
	for (const auto &[body, diagnostic] : fragments) {
		CAPTURE(diagnostic);
		const auto compiled = compiler.Compile(
			std::string("#version 450\nlayout(location=0)out vec4 colour;\n") + std::string(body),
			ShaderStage::Fragment,
			"unbound-descriptor.frag"
		);
		REQUIRE_FALSE(compiled.Failed);
		const auto error = AdmitMaterialSamplers(compiled.SpirV);
		REQUIRE(error);
		CHECK(error->find(diagnostic) != std::string::npos);
		CHECK(AdmitCookedMaterialInterface(compiled.SpirV));
	}
}

TEST_CASE(
	"cooked fragments reject atomic acceleration and standalone uniform descriptors",
	"[render][shader-sampler]"
) {
	using namespace engine::render;
	// Compiled CPU fixture: glslc --target-env=opengl -fshader-stage=frag fixture.frag -o fixture.spv
	// #version 450
	// layout(binding=0,offset=0) uniform atomic_uint counter;
	// layout(location=0) out vec4 colour;
	// void main(){colour=vec4(atomicCounterIncrement(counter));}
	const uint32_t atomic[] = {
		0x07230203u, 0x00010000u, 0x000d000bu, 0x00000012u, 0x00000000u, 0x00020011u, 0x00000001u,
		0x00020011u, 0x00000015u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu,
		0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0006000fu, 0x00000004u, 0x00000004u,
		0x6e69616du, 0x00000000u, 0x00000009u, 0x00030010u, 0x00000004u, 0x00000008u, 0x00030003u,
		0x00000002u, 0x000001c2u, 0x000a0004u, 0x475f4c47u, 0x4c474f4fu, 0x70635f45u, 0x74735f70u,
		0x5f656c79u, 0x656e696cu, 0x7269645fu, 0x69746365u, 0x00006576u, 0x00080004u, 0x475f4c47u,
		0x4c474f4fu, 0x6e695f45u, 0x64756c63u, 0x69645f65u, 0x74636572u, 0x00657669u, 0x00040005u,
		0x00000004u, 0x6e69616du, 0x00000000u, 0x00040005u, 0x00000009u, 0x6f6c6f63u, 0x00007275u,
		0x00040005u, 0x0000000cu, 0x6e756f63u, 0x00726574u, 0x00040047u, 0x00000009u, 0x0000001eu,
		0x00000000u, 0x00040047u, 0x0000000cu, 0x00000023u, 0x00000000u, 0x00040047u, 0x0000000cu,
		0x00000021u, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u,
		0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u,
		0x00040020u, 0x00000008u, 0x00000003u, 0x00000007u, 0x0004003bu, 0x00000008u, 0x00000009u,
		0x00000003u, 0x00040015u, 0x0000000au, 0x00000020u, 0x00000000u, 0x00040020u, 0x0000000bu,
		0x0000000au, 0x0000000au, 0x0004003bu, 0x0000000bu, 0x0000000cu, 0x0000000au, 0x0004002bu,
		0x0000000au, 0x0000000du, 0x00000001u, 0x0004002bu, 0x0000000au, 0x0000000eu, 0x00000000u,
		0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u,
		0x000600e8u, 0x0000000au, 0x0000000fu, 0x0000000cu, 0x0000000du, 0x0000000eu, 0x00040070u,
		0x00000006u, 0x00000010u, 0x0000000fu, 0x00070050u, 0x00000007u, 0x00000011u, 0x00000010u,
		0x00000010u, 0x00000010u, 0x00000010u, 0x0003003eu, 0x00000009u, 0x00000011u, 0x000100fdu,
		0x00010038u,
	};
	// Compiled CPU fixture: glslc --target-env=vulkan1.2 -fshader-stage=frag fixture.frag -o fixture.spv
	// #version 460
	// #extension GL_EXT_ray_query : require
	// layout(set=2,binding=0) uniform accelerationStructureEXT scene;
	// layout(location=0) out vec4 colour;
	// void main(){rayQueryEXT
	// query;rayQueryInitializeEXT(query,scene,gl_RayFlagsOpaqueEXT,255,vec3(0),0.0,vec3(0,0,1),100.0);while(rayQueryProceedEXT(query)){}
	// colour=vec4(float(rayQueryGetIntersectionTypeEXT(query,true)));}
	const uint32_t acceleration[] = {
		0x07230203u, 0x00010500u, 0x000d000bu, 0x00000027u, 0x00000000u, 0x00020011u, 0x00000001u,
		0x00020011u, 0x00001178u, 0x0006000au, 0x5f565053u, 0x5f52484bu, 0x5f796172u, 0x72657571u,
		0x00000079u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u,
		0x0003000eu, 0x00000000u, 0x00000001u, 0x0008000fu, 0x00000004u, 0x00000004u, 0x6e69616du,
		0x00000000u, 0x00000008u, 0x0000000bu, 0x00000020u, 0x00030010u, 0x00000004u, 0x00000007u,
		0x00030003u, 0x00000002u, 0x000001ccu, 0x00060004u, 0x455f4c47u, 0x725f5458u, 0x715f7961u,
		0x79726575u, 0x00000000u, 0x000a0004u, 0x475f4c47u, 0x4c474f4fu, 0x70635f45u, 0x74735f70u,
		0x5f656c79u, 0x656e696cu, 0x7269645fu, 0x69746365u, 0x00006576u, 0x00080004u, 0x475f4c47u,
		0x4c474f4fu, 0x6e695f45u, 0x64756c63u, 0x69645f65u, 0x74636572u, 0x00657669u, 0x00040005u,
		0x00000004u, 0x6e69616du, 0x00000000u, 0x00040005u, 0x00000008u, 0x72657571u, 0x00000079u,
		0x00040005u, 0x0000000bu, 0x6e656373u, 0x00000065u, 0x00040005u, 0x00000020u, 0x6f6c6f63u,
		0x00007275u, 0x00040047u, 0x0000000bu, 0x00000022u, 0x00000002u, 0x00040047u, 0x0000000bu,
		0x00000021u, 0x00000000u, 0x00040047u, 0x00000020u, 0x0000001eu, 0x00000000u, 0x00020013u,
		0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00021178u, 0x00000006u, 0x00040020u,
		0x00000007u, 0x00000006u, 0x00000006u, 0x0004003bu, 0x00000007u, 0x00000008u, 0x00000006u,
		0x000214ddu, 0x00000009u, 0x00040020u, 0x0000000au, 0x00000000u, 0x00000009u, 0x0004003bu,
		0x0000000au, 0x0000000bu, 0x00000000u, 0x00040015u, 0x0000000du, 0x00000020u, 0x00000000u,
		0x0004002bu, 0x0000000du, 0x0000000eu, 0x00000001u, 0x0004002bu, 0x0000000du, 0x0000000fu,
		0x000000ffu, 0x00030016u, 0x00000010u, 0x00000020u, 0x00040017u, 0x00000011u, 0x00000010u,
		0x00000003u, 0x0004002bu, 0x00000010u, 0x00000012u, 0x00000000u, 0x0006002cu, 0x00000011u,
		0x00000013u, 0x00000012u, 0x00000012u, 0x00000012u, 0x0004002bu, 0x00000010u, 0x00000014u,
		0x3f800000u, 0x0006002cu, 0x00000011u, 0x00000015u, 0x00000012u, 0x00000012u, 0x00000014u,
		0x0004002bu, 0x00000010u, 0x00000016u, 0x42c80000u, 0x00020014u, 0x0000001cu, 0x00040017u,
		0x0000001eu, 0x00000010u, 0x00000004u, 0x00040020u, 0x0000001fu, 0x00000003u, 0x0000001eu,
		0x0004003bu, 0x0000001fu, 0x00000020u, 0x00000003u, 0x00030029u, 0x0000001cu, 0x00000021u,
		0x00040015u, 0x00000022u, 0x00000020u, 0x00000001u, 0x0004002bu, 0x00000022u, 0x00000023u,
		0x00000001u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u,
		0x00000005u, 0x0004003du, 0x00000009u, 0x0000000cu, 0x0000000bu, 0x00091179u, 0x00000008u,
		0x0000000cu, 0x0000000eu, 0x0000000fu, 0x00000013u, 0x00000012u, 0x00000015u, 0x00000016u,
		0x000200f9u, 0x00000017u, 0x000200f8u, 0x00000017u, 0x000400f6u, 0x00000019u, 0x0000001au,
		0x00000000u, 0x000200f9u, 0x0000001bu, 0x000200f8u, 0x0000001bu, 0x0004117du, 0x0000001cu,
		0x0000001du, 0x00000008u, 0x000400fau, 0x0000001du, 0x00000018u, 0x00000019u, 0x000200f8u,
		0x00000018u, 0x000200f9u, 0x0000001au, 0x000200f8u, 0x0000001au, 0x000200f9u, 0x00000017u,
		0x000200f8u, 0x00000019u, 0x0005117fu, 0x0000000du, 0x00000024u, 0x00000008u, 0x00000023u,
		0x00040070u, 0x00000010u, 0x00000025u, 0x00000024u, 0x00070050u, 0x0000001eu, 0x00000026u,
		0x00000025u, 0x00000025u, 0x00000025u, 0x00000025u, 0x0003003eu, 0x00000020u, 0x00000026u,
		0x000100fdu, 0x00010038u,
	};
	// Compiled CPU fixture: glslc --target-env=opengl -fshader-stage=frag fixture.frag -o fixture.spv
	// #version 450
	// layout(location=0) uniform vec4 tint;
	// layout(location=0) out vec4 colour;
	// void main(){colour=tint;}
	const uint32_t plain[] = {
		0x07230203u, 0x00010000u, 0x000d000bu, 0x0000000du, 0x00000000u, 0x00020011u, 0x00000001u,
		0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu,
		0x00000000u, 0x00000001u, 0x0006000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u,
		0x00000009u, 0x00030010u, 0x00000004u, 0x00000008u, 0x00030003u, 0x00000002u, 0x000001c2u,
		0x000a0004u, 0x475f4c47u, 0x4c474f4fu, 0x70635f45u, 0x74735f70u, 0x5f656c79u, 0x656e696cu,
		0x7269645fu, 0x69746365u, 0x00006576u, 0x00080004u, 0x475f4c47u, 0x4c474f4fu, 0x6e695f45u,
		0x64756c63u, 0x69645f65u, 0x74636572u, 0x00657669u, 0x00040005u, 0x00000004u, 0x6e69616du,
		0x00000000u, 0x00040005u, 0x00000009u, 0x6f6c6f63u, 0x00007275u, 0x00040005u, 0x0000000bu,
		0x746e6974u, 0x00000000u, 0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u, 0x00040047u,
		0x0000000bu, 0x0000001eu, 0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u,
		0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u,
		0x00000004u, 0x00040020u, 0x00000008u, 0x00000003u, 0x00000007u, 0x0004003bu, 0x00000008u,
		0x00000009u, 0x00000003u, 0x00040020u, 0x0000000au, 0x00000000u, 0x00000007u, 0x0004003bu,
		0x0000000au, 0x0000000bu, 0x00000000u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u,
		0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003du, 0x00000007u, 0x0000000cu, 0x0000000bu,
		0x0003003eu, 0x00000009u, 0x0000000cu, 0x000100fdu, 0x00010038u,
	};

	const std::array<std::pair<std::span<const uint32_t>, std::string_view>, 3> fixtures{
		{{atomic, "atomic counters"},
		 {acceleration, "acceleration structures"},
		 {plain, "standalone uniforms"}}
	};
	for (const auto &[words, diagnostic] : fixtures) {
		CAPTURE(diagnostic);
		const auto error = AdmitMaterialSamplers(words);
		REQUIRE(error);
		CHECK(error->find(diagnostic) != std::string::npos);
		CHECK(AdmitCookedMaterialInterface(words));
	}
}

TEST_CASE("cooked material output uses the complete primary colour slot", "[render][shader-sampler]") {
	using namespace engine::render;
	ShaderCompiler compiler;
	const auto compiled = compiler.Compile(
		"#version 450\nlayout(location=0) out vec4 colour;void main(){colour=vec4(1);}",
		ShaderStage::Fragment,
		"primary-output.frag"
	);
	REQUIRE_FALSE(compiled.Failed);
	REQUIRE_FALSE(AdmitCookedMaterialInterface(compiled.SpirV));

	constexpr uint32_t OP_DECORATE = 71;
	constexpr uint32_t DECORATION_LOCATION = 30;
	constexpr uint32_t DECORATION_COMPONENT = 31;
	constexpr uint32_t DECORATION_INDEX = 32;
	const auto decorateOutput = [&](uint32_t decoration, uint32_t value) {
		auto words = compiled.SpirV;
		for (size_t instruction = 5; instruction < words.size();) {
			const uint32_t count = words[instruction] >> 16;
			REQUIRE(count > 0);
			REQUIRE(count <= words.size() - instruction);
			if ((words[instruction] & 0xffffu) == OP_DECORATE && count == 4 &&
				words[instruction + 2] == DECORATION_LOCATION) {
				const std::array<uint32_t, 4> added{
					(4u << 16) | OP_DECORATE, words[instruction + 1], decoration, value
				};
				words.insert(words.begin() + instruction, added.begin(), added.end());
				return words;
			}
			instruction += count;
		}
		FAIL("compiled colour output has no location decoration");
		return words;
	};
	for (const auto decoration : {DECORATION_INDEX, DECORATION_COMPONENT}) {
		CAPTURE(decoration);
		CHECK_FALSE(AdmitCookedMaterialInterface(decorateOutput(decoration, 0)));
		const auto error = AdmitCookedMaterialInterface(decorateOutput(decoration, 1));
		REQUIRE(error);
		CHECK(error->find("colour output") != std::string::npos);
	}
}
