// CPU contracts for authored trace settings and unavailable history inputs.

#include "HardRender.hpp"

#include "RenderTypes.hpp"

#include <engine/resources/Shaders.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.render.hardrender")
TEST_DEPENDS("engine.resources.shaders")

using engine::core::Name;
using engine::graph::Node;
using engine::render::IsTraceNode;
using engine::render::TraceOptionsFor;

namespace {
	struct SpirvPassMembers {
		std::unordered_map<std::string, uint32_t> Offsets;
		bool Found = false;
	};

	uint64_t MemberKey(uint32_t structure, uint32_t member) {
		return (uint64_t(structure) << 32) | member;
	}

	bool DecodeMemberName(const uint32_t *words, size_t count, std::string &name) {
		for (size_t index = 0; index < count; ++index) {
			for (uint32_t byte = 0; byte < 4; ++byte) {
				const char value = static_cast<char>((words[index] >> (byte * 8)) & 0xffu);
				if (value == '\0') return true;
				if (name.size() == 128) return false;
				name.push_back(value);
			}
		}
		return false;
	}

	SpirvPassMembers ReadPassMembers(std::string_view shaderName) {
		const auto path = engine::resources::Shader(shaderName, engine::resources::ShaderForm::SpirV);
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (!input) return {};
		const std::streamsize fileBytes = input.tellg();
		if (fileBytes < 20 || fileBytes > 1024 * 1024 || fileBytes % 4 != 0) return {};
		input.seekg(0);
		std::vector<uint8_t> bytes(static_cast<size_t>(fileBytes));
		if (!input.read(reinterpret_cast<char *>(bytes.data()), fileBytes)) return {};
		std::vector<uint32_t> words(bytes.size() / 4);
		for (size_t index = 0; index < words.size(); ++index) {
			words[index] = uint32_t(bytes[index * 4]) | uint32_t(bytes[index * 4 + 1]) << 8 |
						   uint32_t(bytes[index * 4 + 2]) << 16 | uint32_t(bytes[index * 4 + 3]) << 24;
		}
		if (words.front() != 0x07230203u) return {};

		std::unordered_map<uint32_t, uint32_t> sets, bindings, pointees;
		std::unordered_map<uint64_t, std::string> names;
		std::unordered_map<uint64_t, uint32_t> offsets;
		std::vector<std::pair<uint32_t, uint32_t>> variables;
		for (size_t at = 5; at < words.size();) {
			const uint32_t instruction = words[at];
			const size_t wordCount = instruction >> 16;
			const uint32_t opcode = instruction & 0xffffu;
			if (wordCount == 0 || wordCount > words.size() - at) return {};
			const uint32_t *const operands = words.data() + at + 1;
			const size_t operandCount = wordCount - 1;
			switch (opcode) {
			case 6: // OpMemberName
				if (operandCount < 3) return {};
				{
					std::string name;
					if (!DecodeMemberName(operands + 2, operandCount - 2, name)) return {};
					if (!names.emplace(MemberKey(operands[0], operands[1]), std::move(name)).second)
						return {};
				}
				break;
			case 71: // OpDecorate
				if (operandCount >= 3 && operands[1] == 33) bindings[operands[0]] = operands[2];
				if (operandCount >= 3 && operands[1] == 34) sets[operands[0]] = operands[2];
				break;
			case 72: // OpMemberDecorate
				if (operandCount >= 4 && operands[2] == 35)
					offsets[MemberKey(operands[0], operands[1])] = operands[3];
				break;
			case 32: // OpTypePointer
				if (operandCount >= 3) pointees[operands[0]] = operands[2];
				break;
			case 59: // OpVariable
				if (operandCount >= 2) variables.emplace_back(operands[1], operands[0]);
				break;
			default:
				break;
			}
			at += wordCount;
		}

		uint32_t passStructure = 0;
		bool foundPass = false;
		for (const auto &[variable, pointer] : variables) {
			const auto set = sets.find(variable);
			const auto binding = bindings.find(variable);
			if (set == sets.end() || binding == bindings.end() || set->second != 2 || binding->second != 0)
				continue;
			if (foundPass) return {};
			const auto structure = pointees.find(pointer);
			if (structure == pointees.end()) return {};
			passStructure = structure->second;
			foundPass = true;
		}
		if (!foundPass) return {};

		SpirvPassMembers result;
		for (const auto &[key, name] : names) {
			if (uint32_t(key >> 32) != passStructure) continue;
			const auto offset = offsets.find(key);
			if (offset == offsets.end() || !result.Offsets.emplace(name, offset->second).second) return {};
		}
		result.Found = !result.Offsets.empty();
		return result;
	}
}

TEST_CASE("hard render options preserve authored trace settings", "[render][hard-render]") {
	Node illumination;
	illumination.Kind = Name("global-illumination");
	illumination.Parameters = {
		{Name("rays-per-pixel"), "7"},
		{Name("max-distance"), "42"},
	};
	const auto gi = TraceOptionsFor(illumination, false);
	CHECK(gi.Trace.x == 7.0f);
	CHECK(gi.Trace.y == 42.0f);
	CHECK(gi.Inputs.x == 0u);

	Node pathtrace;
	pathtrace.Kind = Name("pathtrace");
	pathtrace.Parameters = {
		{Name("max-bounces"), "5"},
		{Name("samples-per-frame"), "3"},
	};
	const auto trace = TraceOptionsFor(pathtrace, true);
	CHECK(trace.Trace.x == 5.0f);
	CHECK(trace.Trace.w == 3.0f);
	CHECK(trace.Inputs.x == 1u);
}

TEST_CASE("only trace nodes allocate the trace uniform slot", "[render][hard-render]") {
	CHECK(IsTraceNode(Name("global-illumination")));
	CHECK(IsTraceNode(Name("raytrace")));
	CHECK(IsTraceNode(Name("pathtrace")));
	CHECK_FALSE(IsTraceNode(Name("dispatch")));
}

TEST_CASE("staged trace shaders match named render pass uniform members", "[render][hard-render]") {
	using engine::render::GraphPassUniforms;
	const std::array<std::string_view, 3> shaders{
		"global-illumination.comp", "raytrace.comp", "pathtrace.comp"
	};
	const std::array<std::pair<std::string_view, size_t>, 7> expected{{
		{"vp", offsetof(GraphPassUniforms, ViewProjection)},
		{"ivp", offsetof(GraphPassUniforms, InverseViewProjection)},
		{"target", offsetof(GraphPassUniforms, Target)},
		{"view", offsetof(GraphPassUniforms, View)},
		{"eye", offsetof(GraphPassUniforms, Eye)},
		{"cameraDepth", offsetof(GraphPassUniforms, CameraDepth)},
		{"features", offsetof(GraphPassUniforms, RenderFeatures)},
	}};
	for (const auto shader : shaders) {
		CAPTURE(shader);
		const auto reflected = ReadPassMembers(shader);
		REQUIRE(reflected.Found);
		for (const auto &[name, expectedOffset] : expected) {
			const auto found = reflected.Offsets.find(std::string(name));
			REQUIRE(found != reflected.Offsets.end());
			CHECK(found->second == expectedOffset);
		}
	}
}
