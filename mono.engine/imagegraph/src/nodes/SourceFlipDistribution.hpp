#pragma once
#include "Sampler.hpp"
namespace engine::imagegraph::detail {
	// Caller admits amount * sizeof(Vector2) before allocation.
	// Explicit float32 shader-math and RGBA8 readback reference. GPU sin precision
	// and filtering require a captured device profile for target parity.
	inline bool SourceFlipDistribution(
		NodeContext &context,
		const Image &surface,
		size_t amount,
		int64_t attempts,
		double seed,
		std::vector<Vector2> &points
	) {
		if (context.Request.RequireSourceGpuRasterCoverage)
			return context.Fail(
				Status::UnsupportedExecution,
				"FLIP surface distribution CPU profile requires "
				"captured GPU shader precision and filtering",
				"spawn_surface"
			);
		if (attempts < 0 || attempts > FluidDomainLimits::MaximumIterations ||
			amount > FluidDomainLimits::MaximumParticles ||
			uint64_t(amount) * uint64_t(attempts) > FluidDomainLimits::MaximumWork)
			return context.Fail(
				Status::LimitExceeded, "FLIP surface distribution exceeds bounded sample work", "attempt"
			);
		if (!std::isfinite(seed) || !std::isfinite(float(seed)))
			return context.Fail(
				Status::InvalidValue, "FLIP surface distribution seed exceeds float32 shader range", "seed"
			);
		points.reserve(amount);
		const bool filtered = Filtered(ReadSampler(context));
		auto random = [](float x, float y, float seed) {
			const float dot = float(float(x * 12.9898f) + float(y * 78.233f));
			const float modulo = float(seed - 32.156f * std::floor(seed / 32.156f));
			const float value = float(std::sin(float(float(dot * modulo) * 12.588f)) * 43758.5453123f);
			return float(value - std::floor(value));
		};
		for (size_t index = 0; index < amount; ++index) {
			const float u = float((float(index) + .5f) / float(amount));
			float best = 0, x = 0, y = 0;
			for (int64_t attempt = 0; attempt < attempts; ++attempt) {
				const float i = float(attempt);
				const float sx = random(float(i + u), u, float(132.54664f + float(seed)));
				const float sy = random(u, float(i + u), float(78.29131f + float(seed)));
				const float weight = random(float(i + u), float(i + u), float(8.10684f + float(seed)));
				const auto color = Texture(surface, sx, sy, filtered);
				const float grey =
					float(float(float(float(color[0]) + float(color[1])) + float(color[2])) / 3.f);
				const float brightness = float(float(grey * float(color[3])) * weight);
				if (brightness > best) {
					best = brightness;
					x = sx;
					y = sy;
				}
			}
			if (best > 0)
				points.push_back({std::round(double(x) * 255) / 255, std::round(double(y) * 255) / 255});
		}
		return true;
	}
} // namespace engine::imagegraph::detail
