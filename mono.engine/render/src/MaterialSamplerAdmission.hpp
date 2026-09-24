#pragma once

#include <engine/render/ShaderCompiler.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace engine::render {
	// Authored material fragments share the first ten texture slots with the
	// renderer's material draw. Reflection must identify the same named input
	// the draw binds at each declared slot.
	inline std::optional<std::string> AdmitMaterialSamplers(const ShaderCapabilities &capabilities) {
		constexpr std::array<std::string_view, 10> names{
			"shadowMap",
			"surfaceMap",
			"colourMap",
			"beamMap",
			"normalMap",
			"roughnessMap",
			"occlusionMap",
			"emissiveMap",
			"heightMap",
			"metalnessMap"
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
			if (resource.Set != 2 || resource.Binding >= names.size())
				return location + " is outside the material sampler contract";
			if (occupied[resource.Binding]) return location + " duplicates a material sampler slot";
			if (resource.Name != names[resource.Binding])
				return location + " must be named '" + std::string(names[resource.Binding]) + "'";
			occupied[resource.Binding] = true;
		}
		return std::nullopt;
	}
}
