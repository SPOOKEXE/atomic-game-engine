#include <engine/render/ShaderCompiler.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.render.shadercompiler")

using engine::render::ShaderCompilation;
using engine::render::ShaderCompiler;
using engine::render::ShaderStage;

namespace {
	constexpr const char *VALID_FRAGMENT = R"(#version 450
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColour;
layout(set = 2, binding = 0) uniform sampler2D SceneColour;
void main() { outColour = vec4(texture(SceneColour, inUv).rgb * 0.5, 1.0); }
)";

	constexpr const char *VALID_VERTEX = R"(#version 450
layout(location = 0) out vec2 outUv;
void main() {
	outUv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(outUv * 2.0 - 1.0, 0.0, 1.0);
}
)";

	// The first word of a SPIR-V module. Checking it is what distinguishes
	// "returned some bytes" from "returned SPIR-V".
	constexpr uint32_t SPIRV_MAGIC = 0x07230203u;
}

TEST_CASE("a valid fragment shader compiles to SPIR-V", "[shaderc]") {
	ShaderCompiler compiler;
	const ShaderCompilation result = compiler.Compile(VALID_FRAGMENT, ShaderStage::Fragment, "valid.frag");

	REQUIRE_FALSE(result.Failed);
	REQUIRE(result.Error.empty());
	REQUIRE_FALSE(result.SpirV.empty());
	REQUIRE(result.SpirV.front() == SPIRV_MAGIC);
}

TEST_CASE("a malformed shader produces a non-empty error", "[shaderc]") {
	ShaderCompiler compiler;
	const ShaderCompilation result = compiler.Compile(
		"#version 450\nvoid main() { this is not glsl }\n", ShaderStage::Fragment, "broken.frag"
	);

	// This is the assertion that matters, and RENDER_PIPELINE.md §11.9.1 says
	// so directly: an empty error on invalid input means nothing compiled it.
	// A stub that reports success unconditionally passes every other test in
	// this file and fails only this one.
	REQUIRE(result.Failed);
	REQUIRE_FALSE(result.Error.empty());
	REQUIRE(result.SpirV.empty());
}

TEST_CASE("the diagnostic names the shader and the line", "[shaderc]") {
	ShaderCompiler compiler;
	const ShaderCompilation result = compiler.Compile(
		"#version 450\nvoid main() {\n\tundeclared_function();\n}\n", ShaderStage::Fragment, "named.frag"
	);

	REQUIRE(result.Failed);
	// The name is what a script author sees to locate the failure, so it has
	// to survive into the message rather than being a label we drop.
	REQUIRE(result.Error.find("named.frag") != std::string::npos);
	REQUIRE(result.Error.find("3") != std::string::npos);
}

TEST_CASE("each stage compiles its own kind", "[shaderc]") {
	ShaderCompiler compiler;

	REQUIRE_FALSE(compiler.Compile(VALID_VERTEX, ShaderStage::Vertex, "v").Failed);
	REQUIRE_FALSE(compiler.Compile(VALID_FRAGMENT, ShaderStage::Fragment, "f").Failed);
	REQUIRE_FALSE(
		compiler
			.Compile(
				"#version 450\nlayout(local_size_x = 8) in;\nvoid main() {}\n", ShaderStage::Compute, "c"
			)
			.Failed
	);
}

TEST_CASE("the stage is honoured, not ignored", "[shaderc]") {
	ShaderCompiler compiler;

	// Deliberately not "the fragment shader as a vertex shader": that pair
	// compiles, because a `in vec2` / `out vec4` pass-through is legal GLSL in
	// either stage. The first version of this test asserted otherwise and was
	// wrong about the language rather than about the compiler.
	//
	// These two are genuinely stage-specific. If the stage were being dropped
	// on the way through, both would compile and the mismatch would surface
	// later as a pipeline that will not create - much further from the cause.
	REQUIRE(compiler.Compile(VALID_VERTEX, ShaderStage::Fragment, "no-gl-position").Failed);
	REQUIRE(
		compiler.Compile("#version 450\nvoid main() { discard; }\n", ShaderStage::Vertex, "no-discard").Failed
	);
}

TEST_CASE("an empty source fails rather than returning nothing", "[shaderc]") {
	ShaderCompiler compiler;
	const ShaderCompilation result = compiler.Compile("", ShaderStage::Fragment, "empty");

	REQUIRE(result.Failed);
	REQUIRE_FALSE(result.Error.empty());
}

TEST_CASE("optimising changes the output but not the outcome", "[shaderc]") {
	ShaderCompiler compiler;
	const ShaderCompilation unoptimised = compiler.Compile(VALID_FRAGMENT, ShaderStage::Fragment, "f");

	compiler.SetOptimise(true);
	const ShaderCompilation optimised = compiler.Compile(VALID_FRAGMENT, ShaderStage::Fragment, "f");

	REQUIRE_FALSE(optimised.Failed);
	REQUIRE(optimised.SpirV.front() == SPIRV_MAGIC);
	// Optimisation is worth having only if it does something.
	REQUIRE(optimised.SpirV.size() < unoptimised.SpirV.size());
}

TEST_CASE("one compiler can be reused", "[shaderc]") {
	// Construction builds glslang's tables; doing it per shader would make a
	// live edit noticeably slower than the compile it is paying for.
	ShaderCompiler compiler;

	for (int index = 0; index < 8; index++) {
		REQUIRE_FALSE(compiler.Compile(VALID_FRAGMENT, ShaderStage::Fragment, "f").Failed);
		REQUIRE(compiler.Compile("garbage", ShaderStage::Fragment, "g").Failed);
	}
}
