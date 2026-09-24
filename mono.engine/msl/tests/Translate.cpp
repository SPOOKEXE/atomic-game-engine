#include "fixtures/Fragment.hpp"

#include <engine/msl/Translate.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.msl.translate")

using engine::msl::Translate;
using engine::msl::Translation;

namespace {

	// One compiled fragment shader, and the GLSL it came from.
	//
	// **A fixture rather than an assembled instruction stream.** SPIRV-Cross
	// needs a complete module - types, a function body, a return - so the
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
	constexpr std::span<const uint32_t> FRAGMENT = engine::msl::testdata::FRAGMENT;

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
// id order, which for this shader puts `colourMap` at [[texture(0)]] - and SDL's
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
