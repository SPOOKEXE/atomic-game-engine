#include <engine/assets/Shader.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/render/ComposerHlsl.hpp>

#include <algorithm>
#include <spirv_cross.hpp>
namespace engine::render::hlsl {
	namespace {
		std::string Type(const spirv_cross::SPIRType &type) {
			if (type.columns > 1)
				return "float" + std::to_string(type.columns) + "x" + std::to_string(type.vecsize);
			if (type.basetype == spirv_cross::SPIRType::Int)
				return type.vecsize == 1 ? "int" : "int" + std::to_string(type.vecsize);
			return type.vecsize == 1 ? "float" : "float" + std::to_string(type.vecsize);
		}
	} // namespace
	std::optional<std::string>
	BuildContainer(const Program &program, std::string_view compilerIdentity, assets::ShaderData &output) {
		ENGINE_PROFILE_CAT("composer HLSL container", core::ProfileCategory::Assets);
		if (const auto failure = Admit(program)) return failure;
		if (compilerIdentity.empty() || compilerIdentity.size() > assets::Shader::MAXIMUM_NAME)
			return "HLSL compiler identity missing";
		assets::ShaderData candidate;
		candidate.CompilerVersion = compilerIdentity;
		candidate.OptimizerVersion = "none";
		candidate.TranslatorVersion = "none";
		candidate.ShaderAbi = "atomic.composer-hlsl.v1";
		candidate.TargetEnvironment = "vulkan1.0";
		candidate.SourceLanguage = "hlsl";
		candidate.CapabilityProfile = "composer-surface";
		candidate.CookProfile = "preview";
		for (uint32_t stage = 0; stage < 2; ++stage) {
			const auto &words = stage ? program.Fragment.SpirV : program.Vertex.SpirV;
			spirv_cross::Compiler compiler(words.data(), words.size());
			const auto resources = compiler.get_shader_resources();
			assets::ShaderVariant variant;
			variant.Name = stage ? "hlsl.fragment" : "hlsl.vertex";
			variant.Stage = stage ? "fragment" : "vertex";
			for (const auto &image : resources.sampled_images) {
				assets::ShaderResource resource;
				resource.Name = image.name;
				resource.Kind = "sampled-texture";
				resource.Set = 2;
				resource.Binding = compiler.get_decoration(image.id, spv::DecorationBinding);
				resource.Access = "read";
				resource.Dimension = "2d";
				resource.SampleType = "float";
				variant.Resources.push_back(std::move(resource));
			}
			for (const auto &buffer : resources.uniform_buffers) {
				const auto &block = compiler.get_type(buffer.base_type_id);
				assets::ShaderResource resource;
				resource.Name = buffer.name;
				resource.Kind = "uniform-buffer";
				resource.Set = stage ? 3 : 1;
				resource.Binding = 0;
				resource.Access = "read";
				resource.Dimension = "none";
				resource.MinimumBytes = compiler.get_declared_struct_size(block);
				variant.Resources.push_back(resource);
				for (uint32_t index = 0; index < block.member_types.size(); ++index) {
					const auto &type = compiler.get_type(block.member_types[index]);
					assets::ShaderParameter parameter;
					parameter.Name = compiler.get_member_name(block.self, index);
					parameter.Resource = resource.Name;
					parameter.Type = Type(type);
					parameter.Offset = compiler.type_struct_member_offset(block, index);
					parameter.Bytes = uint32_t(compiler.get_declared_struct_member_size(block, index));
					parameter.Alignment = 4;
					if (!type.array.empty()) {
						parameter.ArrayCount = type.array[0];
						parameter.ArrayStride = compiler.type_struct_member_array_stride(block, index);
					}
					if (type.columns > 1) {
						parameter.MatrixStride = compiler.type_struct_member_matrix_stride(block, index);
						parameter.RowMajor =
							compiler.has_member_decoration(block.self, index, spv::DecorationRowMajor);
					}
					variant.Parameters.push_back(std::move(parameter));
				}
			}
			const auto interface = [&](const auto &values, auto &target) {
				for (const auto &value : values) {
					assets::ShaderInterfaceVariable variable;
					variable.Name = value.name;
					variable.Type = Type(compiler.get_type(value.type_id));
					variable.Location = compiler.get_decoration(value.id, spv::DecorationLocation);
					target.push_back(std::move(variable));
				}
			};
			interface(resources.stage_inputs, variant.Inputs);
			interface(resources.stage_outputs, variant.Outputs);
			const auto sorted = [](auto &values) {
				std::sort(values.begin(), values.end(), [](const auto &a, const auto &b) {
					return a.Name < b.Name;
				});
			};
			sorted(variant.Resources);
			sorted(variant.Parameters);
			sorted(variant.Inputs);
			sorted(variant.Outputs);
			assets::ShaderPayload payload;
			payload.Backend = "spirv";
			payload.EntryPoint = "main";
			payload.Target = "vulkan1.0";
			payload.Bytes.resize(words.size() * 4);
			for (size_t index = 0; index < words.size(); ++index)
				for (uint32_t byte = 0; byte < 4; ++byte)
					payload.Bytes[index * 4 + byte] = std::byte((words[index] >> (8 * byte)) & 255);
			variant.Payloads.push_back(std::move(payload));
			candidate.Variants.push_back(std::move(variant));
		}
		std::sort(candidate.Variants.begin(), candidate.Variants.end(), [](const auto &a, const auto &b) {
			return a.Name < b.Name;
		});
		if (!candidate.IsValid()) return "HLSL cooked container structural validation failed";
		core::Metrics::Count(
			"shader.composer.container_spirv_bytes",
			(program.Vertex.SpirV.size() + program.Fragment.SpirV.size()) * sizeof(uint32_t)
		);
		output = std::move(candidate);
		return {};
	}
} // namespace engine::render::hlsl
