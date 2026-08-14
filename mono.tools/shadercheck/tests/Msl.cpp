#include "SpirvBuilder.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <shadercheck/Msl.hpp>
#include <shadercheck/Spirv.hpp>
#include <string>
#include <vector>

TEST_SUITE_ID("tools.shadercheck.msl")

// The MSL here is written out rather than translated, for the reason
// `SpirvBuilder.hpp` gives about SPIR-V: every case worth checking is a
// translation that has gone wrong, and a suite that had to produce one would
// need SPIRV-Cross on its link line and a way to make it misbehave. Text in and
// findings out is the whole of what `CheckMsl` is.
//
// `GoodFragment()` is one sampled texture at set 2 binding 0 and one uniform
// buffer at set 3 binding 0, so its translation binds `[[texture(0)]]`,
// `[[sampler(0)]]` and `[[buffer(0)]]`.

using shadercheck::CheckMsl;
using shadercheck::Reflect;

namespace {

	using namespace shadercheck::testing;

	std::vector<shadercheck::Finding> Findings(const Builder &builder, const std::string &msl) {
		return CheckMsl(Reflect(builder.Words()), msl);
	}

	bool Mentions(const std::vector<shadercheck::Finding> &findings, const std::string &text) {
		for (const shadercheck::Finding &finding : findings) {
			if (finding.Message.find(text) != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	const std::string PREAMBLE = "#include <metal_stdlib>\n"
								 "#include <simd/simd.h>\n"
								 "\n"
								 "using namespace metal;\n"
								 "\n"
								 "struct main0_out { float4 outColour [[color(0)]]; };\n"
								 "struct main0_in { float2 inTexCoord [[user(locn0)]]; };\n"
								 "\n";

	// What `shadercross` emits for `GoodFragment()`, to the letter that matters.
	std::string GoodFragmentMsl() {
		return PREAMBLE + "fragment main0_out main0(main0_in in [[stage_in]], constant Flipbook& flipbook "
						  "[[buffer(0)]], texture2d<float> interfaceTexture [[texture(0)]], sampler "
						  "interfaceTextureSmplr [[sampler(0)]])\n"
						  "{\n"
						  "    main0_out out = {};\n"
						  "    return out;\n"
						  "}\n";
	}
}

TEST_CASE("a translation that agrees with its module has nothing to report", "[shadercheck]") {
	REQUIRE(Findings(GoodFragment(), GoodFragmentMsl()).empty());
}

TEST_CASE("an empty translation is a finding rather than a pass", "[shadercheck]") {
	REQUIRE(Mentions(Findings(GoodFragment(), ""), "wrote a file and no shader"));
}

TEST_CASE("a translation with no metal preamble is reported", "[shadercheck]") {
	std::string msl = GoodFragmentMsl();
	msl.replace(msl.find("using namespace metal;"), std::string("using namespace metal;").size(), "");

	REQUIRE(Mentions(Findings(GoodFragment(), msl), "Metal will not compile it"));
}

// The failure a truncated write produces, and the only thing "syntactically
// valid" can mean without a Metal compiler to ask.
TEST_CASE("an unbalanced translation is reported with a line number", "[shadercheck]") {
	std::string msl = GoodFragmentMsl();
	msl.resize(msl.size() - 3);

	const std::vector<shadercheck::Finding> findings = Findings(GoodFragment(), msl);
	REQUIRE(Mentions(findings, "does not parse"));
	REQUIRE(Mentions(findings, "never closed"));
}

// MSL reserves `main`, so a translation that kept it is one SDL cannot be asked
// for the entry point of.
TEST_CASE("an entry point that is not main0 is reported", "[shadercheck]") {
	std::string msl = GoodFragmentMsl();
	msl.replace(msl.find("main0("), std::string("main0(").size(), "main(");

	REQUIRE(Mentions(Findings(GoodFragment(), msl), "asks for 'main0'"));
}

TEST_CASE("a translation qualified for the wrong stage is reported", "[shadercheck]") {
	std::string msl = GoodFragmentMsl();
	msl.replace(msl.find("fragment main0_out"), std::string("fragment").size(), "vertex  ");

	REQUIRE(Mentions(Findings(GoodFragment(), msl), "not qualified `fragment`"));
}

TEST_CASE("two entry points in one translation are reported", "[shadercheck]") {
	const std::string msl = GoodFragmentMsl() + "\nvertex void main0()\n{\n}\n";

	REQUIRE(Mentions(Findings(GoodFragment(), msl), "declares 2 entry points"));
}

// The one that would otherwise be silent. Every texture is bound and none of
// them to the resource the engine thinks it bound, which on a Mac is a surface
// sampling its neighbour's map.
TEST_CASE("a resource on the wrong metal index is reported", "[shadercheck]") {
	Builder builder;
	builder.Instruction(OpCapability, {CapabilityShader});
	builder.Entry(ModelFragment, "main");
	builder.SampledTexture(10, 2, 0, "colourMap");
	builder.SampledTexture(11, 2, 1, "surfaceMap");

	const std::string msl = PREAMBLE + "fragment main0_out main0(texture2d<float> colourMap [[texture(1)]], "
									   "texture2d<float> surfaceMap [[texture(0)]])\n"
									   "{\n"
									   "}\n";

	const std::vector<shadercheck::Finding> findings = CheckMsl(Reflect(builder.Words()), msl);
	REQUIRE(Mentions(findings, "'colourMap' is at [[texture(1)]] and belongs at [[texture(0)]]"));
	REQUIRE(Mentions(findings, "'surfaceMap' is at [[texture(0)]] and belongs at [[texture(1)]]"));
}

TEST_CASE("a sampler carries the index of the texture it belongs to", "[shadercheck]") {
	std::string msl = GoodFragmentMsl();
	msl.replace(msl.find("[[sampler(0)]]"), std::string("[[sampler(0)]]").size(), "[[sampler(2)]]");

	REQUIRE(Mentions(Findings(GoodFragment(), msl), "'interfaceTextureSmplr' is at [[sampler(2)]]"));
}

// A translation that renamed a resource is indistinguishable from one that
// dropped it, unless the question is asked from the emitted side.
TEST_CASE("a binding the module never declared is reported", "[shadercheck]") {
	std::string msl = GoodFragmentMsl();
	msl.replace(msl.find("interfaceTexture "), std::string("interfaceTexture ").size(), "somethingElse ");

	REQUIRE(Mentions(Findings(GoodFragment(), msl), "is not a resource the SPIR-V declares"));
}

// A resource declared and never sampled is dropped by the translator, and that
// is correct rather than a gap: `unlit.frag` declares four textures and reads
// one, and Metal binds by index whatever the shader names.
TEST_CASE("a resource the translation dropped is not a finding", "[shadercheck]") {
	const std::string msl = PREAMBLE + "fragment main0_out main0(constant Flipbook& flipbook [[buffer(0)]])\n"
									   "{\n"
									   "}\n";

	REQUIRE(CheckMsl(Reflect(GoodFragment().Words()), msl).empty());
}

// A `fragment` in a comment is not a second entry point, and a `[[texture(9)]]`
// in one is not a binding. Both are what the blanking pass is for.
TEST_CASE("comments are not read as code", "[shadercheck]") {
	std::string msl = GoodFragmentMsl();
	msl += "// fragment main0_out main0(texture2d<float> ghost [[texture(9)]])\n";
	msl += "/* fragment void main0() { */\n";

	REQUIRE(Findings(GoodFragment(), msl).empty());
}
