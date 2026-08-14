#include "SpirvBuilder.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <shadercheck/Spirv.hpp>
#include <vector>

TEST_SUITE_ID("tools.shadercheck.spirv")

using shadercheck::Reflect;
using shadercheck::ReflectBytes;
using shadercheck::ResourceKind;
using shadercheck::Stage;

namespace {

	using namespace shadercheck::testing;

	std::vector<std::byte> Bytes(const std::vector<uint32_t> &words, bool reversed = false) {
		std::vector<std::byte> bytes;
		bytes.reserve(words.size() * 4);
		for (const uint32_t word : words) {
			for (int shift = 0; shift < 4; ++shift) {
				const int byte = reversed ? 3 - shift : shift;
				bytes.push_back(static_cast<std::byte>((word >> (byte * 8)) & 0xFFu));
			}
		}
		return bytes;
	}
}

TEST_CASE("a file that is not SPIR-V is named as one, not parsed as one", "[shadercheck]") {
	const std::vector<uint32_t> notSpirv = {0x12345678u, 0, 0, 0, 0};
	REQUIRE_FALSE(Reflect(notSpirv).Parsed());

	// Shorter than the header. A five-word read on a four-word file is how a
	// reader of binaries produces a plausible answer from nothing.
	REQUIRE_FALSE(Reflect(std::vector<uint32_t>{0x07230203u, 0, 0, 0}).Parsed());
}

TEST_CASE("an instruction running past the end is truncation, not a resource", "[shadercheck]") {
	Builder builder;
	builder.Entry(ModelFragment, "main");
	std::vector<uint32_t> words = builder.Words();
	// Claim forty words for an instruction with four left after it.
	words.push_back((40u << 16) | OpCapability);
	words.push_back(CapabilityShader);

	REQUIRE_FALSE(Reflect(words).Parsed());
}

TEST_CASE("the entry point's name and stage are read out of the module", "[shadercheck]") {
	Builder builder;
	builder.Entry(ModelVertex, "main");

	const shadercheck::Module module = Reflect(builder.Words());
	REQUIRE(module.Parsed());
	REQUIRE(module.EntryPointName == "main");
	REQUIRE(module.EntryStage == Stage::Vertex);
	REQUIRE(module.EntryPointCount == 1);
}

// A stage SDL's GPU API has no slot for is a value rather than a parse failure,
// because the module is valid SPIR-V and the finding belongs to the contract.
TEST_CASE("a geometry entry point parses and reports an unsupported stage", "[shadercheck]") {
	Builder builder;
	builder.Entry(ModelGeometry, "main");

	const shadercheck::Module module = Reflect(builder.Words());
	REQUIRE(module.Parsed());
	REQUIRE(module.EntryStage == Stage::Unsupported);
}

TEST_CASE("two entry points are counted rather than the second one ignored", "[shadercheck]") {
	Builder builder;
	builder.Entry(ModelVertex, "main");
	builder.Entry(ModelFragment, "other");

	const shadercheck::Module module = Reflect(builder.Words());
	REQUIRE(module.EntryPointCount == 2);
	// The first is kept, so a report can say which name it saw.
	REQUIRE(module.EntryPointName == "main");
}

TEST_CASE("a combined image sampler is a sampled texture at its set and binding", "[shadercheck]") {
	Builder builder;
	builder.Entry(ModelFragment, "main");
	builder.SampledTexture(10, 2, 0, "interfaceTexture");

	const shadercheck::Module module = Reflect(builder.Words());
	REQUIRE(module.Resources.size() == 1);
	REQUIRE(module.Resources[0].Kind == ResourceKind::SampledTexture);
	REQUIRE(module.Resources[0].Name == "interfaceTexture");
	REQUIRE(module.Resources[0].Set == 2);
	REQUIRE(module.Resources[0].Binding == 0);
	REQUIRE(module.Resources[0].HasSet);
	REQUIRE(module.Resources[0].HasBinding);
}

TEST_CASE("a Block struct behind a Uniform variable is a uniform buffer", "[shadercheck]") {
	Builder builder;
	builder.Entry(ModelVertex, "main");
	builder.UniformBuffer(20, 1, 0, "frame");

	const shadercheck::Module module = Reflect(builder.Words());
	REQUIRE(module.Resources.size() == 1);
	REQUIRE(module.Resources[0].Kind == ResourceKind::UniformBuffer);
	REQUIRE(module.Resources[0].Set == 1);
}

// The failure this distinction exists for: GLSL without a `layout` qualifier
// compiles, and the missing decoration is invisible in the source.
TEST_CASE("a resource with no layout qualifier keeps no set or binding", "[shadercheck]") {
	Builder builder;
	builder.Entry(ModelVertex, "main");
	builder.UndecoratedUniformBuffer(20, "frame");

	const shadercheck::Module module = Reflect(builder.Words());
	REQUIRE(module.Resources.size() == 1);
	REQUIRE_FALSE(module.Resources[0].HasSet);
	REQUIRE_FALSE(module.Resources[0].HasBinding);
}

TEST_CASE("resources come back ordered by set and then by binding", "[shadercheck]") {
	Builder builder;
	builder.Entry(ModelFragment, "main");
	builder.UniformBuffer(30, 3, 0, "third");
	builder.SampledTexture(20, 2, 1, "second");
	builder.SampledTexture(10, 2, 0, "first");

	const shadercheck::Module module = Reflect(builder.Words());
	REQUIRE(module.Resources.size() == 3);
	REQUIRE(module.Resources[0].Name == "first");
	REQUIRE(module.Resources[1].Name == "second");
	REQUIRE(module.Resources[2].Name == "third");
}

TEST_CASE("capabilities are kept in the order the module declares them", "[shadercheck]") {
	Builder builder;
	builder.Instruction(OpCapability, {CapabilityShader});
	builder.Instruction(OpCapability, {CapabilityFloat64});
	builder.Entry(ModelFragment, "main");

	const shadercheck::Module module = Reflect(builder.Words());
	REQUIRE(module.Capabilities.size() == 2);
	REQUIRE(module.Capabilities[0] == CapabilityShader);
	REQUIRE(module.Capabilities[1] == CapabilityFloat64);
}

TEST_CASE("bytes are read either way round", "[shadercheck]") {
	const Builder builder = GoodFragment();

	const shadercheck::Module native = ReflectBytes(Bytes(builder.Words()));
	const shadercheck::Module reversed = ReflectBytes(Bytes(builder.Words(), true));

	REQUIRE(native.Parsed());
	REQUIRE(reversed.Parsed());
	REQUIRE(reversed.EntryPointName == native.EntryPointName);
	REQUIRE(reversed.Resources.size() == native.Resources.size());
}

TEST_CASE("a file that is not a whole number of words is not a module", "[shadercheck]") {
	std::vector<std::byte> bytes = Bytes(GoodFragment().Words());
	bytes.pop_back();
	REQUIRE_FALSE(ReflectBytes(bytes).Parsed());
}
