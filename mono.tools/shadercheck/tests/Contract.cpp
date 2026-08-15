#include "SpirvBuilder.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <shadercheck/Contract.hpp>
#include <shadercheck/Spirv.hpp>
#include <string>
#include <vector>

TEST_SUITE_ID("tools.shadercheck.contract")

using shadercheck::Check;
using shadercheck::MetalIndices;
using shadercheck::Reflect;
using shadercheck::Stage;

namespace {

	using namespace shadercheck::testing;

	std::vector<shadercheck::Finding> Findings(const Builder &builder, Stage expected) {
		return Check(Reflect(builder.Words()), expected);
	}

	// Every finding is a sentence somebody reads in a build log, so the suites
	// match on the words that make it useful rather than on the whole string.
	bool Mentions(const std::vector<shadercheck::Finding> &findings, const std::string &text) {
		for (const shadercheck::Finding &finding : findings) {
			if (finding.Message.find(text) != std::string::npos) {
				return true;
			}
		}
		return false;
	}
}

TEST_CASE("a shader in the sets SDL documents has nothing to report", "[shadercheck]") {
	REQUIRE(Findings(GoodFragment(), Stage::Fragment).empty());

	Builder vertex;
	vertex.Instruction(OpCapability, {CapabilityShader});
	vertex.Entry(ModelVertex, "main");
	vertex.UniformBuffer(20, 1, 0, "frame");
	REQUIRE(Findings(vertex, Stage::Vertex).empty());
}

// The bug this catches does not look like a binding bug. A fragment shader
// authored against a vertex shader's sets binds nothing at all, and the symptom
// is a black draw with no error anywhere.
TEST_CASE("a resource in another stage's descriptor set is reported", "[shadercheck]") {
	Builder builder;
	builder.Instruction(OpCapability, {CapabilityShader});
	builder.Entry(ModelFragment, "main");
	builder.SampledTexture(10, 0, 0, "wrongSet");

	REQUIRE(Mentions(Findings(builder, Stage::Fragment), "belongs in set 2"));
}

TEST_CASE("a uniform buffer in the texture set is reported", "[shadercheck]") {
	Builder builder;
	builder.Instruction(OpCapability, {CapabilityShader});
	builder.Entry(ModelFragment, "main");
	builder.UniformBuffer(20, 2, 0, "flipbook");

	REQUIRE(Mentions(Findings(builder, Stage::Fragment), "belongs in set 3"));
}

TEST_CASE("a resource with no layout qualifier is reported", "[shadercheck]") {
	Builder builder;
	builder.Instruction(OpCapability, {CapabilityShader});
	builder.Entry(ModelVertex, "main");
	builder.UndecoratedUniformBuffer(20, "frame");

	REQUIRE(Mentions(Findings(builder, Stage::Vertex), "no explicit layout"));
}

// Contiguity is what makes the translated index space match the SPIR-V one, so
// this is the rule that keeps `MetalIndices` from being a guess.
TEST_CASE("a gap in a set's bindings is reported", "[shadercheck]") {
	Builder builder;
	builder.Instruction(OpCapability, {CapabilityShader});
	builder.Entry(ModelFragment, "main");
	builder.SampledTexture(10, 2, 0, "first");
	builder.SampledTexture(20, 2, 2, "third");

	REQUIRE(Mentions(Findings(builder, Stage::Fragment), "jumps from binding 1 to 2"));
}

TEST_CASE("a set that does not start at binding zero is reported", "[shadercheck]") {
	Builder builder;
	builder.Instruction(OpCapability, {CapabilityShader});
	builder.Entry(ModelFragment, "main");
	builder.SampledTexture(10, 2, 1, "only");

	REQUIRE(Mentions(Findings(builder, Stage::Fragment), "jumps from binding 0 to 1"));
}

TEST_CASE("a set holding its kinds out of order is reported", "[shadercheck]") {
	Builder builder;
	builder.Instruction(OpCapability, {CapabilityShader});
	builder.Entry(ModelFragment, "main");
	builder.StorageBuffer(10, 2, 0, "storage");
	builder.SampledTexture(20, 2, 1, "texture");

	REQUIRE(Mentions(Findings(builder, Stage::Fragment), "comes after a storage buffer"));
}

TEST_CASE("a module whose stage disagrees with its filename is reported", "[shadercheck]") {
	// The same module, checked as what the name on disk claims it is.
	REQUIRE(Mentions(Findings(GoodFragment(), Stage::Vertex), "its name says vertex"));
}

TEST_CASE("a stage SDL's GPU API does not have is reported", "[shadercheck]") {
	Builder builder;
	builder.Instruction(OpCapability, {CapabilityGeometry});
	builder.Entry(ModelGeometry, "main");

	REQUIRE(Mentions(Findings(builder, Stage::Vertex), "a stage SDL's GPU API does not have"));
}

TEST_CASE("more than one entry point is reported and stops the rest", "[shadercheck]") {
	Builder builder;
	builder.Instruction(OpCapability, {CapabilityShader});
	builder.Entry(ModelFragment, "main");
	builder.Entry(ModelFragment, "second");

	const std::vector<shadercheck::Finding> findings = Findings(builder, Stage::Fragment);
	REQUIRE(findings.size() == 1);
	REQUIRE(Mentions(findings, "has 2 entry points"));
}

TEST_CASE("an entry point the renderer does not ask for is reported", "[shadercheck]") {
	Builder builder;
	builder.Instruction(OpCapability, {CapabilityShader});
	builder.Entry(ModelFragment, "main0");

	REQUIRE(Mentions(Findings(builder, Stage::Fragment), "entry point is 'main0'"));
}

// The capability list is an allowlist, and this is why: `double` in GLSL
// compiles to SPIR-V that runs on Vulkan and cannot be expressed in MSL at all.
TEST_CASE("a capability Metal cannot express is refused by name", "[shadercheck]") {
	Builder builder;
	builder.Instruction(OpCapability, {CapabilityShader});
	builder.Instruction(OpCapability, {CapabilityFloat64});
	builder.Entry(ModelFragment, "main");

	const std::vector<shadercheck::Finding> findings = Findings(builder, Stage::Fragment);
	REQUIRE(Mentions(findings, "Float64"));
	REQUIRE(Mentions(findings, "MSL has no double"));
}

TEST_CASE("a module that will not parse reports the parse failure and nothing else", "[shadercheck]") {
	const shadercheck::Module module = Reflect(std::vector<uint32_t>{0x12345678u, 0, 0, 0, 0});
	const std::vector<shadercheck::Finding> findings = Check(module, Stage::Fragment);

	REQUIRE(findings.size() == 1);
	REQUIRE(Mentions(findings, "not SPIR-V"));
}

// The derived half. Nothing here proves a Mac accepts the result; it proves the
// assignment is a property of the SPIR-V and can therefore be read from a
// machine that has no Metal.
TEST_CASE("textures and buffers are numbered in separate spaces", "[shadercheck]") {
	Builder builder;
	builder.Instruction(OpCapability, {CapabilityShader});
	builder.Entry(ModelFragment, "main");
	builder.SampledTexture(10, 2, 0, "albedo");
	builder.SampledTexture(20, 2, 1, "shadow");
	builder.StorageBuffer(30, 2, 2, "instances");
	builder.UniformBuffer(40, 3, 0, "frame");

	const shadercheck::Module module = Reflect(builder.Words());
	REQUIRE(Check(module, Stage::Fragment).empty());

	const std::vector<uint32_t> indices = MetalIndices(module.Resources);
	REQUIRE(indices.size() == 4);
	// Sorted by set then binding, so the order is albedo, shadow, instances, frame.
	REQUIRE(indices[0] == 0); // [[texture(0)]]
	REQUIRE(indices[1] == 1); // [[texture(1)]]
	// Uniform buffers come before storage buffers whatever set they sit in,
	// which is the one place the Metal order is not the descriptor order.
	REQUIRE(indices[2] == 1); // [[buffer(1)]]
	REQUIRE(indices[3] == 0); // [[buffer(0)]]
}
