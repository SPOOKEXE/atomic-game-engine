#pragma once

#include <engine/render/ResourceImage.hpp>

namespace engine::render {
	inline std::optional<AmbientOcclusionProvenance> CapturedAmbientOcclusion(
		ResourceImageFormat format, bool builtInOcclusion, const AmbientOcclusionProvenance &builtIn
	) {
		if (format != ResourceImageFormat::R8_UNorm) return std::nullopt;
		if (builtInOcclusion) return builtIn;
		return AmbientOcclusionProvenance{
			.SourceState = AmbientOcclusionSourceState::Unavailable,
			.ProducerFrame = std::nullopt,
			.Enabled = std::nullopt,
			.SampleCount = std::nullopt,
			.RadiusWorldUnits = std::nullopt,
			.Denoiser = std::nullopt,
			.TemporalHistory = std::nullopt,
			.BackgroundValue = std::nullopt,
			.BackgroundClassification = AmbientOcclusionBackgroundClassification::Unavailable
		};
	}
}
