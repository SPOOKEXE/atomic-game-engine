#pragma once

#include <SDL3/SDL_gpu.h>

#include <optional>
#include <string>

namespace engine::render {
	struct SecondSurfaceDepthRule {
		float SourceQuantum = 0;
		bool NextRepresentable = false;
		const char *SourceFormat = "";
		const char *EqualityBias = "";
	};

	inline std::optional<SecondSurfaceDepthRule> SecondSurfaceRule(SDL_GPUTextureFormat format) {
		switch (format) {
		case SDL_GPU_TEXTUREFORMAT_D16_UNORM:
			return SecondSurfaceDepthRule{1.0f / 65535.0f, false, "d16_unorm", "one_source_quantum"};
		case SDL_GPU_TEXTUREFORMAT_D24_UNORM:
		case SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT:
			return SecondSurfaceDepthRule{1.0f / 16777215.0f, false, "d24_unorm", "one_source_quantum"};
		case SDL_GPU_TEXTUREFORMAT_D32_FLOAT:
		case SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT:
			return SecondSurfaceDepthRule{0, true, "d32_float", "next_representable_float"};
		default:
			return std::nullopt;
		}
	}

	inline std::string SecondSurfaceProvenance(SDL_GPUTextureFormat format) {
		const auto rule = SecondSurfaceRule(format);
		if (!rule) return {};
		return std::string("second_surface_depth_peel/v1;source_depth=") + rule->SourceFormat +
			   ";equality_bias=" + rule->EqualityBias +
			   ";eligibility=built_in_plain_opaque_front_facing;invalid_depth_metres=0;"
			   "validity=0_or_255;identity=unavailable;amodal_ground_truth=false";
	}
}
