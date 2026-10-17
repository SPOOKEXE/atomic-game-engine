#include "MaterialSamplerAdmission.hpp"

#include "RenderTypes.hpp"

#include <spirv_cross.hpp>

namespace engine::render {
	namespace {
		bool MaterialUniformLayout(
			const spirv_cross::Compiler &module, const spirv_cross::SPIRType &block, uint32_t binding
		) {
			const auto vector = [](const spirv_cross::SPIRType &field, bool integer = false) {
				return field.basetype ==
						   (integer ? spirv_cross::SPIRType::UInt : spirv_cross::SPIRType::Float) &&
					   field.width == 32 && field.vecsize == 4 && field.columns == 1;
			};
			if (binding == 0) {
				// Established Lighting prefixes remain compatible as fields are appended.
				if (block.member_types.empty() || block.member_types.size() > sizeof(LightingUniforms) / 16)
					return false;
				for (uint32_t index = 0; index < block.member_types.size(); ++index) {
					const auto &field = module.get_type(block.member_types[index]);
					if (!vector(field, index * 16 == offsetof(LightingUniforms, RenderFeatures)) ||
						!field.array.empty() || module.type_struct_member_offset(block, index) != index * 16)
						return false;
				}
				return true;
			}
			const bool lights = binding == 1;
			if (block.member_types.size() != (lights ? 4 : 5)) return false;
			const std::array<size_t, 5> offsets =
				lights
					? std::array<
						  size_t,
						  5>{offsetof(LightUniforms, Position), offsetof(LightUniforms, Colour), offsetof(LightUniforms, Direction), offsetof(LightUniforms, Count), 0}
					: std::array<size_t, 5>{
						  offsetof(BeamUniforms, Light),
						  offsetof(BeamUniforms, Back),
						  offsetof(BeamUniforms, Plane),
						  offsetof(BeamUniforms, Region),
						  offsetof(BeamUniforms, Count)
					  };
			for (uint32_t index = 0; index < block.member_types.size(); ++index) {
				const auto &field = module.get_type(block.member_types[index]);
				const bool matrix = !lights && index < 2;
				const bool array = index + 1 != block.member_types.size();
				if (module.type_struct_member_offset(block, index) != offsets[index]) return false;
				if (matrix) {
					if (field.basetype != spirv_cross::SPIRType::Float || field.width != 32 ||
						field.vecsize != 4 || field.columns != 4 ||
						module.type_struct_member_matrix_stride(block, index) != 16 ||
						module.has_member_decoration(block.self, index, spv::DecorationRowMajor))
						return false;
				} else if (!vector(field))
					return false;
				if (array) {
					if (field.array.size() != 1 || !field.array_size_literal[0] ||
						field.array[0] != (lights ? MAX_SCENE_LIGHTS : MAX_PORTAL_BEAMS) ||
						module.type_struct_member_array_stride(block, index) != (matrix ? 64 : 16))
						return false;
				} else if (!field.array.empty())
					return false;
			}
			return true;
		}
	}
	// Cooked binaries use the same admission as authoring, without compiling source at load time.
	std::optional<std::string> AdmitMaterialSamplers(std::span<const uint32_t> spirv) {
		if (spirv.size() < 5 || spirv.front() != 0x07230203u)
			return "material shader requires a SPIR-V module";
		try {
			spirv_cross::Compiler module(spirv.data(), spirv.size());
			const auto entries = module.get_entry_points_and_stages();
			if (entries.size() != 1 || entries.front().name != "main")
				return "material shader requires one main entry point";
			if (module.get_execution_model() != spv::ExecutionModelFragment)
				return "material shader requires a fragment entry point";
			if (const auto failure = AdmitMaterialSamplers(InspectShaderCapabilities(spirv))) return failure;
			const auto resources = module.get_shader_resources();
			if (!resources.storage_images.empty()) return "material draw does not bind storage images";
			if (!resources.storage_buffers.empty() || !resources.push_constant_buffers.empty())
				return "material draw does not bind storage buffers or push constants";
			if (!resources.subpass_inputs.empty()) return "material draw does not bind input attachments";
			if (!resources.atomic_counters.empty()) return "material draw does not bind atomic counters";
			if (!resources.acceleration_structures.empty())
				return "material draw does not bind acceleration structures";
			if (!resources.shader_record_buffers.empty())
				return "material draw does not bind shader-record buffers";
			if (!resources.gl_plain_uniforms.empty())
				return "material draw does not bind standalone uniforms";
			if (!resources.tensors.empty()) return "material draw does not bind tensors";
			for (const auto &resource : resources.sampled_images) {
				const auto &type = module.get_type(resource.type_id);
				const std::string location =
					"sampler '" + resource.name + "' at set " +
					std::to_string(module.get_decoration(resource.id, spv::DecorationDescriptorSet)) +
					" binding " + std::to_string(module.get_decoration(resource.id, spv::DecorationBinding));
				if (!type.array.empty()) return location + " must be a single descriptor";
				if (type.image.dim != spv::Dim2D || type.image.arrayed || type.image.ms || type.image.depth)
					return location + " must sample a non-comparison, non-multisampled 2D texture";
				const auto &sample = module.get_type(type.image.type);
				if (sample.basetype != spirv_cross::SPIRType::Float || sample.width != 32)
					return location + " must use floating-point texture samples";
			}
		} catch (const spirv_cross::CompilerError &error) {
			return "material shader reflection failed: " + std::string(error.what());
		}
		return std::nullopt;
	}

	std::optional<std::string> AdmitCookedMaterialInterface(std::span<const uint32_t> spirv) {
		if (const auto error = AdmitMaterialSamplers(spirv)) return error;
		try {
			spirv_cross::Compiler module(spirv.data(), spirv.size());
			const auto resources = module.get_shader_resources();
			constexpr std::array<uint32_t, 13> widths{3, 4, 4, 4, 2, 3, 1, 3, 4, 2, 1, 1, 1};
			for (const auto &input : resources.stage_inputs) {
				const uint32_t location = module.get_decoration(input.id, spv::DecorationLocation);
				const auto &type = module.get_type(input.type_id);
				const bool integer = location == 6 || location >= 9;
				const auto expected = integer ? spirv_cross::SPIRType::UInt : spirv_cross::SPIRType::Float;
				if (location >= widths.size() || type.basetype != expected || type.width != 32 ||
					type.vecsize != widths[location] || type.columns != 1 || !type.array.empty() ||
					module.get_decoration(input.id, spv::DecorationComponent) != 0 ||
					module.has_decoration(input.id, spv::DecorationFlat) != (location >= 6))
					return "cooked material input does not match the fixed vertex stage at location " +
						   std::to_string(location);
			}
			if (resources.stage_outputs.size() != 1) return "cooked material requires one colour output";
			const auto &output = resources.stage_outputs.front();
			const auto &type = module.get_type(output.type_id);
			if (module.get_decoration(output.id, spv::DecorationLocation) != 0 ||
				module.get_decoration(output.id, spv::DecorationComponent) != 0 ||
				module.get_decoration(output.id, spv::DecorationIndex) != 0 ||
				type.basetype != spirv_cross::SPIRType::Float || type.width != 32 || type.vecsize != 4 ||
				type.columns != 1 || !type.array.empty())
				return "cooked material colour output must be float4 at location 0";
			for (const auto &buffer : resources.uniform_buffers) {
				const auto binding = module.get_decoration(buffer.id, spv::DecorationBinding);
				if (module.get_decoration(buffer.id, spv::DecorationDescriptorSet) != 3 || binding >= 3 ||
					!module.get_type(buffer.type_id).array.empty())
					return "cooked material uniform block is outside the material binding contract";
				if (!MaterialUniformLayout(module, module.get_type(buffer.base_type_id), binding))
					return "cooked material uniform block layout does not match host slot " +
						   std::to_string(binding);
			}
		} catch (const spirv_cross::CompilerError &error) {
			return "cooked material interface reflection failed: " + std::string(error.what());
		}
		return std::nullopt;
	}

}
