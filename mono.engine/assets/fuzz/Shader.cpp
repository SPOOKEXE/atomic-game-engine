#include <engine/assets/Shader.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string_view>

namespace {

	void Require(bool condition) {
		if (!condition) {
			std::abort();
		}
	}

	engine::assets::ShaderData Seed(std::string_view stage) {
		using namespace engine::assets;
		ShaderData data;
		data.CompilerVersion = "fuzz.seed";
		data.OptimizerVersion = "fuzz.seed";
		data.TranslatorVersion = "fuzz.seed";
		data.ShaderAbi = "atomic.fuzz.v1";
		data.TargetEnvironment = "vulkan1.0";
		data.Dependencies = {{"source", Hasher::Of(std::as_bytes(std::span(stage)))}};
		ShaderVariant variant;
		variant.Name = "default";
		variant.Stage = stage;
		variant.Features = {{"lighting", "on"}};
		variant.Resources = {{"material", "uniform-buffer", 3, 0, "read", "none", "", 80}};
		variant.Parameters = {
			{"colour", "material", "float4", 0, 16, 16},
			{"matrix", "material", "float4x4", 16, 64, 16, 1, 0, 16, false}
		};
		variant.Inputs = {{"uv", "float2", 0}};
		variant.Outputs = {{"colour", "float4", 0}};
		variant.RequiredCapabilities = {"Shader"};
		if (stage == "compute") {
			variant.Inputs.clear();
			variant.Outputs.clear();
			variant.WorkgroupX = 8;
			variant.WorkgroupY = 8;
		}
		// Transport seeds need a shaped payload, not an executable device program.
		engine::core::ByteWriter words;
		for (uint32_t word : {0x07230203u, 0x00010000u, 0u, 1u, 0u}) {
			words.WriteUInt32(word);
		}
		const std::string_view msl = "fragment float4 main0() { return float4(1); }";
		const auto mslBytes = std::as_bytes(std::span(msl));
		variant.Payloads = {
			{"msl", "main0", "msl2.0", {mslBytes.begin(), mslBytes.end()}},
			{"spirv", "main", "spirv1.0", {words.Bytes().begin(), words.Bytes().end()}}
		};
		data.Variants.push_back(std::move(variant));
		return data;
	}
}

// libFuzzer calls this before parsing its own arguments. Seed files stay under
// the build directory selected by the recipe, separate from crash artifacts.
extern "C" int LLVMFuzzerInitialize(int *argumentCount, char ***arguments) {
	if (*argumentCount != 3 || std::string_view((*arguments)[1]) != "--write-seeds") {
		return 0;
	}
	const std::filesystem::path directory((*arguments)[2]);
	std::filesystem::create_directories(directory);
	for (const std::string_view stage : {"vertex", "fragment", "compute"}) {
		engine::core::ByteWriter writer;
		Require(engine::assets::Shader::Write(writer, Seed(stage)));
		std::ofstream file(directory / (std::string(stage) + ".ashader"), std::ios::binary);
		const auto bytes = writer.Bytes();
		file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		Require(file.good());
	}
	std::cout << "wrote 3 cooked shader transport seeds\n";
	std::exit(0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *input, size_t size) {
	using namespace engine::assets;
	const auto bytes = std::as_bytes(std::span(input, size));
	engine::core::ByteReader reader(bytes);
	ShaderData parsed;
	parsed.ShaderAbi = "unchanged-on-refusal";
	if (!Shader::Read(reader, parsed)) {
		Require(reader.Failed());
		Require(parsed.ShaderAbi == "unchanged-on-refusal" && parsed.Variants.empty());
		return 0;
	}
	Require(!reader.Failed() && parsed.IsValid());
	engine::core::ByteWriter canonical;
	Require(Shader::Write(canonical, parsed));
	Require(canonical.Bytes().size() <= size);
	Require(std::equal(canonical.Bytes().begin(), canonical.Bytes().end(), bytes.begin()));
	engine::core::ByteReader again(canonical.Bytes());
	ShaderData roundTrip;
	Require(Shader::Read(again, roundTrip) && again.AtEnd() && roundTrip == parsed);
	return 0;
}
