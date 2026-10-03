#include <engine/assets/Shader.hpp>
#include <engine/msl/Translate.hpp>
#include <engine/render/ComposerHlsl.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <string>

TEST_SUITE_ID("engine.render.composerhlsl")

namespace {
	using namespace engine::render::hlsl;
	constexpr std::string_view DEFAULT_MAIN =
		"float4 surfaceColor=gm_BaseTextureObject.Sample(gm_BaseTexture,input.uv); "
		"output.color=surfaceColor;";
	constexpr std::string_view ARGUMENT_MAIN =
		"output.color=gm_BaseTextureObject.Sample(gm_BaseTexture,input.uv)*gain+float(count)+"
		"float4(pair,triple.x,triple.y)+quad+mul(matrix3,triple).xxxx+mul(matrix4,quad)+"
		"extraObject.Sample(extra,input.uv)+tint;";

	void Accepted(std::optional<std::string> failure) {
		INFO(failure.value_or("accepted"));
		REQUIRE_FALSE(failure.has_value());
	}
	std::vector<Argument> Arguments() {
		return {
			{"gain", ArgumentKind::Float},
			{"count", ArgumentKind::Int},
			{"pair", ArgumentKind::Vec2},
			{"triple", ArgumentKind::Vec3},
			{"quad", ArgumentKind::Vec4},
			{"matrix3", ArgumentKind::Mat3},
			{"matrix4", ArgumentKind::Mat4},
			{"extra", ArgumentKind::Sampler2D},
			{"tint", ArgumentKind::Color}
		};
	}
	Program Cooked(std::span<const Argument> arguments = {}, std::string_view main = DEFAULT_MAIN) {
		const Definition definition{"deliberately invalid ignored vertex", main, "", "", arguments};
		Source source;
		Accepted(Assemble(definition, {}, source));
		Program program;
		Accepted(Cook(definition, source, program));
		Accepted(Admit(program));
		return program;
	}
	uint32_t Word(std::span<const std::byte> bytes, size_t offset) {
		REQUIRE(offset <= bytes.size());
		REQUIRE(bytes.size() - offset >= 4);
		uint32_t word = 0;
		for (uint32_t index = 0; index < 4; ++index)
			word |= uint32_t(bytes[offset + index]) << (index * 8);
		return word;
	}
}

TEST_CASE(
	"Composer HLSL preserves fixed vertex and source library assembly order", "[render][composer-hlsl]"
) {
	Definition definition{
		"invalid authored vertex",
		DEFAULT_MAIN,
		"float globalValue;",
		"using \"math\";\nusing \"missing\";",
		{}
	};
	const std::array<Library, 1> libraries{{{"math", "float helper(float x){return x*2;}"}}};
	Source source;
	Accepted(Assemble(definition, libraries, source));
	CHECK(source.Vertex.find("invalid authored vertex") == std::string::npos);
	CHECK(source.Vertex.find("gm_Matrices[3]") != std::string::npos);
	CHECK(source.MissingLibraries == std::vector<std::string>{"missing"});
	CHECK(source.Fragment.find("helper") < source.Fragment.find("cbuffer Data"));
	CHECK(source.Fragment.find("cbuffer Data") < source.Fragment.find("globalValue"));
	CHECK(source.Fragment.find("globalValue") < source.Fragment.find("void main"));
}

TEST_CASE(
	"Composer HLSL bounds authored dependencies and rejects generated name collisions",
	"[render][composer-hlsl]"
) {
	Source source;
	Accepted(Assemble({"", DEFAULT_MAIN, "", "", {}}, {}, source));
	const auto accepted = source.Fragment;
	std::vector<Argument> arguments{{"extra", ArgumentKind::Sampler2D}, {"extraObject", ArgumentKind::Float}};
	CHECK(Assemble({"", DEFAULT_MAIN, "", "", arguments}, {}, source).has_value());
	CHECK(source.Fragment == accepted);
	arguments = {{"bad; declaration", ArgumentKind::Float}};
	CHECK(Assemble({"", DEFAULT_MAIN, "", "", arguments}, {}, source).has_value());
	arguments = {{"duplicate", ArgumentKind::Float}, {"duplicate", ArgumentKind::Float}};
	CHECK(Assemble({"", DEFAULT_MAIN, "", "", arguments}, {}, source).has_value());
	const std::array<Library, 2> duplicateLibraries{{{"same", ""}, {"same", ""}}};
	CHECK(Assemble({"", DEFAULT_MAIN, "", "", {}}, duplicateLibraries, source).has_value());
	std::string selectors;
	for (uint32_t index = 0; index < 65; ++index)
		selectors += "missing;";
	CHECK(Assemble({"", DEFAULT_MAIN, "", selectors, {}}, {}, source).has_value());
	arguments.clear();
	for (uint32_t index = 0; index < 16; ++index)
		arguments.push_back({"texture" + std::to_string(index), ArgumentKind::Sampler2D});
	CHECK(Assemble({"", DEFAULT_MAIN, "", "", arguments}, {}, source).has_value());
	const std::string large(MAXIMUM_SOURCE_BYTES + 1, ' ');
	CHECK(Assemble({"", large, "", "", {}}, {}, source).has_value());
	CHECK(source.Fragment == accepted);
}

TEST_CASE(
	"Composer HLSL cooks the paired default surface and revalidates stage metadata", "[render][composer-hlsl]"
) {
	const auto program = Cooked();
	CHECK(program.SamplerCount == 1);
	CHECK(program.UniformBytes == 0);
	REQUIRE_FALSE(program.Vertex.SpirV.empty());
	REQUIRE_FALSE(program.Fragment.SpirV.empty());
	CHECK(program.Vertex.SpirV.front() == 0x07230203u);
	auto invalid = program;
	invalid.Vertex = invalid.Fragment;
	CHECK(Admit(invalid).has_value());
	invalid = program;
	invalid.SamplerCount = 2;
	CHECK(Admit(invalid).has_value());
}

TEST_CASE(
	"Composer HLSL packs all source numeric argument kinds using reflected offsets", "[render][composer-hlsl]"
) {
	const auto arguments = Arguments();
	const auto program = Cooked(arguments, ARGUMENT_MAIN);
	REQUIRE(program.Members.size() == 8);
	CHECK(program.SamplerCount == 2);
	const std::array<double, 1> gain{2.5}, count{-3};
	const std::array<double, 2> pair{1, 2};
	const std::array<double, 3> triple{3, 4, 5};
	const std::array<double, 4> quad{6, 7, 8, 9}, tint{.25, .5, .75, 1};
	const std::array<double, 9> matrix3{1, 2, 3, 4, 5, 6, 7, 8, 9};
	const std::array<double, 16> matrix4{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
	std::vector<Value> values{
		{"gain", gain},
		{"count", count},
		{"pair", pair},
		{"triple", triple},
		{"quad", quad},
		{"matrix3", matrix3},
		{"matrix4", matrix4},
		{"tint", tint}
	};
	std::vector<std::byte> packed;
	Accepted(Pack(program, values, packed));
	CHECK(packed.size() == program.UniformBytes);
	for (const auto &member : program.Members) {
		INFO(member.Name);
		const auto authored = std::find_if(values.begin(), values.end(), [&](const auto &value) {
			return value.Name == member.Name;
		});
		REQUIRE(authored != values.end());
		if (member.Kind == ArgumentKind::Int) {
			CHECK(std::bit_cast<int32_t>(Word(packed, member.Offset)) == -3);
			continue;
		}
		const uint32_t width = member.Kind == ArgumentKind::Mat3   ? 3
							   : member.Kind == ArgumentKind::Mat4 ? 4
																   : 0;
		if (width) {
			CHECK(member.MatrixStride == 16);
			for (uint32_t column = 0; column < width; ++column)
				for (uint32_t row = 0; row < width; ++row) {
					const auto offset =
						member.Offset + (member.RowMajor ? column * member.MatrixStride + row * 4
														 : row * member.MatrixStride + column * 4);
					CHECK(
						std::bit_cast<float>(Word(packed, offset)) ==
						float(authored->Numbers[column * width + row])
					);
				}
		} else
			for (size_t index = 0; index < authored->Numbers.size(); ++index)
				CHECK(
					std::bit_cast<float>(Word(packed, member.Offset + index * 4)) ==
					float(authored->Numbers[index])
				);
	}
	const auto accepted = packed;
	auto duplicate = values;
	duplicate.push_back(values[0]);
	CHECK(Pack(program, duplicate, packed).has_value());
	CHECK(packed == accepted);
	const std::array<double, 1> fractional{.5};
	values[1].Numbers = fractional;
	CHECK(Pack(program, values, packed).has_value());
	CHECK(packed == accepted);
	values[1].Numbers = count;
	const std::array<double, 1> nonfinite{std::numeric_limits<double>::quiet_NaN()};
	values[0].Numbers = nonfinite;
	CHECK(Pack(program, values, packed).has_value());
	CHECK(packed == accepted);
	auto forged = program;
	forged.Members[0].Offset += 4;
	CHECK(Admit(forged).has_value());
}

TEST_CASE("Composer HLSL failed source edits preserve the accepted pair", "[render][composer-hlsl]") {
	auto program = Cooked();
	const auto accepted = program;
	Source source;
	Definition definition{
		"",
		"rogue[0]=float4(1,1,1,1); output.color=rogue[0];",
		"RWStructuredBuffer<float4> rogue:register(u1);",
		"",
		{}
	};
	Accepted(Assemble(definition, {}, source));
	CHECK(Cook(definition, source, program).has_value());
	CHECK(program == accepted);
	definition.Global = "";
	definition.Main = "output.color=notDeclared;";
	Accepted(Assemble(definition, {}, source));
	CHECK(Cook(definition, source, program).has_value());
	CHECK(program == accepted);
}

TEST_CASE(
	"Composer HLSL sparse sampler mapping preserves the authored texture across backends",
	"[render][composer-hlsl]"
) {
	const auto arguments = Arguments();
	const auto program = Cooked(arguments, "output.color=extraObject.Sample(extra,input.uv);");
	std::vector<SamplerBinding> vulkan, metal;
	Accepted(SamplerBindings(program, "spirv", vulkan));
	Accepted(SamplerBindings(program, "msl", metal));
	REQUIRE(vulkan.size() == 1);
	REQUIRE(metal.size() == 1);
	CHECK(vulkan[0].SourceName == "extra");
	CHECK(vulkan[0].SourceSlot == 1);
	CHECK(vulkan[0].BackendSlot == 1);
	CHECK(metal[0].SourceName == "extra");
	CHECK(metal[0].SourceSlot == 1);
	CHECK(metal[0].BackendSlot == 0);
	const auto translated = engine::msl::Translate(program.Fragment.SpirV);
	INFO(translated.Error);
	REQUIRE_FALSE(translated.Failed);
	CHECK(translated.Source.find("[[texture(0)]]") != std::string::npos);
}

TEST_CASE(
	"Composer HLSL paired ASH1 transport owns exact reflected metadata and bytes", "[render][composer-hlsl]"
) {
	const auto arguments = Arguments();
	const auto program = Cooked(arguments, ARGUMENT_MAIN);
	engine::assets::ShaderData container;
	Accepted(BuildContainer(program, "shaderc-test-provenance", container));
	REQUIRE(container.IsValid());
	CHECK(container.ShaderAbi == "atomic.composer-hlsl.v1");
	CHECK(container.SourceLanguage == "hlsl");
	REQUIRE(container.Variants.size() == 2);
	CHECK(container.Variants[0].Name == "hlsl.fragment");
	CHECK(container.Variants[1].Name == "hlsl.vertex");
	for (const auto &variant : container.Variants) {
		REQUIRE(variant.Payloads.size() == 1);
		CHECK(variant.Payloads[0].EntryPoint == "main");
		CHECK(variant.Payloads[0].Backend == "spirv");
		REQUIRE_FALSE(variant.Parameters.empty());
	}
	engine::core::ByteWriter writer;
	REQUIRE(engine::assets::Shader::Write(writer, container));
	engine::core::ByteReader reader(writer.Bytes());
	engine::assets::ShaderData decoded;
	REQUIRE(engine::assets::Shader::Read(reader, decoded));
	CHECK(decoded == container);
	const auto accepted = container;
	CHECK(BuildContainer(program, "", container).has_value());
	CHECK(container == accepted);
}

TEST_CASE(
	"Composer HLSL source cap includes selected-library separators and generated declarations",
	"[render][composer-hlsl]"
) {
	Source source;
	Accepted(Assemble({"", DEFAULT_MAIN, "", "", {}}, {}, source));
	const auto accepted = source.Fragment;
	const std::string exact(MAXIMUM_SOURCE_BYTES - 1, ' ');
	const std::array<Library, 1> libraries{{{"x", exact}}};
	const auto separator = Assemble({"", "", "", "x", {}}, libraries, source);
	REQUIRE(separator.has_value());
	CHECK(separator->find("selected libraries exceed") != std::string::npos);
	CHECK(source.Fragment == accepted);
	const std::string repeated(MAXIMUM_SOURCE_BYTES / 2 - 2, ' ');
	const std::array<Library, 1> repeatLibraries{{{"x", repeated}}};
	const auto repeat = Assemble({"", "", "", "x;x;x", {}}, repeatLibraries, source);
	REQUIRE(repeat.has_value());
	CHECK(repeat->find("selected libraries exceed") != std::string::npos);
	CHECK(source.Fragment == accepted);
	const std::string prefix(MAXIMUM_SOURCE_BYTES - 16, ' ');
	const std::array<Library, 1> prefixLibrary{{{"x", prefix}}};
	const auto generated = Assemble({"", "", "", "x", {}}, prefixLibrary, source);
	REQUIRE(generated.has_value());
	CHECK(generated->find("assembled source exceeds") != std::string::npos);
	CHECK(source.Fragment == accepted);
}

TEST_CASE(
	"Composer HLSL reflection preflights member count and unique declared identities",
	"[render][composer-hlsl]"
) {
	engine::render::ShaderCompiler compiler;
	auto program = Cooked();
	program.Arguments = {{"gain", ArgumentKind::Float}, {"next", ArgumentKind::Float}};
	std::string fields;
	for (uint32_t index = 0; index < MAXIMUM_ARGUMENTS + 1; ++index)
		fields += "float member" + std::to_string(index) + ";";
	const auto oversized = compiler.Compile(
		"#version 450\nlayout(location=0)in vec2 uv;layout(location=0)out vec4 "
		"colour;layout(set=3,binding=0)uniform Data{" +
			fields + "}data;void main(){colour=vec4(data.member0+uv.x);}",
		engine::render::ShaderStage::Fragment,
		"oversized-block.frag"
	);
	INFO(oversized.Error);
	REQUIRE_FALSE(oversized.Failed);
	program.Fragment.SpirV = oversized.SpirV;
	const auto count = Admit(program);
	REQUIRE(count.has_value());
	CHECK(count->find("member count") != std::string::npos);
	const auto duplicate = compiler.Compile(
		"#version 450\nlayout(location=0)in vec2 uv;layout(location=0)out vec4 "
		"colour;layout(set=3,binding=0)uniform Data{float gain;float next;}data;void "
		"main(){colour=vec4(data.gain+data.next+uv.x);}",
		engine::render::ShaderStage::Fragment,
		"duplicate-block.frag"
	);
	INFO(duplicate.Error);
	REQUIRE_FALSE(duplicate.Failed);
	program.Fragment.SpirV = duplicate.SpirV;
	bool renamed = false;
	for (size_t at = 5; at < program.Fragment.SpirV.size();) {
		const auto instruction = program.Fragment.SpirV[at], words = instruction >> 16;
		REQUIRE(words > 0);
		REQUIRE(words <= program.Fragment.SpirV.size() - at);
		// Change the actual OpMemberName for "next" to "gain", preserving executable words.
		if ((instruction & 65535) == 6 && words == 5 && program.Fragment.SpirV[at + 2] == 1 &&
			program.Fragment.SpirV[at + 3] == 0x7478656e) {
			program.Fragment.SpirV[at + 3] = 0x6e696167;
			renamed = true;
		}
		at += words;
	}
	REQUIRE(renamed);
	program.UniformBytes = 8;
	program.Members = {
		{"gain", ArgumentKind::Float, 0, 4, 0, false}, {"gain", ArgumentKind::Float, 4, 4, 0, false}
	};
	const auto identity = Admit(program);
	REQUIRE(identity.has_value());
	CHECK(identity->find("duplicate uniform member") != std::string::npos);
}

TEST_CASE(
	"HLSL constant fragment admits optimized UV omission and packs asymmetric source matrix",
	"[render][composer-hlsl]"
) {
	using namespace engine::render::hlsl;
	const std::array<Argument, 1> arguments{{{"matrixArg", ArgumentKind::Mat3}}};
	Definition definition{"", "output.color=float4(mul(matrixArg,float3(1,0,0)),1);", "", "", arguments};
	Source source;
	Accepted(Assemble(definition, {}, source));
	Program program;
	Accepted(Cook(definition, source, program));
	Accepted(Admit(program));
	REQUIRE(program.Members.size() == 1);
	const auto &member = program.Members.front();
	const std::array<double, 9> components{0, 1, 0, 0, 0, 1, 1, 0, 0};
	const std::array<Value, 1> values{{{"matrixArg", components}}};
	std::vector<std::byte> bytes;
	Accepted(Pack(program, values, bytes));
	// Evaluate SPIR-V vector-times-matrix with its actual memory layout.
	// HLSL mul(matrix, unitX) must select the source first column, green.
	std::array<float, 3> transformed{};
	for (size_t column = 0; column < 3; ++column) {
		const auto offset = member.Offset + (member.RowMajor ? column * 4 : column * member.MatrixStride);
		transformed[column] = std::bit_cast<float>(Word(bytes, offset));
	}
	CHECK(transformed == std::array<float, 3>{0, 1, 0});
}
