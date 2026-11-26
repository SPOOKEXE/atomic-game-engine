#pragma once

#include <engine/render/resources/ShaderCompiler.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace engine::render {
	// Authored material fragments share these texture slots with the
	// renderer's material draw. Reflection must identify the same named input
	// the draw binds at each declared slot.
	inline std::optional<std::string> AdmitMaterialSamplers(const ShaderCapabilities &capabilities) {
		constexpr std::array<std::string_view, 16> names{
			"shadowMap",
			"surfaceMap",
			"colourMap",
			"beamMap",
			"normalMap",
			"roughnessMap",
			"occlusionMap",
			"emissiveMap",
			"heightMap",
			"metalnessMap",
			"packedPbrMap",
			// The shader's own, under any name.
			"*",
			"*",
			// Owned by the pass: transparent layers.
			"",
			"",
			"weatherShadowImage"
		};
		std::array<bool, names.size()> occupied{};
		for (const ShaderResourceEstimate &resource : capabilities.Resources) {
			if (resource.Kind != ShaderResourceKind::SampledTexture &&
				resource.Kind != ShaderResourceKind::SeparateTexture &&
				resource.Kind != ShaderResourceKind::Sampler)
				continue;
			const std::string location = "sampler '" + resource.Name + "' at set " +
										 std::to_string(resource.Set) + " binding " +
										 std::to_string(resource.Binding);
			if (resource.Kind != ShaderResourceKind::SampledTexture)
				return location + " must be a combined sampled image";
			if (resource.Set != 2 || resource.Binding >= names.size() || names[resource.Binding].empty())
				return location + " is outside the material sampler contract";
			if (occupied[resource.Binding]) return location + " duplicates a material sampler slot";
			if (names[resource.Binding] != "*" && resource.Name != names[resource.Binding])
				return location + " must be named '" + std::string(names[resource.Binding]) + "'";
			occupied[resource.Binding] = true;
		}
		return std::nullopt;
	}
	// Which of the two authored material slots (bit 0 binding 11, bit 1 binding
	// 12) a fragment declares as a texture array.
	uint8_t MaterialArraySlots(std::span<const uint32_t> spirv);
	// Checks cooked resource types without compiling source at load time.
	std::optional<std::string> AdmitMaterialSamplers(std::span<const uint32_t> spirv);
	// Matches the fixed opaque vertex outputs and the material colour attachment.
	std::optional<std::string> AdmitCookedMaterialInterface(std::span<const uint32_t> spirv);
}
