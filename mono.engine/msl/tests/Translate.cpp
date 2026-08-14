#include <engine/msl/Translate.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.msl.translate")

using engine::msl::Translate;
using engine::msl::Translation;

namespace {

	// One compiled fragment shader, and the GLSL it came from.
	//
	// **A fixture rather than an assembled instruction stream.** SPIRV-Cross
	// needs a complete module — types, a function body, a return — so the
	// word-by-word builder `mono.tools/shadercheck` uses for its negative cases
	// produces nothing this can translate. What matters about a fixture is that
	// its provenance is beside it rather than in somebody's shell history:
	//
	//     #version 450
	//
	//     layout(set = 2, binding = 0) uniform sampler2D shadowMap;
	//     layout(set = 2, binding = 1) uniform sampler2D colourMap;
	//     layout(set = 3, binding = 0) uniform Material { vec4 Tint; } material;
	//
	//     layout(location = 0) in vec2 inTexCoord;
	//     layout(location = 0) out vec4 outColour;
	//
	//     void main() {
	//         outColour = texture(colourMap, inTexCoord) * material.Tint
	//                   + texture(shadowMap, inTexCoord);
	//     }
	//
	// Regenerate with:
	//
	//     .cache/build/dev/mono.vendor/shaderc/glslc/glslc fixture.frag -o fixture.spv
	//
	// The two textures are what make it worth having. One of each kind cannot
	// tell SDL's ordering apart from SPIRV-Cross's own, and two can: `shadowMap`
	// is first in the descriptor set and has to be first in the texture space.
	const std::vector<uint32_t> FRAGMENT = {
		0x07230203u, 0x00010000u, 0x000d000bu, 0x00000022u, 0x00000000u, 0x00020011u, 0x00000001u,
		0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu,
		0x00000000u, 0x00000001u, 0x0007000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u,
		0x00000009u, 0x00000011u, 0x00030010u, 0x00000004u, 0x00000007u, 0x00030003u, 0x00000002u,
		0x000001c2u, 0x000a0004u, 0x475f4c47u, 0x4c474f4fu, 0x70635f45u, 0x74735f70u, 0x5f656c79u,
		0x656e696cu, 0x7269645fu, 0x69746365u, 0x00006576u, 0x00080004u, 0x475f4c47u, 0x4c474f4fu,
		0x6e695f45u, 0x64756c63u, 0x69645f65u, 0x74636572u, 0x00657669u, 0x00040005u, 0x00000004u,
		0x6e69616du, 0x00000000u, 0x00050005u, 0x00000009u, 0x4374756fu, 0x756f6c6fu, 0x00000072u,
		0x00050005u, 0x0000000du, 0x6f6c6f63u, 0x614d7275u, 0x00000070u, 0x00050005u, 0x00000011u,
		0x65546e69u, 0x6f6f4378u, 0x00006472u, 0x00050005u, 0x00000014u, 0x6574614du, 0x6c616972u,
		0x00000000u, 0x00050006u, 0x00000014u, 0x00000000u, 0x746e6954u, 0x00000000u, 0x00050005u,
		0x00000016u, 0x6574616du, 0x6c616972u, 0x00000000u, 0x00050005u, 0x0000001du, 0x64616873u,
		0x614d776fu, 0x00000070u, 0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u, 0x00040047u,
		0x0000000du, 0x00000021u, 0x00000001u, 0x00040047u, 0x0000000du, 0x00000022u, 0x00000002u,
		0x00040047u, 0x00000011u, 0x0000001eu, 0x00000000u, 0x00030047u, 0x00000014u, 0x00000002u,
		0x00050048u, 0x00000014u, 0x00000000u, 0x00000023u, 0x00000000u, 0x00040047u, 0x00000016u,
		0x00000021u, 0x00000000u, 0x00040047u, 0x00000016u, 0x00000022u, 0x00000003u, 0x00040047u,
		0x0000001du, 0x00000021u, 0x00000000u, 0x00040047u, 0x0000001du, 0x00000022u, 0x00000002u,
		0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u,
		0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040020u, 0x00000008u,
		0x00000003u, 0x00000007u, 0x0004003bu, 0x00000008u, 0x00000009u, 0x00000003u, 0x00090019u,
		0x0000000au, 0x00000006u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u,
		0x00000000u, 0x0003001bu, 0x0000000bu, 0x0000000au, 0x00040020u, 0x0000000cu, 0x00000000u,
		0x0000000bu, 0x0004003bu, 0x0000000cu, 0x0000000du, 0x00000000u, 0x00040017u, 0x0000000fu,
		0x00000006u, 0x00000002u, 0x00040020u, 0x00000010u, 0x00000001u, 0x0000000fu, 0x0004003bu,
		0x00000010u, 0x00000011u, 0x00000001u, 0x0003001eu, 0x00000014u, 0x00000007u, 0x00040020u,
		0x00000015u, 0x00000002u, 0x00000014u, 0x0004003bu, 0x00000015u, 0x00000016u, 0x00000002u,
		0x00040015u, 0x00000017u, 0x00000020u, 0x00000001u, 0x0004002bu, 0x00000017u, 0x00000018u,
		0x00000000u, 0x00040020u, 0x00000019u, 0x00000002u, 0x00000007u, 0x0004003bu, 0x0000000cu,
		0x0000001du, 0x00000000u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
		0x000200f8u, 0x00000005u, 0x0004003du, 0x0000000bu, 0x0000000eu, 0x0000000du, 0x0004003du,
		0x0000000fu, 0x00000012u, 0x00000011u, 0x00050057u, 0x00000007u, 0x00000013u, 0x0000000eu,
		0x00000012u, 0x00050041u, 0x00000019u, 0x0000001au, 0x00000016u, 0x00000018u, 0x0004003du,
		0x00000007u, 0x0000001bu, 0x0000001au, 0x00050085u, 0x00000007u, 0x0000001cu, 0x00000013u,
		0x0000001bu, 0x0004003du, 0x0000000bu, 0x0000001eu, 0x0000001du, 0x0004003du, 0x0000000fu,
		0x0000001fu, 0x00000011u, 0x00050057u, 0x00000007u, 0x00000020u, 0x0000001eu, 0x0000001fu,
		0x00050081u, 0x00000007u, 0x00000021u, 0x0000001cu, 0x00000020u, 0x0003003eu, 0x00000009u,
		0x00000021u, 0x000100fdu, 0x00010038u,
	};

	bool Mentions(const std::string &text, const std::string &part) {
		return text.find(part) != std::string::npos;
	}
}

TEST_CASE("a compiled module translates to MSL", "[msl]") {
	const Translation translation = Translate(FRAGMENT);

	REQUIRE_FALSE(translation.Failed);
	REQUIRE(translation.Error.empty());
	REQUIRE(Mentions(translation.Source, "#include <metal_stdlib>"));
	REQUIRE(Mentions(translation.Source, "using namespace metal;"));
}

// MSL reserves `main`. Every caller of SDL_CreateGPUShader on this format has to
// ask for the name the translation produced, which is why the constant is
// beside the function rather than typed out at each call site.
TEST_CASE("the entry point is renamed to main0", "[msl]") {
	const Translation translation = Translate(FRAGMENT);

	REQUIRE(Mentions(translation.Source, "fragment main0_out main0("));
	REQUIRE(Mentions(translation.Source, engine::msl::ENTRY_POINT));
}

// The reason this module exists. SPIRV-Cross left to itself numbers resources in
// id order, which for this shader puts `colourMap` at [[texture(0)]] — and SDL's
// Metal backend binds the shadow map there.
TEST_CASE("resources land on the indices SDL_CreateGPUShader documents", "[msl]") {
	const Translation translation = Translate(FRAGMENT);

	REQUIRE(Mentions(translation.Source, "shadowMap [[texture(0)]]"));
	REQUIRE(Mentions(translation.Source, "colourMap [[texture(1)]]"));
	REQUIRE(Mentions(translation.Source, "shadowMapSmplr [[sampler(0)]]"));
	REQUIRE(Mentions(translation.Source, "colourMapSmplr [[sampler(1)]]"));
	REQUIRE(Mentions(translation.Source, "material [[buffer(0)]]"));
}

// SPIRV-Cross reports by exception and one caller is a frame that has to keep
// running. A failure is a string here or it is a crash there.
TEST_CASE("bytes that are not SPIR-V are a diagnostic and not a throw", "[msl]") {
	const std::vector<uint32_t> rubbish(64, 0xdeadbeefu);
	const Translation translation = Translate(rubbish);

	REQUIRE(translation.Failed);
	REQUIRE_FALSE(translation.Error.empty());
	REQUIRE(translation.Source.empty());
}

TEST_CASE("an empty module is refused rather than translated", "[msl]") {
	const Translation translation = Translate({});

	REQUIRE(translation.Failed);
	REQUIRE(translation.Error == "no SPIR-V to translate");
}
