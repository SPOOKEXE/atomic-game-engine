#include <engine/assets/Shader.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

TEST_SUITE_ID("engine.assets.shader")
TEST_DEPENDS("engine.assets.contenthash")

using namespace engine::assets;
using engine::core::ByteReader;
using engine::core::ByteWriter;

namespace {
	// glslc -fshader-stage=frag -O --target-env=vulkan1.0 fixture.frag -o fixture.spv
	// #version 450
	// layout(location = 0) out vec4 colour;
	// void main() { colour = vec4(1, 0, 0, 1); }
	constexpr std::array COMPILED_FRAGMENT{
		0x07230203u, 0x00010000u, 0x000d000bu, 0x0000000du, 0x00000000u, 0x00020011u, 0x00000001u,
		0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu,
		0x00000000u, 0x00000001u, 0x0006000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u,
		0x00000009u, 0x00030010u, 0x00000004u, 0x00000007u, 0x00040047u, 0x00000009u, 0x0000001eu,
		0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u,
		0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040020u,
		0x00000008u, 0x00000003u, 0x00000007u, 0x0004003bu, 0x00000008u, 0x00000009u, 0x00000003u,
		0x0004002bu, 0x00000006u, 0x0000000au, 0x3f800000u, 0x0004002bu, 0x00000006u, 0x0000000bu,
		0x00000000u, 0x0007002cu, 0x00000007u, 0x0000000cu, 0x0000000au, 0x0000000bu, 0x0000000bu,
		0x0000000au, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u,
		0x00000005u, 0x0003003eu, 0x00000009u, 0x0000000cu, 0x000100fdu, 0x00010038u,
	};

	std::vector<std::byte> Word(uint32_t word) {
		ByteWriter writer;
		writer.WriteUInt32(word);
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}

	ShaderData Fixture() {
		ShaderData data;
		data.CompilerVersion = "shaderc.fixture";
		data.OptimizerVersion = "spirv-tools.fixture";
		data.TranslatorVersion = "none";
		data.ShaderAbi = "atomic.material.v1";
		data.TargetEnvironment = "vulkan1.0";
		ShaderVariant variant;
		variant.Name = "opaque";
		variant.Stage = "fragment";
		variant.Outputs = {{"colour", "float4", 0}};
		variant.RequiredCapabilities = {"Shader"};
		ByteWriter module;
		for (uint32_t word : COMPILED_FRAGMENT)
			module.WriteUInt32(word);
		variant.Payloads = {{"spirv", "main", "spirv1.0", {module.Bytes().begin(), module.Bytes().end()}}};
		data.Variants.push_back(std::move(variant));
		return data;
	}

	ShaderData MetadataFixture() {
		auto data = Fixture();
		data.TranslatorVersion = "spirv-cross.fixture";
		data.CompilerOptions = {{"target", "vulkan1.0"}};
		data.OptimizerOptions = {{"profile", "performance"}};
		data.TranslatorOptions = {{"version", "2.0"}};
		data.Dependencies = {{"shaders/main.frag", Hasher::Of(std::as_bytes(std::span(COMPILED_FRAGMENT)))}};
		auto &variant = data.Variants.front();
		variant.Features = {{"alpha", "opaque"}, {"lights", "clustered"}};
		variant.Specializations = {{"enabled", "bool", 2, Word(1)}, {"samples", "uint", 3, Word(4)}};
		variant.Resources = {
			{"material", "uniform-buffer", 3, 0, "read", "none", "", 32},
			{"texture", "sampled-texture", 2, 0, "read", "2d", "rgba8", 0}
		};
		variant.Parameters = {
			{"colour", "material", "float4", 0, 16, 16}, {"roughness", "material", "float", 16, 4, 4}
		};
		variant.Inputs = {{"uv", "float2", 0}};
		variant.Instructions = 35;
		variant.ArithmeticInstructions = 4;
		variant.TextureInstructions = 2;
		variant.MemoryInstructions = 6;
		variant.ControlFlowInstructions = 5;
		variant.Optimizations = {{"constant-folding", 40, 35, true}, {"common-subexpression", 35, 35, false}};
		const std::string msl = "fragment float4 main0() { return float4(1, 0, 0, 1); }";
		const auto bytes = std::as_bytes(std::span(msl));
		variant.Payloads.insert(
			variant.Payloads.begin(), {"msl", "main0", "msl2.0", {bytes.begin(), bytes.end()}}
		);
		return data;
	}

	std::vector<std::byte> Encode(const ShaderData &data) {
		ByteWriter writer;
		REQUIRE(Shader::Write(writer, data));
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}

	void RefusesWrite(const ShaderData &data) {
		ByteWriter writer;
		writer.WriteUInt32(0xAABBCCDDu);
		CHECK_FALSE(data.IsValid());
		CHECK_FALSE(Shader::Write(writer, data));
		CHECK(writer.Bytes().size() == 4);
		ByteReader prefix(writer.Bytes());
		CHECK(prefix.ReadUInt32() == 0xAABBCCDDu);
	}

	void RefusesRead(std::span<const std::byte> bytes) {
		ShaderData held = Fixture();
		const ShaderData baseline = held;
		ByteReader reader(bytes);
		CHECK_FALSE(Shader::Read(reader, held));
		CHECK(reader.Failed());
		CHECK(held == baseline);
	}
}

TEST_CASE(
	"cooked shader bytes round trip metadata and backend entry points canonically", "[assets][shader]"
) {
	for (const auto &data : {Fixture(), MetadataFixture()}) {
		CHECK(data.IsValid());
		const auto bytes = Encode(data);
		CHECK(bytes[0] == std::byte{'A'});
		CHECK(bytes[1] == std::byte{'S'});
		CHECK(bytes[2] == std::byte{'H'});
		CHECK(bytes[3] == std::byte{'1'});
		ShaderData decoded;
		ByteReader reader(bytes);
		REQUIRE(Shader::Read(reader, decoded));
		CHECK(reader.AtEnd());
		CHECK(decoded == data);
		CHECK(Encode(decoded) == bytes);
		CHECK(decoded.Variants.front().Payloads.back().EntryPoint == "main");
	}
	const auto data = MetadataFixture();
	CHECK(data.Variants.front().Payloads.front().EntryPoint == "main0");
	CHECK(Shader::InterfaceHash(data.Variants.front()) != ContentHash{});
}

TEST_CASE("shader container truncation and corrupt length leave accepted output intact", "[assets][shader]") {
	const auto bytes = Encode(MetadataFixture());
	for (size_t length = 0; length < bytes.size(); length++) {
		INFO(length);
		RefusesRead(std::span(bytes).first(length));
	}
	for (size_t offset : {size_t{0}, size_t{4}, size_t{6}, size_t{10}}) {
		auto corrupt = bytes;
		corrupt[offset] = std::byte{255};
		RefusesRead(corrupt);
	}
	ByteWriter framed;
	REQUIRE(Shader::Write(framed, Fixture()));
	REQUIRE(Shader::Write(framed, MetadataFixture()));
	ByteReader reader(framed.Bytes());
	ShaderData first;
	ShaderData second;
	REQUIRE(Shader::Read(reader, first));
	REQUIRE(Shader::Read(reader, second));
	CHECK(first == Fixture());
	CHECK(second == MetadataFixture());
	CHECK(reader.AtEnd());
}

TEST_CASE("shader code and interface hashes catch corruption", "[assets][shader]") {
	const auto data = MetadataFixture();
	const auto bytes = Encode(data);
	const auto payloadDigest = Hasher::Of(data.Variants.front().Payloads.back().Bytes);
	const auto interfaceDigest = Shader::InterfaceHash(data.Variants.front());
	for (const auto &digest : {payloadDigest, interfaceDigest}) {
		const auto digestBytes = std::as_bytes(std::span(digest.Digest));
		const auto found = std::find_end(bytes.begin(), bytes.end(), digestBytes.begin(), digestBytes.end());
		REQUIRE(found != bytes.end());
		auto corrupt = bytes;
		corrupt[static_cast<size_t>(found - bytes.begin())] ^= std::byte{1};
		RefusesRead(corrupt);
	}
	auto corrupt = bytes;
	corrupt.back() ^= std::byte{1};
	RefusesRead(corrupt);
	auto changed = data.Variants.front();
	changed.Parameters.front().Offset = 4;
	CHECK(Shader::InterfaceHash(changed).IsZero());
	changed = data.Variants.front();
	changed.Resources.front().Set = 1;
	CHECK(Shader::InterfaceHash(changed) != interfaceDigest);
	changed = data.Variants.front();
	changed.Name = "another label";
	changed.Payloads.back().Bytes.back() ^= std::byte{1};
	CHECK(Shader::InterfaceHash(changed) == interfaceDigest);
}

TEST_CASE("shader names feature keys and variants have one canonical order", "[assets][shader]") {
	auto data = MetadataFixture();
	std::swap(data.Variants[0].Features[0], data.Variants[0].Features[1]);
	RefusesWrite(data);
	data = MetadataFixture();
	data.Variants[0].Features[1].Name = data.Variants[0].Features[0].Name;
	RefusesWrite(data);
	data = MetadataFixture();
	data.Dependencies.push_back(data.Dependencies.front());
	RefusesWrite(data);
	data = MetadataFixture();
	data.Dependencies.front().Root = {};
	RefusesWrite(data);
	data = MetadataFixture();
	data.Variants.push_back(data.Variants.front());
	data.Variants.back().Name = "transparent";
	RefusesWrite(data);
	data.Variants.back().Features.back().Value = "unclustered";
	CHECK(data.IsValid());
	std::swap(data.Variants.front(), data.Variants.back());
	RefusesWrite(data);
	data = Fixture();
	data.ShaderAbi.assign(Shader::MAXIMUM_NAME + 1, 'x');
	RefusesWrite(data);
	data = Fixture();
	data.Variants.front().Name = std::string("bad\0name", 8);
	RefusesWrite(data);
	data = Fixture();
	data.Variants.clear();
	RefusesWrite(data);
}

TEST_CASE(
	"shader structural checks reject incompatible metadata and malformed backend payloads", "[assets][shader]"
) {
	auto data = MetadataFixture();
	data.Variants.front().Stage = "future-stage";
	RefusesWrite(data);
	data = MetadataFixture();
	data.Variants.front().Resources[1].Binding = 0;
	data.Variants.front().Resources[1].Set = 3;
	RefusesWrite(data);
	data = MetadataFixture();
	data.Variants.front().Resources.front().Kind = "device-address";
	RefusesWrite(data);
	data = MetadataFixture();
	data.Variants.front().Parameters.back().Offset = 8;
	RefusesWrite(data);
	data = MetadataFixture();
	data.Variants.front().Parameters.back().Bytes = 32;
	RefusesWrite(data);
	data = MetadataFixture();
	data.Variants.front().Parameters.front().Alignment = 0;
	RefusesWrite(data);
	data.Variants.front().Parameters.front().Alignment = 3;
	RefusesWrite(data);
	data = MetadataFixture();
	data.Variants.front().Parameters.front().Type = "unknown";
	RefusesWrite(data);
	data = MetadataFixture();
	data.Variants.front().Parameters.front().Resource = "missing";
	RefusesWrite(data);
	data = MetadataFixture();
	data.Variants.front().Payloads.back().EntryPoint.clear();
	RefusesWrite(data);
	data = MetadataFixture();
	data.Variants.front().Payloads.back().Bytes.pop_back();
	RefusesWrite(data);
	data = MetadataFixture();
	data.Variants.front().Payloads.back().Bytes.front() = std::byte{0};
	RefusesWrite(data);
	data = MetadataFixture();
	data.Variants.front().Payloads.front().Bytes.push_back(std::byte{0});
	RefusesWrite(data);
	data = MetadataFixture();
	std::swap(data.Variants.front().Payloads.front(), data.Variants.front().Payloads.back());
	RefusesWrite(data);
	data = MetadataFixture();
	data.Variants.front().Payloads.pop_back();
	RefusesWrite(data);
}

TEST_CASE("shader specialization values and local sizes are bounded before admission", "[assets][shader]") {
	auto data = MetadataFixture();
	data.Variants.front().Specializations.front().Value = Word(2);
	RefusesWrite(data);
	data = MetadataFixture();
	data.Variants.front().Specializations.back().ConstantId = 2;
	RefusesWrite(data);
	data = MetadataFixture();
	data.Variants.front().Specializations.front().Type = "float";
	data.Variants.front().Specializations.front().Value = Word(0x7FC00000u);
	RefusesWrite(data);
	data.Variants.front().Specializations.front().Value = Word(std::bit_cast<uint32_t>(-0.5f));
	CHECK(data.IsValid());
	data = Fixture();
	data.Variants.front().WorkgroupX = 8;
	RefusesWrite(data);
	data.Variants.front().Stage = "compute";
	CHECK(data.IsValid());
	data.Variants.front().WorkgroupY = 0;
	RefusesWrite(data);
	data.Variants.front().WorkgroupY = std::numeric_limits<uint32_t>::max();
	RefusesWrite(data);
	data.Variants.front().WorkgroupY = 1;
	data.Variants.front().WorkgroupZ = std::numeric_limits<uint32_t>::max();
	RefusesWrite(data);
}

TEST_CASE(
	"shader collection payload and total container bounds prevent oversized allocation", "[assets][shader]"
) {
	auto data = Fixture();
	data.Variants.resize(Shader::MAXIMUM_VARIANTS + 1);
	RefusesWrite(data);
	data = Fixture();
	data.Variants.front().Resources.resize(Shader::MAXIMUM_DECLARATIONS + 1);
	RefusesWrite(data);
	data = Fixture();
	data.Variants.front().Features.resize(Shader::MAXIMUM_KEYS + 1);
	RefusesWrite(data);
	data = Fixture();
	data.Variants.front().Payloads.front().Bytes.resize(Shader::MAXIMUM_PAYLOAD_BYTES + 1);
	RefusesWrite(data);
	data = Fixture();
	data.Variants.front().Payloads.front().Bytes.resize(Shader::MAXIMUM_PAYLOAD_BYTES);
	CHECK(data.IsValid());
	const auto variant = data.Variants.front();
	for (uint32_t index = 0; index < 3; index++) {
		data.Variants.push_back(variant);
		data.Variants.back().Name = "variant" + std::to_string(index);
		data.Variants.back().Features = {{"mode", std::to_string(index)}};
	}
	RefusesWrite(data);

	const auto bytes = Encode(Fixture());
	ByteReader locate(bytes);
	locate.ReadUInt32();
	locate.ReadUInt16();
	locate.ReadUInt32();
	for (uint32_t field = 0; field < 9; field++)
		locate.ReadString();
	for (uint32_t field = 0; field < 3; field++)
		CHECK(locate.ReadUInt32() == 0);
	auto corrupt = bytes;
	for (size_t byte = 0; byte < 4; byte++)
		corrupt[locate.Position() + byte] = std::byte{255};
	RefusesRead(corrupt);
}

TEST_CASE("shader parsing is structural and never claims payload execution validity", "[assets][shader]") {
	auto data = Fixture();
	data.Variants.front().Payloads.front().Bytes.resize(20);
	CHECK(data.IsValid());
	const auto bytes = Encode(data);
	ByteReader reader(bytes);
	ShaderData decoded;
	REQUIRE(Shader::Read(reader, decoded));
	CHECK(decoded == data);
	// A header with no instructions has valid transport shape and is not an executable shader.
	CHECK(decoded.Variants.front().Payloads.front().Bytes.size() == 20);
}

TEST_CASE(
	"shader interface identity includes descriptor arrays uniform strides and interpolants",
	"[assets][shader]"
) {
	const auto baseline = MetadataFixture();
	const ContentHash signature = Shader::InterfaceHash(baseline.Variants.front());
	const auto differs = [&](const ShaderData &changed) {
		REQUIRE(changed.IsValid());
		CHECK(Shader::InterfaceHash(changed.Variants.front()) != signature);
		const auto bytes = Encode(changed);
		ByteReader reader(bytes);
		ShaderData parsed;
		REQUIRE(Shader::Read(reader, parsed));
		CHECK(parsed == changed);
	};
	auto changed = baseline;
	changed.Variants.front().Resources.back().DescriptorCount = 4;
	differs(changed);
	changed.Variants.front().Resources.back().DescriptorCount = 0;
	changed.Variants.front().Resources.back().RuntimeArray = true;
	differs(changed);
	changed = baseline;
	changed.Variants.front().Resources.back().SampleType = "uint";
	differs(changed);
	changed = baseline;
	changed.Variants.front().Parameters.back().ArrayCount = 2;
	changed.Variants.front().Parameters.back().ArrayStride = 4;
	changed.Variants.front().Parameters.back().Bytes = 8;
	differs(changed);
	changed.Variants.front().Parameters.back().ArrayStride = 8;
	changed.Variants.front().Parameters.back().Bytes = 12;
	differs(changed);
	changed = baseline;
	changed.Variants.front().Parameters.resize(1);
	changed.Variants.front().Parameters.front().Type = "float2x2";
	changed.Variants.front().Parameters.front().MatrixStride = 8;
	differs(changed);
	changed.Variants.front().Parameters.front().RowMajor = true;
	differs(changed);
	changed.Variants.front().Parameters.front().MatrixStride = 16;
	changed.Variants.front().Parameters.front().Bytes = 24;
	differs(changed);
	changed = baseline;
	changed.Variants.front().Inputs.front().Interpolation = "flat";
	differs(changed);
	changed = baseline;
	changed.Variants.front().Inputs.front().Centroid = true;
	differs(changed);
	changed = baseline;
	changed.Variants.front().Inputs.front().Sample = true;
	differs(changed);
	changed = baseline;
	changed.Variants.front().Inputs.front().Component = 2;
	differs(changed);
	changed = baseline;
	changed.Variants.front().Inputs.front().LocationCount = 2;
	differs(changed);
	changed = baseline;
	changed.Variants.front().Inputs.push_back({"weights", "float2", 0, 2});
	differs(changed);
	changed.Variants.front().Inputs.back().Component = 1;
	RefusesWrite(changed);
	changed = baseline;
	changed.Variants.front().Inputs.front().Type = "float4x4";
	RefusesWrite(changed);
	changed.Variants.front().Inputs.front().LocationCount = 4;
	differs(changed);
}

TEST_CASE(
	"bounded shader byte mutations either refuse or preserve canonical round trips", "[assets][shader]"
) {
	const auto bytes = Encode(MetadataFixture());
	for (size_t index = 0; index < bytes.size(); index++) {
		auto mutation = bytes;
		mutation[index] ^= std::byte{0x80};
		ByteReader reader(mutation);
		ShaderData decoded;
		if (!Shader::Read(reader, decoded)) {
			CHECK(reader.Failed());
			continue;
		}
		CHECK(decoded.IsValid());
		CHECK(reader.AtEnd());
		CHECK(Encode(decoded) == mutation);
	}
}
