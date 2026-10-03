#include "ComposerCookResidency.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/render/ComposerHlsl.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <shaderc/shaderc.hpp>
#include <spirv_cross.hpp>
namespace engine::render::hlsl {
	namespace {
		constexpr std::string_view VERTEX =
			R"HLSL(#define MATRIX_WORLD                 0
#define MATRIX_WORLD_VIEW            1
#define MATRIX_WORLD_VIEW_PROJECTION 2

cbuffer Matrices : register(b0) {
    float4x4 gm_Matrices[3];
};

struct VertexShaderInput {
    float3 pos      : POSITION;
    float3 color    : COLOR0;
    float2 uv       : TEXCOORD0;
};

struct VertexShaderOutput {
    float4 pos      : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

void main(in VertexShaderInput input, out VertexShaderOutput output) {
    output.pos  = mul(gm_Matrices[MATRIX_WORLD_VIEW_PROJECTION], float4(input.pos, 1.0f));
    output.uv   = input.uv;
})HLSL";
		constexpr std::string_view PRE_MAIN =
			R"HLSL(Texture2D gm_BaseTextureObject : register(t0);
SamplerState gm_BaseTexture    : register(s0);

struct VertexShaderOutput {
    float4 pos      : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

struct PixelShaderOutput {
    float4 color : SV_TARGET0;
};


)HLSL";
		constexpr std::string_view POST_MAIN =
			"void main(in VertexShaderOutput _input, out PixelShaderOutput output) "
			"{\nVertexShaderOutput input = _input;\n";
		constexpr std::array<std::string_view, 9> TYPES{
			"float", "int", "float2", "float3", "float4", "float3x3", "float4x4", "", "float4"
		};
		bool Identifier(std::string_view name) {
			if (name.empty() || name.size() > 128) return false;
			const auto letter = [](char c) {
				return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
			};
			if (!letter(name.front())) return false;
			return std::all_of(name.begin() + 1, name.end(), [&](char c) {
				return letter(c) || (c >= '0' && c <= '9');
			});
		}
		std::optional<std::string> ArgumentsValid(std::span<const Argument> arguments) {
			if (arguments.size() > MAXIMUM_ARGUMENTS) return "too many HLSL argument declarations";
			uint32_t samplers = 1;
			std::vector<std::string> names{"gm_BaseTexture", "gm_BaseTextureObject"};
			for (const auto &argument : arguments) {
				if (argument.Name.empty()) continue;
				if (!Identifier(argument.Name) || size_t(argument.Kind) >= TYPES.size())
					return "invalid HLSL argument declaration";
				if (std::find(names.begin(), names.end(), argument.Name) != names.end())
					return "duplicate HLSL argument name";
				names.push_back(argument.Name);
				if (argument.Kind == ArgumentKind::Sampler2D) {
					const auto texture = argument.Name + "Object";
					if (std::find(names.begin(), names.end(), texture) != names.end())
						return "HLSL sampler declaration name collision";
					names.push_back(texture);
					if (++samplers > MAXIMUM_SAMPLERS) return "too many HLSL texture arguments";
				}
			}
			return {};
		}
		void Replace(std::string &text, std::string_view from) {
			for (size_t at = 0; (at = text.find(from, at)) != std::string::npos;)
				text.erase(at, from.size());
		}
		void Trim(std::string &text) {
			const auto first = text.find_first_not_of(" \t\r\n"), last = text.find_last_not_of(" \t\r\n");
			text = first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
		}
		// HLSL anonymous cbuffer instances have an empty OpName. Give authoring
		// reflection and the independent MSL checker a shared stable debug name.
		bool NameAnonymousBuffers(std::vector<uint32_t> &words) {
			spirv_cross::Compiler compiler(words.data(), words.size());
			const auto resources = compiler.get_shader_resources();
			if (resources.uniform_buffers.size() > MAXIMUM_ARGUMENTS) return false;
			for (const auto &resource : resources.uniform_buffers) {
				if (!compiler.get_name(resource.id).empty()) continue;
				const std::string name = "composer_" + resource.name;
				if (!Identifier(name)) continue;
				std::vector<uint32_t> instruction(2 + (name.size() + 4) / 4);
				instruction[0] = (uint32_t(instruction.size()) << 16) | uint32_t(spv::OpName);
				instruction[1] = resource.id;
				for (size_t index = 0; index < name.size(); ++index)
					instruction[2 + index / 4] |= uint32_t(uint8_t(name[index])) << (8 * (index % 4));
				for (size_t at = 5; at < words.size();) {
					const uint32_t count = words[at] >> 16, op = words[at] & 65535;
					if (!count || count > words.size() - at) return false;
					if (op == spv::OpName && count >= 3 && words[at + 1] == resource.id) {
						words.erase(words.begin() + at, words.begin() + at + count);
						words.insert(words.begin() + at, instruction.begin(), instruction.end());
						break;
					}
					at += count;
				}
			}
			return true;
		}
		bool Shape(
			const spirv_cross::SPIRType &type, uint32_t vectors, uint32_t columns = 1, bool integer = false
		) {
			return type.width == 32 && type.vecsize == vectors && type.columns == columns &&
				   type.array.empty() &&
				   type.basetype == (integer ? spirv_cross::SPIRType::Int : spirv_cross::SPIRType::Float);
		}
		std::optional<std::string>
		Reflect(const Program &program, std::vector<Member> &members, uint32_t &bytes, uint32_t &samplers) {
			try {
				for (uint32_t stage = 0; stage < 2; ++stage) {
					const auto &words = stage ? program.Fragment.SpirV : program.Vertex.SpirV;
					if (words.size() < 5 || words.size() > 4 * 1024 * 1024 || words[0] != 0x07230203)
						return "HLSL cooked stage lacks SPIR-V";
					spirv_cross::Compiler compiler(words.data(), words.size());
					const auto entries = compiler.get_entry_points_and_stages();
					if (entries.size() != 1 || entries[0].name != "main" ||
						entries[0].execution_model !=
							(stage ? spv::ExecutionModelFragment : spv::ExecutionModelVertex))
						return "HLSL cooked stage entry point mismatch";
					const auto resources = compiler.get_shader_resources();
					if (!resources.separate_images.empty() || !resources.separate_samplers.empty() ||
						!resources.storage_images.empty() || !resources.storage_buffers.empty() ||
						!resources.push_constant_buffers.empty() || !resources.subpass_inputs.empty() ||
						!resources.atomic_counters.empty() || !resources.acceleration_structures.empty() ||
						!resources.shader_record_buffers.empty() || !resources.gl_plain_uniforms.empty() ||
						!resources.tensors.empty())
						return "HLSL surface pass does not bind this resource family";
					const auto interface = [&](const auto &values, bool inputs) {
						if (stage) {
							if (values.size() != 1) return false;
							const auto &value = values[0];
							return compiler.get_decoration(value.id, spv::DecorationLocation) == 0 &&
								   Shape(compiler.get_type(value.type_id), inputs ? 2 : 4);
						}
						if (!inputs)
							return values.size() == 1 &&
								   compiler.get_decoration(values[0].id, spv::DecorationLocation) == 0 &&
								   Shape(compiler.get_type(values[0].type_id), 2);
						if (values.size() != 2) return false;
						for (const auto &value : values) {
							const auto location = compiler.get_decoration(value.id, spv::DecorationLocation);
							if ((location != 0 && location != 2) ||
								!Shape(compiler.get_type(value.type_id), location == 0 ? 3 : 2))
								return false;
						}
						return true;
					};
					if (!interface(resources.stage_inputs, true) ||
						!interface(resources.stage_outputs, false))
						return "HLSL paired surface interface mismatch";
					if ((!stage && !resources.sampled_images.empty()) || resources.uniform_buffers.size() > 1)
						return "HLSL stage binds unexpected resources";
					if (stage) {
						uint32_t declared = 1;
						for (const auto &argument : program.Arguments)
							if (!argument.Name.empty() && argument.Kind == ArgumentKind::Sampler2D)
								++declared;
						std::vector<bool> seen(declared);
						for (const auto &image : resources.sampled_images) {
							const auto &type = compiler.get_type(image.type_id);
							const auto binding = compiler.get_decoration(image.id, spv::DecorationBinding);
							if (compiler.get_decoration(image.id, spv::DecorationDescriptorSet) != 2 ||
								binding >= declared || seen[binding] || !type.array.empty() ||
								type.image.dim != spv::Dim2D || type.image.ms || type.image.arrayed ||
								type.image.depth ||
								compiler.get_type(type.image.type).basetype != spirv_cross::SPIRType::Float)
								return "HLSL sampled texture binding mismatch";
							seen[binding] = true;
						}
						samplers = declared; // Inactive sampler slots are still supplied by the host.
					}
					for (const auto &resource : resources.uniform_buffers) {
						if (compiler.get_decoration(resource.id, spv::DecorationDescriptorSet) !=
								(stage ? 3 : 1) ||
							compiler.get_decoration(resource.id, spv::DecorationBinding) != 0)
							return "HLSL uniform binding mismatch";
						const auto &block = compiler.get_type(resource.base_type_id);
						if (!stage) {
							if (block.member_types.size() != 1) return "HLSL fixed vertex matrices mismatch";
							const auto &matrix = compiler.get_type(block.member_types[0]);
							if (matrix.basetype != spirv_cross::SPIRType::Float || matrix.width != 32 ||
								matrix.vecsize != 4 || matrix.columns != 4 || matrix.array.size() != 1 ||
								matrix.array[0] != 3 || compiler.type_struct_member_offset(block, 0) != 0 ||
								compiler.type_struct_member_array_stride(block, 0) != 64 ||
								compiler.type_struct_member_matrix_stride(block, 0) != 16 ||
								compiler.get_declared_struct_size(block) != 192)
								return "HLSL fixed vertex matrices mismatch";
							continue;
						}

						if (block.member_types.size() > MAXIMUM_ARGUMENTS)
							return "HLSL uniform member count exceeds argument budget";
						std::array<std::string_view, MAXIMUM_ARGUMENTS> memberNames{};
						for (uint32_t index = 0; index < block.member_types.size(); ++index) {
							memberNames[index] = compiler.get_member_name(block.self, index);
							if (memberNames[index].empty()) return "HLSL uniform member identity missing";
							for (uint32_t prior = 0; prior < index; ++prior)
								if (memberNames[index] == memberNames[prior])
									return "HLSL duplicate uniform member identity";
						}
						const auto authoredCount = std::count_if(
							program.Arguments.begin(), program.Arguments.end(), [](const auto &argument) {
								return !argument.Name.empty() && argument.Kind != ArgumentKind::Sampler2D;
							}
						);
						if (block.member_types.size() != size_t(authoredCount))
							return "HLSL uniform member count differs from authored arguments";
						const auto extent = compiler.get_declared_struct_size(block);
						if (extent > 16384) return "HLSL uniform block exceeds surface budget";
						bytes = uint32_t(extent);
						for (uint32_t index = 0; index < block.member_types.size(); ++index) {
							const auto name = memberNames[index];
							const auto found = std::find_if(
								program.Arguments.begin(),
								program.Arguments.end(),
								[&](const auto &argument) { return argument.Name == name; }
							);
							if (found == program.Arguments.end() || found->Kind == ArgumentKind::Sampler2D)
								return "HLSL uniform member is not a declared argument";
							const auto &type = compiler.get_type(block.member_types[index]);
							const uint32_t kind = uint32_t(found->Kind);
							const uint32_t width = kind < 2	   ? 1
												   : kind < 5  ? kind
												   : kind == 5 ? 3
												   : kind == 6 ? 4
															   : 4;
							const uint32_t columns = kind == 5 ? 3 : kind == 6 ? 4 : 1;
							if (!Shape(type, width, columns, kind == 1)) return "HLSL argument type mismatch";
							Member member{
								std::string(name),
								found->Kind,
								compiler.type_struct_member_offset(block, index),
								uint32_t(compiler.get_declared_struct_member_size(block, index)),
								0,
								false
							};
							if (columns > 1) {
								member.MatrixStride = compiler.type_struct_member_matrix_stride(block, index);
								member.RowMajor = compiler.has_member_decoration(
									block.self, index, spv::DecorationRowMajor
								);
							}
							members.push_back(std::move(member));
						}
					}
					if (!stage && resources.uniform_buffers.size() != 1)
						return "HLSL fixed vertex matrix buffer missing";
				}
			} catch (const spirv_cross::CompilerError &error) {
				return std::string("HLSL reflection: ") + error.what();
			}
			return {};
		}
	} // namespace
	std::optional<std::string> Assemble(
		const Definition &definition,
		std::span<const Library> libraries,
		Source &output,
		uint64_t maximumBytes
	) {
		ENGINE_PROFILE_CAT("composer HLSL assembly", core::ProfileCategory::Assets);
		if (definition.Libraries.size() > MAXIMUM_SOURCE_BYTES)
			return "HLSL library selectors exceed source budget";
		const uint64_t previousBytes = detail::SourceRetainedBytes(output);
		// Selector normalization, missing-name copies and bounded declaration scratch overlap assembly.
		const uint64_t scratch =
			MAXIMUM_ARGUMENTS * (sizeof(std::string) + 512) + 4 * (definition.Libraries.size() + 1);
		if (previousBytes > maximumBytes || scratch > maximumBytes - previousBytes ||
			sizeof(Source) + 2 * VERTEX.size() + 32 > maximumBytes - previousBytes - scratch)
			return "HLSL assembly replacement exceeds operation budget";
		if (const auto failure = ArgumentsValid(definition.Arguments)) return failure;
		if (libraries.size() > 64) return "too many HLSL libraries";
		for (size_t index = 0; index < libraries.size(); ++index) {
			if (libraries[index].Name.empty() || libraries[index].Name.size() > 1024)
				return "invalid HLSL library name";
			for (size_t prior = 0; prior < index; ++prior)
				if (libraries[index].Name == libraries[prior].Name) return "duplicate HLSL library name";
		}
		size_t total = 0;
		for (const auto text : {definition.Main, definition.Global, definition.Libraries}) {
			if (text.size() > MAXIMUM_SOURCE_BYTES - total) return "HLSL authored source exceeds budget";
			total += text.size();
		}
		for (const auto &library : libraries) {
			if (library.Source.size() > MAXIMUM_SOURCE_BYTES - total)
				return "HLSL libraries exceed source budget";
			total += library.Source.size();
		}
		Source candidate;
		candidate.Vertex = VERTEX;
		candidate.Fragment = "\n";
		const auto append = [&](std::string_view text) {
			if (text.size() > MAXIMUM_SOURCE_BYTES - candidate.Fragment.size()) return false;
			const uint64_t held = detail::SourceRetainedBytes(candidate);
			const uint64_t growth = 2 * (candidate.Fragment.size() + text.size() + 1);
			if (held > maximumBytes - previousBytes - scratch ||
				growth > maximumBytes - previousBytes - scratch - held)
				return false;
			candidate.Fragment.append(text);
			return detail::SourceRetainedBytes(candidate) <= maximumBytes - previousBytes - scratch;
		};
		std::string selectors(definition.Libraries);
		Replace(selectors, "\n");
		uint32_t selectedCount = 0;
		for (size_t first = 0; first <= selectors.size();) {
			const auto delimiter = selectors.find(';', first);
			std::string name = selectors.substr(
				first, delimiter == std::string::npos ? std::string::npos : delimiter - first
			);
			Replace(name, "using");
			Replace(name, "\"");
			Trim(name);
			if (!name.empty()) {
				if (++selectedCount > 64) return "too many selected HLSL libraries";
				const auto found = std::find_if(libraries.begin(), libraries.end(), [&](const auto &library) {
					return library.Name == name;
				});
				if (found == libraries.end())
					candidate.MissingLibraries.push_back(name);
				else {
					if (candidate.Fragment.size() >= MAXIMUM_SOURCE_BYTES ||
						found->Source.size() > MAXIMUM_SOURCE_BYTES - candidate.Fragment.size() - 1)
						return "HLSL selected libraries exceed source budget";
					if (!append(found->Source) || !append("\n"))
						return "HLSL assembly exceeds operation budget";
				}
			}
			if (delimiter == std::string::npos) break;
			first = delimiter + 1;
		}
		if (!append("\ncbuffer Data : register(b10) {\n")) return "HLSL assembled source exceeds budget";
		std::string samplers;
		uint32_t slot = 1;
		for (const auto &argument : definition.Arguments) {
			if (argument.Name.empty()) continue;
			if (argument.Kind == ArgumentKind::Sampler2D) {
				samplers += "Texture2D " + argument.Name + "Object : register(t" + std::to_string(slot) +
							");\nSamplerState " + argument.Name + " : register(s" + std::to_string(slot) +
							");\n";
				++slot;
			} else {
				const std::string declaration =
					"    " + std::string(TYPES[size_t(argument.Kind)]) + " " + argument.Name + ";\n";
				if (!append(declaration)) return "HLSL assembled source exceeds budget";
			}
		}
		for (const auto text :
			 {std::string_view("};\n"),
			  std::string_view(samplers),
			  PRE_MAIN,
			  definition.Global,
			  std::string_view("\n"),
			  POST_MAIN,
			  definition.Main,
			  std::string_view("}")})
			if (!append(text)) return "HLSL assembled source exceeds budget";
		output = std::move(candidate);
		return {};
	}
	std::optional<std::string>
	Cook(const Definition &definition, const Source &source, Program &output, uint64_t maximumBytes) {
		ENGINE_PROFILE_CAT("composer HLSL cook", core::ProfileCategory::Assets);
		const uint64_t previousBytes = detail::ProgramRetainedBytes(output);
		const uint64_t metadata =
			sizeof(Program) + MAXIMUM_ARGUMENTS * (sizeof(Argument) + sizeof(Member) + 512);
		if (previousBytes > maximumBytes || metadata > maximumBytes - previousBytes)
			return "HLSL program replacement exceeds operation budget";
		if (const auto failure = ArgumentsValid(definition.Arguments)) return failure;
		if (source.Vertex != VERTEX || source.Fragment.size() > MAXIMUM_SOURCE_BYTES)
			return "HLSL source pair is not bounded fixed-vertex source";
		shaderc::Compiler compiler;
		Program candidate;
		candidate.Arguments.reserve(definition.Arguments.size());
		candidate.Members.reserve(MAXIMUM_ARGUMENTS);
		candidate.Arguments.assign(definition.Arguments.begin(), definition.Arguments.end());
		for (uint32_t stage = 0; stage < 2; ++stage) {
			const auto kind = stage ? shaderc_glsl_fragment_shader : shaderc_glsl_vertex_shader;
			shaderc::CompileOptions options;
			options.SetSourceLanguage(shaderc_source_language_hlsl);
			options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
			options.SetHlslIoMapping(true);
			options.SetAutoSampledTextures(true);
			options.SetHlslRegisterSetAndBindingForStage(kind, stage ? "b10" : "b0", stage ? "3" : "1", "0");
			if (stage)
				for (uint32_t slot = 0; slot < MAXIMUM_SAMPLERS; ++slot) {
					const auto number = std::to_string(slot), texture = "t" + number, sampler = "s" + number;
					options.SetHlslRegisterSetAndBindingForStage(kind, texture, "2", number);
					options.SetHlslRegisterSetAndBindingForStage(kind, sampler, "2", number);
				}
			const auto &text = stage ? source.Fragment : source.Vertex;
			const auto result = compiler.CompileGlslToSpv(
				text, kind, stage ? "composer.frag.hlsl" : "composer.vert.hlsl", "main", options
			);
			if (result.GetCompilationStatus() != shaderc_compilation_status_success)
				return result.GetErrorMessage();
			auto &words = stage ? candidate.Fragment.SpirV : candidate.Vertex.SpirV;
			if (size_t(result.cend() - result.cbegin()) > 4 * 1024 * 1024)
				return "HLSL compiled payload exceeds budget";
			const uint64_t held = detail::ProgramRetainedBytes(candidate);
			const uint64_t reservedWords = uint64_t(result.cend() - result.cbegin()) + MAXIMUM_ARGUMENTS * 35;
			if (held > maximumBytes - previousBytes || metadata > maximumBytes - previousBytes - held ||
				reservedWords > (maximumBytes - previousBytes - held - metadata) / sizeof(uint32_t))
				return "HLSL compiled stage copy exceeds operation budget";
			words.reserve(size_t(reservedWords));
			if (detail::ProgramRetainedBytes(candidate) > maximumBytes - previousBytes - metadata)
				return "HLSL actual stage capacity exceeds operation budget";
			words.assign(result.cbegin(), result.cend());
			if (!NameAnonymousBuffers(words)) return "HLSL anonymous buffer metadata exceeds budget";
		}
		if (const auto failure =
				Reflect(candidate, candidate.Members, candidate.UniformBytes, candidate.SamplerCount))
			return failure;
		if (detail::ProgramRetainedBytes(candidate) > maximumBytes - previousBytes)
			return "HLSL reflected program exceeds operation budget";
		core::Metrics::Count(
			"shader.composer.cooked_spirv_bytes",
			(candidate.Vertex.SpirV.size() + candidate.Fragment.SpirV.size()) * sizeof(uint32_t)
		);
		core::Metrics::Count("shader.composer.paired_cooks", 1);
		output = std::move(candidate);
		return {};
	}
	std::optional<std::string> Admit(const Program &program) {
		if (const auto failure = ArgumentsValid(program.Arguments)) return failure;
		std::vector<Member> members;
		uint32_t bytes = 0, samplers = 0;
		if (const auto failure = Reflect(program, members, bytes, samplers)) return failure;
		if (bytes != program.UniformBytes || samplers != program.SamplerCount ||
			members.size() != program.Members.size())
			return "HLSL cooked layout metadata mismatch";
		for (size_t index = 0; index < members.size(); ++index) {
			const auto &a = members[index], &b = program.Members[index];
			if (a.Name != b.Name || a.Kind != b.Kind || a.Offset != b.Offset || a.Bytes != b.Bytes ||
				a.MatrixStride != b.MatrixStride || a.RowMajor != b.RowMajor)
				return "HLSL cooked uniform metadata mismatch";
		}
		return {};
	}
	std::optional<std::string>
	SamplerBindings(const Program &program, std::string_view backend, std::vector<SamplerBinding> &output) {
		if (backend != "spirv" && backend != "msl") return "unsupported source shader backend";
		if (const auto failure = Admit(program)) return failure;
		spirv_cross::Compiler compiler(program.Fragment.SpirV.data(), program.Fragment.SpirV.size());
		const auto resources = compiler.get_shader_resources();
		std::vector<std::string> sourceNames{"gm_BaseTexture"};
		for (const auto &argument : program.Arguments)
			if (!argument.Name.empty() && argument.Kind == ArgumentKind::Sampler2D)
				sourceNames.push_back(argument.Name);
		std::vector<SamplerBinding> candidate;
		for (const auto &resource : resources.sampled_images) {
			const auto slot = compiler.get_decoration(resource.id, spv::DecorationBinding);
			candidate.push_back({sourceNames.at(slot), slot, slot});
		}
		std::sort(candidate.begin(), candidate.end(), [](const auto &a, const auto &b) {
			return a.SourceSlot < b.SourceSlot;
		});
		if (backend == "msl")
			for (uint32_t index = 0; index < candidate.size(); ++index)
				candidate[index].BackendSlot = index;
		output = std::move(candidate);
		return {};
	}
	std::optional<std::string>
	Pack(const Program &program, std::span<const Value> values, std::vector<std::byte> &output) {
		ENGINE_PROFILE_CAT("composer HLSL argument packing", core::ProfileCategory::Assets);
		if (const auto failure = Admit(program)) return failure;
		if (values.size() > MAXIMUM_ARGUMENTS) return "too many HLSL runtime values";
		for (size_t first = 0; first < values.size(); ++first)
			for (size_t second = 0; second < first; ++second)
				if (values[first].Name == values[second].Name) return "duplicate HLSL runtime value";
		std::vector<std::byte> candidate(program.UniformBytes);
		for (const auto &member : program.Members) {
			const auto found = std::find_if(values.begin(), values.end(), [&](const auto &value) {
				return value.Name == member.Name;
			});
			const auto kind = uint32_t(member.Kind);
			const uint32_t width = kind < 2 ? 1 : kind < 5 ? kind : kind == 5 ? 3 : 4;
			const uint32_t columns = kind == 5 ? 3 : kind == 6 ? 4 : 1;
			if (found == values.end() || found->Numbers.size() != width * columns)
				return "HLSL runtime argument shape mismatch";
			for (uint32_t column = 0; column < columns; ++column)
				for (uint32_t row = 0; row < width; ++row) {
					const auto value = found->Numbers[column * width + row];
					if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
						return "HLSL runtime argument is not finite float32";
					if (kind == 1 &&
						(std::trunc(value) != value || value < std::numeric_limits<int32_t>::min() ||
						 value > std::numeric_limits<int32_t>::max()))
						return "HLSL integer argument is outside int32";
					const uint32_t offset =
						member.Offset + (columns == 1	   ? row * 4
										 : member.RowMajor ? row * member.MatrixStride + column * 4
														   : column * member.MatrixStride + row * 4);
					if (offset > candidate.size() || candidate.size() - offset < 4)
						return "HLSL runtime uniform member exceeds block";
					const uint32_t word =
						kind == 1 ? uint32_t(int32_t(value)) : std::bit_cast<uint32_t>(float(value));
					for (uint32_t byte = 0; byte < 4; ++byte)
						candidate[offset + byte] = std::byte((word >> (byte * 8)) & 255);
				}
		}
		output = std::move(candidate);
		return {};
	}
} // namespace engine::render::hlsl
